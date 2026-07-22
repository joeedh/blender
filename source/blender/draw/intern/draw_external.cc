/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 */

#include "draw_external.hh"

#include "BLI_map.hh"
#include "BLI_math_geom_c.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_set.hh"

#include "DNA_mesh_types.h"
#include "DNA_object_types.h"

#include "BKE_attribute.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_object.hh"
#include "BKE_object_draw_provider.hh"
#include "BKE_object_types.hh"

#include "DEG_depsgraph_query.hh"

#include "GPU_batch.hh"
#include "GPU_vertex_buffer.hh"
#include "GPU_vertex_format.hh"

#include "attribute_convert.hh"
#include "draw_attributes.hh"
#include "draw_context_private.hh"
#include "draw_view.hh"

namespace blender::draw {

/* -------------------------------------------------------------------- */
/** \name Vertex Formats
 * \{ */

static const GPUVertFormat &position_format()
{
  static const GPUVertFormat format = GPU_vertformat_from_attribute(
      "pos", gpu::VertAttrType::SFLOAT_32_32_32);
  return format;
}

static const GPUVertFormat &normal_format()
{
  static const GPUVertFormat format = GPU_vertformat_from_attribute(
      "nor", gpu::VertAttrType::SNORM_16_16_16_16);
  return format;
}

static short4 normal_float_to_short(const float3 &value)
{
  short3 result;
  normal_float_to_short_v3(result, value);
  return short4(result.x, result.y, result.z, 0);
}

/* When the object's active color attribute is a point float color (what the
 * engine's provider exposes as its float4 color stream), build a matching
 * vertex format with the shader aliases so the color binds. Returns false when
 * there is no such attribute, i.e. no color to draw. */
static bool color_vertex_format(const Object *ob, GPUVertFormat &r_format)
{
  const Mesh *mesh = BKE_object_get_original_mesh(ob);
  if (mesh == nullptr || mesh->active_color_attribute == nullptr) {
    return false;
  }
  const StringRef name = mesh->active_color_attribute;
  const bke::AttributeAccessor attributes = mesh->attributes();
  const std::optional<bke::AttributeMetaData> meta = attributes.lookup_meta_data(name);
  if (!meta || meta->data_type != bke::AttrType::ColorFloat ||
      meta->domain != bke::AttrDomain::Point)
  {
    return false;
  }
  r_format = init_format_for_attribute(bke::AttrType::ColorFloat, "data");
  const bool is_active = true;
  const bool is_render = mesh->default_color_attribute && name == mesh->default_color_attribute;
  DRW_cdlayer_attr_aliases_add(
      &r_format, "c", bke::AttrType::ColorFloat, name, is_render, is_active);
  return true;
}

/* Build a Float2 UV vertex format aliased to the object's active UV map, so
 * texture shading binds it. Returns false when there is no UV map. */
static bool uv_vertex_format(const Object *ob, GPUVertFormat &r_format)
{
  const Mesh *mesh = BKE_object_get_original_mesh(ob);
  if (mesh == nullptr) {
    return false;
  }
  const StringRef name = mesh->active_uv_map_name();
  if (name.is_empty()) {
    return false;
  }
  r_format = init_format_for_attribute(bke::AttrType::Float2, "data");
  const bool is_active = true;
  const bool is_render = name == mesh->default_uv_map_name();
  DRW_cdlayer_attr_aliases_add(&r_format, "u", bke::AttrType::Float2, name, is_render, is_active);
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Per-Object GPU Cache
 * \{ */

/* Per-drawable-node GPU buffers + batch, plus the vertex count they were built
 * for so the provider's TOPOLOGY flag can be corroborated. */
struct NodeCache {
  gpu::VertBufPtr pos;
  gpu::VertBufPtr nor;
  gpu::VertBufPtr col;
  gpu::VertBufPtr uv;
  gpu::Batch *batch = nullptr;
  int verts_num = 0;
  bool has_color = false;
  bool has_uv = false;

  ~NodeCache()
  {
    if (batch) {
      GPU_batch_discard(batch);
    }
  }
  NodeCache() = default;
  /* `batch` is a raw owning pointer, so a move must transfer it and null the
   * source — otherwise the moved-from destructor discards a batch the moved-to
   * copy (and its cached VBOs) still references (use-after-free at draw). */
  NodeCache(NodeCache &&other) noexcept
      : pos(std::move(other.pos)),
        nor(std::move(other.nor)),
        col(std::move(other.col)),
        uv(std::move(other.uv)),
        batch(other.batch),
        verts_num(other.verts_num),
        has_color(other.has_color),
        has_uv(other.has_uv)
  {
    other.batch = nullptr;
  }
  NodeCache &operator=(NodeCache &&other) noexcept
  {
    if (this != &other) {
      if (batch) {
        GPU_batch_discard(batch);
      }
      pos = std::move(other.pos);
      nor = std::move(other.nor);
      col = std::move(other.col);
      uv = std::move(other.uv);
      batch = other.batch;
      verts_num = other.verts_num;
      has_color = other.has_color;
      has_uv = other.has_uv;
      other.batch = nullptr;
    }
    return *this;
  }
  NodeCache(const NodeCache &) = delete;
  NodeCache &operator=(const NodeCache &) = delete;
};

/* Keyed by the provider's stable node id (never by list position: the node
 * list reorders when the provider's spatial structure rebalances, and a
 * positionally-mismatched cache entry with a coincidentally equal vertex count
 * would silently draw another node's stale geometry). */
struct ObjectCache {
  Map<uint32_t, NodeCache> nodes;
};

/* Keyed by original object pointer (the draw object is a depsgraph copy). Freed
 * on mode exit / provider unregister (external_draw_cache_free) and GPU
 * teardown (external_draw_cache_free_all). */
static Map<const Object *, ObjectCache> &object_caches()
{
  static Map<const Object *, ObjectCache> caches;
  return caches;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Node Upload
 * \{ */

/* Upload one node's positions + normals (+ color when requested) into `cache`,
 * (re)allocating the VBOs and batch when the vertex count or attribute set
 * changed or nothing was cached yet. */
static void node_upload(NodeCache &cache,
                        const ExternalDrawNode &node,
                        const bool want_color,
                        const GPUVertFormat *color_format,
                        const bool want_uv,
                        const GPUVertFormat *uv_format)
{
  /* The provider exposes attrs in a fixed slot order: color@0, uv@1. */
  const bool have_color_src = want_color && node.attrs != nullptr && node.attrs[0] != nullptr;
  const bool have_uv_src = want_uv && node.attrs != nullptr && node.attrs[1] != nullptr;
  const bool realloc = cache.batch == nullptr || cache.verts_num != node.verts_num ||
                       cache.has_color != have_color_src || cache.has_uv != have_uv_src ||
                       (node.update_flags & EXTERNAL_DRAW_UPDATE_TOPOLOGY) != 0;
  const bool upload = realloc || (node.update_flags & EXTERNAL_DRAW_UPDATE_DATA) != 0;
  if (!upload) {
    return;
  }

  if (realloc) {
    if (cache.batch) {
      GPU_batch_discard(cache.batch);
      cache.batch = nullptr;
    }
    /* Dynamic: the CPU-side data is kept so a stroke can re-upload positions
     * each frame (static usage frees it after the first GPU upload). */
    cache.pos = gpu::VertBufPtr(
        GPU_vertbuf_create_with_format_ex(position_format(), GPU_USAGE_DYNAMIC));
    cache.nor = gpu::VertBufPtr(
        GPU_vertbuf_create_with_format_ex(normal_format(), GPU_USAGE_DYNAMIC));
    GPU_vertbuf_data_alloc(*cache.pos, node.verts_num);
    GPU_vertbuf_data_alloc(*cache.nor, node.verts_num);
    if (have_color_src) {
      cache.col = gpu::VertBufPtr(
          GPU_vertbuf_create_with_format_ex(*color_format, GPU_USAGE_DYNAMIC));
      GPU_vertbuf_data_alloc(*cache.col, node.verts_num);
    }
    else {
      cache.col.reset();
    }
    if (have_uv_src) {
      cache.uv = gpu::VertBufPtr(GPU_vertbuf_create_with_format_ex(*uv_format, GPU_USAGE_DYNAMIC));
      GPU_vertbuf_data_alloc(*cache.uv, node.verts_num);
    }
    else {
      cache.uv.reset();
    }
    cache.verts_num = node.verts_num;
    cache.has_color = have_color_src;
    cache.has_uv = have_uv_src;
  }

  MutableSpan<float3> positions = cache.pos->data<float3>();
  positions.copy_from(
      Span<float3>(reinterpret_cast<const float3 *>(node.positions), node.verts_num));

  MutableSpan<short4> normals = cache.nor->data<short4>();
  if (node.normals != nullptr) {
    const Span<float3> src(reinterpret_cast<const float3 *>(node.normals), node.verts_num);
    for (const int i : IndexRange(node.verts_num)) {
      normals[i] = normal_float_to_short(src[i]);
    }
  }
  else {
    /* Flat: one geometric normal per triangle (soup order, every 3 verts). */
    for (int tri = 0; tri * 3 < node.verts_num; tri++) {
      const float3 &a = positions[tri * 3 + 0];
      const float3 &b = positions[tri * 3 + 1];
      const float3 &c = positions[tri * 3 + 2];
      const short4 packed = normal_float_to_short(math::normalize(math::cross(b - a, c - a)));
      normals[tri * 3 + 0] = packed;
      normals[tri * 3 + 1] = packed;
      normals[tri * 3 + 2] = packed;
    }
  }

  if (have_color_src) {
    /* Engine color stream (float4, slot 0). */
    MutableSpan<float4> colors = cache.col->data<float4>();
    colors.copy_from(Span<float4>(static_cast<const float4 *>(node.attrs[0]), node.verts_num));
  }
  if (have_uv_src) {
    /* Engine UV stream (float2, slot 1). */
    MutableSpan<float2> uvs = cache.uv->data<float2>();
    uvs.copy_from(Span<float2>(static_cast<const float2 *>(node.attrs[1]), node.verts_num));
  }

  /* Flag the refilled buffers and force the upload now (like #draw_pbvh's
   * node updates): the GL backend only processes the dirty flag on a bind,
   * and a batch's cached VAO never rebinds its vertbufs — without the
   * explicit use the viewport keeps drawing the stale upload. */
  GPU_vertbuf_tag_dirty(cache.pos.get());
  GPU_vertbuf_use(cache.pos.get());
  GPU_vertbuf_tag_dirty(cache.nor.get());
  GPU_vertbuf_use(cache.nor.get());
  if (cache.col) {
    GPU_vertbuf_tag_dirty(cache.col.get());
    GPU_vertbuf_use(cache.col.get());
  }
  if (cache.uv) {
    GPU_vertbuf_tag_dirty(cache.uv.get());
    GPU_vertbuf_use(cache.uv.get());
  }

  if (realloc) {
    cache.batch = GPU_batch_create(GPU_PRIM_TRIS, nullptr, nullptr);
    GPU_batch_vertbuf_add(cache.batch, cache.pos.get(), false);
    GPU_batch_vertbuf_add(cache.batch, cache.nor.get(), false);
    if (cache.col) {
      GPU_batch_vertbuf_add(cache.batch, cache.col.get(), false);
    }
    if (cache.uv) {
      GPU_batch_vertbuf_add(cache.batch, cache.uv.get(), false);
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Public API
 * \{ */

Vector<SculptBatch> external_batches_get(const Object *ob, SculptBatchFeature /*features*/)
{
  const ExternalDrawProvider *provider = BKE_object_external_draw_provider_get(ob);
  if (provider == nullptr) {
    return {};
  }

  /* Positions + normals, plus color and/or UV from the provider's fixed
   * color@0 / uv@1 attribute slots. The attribute set is derived from what the
   * object *has*, never from the caller's `features`: several passes (workbench,
   * overlay outline, EEVEE per-material) request this object in the same frame
   * with different feature flags, and each returned batch shares one per-node
   * cache. Rebuilding the cache for a narrower feature set would free vertex
   * buffers an already-returned batch still references (a use-after-free). A
   * pass that does not need a stream simply leaves it unbound. */
  GPUVertFormat color_format = {};
  GPUVertFormat uv_format = {};
  const bool want_color = color_vertex_format(ob, color_format);
  const bool want_uv = uv_vertex_format(ob, uv_format);
  const char *attr_names[2] = {"color", "uv"};
  const int attrs_num = want_uv ? 2 : (want_color ? 1 : 0);
  const ExternalDrawAttrRequest request = {attrs_num, attrs_num ? attr_names : nullptr};

  const Object *ob_orig = DEG_get_original(ob);
  const unsigned int object_key = ob_orig->id.session_uid;
  ExternalDrawNode *nodes = nullptr;
  const int nodes_num = provider->nodes_get(provider->user_data, object_key, &request, &nodes);
  if (nodes_num == 0 || nodes == nullptr) {
    provider->nodes_release(provider->user_data, object_key);
    return {};
  }

  ObjectCache &cache = object_caches().lookup_or_add_default(ob_orig);

  /* Frustum planes in object space (transform by inverse(obmat); the transpose
   * inverse of a plane cancels the obmat inverse), matching draw_sculpt.cc. */
  std::array<float4, 6> planes = View::default_get().frustum_planes_get();
  const float4x4 tmat = math::transpose(ob->object_to_world());
  for (const int i : IndexRange(planes.size())) {
    planes[i] = tmat * planes[i];
  }

  const int max_material = std::max(0, BKE_object_material_count_eval(ob) - 1);

  Vector<SculptBatch> result;
  Set<uint32_t> seen_ids;
  for (const int i : IndexRange(nodes_num)) {
    const ExternalDrawNode &node = nodes[i];
    seen_ids.add(node.node_id);
    NodeCache &node_cache = cache.nodes.lookup_or_add_default(node.node_id);
    node_upload(node_cache, node, want_color, &color_format, want_uv, &uv_format);

    if (node.verts_num == 0) {
      continue;
    }
    /* Frustum cull: skip a node whose AABB is fully outside any plane. */
    bool outside = false;
    for (const float4 &plane : planes) {
      float3 vmin;
      for (int axis = 0; axis < 3; axis++) {
        vmin[axis] = plane[axis] < 0.0f ? node.bounds_min[axis] : node.bounds_max[axis];
      }
      if (math::dot(float3(plane.x, plane.y, plane.z), vmin) + plane.w < 0.0f) {
        outside = true;
        break;
      }
    }
    if (outside) {
      continue;
    }

    SculptBatch batch = {};
    batch.batch = node_cache.batch;
    batch.material_slot = std::clamp(node.material_index, 0, max_material);
    batch.debug_index = result.size();
    result.append(batch);
  }

  /* Drop cache entries for nodes the provider no longer reports (merged away /
   * repartitioned), freeing their GPU buffers. Safe within a frame: every sync
   * of the same settled provider state returns the same id set, so no batch
   * returned by an earlier pass this frame refers to a pruned entry. */
  cache.nodes.remove_if([&](auto item) { return !seen_ids.contains(item.key); });

  provider->nodes_release(provider->user_data, object_key);
  return result;
}

Vector<SculptBatch> external_batches_per_material_get(const Object *ob,
                                                      Span<const GPUMaterial *> /*materials*/)
{
  /* v1: positions + normals only; per-node material_slot already groups the
   * result. Generic per-material attributes land with the attribute stage. */
  return external_batches_get(ob, SCULPT_BATCH_DEFAULT);
}

void external_draw_cache_free(const Object *ob)
{
  if (ob == nullptr) {
    return;
  }
  object_caches().remove(DEG_get_original(ob));
}

void external_draw_cache_free_all()
{
  object_caches().clear();
}

/** \} */

}  // namespace blender::draw
