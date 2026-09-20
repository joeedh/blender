/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 */

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <unordered_map>

#include "BKE_curvemapping_idprop.hh"
#include "BKE_idprop.hh"
#include "BKE_report.hh"
#include "RNA_define.hh"
#include "WM_api.hh"
#include "WM_types.hh"

#include "BKE_colortools.hh"
#include "BKE_context.hh"
#include "BKE_library.hh"

#include "BLI_bounds.hh"
#include "BLI_math_base_c.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_rect.hh"
#include "BLI_string_ref.hh"

#include "BLT_translation.hh"

#include "ED_screen.hh"
#include "ED_undo.hh"

#include "BKE_lib_id.hh"
#include "BKE_undo_system.hh"
#include "ED_authoring_undo.hh"
#include "ED_undo.hh"
#include "RNA_access.hh"
#include "RNA_owned_curve.hh"
#include "ED_authoring_undo.hh"
#include "ED_undo.hh"
#include "BKE_lib_id.hh"
#include "BKE_undo_system.hh"
#include "RNA_prototypes.hh"

#include "UI_interface_layout.hh"
#include "interface_intern.hh"
#include "interface_templates_intern.hh"

namespace blender::ui {

using blender::Vector;

namespace {
struct CurveRuntimeProperties {
  CurveMapPoint *last_pt = nullptr;
  float2 last_pos;
};
}  // namespace

static bool curvemap_can_zoom_out(CurveMapping *cumap)
{
  return (cumap->flag & CUMA_DO_CLIP) == 0 ||
         (BLI_rctf_size_x(&cumap->curr) < BLI_rctf_size_x(&cumap->clipr));
}

static bool curvemap_can_zoom_in(CurveMapping *cumap)
{
  return (cumap->flag & CUMA_DO_CLIP) == 0 ||
         (BLI_rctf_size_x(&cumap->curr) > CURVE_ZOOM_MAX * BLI_rctf_size_x(&cumap->clipr));
}

static void curvemap_zoom(CurveMapping &cumap, const float scale)
{
  const Bounds<float2> curr_bounds(float2(cumap.curr.xmin, cumap.curr.ymin),
                                   float2(cumap.curr.xmax, cumap.curr.ymax));
  const float2 offset = curr_bounds.size() * 0.5f * (scale - 1.0f);
  const Bounds<float2> new_bounds(curr_bounds.min - offset, curr_bounds.max + offset);

  Bounds<float2> clamped_bounds = new_bounds;
  /* Clamp to clip bounds if enabled, snap if the difference is small. */
  if (cumap.flag & CUMA_DO_CLIP) {
    const Bounds<float2> clip_bounds(float2(cumap.clipr.xmin, cumap.clipr.ymin),
                                     float2(cumap.clipr.xmax, cumap.clipr.ymax));
    const float2 threshold = 0.01f * clip_bounds.size();
    if (clamped_bounds.min.x < clip_bounds.min.x + threshold.x) {
      clamped_bounds.min.x = clip_bounds.min.x;
    }
    if (clamped_bounds.min.y < clip_bounds.min.y + threshold.y) {
      clamped_bounds.min.y = clip_bounds.min.y;
    }
    if (clamped_bounds.max.x > clip_bounds.max.x - threshold.x) {
      clamped_bounds.max.x = clip_bounds.max.x;
    }
    if (clamped_bounds.max.y > clip_bounds.max.y - threshold.y) {
      clamped_bounds.max.y = clip_bounds.max.y;
    }
  }
  cumap.curr.xmin = clamped_bounds.min.x;
  cumap.curr.ymin = clamped_bounds.min.y;
  cumap.curr.xmax = clamped_bounds.max.x;
  cumap.curr.ymax = clamped_bounds.max.y;
}

static void curvemap_buttons_zoom_in(bContext *C, CurveMapping *cumap)
{
  if (!curvemap_can_zoom_in(cumap)) {
    return;
  }

  curvemap_zoom(*cumap, 0.7692f);

  ED_region_tag_redraw(CTX_wm_region(C));
}

static void curvemap_buttons_zoom_out(bContext *C, CurveMapping *cumap)
{
  if (!curvemap_can_zoom_out(cumap)) {
    return;
  }

  curvemap_zoom(*cumap, 1.3f);

  ED_region_tag_redraw(CTX_wm_region(C));
}

/* NOTE: this is a block-menu, needs 0 events, otherwise the menu closes */
static Block *curvemap_clipping_func(bContext *C, ARegion *region, void *cumap_v)
{
  CurveMapping *cumap = static_cast<CurveMapping *>(cumap_v);
  Button *bt;
  const float width = 8 * UI_UNIT_X;

  Block *block = block_begin(C, region, __func__, EmbossType::Emboss);
  block_flag_enable(block, BLOCK_KEEP_OPEN | BLOCK_MOVEMOUSE_QUIT);
  block_theme_style_set(block, BLOCK_THEME_STYLE_POPUP);

  bt = uiDefButBit(block,
                   ButtonType::Checkbox,
                   CUMA_DO_CLIP,
                   IFACE_("Clipping"),
                   0,
                   5 * UI_UNIT_Y,
                   width,
                   UI_UNIT_Y,
                   &cumap->flag,
                   0.0,
                   0.0,
                   "");
  button_func_set(bt, [cumap](bContext & /*C*/) { BKE_curvemapping_changed(cumap, false); });

  block_align_begin(block);
  bt = uiDefButV(block,
                 ButtonType::Num,
                 IFACE_("Min X:"),
                 0,
                 4 * UI_UNIT_Y,
                 width,
                 UI_UNIT_Y,
                 &cumap->clipr.xmin,
                 -100.0,
                 cumap->clipr.xmax,
                 "");
  button_number_step_size_set(bt, 10);
  button_number_precision_set(bt, 2);
  bt = uiDefButV(block,
                 ButtonType::Num,
                 IFACE_("Min Y:"),
                 0,
                 3 * UI_UNIT_Y,
                 width,
                 UI_UNIT_Y,
                 &cumap->clipr.ymin,
                 -100.0,
                 cumap->clipr.ymax,
                 "");
  button_number_step_size_set(bt, 10);
  button_number_precision_set(bt, 2);
  bt = uiDefButV(block,
                 ButtonType::Num,
                 IFACE_("Max X:"),
                 0,
                 2 * UI_UNIT_Y,
                 width,
                 UI_UNIT_Y,
                 &cumap->clipr.xmax,
                 cumap->clipr.xmin,
                 100.0,
                 "");
  button_number_step_size_set(bt, 10);
  button_number_precision_set(bt, 2);
  bt = uiDefButV(block,
                 ButtonType::Num,
                 IFACE_("Max Y:"),
                 0,
                 UI_UNIT_Y,
                 width,
                 UI_UNIT_Y,
                 &cumap->clipr.ymax,
                 cumap->clipr.ymin,
                 100.0,
                 "");
  button_number_step_size_set(bt, 10);
  button_number_precision_set(bt, 2);

  block_bounds_set_normal(block, 0.3f * U.widget_unit);
  block_direction_set(block, UI_DIR_DOWN);

  return block;
}

static Block *curvemap_tools_func(
    bContext *C, ARegion *region, RNAUpdateCb &cb, bool show_extend, CurveMapSlopeType reset_mode)
{
  PointerRNA cumap_ptr = RNA_property_pointer_get(&cb.ptr, cb.prop);
  CurveMapping *cumap = static_cast<CurveMapping *>(cumap_ptr.data);

  short yco = 0;
  const short menuwidth = 10 * UI_UNIT_X;

  Block *block = block_begin(C, region, __func__, EmbossType::Emboss);

  {
    Button *but = uiDefIconTextBut(block,
                                   ButtonType::ButMenu,
                                   ICON_BLANK1,
                                   IFACE_("Reset View"),
                                   0,
                                   yco -= UI_UNIT_Y,
                                   menuwidth,
                                   UI_UNIT_Y,
                                   nullptr,
                                   "");
    button_func_set(but, [cumap](bContext &C) {
      BKE_curvemapping_reset_view(cumap);
      ED_region_tag_redraw(CTX_wm_region(&C));
    });
  }

  if (show_extend && !(cumap->flag & CUMA_USE_WRAPPING)) {
    {
      Button *but = uiDefIconTextBut(block,
                                     ButtonType::ButMenu,
                                     ICON_BLANK1,
                                     IFACE_("Extend Horizontal"),
                                     0,
                                     yco -= UI_UNIT_Y,
                                     menuwidth,
                                     UI_UNIT_Y,
                                     nullptr,
                                     "");
      button_func_set(but, [cumap, cb](bContext &C) {
        cumap->flag &= ~CUMA_EXTEND_EXTRAPOLATE;
        BKE_curvemapping_changed(cumap, false);
        rna_update_cb(C, cb);
        ED_undo_push(&C, "CurveMap tools");
        ED_region_tag_redraw(CTX_wm_region(&C));
      });
    }
    {
      Button *but = uiDefIconTextBut(block,
                                     ButtonType::ButMenu,
                                     ICON_BLANK1,
                                     IFACE_("Extend Extrapolated"),
                                     0,
                                     yco -= UI_UNIT_Y,
                                     menuwidth,
                                     UI_UNIT_Y,
                                     nullptr,
                                     "");
      button_func_set(but, [cumap, cb](bContext &C) {
        cumap->flag |= CUMA_EXTEND_EXTRAPOLATE;
        BKE_curvemapping_changed(cumap, false);
        rna_update_cb(C, cb);
        ED_undo_push(&C, "CurveMap tools");
        ED_region_tag_redraw(CTX_wm_region(&C));
      });
    }
  }

  {
    Button *but = uiDefIconTextBut(block,
                                   ButtonType::ButMenu,
                                   ICON_BLANK1,
                                   IFACE_("Reset Curve"),
                                   0,
                                   yco -= UI_UNIT_Y,
                                   menuwidth,
                                   UI_UNIT_Y,
                                   nullptr,
                                   "");
    button_func_set(but, [cumap, cb, reset_mode](bContext &C) {
      CurveMap *cuma = cumap->cm + cumap->cur;
      BKE_curvemap_reset(cuma, &cumap->clipr, cumap->preset, reset_mode);
      BKE_curvemapping_changed(cumap, false);
      rna_update_cb(C, cb);
      ED_undo_push(&C, "CurveMap tools");
      ED_region_tag_redraw(CTX_wm_region(&C));
    });
  }

  block_direction_set(block, UI_DIR_DOWN);
  block_bounds_set_text(block, 3.0f * UI_UNIT_X);

  return block;
}

static Block *curvemap_tools_posslope_func(bContext *C, ARegion *region, void *cb_v)
{
  return curvemap_tools_func(
      C, region, *static_cast<RNAUpdateCb *>(cb_v), true, CurveMapSlopeType::Positive);
}

static Block *curvemap_tools_negslope_func(bContext *C, ARegion *region, void *cb_v)
{
  return curvemap_tools_func(
      C, region, *static_cast<RNAUpdateCb *>(cb_v), true, CurveMapSlopeType::Negative);
}

static Block *curvemap_brush_tools_func(bContext *C, ARegion *region, void *cb_v)
{
  return curvemap_tools_func(
      C, region, *static_cast<RNAUpdateCb *>(cb_v), false, CurveMapSlopeType::Positive);
}

static Block *curvemap_brush_tools_negslope_func(bContext *C, ARegion *region, void *cb_v)
{
  return curvemap_tools_func(
      C, region, *static_cast<RNAUpdateCb *>(cb_v), false, CurveMapSlopeType::Negative);
}

static void curvemap_buttons_redraw(bContext &C)
{
  ED_region_tag_redraw(CTX_wm_region(&C));
}

static void add_preset_button(Block *block,
                              const float dx,
                              const int icon,
                              std::optional<StringRef> tip,
                              CurveMapping *cumap,
                              const bool neg_slope,
                              const eCurveMappingPreset preset,
                              const RNAUpdateCb &cb)
{
  Button *bt = uiDefIconBut(
      block, ButtonType::Row, icon, 0, 0, dx, dx, &cumap->cur, 0.0, 3.0, tip);
  button_func_set(bt, [&, cumap, neg_slope, preset, cb](bContext &C) {
    const CurveMapSlopeType slope = neg_slope ? CurveMapSlopeType::Negative :
                                                CurveMapSlopeType::Positive;
    cumap->flag &= ~CUMA_EXTEND_EXTRAPOLATE;
    cumap->preset = preset;
    BKE_curvemap_reset(cumap->cm, &cumap->clipr, cumap->preset, slope);
    BKE_curvemapping_changed(cumap, false);
    rna_update_cb(C, cb);
  });
}

/**
 * \note Still unsure how this call evolves.
 *
 * \param labeltype: Used for defining which curve-channels to show.
 */
static void curvemap_buttons_layout(Layout *layout,
                                    PointerRNA *ptr,
                                    char labeltype,
                                    bool levels,
                                    bool brush,
                                    bool neg_slope,
                                    bool tone,
                                    bool presets,
                                    const RNAUpdateCb &cb)
{
  CurveMapping *cumap = static_cast<CurveMapping *>(ptr->data);
  CurveMap *cm = &cumap->cm[cumap->cur];
  Button *bt;
  const float dx = UI_UNIT_X;
  eButGradientType bg = GRAD_NONE;

  Block *block = layout->block();

  block_emboss_set(block, EmbossType::Emboss);

  if (tone) {
    Layout &split = layout->split(0.0f, false);
    split.row(false).prop(ptr, "tone", ITEM_R_EXPAND, std::nullopt, ICON_NONE);
  }

  /* curve chooser */
  Layout *row = &layout->row(false);

  if (labeltype == 'v') {
    /* vector */
    Layout &sub = row->row(true);
    sub.alignment_set(LayoutAlign::Left);

    if (cumap->cm[0].curve) {
      bt = uiDefButV(block, ButtonType::Row, "X", 0, 0, dx, dx, &cumap->cur, 0.0, 0.0, "");
      button_func_set(bt, curvemap_buttons_redraw);
    }
    if (cumap->cm[1].curve) {
      bt = uiDefButV(block, ButtonType::Row, "Y", 0, 0, dx, dx, &cumap->cur, 0.0, 1.0, "");
      button_func_set(bt, curvemap_buttons_redraw);
    }
    if (cumap->cm[2].curve) {
      bt = uiDefButV(block, ButtonType::Row, "Z", 0, 0, dx, dx, &cumap->cur, 0.0, 2.0, "");
      button_func_set(bt, curvemap_buttons_redraw);
    }
  }
  else if (labeltype == 'c' && cumap->tone != CURVE_TONE_FILMLIKE) {
    /* color */
    Layout &sub = row->row(true);
    sub.alignment_set(LayoutAlign::Left);

    if (cumap->cm[3].curve) {
      bt = uiDefButV(block,
                     ButtonType::Row,
                     CTX_IFACE_(BLT_I18NCONTEXT_COLOR, "C"),
                     0,
                     0,
                     dx,
                     dx,
                     &cumap->cur,
                     0.0,
                     3.0,
                     TIP_("Combined channels"));
      button_func_set(bt, curvemap_buttons_redraw);
    }
    if (cumap->cm[0].curve) {
      bt = uiDefButV(block,
                     ButtonType::Row,
                     CTX_IFACE_(BLT_I18NCONTEXT_COLOR, "R"),
                     0,
                     0,
                     dx,
                     dx,
                     &cumap->cur,
                     0.0,
                     0.0,
                     TIP_("Red channel"));
      button_func_set(bt, curvemap_buttons_redraw);
    }
    if (cumap->cm[1].curve) {
      bt = uiDefButV(block,
                     ButtonType::Row,
                     CTX_IFACE_(BLT_I18NCONTEXT_COLOR, "G"),
                     0,
                     0,
                     dx,
                     dx,
                     &cumap->cur,
                     0.0,
                     1.0,
                     TIP_("Green channel"));
      button_func_set(bt, curvemap_buttons_redraw);
    }
    if (cumap->cm[2].curve) {
      bt = uiDefButV(block,
                     ButtonType::Row,
                     CTX_IFACE_(BLT_I18NCONTEXT_COLOR, "B"),
                     0,
                     0,
                     dx,
                     dx,
                     &cumap->cur,
                     0.0,
                     2.0,
                     TIP_("Blue channel"));
      button_func_set(bt, curvemap_buttons_redraw);
    }
  }
  else if (labeltype == 'h') {
    /* HSV */
    Layout &sub = row->row(true);
    sub.alignment_set(LayoutAlign::Left);

    if (cumap->cm[0].curve) {
      bt = uiDefButV(block,
                     ButtonType::Row,
                     IFACE_("H"),
                     0,
                     0,
                     dx,
                     dx,
                     &cumap->cur,
                     0.0,
                     0.0,
                     TIP_("Hue level"));
      button_func_set(bt, curvemap_buttons_redraw);
    }
    if (cumap->cm[1].curve) {
      bt = uiDefButV(block,
                     ButtonType::Row,
                     IFACE_("S"),
                     0,
                     0,
                     dx,
                     dx,
                     &cumap->cur,
                     0.0,
                     1.0,
                     TIP_("Saturation level"));
      button_func_set(bt, curvemap_buttons_redraw);
    }
    if (cumap->cm[2].curve) {
      bt = uiDefButV(block,
                     ButtonType::Row,
                     IFACE_("V"),
                     0,
                     0,
                     dx,
                     dx,
                     &cumap->cur,
                     0.0,
                     2.0,
                     TIP_("Value level"));
      button_func_set(bt, curvemap_buttons_redraw);
    }
  }
  else {
    row->alignment_set(LayoutAlign::Right);
  }

  if (labeltype == 'h') {
    bg = GRAD_H;
  }

  /* operation buttons */
  /* (Right aligned) */
  Layout &sub = row->row(true);
  sub.alignment_set(LayoutAlign::Right);

  if (!(cumap->flag & CUMA_USE_WRAPPING)) {
    /* Zoom in */
    bt = uiDefIconBut(
        block, ButtonType::But, ICON_ZOOM_IN, 0, 0, dx, dx, nullptr, 0.0, 0.0, TIP_("Zoom in"));
    button_func_set(bt, [cumap](bContext &C) { curvemap_buttons_zoom_in(&C, cumap); });
    if (!curvemap_can_zoom_in(cumap)) {
      button_disable(bt, "");
    }

    /* Zoom out */
    bt = uiDefIconBut(
        block, ButtonType::But, ICON_ZOOM_OUT, 0, 0, dx, dx, nullptr, 0.0, 0.0, TIP_("Zoom out"));
    button_func_set(bt, [cumap](bContext &C) { curvemap_buttons_zoom_out(&C, cumap); });
    if (!curvemap_can_zoom_out(cumap)) {
      button_disable(bt, "");
    }

    /* Clipping button. */
    const int icon = (cumap->flag & CUMA_DO_CLIP) ? ICON_CLIPUV_HLT : ICON_CLIPUV_DEHLT;
    bt = uiDefIconBlockBut(
        block, curvemap_clipping_func, cumap, icon, 0, 0, dx, dx, TIP_("Clipping options"));
    bt->drawflag &= ~BUT_ICON_LEFT;
    button_func_set(bt, [cb](bContext &C) { rna_update_cb(C, cb); });
  }

  RNAUpdateCb *tools_cb = MEM_new<RNAUpdateCb>(__func__, cb);
  if (brush && neg_slope) {
    bt = uiDefIconBlockBut(block,
                           curvemap_brush_tools_negslope_func,
                           tools_cb,
                           ICON_NONE,
                           0,
                           0,
                           dx,
                           dx,
                           TIP_("Tools"));
  }
  else if (brush) {
    bt = uiDefIconBlockBut(
        block, curvemap_brush_tools_func, tools_cb, ICON_NONE, 0, 0, dx, dx, TIP_("Tools"));
  }
  else if (neg_slope) {
    bt = uiDefIconBlockBut(
        block, curvemap_tools_negslope_func, tools_cb, ICON_NONE, 0, 0, dx, dx, TIP_("Tools"));
  }
  else {
    bt = uiDefIconBlockBut(
        block, curvemap_tools_posslope_func, tools_cb, ICON_NONE, 0, 0, dx, dx, TIP_("Tools"));
  }
  /* Pass ownership of `tools_cb` to the button. */
  button_funcN_set(
      bt,
      [](bContext *, void *, void *) {},
      tools_cb,
      nullptr,
      but_func_argN_free<RNAUpdateCb>,
      but_func_argN_copy<RNAUpdateCb>);

  block_funcN_set(block,
                  rna_update_cb,
                  MEM_new<RNAUpdateCb>(__func__, cb),
                  nullptr,
                  but_func_argN_free<RNAUpdateCb>,
                  but_func_argN_copy<RNAUpdateCb>);

  /* Curve itself. */
  const int size = max_ii(layout->width(), UI_UNIT_X);
  row = &layout->row(false);
  ButtonCurveMapping *curve_but = static_cast<ButtonCurveMapping *>(
      uiDefBut(block,
               ButtonType::Curve,
               IFACE_("Edit Curve Map"),
               0,
               0,
               size,
               8.0f * UI_UNIT_X,
               cumap,
               0.0f,
               1.0f,
               ""));
  curve_but->gradient_type = bg;
  if (!layout->active()) {
    button_flag_enable(curve_but, BUT_INACTIVE);
  }

  /* Sliders for selected curve point. */
  Vector<CurveMapPoint *> selected_points;
  bool point_last_or_first = false;
  for (int i = 0; i < cm->totpoint; i++) {
    const bool selected = cm->curve[i].flag & CUMA_SELECT;
    if (selected) {
      selected_points.append(&cm->curve[i]);
    }
    if (ELEM(i, 0, cm->totpoint - 1) && selected) {
      point_last_or_first = true;
    }
  }

  if (!selected_points.is_empty()) {
    CurveMap *active_cm = cumap->cm + cumap->cur;

    rctf bounds;
    if (cumap->flag & CUMA_DO_CLIP) {
      bounds = cumap->clipr;
    }
    else {
      bounds.xmin = bounds.ymin = -1000.0;
      bounds.xmax = bounds.ymax = 1000.0;
    }

    block_emboss_set(block, EmbossType::Emboss);

    layout->row(true);

    /* Curve handle buttons. */
    bt = uiDefIconBut(block,
                      ButtonType::But,
                      ICON_HANDLE_AUTO,
                      0,
                      UI_UNIT_Y,
                      UI_UNIT_X,
                      UI_UNIT_Y,
                      nullptr,
                      0.0,
                      0.0,
                      TIP_("Auto handle"));
    button_func_set(bt, [cumap, cb](bContext &C) {
      CurveMap *cuma = cumap->cm + cumap->cur;
      BKE_curvemap_handle_set(cuma, HD_AUTO);
      BKE_curvemapping_changed(cumap, false);
      rna_update_cb(C, cb);
    });

    for (const CurveMapPoint *cmp : selected_points) {
      const bool auto_anim_vec = ((cmp->flag & CUMA_HANDLE_AUTO_ANIM) == false) &&
                                 ((cmp->flag & CUMA_HANDLE_VECTOR) == false);
      if (auto_anim_vec) {
        bt->flag |= UI_SELECT_DRAW;
      }
    }

    bt = uiDefIconBut(block,
                      ButtonType::But,
                      ICON_HANDLE_VECTOR,
                      0,
                      UI_UNIT_Y,
                      UI_UNIT_X,
                      UI_UNIT_Y,
                      nullptr,
                      0.0,
                      0.0,
                      TIP_("Vector handle"));
    button_func_set(bt, [cumap, cb](bContext &C) {
      CurveMap *cuma = cumap->cm + cumap->cur;
      BKE_curvemap_handle_set(cuma, HD_VECT);
      BKE_curvemapping_changed(cumap, false);
      rna_update_cb(C, cb);
    });

    for (const CurveMapPoint *cmp : selected_points) {
      const bool vec = (cmp->flag & CUMA_HANDLE_VECTOR);
      if (vec) {
        bt->flag |= UI_SELECT_DRAW;
      }
    }

    bt = uiDefIconBut(block,
                      ButtonType::But,
                      ICON_HANDLE_AUTOCLAMPED,
                      0,
                      UI_UNIT_Y,
                      UI_UNIT_X,
                      UI_UNIT_Y,
                      nullptr,
                      0.0,
                      0.0,
                      TIP_("Auto clamped"));
    button_func_set(bt, [cumap, cb](bContext &C) {
      CurveMap *cuma = cumap->cm + cumap->cur;
      BKE_curvemap_handle_set(cuma, HD_AUTO_ANIM);
      BKE_curvemapping_changed(cumap, false);
      rna_update_cb(C, cb);
    });

    for (const CurveMapPoint *cmp : selected_points) {
      const bool auto_anim = (cmp->flag & CUMA_HANDLE_AUTO_ANIM);
      if (auto_anim) {
        bt->flag |= UI_SELECT_DRAW;
      }
    }

    /* Curve handle position */
    auto curve_runtime = std::make_shared<CurveRuntimeProperties>();
    curve_runtime->last_pt = BKE_curvemap_active_get(active_cm);
    curve_runtime->last_pos.x = curve_runtime->last_pt->x;
    curve_runtime->last_pos.y = curve_runtime->last_pt->y;

    /* While the slider controls the active element, all selected points move together.
     * Contract the slider range so the outermost selected points stay within the clip region. */
    rctf slider_bounds = bounds;
    if (selected_points.size() > 1) {
      rctf selection_bounds;
      BLI_rctf_init_minmax(&selection_bounds);

      /* The slider only shows the active point's position, but moves all selected points by the
       * same delta. Clamp the range so points at the edges of the selection can't be moved outside
       * the clip region. */
      for (const CurveMapPoint *cmp : selected_points) {
        const float loc[2] = {cmp->x, cmp->y};
        BLI_rctf_do_minmax_v(&selection_bounds, loc);
      }

      slider_bounds.xmin += curve_runtime->last_pt->x - selection_bounds.xmin;
      slider_bounds.xmax += curve_runtime->last_pt->x - selection_bounds.xmax;
      slider_bounds.ymin += curve_runtime->last_pt->y - selection_bounds.ymin;
      slider_bounds.ymax += curve_runtime->last_pt->y - selection_bounds.ymax;
    }

    const char *const axis_labels[2] = {"X:", "Y:"};
    float *const axis_ptrs[2] = {&curve_runtime->last_pt->x, &curve_runtime->last_pt->y};
    const float axis_min[2] = {slider_bounds.xmin, slider_bounds.ymin};
    const float axis_max[2] = {slider_bounds.xmax, slider_bounds.ymax};
    for (int axis = 0; axis < 2; axis++) {
      bt = uiDefButV(block,
                     ButtonType::Num,
                     axis_labels[axis],
                     0,
                     (2 - axis) * UI_UNIT_Y,
                     UI_UNIT_X * 10,
                     UI_UNIT_Y,
                     axis_ptrs[axis],
                     axis_min[axis],
                     axis_max[axis],
                     "");
      button_number_step_size_set(bt, 1);
      button_number_precision_set(bt, 5);
      if (selected_points.size() == 1) {
        /* Simplified logic */
        button_func_set(bt, [cumap, cb](bContext &C) {
          BKE_curvemapping_changed(cumap, true);
          rna_update_cb(C, cb);
        });
      }
      else {
        button_func_set(bt, [cumap, cb, curve_runtime, axis](bContext &C) {
          CurveMap *cuma = cumap->cm + cumap->cur;
          float *last_pt_co = &curve_runtime->last_pt->x;
          const float delta = last_pt_co[axis] - curve_runtime->last_pos[axis];
          /* Logically `-= delta`, better restore the original value. */
          last_pt_co[axis] = curve_runtime->last_pos[axis];
          float2 offset(0.0f);
          offset[axis] = delta;
          BKE_curvemap_translate_selection(cuma, offset);
          BKE_curvemapping_changed(cumap, true);
          rna_update_cb(C, cb);

          /* Update the active point if the pointer changed. */
          curve_runtime->last_pt = BKE_curvemap_active_get(cuma);
          last_pt_co = &curve_runtime->last_pt->x;
          curve_runtime->last_pos[axis] = last_pt_co[axis];
        });
      }
    }

    /* Curve handle delete point */
    bt = uiDefIconBut(
        block, ButtonType::But, ICON_X, 0, 0, dx, dx, nullptr, 0.0, 0.0, TIP_("Delete points"));
    button_func_set(bt, [cumap, cb](bContext &C) {
      BKE_curvemap_remove(cumap->cm + cumap->cur, SELECT);
      BKE_curvemapping_changed(cumap, false);
      rna_update_cb(C, cb);
    });
    if (point_last_or_first) {
      button_flag_enable(bt, BUT_DISABLED);
    }
  }

  /* black/white levels */
  if (levels) {
    Layout &split = layout->split(0.0f, false);
    split.column(false).prop(ptr, "black_level", ITEM_R_EXPAND, std::nullopt, ICON_NONE);
    split.column(false).prop(ptr, "white_level", ITEM_R_EXPAND, std::nullopt, ICON_NONE);

    layout->row(false);
    bt = uiDefBut(block,
                  ButtonType::But,
                  IFACE_("Reset"),
                  0,
                  0,
                  UI_UNIT_X * 10,
                  UI_UNIT_Y,
                  nullptr,
                  0.0f,
                  0.0f,
                  TIP_("Reset curves and black/white point"));
    button_func_set(bt, [cumap, cb](bContext &C) {
      cumap->preset = CURVE_PRESET_LINE;
      for (int a = 0; a < CM_TOT; a++) {
        BKE_curvemap_reset(
            cumap->cm + a, &cumap->clipr, cumap->preset, CurveMapSlopeType::Positive);
      }

      cumap->black[0] = cumap->black[1] = cumap->black[2] = 0.0f;
      cumap->white[0] = cumap->white[1] = cumap->white[2] = 1.0f;
      BKE_curvemapping_set_black_white(cumap, nullptr, nullptr);

      BKE_curvemapping_changed(cumap, false);
      rna_update_cb(C, cb);
    });
  }

  if (presets) {
    row = &layout->row(true);
    sub.alignment_set(LayoutAlign::Left);
    add_preset_button(block,
                      dx,
                      ICON_SMOOTHCURVE,
                      TIP_("Smooth preset"),
                      cumap,
                      neg_slope,
                      CURVE_PRESET_SMOOTH,
                      cb);
    add_preset_button(block,
                      dx,
                      ICON_SPHERECURVE,
                      TIP_("Round preset"),
                      cumap,
                      neg_slope,
                      CURVE_PRESET_ROUND,
                      cb);
    add_preset_button(
        block, dx, ICON_ROOTCURVE, TIP_("Root preset"), cumap, neg_slope, CURVE_PRESET_ROOT, cb);
    add_preset_button(block,
                      dx,
                      ICON_SHARPCURVE,
                      TIP_("Sharp preset"),
                      cumap,
                      neg_slope,
                      CURVE_PRESET_SHARP,
                      cb);
    add_preset_button(
        block, dx, ICON_LINCURVE, TIP_("Linear preset"), cumap, neg_slope, CURVE_PRESET_LINE, cb);
    add_preset_button(
        block, dx, ICON_NOCURVE, TIP_("Constant preset"), cumap, neg_slope, CURVE_PRESET_MAX, cb);
  }

  block_funcN_set(block, nullptr, nullptr, nullptr);
}

namespace {
struct OwnedCurveNumeric {
  uint64_t generation;
  int point;
  float coordinate[2];
};
struct OwnedCurveDialog {
  std::shared_ptr<rna::OwnedCurvePath> path;
  std::shared_ptr<rna::OwnedCurveEdit> edit;
  std::shared_ptr<OwnedCurveNumeric> numeric;
  uint64_t generation = 0;
  float clip[4] = {0, 0, 1, 1};
  bool use_clip = true;
  bool extrapolate = true;
  bool cancelled = false;
  std::string error;
};
using OwnedCurveDialogPtr = std::shared_ptr<OwnedCurveDialog>;
static std::unordered_map<int, OwnedCurveDialogPtr> owned_curve_tickets;
static int owned_curve_next_ticket = 1;

static OwnedCurveDialogPtr owned_curve_dialog_take(wmOperator *op)
{
  auto *holder = static_cast<OwnedCurveDialogPtr *>(op->customdata);
  if (!holder) {
    return nullptr;
  }
  OwnedCurveDialogPtr result = std::move(*holder);
  MEM_delete(holder);
  op->customdata = nullptr;
  return result;
}

static bool owned_curve_candidate_valid(const CurveMapping &mapping, std::string &error)
{
  IDProperty *encoded = BKE_curvemapping_to_idprop(mapping, "curve", nullptr, error);
  if (!encoded) {
    return false;
  }
  IDP_FreeProperty(encoded);
  return true;
}

static void owned_curve_dialog_changed(OwnedCurveDialog &session)
{
  session.generation++;
  session.error.clear();
  BKE_curvemapping_changed(&RNA_owned_curve_edit_mapping(*session.edit), false);
}

static Button *owned_curve_button(Block *block, const char *label)
{
  Button *button = uiDefBut(
      block, ButtonType::But, label, 0, 0, 4 * UI_UNIT_X, UI_UNIT_Y, nullptr, 0, 0, "");
  button->flag &= ~BUT_UNDO;
  return button;
}

enum class OwnedCurveActionKind { Preset, Handle, Remove, View };
struct OwnedCurveAction {
  OwnedCurveDialogPtr session;
  std::shared_ptr<OwnedCurveNumeric> numeric;
  OwnedCurveActionKind kind;
  int parameter;
  std::function<void(bContext &)> apply;
};

static void owned_curve_action_dispatch(bContext *C, void *argument, void *)
{
  static_cast<OwnedCurveAction *>(argument)->apply(*C);
}

static void owned_curve_action_set(Button *button,
                                   const OwnedCurveDialogPtr &session,
                                   OwnedCurveActionKind kind,
                                   int parameter,
                                   std::function<void(bContext &)> apply,
                                   std::shared_ptr<OwnedCurveNumeric> numeric = nullptr)
{
  button_funcN_set(
      button,
      owned_curve_action_dispatch,
      MEM_new<OwnedCurveAction>(
          __func__,
          OwnedCurveAction{session, std::move(numeric), kind, parameter, std::move(apply)}),
      nullptr,
      but_func_argN_free<OwnedCurveAction>,
      but_func_argN_copy<OwnedCurveAction>);
  button_func_identity_compare_set(button, [](const Button *a, const Button *b) {
    const auto &left = *static_cast<const OwnedCurveAction *>(a->func_argN);
    const auto &right = *static_cast<const OwnedCurveAction *>(b->func_argN);
    return left.session == right.session && left.numeric == right.numeric &&
           left.kind == right.kind && left.parameter == right.parameter;
  });
}

static void owned_curve_dialog_draw(bContext * /*C*/, wmOperator *op)
{
  auto *holder = static_cast<OwnedCurveDialogPtr *>(op->customdata);
  if (!holder || !(*holder)->edit) {
    return;
  }
  const OwnedCurveDialogPtr session = *holder;
  CurveMapping &mapping = RNA_owned_curve_edit_mapping(*session->edit);
  CurveMap &curve = mapping.cm[0];
  Layout *layout = op->layout;
  Block *block = layout->block();

  layout->row(true);
  for (const auto &[label, preset] : {std::pair{"Linear", CURVE_PRESET_LINE},
                                      {"Smooth", CURVE_PRESET_SMOOTH},
                                      {"Sharp", CURVE_PRESET_SHARP},
                                      {"Round", CURVE_PRESET_ROUND}})
  {
    Button *button = owned_curve_button(block, IFACE_(label));
    owned_curve_action_set(
        button, session, OwnedCurveActionKind::Preset, preset, [session, preset](bContext &) {
          CurveMapping &mapping = RNA_owned_curve_edit_mapping(*session->edit);
          BKE_curvemap_reset(&mapping.cm[0], &mapping.clipr, preset, CurveMapSlopeType::Positive);
          owned_curve_dialog_changed(*session);
        });
  }

  layout->row(false);
  auto *graph = static_cast<ButtonCurveMapping *>(uiDefBut(block,
                                                           ButtonType::Curve,
                                                           IFACE_("Edit Curve"),
                                                           0,
                                                           0,
                                                           16 * UI_UNIT_X,
                                                           10 * UI_UNIT_Y,
                                                           &mapping,
                                                           0,
                                                           1,
                                                           ""));
  graph->flag &= ~BUT_UNDO;
  graph->owned_curve_cancel = [session]() { session->cancelled = true; };
  graph->owned_curve_paste_validate = [session](const CurveMapping &source) {
    if (source.cur != 0 || !std::isfinite(source.curr.xmin) || !std::isfinite(source.curr.xmax) ||
        !std::isfinite(source.curr.ymin) || !std::isfinite(source.curr.ymax) ||
        source.curr.xmin >= source.curr.xmax || source.curr.ymin >= source.curr.ymax)
    {
      session->error = "Clipboard curve has an invalid scalar view";
      return false;
    }
    if (!owned_curve_candidate_valid(source, session->error)) {
      return false;
    }
    session->clip[0] = source.clipr.xmin;
    session->clip[1] = source.clipr.ymin;
    session->clip[2] = source.clipr.xmax;
    session->clip[3] = source.clipr.ymax;
    session->use_clip = source.flag & CUMA_DO_CLIP;
    session->extrapolate = source.flag & CUMA_EXTEND_EXTRAPOLATE;
    return true;
  };
  button_func_set(graph, [session](bContext &) { owned_curve_dialog_changed(*session); });

  int selected = -1;
  for (int i = 0; i < curve.totpoint; i++) {
    if (curve.curve[i].flag & CUMA_SELECT) {
      selected = i;
      if (curve.curve[i].flag & CUMA_ACTIVE) {
        break;
      }
    }
  }
  if (selected >= 0) {
    if (!session->numeric || session->numeric->generation != session->generation ||
        session->numeric->point != selected)
    {
      session->numeric = std::make_shared<OwnedCurveNumeric>(OwnedCurveNumeric{
          session->generation, selected, {curve.curve[selected].x, curve.curve[selected].y}});
    }
    const auto numeric = session->numeric;
    layout->row(true);
    for (int axis = 0; axis < 2; axis++) {
      Button *button = uiDefButV(block,
                                 ButtonType::Num,
                                 axis == 0 ? "X:" : "Y:",
                                 0,
                                 0,
                                 7 * UI_UNIT_X,
                                 UI_UNIT_Y,
                                 &numeric->coordinate[axis],
                                 -FLT_MAX,
                                 FLT_MAX,
                                 "");
      button->flag &= ~BUT_UNDO;
      button_number_precision_set(button, 5);
      button_func_set(button, [session, numeric](bContext &) {
        if (numeric != session->numeric || numeric->generation != session->generation) {
          session->error = "Point selection changed during numeric editing";
          return;
        }
        CurveMapping &mapping = RNA_owned_curve_edit_mapping(*session->edit);
        CurveMapping *candidate = BKE_curvemapping_copy(&mapping);
        auto &point = candidate->cm[0].curve[numeric->point];
        point.x = numeric->coordinate[0];
        point.y = numeric->coordinate[1];
        for (int i = 0; i < candidate->cm[0].totpoint; i++) {
          SET_FLAG_FROM_TEST(candidate->cm[0].curve[i].flag, i == numeric->point, CUMA_ACTIVE);
        }
        BKE_curvemapping_changed(candidate, false);
        if (owned_curve_candidate_valid(*candidate, session->error)) {
          BKE_curvemapping_free_data(&mapping);
          BKE_curvemapping_copy_data(&mapping, candidate);
          owned_curve_dialog_changed(*session);
          numeric->generation = session->generation;
          for (int i = 0; i < mapping.cm[0].totpoint; i++) {
            if (mapping.cm[0].curve[i].flag & CUMA_ACTIVE) {
              numeric->point = i;
              numeric->coordinate[0] = mapping.cm[0].curve[i].x;
              numeric->coordinate[1] = mapping.cm[0].curve[i].y;
              break;
            }
          }
        }
        BKE_curvemapping_free(candidate);
      });
    }
    layout->row(true);
    for (const auto &[label, flags] : {std::pair{"Auto", eCurveMapPoint_Flag(0)},
                                       {"Clamped", CUMA_HANDLE_AUTO_ANIM},
                                       {"Vector", CUMA_HANDLE_VECTOR}})
    {
      Button *button = owned_curve_button(block, IFACE_(label));
      owned_curve_action_set(
          button,
          session,
          OwnedCurveActionKind::Handle,
          flags,
          [session, numeric, flags](bContext &) {
            if (numeric != session->numeric || numeric->generation != session->generation) {
              return;
            }
            auto &point = RNA_owned_curve_edit_mapping(*session->edit).cm[0].curve[numeric->point];
            point.flag = (point.flag & ~(CUMA_HANDLE_AUTO_ANIM | CUMA_HANDLE_VECTOR)) | flags;
            owned_curve_dialog_changed(*session);
          },
          numeric);
    }
    Button *remove = owned_curve_button(block, IFACE_("Remove"));
    if (curve.totpoint <= 2) {
      button_disable(remove, "A curve requires at least two points");
    }
    owned_curve_action_set(
        remove,
        session,
        OwnedCurveActionKind::Remove,
        0,
        [session, numeric](bContext &) {
          if (numeric != session->numeric || numeric->generation != session->generation) {
            return;
          }
          auto &curve = RNA_owned_curve_edit_mapping(*session->edit).cm[0];
          if (curve.totpoint > 2) {
            BKE_curvemap_remove_point(&curve, &curve.curve[numeric->point]);
            owned_curve_dialog_changed(*session);
          }
        },
        numeric);
  }

  layout->row(true);
  Button *clip = uiDefButV(block,
                           ButtonType::Checkbox,
                           IFACE_("Clip"),
                           0,
                           0,
                           7 * UI_UNIT_X,
                           UI_UNIT_Y,
                           &session->use_clip,
                           0,
                           1,
                           "");
  clip->flag &= ~BUT_UNDO;
  button_func_set(clip, [session](bContext &) {
    CurveMapping &mapping = RNA_owned_curve_edit_mapping(*session->edit);
    SET_FLAG_FROM_TEST(mapping.flag, session->use_clip, CUMA_DO_CLIP);
    owned_curve_dialog_changed(*session);
  });
  Button *extend = uiDefButV(block,
                             ButtonType::Checkbox,
                             IFACE_("Extrapolate"),
                             0,
                             0,
                             7 * UI_UNIT_X,
                             UI_UNIT_Y,
                             &session->extrapolate,
                             0,
                             1,
                             "");
  extend->flag &= ~BUT_UNDO;
  button_func_set(extend, [session](bContext &) {
    auto &mapping = RNA_owned_curve_edit_mapping(*session->edit);
    SET_FLAG_FROM_TEST(mapping.flag, session->extrapolate, CUMA_EXTEND_EXTRAPOLATE);
    owned_curve_dialog_changed(*session);
  });

  const char *labels[4] = {"Min X:", "Min Y:", "Max X:", "Max Y:"};
  for (int i = 0; i < 4; i++) {
    if (i % 2 == 0) {
      layout->row(true);
    }
    Button *button = uiDefButV(block,
                               ButtonType::Num,
                               labels[i],
                               0,
                               0,
                               7 * UI_UNIT_X,
                               UI_UNIT_Y,
                               &session->clip[i],
                               -100,
                               100,
                               "");
    button->flag &= ~BUT_UNDO;
    button_func_set(button, [session](bContext &) {
      const float *values = session->clip;
      if (!std::all_of(values, values + 4, [](float value) { return std::isfinite(value); }) ||
          values[0] >= values[2] || values[1] >= values[3])
      {
        session->error = "Clip minimum must be smaller than maximum";
        return;
      }
      auto &mapping = RNA_owned_curve_edit_mapping(*session->edit);
      BLI_rctf_init(&mapping.clipr, values[0], values[2], values[1], values[3]);
      owned_curve_dialog_changed(*session);
    });
  }
  layout->row(true);
  for (int direction = -1; direction <= 1; direction++) {
    const char *label = direction < 0 ? "Zoom Out" : direction > 0 ? "Zoom In" : "Reset View";
    Button *button = owned_curve_button(block, IFACE_(label));
    owned_curve_action_set(
        button, session, OwnedCurveActionKind::View, direction, [session, direction](bContext &C) {
          auto &mapping = RNA_owned_curve_edit_mapping(*session->edit);
          if (direction == 0) {
            BKE_curvemapping_reset_view(&mapping);
          }
          else if (direction > 0) {
            curvemap_buttons_zoom_in(&C, &mapping);
          }
          else {
            curvemap_buttons_zoom_out(&C, &mapping);
          }
        });
  }
  if (!session->error.empty()) {
    layout->label(session->error, ICON_ERROR);
  }
}

static wmOperatorStatus owned_curve_dialog_exec(bContext *C, wmOperator *op)
{
  const auto session = owned_curve_dialog_take(op);
  if (!session || session->cancelled) {
    return OPERATOR_CANCELLED;
  }
  OwnedCurveRNAErrorScope errors;
  bool changed = false;
  bool success;
  ID *owner = RNA_owned_curve_path_owner(*session->path, *CTX_data_main(C));
  if (!owner) {
    return OPERATOR_CANCELLED;
  }
  const uint32_t owner_uid = owner->session_uid;
  std::string authoring_error;
  const bool scoped = ELEM(GS(owner->name), ID_BR, ID_SCE);
  auto authoring = scoped ? ed::authoring_edit_begin(C, *owner, false, true, authoring_error) :
                            nullptr;
  if (scoped && !authoring) {
    BKE_report(op->reports, RPT_ERROR, authoring_error.c_str());
    return OPERATOR_CANCELLED;
  }
  if (session->edit) {
    const float *clip = session->clip;
    if (!std::all_of(clip, clip + 4, [](float value) { return std::isfinite(value); }) ||
        clip[0] >= clip[2] || clip[1] >= clip[3])
    {
      BKE_report(op->reports, RPT_ERROR, "Clip minimum must be smaller than maximum");
      return OPERATOR_CANCELLED;
    }
    success = RNA_owned_curve_edit_commit(*session->edit, C, changed);
  }
  else {
    success = RNA_owned_curve_path_initialize(*session->path, *CTX_data_main(C), C);
    changed = success;
  }
  if (!success) {
    BKE_report(op->reports,
               RPT_ERROR,
               errors.message.empty() ? "Owned curve edit rejected" : errors.message.c_str());
  }
  if (success && changed) {
    if (authoring) {
      /* The RNA callback can remove the owner. Revalidate without dereferencing it. */
      ID *current = BKE_libblock_find_session_uid(CTX_data_main(C), owner_uid);
      if (!current ||
          !ed::authoring_edit_finish(
              C, *current, *authoring, true, "Edit Owned Curve", changed, authoring_error))
      {
        BKE_report(op->reports, RPT_ERROR, authoring_error.c_str());
        return OPERATOR_CANCELLED;
      }
    }
    else {
      ED_undo_push(C, "Edit Owned Curve", UndoEncodeHints::None);
    }
  }
  return success && changed ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
}

static wmOperatorStatus owned_curve_dialog_invoke(bContext *C, wmOperator *op, const wmEvent *)
{
  const int ticket = RNA_int_get(op->ptr, "ticket");
  const auto it = owned_curve_tickets.find(ticket);
  if (it == owned_curve_tickets.end()) {
    return OPERATOR_CANCELLED;
  }
  const auto session = it->second;
  owned_curve_tickets.erase(it);
  op->customdata = MEM_new<OwnedCurveDialogPtr>(__func__, session);
  if (!RNA_owned_curve_path_is_set(*session->path)) {
    return owned_curve_dialog_exec(C, op);
  }
  OwnedCurveRNAErrorScope errors;
  session->edit = RNA_owned_curve_edit_begin(*session->path);
  if (!session->edit) {
    BKE_report(op->reports, RPT_ERROR, errors.message.c_str());
    owned_curve_dialog_take(op);
    return OPERATOR_CANCELLED;
  }
  const auto &mapping = RNA_owned_curve_edit_mapping(*session->edit);
  session->clip[0] = mapping.clipr.xmin;
  session->clip[1] = mapping.clipr.ymin;
  session->clip[2] = mapping.clipr.xmax;
  session->clip[3] = mapping.clipr.ymax;
  session->use_clip = mapping.flag & CUMA_DO_CLIP;
  session->extrapolate = mapping.flag & CUMA_EXTEND_EXTRAPOLATE;
  return WM_operator_props_dialog_popup(C, op, 440, IFACE_("Edit Curve"), IFACE_("Apply"));
}
}  // namespace

void UI_OT_owned_curve_edit(wmOperatorType *ot)
{
  ot->name = "Edit Owned Curve";
  ot->idname = "UI_OT_owned_curve_edit";
  ot->description = "Edit a temporary curve and apply it to its captured owner";
  ot->invoke = owned_curve_dialog_invoke;
  ot->exec = owned_curve_dialog_exec;
  ot->ui = owned_curve_dialog_draw;
  ot->cancel = [](bContext *, wmOperator *op) { owned_curve_dialog_take(op); };
  ot->flag = OPTYPE_INTERNAL;
  PropertyRNA *prop = RNA_def_int(ot->srna, "ticket", 0, 0, INT_MAX, "Ticket", "", 0, INT_MAX);
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
}

void template_owned_curve_mapping(Layout *layout, PointerRNA *ptr, const StringRefNull path)
{
  OwnedCurveRNAErrorScope errors;
  const auto target = RNA_owned_curve_path_capture(*ptr, path.c_str());
  if (!target) {
    layout->label(errors.message, ICON_ERROR);
    return;
  }
  Button *button = owned_curve_button(
      layout->block(),
      RNA_owned_curve_path_is_set(*target) ? IFACE_("Edit Curve") : IFACE_("Create Custom Curve"));
  if (!ID_IS_EDITABLE(ptr->owner_id) || ID_IS_OVERRIDE_LIBRARY(ptr->owner_id)) {
    button_disable(button, "Owned curve owner is read-only");
  }
  using Target = std::shared_ptr<rna::OwnedCurvePath>;
  button_funcN_set(
      button,
      [](bContext *C, void *argument, void *) {
        const auto target = *static_cast<Target *>(argument);
        const auto session = std::make_shared<OwnedCurveDialog>();
        session->path = target;
        while (owned_curve_tickets.contains(owned_curve_next_ticket)) {
          owned_curve_next_ticket = owned_curve_next_ticket == INT_MAX ?
                                        1 :
                                        owned_curve_next_ticket + 1;
        }
        const int ticket = owned_curve_next_ticket;
        owned_curve_tickets.emplace(ticket, session);
        PointerRNA properties = WM_operator_properties_create("UI_OT_owned_curve_edit");
        RNA_int_set(&properties, "ticket", ticket);
        WM_operator_name_call(
            C, "UI_OT_owned_curve_edit", wm::OpCallContext::InvokeDefault, &properties, nullptr);
        WM_operator_properties_free(&properties);
        owned_curve_tickets.erase(ticket);
      },
      MEM_new<Target>(__func__, target),
      nullptr,
      but_func_argN_free<Target>,
      but_func_argN_copy<Target>);
  button_func_identity_compare_set(button, [](const Button *a, const Button *b) {
    const auto &left = *static_cast<const Target *>(a->func_argN);
    const auto &right = *static_cast<const Target *>(b->func_argN);
    return RNA_owned_curve_path_equal(*left, *right);
  });
}

void template_curve_mapping(Layout *layout,
                            PointerRNA *ptr,
                            const StringRefNull propname,
                            int type,
                            bool levels,
                            bool brush,
                            bool neg_slope,
                            bool tone,
                            bool presets)
{
  PropertyRNA *prop = RNA_struct_find_property(ptr, propname.c_str());
  Block *block = layout->block();

  if (!prop) {
    RNA_warning(
        "curve property not found: %s.%s", RNA_struct_identifier(ptr->type), propname.c_str());
    return;
  }

  if (RNA_property_type(prop) != PROP_POINTER) {
    RNA_warning(
        "curve is not a pointer: %s.%s", RNA_struct_identifier(ptr->type), propname.c_str());
    return;
  }

  if (RNA_property_is_owned_curve(prop)) {
    if (ELEM(type, 'c', 'h', 'v') || levels || tone || neg_slope) {
      layout->label(IFACE_("Owned curves require scalar controls with positive presets"),
                    ICON_ERROR);
      return;
    }
    template_owned_curve_mapping(layout, ptr, propname);
    return;
  }
  PointerRNA cptr = RNA_property_pointer_get(ptr, prop);
  if (!cptr || !RNA_struct_is_a(cptr.type, RNA_CurveMapping)) {
    return;
  }

  ID *id = cptr.owner_id;
  block_lock_set(block, (id && !ID_IS_EDITABLE(id)), ERROR_LIBDATA_MESSAGE);

  curvemap_buttons_layout(
      layout, &cptr, type, levels, brush, neg_slope, tone, presets, RNAUpdateCb{*ptr, prop});

  block_lock_clear(block);
}

}  // namespace blender::ui
