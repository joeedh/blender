/* SPDX-FileCopyrightText: 2020 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "MEM_guardedalloc.h"

#include "DNA_meshdata_types.h"

#include "multires_reshape.hh"

#include "BLI_math_base_c.hh"
#include "BLI_math_vector_c.hh"

#include "BKE_ccg.hh"
#include "BKE_subdiv_foreach.hh"
#include "BKE_subdiv_mesh.hh"

namespace blender {

struct MultiresReshapeAssignVertcosContext {
  const MultiresReshapeContext *reshape_context;

  Span<float3> positions;
};

/**
 * Set single displacement grid value at a reshape level to a corresponding vertex coordinate.
 * This function will be called for every side of a boundary grid points for inner coordinates.
 */
static void multires_reshape_vertcos_foreach_single_vert(
    const bke::subdiv::ForeachContext *foreach_context,
    const GridCoord *grid_coord,
    const int subdiv_vert_index)
{
  MultiresReshapeAssignVertcosContext *reshape_vertcos_context =
      static_cast<MultiresReshapeAssignVertcosContext *>(foreach_context->user_data);
  const float3 &coordinate = reshape_vertcos_context->positions[subdiv_vert_index];

  ReshapeGridElement grid_element = multires_reshape_grid_element_for_grid_coord(
      reshape_vertcos_context->reshape_context, grid_coord);
  BLI_assert(grid_element.displacement != nullptr);
  *grid_element.displacement = coordinate;
}

/* TODO(sergey): De-duplicate with similar function in multires_reshape_smooth.cc */
static void multires_reshape_vertcos_foreach_vert(
    const bke::subdiv::ForeachContext *foreach_context,
    const PTexCoord *ptex_coord,
    const int subdiv_vert_index)
{
  const MultiresReshapeAssignVertcosContext *reshape_vertcos_context =
      static_cast<MultiresReshapeAssignVertcosContext *>(foreach_context->user_data);
  const MultiresReshapeContext *reshape_context = reshape_vertcos_context->reshape_context;

  const GridCoord grid_coord = multires_reshape_ptex_coord_to_grid(reshape_context, ptex_coord);
  const int face_index = multires_reshape_grid_to_face_index(reshape_context,
                                                             grid_coord.grid_index);

  const int num_corners = reshape_context->base_faces[face_index].size();
  const int start_grid_index = reshape_context->base_faces[face_index].start();
  const int corner = grid_coord.grid_index - start_grid_index;

  if (grid_coord.u == 0.0f && grid_coord.v == 0.0f) {
    for (int current_corner = 0; current_corner < num_corners; ++current_corner) {
      GridCoord corner_grid_coord = grid_coord;
      corner_grid_coord.grid_index = start_grid_index + current_corner;
      multires_reshape_vertcos_foreach_single_vert(
          foreach_context, &corner_grid_coord, subdiv_vert_index);
    }
    return;
  }

  multires_reshape_vertcos_foreach_single_vert(foreach_context, &grid_coord, subdiv_vert_index);

  if (grid_coord.u == 0.0f) {
    GridCoord prev_grid_coord;
    prev_grid_coord.grid_index = start_grid_index + ((corner + num_corners - 1) % num_corners);
    prev_grid_coord.u = grid_coord.v;
    prev_grid_coord.v = 0.0f;

    multires_reshape_vertcos_foreach_single_vert(
        foreach_context, &prev_grid_coord, subdiv_vert_index);
  }

  if (grid_coord.v == 0.0f) {
    GridCoord next_grid_coord;
    next_grid_coord.grid_index = start_grid_index + ((corner + 1) % num_corners);
    next_grid_coord.u = 0.0f;
    next_grid_coord.v = grid_coord.u;

    multires_reshape_vertcos_foreach_single_vert(
        foreach_context, &next_grid_coord, subdiv_vert_index);
  }
}

/* bke::subdiv::ForeachContext::topology_info() */
static bool multires_reshape_vertcos_foreach_topology_info(
    const bke::subdiv::ForeachContext *foreach_context,
    const int num_vertices,
    const int /*num_edges*/,
    const int /*num_loops*/,
    const int /*num_faces*/,
    const Span<int> /*subdiv_face_offset*/)
{
  MultiresReshapeAssignVertcosContext *reshape_vertcos_context =
      static_cast<MultiresReshapeAssignVertcosContext *>(foreach_context->user_data);
  if (num_vertices != reshape_vertcos_context->positions.size()) {
    return false;
  }
  return true;
}

