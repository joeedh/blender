/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace blender {
struct CurveMapping;
struct ID;
struct IDProperty;

namespace bke {

enum class OwnedCurveResult { Changed, Unchanged, Stale, Invalid, ReadOnly };

/** Opaque, immutable evaluated data, independent of the lifetime of its owner. */
class OwnedCurveSnapshot {
 private:
  CurveMapping *mapping_;
  explicit OwnedCurveSnapshot(CurveMapping *mapping);
  friend class OwnedCurveRecord;

 public:
  ~OwnedCurveSnapshot();
  OwnedCurveSnapshot(const OwnedCurveSnapshot &) = delete;
  OwnedCurveSnapshot &operator=(const OwnedCurveSnapshot &) = delete;
  /** Native RNA scalar semantics (no output clipping). Reject non-finite input/output. */
  bool evaluate(float input, float &output) const;
  /** Caller owns the deep copy and frees it with BKE_curvemapping_free. */
  CurveMapping *copy_mapping() const;
};

/**
 * Main-thread handle to an attached definition. Registered original IDs, including their
 * property trees, must only be mutated or destroyed on the main thread. Evaluated IDs are
 * unsupported. Only immutable snapshots may be used on worker threads.
 */
class OwnedCurveRecord {
 private:
  struct State;
  std::unique_ptr<State> state_;
  OwnedCurveRecord();
  friend struct OwnedCurveRegistry;
  bool attached(std::string &error);

 public:
  ~OwnedCurveRecord();
  static std::shared_ptr<OwnedCurveRecord> acquire(ID &owner,
                                                   IDProperty &definition,
                                                   std::string &error);
  /** Process-local identity, independent of wrapper addresses and declaration generations. */
  uint64_t identity() const;
  uint64_t revision() const;
  /** Attachment validity only; unsynchronized raw edits do not detach a record. */
  bool is_attached();
  /** Raw changes require explicit sync; never return a silently stale evaluator. */
  std::shared_ptr<const OwnedCurveSnapshot> snapshot(std::string &error);
  OwnedCurveResult sync(std::string &error);
  OwnedCurveResult commit(uint64_t expected_revision,
                          const CurveMapping &candidate,
                          std::string &error);
};

/** A conservative guard for a UI path before its curve definition exists.
 * Independent from definition records: collection movement expires path guards,
 * but does not detach curves already stored in that collection. Main-thread only. */
class OwnedCurvePathWatch {
 private:
  const ID *owner_;
  bool valid_ = true;
  explicit OwnedCurvePathWatch(const ID &owner) : owner_(&owner) {}
  friend struct OwnedCurveRegistry;

 public:
  static std::shared_ptr<OwnedCurvePathWatch> acquire(const ID &owner, const IDProperty &property);
  static std::shared_ptr<OwnedCurvePathWatch> acquire_owner(const ID &owner);
  bool is_valid() const;
};

/** Invalidate the container watch without traversing unchanged children or detaching records. */
void owned_curve_membership_changed(const IDProperty *property);

/** Invalidate path guards before changing group membership or relocating inline array items. */
void owned_curve_structure_changed(const IDProperty *property);

/** Lifecycle hooks. Any-thread calls are supported only for unregistered temporary data. */
void owned_curve_invalidate_property(const IDProperty *property);
void owned_curve_invalidate_tree(const IDProperty *property);
void owned_curve_invalidate_owner(const ID *owner);
void owned_curve_invalidate_all();

/** Exact data comparison; groups are unordered, arrays ordered, floats compared by bits.
 * Allocation, list links, flags and UI metadata are intentionally excluded. */
bool owned_curve_definition_equal(const IDProperty &a, const IDProperty &b);

}  // namespace bke
}  // namespace blender
