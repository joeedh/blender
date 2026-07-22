# CLAUDE.md — Blender fork: custom object modes

Guidance for the `custom-object-modes` branch of this Blender fork.

## What this branch is

A focused set of **engine-agnostic core changes** that let an addon register a
first-class **object mode from Python** — the same footing as Edit/Sculpt/Paint
mode, not a modal operator. Nothing sculpt-specific lives here; the branch is
meant to stand on its own (and, eventually, to be proposable upstream).

The companion sculpt engine and its addon live in a **separate repository**
(`sculptcore-blender-addon`, with the SculptCore engine as a submodule). A
build of this branch is a normal Blender that *can host* Python-registered
modes; it ships no sculpt mode itself.

## Building

Standard Blender build — this branch adds no new build options or external
dependencies. Existing out-of-source build trees live beside the checkout
(e.g. `../build_windows_x64_clang_RelWithDebInfo`). Use the normal `make` /
CMake preset flow; there is no `WITH_SCULPTCORE` here (that glue was
deliberately left in the engine/addon repo).

## The API surface (what this branch adds)

Registration (Python):
- **`bpy.types.ObjectModeType`** — `source/blender/makesrna/intern/rna_object_mode.cc`.
  Subclass and `bpy.utils.register_class` it to define a mode. Class fields:
  `bl_idname`, `bl_label`, `bl_icon`, `bl_object_types` (e.g. `{'MESH'}`),
  `bl_keymap`, `bl_default_tool`, `bl_brush_asset_shelf`, `bl_draw_provider`
  (an external-draw provider handle as a string), `bl_use_custom_undo`,
  `bl_use_sculpt_paint`. Callbacks: `enter(context, ob)`, `exit(context, ob)`,
  `flush(ob)`, `refresh(context, ob)`, `undo_decode(context, ob, state_id,
  direction, is_final)`, `undo_free(state_id)`, `draw_cursor(context, x, y)`.
  The Python RNA plumbing is in `bpy_rna.cc`.

Core (C/C++):
- **`OB_MODE_CUSTOM`** (`DNA_object_enums.h`) + `Object::custom_mode_id`
  (`DNA_object_types.h`) — the object is "in a custom mode named X".
- **Mode registry / lifecycle** — `BKE_object_modes.hh`,
  `blenkernel/intern/object_modes_custom.cc` (`BKE_object_mode_type_add/find`,
  draw-provider attach, enter/exit/flush dispatch). Editor-side enter/exit and
  operators (`OBJECT_OT_custom_mode_toggle`, `OBJECT_OT_custom_mode_undo_push`,
  `OBJECT_OT_mode_set_with_submode`) in `editors/object/object_edit.cc` and
  `object_modes.cc`.
- **Wrapped custom-mode undo** — `editors/undo/custom_mode_undo.cc` (+
  `ed_undo.cc`, `undo_system_types.cc`). A custom undo step wraps an addon
  state id and calls back into the mode's `undo_decode`/`undo_free`; the Mesh
  ID stays authoritative for save/memfile.
- **External draw provider** — `BKE_object_draw_provider.hh`,
  `draw/intern/draw_external.cc/.hh` (ABI `BKE_EXTERNAL_DRAW_ABI_VERSION`;
  `ExternalDrawProvider`/`ExternalDrawNode`). Lets a mode draw an object's
  geometry from its own per-node caches instead of the Mesh. Consumed by
  Workbench (`workbench_engine.cc`), EEVEE (`eevee_sync.cc`), and the overlay
  engine (outline/prepass/wireframe/facing/fade/mode-transfer gating).
- **Multires reshape-from-coords API** — `BKE_multires.hh`,
  `blenkernel/intern/multires_reshape_vertcos.cc` + `multires_reshape.cc`.
  Engine-agnostic helpers to push a flat vertex-coordinate array back onto a
  multires grid (used by the addon's multires import/export, useful on its own).
- **Vanilla-UI hooks** — `scripts/startup/bl_ui/space_view3d.py`,
  `properties_paint_common.py`: the mode dropdown and paint panels recognize
  registered custom modes generically (no addon name hardcoded).

## Conventions

- **C/C++ & RNA** follow Blender's own style (clang-format the repo config,
  `snake_case`, doxygen `\name` section banners, SPDX headers). Match the
  surrounding code.
- **Python** follows Blender's guidelines (PEP 8, 4-space, 120 cols).
- Prefix scaffolding/helper comments with `CLAUDENOTE:` so they are greppable
  and can be stripped before a change is called done. **The branch is currently
  free of `CLAUDENOTE` markers** — keep it that way.
- Kept deliberately: two `__declspec(no_sanitize_address)` attributes in
  `bpy_rna.cc` (`bpy_class_validate_recursive`, `bpy_class_call`). They are
  inert outside MSVC ASAN builds and suppress genuine false positives from the
  bundled CPython's poisoned obmalloc pools. Not scaffolding — do not treat the
  now-neutral comments as removable.

## Related repositories

- **`sculptcore-blender-addon`** — the sculpt mode addon + engine submodule; it
  is the primary consumer of this API and the reference for how a real mode is
  built on it.