/* bke::subdiv::ForeachContext::vert_inner() */
static void multires_reshape_vertcos_foreach_vert_inner(
    const bke::subdiv::ForeachContext *foreach_context,
    void * /*tls_v*/,
    const int ptex_face_index,
    const float ptex_face_u,
    const float ptex_face_v,
    const int /*coarse_face_index*/,
    const int /*coarse_face_corner*/,
    const int subdiv_vert_index)
{
  PTexCoord ptex_coord{};
  ptex_coord.ptex_face_index = ptex_face_index;
  ptex_coord.u = ptex_face_u;
  ptex_coord.v = ptex_face_v;
  multires_reshape_vertcos_foreach_vert(foreach_context, &ptex_coord, subdiv_vert_index);
}

/* bke::subdiv::ForeachContext::vert_every_corner() */
static void multires_reshape_vertcos_foreach_vert_every_corner(
    const bke::subdiv::ForeachContext *foreach_context,
    void * /*tls_v*/,
    const int ptex_face_index,
    const float ptex_face_u,
    const float ptex_face_v,
    const int /*coarse_vert_index*/,
    const int /*coarse_face_index*/,
    const int /*coarse_face_corner*/,
    const int subdiv_vert_index)
{
  PTexCoord ptex_coord{};
  ptex_coord.ptex_face_index = ptex_face_index;
  ptex_coord.u = ptex_face_u;
  ptex_coord.v = ptex_face_v;
  multires_reshape_vertcos_foreach_vert(foreach_context, &ptex_coord, subdiv_vert_index);
}

/* bke::subdiv::ForeachContext::vert_every_edge() */
static void multires_reshape_vertcos_foreach_vert_every_edge(
    const bke::subdiv::ForeachContext *foreach_context,
    void * /*tls_v*/,
    const int ptex_face_index,
    const float ptex_face_u,
    const float ptex_face_v,
    const int /*coarse_edge_index*/,
    const int /*coarse_face_index*/,
    const int /*coarse_face_corner*/,
    const int subdiv_vert_index)
{
  PTexCoord ptex_coord{};
  ptex_coord.ptex_face_index = ptex_face_index;
  ptex_coord.u = ptex_face_u;
  ptex_coord.v = ptex_face_v;
  multires_reshape_vertcos_foreach_vert(foreach_context, &ptex_coord, subdiv_vert_index);
}

bool multires_reshape_assign_final_coords_from_vertcos(
    const MultiresReshapeContext *reshape_context, const Span<float3> positions)
{
  MultiresReshapeAssignVertcosContext reshape_vertcos_context{};
  reshape_vertcos_context.reshape_context = reshape_context;
  reshape_vertcos_context.positions = positions;

  bke::subdiv::ForeachContext foreach_context{};
  foreach_context.topology_info = multires_reshape_vertcos_foreach_topology_info;
  foreach_context.vert_inner = multires_reshape_vertcos_foreach_vert_inner;
  foreach_context.vert_every_edge = multires_reshape_vertcos_foreach_vert_every_edge;
  foreach_context.vert_every_corner = multires_reshape_vertcos_foreach_vert_every_corner;
  foreach_context.user_data = &reshape_vertcos_context;

  bke::subdiv::ToMeshSettings mesh_settings;
  mesh_settings.resolution = (1 << reshape_context->reshape.level) + 1;
  mesh_settings.use_optimal_display = false;

  return bke::subdiv::foreach_subdiv_geometry(
      reshape_context->subdiv, &foreach_context, &mesh_settings, reshape_context->base_mesh);
}

/* -------------------------------------------------------------------- */
/** \name Paint mask <-> subdivided-vertex values
 *
 * The mask twin of the vertcos assign above: the same subdivision foreach
 * maps every subdivided vertex to its grid element(s) (boundary vertices to
 * every replica), but the value exchanged is the grid paint mask. Masks are
 * absolute scalars, so unlike positions there is no smooth/tangent
 * conversion — a direct assignment at the top level is the whole transfer.
 * \{ */

struct MultiresReshapeMaskContext {
  const MultiresReshapeContext *reshape_context;

  /** Write: subdivided-vertex values assigned into the grids. */
  Span<float> values_in;
  /** Read: grid values gathered per subdivided vertex, allocated by the
   * walk's topology callback (only there is the vertex count known). */
  float *values_out = nullptr;
  int values_out_num = 0;
};

