/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <algorithm>
#include <cmath>
#include <memory>
#include <unordered_map>
#include <vector>

#include "BKE_brush.hh"
#include "BKE_colortools.hh"
#include "BKE_curvemapping_idprop.hh"
#include "BKE_curvemapping_owned.hh"
#include "BKE_idprop.hh"
#include "BKE_lib_id.hh"
#include "BKE_library.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BLI_listbase.hh"
#include "BLI_threads.hh"
#include "DEG_depsgraph.hh"
#include "DNA_brush_types.h"
#include "DNA_color_types.h"
#include "RNA_access.hh"
#include "RNA_owned_curve.hh"
#include "RNA_prototypes.hh"
#include "WM_api.hh"
#include "WM_types.hh"
#include "rna_internal.hh"
#include "rna_internal_types.hh"

namespace blender {

thread_local OwnedCurveRNAErrorScope *OwnedCurveRNAErrorScope::current_ = nullptr;
OwnedCurveRNAErrorScope::OwnedCurveRNAErrorScope() : previous_(current_)
{
  current_ = this;
}
OwnedCurveRNAErrorScope::~OwnedCurveRNAErrorScope()
{
  current_ = previous_;
}
void OwnedCurveRNAErrorScope::report(const OwnedCurveRNAError code, const std::string &message)
{
  if (current_ && current_->code == OwnedCurveRNAError::None) {
    current_->code = code;
    current_->message = message;
  }
}
bool OwnedCurveRNAErrorScope::finite_numbers_required()
{
  for (auto *scope = current_; scope; scope = scope->previous_) {
    if (scope->require_finite_numbers) {
      return true;
    }
  }
  return false;
}
static bool fail(const std::string &message,
                 const OwnedCurveRNAError code = OwnedCurveRNAError::Invalid)
{
  OwnedCurveRNAErrorScope::report(code, message);
  return false;
}
static bool main_thread()
{
  return BLI_thread_is_main() || fail("Owned CurveMapping RNA access requires the main thread");
}
bool RNA_property_is_owned_curve(const PropertyRNA *prop)
{
  return prop && prop->magic == RNA_MAGIC && (prop->flag_internal & PROP_INTERN_OWNED_CURVE);
}

namespace rna {
struct CurveDeclaration {
  PropertyRNA *property;
  uint64_t identity = 0;
  bool valid = true;
};
struct CurveView {
  std::shared_ptr<const bke::OwnedCurveSnapshot> snapshot;
  CurveMapping *mapping;
  uint64_t revision;
  ~CurveView()
  {
    BKE_curvemapping_free(mapping);
  }
};
struct CurveBinding {
  std::shared_ptr<bke::OwnedCurveRecord> record;
  ID *owner;
  IDProperty *definition;
  std::vector<std::shared_ptr<CurveDeclaration>> declarations;
  std::shared_ptr<CurveView> view;
  bool in_callback = false;
  bool pending_callback = false;
};
enum class CurveKind { Mapping, Channel, Point };
class OwnedCurveHandle {
 public:
  std::shared_ptr<CurveBinding> binding;
  std::shared_ptr<CurveView> view;
  CurveKind kind = CurveKind::Mapping;
  int point = -1;
  bool pinned = false;
};
}  // namespace rna

using namespace rna;
static std::unordered_map<PropertyRNA *, std::shared_ptr<CurveDeclaration>> declarations;
static std::unordered_map<bke::OwnedCurveRecord *, std::weak_ptr<CurveBinding>> bindings;

static std::shared_ptr<CurveDeclaration> declaration(PropertyRNA *prop)
{
  auto &token = declarations[prop];
  if (!token) {
    token = std::make_shared<CurveDeclaration>();
    token->property = prop;
  }
  return token;
}
bool RNA_owned_curve_declaration_key(PropertyRNA *prop, uint64_t &key)
{
  if (!main_thread() || !RNA_property_is_owned_curve(prop)) {
    return fail("Expected a directly declared owned CurveMapping property");
  }
  /* Never reset across unregister, load or undo; zero permanently marks exhaustion. */
  static uint64_t next_identity = 1;
  auto token = declaration(prop);
  if (token->identity == 0) {
    if (next_identity == 0) {
      return fail("Owned CurveMapping declaration identity exhausted");
    }
    token->identity = next_identity++;
  }
  key = token->identity;
  return true;
}

void RNA_owned_curve_declaration_removed(PropertyRNA *prop)
{
  auto it = declarations.find(prop);
  if (it != declarations.end()) {
    it->second->valid = false;
    it->second->property = nullptr;
    declarations.erase(it);
  }
}

/** Traverse only declared system-backed groups, without generic RNA getters or verification. */
static bool find_parent(PointerRNA parent,
                        IDProperty *group,
                        const IDProperty *definition,
                        PropertyRNA *expected,
                        PointerRNA &result,
                        std::vector<std::shared_ptr<CurveDeclaration>> &tokens,
                        const int depth = 0)
{
  if (!group || group->type != IDP_GROUP || depth > 64) {
    return false;
  }
  if (!expected && group == definition) {
    result = parent;
    return true;
  }
  for (IDProperty &child : group->data.group) {
    PropertyRNA *prop = RNA_struct_type_find_property(parent.type, child.name);
    if (!prop || !(prop->flag & PROP_IDPROPERTY)) {
      continue;
    }
    tokens.push_back(declaration(prop));
    if (&child == definition && prop == expected) {
      result = parent;
      return true;
    }
    StructRNA *type = nullptr;
    if (prop->type == PROP_POINTER && !RNA_property_is_owned_curve(prop)) {
      type = reinterpret_cast<PointerPropertyRNA *>(prop)->pointer_type;
      if (child.type == IDP_GROUP && RNA_struct_is_a(type, RNA_PropertyGroup) &&
          find_parent({parent.owner_id, type, &child},
                      &child,
                      definition,
                      expected,
                      result,
                      tokens,
                      depth + 1))
      {
        return true;
      }
    }
    else if (prop->type == PROP_COLLECTION && child.type == IDP_IDPARRAY) {
      type = reinterpret_cast<CollectionPropertyRNA *>(prop)->item_type;
      if (RNA_struct_is_a(type, RNA_PropertyGroup)) {
        for (int i = 0; i < child.len; i++) {
          IDProperty *item = IDP_property_array_get(&child) + i;
          if (find_parent({parent.owner_id, type, item},
                          item,
                          definition,
                          expected,
                          result,
                          tokens,
                          depth + 1))
          {
            return true;
          }
        }
      }
    }
    tokens.pop_back();
  }
  return false;
}

static bool binding_valid(CurveBinding &binding)
{
  if (!main_thread()) {
    return false;
  }
  for (const auto &token : binding.declarations) {
    if (!token->valid) {
      return fail("Owned CurveMapping declaration was removed", OwnedCurveRNAError::Stale);
    }
  }
  std::string error;
  auto snapshot = binding.record->snapshot(error);
  if (!snapshot) {
    return fail(error, OwnedCurveRNAError::Stale);
  }
  if (!binding.view || binding.view->snapshot != snapshot) {
    auto view = std::make_shared<CurveView>();
    view->snapshot = snapshot;
    view->mapping = snapshot->copy_mapping();
    view->revision = binding.record->revision();
    binding.view = std::move(view);
  }
  return true;
}

static void *handle_data(const OwnedCurveHandle &handle)
{
  if (handle.kind == CurveKind::Mapping) {
    return handle.view->mapping;
  }
  if (handle.kind == CurveKind::Channel) {
    return &handle.view->mapping->cm[0];
  }
  return &handle.view->mapping->cm[0].curve[handle.point];
}
static PointerRNA make_pointer(const std::shared_ptr<CurveBinding> &binding,
                               const std::shared_ptr<CurveView> &view,
                               StructRNA *type,
                               CurveKind kind,
                               const int point = -1)
{
  auto handle = std::make_shared<OwnedCurveHandle>();
  handle->binding = binding;
  handle->view = view;
  handle->kind = kind;
  handle->point = point;
  PointerRNA result{binding->owner, type, handle_data(*handle)};
  result.owned_curve = std::move(handle);
  return result;
}
bool RNA_owned_curve_validate(PointerRNA &ptr)
{
  auto handle = ptr.owned_curve;
  if (!handle) {
    return true;
  }
  if (!binding_valid(*handle->binding)) {
    return false;
  }
  if (handle->view != handle->binding->view) {
    if (handle->pinned || handle->kind == CurveKind::Point) {
      return fail("Owned curve handle belongs to an older revision", OwnedCurveRNAError::Stale);
    }
    ptr = make_pointer(handle->binding, handle->binding->view, ptr.type, handle->kind);
  }
  return true;
}
bool RNA_owned_curve_cache_key(PointerRNA &ptr, uint64_t &identity, uint64_t &revision)
{
  if (!main_thread()) {
    return false;
  }
  if (!ptr.has_type()) {
    return fail("Owned CurveMapping wrapper was removed", OwnedCurveRNAError::Stale);
  }
  if (!ptr.owned_curve || ptr.type != RNA_CurveMapping ||
      ptr.owned_curve->kind != CurveKind::Mapping)
  {
    return fail("Cache keys require an owned CurveMapping");
  }
  if (!RNA_owned_curve_validate(ptr)) {
    return false;
  }
  /* Validation may replace an unpinned handle with the current snapshot. */
  const auto &handle = *ptr.owned_curve;
  identity = handle.binding->record->identity();
  revision = handle.view->revision;
  return true;
}

void RNA_owned_curve_inherit(const PointerRNA &parent, PointerRNA &child)
{
  if (!parent.owned_curve || !child.data) {
    return;
  }
  const auto &handle = *parent.owned_curve;
  if (ELEM(child.type, RNA_CurveMap, RNA_CurveMapPoints)) {
    child = make_pointer(handle.binding, handle.view, child.type, CurveKind::Channel);
  }
  else if (child.type == RNA_CurveMapPoint) {
    const int point = int(static_cast<CurveMapPoint *>(child.data) -
                          handle.view->mapping->cm[0].curve);
    child = make_pointer(handle.binding, handle.view, child.type, CurveKind::Point, point);
  }
}
void RNA_owned_curve_pin(PointerRNA &ptr)
{
  if (ptr.owned_curve) {
    ptr.owned_curve = std::make_shared<OwnedCurveHandle>(*ptr.owned_curve);
    ptr.owned_curve->pinned = true;
  }
}
uint64_t RNA_owned_curve_hash(const PointerRNA &ptr)
{
  if (!ptr.owned_curve) {
    return uintptr_t(ptr.data);
  }
  const auto &h = *ptr.owned_curve;
  uint64_t value = uintptr_t(h.binding.get()) ^ (uint64_t(h.kind) << 3);
  if (h.kind == CurveKind::Point) {
    value ^= h.view->revision * 0x9e3779b97f4a7c15ULL ^ uint64_t(h.point + 1);
  }
  return value;
}
bool RNA_owned_curve_equal(const PointerRNA &a, const PointerRNA &b)
{
  if (!a.owned_curve || !b.owned_curve) {
    return !a.owned_curve && !b.owned_curve && a.data == b.data;
  }
  const auto &x = *a.owned_curve;
  const auto &y = *b.owned_curve;
  return x.binding == y.binding && x.kind == y.kind &&
         (x.kind != CurveKind::Point ||
          (x.view->revision == y.view->revision && x.point == y.point));
}

PointerRNA RNA_owned_curve_get(PointerRNA &parent, PropertyRNA &prop)
{
  if (!main_thread() || !parent.owner_id) {
    return {};
  }
  IDProperty *group = RNA_struct_system_idprops(&parent, false);
  IDProperty *definition = group ? IDP_GetPropertyFromGroup(group, prop.identifier.ref()) :
                                   nullptr;
  if (!definition) {
    return {};
  }
  std::string error;
  auto record = bke::OwnedCurveRecord::acquire(*parent.owner_id, *definition, error);
  if (!record) {
    fail(error);
    return {};
  }
  if (bindings.size() >= 256) {
    std::erase_if(bindings, [](const auto &entry) { return entry.second.expired(); });
  }
  auto &weak = bindings[record.get()];
  auto binding = weak.lock();
  if (binding && std::any_of(binding->declarations.begin(),
                             binding->declarations.end(),
                             [](const auto &token) { return !token->valid; }))
  {
    binding.reset();
  }
  if (!binding) {
    binding = std::make_shared<CurveBinding>();
    binding->record = record;
    binding->owner = parent.owner_id;
    binding->definition = definition;
    PointerRNA declaring_parent;
    if (!find_parent(RNA_id_pointer_create(parent.owner_id),
                     parent.owner_id->system_properties,
                     definition,
                     &prop,
                     declaring_parent,
                     binding->declarations))
    {
      fail("Owned CurveMapping must belong to a declared system property path");
      return {};
    }
    weak = binding;
  }
  if (!binding_valid(*binding)) {
    return {};
  }
  return make_pointer(binding, binding->view, RNA_CurveMapping, CurveKind::Mapping);
}

static bool editable(ID *owner)
{
  return (main_thread() && owner && ID_IS_EDITABLE(owner) && !ID_IS_OVERRIDE_LIBRARY(owner) &&
          !(owner->tag & ID_TAG_COPIED_ON_EVAL)) ||
         fail("Owned CurveMapping owner is read-only or an unsupported override",
              OwnedCurveRNAError::ReadOnly);
}
static void notify_owner(ID *owner)
{
  DEG_id_tag_update(owner, ID_RECALC_SYNC_TO_EVAL);
  if (owner->id_type() == ID_BR) {
    BKE_brush_tag_unsaved_changes(id_cast<Brush *>(owner));
    WM_main_add_notifier(NC_BRUSH | NA_EDITED, owner);
  }
  else if (owner->id_type() == ID_SCE) {
    WM_main_add_notifier(NC_SCENE | ND_TOOLSETTINGS, owner);
  }
}
struct ActiveCurveCallback {
  ID *owner;
  unsigned int owner_session;
  std::shared_ptr<CurveDeclaration> token;
  std::shared_ptr<bke::OwnedCurveRecord> record;
  bool removed;
  ActiveCurveCallback *previous;
  static thread_local ActiveCurveCallback *current;
  ActiveCurveCallback(ID &owner,
                      PropertyRNA &prop,
                      std::shared_ptr<bke::OwnedCurveRecord> record,
                      const bool removed)
      : owner(&owner),
        owner_session(owner.session_uid),
        token(declaration(&prop)),
        record(std::move(record)),
        removed(removed),
        previous(current)
  {
    current = this;
  }
  ~ActiveCurveCallback()
  {
    current = previous;
  }
};
thread_local ActiveCurveCallback *ActiveCurveCallback::current = nullptr;

static bool initialization_allowed(ID &owner, PropertyRNA &prop)
{
  auto token = declaration(&prop);
  for (auto *entry = ActiveCurveCallback::current; entry; entry = entry->previous) {
    if (entry->owner == &owner && entry->owner_session == owner.session_uid &&
        entry->token == token &&
        (entry->removed || !entry->record || !entry->record->is_attached()))
    {
      return fail(
          "Cannot reinitialize a removed curve declaration on this owner during its callback");
    }
  }
  return true;
}
static void invoke_callback(bContext *C,
                            PointerRNA &parent,
                            PropertyRNA &prop,
                            const std::shared_ptr<bke::OwnedCurveRecord> &record,
                            const bool removed = false)
{
  /* Copy the function before Python; neither parent nor property may survive the call. */
  const auto callback = reinterpret_cast<ContextPropUpdateFunc>(prop.update);
  if (C && callback) {
    for (auto *entry = ActiveCurveCallback::current; entry; entry = entry->previous) {
      if (record && entry->record == record) {
        entry->removed |= removed;
        return;
      }
    }
    ActiveCurveCallback scope(*parent.owner_id, prop, record, removed);
    callback(C, &parent, &prop);
  }
}
void RNA_owned_curve_update(bContext *C, PointerRNA &ptr)
{
  auto handle = ptr.owned_curve;
  if (!handle || !handle->binding->pending_callback) {
    return;
  }
  handle->binding->pending_callback = false;
  auto binding = handle->binding;
  if (binding->in_callback || !binding_valid(*binding)) {
    return;
  }
  PointerRNA parent;
  std::vector<std::shared_ptr<CurveDeclaration>> tokens;
  PropertyRNA *prop = binding->declarations.back()->property;
  if (!find_parent(RNA_id_pointer_create(binding->owner),
                   binding->owner->system_properties,
                   binding->definition,
                   prop,
                   parent,
                   tokens))
  {
    return;
  }
  binding->in_callback = true;
  invoke_callback(C, parent, *prop, binding->record);
  binding->in_callback = false;
}

static bool commit(PointerRNA &ptr, CurveMapping *candidate)
{
  auto handle = ptr.owned_curve;
  std::string error;
  const auto result = handle->binding->record->commit(handle->view->revision, *candidate, error);
  BKE_curvemapping_free(candidate);
  if (result == bke::OwnedCurveResult::Changed) {
    notify_owner(handle->binding->owner);
    handle->binding->pending_callback = true;
    return true;
  }
  if (result == bke::OwnedCurveResult::Unchanged) {
    return true;
  }
  return fail(error,
              result == bke::OwnedCurveResult::ReadOnly ? OwnedCurveRNAError::ReadOnly :
              result == bke::OwnedCurveResult::Stale    ? OwnedCurveRNAError::Stale :
                                                          OwnedCurveRNAError::Invalid);
}

void RNA_owned_curve_set(PointerRNA &ptr, PropertyRNA &prop, const double *values, const int count)
{
  if (!RNA_owned_curve_validate(ptr)) {
    return;
  }
  auto h = ptr.owned_curve;
  if (!editable(h->binding->owner)) {
    return;
  }
  for (int i = 0; i < count; i++) {
    if (!std::isfinite(values[i])) {
      fail("Owned CurveMapping values must be finite");
      return;
    }
  }
  const std::string name = prop.identifier.c_str();
  if (h->kind == CurveKind::Point && name == "select") {
    SET_FLAG_FROM_TEST(h->view->mapping->cm[0].curve[h->point].flag, values[0] != 0, CUMA_SELECT);
    return;
  }
  CurveMapping *copy = h->view->snapshot->copy_mapping();
  bool supported = true;
  if (h->kind == CurveKind::Point) {
    auto &point = copy->cm[0].curve[h->point];
    if (name == "location" && count == 2) {
      point.x = float(values[0]);
      point.y = float(values[1]);
      std::stable_sort(copy->cm[0].curve,
                       copy->cm[0].curve + copy->cm[0].totpoint,
                       [](const auto &a, const auto &b) { return a.x < b.x; });
    }
    else if (name == "handle_type") {
      point.flag = eCurveMapPoint_Flag(int(values[0]));
    }
    else {
      supported = false;
    }
  }
  else if (h->kind == CurveKind::Mapping) {
    if (name == "use_clip") {
      SET_FLAG_FROM_TEST(copy->flag, values[0] != 0, CUMA_DO_CLIP);
    }
    else if (name == "extend") {
      SET_FLAG_FROM_TEST(copy->flag, values[0] != 0, CUMA_EXTEND_EXTRAPOLATE);
    }
    else if (name == "tone") {
      copy->tone = eCurveMappingTone(int(values[0]));
    }
    else if (name == "clip_min_x") {
      copy->clipr.xmin = float(values[0]);
    }
    else if (name == "clip_min_y") {
      copy->clipr.ymin = float(values[0]);
    }
    else if (name == "clip_max_x") {
      copy->clipr.xmax = float(values[0]);
    }
    else if (name == "clip_max_y") {
      copy->clipr.ymax = float(values[0]);
    }
    else if (ELEM(name, "black_level", "white_level") && count == 3) {
      float *target = name == "black_level" ? copy->black : copy->white;
      for (int i = 0; i < 3; i++) {
        target[i] = float(values[i]);
      }
    }
    else {
      supported = false;
    }
  }
  else {
    supported = false;
  }
  if (!supported) {
    BKE_curvemapping_free(copy);
    fail("Unsupported owned CurveMapping field");
    return;
  }
  commit(ptr, copy);
}

/** Missing pointer ancestors are staged off-tree until the leaf is validated. */
struct CurvePath {
  PointerRNA parent;
  PropertyRNA *property = nullptr;
  IDProperty *group = nullptr;
  IDProperty *attachment = nullptr;
  IDProperty *staged = nullptr;
  ~CurvePath()
  {
    if (staged) {
      IDP_FreeProperty(staged);
    }
  }
};
static bool resolve_path(PointerRNA parent, const char *path, const bool create, CurvePath &result)
{
  int parent_depth = 0;
  if (parent.data != parent.owner_id || !RNA_struct_is_ID(parent.type)) {
    PointerRNA declared;
    std::vector<std::shared_ptr<CurveDeclaration>> tokens;
    if (!RNA_struct_is_a(parent.type, RNA_PropertyGroup) ||
        !find_parent(RNA_id_pointer_create(parent.owner_id),
                     parent.owner_id->system_properties,
                     static_cast<IDProperty *>(parent.data),
                     nullptr,
                     declared,
                     tokens) ||
        declared.type != parent.type)
    {
      return fail("Owned curves require an ID-rooted declared PropertyGroup path");
    }
    parent_depth = int(tokens.size());
  }
  IDProperty *group = RNA_struct_system_idprops(&parent, false);
  std::string remaining = path;
  for (int depth = parent_depth; depth <= 64; depth++) {
    const size_t end = remaining.find_first_of(".[");
    const std::string name = remaining.substr(0, end);
    PropertyRNA *prop = RNA_struct_type_find_property(parent.type, name.c_str());
    if (!prop || !(prop->flag & PROP_IDPROPERTY)) {
      return fail("Curve path must use declared system properties");
    }
    if (end == std::string::npos) {
      if (!RNA_property_is_owned_curve(prop)) {
        return fail("Expected a declared CurveMappingProperty");
      }
      result.parent = parent;
      result.property = prop;
      result.group = group;
      return true;
    }
    IDProperty *child = group ? IDP_GetPropertyFromGroup(group, name) : nullptr;
    StructRNA *type = nullptr;
    if (remaining[end] == '.' && prop->type == PROP_POINTER && !RNA_property_is_owned_curve(prop))
    {
      type = reinterpret_cast<PointerPropertyRNA *>(prop)->pointer_type;
      if (!RNA_struct_is_a(type, RNA_PropertyGroup)) {
        return fail("Curve path requires PropertyGroup ancestors");
      }
      if (!child) {
        if (!create) {
          return fail("Curve path parent is unset");
        }
        child = bke::idprop::create_group(name, IDP_FLAG_STATIC_TYPE).release();
        if (!result.staged) {
          result.staged = child;
          result.attachment = group;
        }
        else {
          IDP_AddToGroup(group, child);
        }
      }
      if (child->type != IDP_GROUP) {
        return fail("Malformed curve path parent");
      }
      remaining.erase(0, end + 1);
    }
    else if (remaining[end] == '[' && prop->type == PROP_COLLECTION) {
      type = reinterpret_cast<CollectionPropertyRNA *>(prop)->item_type;
      if (!RNA_struct_is_a(type, RNA_PropertyGroup) || !child || child->type != IDP_IDPARRAY) {
        return fail("Curve path requires an existing PropertyGroup collection item");
      }
      size_t cursor = end + 1;
      int64_t index = 0;
      const size_t first = cursor;
      while (cursor < remaining.size() && remaining[cursor] >= '0' && remaining[cursor] <= '9') {
        if (index > child->len) {
          return fail("Curve collection index is out of range");
        }
        index = index * 10 + (remaining[cursor++] - '0');
      }
      if (cursor == first || cursor + 2 >= remaining.size() || remaining[cursor] != ']' ||
          remaining[cursor + 1] != '.' || index >= child->len)
      {
        return fail("Curve path requires an existing numeric collection index");
      }
      child = IDP_property_array_get(child) + index;
      if (child->type != IDP_GROUP) {
        return fail("Malformed curve collection item");
      }
      remaining.erase(0, cursor + 2);
    }
    else {
      return fail("Unsupported owned curve property path");
    }
    parent = {parent.owner_id, type, child};
    group = child;
  }
  return fail("Curve path exceeds the supported depth");
}

PointerRNA RNA_owned_curve_initialize(PointerRNA &parent,
                                      const char *path,
                                      const char *preset,
                                      bContext *C)
{
  if (!editable(parent.owner_id)) {
    return {};
  }
  CurvePath resolved;
  if (!resolve_path(parent, path, true, resolved)) {
    return {};
  }
  PropertyRNA *prop = resolved.property;
  if (!STREQ(preset, "LINEAR")) {
    fail("Owned CurveMapping initialization currently accepts LINEAR");
    return {};
  }
  IDProperty *group = resolved.group;
  if (group && IDP_GetPropertyFromGroup(group, prop->identifier.ref())) {
    return RNA_owned_curve_get(resolved.parent, *prop);
  }
  if (!initialization_allowed(*parent.owner_id, *prop)) {
    return {};
  }
  CurveMapping *mapping = BKE_curvemapping_add(1, 0, 0, 1, 1);
  std::string error;
  IDProperty *definition = BKE_curvemapping_to_idprop(
      *mapping, prop->identifier.ref(), nullptr, error);
  BKE_curvemapping_free(mapping);
  if (!definition) {
    fail(error);
    return {};
  }
  definition->flag |= IDP_FLAG_STATIC_TYPE;
  if (!group) {
    group = RNA_struct_system_idprops(&parent, true);
  }
  if (!group) {
    IDP_FreeProperty(definition);
    fail("CurveMappingProperty requires an ID or PropertyGroup owner");
    return {};
  }
  IDP_AddToGroup(group, definition);
  if (resolved.staged) {
    IDProperty *attachment = resolved.attachment;
    if (!attachment) {
      attachment = RNA_struct_system_idprops(&parent, true);
    }
    if (!attachment) {
      fail("Curve path has no system storage root");
      return {};
    }
    IDP_AddToGroup(attachment, resolved.staged);
    resolved.staged = nullptr;
  }
  PointerRNA result = RNA_owned_curve_get(resolved.parent, *prop);
  notify_owner(parent.owner_id);
  if (result.owned_curve) {
    result.owned_curve->binding->pending_callback = true;
    RNA_owned_curve_update(C, result);
  }
  return result;
}
void RNA_owned_curve_sync(PointerRNA &parent, const char *path, bContext *C)
{
  if (!editable(parent.owner_id)) {
    return;
  }
  CurvePath resolved;
  if (!resolve_path(parent, path, false, resolved)) {
    return;
  }
  PropertyRNA *prop = resolved.property;
  IDProperty *group = resolved.group;
  IDProperty *definition = group ? IDP_GetPropertyFromGroup(group, prop->identifier.ref()) :
                                   nullptr;
  if (!definition) {
    fail("Owned CurveMapping is unset");
    return;
  }
  std::string error;
  auto record = bke::OwnedCurveRecord::acquire(*parent.owner_id, *definition, error);
  if (!record) {
    fail(error);
    return;
  }
  auto result = record->sync(error);
  if (result == bke::OwnedCurveResult::Unchanged) {
    return;
  }
  if (!ELEM(result, bke::OwnedCurveResult::Changed, bke::OwnedCurveResult::Unchanged)) {
    fail(error);
    return;
  }
  PointerRNA ptr = RNA_owned_curve_get(resolved.parent, *prop);
  if (ptr.owned_curve) {
    notify_owner(parent.owner_id);
    ptr.owned_curve->binding->pending_callback = true;
    RNA_owned_curve_update(C, ptr);
  }
}
void RNA_owned_curve_unset(PointerRNA &parent, PropertyRNA &prop, bContext *C)
{
  if (!editable(parent.owner_id)) {
    return;
  }
  IDProperty *group = RNA_struct_system_idprops(&parent, false);
  IDProperty *definition = group ? IDP_GetPropertyFromGroup(group, prop.identifier.ref()) :
                                   nullptr;
  if (!definition) {
    return;
  }
  std::string error;
  auto record = bke::OwnedCurveRecord::acquire(*parent.owner_id, *definition, error);
  /* Explicit unset may discard even an unsupported payload, never an ordinary read. */
  IDP_FreeFromGroup(group, definition);
  notify_owner(parent.owner_id);
  invoke_callback(C, parent, prop, record, true);
}
PointerRNA RNA_owned_curve_point_new(PointerRNA &ptr, float x, float y, bContext *C)
{
  if (!RNA_owned_curve_validate(ptr)) {
    return {};
  }
  auto h = ptr.owned_curve;
  if (!std::isfinite(x) || !std::isfinite(y) || h->view->mapping->cm[0].totpoint >= 32767) {
    fail("Invalid owned curve point or point limit exceeded");
    return {};
  }
  CurveMapping *copy = h->view->snapshot->copy_mapping();
  CurveMapPoint *point = BKE_curvemap_insert(&copy->cm[0], x, y);
  const int index = int(point - copy->cm[0].curve);
  if (!commit(ptr, copy) || !binding_valid(*h->binding)) {
    return {};
  }
  PointerRNA result = make_pointer(
      h->binding, h->binding->view, RNA_CurveMapPoint, CurveKind::Point, index);
  RNA_owned_curve_update(C, ptr);
  return result;
}
void RNA_owned_curve_point_remove(PointerRNA &ptr, PointerRNA &point, bContext *C)
{
  if (!RNA_owned_curve_validate(ptr) || !RNA_owned_curve_validate(point)) {
    return;
  }
  auto h = ptr.owned_curve;
  auto p = point.owned_curve;
  if (!p || h->binding->record != p->binding->record || p->kind != CurveKind::Point ||
      h->view->mapping->cm[0].totpoint <= 2)
  {
    fail("Point is not in this owned curve, or only two points remain");
    return;
  }
  CurveMapping *copy = h->view->snapshot->copy_mapping();
  BKE_curvemap_remove_point(&copy->cm[0], &copy->cm[0].curve[p->point]);
  if (commit(ptr, copy)) {
    RNA_owned_curve_update(C, ptr);
  }
}
float RNA_owned_curve_evaluate(PointerRNA &ptr, PointerRNA &curve, const float x)
{
  if (!RNA_owned_curve_validate(ptr) || !RNA_owned_curve_validate(curve)) {
    return 0;
  }
  if (!curve.owned_curve || ptr.owned_curve->binding->record != curve.owned_curve->binding->record)
  {
    fail("CurveMapping does not own CurveMap");
    return 0;
  }
  float result = 0;
  if (!ptr.owned_curve->view->snapshot->evaluate(x, result)) {
    fail("Owned curve evaluation requires finite input and output");
  }
  return result;
}
void RNA_owned_curve_reset_view(PointerRNA &ptr)
{
  if (RNA_owned_curve_validate(ptr)) {
    BKE_curvemapping_reset_view(ptr.owned_curve->view->mapping);
  }
}

namespace rna {
class OwnedCurvePath {
 public:
  ID *owner = nullptr;
  unsigned int owner_session = 0;
  IDProperty *root = nullptr;
  std::string path;
  std::vector<std::shared_ptr<CurveDeclaration>> declarations;
  std::vector<std::shared_ptr<bke::OwnedCurvePathWatch>> watches;
  PointerRNA existing;
};
class OwnedCurveEdit {
 public:
  PointerRNA target;
  CurveMapping *working = nullptr;
  bool finished = false;
  ~OwnedCurveEdit()
  {
    if (working) {
      BKE_curvemapping_free(working);
    }
  }
};
}  // namespace rna

/** Find a current numeric path without obtaining or creating any pointer properties. */
static bool find_group_path(StructRNA *type,
                            IDProperty *group,
                            const PointerRNA &target,
                            std::string &path,
                            const int depth = 0)
{
  if (!group || group->type != IDP_GROUP || depth > 64) {
    return false;
  }
  if (group == target.data && type == target.type) {
    return true;
  }
  for (IDProperty &child : group->data.group) {
    PropertyRNA *prop = RNA_struct_type_find_property(type, child.name);
    if (!prop || !(prop->flag & PROP_IDPROPERTY)) {
      continue;
    }
    const std::string prefix = path + child.name;
    if (prop->type == PROP_POINTER && !RNA_property_is_owned_curve(prop)) {
      StructRNA *child_type = reinterpret_cast<PointerPropertyRNA *>(prop)->pointer_type;
      std::string candidate = prefix + ".";
      if (RNA_struct_is_a(child_type, RNA_PropertyGroup) &&
          find_group_path(child_type, &child, target, candidate, depth + 1))
      {
        path = std::move(candidate);
        return true;
      }
    }
    else if (prop->type == PROP_COLLECTION && child.type == IDP_IDPARRAY) {
      StructRNA *child_type = reinterpret_cast<CollectionPropertyRNA *>(prop)->item_type;
      if (!RNA_struct_is_a(child_type, RNA_PropertyGroup)) {
        continue;
      }
      for (int i = 0; i < child.len; i++) {
        std::string candidate = prefix + "[" + std::to_string(i) + "].";
        if (find_group_path(
                child_type, IDP_property_array_get(&child) + i, target, candidate, depth + 1))
        {
          path = std::move(candidate);
          return true;
        }
      }
    }
  }
  return false;
}

std::shared_ptr<rna::OwnedCurvePath> RNA_owned_curve_path_capture(PointerRNA &parent,
                                                                  const char *path)
{
  if (!main_thread() || !parent.owner_id ||
      ((parent.owner_id->tag & ID_TAG_COPIED_ON_EVAL) ||
       ((parent.owner_id->tag & ID_TAG_NO_MAIN) &&
        !(parent.owner_id->flag & ID_FLAG_EMBEDDED_DATA))))
  {
    fail("Owned curve editor requires an original ID");
    return nullptr;
  }
  auto result = std::make_shared<rna::OwnedCurvePath>();
  result->owner = parent.owner_id;
  result->owner_session = parent.owner_id->session_uid;
  result->root = parent.owner_id->system_properties;
  result->watches.push_back(bke::OwnedCurvePathWatch::acquire_owner(*result->owner));
  PointerRNA root = RNA_id_pointer_create(parent.owner_id);
  if (parent.data != parent.owner_id || !RNA_struct_is_ID(parent.type)) {
    if (!RNA_struct_is_a(parent.type, RNA_PropertyGroup) ||
        !find_group_path(root.type, result->root, parent, result->path))
    {
      fail("Owned curve editor requires a declared system property path");
      return nullptr;
    }
  }
  result->path += path;
  std::string remaining = result->path;
  StructRNA *type = root.type;
  IDProperty *group = result->root;
  for (int depth = 0; depth <= 64; depth++) {
    if (group) {
      if (group->type != IDP_GROUP) {
        fail("Malformed owned curve path group");
        return nullptr;
      }
      result->watches.push_back(bke::OwnedCurvePathWatch::acquire(*result->owner, *group));
    }
    const size_t end = remaining.find_first_of(".[");
    const std::string name = remaining.substr(0, end);
    PropertyRNA *prop = RNA_struct_type_find_property(type, name.c_str());
    if (!prop || !(prop->flag & PROP_IDPROPERTY)) {
      fail("Owned curve editor path must use declared system properties");
      return nullptr;
    }
    result->declarations.push_back(declaration(prop));
    IDProperty *child = group ? IDP_GetPropertyFromGroup(group, name) : nullptr;
    if (end == std::string::npos) {
      if (!RNA_property_is_owned_curve(prop)) {
        fail("Expected a CurveMappingProperty");
        return nullptr;
      }
      if (child) {
        PointerRNA declaring = depth == 0 ? root : PointerRNA{root.owner_id, type, group};
        result->existing = RNA_owned_curve_get(declaring, *prop);
        if (!result->existing.owned_curve) {
          return nullptr;
        }
      }
      return result;
    }
    if (remaining[end] == '.' && prop->type == PROP_POINTER && !RNA_property_is_owned_curve(prop))
    {
      type = reinterpret_cast<PointerPropertyRNA *>(prop)->pointer_type;
      if (!RNA_struct_is_a(type, RNA_PropertyGroup) || (child && child->type != IDP_GROUP)) {
        fail("Owned curve editor path requires PropertyGroup ancestors");
        return nullptr;
      }
      remaining.erase(0, end + 1);
      group = child;
    }
    else if (remaining[end] == '[' && prop->type == PROP_COLLECTION) {
      type = reinterpret_cast<CollectionPropertyRNA *>(prop)->item_type;
      if (!RNA_struct_is_a(type, RNA_PropertyGroup) || !child || child->type != IDP_IDPARRAY) {
        fail("Owned curve editor requires an existing collection item");
        return nullptr;
      }
      result->watches.push_back(bke::OwnedCurvePathWatch::acquire(*result->owner, *child));
      size_t cursor = end + 1;
      const size_t first = cursor;
      int64_t index = 0;
      while (cursor < remaining.size() && remaining[cursor] >= '0' && remaining[cursor] <= '9') {
        if (index > child->len) {
          fail("Owned curve editor collection index out of range");
          return nullptr;
        }
        index = index * 10 + (remaining[cursor++] - '0');
      }
      if (cursor == first || cursor + 2 >= remaining.size() || remaining[cursor] != ']' ||
          remaining[cursor + 1] != '.' || index >= child->len)
      {
        fail("Owned curve editor requires an existing numeric collection index");
        return nullptr;
      }
      group = IDP_property_array_get(child) + index;
      remaining.erase(0, cursor + 2);
    }
    else {
      fail("Unsupported owned curve editor path");
      return nullptr;
    }
  }
  fail("Owned curve editor path exceeds the supported depth");
  return nullptr;
}

bool RNA_owned_curve_path_equal(const rna::OwnedCurvePath &a, const rna::OwnedCurvePath &b)
{
  const auto declarations_valid = [](const rna::OwnedCurvePath &path) {
    return std::all_of(path.declarations.begin(), path.declarations.end(), [](const auto &token) {
      return token->valid;
    });
  };
  if (!declarations_valid(a) || !declarations_valid(b)) {
    return false;
  }
  if (a.existing.owned_curve || b.existing.owned_curve) {
    return a.existing.owned_curve && b.existing.owned_curve &&
           RNA_owned_curve_equal(a.existing, b.existing);
  }
  return a.owner == b.owner && a.owner_session == b.owner_session && a.root == b.root &&
         a.path == b.path && a.declarations == b.declarations && a.watches == b.watches &&
         std::all_of(a.watches.begin(), a.watches.end(), [](const auto &watch) {
           return watch->is_valid();
         });
}

ID *RNA_owned_curve_path_owner(const rna::OwnedCurvePath &path, Main &bmain)
{
  if (!BLI_thread_is_main() ||
      std::any_of(path.watches.begin(),
                  path.watches.end(),
                  [](const auto &watch) { return !watch->is_valid(); }) ||
      !BKE_id_is_in_main(&bmain, path.owner) || path.owner->session_uid != path.owner_session)
  {
    fail("Owned curve target is stale", OwnedCurveRNAError::Stale);
    return nullptr;
  }
  return path.owner;
}

bool RNA_owned_curve_path_is_set(const rna::OwnedCurvePath &path)
{
  return bool(path.existing.owned_curve);
}

bool RNA_owned_curve_path_initialize(rna::OwnedCurvePath &path, Main & /*bmain*/, bContext *C)
{
  if (!main_thread() || path.existing.owned_curve) {
    return fail("Owned curve initialization requires an unset captured path");
  }
  /* The owner watch also covers embedded IDs, which are absent from Main's listbases. */
  if (std::any_of(path.declarations.begin(),
                  path.declarations.end(),
                  [](const auto &token) { return !token->valid; }) ||
      std::any_of(path.watches.begin(),
                  path.watches.end(),
                  [](const auto &watch) { return !watch->is_valid(); }))
  {
    return fail("Owned curve path changed after drawing; redraw before initializing",
                OwnedCurveRNAError::Stale);
  }
  ID *owner = path.owner;
  if (owner->session_uid != path.owner_session || owner->system_properties != path.root) {
    return fail("Owned curve owner storage changed after drawing", OwnedCurveRNAError::Stale);
  }
  PointerRNA root = RNA_id_pointer_create(owner);
  /* No owner, property or path-tree access after the user callback. */
  return bool(RNA_owned_curve_initialize(root, path.path.c_str(), "LINEAR", C).owned_curve);
}

std::shared_ptr<rna::OwnedCurveEdit> RNA_owned_curve_edit_begin(rna::OwnedCurvePath &path)
{
  if (!path.existing.owned_curve || !RNA_owned_curve_validate(path.existing) ||
      !editable(path.existing.owner_id))
  {
    return nullptr;
  }
  auto result = std::make_shared<rna::OwnedCurveEdit>();
  result->target = path.existing;
  RNA_owned_curve_pin(result->target);
  result->working = path.existing.owned_curve->view->snapshot->copy_mapping();
  result->working->cur = 0;
  BKE_curvemapping_reset_view(result->working);
  return result;
}

CurveMapping &RNA_owned_curve_edit_mapping(rna::OwnedCurveEdit &edit)
{
  return *edit.working;
}

bool RNA_owned_curve_edit_commit(rna::OwnedCurveEdit &edit, bContext *C, bool &changed)
{
  changed = false;
  if (edit.finished) {
    return fail("Owned curve edit transaction has already finished");
  }
  edit.finished = true;
  if (!RNA_owned_curve_validate(edit.target)) {
    return false;
  }
  auto binding = edit.target.owned_curve->binding;
  std::string error;
  const auto result = binding->record->commit(
      edit.target.owned_curve->view->revision, *edit.working, error);
  if (result == bke::OwnedCurveResult::Unchanged) {
    return true;
  }
  if (result != bke::OwnedCurveResult::Changed) {
    return fail(error,
                result == bke::OwnedCurveResult::ReadOnly ? OwnedCurveRNAError::ReadOnly :
                result == bke::OwnedCurveResult::Stale    ? OwnedCurveRNAError::Stale :
                                                            OwnedCurveRNAError::Invalid);
  }
  changed = true;
  notify_owner(binding->owner);
  binding->pending_callback = true;
  RNA_owned_curve_update(C, edit.target);
  return true;
}

}  // namespace blender
