/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup pygen
 */

#pragma once

#include <Python.h>

namespace blender {

struct IDProperty;

extern PyTypeObject BPy_IDPropertyUIManager_Type;

struct BPy_IDPropertyUIManager {
  PyObject_HEAD
  IDProperty *property;
};

PyObject *BPy_IDPropertyUIData_update(IDProperty *property, PyObject *kwargs);
PyObject *BPy_IDPropertyUIData_as_dict(IDProperty *property);

void IDPropertyUIData_Init_Types();

}  // namespace blender
