/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <Python.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../generic/idprop_py_api.hh"
#include "../generic/idprop_py_ui_api.hh"
#include "BKE_brush.hh"
#include "BKE_global.hh"
#include "BKE_idprop.hh"
#include "BKE_lib_id.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BLI_listbase.hh"
#include "BLI_threads.hh"
#include "BLI_utildefines.hh"
#include "DEG_depsgraph.hh"
#include "DNA_ID.h"
#include "DNA_brush_types.h"
#include "RNA_access.hh"
#include "RNA_enum_types.hh"
#include "WM_api.hh"
#include "WM_types.hh"
#include "bpy_rna.hh"
#include "bpy_rna_idprops.hh"

namespace blender {
namespace {

/* No Python callbacks or GIL release are allowed in the transaction. GC finalizers
 * are callbacks too. Restore GC only after all staging temporaries are destroyed. */
struct GCGuard {
  bool enabled = PyGC_Disable();
  ~GCGuard()
  {
    if (enabled) {
      PyGC_Enable();
    }
  }
};
struct PropFree {
  void operator()(IDProperty *prop) const
  {
    if (prop) {
      IDP_FreeProperty_ex(prop, false);
    }
  }
};
using Prop = std::unique_ptr<IDProperty, PropFree>;
struct PyFree {
  void operator()(PyObject *object) const
  {
    Py_XDECREF(object);
  }
};
using PyRef = std::unique_ptr<PyObject, PyFree>;

bool error(const char *message)
{
  PyErr_SetString(PyExc_ValueError, message);
  return false;
}

bool key_string(PyObject *object, std::string &result)
{
  if (!PyUnicode_CheckExact(object)) {
    return error("Property keys must be exact strings");
  }
  Py_ssize_t size;
  const char *text = PyUnicode_AsUTF8AndSize(object, &size);
  if (!text) {
    return false;
  }
  if (size < 1 || size >= MAX_IDPROP_NAME || std::memchr(text, 0, size)) {
    return error("Property keys require 1..63 UTF-8 bytes without NUL");
  }
  result.assign(text, size);
  return true;
}

bool scalar_value(PyObject *object)
{
  if (PyBool_Check(object)) {
    return true;
  }
  if (PyLong_CheckExact(object)) {
    const long long value = PyLong_AsLongLong(object);
    if (PyErr_Occurred()) {
      return false;
    }
    return (value >= INT32_MIN && value <= INT32_MAX) || error("Integer must fit int32");
  }
  if (PyFloat_CheckExact(object)) {
    return std::isfinite(PyFloat_AS_DOUBLE(object)) || error("Float must be finite");
  }
  if (PyUnicode_CheckExact(object)) {
    Py_ssize_t size;
    const char *text = PyUnicode_AsUTF8AndSize(object, &size);
    if (!text) {
      return false;
    }
    return (size <= 1024 * 1024 && !std::memchr(text, 0, size)) ||
           error("String must fit one MiB and contain no NUL");
  }
  return error("Only exact bool, int32, finite float and string scalar values are supported");
}

bool ui_valid(PyObject *ui)
{
  if (ui == Py_None) {
    return true;
  }
  if (!PyDict_CheckExact(ui)) {
    return error("UI metadata must be an exact dict or None");
  }
  Py_ssize_t position = 0;
  PyObject *key, *value;
  while (PyDict_Next(ui, &position, &key, &value)) {
    std::string name;
    if (!key_string(key, name)) {
      return false;
    }
    if (!ELEM(name,
              "min",
              "max",
              "soft_min",
              "soft_max",
              "step",
              "precision",
              "default",
              "description",
              "subtype"))
    {
      return error("Unsupported scalar UI keyword");
    }
    if (value != Py_None && !scalar_value(value)) {
      return false;
    }
    if (name == "step" && value != Py_None &&
        (PyFloat_CheckExact(value) || PyLong_CheckExact(value)))
    {
      if (!std::isfinite(float(PyFloat_AsDouble(value)))) {
        return error("UI step must fit finite float storage");
      }
    }
  }
  return true;
}

bool scalar_property(const IDProperty *prop)
{
  return ELEM(prop->type, IDP_INT, IDP_FLOAT, IDP_DOUBLE, IDP_BOOLEAN, IDP_STRING);
}

struct Edit {
  std::vector<std::string> path;
  Prop value;
  PyObject *ui = Py_None; /* Borrowed from exact builtin arguments. */
};

bool parse_edits(PyObject *operations, std::vector<Edit> &edits)
{
  if (!PyTuple_CheckExact(operations) && !PyList_CheckExact(operations)) {
    return error("Operations must be an exact tuple or list");
  }
  const Py_ssize_t count = PySequence_Size(operations);
  if (count > 1024) {
    return error("At most 1024 operations per transaction");
  }
  for (Py_ssize_t i = 0; i < count; i++) {
    PyObject *op = PyTuple_CheckExact(operations) ? PyTuple_GET_ITEM(operations, i) :
                                                    PyList_GET_ITEM(operations, i);
    if (!PyTuple_CheckExact(op) || !ELEM(PyTuple_GET_SIZE(op), 2, 4)) {
      return error("Expected SET or DELETE operation tuple");
    }
    PyObject *token = PyTuple_GET_ITEM(op, 0);
    if (!PyUnicode_CheckExact(token)) {
      return error("Operation token must be an exact string");
    }
    const bool set = PyUnicode_CompareWithASCIIString(token, "SET") == 0;
    const bool erase = PyUnicode_CompareWithASCIIString(token, "DELETE") == 0;
    if ((!set && !erase) || PyTuple_GET_SIZE(op) != (set ? 4 : 2)) {
      return error("Invalid SET or DELETE operation shape");
    }
    PyObject *path = PyTuple_GET_ITEM(op, 1);
    if (!PyTuple_CheckExact(path) || PyTuple_GET_SIZE(path) < 1 || PyTuple_GET_SIZE(path) > 32) {
      return error("Path must be an exact tuple with 1..32 keys");
    }
    Edit edit;
    for (Py_ssize_t j = 0; j < PyTuple_GET_SIZE(path); j++) {
      std::string key;
      if (!key_string(PyTuple_GET_ITEM(path, j), key)) {
        return false;
      }
      edit.path.push_back(std::move(key));
    }
    for (const Edit &other : edits) {
      const size_t common = std::min(edit.path.size(), other.path.size());
      if (std::equal(edit.path.begin(), edit.path.begin() + common, other.path.begin())) {
        return error("Duplicate or overlapping paths are not allowed");
      }
    }
    if (set) {
      PyObject *value = PyTuple_GET_ITEM(op, 2);
      edit.ui = PyTuple_GET_ITEM(op, 3);
      if (!scalar_value(value) || !ui_valid(edit.ui)) {
        return false;
      }
      edit.value.reset(
          BPy_IDProperty_FromPyObject(nullptr, edit.path.back().c_str(), value, false, true));
      if (!edit.value) {
        return false;
      }
    }
    edits.push_back(std::move(edit));
  }
  return true;
}

bool bounded_tree(const IDProperty *root)
{
  std::vector<std::pair<const IDProperty *, int>> pending{{root, 0}};
  size_t visited = 0;
  while (!pending.empty()) {
    const auto [prop, depth] = pending.back();
    pending.pop_back();
    if (depth > 64 || ++visited > 100000) {
      return error("Existing root exceeds traversal limits");
    }
    if (prop->type == IDP_GROUP) {
      for (const IDProperty &child : prop->data.group) {
        pending.emplace_back(&child, depth + 1);
      }
    }
    else if (prop->type == IDP_IDPARRAY) {
      const IDProperty *items = IDP_property_array_get(prop);
      for (int i = 0; i < prop->len; i++) {
        pending.emplace_back(&items[i], depth + 1);
      }
    }
    else if (prop->type == IDP_ARRAY) {
      if (!ELEM(prop->subtype, IDP_INT, IDP_FLOAT, IDP_DOUBLE, IDP_BOOLEAN)) {
        return error("Unsupported legacy array in root; data preserved");
      }
    }
    else if (!scalar_property(prop) && prop->type != IDP_ID) {
      return error("Unsupported property in root; data preserved");
    }
  }
  return true;
}

Prop new_group(const char *name)
{
  IDPropertyTemplate value{};
  return Prop(IDP_New(IDP_GROUP, &value, name));
}

/* Compare detached leaves only. The UI helper can normalize defaults by ensuring
 * UI data, so it must never be called on an original with absent UI metadata. */
int same_leaf(IDProperty *a, IDProperty *b, const bool compare_ui)
{
  if (a->type != b->type || a->subtype != b->subtype || a->flag != b->flag ||
      !IDP_EqualsProperties(a, b))
  {
    return 0;
  }
  if (!compare_ui) {
    return 1;
  }
  if (!a->ui_data || !b->ui_data) {
    return a->ui_data == b->ui_data;
  }
  PyRef first(BPy_IDPropertyUIData_as_dict(a));
  PyRef second(BPy_IDPropertyUIData_as_dict(b));
  if (!first || !second) {
    return -1;
  }
  return PyObject_RichCompareBool(first.get(), second.get(), Py_EQ);
}

bool apply_edit(IDProperty *root, Edit &edit, bool &changed)
{
  IDProperty *parent = root;
  for (size_t i = 0; i + 1 < edit.path.size(); i++) {
    IDProperty *child = IDP_GetPropertyFromGroup(parent, edit.path[i].c_str());
    if (!child) {
      if (!edit.value) {
        return true;
      }
      Prop group = new_group(edit.path[i].c_str());
      child = group.get();
      IDP_AddToGroup(parent, group.release());
    }
    if (child->type != IDP_GROUP) {
      return error("Path ancestor is not a group; data preserved");
    }
    parent = child;
  }
  IDProperty *old = IDP_GetPropertyFromGroup(parent, edit.path.back().c_str());
  if (old && !scalar_property(old)) {
    return error("Cannot replace or delete a non-scalar leaf");
  }
  if (!edit.value) {
    if (old) {
      IDP_FreeFromGroup(parent, old);
      changed = true;
    }
    return true;
  }
  if (old && (old->flag & IDP_FLAG_STATIC_TYPE) && old->type != edit.value->type) {
    return error("Cannot change the type of a static property");
  }
  if (old) {
    edit.value->flag = old->flag;
    if (old->type == edit.value->type && old->ui_data) {
      edit.value->ui_data = IDP_ui_data_copy(old);
    }
  }
  if (edit.ui != Py_None) {
    PyRef result(BPy_IDPropertyUIData_update(edit.value.get(), edit.ui));
    if (!result) {
      return false;
    }
  }
  if (old) {
    const int same = same_leaf(old, edit.value.get(), edit.ui != Py_None);
    if (same < 0) {
      return false;
    }
    if (same) {
      return true;
    }
  }
  IDP_ReplaceInGroup_ex(parent, edit.value.release(), old, LIB_ID_CREATE_NO_USER_REFCOUNT);
  changed = true;
  return true;
}

void notify(ID *owner)
{
  DEG_id_tag_update(owner,
                    ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY | ID_RECALC_PARAMETERS |
                        ID_RECALC_SYNC_TO_EVAL);
  WM_main_add_notifier(NC_WINDOW, nullptr);
  WM_main_add_notifier(NC_ID | NA_EDITED, owner);
  switch (GS(owner->name)) {
    case ID_BR:
      BKE_brush_tag_unsaved_changes(id_cast<Brush *>(owner));
      WM_main_add_notifier(NC_BRUSH | NA_EDITED, owner);
      break;
    case ID_SCE:
      WM_main_add_notifier(NC_SCENE | ND_TOOLSETTINGS, owner);
      break;
    case ID_NT:
      WM_main_add_notifier(NC_MATERIAL | ND_SHADING, nullptr);
      break;
    default:
      break;
  }
}

PyObject *system_scalar(BPy_StructRNA *self, PyObject *args)
{
  GCGuard gc;
  if (!BLI_thread_is_main()) {
    PyErr_SetString(PyExc_RuntimeError, "System property inspection requires the main thread");
    return nullptr;
  }
  PyObject *path;
  if (!PyArg_ParseTuple(args, "O:system_property_scalar", &path)) {
    return nullptr;
  }
  if (!PyTuple_CheckExact(path) || PyTuple_GET_SIZE(path) < 1 || PyTuple_GET_SIZE(path) > 32) {
    error("Expected an exact path tuple containing 1..32 keys");
    return nullptr;
  }
  std::vector<std::string> keys;
  for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(path); i++) {
    std::string key;
    if (!key_string(PyTuple_GET_ITEM(path, i), key)) {
      return nullptr;
    }
    keys.push_back(std::move(key));
  }
  if (!self->ptr.has_value()) {
    PyErr_SetString(PyExc_ReferenceError, "ID is no longer available");
    return nullptr;
  }
  PYRNA_STRUCT_CHECK_OBJ(self);
  ID *owner = self->ptr->owner_id;
  if (!G_MAIN || !owner || self->ptr->data != owner || !RNA_struct_is_ID(self->ptr->type) ||
      !BKE_id_is_in_main(G_MAIN, owner) || (owner->tag & ID_TAG_COPIED_ON_EVAL)) {
    error("Expected an original ID in the current Main");
    return nullptr;
  }
  const IDProperty *property = owner->system_properties;
  for (const std::string &key : keys) {
    if (!property || (property->flag & IDP_FLAG_GHOST)) {
      return Py_BuildValue("(OOO)", Py_False, Py_None, Py_None);
    }
    if (property->type != IDP_GROUP) {
      error("System path ancestor is not a group; data preserved");
      return nullptr;
    }
    property = IDP_GetPropertyFromGroup(property, key.c_str());
  }
  if (!property || (property->flag & IDP_FLAG_GHOST)) {
    return Py_BuildValue("(OOO)", Py_False, Py_None, Py_None);
  }
  PyRef value;
  const char *type;
  switch (property->type) {
    case IDP_INT:
      type = "INT";
      value.reset(PyLong_FromLong(IDP_int_get(property)));
      break;
    case IDP_FLOAT:
      type = "FLOAT";
      value.reset(PyFloat_FromDouble(IDP_float_get(property)));
      break;
    case IDP_DOUBLE:
      type = "DOUBLE";
      value.reset(PyFloat_FromDouble(IDP_double_get(property)));
      break;
    case IDP_BOOLEAN:
      type = "BOOLEAN";
      value.reset(PyBool_FromLong(IDP_bool_get(property)));
      break;
    case IDP_STRING: {
      type = "STRING";
      if (property->subtype != IDP_STRING_SUB_UTF8 || property->len < 1 ||
          property->len > 1024 * 1024 + 1 || !IDP_string_get(property) ||
          IDP_string_get(property)[property->len - 1] != 0) {
        error("System string must be bounded UTF-8; data preserved");
        return nullptr;
      }
      value.reset(PyUnicode_DecodeUTF8(IDP_string_get(property), property->len - 1, "strict"));
      break;
    }
    default:
      error("System leaf is not a supported scalar; data preserved");
      return nullptr;
  }
  if (!value) {
    return nullptr;
  }
  return Py_BuildValue("(OsO)", Py_True, type, value.get());
}

PyObject *update_atomic(BPy_StructRNA *self, PyObject *args)
{
  GCGuard gc;
  if (!BLI_thread_is_main() || !pyrna_write_check()) {
    PyErr_SetString(PyExc_PermissionError,
                    "Atomic property updates require a writable main-thread context");
    return nullptr;
  }
  PyObject *root_arg, *operations;
  if (!PyArg_ParseTuple(args, "OO:id_properties_update_atomic", &root_arg, &operations)) {
    return nullptr;
  }
  std::string root_name;
  std::vector<Edit> edits;
  if (!key_string(root_arg, root_name) || !parse_edits(operations, edits)) {
    return nullptr;
  }
  PYRNA_STRUCT_CHECK_OBJ(self);
  ID *owner = self->ptr->owner_id;
  if (!owner || self->ptr->data != owner || !RNA_struct_is_ID(self->ptr->type) ||
      !BKE_id_is_in_main(G_MAIN, owner) || !ID_IS_EDITABLE(owner) ||
      ID_IS_OVERRIDE_LIBRARY(owner) || (owner->tag & ID_TAG_COPIED_ON_EVAL))
  {
    PyErr_SetString(PyExc_PermissionError,
                    "Expected an editable original Main ID without an override");
    return nullptr;
  }
  if (edits.empty()) {
    Py_RETURN_FALSE;
  }
  IDProperty *properties = owner->properties;
  IDProperty *original = properties ? IDP_GetPropertyFromGroup(properties, root_name.c_str()) :
                                      nullptr;
  if (original && original->type != IDP_GROUP) {
    error("Existing root is not a group; data preserved");
    return nullptr;
  }
  if (original && !bounded_tree(original)) {
    return nullptr;
  }
  Prop staged(original ? IDP_CopyProperty_ex(original, LIB_ID_CREATE_NO_USER_REFCOUNT) :
                         new_group(root_name.c_str()).release());
  bool changed = false;
  for (Edit &edit : edits) {
    if (!apply_edit(staged.get(), edit, changed)) {
      return nullptr;
    }
  }
  if (!changed) {
    Py_RETURN_FALSE;
  }
  if (!properties) {
    Prop parent = new_group("");
    IDP_AddToGroup(parent.get(), staged.release());
    owner->properties = parent.release();
  }
  else {
    IDP_ReplaceInGroup_ex(properties, staged.release(), original, LIB_ID_CREATE_NO_USER_REFCOUNT);
  }
  notify(owner);
  Py_RETURN_TRUE;
}

}  // namespace

PyMethodDef BPY_rna_system_property_scalar_method_def = {
    "system_property_scalar",
    reinterpret_cast<PyCFunction>(system_scalar),
    METH_VARARGS,
    "Inspect an original Main ID's raw system property scalar without allocation or repair. "
    "Exact path tuple, 1..32 keys of 1..63 UTF-8 bytes. Returns (present, type, value); "
    "missing or ghost paths return (False, None, None). Main thread only; read-only IDs allowed."};

PyMethodDef BPY_rna_id_properties_update_atomic_method_def = {
    "id_properties_update_atomic",
    reinterpret_cast<PyCFunction>(update_atomic),
    METH_VARARGS,
    "Atomically update scalar leaves and UI metadata in a named custom-property group. "
    "Operations are ('SET', path_tuple, scalar, ui_dict_or_None) or ('DELETE', path_tuple). "
    "Returns whether stored state changed. Does not create undo steps."};

}  // namespace blender
