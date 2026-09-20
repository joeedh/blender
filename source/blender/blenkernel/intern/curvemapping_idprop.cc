/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <cmath>
#include <cstring>
#include <limits>

#include "MEM_guardedalloc.h"

#include "DNA_color_types.h"

#include "BLI_listbase.hh"
#include "BLI_utildefines.hh"

#include "BKE_colortools.hh"
#include "BKE_curvemapping_idprop.hh"
#include "BKE_idprop.hh"

namespace blender {

static const IDProperty *field(const IDProperty &group, const char *name, const int type)
{
  return group.type == IDP_GROUP ? IDP_GetPropertyTypeFromGroup(&group, name, type) : nullptr;
}

static bool string_is(const IDProperty *prop, const char *value)
{
  return prop && prop->type == IDP_STRING && prop->subtype == IDP_STRING_SUB_UTF8 &&
         prop->len == int(strlen(value) + 1) && STREQ(IDP_string_get(prop), value);
}

static bool definition_has_only_owned_data(const IDProperty &prop, const int depth = 0)
{
  if (depth > 64) {
    return false;
  }
  if (prop.type == IDP_GROUP) {
    for (const IDProperty &child : prop.data.group) {
      if (!definition_has_only_owned_data(child, depth + 1)) {
        return false;
      }
    }
    return true;
  }
  if (prop.type == IDP_IDPARRAY) {
    const IDProperty *items = IDP_property_array_get(&prop);
    for (int i = 0; i < prop.len; i++) {
      if (!definition_has_only_owned_data(items[i], depth + 1)) {
        return false;
      }
    }
    return true;
  }
  if (prop.type == IDP_ARRAY) {
    return ELEM(prop.subtype, IDP_INT, IDP_FLOAT, IDP_DOUBLE, IDP_BOOLEAN);
  }
  return ELEM(prop.type, IDP_INT, IDP_FLOAT, IDP_DOUBLE, IDP_BOOLEAN, IDP_STRING);
}

static bool handle_read(const IDProperty *prop, eCurveMapPoint_Flag &handle)
{
  if (string_is(prop, "AUTO")) {
    handle = {};
    return true;
  }
  if (string_is(prop, "AUTO_CLAMPED")) {
    handle = CUMA_HANDLE_AUTO_ANIM;
    return true;
  }
  if (string_is(prop, "VECTOR")) {
    handle = CUMA_HANDLE_VECTOR;
    return true;
  }
  return false;
}

static const char *handle_name(const eCurveMapPoint_Flag flag)
{
  if (flag & CUMA_HANDLE_VECTOR) {
    return "VECTOR";
  }
  return flag & CUMA_HANDLE_AUTO_ANIM ? "AUTO_CLAMPED" : "AUTO";
}

static bool coordinate_read(const IDProperty &point, const char *name, float &value)
{
  const IDProperty *prop = field(point, name, IDP_DOUBLE);
  if (!prop) {
    return false;
  }
  const double number = IDP_double_get(prop);
  if (!std::isfinite(number) || std::abs(number) > std::numeric_limits<float>::max()) {
    return false;
  }
  value = float(number);
  return double(value) == number;
}

static bool definition_validate_structure(const IDProperty &definition, std::string &error)
{
  error.clear();
  if (!definition_has_only_owned_data(definition)) {
    error =
        "Owned CurveMapping definitions cannot contain ID references or nesting beyond 64 levels";
    return false;
  }
  const IDProperty *version = field(definition, "_version", IDP_INT);
  if (!string_is(field(definition, "_type", IDP_STRING), "blender.owned_curve_mapping") ||
      !version || IDP_int_get(version) != 1)
  {
    error = "Unsupported owned CurveMapping definition type or version";
    return false;
  }
  const IDProperty *clip = field(definition, "clip", IDP_ARRAY);
  const IDProperty *use_clip = field(definition, "use_clip", IDP_INT);
  const IDProperty *extend = field(definition, "extend", IDP_STRING);
  eCurveMapPoint_Flag default_handle;
  if (!clip || clip->subtype != IDP_DOUBLE || clip->len != 4 || !use_clip ||
      !ELEM(IDP_int_get(use_clip), 0, 1) ||
      !(string_is(extend, "HORIZONTAL") || string_is(extend, "EXTRAPOLATED")) ||
      !handle_read(field(definition, "default_handle", IDP_STRING), default_handle))
  {
    error = "Invalid owned CurveMapping settings";
    return false;
  }
  const double *rect = IDP_array_double_get(clip);
  for (int i = 0; i < 4; i++) {
    if (!std::isfinite(rect[i]) || rect[i] < -100.0 || rect[i] > 100.0 ||
        double(float(rect[i])) != rect[i])
    {
      error = "CurveMapping clipping coordinates must be finite native floats in [-100, 100]";
      return false;
    }
  }
  if (rect[0] >= rect[2] || rect[1] >= rect[3]) {
    error = "CurveMapping clipping bounds must be increasing";
    return false;
  }
  const IDProperty *points = field(definition, "points", IDP_IDPARRAY);
  if (!points || points->len < 2 || points->len > std::numeric_limits<short>::max()) {
    error = "CurveMapping requires between 2 and 32767 points";
    return false;
  }
  const IDProperty *items = IDP_property_array_get(points);
  float previous_x = -std::numeric_limits<float>::max();
  for (int i = 0; i < points->len; i++) {
    float x, y;
    eCurveMapPoint_Flag handle;
    if (items[i].type != IDP_GROUP || items[i].len != 3 || !coordinate_read(items[i], "x", x) ||
        !coordinate_read(items[i], "y", y) || x < previous_x ||
        !handle_read(field(items[i], "handle", IDP_STRING), handle))
    {
      error = "CurveMapping points require ordered finite native coordinates and valid handles";
      return false;
    }
    previous_x = x;
  }
  return true;
}

CurveMapping *BKE_curvemapping_from_idprop(const IDProperty &definition, std::string &error)
{
  if (!definition_validate_structure(definition, error)) {
    return nullptr;
  }
  const double *rect = IDP_array_double_get(field(definition, "clip", IDP_ARRAY));
  CurveMapping *mapping = BKE_curvemapping_add(1, rect[0], rect[1], rect[2], rect[3]);
  mapping->flag = {};
  if (IDP_int_get(field(definition, "use_clip", IDP_INT))) {
    mapping->flag |= CUMA_DO_CLIP;
  }
  if (string_is(field(definition, "extend", IDP_STRING), "EXTRAPOLATED")) {
    mapping->flag |= CUMA_EXTEND_EXTRAPOLATE;
  }
  CurveMap &curve = mapping->cm[0];
  handle_read(field(definition, "default_handle", IDP_STRING), curve.default_handle_type);
  const IDProperty *points = field(definition, "points", IDP_IDPARRAY);
  const IDProperty *items = IDP_property_array_get(points);
  MEM_delete(curve.curve);
  curve.totpoint = short(points->len);
  curve.curve = MEM_new_array<CurveMapPoint>(points->len, __func__);
  for (int i = 0; i < points->len; i++) {
    coordinate_read(items[i], "x", curve.curve[i].x);
    coordinate_read(items[i], "y", curve.curve[i].y);
    handle_read(field(items[i], "handle", IDP_STRING), curve.curve[i].flag);
  }
  BKE_curvemapping_init(mapping);
  bool finite = std::isfinite(curve.range) && curve.range > 0.0f;
  for (int i = 0; i < 2; i++) {
    finite &= std::isfinite(curve.ext_in[i]) && std::isfinite(curve.ext_out[i]);
  }
  for (int i = 0; i <= CM_TABLE; i++) {
    finite &= std::isfinite(curve.table[i].x) && std::isfinite(curve.table[i].y);
  }
  if (!finite) {
    error = "CurveMapping definition produces non-finite evaluation data";
    BKE_curvemapping_free(mapping);
    return nullptr;
  }
  return mapping;
}

bool BKE_curvemapping_idprop_validate(const IDProperty &definition, std::string &error)
{
  CurveMapping *mapping = BKE_curvemapping_from_idprop(definition, error);
  if (!mapping) {
    return false;
  }
  BKE_curvemapping_free(mapping);
  return true;
}

IDProperty *BKE_curvemapping_to_idprop(const CurveMapping &mapping,
                                       const StringRef name,
                                       const IDProperty *previous,
                                       std::string &error)
{
  error.clear();
  if (previous && !BKE_curvemapping_idprop_validate(*previous, error)) {
    return nullptr;
  }
  const CurveMap &curve = mapping.cm[0];
  if (!curve.curve || curve.totpoint < 2 || mapping.cm[1].totpoint || mapping.cm[2].totpoint ||
      mapping.cm[3].totpoint || (mapping.flag & CUMA_USE_WRAPPING) ||
      mapping.tone != CURVE_TONE_STANDARD)
  {
    error = "Owned CurveMapping storage supports one non-wrapping scalar channel";
    return nullptr;
  }
  for (int i = 0; i < 3; i++) {
    if (mapping.black[i] != 0.0f || mapping.white[i] != 1.0f) {
      error = "Owned scalar CurveMapping cannot encode RGB level transforms";
      return nullptr;
    }
  }
  IDProperty *result = bke::idprop::create_group(name).release();
  if (previous) {
    IDP_MergeGroup(result, previous, true);
  }
  IDP_ReplaceInGroup(result, IDP_NewString("blender.owned_curve_mapping", "_type"));
  IDP_ReplaceInGroup(result, bke::idprop::create("_version", 1).release());
  const double clip[] = {
      mapping.clipr.xmin, mapping.clipr.ymin, mapping.clipr.xmax, mapping.clipr.ymax};
  IDP_ReplaceInGroup(result, bke::idprop::create("clip", Span<double>(clip, 4)).release());
  IDP_ReplaceInGroup(
      result, bke::idprop::create("use_clip", int(bool(mapping.flag & CUMA_DO_CLIP))).release());
  IDP_ReplaceInGroup(
      result,
      IDP_NewString(mapping.flag & CUMA_EXTEND_EXTRAPOLATE ? "EXTRAPOLATED" : "HORIZONTAL",
                    "extend"));
  IDP_ReplaceInGroup(result,
                     IDP_NewString(handle_name(curve.default_handle_type), "default_handle"));
  IDProperty *points = IDP_NewIDPArray("points");
  IDP_ResizeIDPArray(points, curve.totpoint);
  for (int i = 0; i < curve.totpoint; i++) {
    IDProperty *point = bke::idprop::create_group("").release();
    IDP_AddToGroup(point, bke::idprop::create("x", double(curve.curve[i].x)).release());
    IDP_AddToGroup(point, bke::idprop::create("y", double(curve.curve[i].y)).release());
    IDP_AddToGroup(point, IDP_NewString(handle_name(curve.curve[i].flag), "handle"));
    IDP_SetIndexArray(points, i, point);
    MEM_delete(point);
  }
  IDP_ReplaceInGroup(result, points);
  if (!BKE_curvemapping_idprop_validate(*result, error)) {
    IDP_FreeProperty(result);
    return nullptr;
  }
  return result;
}

}  // namespace blender
