/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <thread>

#include "BKE_colortools.hh"
#include "BKE_curvemapping_idprop.hh"
#include "BKE_curvemapping_owned.hh"
#include "BKE_gtest_base.hh"
#include "BKE_idprop.hh"
#include "BKE_lib_id.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BLI_listbase.hh"
#include "DNA_ID.h"
#include "DNA_color_types.h"
#include "MEM_guardedalloc.h"
#include "testing/testing.h"

namespace blender::bke::tests {

class OwnedCurveRuntimeTest : public BlenderGTestBase {
 protected:
  Main *bmain = nullptr;
  ID *owner = nullptr;
  IDProperty *definition = nullptr;
  CurveMapping *candidate = nullptr;
  std::string error;
  std::shared_ptr<OwnedCurveRecord> record;

  void SetUp() override
  {
    bmain = BKE_main_new();
    owner = static_cast<ID *>(BKE_id_new(bmain, ID_BR, "Owned curve test"));
    candidate = BKE_curvemapping_add(1, 0, 0, 1, 1);
    candidate->cm[0].curve[0].flag = candidate->cm[0].curve[1].flag = CUMA_HANDLE_VECTOR;
    definition = BKE_curvemapping_to_idprop(*candidate, "response", nullptr, error);
    ASSERT_NE(definition, nullptr) << error;
    IDP_AddToGroup(IDP_EnsureProperties(owner), definition);
    record = OwnedCurveRecord::acquire(*owner, *definition, error);
    ASSERT_NE(record, nullptr) << error;
  }
  void TearDown() override
  {
    BKE_curvemapping_free(candidate);
    BKE_main_free(bmain);
    record.reset();
    owned_curve_invalidate_all();
  }
  IDProperty *raw_y()
  {
    return IDP_GetPropertyFromGroup(
        IDP_property_array_get(IDP_GetPropertyFromGroup(definition, "points")) + 1, "y");
  }
};

TEST_F(OwnedCurveRuntimeTest, CommitAndImmutableCopies)
{
  auto before = record->snapshot(error);
  ASSERT_NE(before, nullptr);
  auto *copy = before->copy_mapping();
  copy->cm[0].curve[1].y = 0.5f;
  float value = -1;
  EXPECT_TRUE(before->evaluate(0.5f, value));
  EXPECT_NEAR(value, 0.5f, 1e-6f);
  EXPECT_EQ(record->commit(1, *copy, error), OwnedCurveResult::Changed) << error;
  EXPECT_EQ(record->revision(), 2);
  EXPECT_EQ(OwnedCurveRecord::acquire(*owner, *definition, error), record);
  auto after = record->snapshot(error);
  EXPECT_NE(after, before);
  EXPECT_TRUE(after->evaluate(0.5f, value));
  EXPECT_NEAR(value, 0.25f, 1e-6f);
  EXPECT_TRUE(before->evaluate(0.5f, value));
  EXPECT_NEAR(value, 0.5f, 1e-6f);
  EXPECT_EQ(record->commit(2, *copy, error), OwnedCurveResult::Unchanged);
  EXPECT_EQ(record->snapshot(error), after);
  EXPECT_EQ(record->commit(1, *candidate, error), OwnedCurveResult::Stale);
  EXPECT_EQ(record->snapshot(error), after);
  EXPECT_EQ(record->revision(), 2);
  BKE_curvemapping_free(copy);
}

TEST_F(OwnedCurveRuntimeTest, RejectedTransactionsAndExplicitSync)
{
  auto before = record->snapshot(error);
  definition->flag |= IDP_FLAG_STATIC_TYPE;
  IDProperty *saved = IDP_CopyProperty(definition);
  candidate->cm[0].curve[1].y = std::numeric_limits<float>::infinity();
  EXPECT_EQ(record->commit(1, *candidate, error), OwnedCurveResult::Invalid);
  EXPECT_TRUE(owned_curve_definition_equal(*saved, *definition));
  EXPECT_EQ(saved->flag, definition->flag);
  EXPECT_EQ(record->snapshot(error), before);
  EXPECT_EQ(record->revision(), 1);
  candidate->cm[0].curve[1].x = candidate->cm[0].curve[1].y = 0.0f;
  EXPECT_EQ(record->commit(1, *candidate, error), OwnedCurveResult::Invalid);
  EXPECT_TRUE(owned_curve_definition_equal(*saved, *definition));
  EXPECT_EQ(record->snapshot(error), before);
  EXPECT_EQ(record->revision(), 1);
  candidate->cm[0].curve[1].x = 1.0f;
  candidate->cm[0].curve[1].y = 0.75f;
  IDP_double_set(raw_y(), 0.5);
  EXPECT_EQ(record->snapshot(error), nullptr);
  EXPECT_EQ(record->commit(1, *candidate, error), OwnedCurveResult::Stale);
  EXPECT_EQ(IDP_double_get(raw_y()), 0.5);
  EXPECT_EQ(record->sync(error), OwnedCurveResult::Changed);
  auto synced = record->snapshot(error);
  EXPECT_NE(synced, before);
  EXPECT_EQ(record->commit(2, *candidate, error), OwnedCurveResult::Changed);
  EXPECT_EQ(record->revision(), 3);
  EXPECT_EQ(saved->flag, definition->flag);
  auto latest = record->snapshot(error);
  IDP_double_set(raw_y(), std::numeric_limits<double>::infinity());
  EXPECT_EQ(record->sync(error), OwnedCurveResult::Invalid);
  EXPECT_EQ(record->commit(3, *candidate, error), OwnedCurveResult::Stale);
  EXPECT_EQ(record->revision(), 3);
  EXPECT_EQ(IDP_double_get(raw_y()), std::numeric_limits<double>::infinity());
  IDP_double_set(raw_y(), 0.75);
  EXPECT_EQ(record->snapshot(error), latest);
  IDP_FreeProperty(saved);
}

TEST_F(OwnedCurveRuntimeTest, ReadOnlyAndEditableAssets)
{
  auto before = record->snapshot(error);
  Library *library = static_cast<Library *>(BKE_id_new(bmain, ID_LI, "Test library"));
  owner->lib = library;
  candidate->cm[0].curve[1].y = 0.5f;
  EXPECT_EQ(record->commit(1, *candidate, error), OwnedCurveResult::ReadOnly);
  IDP_double_set(raw_y(), 0.75);
  EXPECT_EQ(record->sync(error), OwnedCurveResult::ReadOnly);
  EXPECT_EQ(record->revision(), 1);
  IDP_double_set(raw_y(), 1.0);
  EXPECT_EQ(record->snapshot(error), before);
  library->runtime->tag |= LIBRARY_ASSET_EDITABLE;
  EXPECT_EQ(record->commit(1, *candidate, error), OwnedCurveResult::Changed) << error;
  owner->lib = nullptr;
  auto after = record->snapshot(error);
  IDOverrideLibrary override_data = {};
  override_data.reference = owner;
  owner->override_library = &override_data;
  EXPECT_EQ(record->commit(2, *candidate, error), OwnedCurveResult::ReadOnly);
  EXPECT_EQ(record->sync(error), OwnedCurveResult::ReadOnly);
  override_data.flag |= LIBOVERRIDE_FLAG_SYSTEM_DEFINED;
  EXPECT_EQ(record->sync(error), OwnedCurveResult::ReadOnly);
  EXPECT_EQ(record->snapshot(error), after);
  EXPECT_EQ(record->revision(), 2);
  owner->override_library = nullptr;
}

TEST_F(OwnedCurveRuntimeTest, DetachReattachAndAncestorReplacement)
{
  auto before = record->snapshot(error);
  IDP_RemoveFromGroup(owner->properties, definition);
  IDP_AddToGroup(owner->properties, definition);
  EXPECT_EQ(record->snapshot(error), nullptr);
  auto next = OwnedCurveRecord::acquire(*owner, *definition, error);
  ASSERT_NE(next, nullptr);
  EXPECT_NE(next, record);
  IDProperty *root_copy = IDP_CopyProperty(owner->properties);
  IDP_CopyPropertyContent(owner->properties, root_copy);
  EXPECT_EQ(next->snapshot(error), nullptr);
  IDP_FreeProperty(root_copy);
  float value = -1;
  EXPECT_TRUE(before->evaluate(0.5f, value));
  EXPECT_NEAR(value, 0.5f, 1e-6f);
}

TEST_F(OwnedCurveRuntimeTest, SystemCollectionGrowthMoveAndDeletion)
{
  IDP_RemoveFromGroup(owner->properties, definition);
  owner->system_properties = idprop::create_group("system").release();
  IDProperty *array = IDP_NewIDPArray("layers");
  IDP_AddToGroup(owner->system_properties, array);
  IDP_ResizeIDPArray(array, 1);
  IDProperty *item = idprop::create_group("").release();
  IDP_AddToGroup(item, definition);
  IDP_SetIndexArray(array, 0, item);
  MEM_delete(item);
  record = OwnedCurveRecord::acquire(*owner, *definition, error);
  ASSERT_NE(record, nullptr) << error;
  auto before = record->snapshot(error);
  IDP_ResizeIDPArray(array, 300);
  std::swap(IDP_property_array_get(array)[0], IDP_property_array_get(array)[299]);
  EXPECT_EQ(record->snapshot(error), before);
  EXPECT_EQ(OwnedCurveRecord::acquire(*owner, IDP_property_array_get(array)[299], error), nullptr);
  IDP_RemoveFromGroup(owner->system_properties, array);
  IDP_AddToGroup(owner->system_properties, array);
  EXPECT_EQ(record->snapshot(error), nullptr);
  record = OwnedCurveRecord::acquire(*owner, *definition, error);
  ASSERT_NE(record, nullptr);
  IDP_ResizeIDPArray(array, 299);
  EXPECT_EQ(record->snapshot(error), nullptr);
  float value;
  EXPECT_TRUE(before->evaluate(0.5f, value));
}

TEST_F(OwnedCurveRuntimeTest, OwnerCopySwapDeleteAndUnregisteredWorkerFree)
{
  auto before = record->snapshot(error);
  ID *other = static_cast<ID *>(BKE_id_new(bmain, ID_BR, "Other"));
  other->properties = IDP_CopyProperty(owner->properties);
  IDProperty *other_definition = IDP_GetPropertyFromGroup(other->properties, "response");
  auto other_record = OwnedCurveRecord::acquire(*other, *other_definition, error);
  ASSERT_NE(other_record, nullptr);
  EXPECT_NE(other_record->snapshot(error), before);
  EXPECT_EQ(OwnedCurveRecord::acquire(*other, *definition, error), nullptr);
  BKE_lib_id_swap(bmain, owner, other, false, 0);
  EXPECT_EQ(record->snapshot(error), nullptr);
  EXPECT_EQ(other_record->snapshot(error), nullptr);
  auto next = OwnedCurveRecord::acquire(*other, *definition, error);
  ASSERT_NE(next, nullptr);
  IDProperty *temporary = IDP_CopyProperty(definition);
  std::thread worker([&]() {
    float value;
    EXPECT_TRUE(before->evaluate(0.5f, value));
    IDP_FreeProperty(temporary);
  });
  worker.join();
  BKE_id_free(bmain, other);
  EXPECT_EQ(next->snapshot(error), nullptr);
  float value;
  EXPECT_TRUE(before->evaluate(0.5f, value));
  /* Drain records with no external handles: canonical frees must not reenter map mutation. */
  definition = IDP_GetPropertyFromGroup(owner->properties, "response");
  OwnedCurveRecord::acquire(*owner, *definition, error).reset();
  owned_curve_invalidate_all();
}

TEST_F(OwnedCurveRuntimeTest, ExactDataComparisonAndPresentation)
{
  IDP_AddToGroup(definition,
                 idprop::create("opaque", std::numeric_limits<double>::quiet_NaN()).release());
  EXPECT_EQ(record->sync(error), OwnedCurveResult::Changed);
  auto before = record->snapshot(error);
  IDProperty *copy = IDP_CopyProperty(definition);
  EXPECT_TRUE(owned_curve_definition_equal(*copy, *definition));
  IDProperty *point = IDP_property_array_get(IDP_GetPropertyFromGroup(copy, "points"));
  IDProperty *x = IDP_GetPropertyFromGroup(point, "x");
  BLI_remlink(&point->data.group, x);
  BLI_addtail(&point->data.group, x);
  EXPECT_TRUE(owned_curve_definition_equal(*copy, *definition));
  definition->flag |= IDP_FLAG_STATIC_TYPE;
  EXPECT_EQ(record->snapshot(error), before);
  EXPECT_EQ(record->commit(record->revision(), *candidate, error), OwnedCurveResult::Unchanged);
  EXPECT_EQ(record->snapshot(error), before);
  IDP_double_set(x, -0.0);
  EXPECT_FALSE(owned_curve_definition_equal(*copy, *definition));
  IDP_double_set(x, 0.0);
  auto *opaque = IDP_GetPropertyFromGroup(copy, "opaque");
  uint64_t bits;
  memcpy(&bits, &opaque->data.val, sizeof(bits));
  bits ^= 1;
  memcpy(&opaque->data.val, &bits, sizeof(bits));
  EXPECT_FALSE(owned_curve_definition_equal(*copy, *definition));
  bits ^= 1;
  memcpy(&opaque->data.val, &bits, sizeof(bits));
  EXPECT_TRUE(owned_curve_definition_equal(*copy, *definition));
  candidate->cm[0].curve[1].y = 0.5f;
  EXPECT_EQ(record->commit(record->revision(), *candidate, error), OwnedCurveResult::Changed);
  EXPECT_TRUE(owned_curve_definition_equal(*IDP_GetPropertyFromGroup(copy, "opaque"),
                                           *IDP_GetPropertyFromGroup(definition, "opaque")));
  IDP_FreeProperty(copy);
  IDProperty *bytes_a = IDP_NewString("abx", "bytes");
  IDProperty *bytes_b = IDP_NewString("aby", "bytes");
  IDP_string_get(bytes_a)[1] = IDP_string_get(bytes_b)[1] = '\0';
  bytes_a->subtype = bytes_b->subtype = IDP_STRING_SUB_BYTE;
  EXPECT_FALSE(owned_curve_definition_equal(*bytes_a, *bytes_b));
  IDP_FreeProperty(bytes_b);
  bytes_b = IDP_CopyProperty(bytes_a);
  bytes_b->subtype = IDP_STRING_SUB_UTF8;
  EXPECT_FALSE(owned_curve_definition_equal(*bytes_a, *bytes_b));
  IDP_FreeProperty(bytes_a);
  IDP_FreeProperty(bytes_b);
}

TEST_F(OwnedCurveRuntimeTest, GuardedEvaluation)
{
  candidate->flag = eCurveMappingFlags(0);
  EXPECT_EQ(record->commit(1, *candidate, error), OwnedCurveResult::Changed);
  auto snapshot = record->snapshot(error);
  float value = 123;
  EXPECT_FALSE(snapshot->evaluate(std::numeric_limits<float>::quiet_NaN(), value));
  EXPECT_FALSE(snapshot->evaluate(std::numeric_limits<float>::infinity(), value));
  EXPECT_FALSE(snapshot->evaluate(-std::numeric_limits<float>::infinity(), value));
  EXPECT_EQ(value, 123);
  EXPECT_TRUE(snapshot->evaluate(std::numeric_limits<float>::max(), value));
  EXPECT_NEAR(value, 1, 1e-6f);
  EXPECT_TRUE(snapshot->evaluate(-std::numeric_limits<float>::max(), value));
  EXPECT_EQ(value, 0);
  candidate->flag = CUMA_EXTEND_EXTRAPOLATE;
  EXPECT_EQ(record->commit(2, *candidate, error), OwnedCurveResult::Changed);
  snapshot = record->snapshot(error);
  EXPECT_TRUE(snapshot->evaluate(2.0f, value));
  EXPECT_NEAR(value, 2.0f, 1e-6f);
}

TEST_F(OwnedCurveRuntimeTest, PathWatchGroupMutationAndLifetime)
{
  auto root_watch = OwnedCurvePathWatch::acquire(*owner, *owner->properties);
  EXPECT_EQ(root_watch, OwnedCurvePathWatch::acquire(*owner, *owner->properties));
  auto definition_watch = OwnedCurvePathWatch::acquire(*owner, *definition);
  IDProperty *extra = IDP_NewString("extension", "extra");
  IDP_AddToGroup(owner->properties, extra);
  EXPECT_FALSE(root_watch->is_valid());
  EXPECT_TRUE(definition_watch->is_valid());
  EXPECT_TRUE(record->is_attached());
  auto replacement_watch = OwnedCurvePathWatch::acquire(*owner, *owner->properties);
  EXPECT_NE(root_watch, replacement_watch);
  IDP_ReplaceInGroup(owner->properties, IDP_NewString("replacement", "extra"));
  EXPECT_FALSE(replacement_watch->is_valid());
  auto removal_watch = OwnedCurvePathWatch::acquire(*owner, *owner->properties);
  IDP_FreeFromGroup(owner->properties, IDP_GetPropertyFromGroup(owner->properties, "extra"));
  EXPECT_FALSE(removal_watch->is_valid());
  EXPECT_TRUE(definition_watch->is_valid());
  owned_curve_invalidate_owner(owner);
  EXPECT_FALSE(definition_watch->is_valid());
  EXPECT_FALSE(record->is_attached());
}

TEST_F(OwnedCurveRuntimeTest, PathWatchArrayIdentityIsIndependentOfCurveRecords)
{
  IDProperty *array = IDP_NewIDPArray("items");
  IDP_AddToGroup(owner->properties, array);
  IDP_ResizeIDPArray(array, 2);
  for (int i = 0; i < 2; i++) {
    auto item = idprop::create_group("");
    IDP_SetIndexArray(array, i, item.get());
    MEM_delete(item.release());
  }
  auto watch = OwnedCurvePathWatch::acquire(*owner, *array);
  auto item_watch = OwnedCurvePathWatch::acquire(*owner, IDP_property_array_get(array)[0]);
  IDP_ResizeIDPArray(array, 2);
  EXPECT_TRUE(watch->is_valid());
  IDP_ResizeIDPArray(array, 3);
  EXPECT_FALSE(watch->is_valid());
  EXPECT_TRUE(item_watch->is_valid());
  watch = OwnedCurvePathWatch::acquire(*owner, *array);
  auto replacement = idprop::create_group("");
  IDP_SetIndexArray(array, 0, replacement.get());
  MEM_delete(replacement.release());
  EXPECT_FALSE(watch->is_valid());
  watch = OwnedCurvePathWatch::acquire(*owner, *array);
  /* The RNA move route calls this before moving identical inline items. */
  owned_curve_structure_changed(array);
  EXPECT_FALSE(watch->is_valid());
  EXPECT_TRUE(record->is_attached());
  EXPECT_NE(record->snapshot(error), nullptr);
  watch = OwnedCurvePathWatch::acquire(*owner, *array);
  IDP_FreeFromGroup(owner->properties, array);
  EXPECT_FALSE(watch->is_valid());
}

TEST_F(OwnedCurveRuntimeTest, PathWatchContentReplacementAndUnrelatedTemporaryData)
{
  auto watch = OwnedCurvePathWatch::acquire(*owner, *definition);
  IDProperty *copy = IDP_CopyProperty(definition);
  IDP_FreeProperty(copy);
  EXPECT_TRUE(watch->is_valid());
  copy = IDP_CopyProperty(definition);
  IDP_CopyPropertyContent(definition, copy);
  IDP_FreeProperty(copy);
  EXPECT_FALSE(watch->is_valid());
  EXPECT_FALSE(record->is_attached());
  watch = OwnedCurvePathWatch::acquire(*owner, *definition);
  auto owner_watch = OwnedCurvePathWatch::acquire_owner(*owner);
  owned_curve_invalidate_all();
  EXPECT_FALSE(watch->is_valid());
  EXPECT_FALSE(owner_watch->is_valid());
}

TEST_F(OwnedCurveRuntimeTest, ArraySlotReplacementPreservesUntouchedWatches)
{
  IDProperty *array = IDP_NewIDPArray("slot_test");
  IDP_AddToGroup(owner->properties, array);
  IDP_ResizeIDPArray(array, 2);
  for (int i = 0; i < 2; i++) {
    auto item = idprop::create_group("");
    IDP_AddToGroup(item.get(), idprop::create_group("nested").release());
    IDP_SetIndexArray(array, i, item.get());
    MEM_delete(item.release());
  }
  auto *items = IDP_property_array_get(array);
  auto container = OwnedCurvePathWatch::acquire(*owner, *array);
  auto target = OwnedCurvePathWatch::acquire(*owner, items[0]);
  auto nested = OwnedCurvePathWatch::acquire(*owner,
                                             *IDP_GetPropertyFromGroup(&items[0], "nested"));
  auto sibling = OwnedCurvePathWatch::acquire(*owner, items[1]);
  auto sibling_nested = OwnedCurvePathWatch::acquire(
      *owner, *IDP_GetPropertyFromGroup(&items[1], "nested"));
  IDProperty *identical = IDP_CopyProperty(&items[0]);
  IDP_SetIndexArray(array, 0, identical);
  MEM_delete(identical);
  EXPECT_FALSE(container->is_valid());
  EXPECT_FALSE(target->is_valid());
  EXPECT_FALSE(nested->is_valid());
  EXPECT_TRUE(sibling->is_valid());
  EXPECT_TRUE(sibling_nested->is_valid());
  EXPECT_TRUE(record->is_attached());
}

TEST_F(OwnedCurveRuntimeTest, ArrayResizeInvalidatesOnlyMovedOrRemovedItems)
{
  IDProperty *array = IDP_NewIDPArray("resize_test");
  IDP_AddToGroup(owner->properties, array);
  IDP_ResizeIDPArray(array, 300);
  for (int i = 0; i < array->len; i++) {
    auto item = idprop::create_group("");
    IDP_AddToGroup(item.get(), idprop::create_group("nested").release());
    IDP_SetIndexArray(array, i, item.get());
    MEM_delete(item.release());
  }
  auto *items = IDP_property_array_get(array);
  auto container = OwnedCurvePathWatch::acquire(*owner, *array);
  auto retained = OwnedCurvePathWatch::acquire(*owner, items[0]);
  auto retained_nested = OwnedCurvePathWatch::acquire(
      *owner, *IDP_GetPropertyFromGroup(&items[0], "nested"));
  auto removed = OwnedCurvePathWatch::acquire(*owner, items[299]);
  auto removed_nested = OwnedCurvePathWatch::acquire(
      *owner, *IDP_GetPropertyFromGroup(&items[299], "nested"));
  IDP_ResizeIDPArray(array, 299);
  EXPECT_EQ(items, IDP_property_array_get(array));
  EXPECT_FALSE(container->is_valid());
  EXPECT_TRUE(retained->is_valid());
  EXPECT_TRUE(retained_nested->is_valid());
  EXPECT_FALSE(removed->is_valid());
  EXPECT_FALSE(removed_nested->is_valid());
  const int smaller = array->totallen - 200;
  ASSERT_GT(smaller, 0);
  ASSERT_LT(smaller, array->len);
  IDP_ResizeIDPArray(array, smaller);
  EXPECT_FALSE(retained->is_valid());
  EXPECT_FALSE(retained_nested->is_valid());
  items = IDP_property_array_get(array);
  retained = OwnedCurvePathWatch::acquire(*owner, items[0]);
  retained_nested = OwnedCurvePathWatch::acquire(*owner,
                                                 *IDP_GetPropertyFromGroup(&items[0], "nested"));
  IDP_ResizeIDPArray(array, array->totallen + 1);
  EXPECT_FALSE(retained->is_valid());
  EXPECT_FALSE(retained_nested->is_valid());
  EXPECT_TRUE(record->is_attached());
}

TEST_F(OwnedCurveRuntimeTest, LargeEncodingWithUnrelatedLivePathWatch)
{
  auto watch = OwnedCurvePathWatch::acquire(*owner, *definition);
  for (const int count : {2048, 8192, 32767}) {
    auto &curve = candidate->cm[0];
    MEM_delete(curve.curve);
    curve.curve = MEM_new_array<CurveMapPoint>(count, "large scalar curve");
    curve.totpoint = count;
    for (int i = 0; i < count; i++) {
      curve.curve[i].x = curve.curve[i].y = float(i) / float(count - 1);
      curve.curve[i].flag = CUMA_HANDLE_VECTOR;
    }
    const auto start = std::chrono::steady_clock::now();
    IDProperty *encoded = BKE_curvemapping_to_idprop(*candidate, "large", nullptr, error);
    ASSERT_NE(encoded, nullptr) << error;
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start);
    printf("OWNED_CURVE_ENCODE_TIMING %d %.6f seconds\n", count, elapsed.count());
    EXPECT_EQ(IDP_GetPropertyFromGroup(encoded, "points")->len, count);
    EXPECT_TRUE(watch->is_valid());
    IDP_FreeProperty(encoded);
  }
}

}  // namespace blender::bke::tests
