/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * External draw provider: a generic, engine-agnostic seam that lets an
 * addon-registered object mode (#OB_MODE_CUSTOM) supply the object's viewport
 * geometry as per-node CPU arrays instead of the evaluated #Mesh. It mirrors
 * the role of the sculpt PBVH draw path (`draw_sculpt.cc` /
 * `BKE_sculptsession_use_pbvh_draw`) but without hardwiring a #SculptSession or
 * `bke::pbvh::Tree`, so a full sculpt mode can live in an addon and still get
 * per-node partial redraw.
 *
 * Ownership split (see the draw-integration design):
 * - The provider describes geometry only, as CPU arrays. It never creates GPU
 *   objects — Blender must render through OpenGL, Metal and Vulkan, so all
 *   VBOs/IBOs/batches are built by Blender's backend-agnostic GPU module from
 *   the provider's arrays (one dirty-node staging upload per update).
 * - The provider implementation lives in the addon's native library and is
 *   registered against its #ObjectModeType through a pointer-passing seam; the
 *   structs here are the stable, versioned C ABI of that boundary.
 */

#include "BLI_sys_types.hh"

namespace blender {

struct Object;
struct RegionView3D;
struct ObjectModeType;

/**
 * ABI version of the provider structs below. The provider reports the version
 * it was built against; Blender refuses a mismatch rather than reading a
 * differently-shaped struct. Bump on any layout change.
 */
#define BKE_EXTERNAL_DRAW_ABI_VERSION 2

/** #ExternalDrawNode.update_flags: what changed since Blender last built this
 * node's GPU buffers, so the cache re-uploads/reallocs only what it must. */
enum eExternalDrawUpdate {
  /** Nothing changed; reuse the cached batch as-is. */
  EXTERNAL_DRAW_UPDATE_NONE = 0,
  /** Vertex data changed in place (same `verts_num`); re-upload, keep buffers. */
  EXTERNAL_DRAW_UPDATE_DATA = (1 << 0),
  /** Vertex count / layout changed; reallocate the node's buffers and batch. */
  EXTERNAL_DRAW_UPDATE_TOPOLOGY = (1 << 1),
};

/**
 * One drawable node: a de-indexed triangle soup (the layout Blender's PBVH VBOs
 * also use, so conversion is a per-node memcpy plus normal packing). All
 * pointers are owned by the provider and must stay valid until the matching
 * #ExternalDrawProvider.nodes_release for this sync.
 */
struct ExternalDrawNode {
  /** `verts_num` positions, triangle-soup order (every 3 verts is a triangle). */
  const float (*positions)[3];
  /** `verts_num` per-vertex normals, same order (null → flat/derived). */
  const float (*normals)[3];
  /**
   * One CPU array per requested attribute, in #ExternalDrawAttrRequest order.
   * Each points at `verts_num` elements of that attribute's type. Null when the
   * node does not carry the attribute. Null overall when none were requested.
   */
  const void **attrs;
  /** Vertex count (a multiple of 3). */
  int verts_num;
  /** Material slot for this node (first face's material, clamped by Blender). */
  int material_index;
  /** #eExternalDrawUpdate bitmask since the last sync. */
  uint32_t update_flags;
  /**
   * Provider-stable node identity. The node list's order and composition may
   * change between syncs (the provider's spatial structure rebalances), so
   * Blender keys its per-node GPU caches on this, never on list position.
   * Unique within one sync's node list.
   */
  uint32_t node_id;
  /** Object-space AABB, for frustum culling before upload. */
  float bounds_min[3];
  float bounds_max[3];
};

/** The attribute set the engine needs this redraw (the analogue of the PBVH
 * `ViewportRequest`); forwarded to the provider so it can fill the matching
 * `ExternalDrawNode.attrs` slots. */
struct ExternalDrawAttrRequest {
  /** Number of requested attributes (0 → positions + normals only). */
  int attrs_num;
  /** `attrs_num` attribute names, defining the `ExternalDrawNode.attrs` order. */
  const char *const *attr_names;
};

/**
 * The provider a mode registers to describe its objects' geometry. All calls
 * happen on the main thread during draw sync (after the engine has finished any
 * geometry update for the frame), so the callbacks only read a settled state.
 */
struct ExternalDrawProvider {
  /** Must equal #BKE_EXTERNAL_DRAW_ABI_VERSION. */
  int abi_version;
  /**
   * Fill the node list for the object identified by `object_key` (the original
   * object's #ID.session_uid — a stable per-session key the provider maps to
   * its own per-object state; the provider is native and cannot dereference a
   * Blender #Object). Returns the node count and sets `*r_nodes` to a
   * provider-owned array valid until #nodes_release. Return 0 (and leave
   * `*r_nodes` untouched) when there is nothing to draw.
   */
  int (*nodes_get)(void *user_data,
                   unsigned int object_key,
                   const ExternalDrawAttrRequest *req,
                   ExternalDrawNode **r_nodes);
  /** Release whatever #nodes_get returned for this sync. */
  void (*nodes_release)(void *user_data, unsigned int object_key);
  /** Opaque provider state (e.g. the addon's session registry). */
  void *user_data;
};

/**
 * Register `provider` against the mode `mt`. The pointer is borrowed (owned by
 * the caller / addon native lib) and must outlive the registration. Rejects a
 * null provider or an #ExternalDrawProvider.abi_version mismatch.
 */
bool BKE_object_mode_draw_provider_set(ObjectModeType *mt, const ExternalDrawProvider *provider);

/** The draw provider of `ob`'s active custom mode, or null when the object is
 * not in a custom mode / the mode registered none. */
const ExternalDrawProvider *BKE_object_external_draw_provider_get(const Object *ob);

/**
 * Whether `ob` should be drawn from its mode's external draw provider instead
 * of the evaluated mesh this redraw. False for external render engines (e.g.
 * Cycles viewport, which renders from evaluated geometry) — mirrors
 * #BKE_sculptsession_use_pbvh_draw's `external_engine` handling.
 */
bool BKE_object_use_external_draw(const Object *ob, const RegionView3D *rv3d);

}  // namespace blender
