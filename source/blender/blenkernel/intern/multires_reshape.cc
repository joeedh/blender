/* SPDX-FileCopyrightText: 2020 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "DNA_mesh_types.h"
#include "DNA_modifier_types.h"

#include "BKE_customdata.hh"
#include "BKE_lib_id.hh"
#include "BKE_modifier.hh"
#include "BKE_multires.hh"
#include "BKE_object.hh"
#include "BKE_paint.hh"

#include "DEG_depsgraph_query.hh"

#include "multires_reshape.hh"

namespace blender {

/**
 * Store the original grids, which #multires_reshape_smooth_object_grids_with_details needs to
 * propagate existing details to the higher levels.
 *
 * Storing them duplicates the entire #CD_MDISPS layer, so skip it when the smoothing is going to
 * be skipped as well, which is the case when displacement is assigned at the top level.
 */
static void multiresModifier_reshape_store_original_grids_for_smoothing(
    MultiresReshapeContext *reshape_context)
{
  if (reshape_context->top.level == reshape_context->reshape.level) {
    return;
  }
  multires_reshape_store_original_grids(reshape_context);
}

/* -------------------------------------------------------------------- */
/** \name Reshape from object
 * \{ */

static bool multiresModifier_reshapeFromVertcos(Depsgraph *depsgraph,
                                                Object *object,
                                                MultiresModifierData *mmd,
                                                Span<float3> positions)
{
  MultiresReshapeContext reshape_context;
  if (!multires_reshape_context_create_from_object(&reshape_context, depsgraph, object, mmd)) {
    return false;
  }
  multiresModifier_reshape_store_original_grids_for_smoothing(&reshape_context);
  multires_reshape_ensure_grids(id_cast<Mesh *>(object->data), reshape_context.top.level);
  if (!multires_reshape_assign_final_coords_from_vertcos(&reshape_context, positions)) {
    multires_reshape_context_free(&reshape_context);
    return false;
  }
  multires_reshape_smooth_object_grids_with_details(&reshape_context);
  multires_reshape_object_grids_to_tangent_displacement(&reshape_context);
  multires_reshape_context_free(&reshape_context);
  return true;
}

