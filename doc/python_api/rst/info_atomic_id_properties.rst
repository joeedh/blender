Atomic scalar IDProperty updates
================================

The fork provides ``ID.id_properties_update_atomic(root, operations)`` for
transactional scalar authoring in a named **custom-property** group. Detect the
capability with ``hasattr(bpy.types.ID, "id_properties_update_atomic")``.
It does not address the separate system-property namespace used by registered
PointerProperty and CurveMappingProperty declarations.

Each operation is an exact tuple in an exact tuple/list::

   changed = brush.id_properties_update_atomic("my_authoring", (
       ('SET', ('schema_version',), 1, None),
       ('SET', ('settings', 'strength'), 0.375,
        {'min': 0.0, 'max': 1.0, 'default': 0.5, 'description': "Strength"}),
       ('DELETE', ('obsolete_scalar',)),
   ))

Only exact builtin bool, signed int32, finite Python float (stored as double),
and UTF-8 str values are accepted. Strings are limited to one MiB and cannot
contain NUL. Paths are tuples of 1..32 nonempty strings, each at most 63 UTF-8
bytes without NUL. The root follows the same key rule. Transactions accept at
most 1024 operations; duplicate and ancestor-overlapping paths reject.

SET creates missing intermediate groups in staging. DELETE on an absent path
does nothing. Existing non-group ancestors reject. Neither operation can replace
or remove a group, array or ID reference. Static property types cannot change.
Unknown siblings, flags, types, UI metadata and ID-reference counts are preserved
by native copying, rather than by conversion through Python dictionaries.
Existing roots beyond 64 levels or 100000 properties, unsupported future types,
and legacy arrays of groups reject without changing the owner.

``ui`` is None (preserve same-type metadata) or an exact builtin-only dictionary
using the existing scalar UI keywords: min/max, soft_min/soft_max, step, precision,
default, description and subtype. Applicability depends on scalar type. Enum
items cannot be changed through this API; existing enum choices are preserved.
Metadata is normalized using Blender's existing UI helpers while detached.
Overflowing float-backed step values reject. Changing a scalar's type discards
that leaf's old UI metadata. Invalid later operations or UI data publish nothing.

The return value is True if stored state changed. A normalized no-op leaves the
owner's root presence, storage and dirty state unchanged. No-op validation still
rejects read-only owners and forbidden contexts. Temporary staging may allocate.

Writes require a writable main-thread Python context and a current editable Main
ID that is neither evaluated nor a library override. Editable linked brush assets
are supported. The post-publication notification phase tags dependency updates,
window/ID refresh, actual Brush unsaved state, Scene tool settings and node-tree
shading as applicable. It does not invoke arbitrary registered RNA update
callbacks, change ID-reference topology, or create an undo step.

Successful publication replaces the metadata root. Discard and reacquire raw
IDPropertyGroup, generic PropertyGroup and UI-manager wrappers after every commit;
do not retain them in a cache or widget. Resolve the parent group immediately
before drawing a property::

   group = brush.path_resolve('["my_authoring"]["settings"]', False)
   layout.prop(group, '["strength"]')

Owned CurveMapping declarations use the separate system-property namespace and
retain their runtime identities during custom metadata updates. Keep custom-curve
declarations separate from this metadata root. Brush IDs opt out of global
memfile undo, and native Scene ToolSettings are preserved across undo; this API
does not change those policies. Generic Scene custom-property edits can use
Blender's ordinary explicit undo conventions.

Nonmutating system scalar inspection
-----------------------------------

``ID.system_property_scalar(path_tuple)`` reads raw declared-property backing
storage without RNA validation, repair, declaration access or owner allocation.
It returns a copied ``(present, type_name, value)`` triple. Missing or ghost paths
return ``(False, None, None)``; present malformed ancestors or unsupported leaves
raise without changing data. Supported types are INT, FLOAT, DOUBLE, BOOLEAN and
UTF-8 STRING (at most one MiB). Groups, arrays, byte strings and ID pointers are
not returned. The exact tuple contains 1..32 exact string keys, each 1..63 UTF-8
bytes without NUL. It requires the main thread and an original current-Main ID;
linked/read-only IDs are supported. It never sends update notifications.