/**
 * Sample a paint-mask grid at a normalized grid coordinate, bilinearly over
 * the grid's own stored level (which may be coarser than the reshape level —
 * unlike the write path, reading must not resize/clear existing mask data).
 */
static float multires_reshape_mask_sample(const GridPaintMask &gpm, const GridCoord &grid_coord)
{
  const int grid_size = CCG_grid_size(gpm.level);
  const float x = grid_coord.u * float(grid_size - 1);
  const float y = grid_coord.v * float(grid_size - 1);
  const int x0 = min_ii(int(x), grid_size - 2);
  const int y0 = min_ii(int(y), grid_size - 2);
  const float tx = x - float(x0);
  const float ty = y - float(y0);
  const float *data = gpm.data;
  const float v00 = data[y0 * grid_size + x0];
  const float v10 = data[y0 * grid_size + x0 + 1];
  const float v01 = data[(y0 + 1) * grid_size + x0];
  const float v11 = data[(y0 + 1) * grid_size + x0 + 1];
  return (v00 * (1.0f - tx) + v10 * tx) * (1.0f - ty) + (v01 * (1.0f - tx) + v11 * tx) * ty;
}

static void multires_reshape_mask_single_vert(const bke::subdiv::ForeachContext *foreach_context,
                                              const GridCoord *grid_coord,
                                              const int subdiv_vert_index)
{
  MultiresReshapeMaskContext *mask_context = static_cast<MultiresReshapeMaskContext *>(
      foreach_context->user_data);
  const MultiresReshapeContext *reshape_context = mask_context->reshape_context;

  if (reshape_context->grid_paint_masks == nullptr) {
    return;
  }
  if (!mask_context->values_in.is_empty()) {
    /* Write: the caller ensured the mask grids at the reshape level, so the
     * direct grid-element access is in range. */
    ReshapeGridElement grid_element = multires_reshape_grid_element_for_grid_coord(reshape_context,
                                                                                   grid_coord);
    if (grid_element.mask != nullptr) {
      *grid_element.mask = mask_context->values_in[subdiv_vert_index];
    }
  }
  else {
    /* Read: sample at the grid's own level. Boundary replicas hold equal
     * values on seam-consistent grids; any visit may win. */
    const GridPaintMask &gpm = reshape_context->grid_paint_masks[grid_coord->grid_index];
    if (gpm.data != nullptr && gpm.level > 0) {
      mask_context->values_out[subdiv_vert_index] = multires_reshape_mask_sample(gpm, *grid_coord);
    }
  }
}

static void multires_reshape_mask_foreach_vert(const bke::subdiv::ForeachContext *foreach_context,
                                               const PTexCoord *ptex_coord,
                                               const int subdiv_vert_index)
{
  const MultiresReshapeMaskContext *mask_context = static_cast<MultiresReshapeMaskContext *>(
      foreach_context->user_data);
  const MultiresReshapeContext *reshape_context = mask_context->reshape_context;

  const GridCoord grid_coord = multires_reshape_ptex_coord_to_grid(reshape_context, ptex_coord);
  const int face_index = multires_reshape_grid_to_face_index(reshape_context,
                                                             grid_coord.grid_index);

  const int num_corners = reshape_context->base_faces[face_index].size();
  const int start_grid_index = reshape_context->face_start_grid_index[face_index];
  const int corner = grid_coord.grid_index - start_grid_index;

  if (grid_coord.u == 0.0f && grid_coord.v == 0.0f) {
    for (int current_corner = 0; current_corner < num_corners; ++current_corner) {
      GridCoord corner_grid_coord = grid_coord;
      corner_grid_coord.grid_index = start_grid_index + current_corner;
      multires_reshape_mask_single_vert(foreach_context, &corner_grid_coord, subdiv_vert_index);
    }
    return;
  }

  multires_reshape_mask_single_vert(foreach_context, &grid_coord, subdiv_vert_index);

  if (grid_coord.u == 0.0f) {
    GridCoord prev_grid_coord;
    prev_grid_coord.grid_index = start_grid_index + ((corner + num_corners - 1) % num_corners);
    prev_grid_coord.u = grid_coord.v;
    prev_grid_coord.v = 0.0f;
    multires_reshape_mask_single_vert(foreach_context, &prev_grid_coord, subdiv_vert_index);
  }

  if (grid_coord.v == 0.0f) {
    GridCoord next_grid_coord;
    next_grid_coord.grid_index = start_grid_index + ((corner + 1) % num_corners);
    next_grid_coord.u = 0.0f;
    next_grid_coord.v = grid_coord.u;
    multires_reshape_mask_single_vert(foreach_context, &next_grid_coord, subdiv_vert_index);
  }
}

