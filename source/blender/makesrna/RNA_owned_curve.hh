/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <cstdint>
#include <string>

#include "RNA_types.hh"

namespace blender {

struct CurveMapping;
struct Main;

enum class OwnedCurveRNAError { None, Invalid, Stale, ReadOnly };

/** Scoped error channel for RNA's void accessors. Nested calls never share errors. */
class OwnedCurveRNAErrorScope {
 private:
  OwnedCurveRNAErrorScope *previous_;
  static thread_local OwnedCurveRNAErrorScope *current_;

 public:
  OwnedCurveRNAError code = OwnedCurveRNAError::None;
  std::string message;
  bool require_finite_numbers = false;
  OwnedCurveRNAErrorScope();
  ~OwnedCurveRNAErrorScope();
  static void report(OwnedCurveRNAError code, const std::string &message);
  static bool finite_numbers_required();
};

bool RNA_property_is_owned_curve(const PropertyRNA *prop);
void RNA_owned_curve_declaration_removed(PropertyRNA *prop);
/** Process-local declaration lifetime identity; does not access or allocate owner data. */
bool RNA_owned_curve_declaration_key(PropertyRNA *prop, uint64_t &key);
bool RNA_owned_curve_validate(PointerRNA &ptr);
/** Validate and return immutable cache identity for a current owned mapping. */
bool RNA_owned_curve_cache_key(PointerRNA &ptr, uint64_t &identity, uint64_t &revision);
void RNA_owned_curve_inherit(const PointerRNA &parent, PointerRNA &child);
void RNA_owned_curve_pin(PointerRNA &ptr);
uint64_t RNA_owned_curve_hash(const PointerRNA &ptr);
bool RNA_owned_curve_equal(const PointerRNA &a, const PointerRNA &b);

PointerRNA RNA_owned_curve_get(PointerRNA &parent, PropertyRNA &prop);
PointerRNA RNA_owned_curve_initialize(PointerRNA &parent,
                                      const char *path,
                                      const char *preset,
                                      bContext *C);
void RNA_owned_curve_sync(PointerRNA &parent, const char *path, bContext *C);
void RNA_owned_curve_unset(PointerRNA &parent, PropertyRNA &prop, bContext *C);
void RNA_owned_curve_set(PointerRNA &ptr, PropertyRNA &prop, const double *values, int count);
void RNA_owned_curve_update(bContext *C, PointerRNA &ptr);
PointerRNA RNA_owned_curve_point_new(PointerRNA &ptr, float x, float y, bContext *C);
void RNA_owned_curve_point_remove(PointerRNA &ptr, PointerRNA &point, bContext *C);
float RNA_owned_curve_evaluate(PointerRNA &ptr, PointerRNA &curve, float x);
void RNA_owned_curve_reset_view(PointerRNA &ptr);

namespace rna {
class OwnedCurvePath;
class OwnedCurveEdit;
}  // namespace rna

/** Capture an editor target without allocating saved properties, even for missing ancestors. */
std::shared_ptr<rna::OwnedCurvePath> RNA_owned_curve_path_capture(PointerRNA &parent,
                                                                  const char *path);
/** Compare captured authority without dereferencing possibly removed owner storage. */
bool RNA_owned_curve_path_equal(const rna::OwnedCurvePath &a, const rna::OwnedCurvePath &b);
bool RNA_owned_curve_path_is_set(const rna::OwnedCurvePath &path);
ID *RNA_owned_curve_path_owner(const rna::OwnedCurvePath &path, Main &bmain);
/** Reject stale first-initialization targets. Existing definitions use edit transactions instead.
 */
bool RNA_owned_curve_path_initialize(rna::OwnedCurvePath &path, Main &bmain, bContext *C);
std::shared_ptr<rna::OwnedCurveEdit> RNA_owned_curve_edit_begin(rna::OwnedCurvePath &path);
/** Temporary edit storage; never points into saved owner data. */
CurveMapping &RNA_owned_curve_edit_mapping(rna::OwnedCurveEdit &edit);
/** One-shot commit with the begin-time revision; callbacks may delete owner/declarations. */
bool RNA_owned_curve_edit_commit(rna::OwnedCurveEdit &edit, bContext *C, bool &changed);

}  // namespace blender
