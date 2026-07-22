# claudeMemory — Blender fork (custom-object-modes)

Working notes for the custom object mode core API on this branch. See
[../CLAUDE.md](../CLAUDE.md) for the overview.

Scope: **only** the engine-agnostic mode API (registration, lifecycle, undo,
external draw, multires reshape). Sculpt-engine and addon notes live in the
`sculptcore-blender-addon` repo's own `claudeMemory`, not here.

## Structure

- `codebase/` — validated reference docs for the API surface.
- `plans/` — implementation/refactor plans for the branch.
- `research/` — investigation notes.

## Index

- [codebase/custom-mode-api.md](codebase/custom-mode-api.md) — the files, types,
  and callbacks that make up the custom object mode API, and how a mode is
  registered from Python.
