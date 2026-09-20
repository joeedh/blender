/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <limits>

#include "DNA_color_types.h"

#include "BKE_colortools.hh"
#include "BKE_curvemapping_idprop.hh"
#include "BKE_gtest_base.hh"
#include "BKE_idprop.hh"

#include "testing/testing.h"

namespace blender::bke::tests {

class OwnedCurveDefinitionTest : public BlenderGTestBase {};

TEST_F(OwnedCurveDefinitionTest, RoundtripEvaluationAndIndependentCopy)
{
  CurveMapping *source = BKE_curvemapping_add(1, 0, 0, 1, 1);
  source->cm[0].curve[1].y = 0.375f;
  source->cm[0].curve[0].flag = source->cm[0].curve[1].flag = CUMA_HANDLE_VECTOR;
  BKE_curvemapping_init(source);
  std::string error;
  IDProperty *definition = BKE_curvemapping_to_idprop(*source, "response", nullptr, error);
  ASSERT_NE(definition, nullptr) << error;
  IDP_AddToGroup(definition, IDP_NewString("retained", "future_key"));
  CurveMapping *decoded = BKE_curvemapping_from_idprop(*definition, error);
  ASSERT_NE(decoded, nullptr) << error;
  EXPECT_NE(decoded->cm[0].curve, source->cm[0].curve);
  EXPECT_NEAR(BKE_curvemapping_evaluateF(decoded, 0, 0.5f), 0.1875f, 1e-6f);
  decoded->cm[0].curve[1].y = 0.75f;
  EXPECT_EQ(source->cm[0].curve[1].y, 0.375f);
  IDProperty *updated = BKE_curvemapping_to_idprop(*decoded, "renamed", definition, error);
  ASSERT_NE(updated, nullptr) << error;
  EXPECT_STREQ(updated->name, "renamed");
  EXPECT_STREQ(IDP_string_get(IDP_GetPropertyFromGroup(updated, "future_key")), "retained");
  const IDProperty *points = IDP_GetPropertyFromGroup(definition, "points");
  EXPECT_EQ(IDP_double_get(IDP_GetPropertyFromGroup(IDP_property_array_get(points) + 1, "y")),
            0.375);
  IDP_FreeProperty(updated);
  IDP_FreeProperty(definition);
  BKE_curvemapping_free(decoded);
  BKE_curvemapping_free(source);
}

TEST_F(OwnedCurveDefinitionTest, MalformedDataIsNotChanged)
{
  CurveMapping *source = BKE_curvemapping_add(1, 0, 0, 1, 1);
  std::string error;
  IDProperty *definition = BKE_curvemapping_to_idprop(*source, "response", nullptr, error);
  ASSERT_NE(definition, nullptr) << error;
  IDProperty *version = IDP_GetPropertyFromGroup(definition, "_version");
  IDP_int_set(version, 99);
  EXPECT_EQ(BKE_curvemapping_from_idprop(*definition, error), nullptr);
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(IDP_int_get(version), 99);
  IDP_int_set(version, 1);
  IDProperty *points = IDP_GetPropertyFromGroup(definition, "points");
  IDProperty *y = IDP_GetPropertyFromGroup(IDP_property_array_get(points) + 1, "y");
  IDP_double_set(y, std::numeric_limits<double>::infinity());
  EXPECT_FALSE(BKE_curvemapping_idprop_validate(*definition, error));
  EXPECT_EQ(IDP_double_get(y), std::numeric_limits<double>::infinity());
  IDP_double_set(y, 0.1);
  EXPECT_FALSE(BKE_curvemapping_idprop_validate(*definition, error));
  IDP_double_set(y, double(0.1f));
  EXPECT_TRUE(BKE_curvemapping_idprop_validate(*definition, error)) << error;
  IDP_FreeProperty(definition);
  BKE_curvemapping_free(source);
}

TEST_F(OwnedCurveDefinitionTest, PreserveHandlesClippingAndExtrapolation)
{
  for (const eCurveMapPoint_Flag handle :
       {eCurveMapPoint_Flag(0), CUMA_HANDLE_VECTOR, CUMA_HANDLE_AUTO_ANIM})
  {
    for (const bool clip : {false, true}) {
      for (const bool extrapolate : {false, true}) {
        CurveMapping *source = BKE_curvemapping_add(1, -1, -2, 2, 3);
        BKE_curvemap_insert(&source->cm[0], 0.0f, 0.375f);
        BKE_curvemap_insert(&source->cm[0], 0.5f, 0.25f);
        source->flag = eCurveMappingFlags((clip ? CUMA_DO_CLIP : 0) |
                                          (extrapolate ? CUMA_EXTEND_EXTRAPOLATE : 0));
        source->cm[0].default_handle_type = handle;
        for (int i = 0; i < source->cm[0].totpoint; i++) {
          source->cm[0].curve[i].flag = handle;
        }
        BKE_curvemapping_init(source);
        std::string error;
        IDProperty *definition = BKE_curvemapping_to_idprop(*source, "response", nullptr, error);
        ASSERT_NE(definition, nullptr) << error;
        CurveMapping *decoded = BKE_curvemapping_from_idprop(*definition, error);
        ASSERT_NE(decoded, nullptr) << error;
        EXPECT_EQ(decoded->flag, source->flag);
        EXPECT_EQ(decoded->cm[0].default_handle_type, handle);
        EXPECT_EQ(decoded->clipr.xmin, -1.0f);
        EXPECT_EQ(decoded->clipr.ymax, 3.0f);
        for (int i = 0; i <= 128; i++) {
          const float x = -2.0f + i / 32.0f;
          EXPECT_FLOAT_EQ(BKE_curvemapping_evaluateF(decoded, 0, x),
                          BKE_curvemapping_evaluateF(source, 0, x));
        }
        BKE_curvemapping_free(decoded);
        IDP_FreeProperty(definition);
        BKE_curvemapping_free(source);
      }
    }
  }
}

TEST_F(OwnedCurveDefinitionTest, RejectOtherCurveKindsAndUnorderedPoints)
{
  std::string error;
  CurveMapping *rgb = BKE_curvemapping_add(4, 0, 0, 1, 1);
  EXPECT_EQ(BKE_curvemapping_to_idprop(*rgb, "response", nullptr, error), nullptr);
  BKE_curvemapping_free(rgb);
  CurveMapping *scalar = BKE_curvemapping_add(1, 0, 0, 1, 1);
  scalar->cm[0].curve[1].x = -1;
  EXPECT_EQ(BKE_curvemapping_to_idprop(*scalar, "response", nullptr, error), nullptr);
  BKE_curvemapping_free(scalar);
}

TEST_F(OwnedCurveDefinitionTest, RejectOpaquePointFieldsAndNestedIDReferences)
{
  std::string error;
  CurveMapping *source = BKE_curvemapping_add(1, 0, 0, 1, 1);
  IDProperty *definition = BKE_curvemapping_to_idprop(*source, "response", nullptr, error);
  ASSERT_NE(definition, nullptr) << error;
  IDProperty *points = IDP_GetPropertyFromGroup(definition, "points");
  IDProperty *point = IDP_property_array_get(points);
  IDProperty *extra = IDP_NewString("do not discard", "future_key");
  IDP_AddToGroup(point, extra);
  EXPECT_FALSE(BKE_curvemapping_idprop_validate(*definition, error));
  EXPECT_EQ(BKE_curvemapping_to_idprop(*source, "response", definition, error), nullptr);
  EXPECT_STREQ(IDP_string_get(extra), "do not discard");
  IDP_FreeFromGroup(point, extra);
  IDProperty *nested = idprop::create_group("future_extension").release();
  IDP_AddToGroup(nested, idprop::create("owner", static_cast<ID *>(nullptr)).release());
  IDP_AddToGroup(definition, nested);
  EXPECT_FALSE(BKE_curvemapping_idprop_validate(*definition, error));
  EXPECT_EQ(IDP_GetPropertyFromGroup(nested, "owner")->type, IDP_ID);
  IDP_FreeProperty(definition);
  BKE_curvemapping_free(source);
}

TEST_F(OwnedCurveDefinitionTest, RejectRGBLevelsAndNonFiniteEvaluation)
{
  std::string error;
  CurveMapping *source = BKE_curvemapping_add(1, 0, 0, 1, 1);
  source->tone = CURVE_TONE_FILMLIKE;
  EXPECT_EQ(BKE_curvemapping_to_idprop(*source, "response", nullptr, error), nullptr);
  source->tone = CURVE_TONE_STANDARD;
  source->black[1] = 0.25f;
  EXPECT_EQ(BKE_curvemapping_to_idprop(*source, "response", nullptr, error), nullptr);
  source->black[1] = 0.0f;
  source->white[2] = std::numeric_limits<float>::quiet_NaN();
  EXPECT_EQ(BKE_curvemapping_to_idprop(*source, "response", nullptr, error), nullptr);
  source->white[2] = 1.0f;
  source->cm[0].curve[0].flag = source->cm[0].curve[1].flag = CUMA_HANDLE_VECTOR;
  source->cm[0].curve[1].x = source->cm[0].curve[1].y = 0.0f;
  EXPECT_EQ(BKE_curvemapping_to_idprop(*source, "response", nullptr, error), nullptr);
  source->cm[0].curve[0].flag = source->cm[0].curve[1].flag = {};
  source->cm[0].curve[0].x = -std::numeric_limits<float>::max();
  source->cm[0].curve[1].x = std::numeric_limits<float>::max();
  source->cm[0].curve[1].y = 1.0f;
  EXPECT_EQ(BKE_curvemapping_to_idprop(*source, "response", nullptr, error), nullptr);
  BKE_curvemapping_free(source);
}

}  // namespace blender::bke::tests
