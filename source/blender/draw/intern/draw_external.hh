/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 *
 * Viewport draw for objects that supply geometry through an external draw
 * provider (#BKE_object_draw_provider.hh) rather than the evaluated mesh — the
 * generic analogue of `draw_sculpt.hh` for addon-registered custom modes.
 *
 * The provider hands Blender per-node CPU triangle-soup arrays; this layer owns
 * the Blender-side GPU cache (one VBO per node per attribute, per-node batches,
 * dirty-flag driven upload) and returns #SculptBatch-shaped results so the
 * engines consume it exactly like the sculpt PBVH path.
 */

#pragma once

#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "draw_sculpt.hh"

namespace blender {
struct Object;
struct GPUMaterial;
namespace gpu {
class Batch;
}
}  // namespace blender

namespace blender::draw {

/**
 * Per-visible-node batches for `ob`'s external draw provider, frustum-culled in
 * object space. Empty when the object has no provider or nothing to draw. Used
 * by engines that don't use GPUMaterials (Workbench, overlays); `features`
 * selects the attribute set, mirroring #sculpt_batches_get.
 */
Vector<SculptBatch> external_batches_get(const Object *ob, SculptBatchFeature features);

/** Per-material variant for EEVEE. v1 draws positions + normals only (one
 * material slot); the `materials` span is accepted for parity and future
 * per-material attribute requests. */
Vector<SculptBatch> external_batches_per_material_get(const Object *ob,
                                                      Span<const GPUMaterial *> materials);

/** Free the cached GPU buffers/batches for `ob` (mode exit, provider
 * unregister, or object removal). Safe to call when nothing is cached. */
void external_draw_cache_free(const Object *ob);

/** Free every cached external-draw entry (GPU/draw-manager teardown). */
void external_draw_cache_free_all();

}  // namespace blender::draw
