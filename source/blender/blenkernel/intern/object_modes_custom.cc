/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Registry of addon-registered object modes, see #BKE_object_modes.hh.
 */

#include "MEM_guardedalloc.h"

#include "BLI_listbase.hh"
#include "BLI_utildefines.hh"

#include "DNA_object_types.h"
#include "DNA_view3d_types.h"

#include "BKE_object_draw_provider.hh"
#include "BKE_object_modes.hh"

namespace blender {

static ListBaseT<ObjectModeType> g_object_mode_types = {nullptr, nullptr};

bool BKE_object_mode_type_add(ObjectModeType *mt)
{
  if (mt->idname[0] == '\0') {
    return false;
  }
  if (BKE_object_mode_type_find(mt->idname) != nullptr) {
    return false;
  }
  BLI_addtail(&g_object_mode_types, mt);
  return true;
}

void BKE_object_mode_type_remove(ObjectModeType *mt)
{
  BLI_remlink(&g_object_mode_types, mt);
  MEM_delete(mt);
}

ObjectModeType *BKE_object_mode_type_find(const char *idname)
{
  for (ObjectModeType &mt : g_object_mode_types) {
    if (STREQ(mt.idname, idname)) {
      return &mt;
    }
  }
  return nullptr;
}

bool BKE_object_custom_mode_uses_custom_undo(const Object *ob)
{
  if (ob == nullptr || (ob->mode & OB_MODE_CUSTOM) == 0 || ob->custom_mode_id[0] == '\0') {
    return false;
  }
  const ObjectModeType *mt = BKE_object_mode_type_find(ob->custom_mode_id);
  return mt != nullptr && (mt->flag & OBJECT_MODE_TYPE_USE_CUSTOM_UNDO) != 0 &&
         mt->undo_decode != nullptr;
}

bool BKE_object_custom_mode_uses_sculpt_paint(const Object *ob)
{
  if (ob == nullptr || (ob->mode & OB_MODE_CUSTOM) == 0 || ob->custom_mode_id[0] == '\0') {
    return false;
  }
  const ObjectModeType *mt = BKE_object_mode_type_find(ob->custom_mode_id);
  return mt != nullptr && (mt->flag & OBJECT_MODE_TYPE_USE_SCULPT_PAINT) != 0;
}

bool BKE_object_mode_draw_provider_set(ObjectModeType *mt, const ExternalDrawProvider *provider)
{
  if (provider == nullptr || provider->abi_version != BKE_EXTERNAL_DRAW_ABI_VERSION) {
    return false;
  }
  mt->draw_provider = provider;
  return true;
}

const ExternalDrawProvider *BKE_object_external_draw_provider_get(const Object *ob)
{
  if (ob == nullptr || (ob->mode & OB_MODE_CUSTOM) == 0 || ob->custom_mode_id[0] == '\0') {
    return nullptr;
  }
  const ObjectModeType *mt = BKE_object_mode_type_find(ob->custom_mode_id);
  return mt != nullptr ? mt->draw_provider : nullptr;
}

bool BKE_object_use_external_draw(const Object *ob, const RegionView3D *rv3d)
{
  if (BKE_object_external_draw_provider_get(ob) == nullptr) {
    return false;
  }
  /* External render engines (e.g. Cycles viewport) render from evaluated
   * geometry and cannot consume the provider, so fall back to the flushed mesh
   * — same rule as sculpt's PBVH draw path. */
  const bool external_engine = rv3d && rv3d->view_render != nullptr;
  return !external_engine;
}

const char *BKE_object_custom_mode_default_tool(const Object *ob)
{
  if (ob == nullptr || (ob->mode & OB_MODE_CUSTOM) == 0 || ob->custom_mode_id[0] == '\0') {
    return nullptr;
  }
  const ObjectModeType *mt = BKE_object_mode_type_find(ob->custom_mode_id);
  if (mt == nullptr || mt->default_tool[0] == '\0') {
    return nullptr;
  }
  return mt->default_tool;
}

ListBaseT<ObjectModeType> &BKE_object_mode_types_get()
{
  return g_object_mode_types;
}

bool BKE_object_mode_type_poll_object(const ObjectModeType *mt, const Object *ob)
{
  BLI_assert(ob->type >= 0 && ob->type < 64);
  return (mt->object_type_mask & (uint64_t(1) << ob->type)) != 0;
}

void BKE_object_mode_types_exit()
{
  while (ObjectModeType *mt = static_cast<ObjectModeType *>(BLI_pophead(&g_object_mode_types))) {
    MEM_delete(mt);
  }
}

}  // namespace blender