static bool multires_reshape_mask_foreach_topology_info(
    const bke::subdiv::ForeachContext *foreach_context,
    const int num_vertices,
    const int /*num_edges*/,
    const int /*num_loops*/,
    const int /*num_faces*/,
    const Span<int> /*subdiv_face_offset*/)
{
  MultiresReshapeMaskContext *mask_context = static_cast<MultiresReshapeMaskContext *>(
      foreach_context->user_data);
  if (mask_context->values_in.is_empty()) {
    mask_context->values_out = MEM_new_array_zeroed<float>(num_vertices,
                                                           "multires mask vert values");
    mask_context->values_out_num = num_vertices;
    return true;
  }
  return num_vertices == mask_context->values_in.size();
}

static void multires_reshape_mask_foreach_vert_inner(
    const bke::subdiv::ForeachContext *foreach_context,
    void * /*tls_v*/,
    const int ptex_face_index,
    const float ptex_face_u,
    const float ptex_face_v,
    const int /*coarse_face_index*/,
    const int /*coarse_face_corner*/,
    const int subdiv_vert_index)
{
  PTexCoord ptex_coord{};
  ptex_coord.ptex_face_index = ptex_face_index;
  ptex_coord.u = ptex_face_u;
  ptex_coord.v = ptex_face_v;
  multires_reshape_mask_foreach_vert(foreach_context, &ptex_coord, subdiv_vert_index);
}

static void multires_reshape_mask_foreach_vert_every_corner(
    const bke::subdiv::ForeachContext *foreach_context,
    void * /*tls_v*/,
    const int ptex_face_index,
    const float ptex_face_u,
    const float ptex_face_v,
    const int /*coarse_vert_index*/,
    const int /*coarse_face_index*/,
    const int /*coarse_face_corner*/,
    const int subdiv_vert_index)
{
  PTexCoord ptex_coord{};
  ptex_coord.ptex_face_index = ptex_face_index;
  ptex_coord.u = ptex_face_u;
  ptex_coord.v = ptex_face_v;
  multires_reshape_mask_foreach_vert(foreach_context, &ptex_coord, subdiv_vert_index);
}

static void multires_reshape_mask_foreach_vert_every_edge(
    const bke::subdiv::ForeachContext *foreach_context,
    void * /*tls_v*/,
    const int ptex_face_index,
    const float ptex_face_u,
    const float ptex_face_v,
    const int /*coarse_edge_index*/,
    const int /*coarse_face_index*/,
    const int /*coarse_face_corner*/,
    const int subdiv_vert_index)
{
  PTexCoord ptex_coord{};
  ptex_coord.ptex_face_index = ptex_face_index;
  ptex_coord.u = ptex_face_u;
  ptex_coord.v = ptex_face_v;
  multires_reshape_mask_foreach_vert(foreach_context, &ptex_coord, subdiv_vert_index);
}

static bool multires_reshape_mask_walk(const MultiresReshapeContext *reshape_context,
                                       MultiresReshapeMaskContext *mask_context)
{
  bke::subdiv::ForeachContext foreach_context{};
  foreach_context.topology_info = multires_reshape_mask_foreach_topology_info;
  foreach_context.vert_inner = multires_reshape_mask_foreach_vert_inner;
  foreach_context.vert_every_edge = multires_reshape_mask_foreach_vert_every_edge;
  foreach_context.vert_every_corner = multires_reshape_mask_foreach_vert_every_corner;
  foreach_context.user_data = mask_context;

  bke::subdiv::ToMeshSettings mesh_settings;
  mesh_settings.resolution = (1 << reshape_context->reshape.level) + 1;
  mesh_settings.use_optimal_display = false;

  return bke::subdiv::foreach_subdiv_geometry(
      reshape_context->subdiv, &foreach_context, &mesh_settings, reshape_context->base_mesh);
}

bool multires_reshape_assign_mask_from_vert_values(const MultiresReshapeContext *reshape_context,
                                                   const Span<float> values)
{
  MultiresReshapeMaskContext mask_context{};
  mask_context.reshape_context = reshape_context;
  mask_context.values_in = values;
  return multires_reshape_mask_walk(reshape_context, &mask_context);
}

