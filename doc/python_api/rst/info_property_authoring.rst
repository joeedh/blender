Explicit Brush and Scene authoring undo
======================================

The fork exposes an explicit property-authoring transaction on editable original
Brush and Scene IDs. Ordinary memfile undo preserves Brush data and selected
Scene tool settings. This API supplies a bounded snapshot for those authoring
edits without changing that global policy::

   token = brush.authoring_edit_begin(native_settings=True)
   try:
       brush.strength = 0.375
       brush["my_settings"] = {"enabled": True}
   except Exception:
       brush.authoring_edit_cancel(token)
       raise
   else:
       brush.authoring_edit_commit(token, "Edit Brush Settings")

``native_settings`` and ``undo`` are keyword-only exact booleans, defaulting to
False and True respectively. Begin captures before mutation. Commit and cancel
return whether semantic state changed. The opaque token is single-use and tied
to its owner, current Main, authoring revision and load/undo epoch. A nested scope
on the same owner rejects. Token disposal frees the snapshot without restoring
or committing; callers must explicitly finish their edits.

Interactive undo requires global undo enabled, a positive history limit, a
memfile-compatible context, and no pending, grouped or nested undo operator.
An operator using this API must omit both UNDO and UNDO_GROUPED options. Otherwise
it could create duplicate history entries. The owned CurveMapping template uses
this boundary automatically for Brush/Scene creation and committed edits.
Native stock curve widgets keep their existing behavior.

Use ``undo=False`` for rollback-only/background edits: no history initialization
or push occurs, but cancel still restores the snapshot. Changed interactive
commits create one visible history entry, obey count/memory limits and truncate
redo. No-op commits and cancels preserve redo. A history change during an edit
rejects commit. Tokens cannot be reused across load, undo, redo or owner removal.

Snapshot scope
--------------

Every scope captures custom and system IDProperty trees, including undeclared
properties, order, flags, UI metadata, arrays and ID references. Snapshots are
bounded to 64 levels, 100000 property nodes and 64 MiB. Unsupported or oversized
data rejects before mutation. ID links are stored by session identity and type;
same-name replacement is not accepted. Cancel preflights every referenced ID
before restoring. Undo skips a removed/replaced owner or missing reference with
a diagnostic, rather than resurrecting it or redirecting by name.

``native_settings=True`` adds this v1 scope:

* Brush pixel/world size pair and size mode, strength, spacing, plane offset,
  crease pinch, hardness, autosmooth, colors and cursor colors, brush type,
  direction, stroke/falloff policies, pressure flags, accumulate and spacing
  attenuation, native size/strength/color unified flags, and native size,
  strength and distance-falloff mappings.
* Scene sculpt unified pixel/world size pair, mode, strength and colors, and
  legacy size/strength/color unified enable bits.
* The owner's sculpt cavity normal/inverted/use-curve flags, blur steps, factor
  and mapping.

Only listed fields and flag bits restore. Native texture resources, gradients,
preview state, jitter mappings, other paint modes and ID identity/asset metadata
are excluded. Requested native Paint/cavity blocks must already exist; capture
does not allocate them. Generic-only scopes remain available when they do not.
The API is not arbitrary operator or whole-ID undo.

Restoration writes exact stored size pairs without invoking proportional native
setters, retires owned-curve handles, updates the actual owner and dirties its
asset when applicable. It calls no addon restore callbacks and works with the
addon disabled. Reacquire stores, groups, nested RNA and mapping wrappers after
restore. ``ID.authoring_revision()`` is a transient restoration counter that
can supplement load/undo handler generation guards; never serialize it.

Native curve fingerprints
-------------------------

``ID.authoring_native_curve_key(path)`` returns canonical bytes for an allowed
native mapping. Brush paths are ``curve_strength``, ``curve_size``,
``curve_distance_falloff`` and ``mesh_automasking_settings.cavity_curve``.
The Scene path is ``tool_settings.sculpt.mesh_automasking_settings.cavity_curve``.
Paths must be exact nonempty strings of at most 1024 bytes without NUL.

The key includes points, handle modes, channel defaults, evaluation flags,
clipping, extension, tone and black/white settings. It excludes UI selection,
view rectangles, runtime lookup tables and timestamps. Compare complete bytes;
a hash alone is insufficient. Inspection does not sample or allocate a saved
mapping. All authoring methods require the main thread; edits additionally
require a writable Python context and reject evaluated and override IDs.
