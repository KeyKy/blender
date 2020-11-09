/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

/** \file
 * \ingroup bke
 */

#include "MEM_guardedalloc.h"

#include "DNA_defaults.h"
#include "DNA_material_types.h"
#include "DNA_object_types.h"
#include "DNA_pointcloud_types.h"

#include "BLI_listbase.h"
#include "BLI_math.h"
#include "BLI_rand.h"
#include "BLI_string.h"
#include "BLI_utildefines.h"

#include "BKE_anim_data.h"
#include "BKE_customdata.h"
#include "BKE_global.h"
#include "BKE_idtype.h"
#include "BKE_lib_id.h"
#include "BKE_lib_query.h"
#include "BKE_lib_remap.h"
#include "BKE_main.h"
#include "BKE_modifier.h"
#include "BKE_object.h"
#include "BKE_pointcloud.h"

#include "BLT_translation.h"

#include "DEG_depsgraph_query.h"

#include "BLO_read_write.h"

/* PointCloud datablock */

static void pointcloud_random(PointCloud *pointcloud);

const char *POINTCLOUD_ATTR_POSITION = "Position";
const char *POINTCLOUD_ATTR_RADIUS = "Radius";

static void pointcloud_init_data(ID *id)
{
  PointCloud *pointcloud = (PointCloud *)id;
  BLI_assert(MEMCMP_STRUCT_AFTER_IS_ZERO(pointcloud, id));

  MEMCPY_STRUCT_AFTER(pointcloud, DNA_struct_default_get(PointCloud), id);

  CustomData_reset(&pointcloud->pdata);
  CustomData_add_layer_named(&pointcloud->pdata,
                             CD_PROP_FLOAT3,
                             CD_CALLOC,
                             nullptr,
                             pointcloud->totpoint,
                             POINTCLOUD_ATTR_POSITION);
  BKE_pointcloud_update_customdata_pointers(pointcloud);
}

static void pointcloud_copy_data(Main *UNUSED(bmain), ID *id_dst, const ID *id_src, const int flag)
{
  PointCloud *pointcloud_dst = (PointCloud *)id_dst;
  const PointCloud *pointcloud_src = (const PointCloud *)id_src;
  pointcloud_dst->mat = static_cast<Material **>(MEM_dupallocN(pointcloud_dst->mat));

  const eCDAllocType alloc_type = (flag & LIB_ID_COPY_CD_REFERENCE) ? CD_REFERENCE : CD_DUPLICATE;
  CustomData_copy(&pointcloud_src->pdata,
                  &pointcloud_dst->pdata,
                  CD_MASK_ALL,
                  alloc_type,
                  pointcloud_dst->totpoint);
  BKE_pointcloud_update_customdata_pointers(pointcloud_dst);
}

static void pointcloud_free_data(ID *id)
{
  PointCloud *pointcloud = (PointCloud *)id;
  BKE_animdata_free(&pointcloud->id, false);
  BKE_pointcloud_batch_cache_free(pointcloud);
  CustomData_free(&pointcloud->pdata, pointcloud->totpoint);
  MEM_SAFE_FREE(pointcloud->mat);
}

static void pointcloud_foreach_id(ID *id, LibraryForeachIDData *data)
{
  PointCloud *pointcloud = (PointCloud *)id;
  for (int i = 0; i < pointcloud->totcol; i++) {
    BKE_LIB_FOREACHID_PROCESS(data, pointcloud->mat[i], IDWALK_CB_USER);
  }
}

static void pointcloud_blend_write(BlendWriter *writer, ID *id, const void *id_address)
{
  PointCloud *pointcloud = (PointCloud *)id;
  if (pointcloud->id.us > 0 || BLO_write_is_undo(writer)) {
    CustomDataLayer *players = nullptr, players_buff[CD_TEMP_CHUNK_SIZE];
    CustomData_blend_write_prepare(
        &pointcloud->pdata, &players, players_buff, ARRAY_SIZE(players_buff));

    /* Write LibData */
    BLO_write_id_struct(writer, PointCloud, id_address, &pointcloud->id);
    BKE_id_blend_write(writer, &pointcloud->id);

    /* Direct data */
    CustomData_blend_write(
        writer, &pointcloud->pdata, players, pointcloud->totpoint, CD_MASK_ALL, &pointcloud->id);

    BLO_write_pointer_array(writer, pointcloud->totcol, pointcloud->mat);
    if (pointcloud->adt) {
      BKE_animdata_blend_write(writer, pointcloud->adt);
    }

    /* Remove temporary data. */
    if (players && players != players_buff) {
      MEM_freeN(players);
    }
  }
}

