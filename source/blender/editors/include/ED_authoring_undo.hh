/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
namespace blender {
struct ID;
struct CurveMapping;
struct UndoType;
struct bContext;
namespace ed {
struct AuthoringEdit;
/**
 * Register a callback invoked (main thread only) whenever an authoring rollback is about to
 * replace an owner's full custom-property tree, so any subsystem caching views over that tree
 * (e.g. the owned-curve-mapping system) can invalidate them before the pointers dangle. A no-op
 * default is used if nothing registers a hook.
 */
void authoring_register_owner_invalidate_hook(std::function<void(const ID *)> hook);
std::shared_ptr<AuthoringEdit> authoring_edit_begin(
    bContext *C, ID &owner, bool native_settings, bool undo, std::string &error);
bool authoring_edit_finish(bContext *C,
                           ID &owner,
                           AuthoringEdit &edit,
                           bool commit,
                           const char *name,
                           bool &changed,
                           std::string &error);
uint64_t authoring_revision(const ID &owner);
bool authoring_curve_key(const CurveMapping &mapping, std::string &key);
void authoring_undosys_type(UndoType *type);
}  // namespace ed
}  // namespace blender
