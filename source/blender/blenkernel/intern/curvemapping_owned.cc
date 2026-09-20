/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "DNA_ID.h"
#include "DNA_color_types.h"

#include "BLI_listbase.hh"
#include "BLI_threads.hh"

#include "BKE_colortools.hh"
#include "BKE_curvemapping_idprop.hh"
#include "BKE_curvemapping_owned.hh"
#include "BKE_idprop.hh"
#include "BKE_library.hh"

namespace blender::bke {

bool owned_curve_definition_equal(const IDProperty &a, const IDProperty &b)
{
  if (a.type != b.type || a.subtype != b.subtype || a.len != b.len || !STREQ(a.name, b.name)) {
    return false;
  }
  switch (a.type) {
    case IDP_GROUP:
      for (const IDProperty &child : a.data.group) {
        const IDProperty *other = IDP_GetPropertyFromGroup(&b, child.name);
        if (!other || !owned_curve_definition_equal(child, *other)) {
          return false;
        }
      }
      return true;
    case IDP_IDPARRAY:
      for (int i = 0; i < a.len; i++) {
        if (!owned_curve_definition_equal(IDP_property_array_get(&a)[i],
                                          IDP_property_array_get(&b)[i]))
        {
          return false;
        }
      }
      return true;
    case IDP_STRING:
      return a.len == 0 || memcmp(a.data.pointer, b.data.pointer, a.len) == 0;
    case IDP_ARRAY: {
      size_t size;
      switch (a.subtype) {
        case IDP_INT:
          size = sizeof(int);
          break;
        case IDP_FLOAT:
          size = sizeof(float);
          break;
        case IDP_DOUBLE:
          size = sizeof(double);
          break;
        case IDP_BOOLEAN:
          size = sizeof(int8_t);
          break;
        default:
          return false;
      }
      return a.len == 0 || memcmp(a.data.pointer, b.data.pointer, size * a.len) == 0;
    }
    case IDP_DOUBLE:
      return memcmp(&a.data.val, &b.data.val, sizeof(double)) == 0;
    case IDP_INT:
    case IDP_FLOAT:
      return memcmp(&a.data.val, &b.data.val, sizeof(int)) == 0;
    case IDP_BOOLEAN:
      return IDP_bool_get(&a) == IDP_bool_get(&b);
    default:
      return false;
  }
}

/** Visit group children with stable addresses, descending movable array items. */
static bool has_definition(const IDProperty *root, const IDProperty *definition)
{
  if (!root) {
    return false;
  }
  if (root->type == IDP_GROUP) {
    for (const IDProperty &child : root->data.group) {
      if (&child == definition || has_definition(&child, definition)) {
        return true;
      }
    }
  }
  else if (root->type == IDP_IDPARRAY) {
    for (int i = 0; i < root->len; i++) {
      if (has_definition(IDP_property_array_get(root) + i, definition)) {
        return true;
      }
    }
  }
  return false;
}

/* Records are created only on the main thread. Never reset this on load or undo. */
static uint64_t next_record_identity = 1;

struct OwnedCurveRecord::State {
  const uint64_t identity = next_record_identity++;
  ID *owner = nullptr;
  IDProperty *definition = nullptr;
  IDProperty *canonical = nullptr;
  uint64_t revision = 1;
  std::shared_ptr<const OwnedCurveSnapshot> snapshot;
  ~State()
  {
    if (canonical) {
      IDP_FreeProperty(canonical);
    }
  }
};

struct OwnedCurveRegistry;
static std::atomic<OwnedCurveRegistry *> live_registry = nullptr;

struct OwnedCurveRegistry {
  std::mutex mutex;
  std::unordered_map<const IDProperty *, std::shared_ptr<OwnedCurveRecord>> records;
  std::unordered_map<const IDProperty *, std::weak_ptr<OwnedCurvePathWatch>> watches;
  std::unordered_map<const ID *, std::weak_ptr<OwnedCurvePathWatch>> owner_watches;

  void invalidate_watch_locked(const IDProperty *property)
  {
    auto it = watches.find(property);
    if (it != watches.end()) {
      if (auto watch = it->second.lock()) {
        BLI_assert(BLI_thread_is_main());
        watch->valid_ = false;
      }
      watches.erase(it);
    }
  }

  void invalidate_path_tree_locked(const IDProperty *property)
  {
    if (!property || watches.empty()) {
      return;
    }
    invalidate_watch_locked(property);
    if (property->type == IDP_GROUP) {
      for (const IDProperty &child : property->data.group) {
        invalidate_path_tree_locked(&child);
      }
    }
    else if (property->type == IDP_IDPARRAY) {
      for (int i = 0; i < property->len; i++) {
        invalidate_path_tree_locked(IDP_property_array_get(property) + i);
      }
    }
  }