static void pointcloud_blend_read_data(BlendDataReader *reader, ID *id)
{
  PointCloud *pointcloud = (PointCloud *)id;
  BLO_read_data_address(reader, &pointcloud->adt);
  BKE_animdata_blend_read_data(reader, pointcloud->adt);

  /* Geometry */
  CustomData_blend_read(reader, &pointcloud->pdata, pointcloud->totpoint);
  BKE_pointcloud_update_customdata_pointers(pointcloud);

  /* Materials */
  BLO_read_pointer_array(reader, (void **)&pointcloud->mat);
}

static void pointcloud_blend_read_lib(BlendLibReader *reader, ID *id)
{
  PointCloud *pointcloud = (PointCloud *)id;
  for (int a = 0; a < pointcloud->totcol; a++) {
    BLO_read_id_address(reader, pointcloud->id.lib, &pointcloud->mat[a]);
  }
}

static void pointcloud_blend_read_expand(BlendExpander *expander, ID *id)
{
  PointCloud *pointcloud = (PointCloud *)id;
  for (int a = 0; a < pointcloud->totcol; a++) {
    BLO_expand(expander, pointcloud->mat[a]);
  }
}

IDTypeInfo IDType_ID_PT = {
    /* id_code */ ID_PT,
    /* id_filter */ FILTER_ID_PT,
    /* main_listbase_index */ INDEX_ID_PT,
    /* struct_size */ sizeof(PointCloud),
    /* name */ "PointCloud",
    /* name_plural */ "pointclouds",
    /* translation_context */ BLT_I18NCONTEXT_ID_POINTCLOUD,
    /* flags */ 0,

    /* init_data */ pointcloud_init_data,
    /* copy_data */ pointcloud_copy_data,
    /* free_data */ pointcloud_free_data,
    /* make_local */ nullptr,
    /* foreach_id */ pointcloud_foreach_id,
    /* foreach_cache */ nullptr,

    /* blend_write */ pointcloud_blend_write,
    /* blend_read_data */ pointcloud_blend_read_data,
    /* blend_read_lib */ pointcloud_blend_read_lib,
    /* blend_read_expand */ pointcloud_blend_read_expand,

    /* blend_read_undo_preserve */ nullptr,
};

static void pointcloud_random(PointCloud *pointcloud)
{
  pointcloud->totpoint = 400;
  CustomData_realloc(&pointcloud->pdata, pointcloud->totpoint);
  BKE_pointcloud_update_customdata_pointers(pointcloud);

  RNG *rng = BLI_rng_new(0);

  for (int i = 0; i < pointcloud->totpoint; i++) {
    pointcloud->co[i][0] = 2.0f * BLI_rng_get_float(rng) - 1.0f;
    pointcloud->co[i][1] = 2.0f * BLI_rng_get_float(rng) - 1.0f;
    pointcloud->co[i][2] = 2.0f * BLI_rng_get_float(rng) - 1.0f;
    pointcloud->radius[i] = 0.05f * BLI_rng_get_float(rng);
  }

  BLI_rng_free(rng);
}

void *BKE_pointcloud_add(Main *bmain, const char *name)
{
  PointCloud *pointcloud = static_cast<PointCloud *>(BKE_id_new(bmain, ID_PT, name));

  return pointcloud;
}

void *BKE_pointcloud_add_default(Main *bmain, const char *name)
{
  PointCloud *pointcloud = static_cast<PointCloud *>(BKE_libblock_alloc(bmain, ID_PT, name, 0));

  pointcloud_init_data(&pointcloud->id);

  CustomData_add_layer_named(&pointcloud->pdata,
                             CD_PROP_FLOAT,
                             CD_CALLOC,
                             nullptr,
                             pointcloud->totpoint,
                             POINTCLOUD_ATTR_RADIUS);
  pointcloud_random(pointcloud);

  return pointcloud;
}

