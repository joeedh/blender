********************
Owned Curve Mappings
********************

This fork provides scalar curve properties which belong to their containing
data-block. They need no node tree or separately saved data-block. Check
``getattr(bpy.props, "owned_curve_mapping_api_version", 0) >= 1`` before
registering code which requires this API. The version describes API availability;
it is independent of the saved definition version and release test status.

Declaration and Initialization
==============================

Declare :func:`bpy.props.CurveMappingProperty` on an ID type or a registered
PropertyGroup. PropertyGroups must ultimately belong to an ID. Reading an unset
curve returns ``None``. It does not create a definition::

   import bpy

   class ResponseSettings(bpy.types.PropertyGroup):
       curve: bpy.props.CurveMappingProperty(name="Response")

   bpy.utils.register_class(ResponseSettings)
   bpy.types.Brush.response = bpy.props.PointerProperty(type=ResponseSettings)

   brush = bpy.data.brushes.new("Example", mode='SCULPT')
   curve = brush.curve_mapping_initialize("response.curve", preset='LINEAR')
   curve.curves[0].points[-1].location = (1.0, 0.75)
   value = curve.evaluate(curve.curves[0], 0.5)

Initialization stages missing pointer ancestors atomically. Collection elements
must already exist and use numeric indices in the path, such as
``"responses[0].curve"``. Existing definitions are returned unchanged. The
initialization preset is currently ``LINEAR``. The declaring parent may instead
call ``curve_mapping_initialize("curve")`` directly. Use
``brush.response.property_unset("curve")`` to remove a definition explicitly.

Only one scalar channel is supported, with finite coordinates, at least two
points and at most 32767 points. RGB, hue, tone and black/white-level controls
are unsupported. Handle types are ``AUTO``, ``AUTO_CLAMPED`` and ``VECTOR``;
extension is ``HORIZONTAL`` or ``EXTRAPOLATED``. Clip bounds must form a
nonempty rectangle within [-100, 100]. NaN/infinite coordinates and invalid
definitions are rejected before publication.

Ownership, Callbacks and Lifetime
================================

The containing ID owns the serialized definition. Copying the ID deep-copies
the curve. Saving a file or an external brush asset saves its own definition;
no other data-block is captured. Unregistering an add-on leaves the saved data
intact. Older Blender readers preserve its ordinary tagged IDProperty group,
but do not provide this API. Unknown schema versions are preserved and rejected,
never silently reset. Explicit removal still discards the definition.

Successful definition changes notify the actual owning ID, including brush
asset dirty state, then call the optional ``update(declaring_parent, context)``
callback. No-op and presentation-only changes do neither. Recursive updates
of the same curve suppress recursive callbacks. A callback may remove its
owner or declaration; it cannot recreate a removed instance of the same
declaration on that owner before returning.

Library-linked read-only owners and library overrides reject edits. Editable
linked brush assets use their actual owner and asset-save lifecycle. Evaluated
copies and unsupported non-Main owners cannot be edited. Embedded ID owners,
such as material node trees, use their own lifetime guards.

Retained mapping and channel wrappers can refresh after a committed change.
Point wrappers, vectors, iterators and editor transactions pin their revision;
acquire them again after editing. Removed owners, removed declarations, raw-data
conflicts, undo and file loading can invalidate handles. Such handles raise
``ReferenceError``. Calls must run on Blender's main thread, including reads;
capturing a bound method does not bypass these checks.

Normally use the typed API. If an importer edits the raw saved IDProperty
definition, call ``curve_mapping_sync(property_path)`` on its owning ID or
declaring parent to validate and publish it. Until then, access to an existing
runtime record rejects the unsynchronized data. Invalid raw data is left intact.
An unchanged sync does not notify the owner or invoke a callback.

Caching
=======

An owned mapping's ``curve_mapping_cache_key()`` returns exact Python integers
``(runtime_record_identity, definition_revision)``. Call this method before
reusing cached samples so invalid data cannot authorize a cached result::

   cache_key = curve.curve_mapping_cache_key()
   samples = cache.get(cache_key)
   if samples is None:
       channel = curve.curves[0]
       samples = tuple(curve.evaluate(channel, i / 255.0) for i in range(256))
       cache[cache_key] = samples

The identity survives wrapper garbage collection. New records after copying,
unset/reinitialization, undo or loading have new identities. Re-registering a
declaration may reuse a record and its key while invalidating previous wrappers.
Committed definition changes advance the revision, including opaque extension
changes published by raw sync. A revision change does not necessarily mean
evaluated output changed. No-ops and point selection preserve the key.

Keys are process-local: do not serialize them. Use integer tuples, not RNA
wrappers, as durable cache keys. Old tuples remain usable for cache eviction
after owner deletion, but never authorize reuse after a current access error.
The method rejects ordinary native CurveMappings and nonmapping RNA structs;
native properties remain authoritative and need their own cache strategy.

Transactional UI
================

``layout.template_owned_curve_mapping(brush, "response.curve")`` can draw a
Create button without allocating absent PropertyGroups or curves. Existing
``layout.template_curve_mapping(parent, "curve")`` also detects owned scalar
properties. Unsupported color/vector/tone/level options are rejected.

Create initializes explicitly and is undoable. Edit opens a dialog with an
independent working mapping, native graph controls, scalar presets, point
coordinates/handles, clipping and extension controls. Apply commits one
definition and creates one undo step. Cancel discards the working mapping;
Escape while dragging cancels the dialog. An unchanged Apply creates no undo
step. Invalid clipping, deleted owners or a conflicting committed revision
reject Apply without changing saved data. An existing edited collection item
can move without retargeting the dialog. Stale Create buttons cannot initialize
a different item after a collection mutation.

Declaration lifetime identity
-----------------------------

``bpy.props.curve_mapping_declaration_key(type, identifier)`` returns a positive,
process-local integer for a directly declared owned CurveMapping property. Test
for this function explicitly: initial API version 1 builds predate it. The call
requires the main thread, an RNA Python type and an exact nonempty string without
NUL. Missing, inherited-only and non-owned properties raise ``ValueError``.
Runtime bookkeeping may be allocated, but no owner data is read or allocated.

The key remains stable while the declaration exists, including across owner
copy/load/undo. Removing and re-registering even the identical deferred property
produces a new key. It cannot be replaced by a PropertyRNA address or the mapping
cache key, both of which can survive or be reused across declaration changes.
Never serialize this key; combine it with owner and mapping lifetime checks.