float *multires_reshape_read_mask_to_vert_values(const MultiresReshapeContext *reshape_context,
                                                 int *r_values_num)
{
  MultiresReshapeMaskContext mask_context{};
  mask_context.reshape_context = reshape_context;
  if (!multires_reshape_mask_walk(reshape_context, &mask_context)) {
    MEM_SAFE_DELETE(mask_context.values_out);
    *r_values_num = 0;
    return nullptr;
  }
  *r_values_num = mask_context.values_out_num;
  return mask_context.values_out;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Grid sample -> subdivided-vertex index map
 *
 * The correspondence itself, exposed as data rather than used to move a
 * payload. The two walks above each carry one channel (positions, mask) across
 * the same grid<->vertex relation; an external engine that keeps its own
 * subdivision needs the relation itself, so it can convert any number of
 * channels without a round trip through Blender for each one, and pair samples
 * exactly rather than by proximity.
 *
 * Every grid sample of the top level is visited, boundary samples included (the
 * foreach hands each shared vertex to every grid replica that owns a copy), so
 * a `-1` left in the result means the walk never reached that sample.
 * \{ */

struct MultiresReshapeGridVertIndicesContext {
  const MultiresReshapeContext *reshape_context;

  int *indices = nullptr;
  int indices_num = 0;
};

static void multires_reshape_grid_vert_index_single(
    const bke::subdiv::ForeachContext *foreach_context,
    const GridCoord *grid_coord,
    const int subdiv_vert_index)
{
  MultiresReshapeGridVertIndicesContext *context =
      static_cast<MultiresReshapeGridVertIndicesContext *>(foreach_context->user_data);
  const int grid_size = context->reshape_context->top.grid_size;

  /* Same quantization as multires_reshape_grid_element_for_grid_coord(), so a
   * sample's slot here is the slot its MDisps/mask element occupies. */
  const int grid_x = lround(grid_coord->u * (grid_size - 1));
  const int grid_y = lround(grid_coord->v * (grid_size - 1));
  const int index = grid_coord->grid_index * grid_size * grid_size + grid_y * grid_size + grid_x;

  BLI_assert(index >= 0 && index < context->indices_num);
  context->indices[index] = subdiv_vert_index;
}

static void multires_reshape_grid_vert_index_foreach_vert(
    const bke::subdiv::ForeachContext *foreach_context,
    const PTexCoord *ptex_coord,
    const int subdiv_vert_index)
{
  const MultiresReshapeGridVertIndicesContext *context =
      static_cast<MultiresReshapeGridVertIndicesContext *>(foreach_context->user_data);
  const MultiresReshapeContext *reshape_context = context->reshape_context;

  const GridCoord grid_coord = multires_reshape_ptex_coord_to_grid(reshape_context, ptex_coord);
  const int face_index = multires_reshape_grid_to_face_index(reshape_context,
                                                             grid_coord.grid_index);

  const int num_corners = reshape_context->base_faces[face_index].size();
  const int start_grid_index = reshape_context->face_start_grid_index[face_index];
  const int corner = grid_coord.grid_index - start_grid_index;

  if (grid_coord.u == 0.0f && grid_coord.v == 0.0f) {
    for (int current_corner = 0; current_corner < num_corners; ++current_corner) {
      GridCoord corner_grid_coord = grid_coord;
      corner_grid_coord.grid_index = start_grid_index + current_corner;
      multires_reshape_grid_vert_index_single(
          foreach_context, &corner_grid_coord, subdiv_vert_index);
    }
    return;
  }

  multires_reshape_grid_vert_index_single(foreach_context, &grid_coord, subdiv_vert_index);

  if (grid_coord.u == 0.0f) {
    GridCoord prev_grid_coord;
    prev_grid_coord.grid_index = start_grid_index + ((corner + num_corners - 1) % num_corners);
    prev_grid_coord.u = grid_coord.v;
    prev_grid_coord.v = 0.0f;
    multires_reshape_grid_vert_index_single(foreach_context, &prev_grid_coord, subdiv_vert_index);
  }

  if (grid_coord.v == 0.0f) {
    GridCoord next_grid_coord;
    next_grid_coord.grid_index = start_grid_index + ((corner + 1) % num_corners);
    next_grid_coord.u = 0.0f;
    next_grid_coord.v = grid_coord.u;
    multires_reshape_grid_vert_index_single(foreach_context, &next_grid_coord, subdiv_vert_index);
  }
}

static bool multires_reshape_grid_vert_index_foreach_topology_info(
    const bke::subdiv::ForeachContext * /*foreach_context*/,
    const int /*num_vertices*/,
    const int /*num_edges*/,
    const int /*num_loops*/,
    const int /*num_faces*/,
    const Span<int> /*subdiv_face_offset*/)
{
  return true;
}

static void multires_reshape_grid_vert_index_foreach_vert_inner(
    const bke::subdiv::ForeachContext *foreach_context,
    void * /*tls_v*/,
    const int ptex_face_index,
    const float ptex_face_u,
    const float ptex_face_v,
    const int /*coarse_face_index*/,
    const int /*coarse_face_corner*/,
    const int subdiv_vert_index)
{
  PTexCoord ptex_coord{};
  ptex_coord.ptex_face_index = ptex_face_index;
  ptex_coord.u = ptex_face_u;
  ptex_coord.v = ptex_face_v;
  multires_reshape_grid_vert_index_foreach_vert(foreach_context, &ptex_coord, subdiv_vert_index);
}

static void multires_reshape_grid_vert_index_foreach_vert_every_corner(
    const bke::subdiv::ForeachContext *foreach_context,
    void * /*tls_v*/,
    const int ptex_face_index,
    const float ptex_face_u,
    const float ptex_face_v,
    const int /*coarse_vert_index*/,
    const int /*coarse_face_index*/,
    const int /*coarse_face_corner*/,
    const int subdiv_vert_index)
{
  PTexCoord ptex_coord{};
  ptex_coord.ptex_face_index = ptex_face_index;
  ptex_coord.u = ptex_face_u;
  ptex_coord.v = ptex_face_v;
  multires_reshape_grid_vert_index_foreach_vert(foreach_context, &ptex_coord, subdiv_vert_index);
}

static void multires_reshape_grid_vert_index_foreach_vert_every_edge(
    const bke::subdiv::ForeachContext *foreach_context,
    void * /*tls_v*/,
    const int ptex_face_index,
    const float ptex_face_u,
    const float ptex_face_v,
    const int /*coarse_edge_index*/,
    const int /*coarse_face_index*/,
    const int /*coarse_face_corner*/,
    const int subdiv_vert_index)
{
  PTexCoord ptex_coord{};
  ptex_coord.ptex_face_index = ptex_face_index;
  ptex_coord.u = ptex_face_u;
  ptex_coord.v = ptex_face_v;
  multires_reshape_grid_vert_index_foreach_vert(foreach_context, &ptex_coord, subdiv_vert_index);
}

int *multires_reshape_read_grid_vert_indices(const MultiresReshapeContext *reshape_context,
                                             int *r_indices_num)
{
  const int grid_size = reshape_context->top.grid_size;
  const int indices_num = reshape_context->num_grids * grid_size * grid_size;

  MultiresReshapeGridVertIndicesContext context{};
  context.reshape_context = reshape_context;
  context.indices = MEM_new_array_zeroed<int>(size_t(indices_num), "multires grid vert indices");
  context.indices_num = indices_num;
  /* Unvisited must stay distinguishable: 0 is a valid vertex index. */
  for (int i = 0; i < indices_num; i++) {
    context.indices[i] = -1;
  }

  bke::subdiv::ForeachContext foreach_context{};
  foreach_context.topology_info = multires_reshape_grid_vert_index_foreach_topology_info;
  foreach_context.vert_inner = multires_reshape_grid_vert_index_foreach_vert_inner;
  foreach_context.vert_every_edge = multires_reshape_grid_vert_index_foreach_vert_every_edge;
  foreach_context.vert_every_corner = multires_reshape_grid_vert_index_foreach_vert_every_corner;
  foreach_context.user_data = &context;

  bke::subdiv::ToMeshSettings mesh_settings;
  mesh_settings.resolution = (1 << reshape_context->top.level) + 1;
  mesh_settings.use_optimal_display = false;

  if (!bke::subdiv::foreach_subdiv_geometry(
          reshape_context->subdiv, &foreach_context, &mesh_settings, reshape_context->base_mesh))
  {
    MEM_SAFE_DELETE(context.indices);
    *r_indices_num = 0;
    return nullptr;
  }

  *r_indices_num = indices_num;
  return context.indices;
}

/** \} */

}  // namespace blender