PointCloud *BKE_pointcloud_new_nomain(const int totpoint)
{
  PointCloud *pointcloud = static_cast<PointCloud *>(
      BKE_libblock_alloc(NULL, ID_PT, BKE_idtype_idcode_to_name(ID_PT), LIB_ID_CREATE_LOCALIZE));

  pointcloud_init_data(&pointcloud->id);

  pointcloud->totpoint = totpoint;

  CustomData_add_layer_named(&pointcloud->pdata,
                             CD_PROP_FLOAT,
                             CD_CALLOC,
                             NULL,
                             pointcloud->totpoint,
                             POINTCLOUD_ATTR_RADIUS);

  pointcloud->totpoint = totpoint;
  CustomData_realloc(&pointcloud->pdata, pointcloud->totpoint);
  BKE_pointcloud_update_customdata_pointers(pointcloud);

  return pointcloud;
}

BoundBox *BKE_pointcloud_boundbox_get(Object *ob)
{
  BLI_assert(ob->type == OB_POINTCLOUD);
  PointCloud *pointcloud = static_cast<PointCloud *>(ob->data);

  if (ob->runtime.bb != nullptr && (ob->runtime.bb->flag & BOUNDBOX_DIRTY) == 0) {
    return ob->runtime.bb;
  }

  if (ob->runtime.bb == nullptr) {
    ob->runtime.bb = static_cast<BoundBox *>(MEM_callocN(sizeof(BoundBox), "pointcloud boundbox"));

    float min[3], max[3];
    INIT_MINMAX(min, max);

    float(*pointcloud_co)[3] = pointcloud->co;
    float *pointcloud_radius = pointcloud->radius;
    for (int a = 0; a < pointcloud->totpoint; a++) {
      float *co = pointcloud_co[a];
      float radius = (pointcloud_radius) ? pointcloud_radius[a] : 0.0f;
      const float co_min[3] = {co[0] - radius, co[1] - radius, co[2] - radius};
      const float co_max[3] = {co[0] + radius, co[1] + radius, co[2] + radius};
      DO_MIN(co_min, min);
      DO_MAX(co_max, max);
    }

    BKE_boundbox_init_from_minmax(ob->runtime.bb, min, max);
  }

  return ob->runtime.bb;
}

void BKE_pointcloud_update_customdata_pointers(PointCloud *pointcloud)
{
  pointcloud->co = static_cast<float(*)[3]>(
      CustomData_get_layer_named(&pointcloud->pdata, CD_PROP_FLOAT3, POINTCLOUD_ATTR_POSITION));
  pointcloud->radius = static_cast<float *>(
      CustomData_get_layer_named(&pointcloud->pdata, CD_PROP_FLOAT, POINTCLOUD_ATTR_RADIUS));
}

bool BKE_pointcloud_customdata_required(PointCloud *UNUSED(pointcloud), CustomDataLayer *layer)
{
  return layer->type == CD_PROP_FLOAT3 && STREQ(layer->name, POINTCLOUD_ATTR_POSITION);
}

/* Dependency Graph */

PointCloud *BKE_pointcloud_new_for_eval(const PointCloud *pointcloud_src, int totpoint)
{
  PointCloud *pointcloud_dst = static_cast<PointCloud *>(BKE_id_new_nomain(ID_PT, nullptr));
  CustomData_free(&pointcloud_dst->pdata, pointcloud_dst->totpoint);

  STRNCPY(pointcloud_dst->id.name, pointcloud_src->id.name);
  pointcloud_dst->mat = static_cast<Material **>(MEM_dupallocN(pointcloud_src->mat));
  pointcloud_dst->totcol = pointcloud_src->totcol;

  pointcloud_dst->totpoint = totpoint;
  CustomData_copy(
      &pointcloud_src->pdata, &pointcloud_dst->pdata, CD_MASK_ALL, CD_CALLOC, totpoint);
  BKE_pointcloud_update_customdata_pointers(pointcloud_dst);

  return pointcloud_dst;
}

PointCloud *BKE_pointcloud_copy_for_eval(struct PointCloud *pointcloud_src, bool reference)
{
  int flags = LIB_ID_COPY_LOCALIZE;

  if (reference) {
    flags |= LIB_ID_COPY_CD_REFERENCE;
  }

  PointCloud *result = (PointCloud *)BKE_id_copy_ex(nullptr, &pointcloud_src->id, nullptr, flags);
  return result;
}