  OwnedCurveRegistry()
  {
    live_registry.store(this);
  }
  ~OwnedCurveRegistry()
  {
    /* Canonical property destruction calls hooks. Disable those before draining the map. */
    live_registry.store(nullptr);
    for (auto &item : records) {
      item.second->state_->owner = nullptr;
      item.second->state_->definition = nullptr;
    }
    records.clear();
    for (auto &item : watches) {
      if (auto watch = item.second.lock()) {
        watch->valid_ = false;
      }
    }
    for (auto &item : owner_watches) {
      if (auto watch = item.second.lock()) {
        watch->valid_ = false;
      }
    }
  }

  void invalidate_one_locked(const IDProperty *property,
                             std::vector<std::shared_ptr<OwnedCurveRecord>> &released)
  {
    invalidate_watch_locked(property);
    auto it = records.find(property);
    if (it == records.end()) {
      return;
    }
    BLI_assert(BLI_thread_is_main());
    released.push_back(it->second);
    it->second->state_->owner = nullptr;
    it->second->state_->definition = nullptr;
    records.erase(it);
  }

  void invalidate_property(const IDProperty *property, const bool subtree)
  {
    std::vector<std::shared_ptr<OwnedCurveRecord>> released;
    {
      std::lock_guard lock(mutex);
      if (records.empty() && watches.empty()) {
        return;
      }
      const auto visit = [&](const auto &self, const IDProperty *node) -> void {
        if (!node) {
          return;
        }
        invalidate_one_locked(node, released);
        if (!subtree || (records.empty() && watches.empty())) {
          return;
        }
        if (node->type == IDP_GROUP) {
          for (const IDProperty &child : node->data.group) {
            self(self, &child);
          }
        }
        else if (node->type == IDP_IDPARRAY) {
          for (int i = 0; i < node->len; i++) {
            self(self, IDP_property_array_get(node) + i);
          }
        }
      };
      visit(visit, property);
    }
  }

