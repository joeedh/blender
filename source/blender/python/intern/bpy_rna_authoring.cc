/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "BKE_global.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BLI_threads.hh"
#include "BLI_utildefines.hh"
#include "ED_authoring_undo.hh"
#include "RNA_access.hh"
#include "RNA_path.hh"
#include "RNA_prototypes.hh"
#include "bpy_capi_utils.hh"
#include "bpy_rna.hh"
#include "bpy_rna_authoring.hh"
#include <Python.h>
#include <cstring>
#include <memory>
#include <string>

namespace blender {
namespace {
using Edit = std::shared_ptr<ed::AuthoringEdit>;
constexpr const char *capsule_name = "bpy.PropertyAuthoringEdit";
struct GCGuard {
  bool enabled = PyGC_Disable();
  ~GCGuard()
  {
    if (enabled) {
      PyGC_Enable();
    }
  }
};
ID *owner_get(BPy_StructRNA *self, bool write)
{
  if (!BLI_thread_is_main() || (write && !pyrna_write_check())) {
    PyErr_SetString(PyExc_PermissionError, "Authoring access requires the writable main thread");
    return nullptr;
  }
  if (pyrna_struct_validity_check(self) == -1) {
    return nullptr;
  }
  ID *id = self->ptr->owner_id;
  if (!id || self->ptr->data != id || !RNA_struct_is_ID(self->ptr->type) ||
      !BKE_id_is_in_main(G_MAIN, id) || (id->tag & ID_TAG_COPIED_ON_EVAL))
  {
    PyErr_SetString(PyExc_ValueError, "Expected an original current Main ID");
    return nullptr;
  }
  return id;
}
PyObject *fail(const std::string &error)
{
  PyErr_SetString(PyExc_ValueError, error.c_str());
  return nullptr;
}
void capsule_free(PyObject *capsule)
{
  delete static_cast<Edit *>(PyCapsule_GetPointer(capsule, capsule_name));
}
PyObject *begin(BPy_StructRNA *self, PyObject *args, PyObject *kw)
{
  GCGuard gc;
  ID *owner = owner_get(self, true);
  if (!owner) {
    return nullptr;
  }
  PyObject *native = Py_False, *undo = Py_True;
  static const char *names[] = {"native_settings", "undo", nullptr};
  if (!PyArg_ParseTupleAndKeywords(
          args, kw, "|$OO:authoring_edit_begin", const_cast<char **>(names), &native, &undo))
  {
    return nullptr;
  }
  if (!PyBool_Check(native) || !PyBool_Check(undo)) {
    return fail("Flags must be exact booleans");
  }
  std::string error;
  auto edit = ed::authoring_edit_begin(
      BPY_context_get(), *owner, native == Py_True, undo == Py_True, error);
  if (!edit) {
    return fail(error);
  }
  auto *holder = new Edit(std::move(edit));
  PyObject *capsule = PyCapsule_New(holder, capsule_name, capsule_free);
  if (!capsule) {
    delete holder;
  }
  return capsule;
}
PyObject *finish(BPy_StructRNA *self, PyObject *args, bool commit)
{
  GCGuard gc;
  ID *owner = owner_get(self, true);
  if (!owner) {
    return nullptr;
  }
  PyObject *capsule;
  const char *name = "Edit Property";
  if (!PyArg_ParseTuple(
          args, commit ? "O|s:authoring_edit_commit" : "O:authoring_edit_cancel", &capsule, &name))
  {
    return nullptr;
  }
  auto *holder = static_cast<Edit *>(PyCapsule_GetPointer(capsule, capsule_name));
  if (!holder) {
    return nullptr;
  }
  std::string error;
  bool changed;
  if (!ed::authoring_edit_finish(
          BPY_context_get(), *owner, **holder, commit, name, changed, error))
  {
    return fail(error);
  }
  return PyBool_FromLong(changed);
}
PyObject *commit(BPy_StructRNA *self, PyObject *args)
{
  return finish(self, args, true);
}
PyObject *cancel(BPy_StructRNA *self, PyObject *args)
{
  return finish(self, args, false);
}
PyObject *revision(BPy_StructRNA *self, PyObject *)
{
  ID *owner = owner_get(self, false);
  return owner ? PyLong_FromUnsignedLongLong(ed::authoring_revision(*owner)) : nullptr;
}
PyObject *curve_key(BPy_StructRNA *self, PyObject *arg)
{
  GCGuard gc;
  ID *owner = owner_get(self, false);
  if (!owner) {
    return nullptr;
  }
  if (!PyUnicode_CheckExact(arg)) {
    return fail("Curve path must be an exact string");
  }
  Py_ssize_t length;
  const char *path = PyUnicode_AsUTF8AndSize(arg, &length);
  if (!path) {
    return nullptr;
  }
  if (length < 1 || length > 1024 || std::memchr(path, 0, length)) {
    return fail("Invalid curve path");
  }
  const std::string name(path, length);
  const bool supported = GS(owner->name) == ID_BR ?
                             ELEM(name,
                                  "curve_strength",
                                  "curve_size",
                                  "curve_distance_falloff",
                                  "mesh_automasking_settings.cavity_curve") :
                             GS(owner->name) == ID_SCE &&
                                 name ==
                                     "tool_settings.sculpt.mesh_automasking_settings.cavity_curve";
  if (!supported) {
    return fail("Unsupported native authoring curve path");
  }
  PointerRNA ptr;
  PropertyRNA *prop = nullptr;
  if (!RNA_path_resolve_full(&*self->ptr, path, &ptr, &prop, nullptr) || prop ||
      ptr.type != RNA_CurveMapping || ptr.owner_id != owner || !ptr.data)
  {
    return fail("Path must resolve to this owner's native CurveMapping");
  }
  std::string key;
  if (!ed::authoring_curve_key(*static_cast<CurveMapping *>(ptr.data), key)) {
    return fail("Unsupported curve data");
  }
  return PyBytes_FromStringAndSize(key.data(), key.size());
}
}  // namespace
PyMethodDef BPY_rna_authoring_begin_method_def = {
    "authoring_edit_begin",
    reinterpret_cast<PyCFunction>(begin),
    METH_VARARGS | METH_KEYWORDS,
    "Capture Brush/Scene properties and optional scoped native settings. Single-use token. "
    "undo=False permits rollback-only background transactions. No commit on token disposal."};
PyMethodDef BPY_rna_authoring_commit_method_def = {
    "authoring_edit_commit",
    reinterpret_cast<PyCFunction>(commit),
    METH_VARARGS,
    "Commit an authoring token, optionally naming its single undo entry. Returns changed."};
PyMethodDef BPY_rna_authoring_cancel_method_def = {
    "authoring_edit_cancel",
    reinterpret_cast<PyCFunction>(cancel),
    METH_VARARGS,
    "Restore an authoring token without pushing undo. Invalidates retained owner subdata."};
PyMethodDef BPY_rna_authoring_revision_method_def = {
    "authoring_revision",
    reinterpret_cast<PyCFunction>(revision),
    METH_NOARGS,
    "Process-local owner restoration revision; not a persistent identifier."};
PyMethodDef BPY_rna_authoring_curve_key_method_def = {
    "authoring_native_curve_key",
    reinterpret_cast<PyCFunction>(curve_key),
    METH_O,
    "Nonmutating canonical native curve evaluation bytes. Excludes UI and cache state."};
}  // namespace blender