static PointCloud *pointcloud_evaluate_modifiers(struct Depsgraph *depsgraph,
                                                 struct Scene *scene,
                                                 Object *object,
                                                 PointCloud *pointcloud_input)
{
  PointCloud *pointcloud = pointcloud_input;

  /* Modifier evaluation modes. */
  const bool use_render = (DEG_get_mode(depsgraph) == DAG_EVAL_RENDER);
  const int required_mode = use_render ? eModifierMode_Render : eModifierMode_Realtime;
  ModifierApplyFlag apply_flag = use_render ? MOD_APPLY_RENDER : MOD_APPLY_USECACHE;
  const ModifierEvalContext mectx = {depsgraph, object, apply_flag};

  /* Get effective list of modifiers to execute. Some effects like shape keys
   * are added as virtual modifiers before the user created modifiers. */
  VirtualModifierData virtualModifierData;
  ModifierData *md = BKE_modifiers_get_virtual_modifierlist(object, &virtualModifierData);

  /* Evaluate modifiers. */
  for (; md; md = md->next) {
    const ModifierTypeInfo *mti = BKE_modifier_get_info((ModifierType)md->type);

    if (!BKE_modifier_is_enabled(scene, md, required_mode)) {
      continue;
    }

    if ((mti->type == eModifierTypeType_OnlyDeform) &&
        (mti->flags & eModifierTypeFlag_AcceptsVertexCosOnly)) {
      /* Ensure we are not modifying the input. */
      if (pointcloud == pointcloud_input) {
        pointcloud = BKE_pointcloud_copy_for_eval(pointcloud, true);
      }

      /* Ensure we are not overwriting referenced data. */
      CustomData_duplicate_referenced_layer_named(
          &pointcloud->pdata, CD_PROP_FLOAT3, POINTCLOUD_ATTR_POSITION, pointcloud->totpoint);
      BKE_pointcloud_update_customdata_pointers(pointcloud);

      /* Created deformed coordinates array on demand. */
      mti->deformVerts(md, &mectx, nullptr, pointcloud->co, pointcloud->totpoint);
    }
    else if (mti->modifyPointCloud) {
      /* Ensure we are not modifying the input. */
      if (pointcloud == pointcloud_input) {
        pointcloud = BKE_pointcloud_copy_for_eval(pointcloud, true);
      }

      PointCloud *pointcloud_next = mti->modifyPointCloud(md, &mectx, pointcloud);

      if (pointcloud_next && pointcloud_next != pointcloud) {
        /* If the modifier returned a new pointcloud, release the old one. */
        if (pointcloud != pointcloud_input) {
          BKE_id_free(nullptr, pointcloud);
        }
        pointcloud = pointcloud_next;
      }
    }
  }

  return pointcloud;
}

void BKE_pointcloud_data_update(struct Depsgraph *depsgraph, struct Scene *scene, Object *object)
{
  /* Free any evaluated data and restore original data. */
  BKE_object_free_derived_caches(object);

  /* Evaluate modifiers. */
  PointCloud *pointcloud = static_cast<PointCloud *>(object->data);
  PointCloud *pointcloud_eval = pointcloud_evaluate_modifiers(
      depsgraph, scene, object, pointcloud);

  /* Assign evaluated object. */
  const bool is_owned = (pointcloud != pointcloud_eval);
  BKE_object_eval_assign_data(object, &pointcloud_eval->id, is_owned);
}

/* Draw Cache */
void (*BKE_pointcloud_batch_cache_dirty_tag_cb)(PointCloud *pointcloud, int mode) = nullptr;
void (*BKE_pointcloud_batch_cache_free_cb)(PointCloud *pointcloud) = nullptr;

void BKE_pointcloud_batch_cache_dirty_tag(PointCloud *pointcloud, int mode)
{
  if (pointcloud->batch_cache) {
    BKE_pointcloud_batch_cache_dirty_tag_cb(pointcloud, mode);
  }
}

void BKE_pointcloud_batch_cache_free(PointCloud *pointcloud)
{
  if (pointcloud->batch_cache) {
    BKE_pointcloud_batch_cache_free_cb(pointcloud);
  }
}
