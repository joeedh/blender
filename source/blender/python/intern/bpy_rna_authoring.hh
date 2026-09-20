/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <Python.h>
namespace blender {
extern PyMethodDef BPY_rna_authoring_begin_method_def;
extern PyMethodDef BPY_rna_authoring_commit_method_def;
extern PyMethodDef BPY_rna_authoring_cancel_method_def;
extern PyMethodDef BPY_rna_authoring_revision_method_def;
extern PyMethodDef BPY_rna_authoring_curve_key_method_def;
}  // namespace blender
