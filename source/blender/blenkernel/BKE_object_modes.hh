/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * Registry of addon-registered object modes (#OB_MODE_CUSTOM).
 *
 * A custom mode is a first-class object mode implemented by an addon: it is
 * registered from Python as a #bpy.types.ObjectModeType, appears in the mode
 * dropdown, owns a keymap and toolbar context, and receives enter/exit/flush/
 * refresh callbacks. All custom modes share the single #OB_MODE_CUSTOM bit in
 * #Object.mode; the active mode of an object is identified by
 * #Object.custom_mode_id, which holds the registered idname.
 */

#include "DNA_listBase.h"

#include "RNA_types.hh"

namespace blender {

struct bContext;
struct Object;
struct ExternalDrawProvider;

struct ObjectModeType {
  ObjectModeType *next, *prev;

  /**
   * Unique identifier, also returned by #CTX_data_mode_string while the mode
   * is active (so panel `bl_context` and tool matching key off it). Use an
   * addon-prefixed, lowercase name (e.g. "my_addon.sculpt2").
   */
  char idname[/*BKE_ST_MAXNAME*/ 64];
  /**
   * The idname mangled into a valid RNA struct identifier (dots become
   * underscores); referenced by the registered #StructRNA, so it must
   * persist on the type.
   */
  char srna_idname[64];
  /** UI label for the mode dropdown / 3D viewport header. */
  char label[64];
  /** #BIFIconID for the mode dropdown / header. */
  int icon;
  /**
   * Bitmask over #Object.type (`1 << OB_MESH`, ...) of object types the mode
   * supports; feeds #ed::object::mode_compat_test.
   */
  uint64_t object_type_mask;
  /** Keymap name ensured at registration; empty for no keymap. */
  char keymap[64];
  /** Idname of the tool made active on mode entry (the tool system's default
   * for the mode); empty falls back to the generic default. */
  char default_tool[64];
  /** #eObjectModeTypeFlag. */
  int flag;

  /**
   * Callbacks, dispatched through the RNA trampolines
   * (`rna_object_mode.cc`); null when the registered class does not define
   * the corresponding function.
   */
  void (*enter)(ObjectModeType *mt, bContext *C, Object *ob);
  void (*exit)(ObjectModeType *mt, bContext *C, Object *ob);
  /** Write deferred mode state into the object's data ID (the sculpt
   * `needs_flush_to_id` pattern): runs before memfile undo encode, file save
   * and render. */
  void (*flush)(ObjectModeType *mt, Object *ob);
  /** Re-sync mode session state after undo/redo or ID reload replaced the
   * underlying data. */
  void (*refresh)(ObjectModeType *mt, bContext *C, Object *ob);

  /**
   * Wrapped (Tier-2) undo, used when the mode sets
   * #OBJECT_MODE_TYPE_USE_CUSTOM_UNDO. The addon owns the real per-step state
   * (keyed by an integer id it manages); Blender's undo stack stores just the
   * id + a truthful byte size. `undo_decode` applies the addon's delta for a
   * step (`direction`: -1 undo, +1 redo; `is_final`: true when this step is the
   * transition's destination, false for an intermediate step being left behind
   * — the type decodes the active step, so an undo passes the leaving step as
   * non-final then the destination as final). `undo_free` drops the state. Null
   * when the registered class does not define them.
   */
  void (*undo_decode)(
      ObjectModeType *mt, bContext *C, Object *ob, int state_id, int direction, bool is_final);
  void (*undo_free)(ObjectModeType *mt, int state_id);

  /**
   * Draw the mode's cursor overlay (brush circle etc.) in the 3D viewport.
   * Dispatched through the WM paint-cursor mechanism, so the region redraws
   * on every mouse move while an object is in the mode. `x`/`y` are
   * region-local pixel coordinates; the window's pixel projection is active
   * with the model-view translated to the region origin, so drawing at
   * region-local pixel coordinates is correct. Null when the registered
   * class does not define it.
   */
  void (*draw_cursor)(ObjectModeType *mt, bContext *C, int x, int y);
  /** The #wmPaintCursor activation owning `draw_cursor` dispatch; managed by
   * the RNA register/unregister layer. */
  void *paint_cursor_handle;

  /**
   * External draw provider (see #BKE_object_draw_provider.hh): when set, objects
   * in this mode draw from provider-described CPU geometry instead of the
   * evaluated mesh. Borrowed pointer, set via #BKE_object_mode_draw_provider_set;
   * null when the mode uses the default (flush-to-Mesh) draw path.
   */
  const ExternalDrawProvider *draw_provider;

  /**
   * The persistent Python instance of the registered class (one per type,
   * created lazily on the first trampoline call, released at unregister).
   */
  void *py_instance;

  /* RNA integration. */
  ExtensionRNA rna_ext;
};

/** #ObjectModeType.flag */
enum eObjectModeTypeFlag {
  /** The mode opts into the wrapped custom undo type instead of memfile. */
  OBJECT_MODE_TYPE_USE_CUSTOM_UNDO = (1 << 0),
  /**
   * The mode's tools use the sculpt paint settings (#ToolSettings.sculpt):
   * paint-context lookups (#BKE_paint_get_active) resolve to it while the
   * mode is active, so brush-driven UI (e.g. the texture properties tab's
   * brush texture user) works as in sculpt mode.
   */
  OBJECT_MODE_TYPE_USE_SCULPT_PAINT = (1 << 1),
};

/**
 * Add a type to the registry. Ownership transfers to the registry (the type
 * and its idname must outlive registration; freed by remove/exit with
 * #MEM_freeN). Fails (returns false) when the idname is empty or already
 * registered.
 */
bool BKE_object_mode_type_add(ObjectModeType *mt);

/** Remove and free a registered type. Does not exit objects using the mode —
 * callers (RNA unregister) must force-exit first. */
void BKE_object_mode_type_remove(ObjectModeType *mt);

/** Find a registered type by idname; null when not registered. */
ObjectModeType *BKE_object_mode_type_find(const char *idname);

/** The default-tool idname for the object's active custom mode, or null when
 * the object is not in a registered custom mode / the mode declares none. */
const char *BKE_object_custom_mode_default_tool(const Object *ob);

/** The registry, for iteration (mode dropdown enum items etc.). */
ListBaseT<ObjectModeType> &BKE_object_mode_types_get();

/** True when `ob`'s type is in `mt`'s supported object-type mask. */
bool BKE_object_mode_type_poll_object(const ObjectModeType *mt, const Object *ob);

/** True when `ob`'s active custom mode opts into the wrapped undo type. */
bool BKE_object_custom_mode_uses_custom_undo(const Object *ob);

/** True when `ob`'s active custom mode declares its tools use the sculpt
 * paint settings (#OBJECT_MODE_TYPE_USE_SCULPT_PAINT). */
bool BKE_object_custom_mode_uses_sculpt_paint(const Object *ob);

/** Free every registered type (called from WM exit). */
void BKE_object_mode_types_exit();

}  // namespace blender