bool multiresModifier_reshapeFromObject(Depsgraph *depsgraph,
                                        MultiresModifierData *mmd,
                                        Object *dst,
                                        Object *src)
{
  const Object *ob_eval = DEG_get_evaluated(depsgraph, src);
  if (!ob_eval) {
    return false;
  }
  const Mesh *src_mesh_eval = BKE_object_get_evaluated_mesh(ob_eval);
  if (!src_mesh_eval) {
    return false;
  }

  return multiresModifier_reshapeFromVertcos(depsgraph, dst, mmd, src_mesh_eval->vert_positions());
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Reshape from modifier
 * \{ */

bool multiresModifier_reshapeFromDeformModifier(Depsgraph *depsgraph,
                                                Object *object,
                                                MultiresModifierData *mmd,
                                                ModifierData *deform_md)
{
  MultiresModifierData highest_mmd = dna::shallow_copy(*mmd);
  highest_mmd.sculptlvl = highest_mmd.totlvl;
  highest_mmd.lvl = highest_mmd.totlvl;
  highest_mmd.renderlvl = highest_mmd.totlvl;

  /* Create mesh for the multires, ignoring any further modifiers (leading
   * deformation modifiers will be applied though). */
  Mesh *multires_mesh = BKE_multires_create_mesh(depsgraph, object, &highest_mmd);
  Array<float3> deformed_verts(multires_mesh->vert_positions());

  /* Apply deformation modifier on the multires, */
  ModifierEvalContext modifier_ctx{};
  modifier_ctx.depsgraph = depsgraph;
  modifier_ctx.object = object;
  modifier_ctx.flag = MOD_APPLY_USECACHE | MOD_APPLY_IGNORE_SIMPLIFY;

  const bool deform_success = BKE_modifier_deform_verts(
      deform_md, &modifier_ctx, multires_mesh, deformed_verts);
  BKE_id_free(nullptr, multires_mesh);
  if (!deform_success) {
    return false;
  }

  /* Reshaping */
  bool result = multiresModifier_reshapeFromVertcos(
      depsgraph, object, &highest_mmd, deformed_verts);

  return result;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Reshape from grids
 * \{ */

bool multiresModifier_reshapeFromCCG(const int tot_level, Mesh *coarse_mesh, SubdivCCG *subdiv_ccg)
{
  MultiresReshapeContext reshape_context;
  if (!multires_reshape_context_create_from_ccg(
          &reshape_context, subdiv_ccg, coarse_mesh, tot_level))
  {
    return false;
  }

  multires_ensure_external_read(coarse_mesh, reshape_context.top.level);

  multiresModifier_reshape_store_original_grids_for_smoothing(&reshape_context);
  multires_reshape_ensure_grids(coarse_mesh, reshape_context.top.level);
  if (!multires_reshape_assign_final_coords_from_ccg(&reshape_context, subdiv_ccg)) {
    multires_reshape_context_free(&reshape_context);
    return false;
  }
  multires_reshape_smooth_object_grids_with_details(&reshape_context);
  multires_reshape_object_grids_to_tangent_displacement(&reshape_context);
  multires_reshape_context_free(&reshape_context);
  return true;
}

/* Assign every top-level grid sample its absolute position straight from a flat
 * per-grid array, mirroring assign_final_coords_from_ccg but sourcing a plain
 * `grid[grid * grid_area + y * grid_size + x]` buffer instead of a SubdivCCG. */
static bool multires_reshape_assign_final_coords_from_grid_array(
    const MultiresReshapeContext *reshape_context, const Span<float3> grid_positions)
{
  const int grid_size = reshape_context->reshape.grid_size;
  const int grid_area = grid_size * grid_size;
  const float grid_size_1_inv = 1.0f / (float(grid_size) - 1.0f);
  const int num_grids = reshape_context->num_grids;
  if (grid_positions.size() != int64_t(num_grids) * grid_area) {
    return false;
  }
  for (int grid_index = 0; grid_index < num_grids; ++grid_index) {
    for (int y = 0; y < grid_size; ++y) {
      const float v = float(y) * grid_size_1_inv;
      for (int x = 0; x < grid_size; ++x) {
        const float u = float(x) * grid_size_1_inv;
        GridCoord grid_coord;
        grid_coord.grid_index = grid_index;
        grid_coord.u = u;
        grid_coord.v = v;
        ReshapeGridElement grid_element = multires_reshape_grid_element_for_grid_coord(
            reshape_context, &grid_coord);
        BLI_assert(grid_element.displacement != nullptr);
        *grid_element.displacement =
            grid_positions[int64_t(grid_index) * grid_area + int64_t(y) * grid_size + x];
      }
    }
  }
  return true;
}

bool multiresModifier_reshapeFromPositions(Depsgraph *depsgraph,
                                           MultiresModifierData *mmd,
                                           Object *object,
                                           const Span<float3> grid_positions)
{
  /* Reshape the full stack: force the reshape level to the top so the grids are
   * assigned and baked at `totlvl` resolution. */
  MultiresModifierData highest_mmd = dna::shallow_copy(*mmd);
  highest_mmd.sculptlvl = highest_mmd.totlvl;
  highest_mmd.lvl = highest_mmd.totlvl;
  highest_mmd.renderlvl = highest_mmd.totlvl;

  MultiresReshapeContext reshape_context;
  if (!multires_reshape_context_create_from_object(
          &reshape_context, depsgraph, object, &highest_mmd))
  {
    return false;
  }
  multires_reshape_store_original_grids(&reshape_context);
  multires_reshape_ensure_grids(id_cast<Mesh *>(object->data), reshape_context.top.level);
  if (!multires_reshape_assign_final_coords_from_grid_array(&reshape_context, grid_positions)) {
    multires_reshape_context_free(&reshape_context);
    return false;
  }
  multires_reshape_smooth_object_grids_with_details(&reshape_context);
  multires_reshape_object_grids_to_tangent_displacement(&reshape_context);
  multires_reshape_context_free(&reshape_context);
  return true;
}

bool multiresModifier_maskFromVertValues(Depsgraph *depsgraph,
                                         Main *bmain,
                                         MultiresModifierData *mmd,
                                         Object *object,
                                         const Span<float> values)
{
  /* Consume the values at `totlvl` resolution, like the vertcos reshape. The
   * mask layer is created at the top level when missing; masks are absolute
   * scalars, so the assignment is the whole transfer (no smooth/bake pass). */
  MultiresModifierData highest_mmd = dna::shallow_copy(*mmd);
  highest_mmd.sculptlvl = highest_mmd.totlvl;
  highest_mmd.lvl = highest_mmd.totlvl;
  highest_mmd.renderlvl = highest_mmd.totlvl;

  BKE_sculpt_mask_layers_ensure(depsgraph, bmain, object, &highest_mmd);

  MultiresReshapeContext reshape_context;
  if (!multires_reshape_context_create_from_object(
          &reshape_context, depsgraph, object, &highest_mmd))
  {
    return false;
  }
  /* An existing layer may hold coarser grids; resize them to the top level
   * (they are fully overwritten by the assignment below). */
  multires_reshape_ensure_grids(id_cast<Mesh *>(object->data), reshape_context.top.level);
  const bool ok = multires_reshape_assign_mask_from_vert_values(&reshape_context, values);
  multires_reshape_context_free(&reshape_context);
  return ok;
}

float *multiresModifier_maskToVertValues(Depsgraph *depsgraph,
                                         MultiresModifierData *mmd,
                                         Object *object,
                                         int *r_values_num,
                                         bool *r_has_mask)
{
  MultiresModifierData highest_mmd = dna::shallow_copy(*mmd);
  highest_mmd.sculptlvl = highest_mmd.totlvl;
  highest_mmd.lvl = highest_mmd.totlvl;
  highest_mmd.renderlvl = highest_mmd.totlvl;

  *r_values_num = 0;
  MultiresReshapeContext reshape_context;
  if (!multires_reshape_context_create_from_object(
          &reshape_context, depsgraph, object, &highest_mmd))
  {
    *r_has_mask = false;
    return nullptr;
  }
  *r_has_mask = reshape_context.grid_paint_masks != nullptr;
  float *values = multires_reshape_read_mask_to_vert_values(&reshape_context, r_values_num);
  multires_reshape_context_free(&reshape_context);
  return values;
}

int *multiresModifier_gridVertIndices(Depsgraph *depsgraph,
                                      MultiresModifierData *mmd,
                                      Object *object,
                                      int *r_indices_num,
                                      int *r_grid_size)
{
  MultiresModifierData highest_mmd = dna::shallow_copy(*mmd);
  highest_mmd.sculptlvl = highest_mmd.totlvl;
  highest_mmd.lvl = highest_mmd.totlvl;
  highest_mmd.renderlvl = highest_mmd.totlvl;

  *r_indices_num = 0;
  *r_grid_size = 0;
  MultiresReshapeContext reshape_context;
  if (!multires_reshape_context_create_from_object(
          &reshape_context, depsgraph, object, &highest_mmd))
  {
    return nullptr;
  }
  *r_grid_size = reshape_context.top.grid_size;
  int *indices = multires_reshape_read_grid_vert_indices(&reshape_context, r_indices_num);
  multires_reshape_context_free(&reshape_context);
  return indices;
}

bool multiresModifier_reshapeFromVertPositions(Depsgraph *depsgraph,
                                               MultiresModifierData *mmd,
                                               Object *object,
                                               const Span<float3> positions)
{
  /* Reshape the whole stack: force the reshape level to the top so `positions`
   * is consumed at `totlvl` subdivided-mesh resolution ((2^totlvl)+1 per base
   * edge, subdiv-vertex order — the layout src_mesh_eval->vert_positions() and
   * BKE_multires_create_mesh produce). Each shared vertex appears once; the
   * reshape scatters it to every grid replica, so there is no seam-ordering
   * hazard (unlike a per-grid feed). */
  MultiresModifierData highest_mmd = dna::shallow_copy(*mmd);
  highest_mmd.sculptlvl = highest_mmd.totlvl;
  highest_mmd.lvl = highest_mmd.totlvl;
  highest_mmd.renderlvl = highest_mmd.totlvl;
  return multiresModifier_reshapeFromVertcos(depsgraph, object, &highest_mmd, positions);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Subdivision
 * \{ */

void multiresModifier_subdivide(Object *object,
                                MultiresModifierData *mmd,
                                const MultiresSubdivideModeType mode)
{
  const int top_level = mmd->totlvl + 1;
  multiresModifier_subdivide_to_level(object, mmd, top_level, mode);
}

void multiresModifier_subdivide_to_level(Object *object,
                                         MultiresModifierData *mmd,
                                         const int top_level,
                                         const MultiresSubdivideModeType mode)
{
  if (top_level <= mmd->totlvl) {
    return;
  }

  Mesh *coarse_mesh = id_cast<Mesh *>(object->data);
  if (coarse_mesh->corners_num == 0) {
    /* If there are no loops in the mesh implies there is no CD_MDISPS as well. So can early output
     * from here as there is nothing to subdivide. */
    return;
  }

  MultiresReshapeContext reshape_context;

  /* There was no multires at all, all displacement is at 0. Can simply make sure all mdisps grids
   * are allocated at a proper level and return. */
  const bool has_mdisps = CustomData_has_layer(&coarse_mesh->corner_data, CD_MDISPS);
  if (!has_mdisps) {
    CustomData_add_layer(
        &coarse_mesh->corner_data, CD_MDISPS, CD_SET_DEFAULT, coarse_mesh->corners_num);
  }

  /* NOTE: Subdivision happens from the top level of the existing multires modifier. If it is set
   * to 0 and there is mdisps layer it would mean that the modifier went out of sync with the data.
   * This happens when, for example, linking modifiers from one object to another.
   *
   * In such cases simply ensure grids to be the proper level.
   *
   * If something smarter is needed it is up to the operators which does data synchronization, so
   * that the mdisps layer is also synchronized. */
  if (!has_mdisps || top_level == 1 || mmd->totlvl == 0) {
    multires_reshape_ensure_grids(coarse_mesh, top_level);
    if (ELEM(mode, MultiresSubdivideModeType::Linear, MultiresSubdivideModeType::Simple)) {
      multires_subdivide_create_tangent_displacement_linear_grids(object, mmd);
    }
    else {
      multires_set_tot_level(object, mmd, top_level);
    }
    return;
  }

  multires_flush_sculpt_updates(object);

  if (!multires_reshape_context_create_from_modifier(&reshape_context, object, mmd, top_level)) {
    return;
  }

  multires_reshape_store_original_grids(&reshape_context);
  multires_reshape_ensure_grids(coarse_mesh, reshape_context.top.level);
  multires_reshape_assign_final_elements_from_orig_mdisps(&reshape_context);

  /* Free original grids which makes it so smoothing with details thinks all the details were
   * added against base mesh's limit surface. This is similar behavior to as if we've done all
   * displacement in sculpt mode at the old top level and then propagated to the new top level. */
  multires_reshape_free_original_grids(&reshape_context);

  if (ELEM(mode, MultiresSubdivideModeType::Linear, MultiresSubdivideModeType::Simple)) {
    multires_reshape_smooth_object_grids(&reshape_context, mode);
  }
  else {
    multires_reshape_smooth_object_grids_with_details(&reshape_context);
  }

  multires_reshape_object_grids_to_tangent_displacement(&reshape_context);
  multires_reshape_context_free(&reshape_context);

  multires_set_tot_level(object, mmd, top_level);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Apply base
 * \{ */

void multiresModifier_base_apply(Depsgraph *depsgraph,
                                 Object *object,
                                 MultiresModifierData *mmd,
                                 const ApplyBaseMode mode)
{
  multires_force_sculpt_rebuild(object);

  MultiresReshapeContext reshape_context;
  if (!multires_reshape_context_create_from_object(&reshape_context, depsgraph, object, mmd)) {
    return;
  }

  multires_reshape_store_original_grids(&reshape_context);

  /* At this point base_mesh is object's mesh, the subdiv is initialized to the deformed state of
   * the base mesh.
   * Store coordinates of top level grids in object space which will define true shape we would
   * want to reshape to after modifying the base mesh. */
  multires_reshape_assign_final_coords_from_mdisps(&reshape_context);

  /* For modifying base mesh we only want to consider deformation caused by multires displacement
   * and ignore all deformation which might be caused by deformation modifiers leading the multires
   * one.
   * So refine the subdiv to the original mesh vertices positions, which will also need to make
   * it so object space displacement is re-evaluated for them (as in, can not re-use any knowledge
   * from the final coordinates in the object space ). */
  multires_reshape_apply_base_refine_from_base(&reshape_context);

  /* Modify original mesh coordinates. This happens in two steps:
   * - Coordinates are set to their final location, where they are intended to be in the final
   *   result.
   * - Heuristic moves them a bit, kind of canceling out the effect of subsurf (so then when
   *   multires modifier applies subsurf vertices are placed at the desired location). */
  multires_reshape_apply_base_update_mesh_coords(&reshape_context);
  if (mode == ApplyBaseMode::ForSubdivision) {
    multires_reshape_apply_base_refit_base_mesh(&reshape_context);
  }
  multires_reshape_apply_base_update_shape_key(&reshape_context);

  /* Reshape to the stored final state.
   * Not that the base changed, so the subdiv is to be refined to the new positions. Unfortunately,
   * this can not be done foe entirely cheap: if there were deformation modifiers prior to the
   * multires they need to be re-evaluated for the new base mesh. */
  multires_reshape_apply_base_refine_from_deform(&reshape_context);
  multires_reshape_object_grids_to_tangent_displacement(&reshape_context);

  multires_reshape_context_free(&reshape_context);
}

/** \} */

}  // namespace blender