  template<typename Predicate> void invalidate(Predicate match)
  {
    std::vector<std::shared_ptr<OwnedCurveRecord>> released;
    {
      std::lock_guard lock(mutex);
      for (auto it = owner_watches.begin(); it != owner_watches.end();) {
        auto watch = it->second.lock();
        if (!watch || match(it->first)) {
          if (watch) {
            BLI_assert(BLI_thread_is_main());
            watch->valid_ = false;
          }
          it = owner_watches.erase(it);
        }
        else {
          ++it;
        }
      }
      for (auto it = watches.begin(); it != watches.end();) {
        auto watch = it->second.lock();
        if (!watch || match(watch->owner_)) {
          if (watch) {
            BLI_assert(BLI_thread_is_main());
            watch->valid_ = false;
          }
          it = watches.erase(it);
        }
        else {
          ++it;
        }
      }
      for (auto it = records.begin(); it != records.end();) {
        if (match(it->second->state_->owner)) {
          BLI_assert(BLI_thread_is_main());
          released.push_back(it->second);
          it->second->state_->owner = nullptr;
          it->second->state_->definition = nullptr;
          it = records.erase(it);
        }
        else {
          ++it;
        }
      }
    }
    /* Record destruction can recursively enter lifecycle hooks, outside map operations. */
  }
};

static OwnedCurveRegistry &registry()
{
  static OwnedCurveRegistry value;
  return value;
}

std::shared_ptr<OwnedCurvePathWatch> OwnedCurvePathWatch::acquire(const ID &owner,
                                                                  const IDProperty &property)
{
  BLI_assert(BLI_thread_is_main());
  OwnedCurveRegistry &value = registry();
  std::lock_guard lock(value.mutex);
  if (value.watches.size() >= 256) {
    std::erase_if(value.watches, [](const auto &entry) { return entry.second.expired(); });
  }
  auto &weak = value.watches[&property];
  auto watch = weak.lock();
  if (watch && watch->owner_ != &owner) {
    watch->valid_ = false;
    watch.reset();
  }
  if (!watch) {
    watch = std::shared_ptr<OwnedCurvePathWatch>(new OwnedCurvePathWatch(owner));
    weak = watch;
  }
  return watch;
}

std::shared_ptr<OwnedCurvePathWatch> OwnedCurvePathWatch::acquire_owner(const ID &owner)
{
  BLI_assert(BLI_thread_is_main());
  OwnedCurveRegistry &value = registry();
  std::lock_guard lock(value.mutex);
  if (value.owner_watches.size() >= 256) {
    std::erase_if(value.owner_watches, [](const auto &entry) { return entry.second.expired(); });
  }
  auto &weak = value.owner_watches[&owner];
  auto watch = weak.lock();
  if (!watch) {
    watch = std::shared_ptr<OwnedCurvePathWatch>(new OwnedCurvePathWatch(owner));
    weak = watch;
  }
  return watch;
}

bool OwnedCurvePathWatch::is_valid() const
{
  BLI_assert(BLI_thread_is_main());
  return valid_;
}

void owned_curve_membership_changed(const IDProperty *property)
{
  if (OwnedCurveRegistry *value = live_registry.load()) {
    std::lock_guard lock(value->mutex);
    value->invalidate_watch_locked(property);
  }
}

void owned_curve_structure_changed(const IDProperty *property)
{
  if (OwnedCurveRegistry *value = live_registry.load()) {
    std::lock_guard lock(value->mutex);
    if (property && property->type == IDP_IDPARRAY) {
      /* Inline descendants can move even when their contents remain identical. */
      value->invalidate_path_tree_locked(property);
    }
    else {
      value->invalidate_watch_locked(property);
    }
  }
}

void owned_curve_invalidate_property(const IDProperty *property)
{
  if (OwnedCurveRegistry *value = live_registry.load()) {
    value->invalidate_property(property, false);
  }
}

void owned_curve_invalidate_tree(const IDProperty *property)
{
  if (OwnedCurveRegistry *value = live_registry.load()) {
    value->invalidate_property(property, true);
  }
}

void owned_curve_invalidate_owner(const ID *owner)
{
  if (OwnedCurveRegistry *value = live_registry.load()) {
    value->invalidate([&](const ID *candidate) { return candidate == owner; });
  }
}

void owned_curve_invalidate_all()
{
  if (OwnedCurveRegistry *value = live_registry.load()) {
    value->invalidate([](const auto &) { return true; });
  }
}

OwnedCurveSnapshot::OwnedCurveSnapshot(CurveMapping *mapping) : mapping_(mapping) {}
OwnedCurveSnapshot::~OwnedCurveSnapshot()
{
  BKE_curvemapping_free(mapping_);
}
bool OwnedCurveSnapshot::evaluate(const float input, float &output) const
{
  if (!std::isfinite(input)) {
    return false;
  }
  const float result = BKE_curvemap_evaluateF(mapping_, &mapping_->cm[0], input);
  if (!std::isfinite(result)) {
    return false;
  }
  output = result;
  return true;
}
CurveMapping *OwnedCurveSnapshot::copy_mapping() const
{
  return BKE_curvemapping_copy(mapping_);
}

OwnedCurveRecord::OwnedCurveRecord() : state_(std::make_unique<State>()) {}
OwnedCurveRecord::~OwnedCurveRecord() = default;

bool OwnedCurveRecord::attached(std::string &error)
{
  BLI_assert(BLI_thread_is_main());
  error.clear();
  if (!state_->owner || !state_->definition) {
    error = "Owned CurveMapping handle has been invalidated";
    return false;
  }
  if (!has_definition(state_->owner->properties, state_->definition) &&
      !has_definition(state_->owner->system_properties, state_->definition))
  {
    owned_curve_invalidate_property(state_->definition);
    error = "Owned CurveMapping definition is no longer attached to its owner";
    return false;
  }
  return true;
}

std::shared_ptr<OwnedCurveRecord> OwnedCurveRecord::acquire(ID &owner,
                                                            IDProperty &definition,
                                                            std::string &error)
{
  BLI_assert(BLI_thread_is_main());
  error.clear();
  if ((owner.tag & ID_TAG_COPIED_ON_EVAL) || definition.type != IDP_GROUP ||
      (!has_definition(owner.properties, &definition) &&
       !has_definition(owner.system_properties, &definition)))
  {
    error = "Owned CurveMapping requires a stable group child on an original ID";
    return nullptr;
  }
  OwnedCurveRegistry &value = registry();
  {
    std::lock_guard lock(value.mutex);
    auto it = value.records.find(&definition);
    if (it != value.records.end()) {
      if (it->second->state_->owner != &owner) {
        error = "Owned CurveMapping owner mismatch";
        return nullptr;
      }
      return it->second;
    }
  }
  CurveMapping *mapping = BKE_curvemapping_from_idprop(definition, error);
  if (!mapping) {
    return nullptr;
  }
  std::shared_ptr<OwnedCurveRecord> record(new OwnedCurveRecord());
  record->state_->owner = &owner;
  record->state_->definition = &definition;
  record->state_->canonical = IDP_CopyProperty(&definition);
  record->state_->snapshot.reset(new OwnedCurveSnapshot(mapping));
  {
    std::lock_guard lock(value.mutex);
    value.records.emplace(&definition, record);
  }
  return record;
}

uint64_t OwnedCurveRecord::identity() const
{
  BLI_assert(BLI_thread_is_main());
  BLI_assert(state_->identity != 0);
  return state_->identity;
}

uint64_t OwnedCurveRecord::revision() const
{
  BLI_assert(BLI_thread_is_main());
  return state_->revision;
}

std::shared_ptr<const OwnedCurveSnapshot> OwnedCurveRecord::snapshot(std::string &error)
{
  if (!attached(error)) {
    return nullptr;
  }
  if (!owned_curve_definition_equal(*state_->canonical, *state_->definition)) {
    error = "Owned CurveMapping raw data changed; explicit sync is required";
    return nullptr;
  }
  return state_->snapshot;
}

bool OwnedCurveRecord::is_attached()
{
  std::string error;
  return attached(error);
}

static bool owner_editable(const ID &owner)
{
  return ID_IS_EDITABLE(&owner) && !ID_IS_OVERRIDE_LIBRARY(&owner);
}

OwnedCurveResult OwnedCurveRecord::sync(std::string &error)
{
  if (!attached(error)) {
    return OwnedCurveResult::Stale;
  }
  if (!owner_editable(*state_->owner)) {
    error = "Owned CurveMapping owner is read-only or an unsupported library override";
    return OwnedCurveResult::ReadOnly;
  }
  if (owned_curve_definition_equal(*state_->canonical, *state_->definition)) {
    return OwnedCurveResult::Unchanged;
  }
  CurveMapping *mapping = BKE_curvemapping_from_idprop(*state_->definition, error);
  if (!mapping) {
    return OwnedCurveResult::Invalid;
  }
  IDProperty *canonical = IDP_CopyProperty(state_->definition);
  IDP_FreeProperty(state_->canonical);
  state_->canonical = canonical;
  state_->snapshot.reset(new OwnedCurveSnapshot(mapping));
  state_->revision++;
  return OwnedCurveResult::Changed;
}

OwnedCurveResult OwnedCurveRecord::commit(const uint64_t expected_revision,
                                          const CurveMapping &candidate,
                                          std::string &error)
{
  if (!attached(error)) {
    return OwnedCurveResult::Stale;
  }
  if (!owner_editable(*state_->owner)) {
    error = "Owned CurveMapping owner is read-only or an unsupported library override";
    return OwnedCurveResult::ReadOnly;
  }
  if (expected_revision != state_->revision ||
      !owned_curve_definition_equal(*state_->canonical, *state_->definition))
  {
    error = "Owned CurveMapping edit conflicts with a newer revision or raw data";
    return OwnedCurveResult::Stale;
  }
  IDProperty *encoded = BKE_curvemapping_to_idprop(
      candidate, state_->definition->name, state_->definition, error);
  if (!encoded) {
    return OwnedCurveResult::Invalid;
  }
  if (owned_curve_definition_equal(*encoded, *state_->canonical)) {
    IDP_FreeProperty(encoded);
    return OwnedCurveResult::Unchanged;
  }
  CurveMapping *mapping = BKE_curvemapping_from_idprop(*encoded, error);
  if (!mapping) {
    IDP_FreeProperty(encoded);
    return OwnedCurveResult::Invalid;
  }
  encoded->flag = state_->definition->flag;
  std::shared_ptr<const OwnedCurveSnapshot> prepared(new OwnedCurveSnapshot(mapping));
  OwnedCurveRegistry &value = registry();
  std::shared_ptr<OwnedCurveRecord> retained;
  {
    std::lock_guard lock(value.mutex);
    retained = value.records.at(state_->definition);
    value.records.erase(state_->definition);
  }
  /* Only this same-root transaction suppresses root invalidation. Child hooks stay active. */
  IDP_CopyPropertyContent(state_->definition, encoded);
  IDP_FreeProperty(state_->canonical);
  state_->canonical = encoded;
  state_->snapshot = std::move(prepared);
  state_->revision++;
  {
    std::lock_guard lock(value.mutex);
    value.records.emplace(state_->definition, retained);
  }
  return OwnedCurveResult::Changed;
}

}  // namespace blender::bke
