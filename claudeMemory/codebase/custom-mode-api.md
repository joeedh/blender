# Custom object mode API — reference

How this branch lets a Python addon register a first-class object mode. All
paths relative to the repo root.

## Data model

- `OB_MODE_CUSTOM` — `source/blender/makesdna/DNA_object_enums.h`. A bit in
  `Object::mode` meaning "in a custom (addon-defined) mode".
- `Object::custom_mode_id` — `source/blender/makesdna/DNA_object_types.h`. The
  `bl_idname` of the mode the object is in (e.g. `"sculptcore.sculpt"`).

## Registry & lifecycle (C/C++)

- `source/blender/blenkernel/BKE_object_modes.hh` +
  `blenkernel/intern/object_modes_custom.cc` — the `ObjectModeType` registry:
  `BKE_object_mode_type_add` / `_find` / `_remove`, draw-provider attach, and
  dispatch of the enter/exit/flush/refresh callbacks.
- `source/blender/editors/object/object_edit.cc`,
  `editors/object/object_modes.cc` — editor-side enter/exit and the operators:
  - `OBJECT_OT_custom_mode_toggle`
  - `OBJECT_OT_custom_mode_undo_push`
  - `OBJECT_OT_mode_set_with_submode`
  Registered in `editors/object/object_ops.cc`, declared in
  `editors/object/object_intern.hh`.
- `windowmanager/intern/wm_toolsystem.cc`, `wm_init_exit.cc` — tool-system and
  shutdown integration.

## Python registration (RNA)

- `source/blender/makesrna/intern/rna_object_mode.cc` — defines
  `bpy.types.ObjectModeType`. This is the whole public API a mode subclasses.
- `source/blender/python/intern/bpy_rna.cc` — the register/unregister plumbing
  that binds a Python subclass to a `ObjectModeType`.

Class fields: `bl_idname`, `bl_label`, `bl_icon`, `bl_object_types`,
`bl_keymap`, `bl_default_tool`, `bl_brush_asset_shelf`, `bl_draw_provider`,
`bl_use_custom_undo`, `bl_use_sculpt_paint`.

Callbacks: `enter(context, ob)`, `exit(context, ob)`, `flush(ob)`,
`refresh(context, ob)`, `undo_decode(context, ob, state_id, direction,
is_final)`, `undo_free(state_id)`, `draw_cursor(context, x, y)`.

The canonical consumer is `sculptcore_addon/__init__.py` (`SculptCoreMode`) in
the addon repo — read it to see every field/callback used in anger.

## Wrapped custom-mode undo

- `source/blender/editors/undo/custom_mode_undo.cc` — a custom undo step type
  that stores an addon-provided `state_id` and, on undo/redo, calls the mode's
  `undo_decode(..., direction, is_final)`; `undo_free` releases the state.
- `editors/undo/ed_undo.cc`, `editors/undo/undo_system_types.cc`,
  `BKE_undo_system.hh`, `blenkernel/intern/undo_system.cc` — step-type
  registration and dispatch.
- The Mesh ID remains authoritative for save / memfile undo; the wrapped step
  is a finer-grained overlay while in the mode. `ED_undo.hh` exposes the entry
  points the operator uses.

## External draw provider

- `source/blender/blenkernel/BKE_object_draw_provider.hh` — the ABI:
  `ExternalDrawProvider`, `ExternalDrawNode`, `ExternalDrawAttrRequest`,
  `BKE_EXTERNAL_DRAW_ABI_VERSION`. A provider returns per-node geometry
  (positions/normals/attrs, a stable `node_id`, bounds) for an object key.
- `source/blender/draw/intern/draw_external.cc/.hh`,
  `draw/intern/draw_context.cc` — the draw-manager side that caches and batches
  provider nodes (keyed by `node_id`), with per-node GPU cache reuse.
- Consumers: `draw/engines/workbench/workbench_engine.cc` (+ `_private.hh`,
  `_state.cc`), `draw/engines/eevee/eevee_sync.cc`, and the overlay engine
  headers (`overlay_outline/prepass/wireframe/facing/fade/mode_transfer.hh`)
  gate on custom-mode objects so overlays match the external geometry.
- A mode opts in by setting `bl_draw_provider` to the provider handle
  (stringified pointer from the engine). Without it, the object falls back to
  the normal flush-to-Mesh draw path.

## Multires reshape-from-coords

- `source/blender/blenkernel/BKE_multires.hh`,
  `blenkernel/intern/multires_reshape_vertcos.cc`,
  `blenkernel/intern/multires_reshape.cc/.hh` — engine-agnostic helpers to
  push a flat vertex-coordinate array back onto a multires grid. Used by the
  addon's multires import/export; independently useful.

## Vanilla-UI hooks (Python)

- `scripts/startup/bl_ui/space_view3d.py` — the mode dropdown lists registered
  custom modes generically.
- `scripts/startup/bl_ui/properties_paint_common.py` — paint panels resolve to
  a custom mode's brush context when `bl_use_sculpt_paint` is set.

Both key off the registered mode type, not any specific addon name.
