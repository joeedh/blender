/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <array>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "BKE_brush.hh"
#include "BKE_callbacks.hh"
#include "BKE_colortools.hh"
#include "BKE_context.hh"
#include "BKE_idprop.hh"
#include "BKE_lib_id.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_paint.hh"
#include "BKE_undo_system.hh"
#include "BLI_listbase.hh"
#include "BLI_threads.hh"
#include "BLI_utildefines.hh"
#include "DEG_depsgraph.hh"
#include "DNA_brush_types.h"
#include "DNA_scene_types.h"
#include "DNA_windowmanager_types.h"
#include "ED_authoring_undo.hh"
#include "ED_undo.hh"
#include "MEM_guardedalloc.h"
#include "WM_api.hh"
#include "WM_types.hh"

namespace blender::ed {
namespace {
constexpr size_t max_bytes = 64 * 1024 * 1024;
constexpr size_t max_nodes = 100000;
uint64_t epoch = 1;
std::unordered_map<uint32_t, uint64_t> revisions;
std::unordered_map<uint32_t, std::weak_ptr<AuthoringEdit>> active;
UndoType *authoring_type = nullptr;
std::function<void(const ID *)> owner_idprop_invalidate_hook;

struct PropFree {
  void operator()(IDProperty *p) const
  {
    if (p) {
      IDP_FreeProperty_ex(p, false);
    }
  }
};
using Prop = std::unique_ptr<IDProperty, PropFree>;
struct CurveFree {
  void operator()(CurveMapping *p) const
  {
    if (p) {
      BKE_curvemapping_free(p);
    }
  }
};
using Curve = std::unique_ptr<CurveMapping, CurveFree>;

/* Canonical data excludes allocator capacity/pointers but includes unknown saved
 * properties and UI metadata. Allocation accounting is deliberately conservative. */
struct Writer {
  std::vector<char> bytes;
  size_t memory = 0;
  size_t nodes = 0;
  bool valid = true;
  void account(size_t n)
  {
    if (n > max_bytes || memory > max_bytes - n) {
      valid = false;
      return;
    }
    memory += n;
  }
  void allocation(const void *p)
  {
    if (p) {
      account(MEM_allocN_len(p));
    }
  }
  void raw(const void *p, size_t n)
  {
    if (!valid || n > max_bytes || bytes.size() > max_bytes - n || (n && !p)) {
      valid = false;
      return;
    }
    if (n) {
      const char *v = static_cast<const char *>(p);
      bytes.insert(bytes.end(), v, v + n);
    }
  }
  template<typename T> void value(const T &v)
  {
    raw(&v, sizeof(v));
  }
  void string(const char *v)
  {
    value(bool(v));
    if (v) {
      allocation(v);
      const size_t n = strlen(v);
      value(n);
      raw(v, n);
    }
  }
  template<typename T> void array(const T *p, int n)
  {
    value(bool(p));
    value(n);
    if (n < 0 || size_t(n) > max_bytes / sizeof(T)) {
      valid = false;
      return;
    }
    if (p) {
      allocation(p);
      raw(p, size_t(n) * sizeof(T));
    }
    else if (n) {
      valid = false;
    }
  }
};

void ui_data(Writer &w, const IDProperty &p)
{
  const auto *ui = p.ui_data;
  w.value(bool(ui));
  if (!ui) {
    return;
  }
  w.allocation(ui);
  w.string(ui->description);
  w.value(ui->rna_subtype);
#define FIELD(f) w.value(u.f)
  switch (IDP_ui_data_type(&p)) {
    case IDP_UI_DATA_TYPE_INT: {
      const auto &u = *reinterpret_cast<const IDPropertyUIDataInt *>(ui);
      w.array(u.default_array, u.default_array_len);
      FIELD(min);
      FIELD(max);
      FIELD(soft_min);
      FIELD(soft_max);
      FIELD(step);
      FIELD(default_value);
      FIELD(enum_items_num);
      w.allocation(u.enum_items);
      if (u.enum_items_num < 0 || u.enum_items_num > int(max_nodes)) {
        w.valid = false;
        break;
      }
      for (int i = 0; i < u.enum_items_num; i++) {
        const auto &e = u.enum_items[i];
        w.string(e.identifier);
        w.string(e.name);
        w.string(e.description);
        w.value(e.value);
        w.value(e.icon);
      }
      break;
    }
    case IDP_UI_DATA_TYPE_FLOAT: {
      const auto &u = *reinterpret_cast<const IDPropertyUIDataFloat *>(ui);
      w.array(u.default_array, u.default_array_len);
      FIELD(min);
      FIELD(max);
      FIELD(soft_min);
      FIELD(soft_max);
      FIELD(step);
      FIELD(precision);
      FIELD(default_value);
      break;
    }
    case IDP_UI_DATA_TYPE_BOOLEAN: {
      const auto &u = *reinterpret_cast<const IDPropertyUIDataBool *>(ui);
      w.array(u.default_array, u.default_array_len);
      FIELD(default_value);
      break;
    }
    case IDP_UI_DATA_TYPE_STRING:
      w.string(reinterpret_cast<const IDPropertyUIDataString *>(ui)->default_value);
      break;
    case IDP_UI_DATA_TYPE_ID:
      w.value(reinterpret_cast<const IDPropertyUIDataID *>(ui)->id_type);
      break;
    default:
      w.valid = false;
  }
#undef FIELD
}

struct Ref {
  uint32_t uid;
  short type;
};
void property(Writer &w, const IDProperty *p, std::vector<Ref> &refs, int depth = 0)
{
  w.value(bool(p));
  if (!p || !w.valid) {
    return;
  }
  if (depth > 64 || ++w.nodes > max_nodes) {
    w.valid = false;
    return;
  }
  w.account(sizeof(*p));
  w.value(p->type);
  w.value(p->subtype);
  w.value(p->flag);
  w.raw(p->name, sizeof(p->name));
  w.value(p->len);
  ui_data(w, *p);
  switch (p->type) {
    case IDP_GROUP:
      if (p->data.children_map) {
        w.account(sizeof(*p->data.children_map) + p->data.children_map->children.size_in_bytes());
      }
      for (const IDProperty &child : p->data.group) {
        property(w, &child, refs, depth + 1);
      }
      break;
    case IDP_ID: {
      const ID *id = IDP_ID_get(p);
      const Ref ref{id ? id->session_uid : 0, short(id ? GS(id->name) : 0)};
      refs.push_back(ref);
      w.value(ref.uid);
      w.value(ref.type);
      break;
    }
    case IDP_IDPARRAY:
      w.allocation(p->data.pointer);
      if (p->len < 0 || p->len > int(max_nodes)) {
        w.valid = false;
        break;
      }
      for (int i = 0; i < p->len; i++) {
        property(w, static_cast<const IDProperty *>(p->data.pointer) + i, refs, depth + 1);
      }
      break;
    case IDP_ARRAY:
      w.allocation(p->data.pointer);
      if (p->len < 0 || size_t(p->len) > max_bytes / sizeof(double)) {
        w.valid = false;
        break;
      }
      if (p->subtype == IDP_GROUP) {
        auto **items = static_cast<IDProperty **>(p->data.pointer);
        for (int i = 0; i < p->len; i++) {
          property(w, items[i], refs, depth + 1);
        }
      }
      else {
        const size_t size = p->subtype == IDP_DOUBLE  ? sizeof(double) :
                            p->subtype == IDP_BOOLEAN ? sizeof(int8_t) :
                                                        sizeof(int);
        if (!ELEM(p->subtype, IDP_INT, IDP_FLOAT, IDP_DOUBLE, IDP_BOOLEAN)) {
          w.valid = false;
          break;
        }
        w.raw(p->data.pointer, size_t(p->len) * size);
      }
      break;
    case IDP_STRING:
      w.allocation(p->data.pointer);
      if (p->len < 0) {
        w.valid = false;
        break;
      }
      w.raw(p->data.pointer, p->len);
      break;
    case IDP_INT:
    case IDP_FLOAT:
    case IDP_BOOLEAN:
      w.value(p->data.val);
      break;
    case IDP_DOUBLE:
      w.value(p->data.val);
      w.value(p->data.val2);
      break;
    default:
      w.valid = false;
  }
}

template<typename Fn> void each_id(IDProperty *p, const Fn &fn)
{
  if (!p) {
    return;
  }
  if (p->type == IDP_ID) {
    fn(*p);
  }
  else if (p->type == IDP_GROUP) {
    for (IDProperty &child : p->data.group) {
      each_id(&child, fn);
    }
  }
  else if (p->type == IDP_IDPARRAY) {
    for (int i = 0; i < p->len; i++) {
      each_id(IDP_GetIndexArray(p, i), fn);
    }
  }
  else if (p->type == IDP_ARRAY && p->subtype == IDP_GROUP) {
    auto **items = static_cast<IDProperty **>(p->data.pointer);
    for (int i = 0; i < p->len; i++) {
      each_id(items[i], fn);
    }
  }
}

void curve_data(Writer &w, const CurveMapping *c)
{
  w.value(bool(c));
  if (!c) {
    return;
  }
  w.allocation(c);
  w.value(c->flag & (CUMA_DO_CLIP | CUMA_EXTEND_EXTRAPOLATE | CUMA_USE_WRAPPING));
  w.value(c->clipr);
  w.value(c->tone);
  w.value(c->black);
  w.value(c->white);
  for (const auto &channel : c->cm) {
    w.value(channel.totpoint);
    w.value(channel.default_handle_type);
    w.allocation(channel.curve);
    w.allocation(channel.table);
    w.allocation(channel.premultable);
    if (channel.totpoint < 0 || channel.totpoint > 32767 || (channel.totpoint && !channel.curve)) {
      w.valid = false;
      return;
    }
    for (int i = 0; i < channel.totpoint; i++) {
      const auto &p = channel.curve[i];
      w.value(p.x);
      w.value(p.y);
      w.value(p.flag & (CUMA_HANDLE_VECTOR | CUMA_HANDLE_AUTO_ANIM));
    }
  }
}

struct Field {
  void *ptr;
  size_t size;
};
struct Mask {
  void *ptr;
  size_t size;
  uint64_t mask;
};
struct Access {
  std::vector<Field> fields;
  std::vector<Mask> masks;
  std::vector<CurveMapping **> curves;
  template<typename T> void field(T &v)
  {
    fields.push_back({&v, sizeof(v)});
  }
  template<typename T> void mask(T &v, uint64_t bits)
  {
    masks.push_back({&v, sizeof(v), bits});
  }
};

bool access_native(ID &id, Access &a)
{
  MeshAutomaskingSettings *cavity;
  if (GS(id.name) == ID_BR) {
    auto &b = reinterpret_cast<Brush &>(id);
#define F(f) a.field(b.f)
    F(size);
    F(unprojected_size);
    F(alpha);
    F(spacing);
    F(plane_offset);
    F(crease_pinch_factor);
    F(hardness);
    F(autosmooth_factor);
    F(color);
    F(secondary_color);
    F(add_col);
    F(sub_col);
    F(sculpt_brush_type);
    F(stroke_method);
    F(curve_distance_falloff_preset);
    F(falloff_shape);
#undef F
    a.mask(b.flag,
           BRUSH_ALPHA_PRESSURE | BRUSH_SIZE_PRESSURE | BRUSH_SPACING_PRESSURE | BRUSH_DIR_IN |
               BRUSH_ACCUMULATE | BRUSH_OFFSET_PRESSURE | BRUSH_SPACE_ATTEN | BRUSH_LOCK_SIZE |
               BRUSH_SMOOTH_PRESSURE);
    a.mask(b.unified_paint_flags,
           BRUSH_USE_UNIFIED_PAINT_SIZE | BRUSH_USE_UNIFIED_PAINT_ALPHA |
               BRUSH_USE_UNIFIED_PAINT_COLOR);
    a.curves = {&b.curve_size, &b.curve_strength, &b.curve_distance_falloff};
    cavity = b.mesh_automasking_settings;
  }
  else {
    auto &s = reinterpret_cast<Scene &>(id);
    if (!s.toolsettings || !s.toolsettings->sculpt) {
      return false;
    }
    auto &p = s.toolsettings->sculpt->paint;
    auto &u = p.unified_paint_settings;
    a.field(u.size);
    a.field(u.unprojected_size);
    a.field(u.alpha);
    a.field(u.color);
    a.field(u.secondary_color);
    a.mask(u.flag,
           UNIFIED_PAINT_BRUSH_LOCK_SIZE | UNIFIED_PAINT_SIZE_DEPRECATED |
               UNIFIED_PAINT_ALPHA_DEPRECATED | UNIFIED_PAINT_COLOR_DEPRECATED);
    cavity = p.mesh_automasking_settings;
  }
  if (!cavity) {
    return false;
  }
  a.mask(cavity->flags, BRUSH_AUTOMASKING_CAVITY_ALL | BRUSH_AUTOMASKING_CAVITY_USE_CURVE);
  a.field(cavity->cavity_blur_steps);
  a.field(cavity->cavity_factor);
  a.curves.push_back(&cavity->cavity_curve);
  return true;
}

uint64_t mask_value(const Mask &m)
{
  uint64_t value = 0;
  memcpy(&value, m.ptr, m.size);
  return value & m.mask;
}

struct Snapshot {
  Prop custom, system;
  std::vector<Ref> refs;
  std::vector<char> canonical, scalars;
  std::vector<uint64_t> masks;
  std::vector<Curve> curves;
  bool native = false;
  size_t size = 0;
};

std::unique_ptr<Snapshot> capture(ID &id, bool native, std::string &error)
{
  auto s = std::make_unique<Snapshot>();
  s->native = native;
  Writer w;
  property(w, id.properties, s->refs);
  property(w, id.system_properties, s->refs);
  Access a;
  if (native && !access_native(id, a)) {
    error = "Native sculpt/cavity settings are unavailable";
    return {};
  }
  for (const Field &f : a.fields) {
    w.raw(f.ptr, f.size);
    const char *v = static_cast<const char *>(f.ptr);
    s->scalars.insert(s->scalars.end(), v, v + f.size);
  }
  for (const Mask &m : a.masks) {
    const uint64_t v = mask_value(m);
    w.value(v);
    s->masks.push_back(v);
  }
  for (CurveMapping **c : a.curves) {
    curve_data(w, *c);
  }
  if (!w.valid) {
    error = "Authoring snapshot exceeds supported depth/count/64 MiB bounds";
    return {};
  }
  if (id.properties) {
    s->custom.reset(IDP_CopyProperty_ex(id.properties, LIB_ID_CREATE_NO_USER_REFCOUNT));
  }
  if (id.system_properties) {
    s->system.reset(IDP_CopyProperty_ex(id.system_properties, LIB_ID_CREATE_NO_USER_REFCOUNT));
  }
  auto detach = [](IDProperty &p) { p.data.pointer = nullptr; };
  each_id(s->custom.get(), detach);
  each_id(s->system.get(), detach);
  for (CurveMapping **c : a.curves) {
    s->curves.emplace_back(*c ? BKE_curvemapping_copy(*c) : nullptr);
  }
  s->canonical = std::move(w.bytes);
  /* Recount clones: group hash capacity can differ from the source. */
  Writer cloned;
  std::vector<Ref> unused;
  property(cloned, s->custom.get(), unused);
  property(cloned, s->system.get(), unused);
  for (const Curve &c : s->curves) {
    curve_data(cloned, c.get());
  }
  s->size = sizeof(Snapshot) + cloned.memory + s->canonical.capacity() + s->scalars.capacity() +
            s->refs.capacity() * sizeof(Ref) + s->masks.capacity() * sizeof(uint64_t) +
            s->curves.capacity() * sizeof(Curve);
  if (!cloned.valid || s->size > max_bytes) {
    error = "Authoring snapshot exceeds 64 MiB";
    return {};
  }
  return s;
}

bool references(Main *main, const Snapshot &s, std::vector<ID *> &ids, std::string &error)
{
  for (const Ref &r : s.refs) {
    ID *id = r.uid ? BKE_libblock_find_session_uid(main, r.type, r.uid) : nullptr;
    if (r.uid && !id) {
      error = "Referenced authoring ID was removed or replaced";
      return false;
    }
    ids.push_back(id);
  }
  return true;
}

void notify(bContext *C, ID &id)
{
  BKE_paint_invalidate_overlay_all();
  if (GS(id.name) == ID_BR) {
    BKE_brush_tag_unsaved_changes(reinterpret_cast<Brush *>(&id));
  }
  DEG_id_tag_update(&id, ID_RECALC_SYNC_TO_EVAL);
  WM_event_add_notifier(C, NC_ID | NA_EDITED, &id);
  WM_event_add_notifier(C, NC_BRUSH | NA_EDITED, &id);
}

bool restore(bContext *C, ID &id, const Snapshot &s, std::string &error)
{
  std::vector<ID *> ids;
  if (!references(CTX_data_main(C), s, ids, error)) {
    return false;
  }
  Access a;
  if (s.native && !access_native(id, a)) {
    error = "Native settings were removed";
    return false;
  }
  Prop custom(s.custom ? IDP_CopyProperty_ex(s.custom.get(), LIB_ID_CREATE_NO_USER_REFCOUNT) :
                         nullptr);
  Prop system(s.system ? IDP_CopyProperty_ex(s.system.get(), LIB_ID_CREATE_NO_USER_REFCOUNT) :
                         nullptr);
  std::vector<Curve> curves;
  for (const Curve &c : s.curves) {
    curves.emplace_back(c ? BKE_curvemapping_copy(c.get()) : nullptr);
  }
  size_t index = 0;
  auto attach = [&](IDProperty &p) { p.data.pointer = ids[index++]; };
  each_id(custom.get(), attach);
  each_id(system.get(), attach);
  auto add_user = [](IDProperty &p) {
    if (ID *target = IDP_ID_get(&p)) {
      id_us_plus(target);
    }
  };
  each_id(custom.get(), add_user);
  each_id(system.get(), add_user);
  if (owner_idprop_invalidate_hook) {
    owner_idprop_invalidate_hook(&id);
  }
  if (id.properties) {
    IDP_FreeProperty(id.properties);
  }
  if (id.system_properties) {
    IDP_FreeProperty(id.system_properties);
  }
  id.properties = custom.release();
  id.system_properties = system.release();
  size_t offset = 0;
  for (const Field &f : a.fields) {
    memcpy(f.ptr, s.scalars.data() + offset, f.size);
    offset += f.size;
  }
  for (size_t i = 0; i < a.masks.size(); i++) {
    const Mask &m = a.masks[i];
    uint64_t current = 0;
    memcpy(&current, m.ptr, m.size);
    current = (current & ~m.mask) | s.masks[i];
    memcpy(m.ptr, &current, m.size);
  }
  for (size_t i = 0; i < a.curves.size(); i++) {
    if (*a.curves[i]) {
      BKE_curvemapping_free(*a.curves[i]);
    }
    *a.curves[i] = curves[i].release();
  }
  revisions[id.session_uid]++;
  notify(C, id);
  return true;
}

struct Payload {
  uint32_t uid;
  short type;
  std::unique_ptr<Snapshot> before, after;
};
struct AuthoringUndoStep {
  UndoStep step;
  UndoRefID owner;
  Payload *payload;
};
std::unique_ptr<Payload> pending;

bool encode(bContext *, Main *main, UndoStep *step)
{
  auto &s = *reinterpret_cast<AuthoringUndoStep *>(step);
  BLI_assert(pending);
  s.owner.ptr = BKE_libblock_find_session_uid(main, pending->type, pending->uid);
  s.step.use_memfile_step = true;
  s.step.data_size = sizeof(Payload) + pending->before->size + pending->after->size;
  s.payload = pending.release();
  return true;
}
void decode(bContext *C, Main *main, UndoStep *step, eUndoStepDir dir, bool)
{
  auto &p = *reinterpret_cast<AuthoringUndoStep *>(step)->payload;
  ID *id = BKE_libblock_find_session_uid(main, p.type, p.uid);
  std::string error;
  if (!id || !ID_IS_EDITABLE(id) ||
      !restore(C, *id, dir == STEP_UNDO ? *p.before : *p.after, error))
  {
    fprintf(stderr,
            "Authoring undo skipped: %s\n",
            id ? error.c_str() : "owner was removed or replaced");
  }
}
void free_step(UndoStep *step)
{
  delete reinterpret_cast<AuthoringUndoStep *>(step)->payload;
}
void foreach_ref(UndoStep *step, UndoTypeForEachIDRefFn fn, void *data)
{
  fn(data, &reinterpret_cast<AuthoringUndoStep *>(step)->owner);
}

void invalidate(Main *, PointerRNA **, int, void *)
{
  ++epoch;
  active.clear();
  revisions.clear();
}
void ensure_callbacks()
{
  static bool initialized = false;
  static bCallbackFuncStore callbacks[3] = {};
  if (initialized) {
    return;
  }
  const eCbEvent events[] = {BKE_CB_EVT_LOAD_PRE, BKE_CB_EVT_UNDO_PRE, BKE_CB_EVT_REDO_PRE};
  for (int i = 0; i < 3; i++) {
    callbacks[i].func = invalidate;
    BKE_callback_add(&callbacks[i], events[i]);
  }
  initialized = true;
}

bool interactive(bContext *C, std::string &error)
{
  UndoStack *stack = ED_undo_stack_get();
  wmWindowManager *wm = CTX_wm_manager(C);
  if (!stack || !wm || !(U.uiflag & USER_GLOBALUNDO) || U.undosteps <= 0 ||
      !ED_undo_is_memfile_compatible(C) || wm->op_undo_depth || stack->step_init ||
      stack->group_level)
  {
    error =
        "Authoring undo requires global undo, a compatible context, and no pending/nested undo "
        "operator";
    return false;
  }
  return true;
}
}  // namespace

struct AuthoringEdit {
  uint64_t generation, revision;
  uint32_t uid;
  short type;
  Main *main;
  const UndoStep *baseline;
  bool undo;
  bool consumed = false;
  std::unique_ptr<Snapshot> before;
};

uint64_t authoring_revision(const ID &owner)
{
  const auto it = revisions.find(owner.session_uid);
  return it == revisions.end() ? 0 : it->second;
}

bool authoring_curve_key(const CurveMapping &mapping, std::string &key)
{
  Writer w;
  curve_data(w, &mapping);
  if (!w.valid) {
    return false;
  }
  key.assign(w.bytes.begin(), w.bytes.end());
  return true;
}

std::shared_ptr<AuthoringEdit> authoring_edit_begin(
    bContext *C, ID &owner, bool native, bool undo, std::string &error)
{
  if (!BLI_thread_is_main() || !ELEM(GS(owner.name), ID_BR, ID_SCE) ||
      !BKE_id_is_in_main(CTX_data_main(C), &owner) || !ID_IS_EDITABLE(&owner) ||
      ID_IS_OVERRIDE_LIBRARY(&owner) || (owner.tag & ID_TAG_COPIED_ON_EVAL))
  {
    error = "Authoring edits require an editable original Brush/Scene on the main thread";
    return {};
  }
  ensure_callbacks();
  if (auto current = active[owner.session_uid].lock(); current && !current->consumed) {
    error = "An authoring edit is already active on this owner";
    return {};
  }
  if (undo && !interactive(C, error)) {
    return {};
  }
  auto before = capture(owner, native, error);
  if (!before) {
    return {};
  }
  if (undo && !ED_undo_stack_get()->step_active) {
    BKE_undosys_stack_init_from_main(ED_undo_stack_get(), CTX_data_main(C));
  }
  auto edit = std::make_shared<AuthoringEdit>();
  edit->generation = epoch;
  edit->revision = authoring_revision(owner);
  edit->uid = owner.session_uid;
  edit->type = GS(owner.name);
  edit->main = CTX_data_main(C);
  edit->undo = undo;
  edit->before = std::move(before);
  edit->baseline = undo ? ED_undo_stack_get()->step_active : nullptr;
  active[owner.session_uid] = edit;
  return edit;
}

bool authoring_edit_finish(bContext *C,
                           ID &owner,
                           AuthoringEdit &edit,
                           bool commit,
                           const char *name,
                           bool &changed,
                           std::string &error)
{
  changed = false;
  if (!BLI_thread_is_main() || edit.consumed || edit.generation != epoch ||
      edit.main != CTX_data_main(C) || edit.uid != owner.session_uid ||
      edit.revision != authoring_revision(owner) || !BKE_id_is_in_main(CTX_data_main(C), &owner) ||
      !ID_IS_EDITABLE(&owner) || ID_IS_OVERRIDE_LIBRARY(&owner))
  {
    error = "Authoring token is stale or belongs to another owner";
    return false;
  }
  auto after = capture(owner, edit.before->native, error);
  if (!after) {
    return false;
  }
  changed = edit.before->canonical != after->canonical;
  if (!changed) {
    edit.consumed = true;
    edit.before.reset();
    return true;
  }
  std::vector<ID *> refs;
  if (!references(edit.main, *edit.before, refs, error)) {
    return false;
  }
  if (!commit) {
    if (!restore(C, owner, *edit.before, error)) {
      return false;
    }
  }
  else if (edit.undo) {
    if (!interactive(C, error)) {
      return false;
    }
    UndoStack *stack = ED_undo_stack_get();
    if (stack->step_active != edit.baseline) {
      error = "Undo history changed during the authoring edit";
      return false;
    }
    if (stack->step_active && !stack->step_active->next) {
      BKE_undosys_stack_limit_steps_and_memory(stack, U.undosteps - 1, 0);
    }
    pending = std::make_unique<Payload>();
    pending->uid = edit.uid;
    pending->type = edit.type;
    pending->before = std::move(edit.before);
    pending->after = std::move(after);
    const auto result = BKE_undosys_step_push_with_type(
        stack, C, name, UndoEncodeHints::None, authoring_type);
    BLI_assert(result & UNDO_PUSH_RET_SUCCESS);
    UNUSED_VARS(result);
    if (U.undomemory) {
      BKE_undosys_stack_limit_steps_and_memory(stack, -1, size_t(U.undomemory) * 1024 * 1024);
    }
    WM_event_add_notifier(C, NC_WM | ND_UNDO, nullptr);
  }
  edit.consumed = true;
  edit.before.reset();
  return true;
}

void authoring_register_owner_invalidate_hook(std::function<void(const ID *)> hook)
{
  owner_idprop_invalidate_hook = std::move(hook);
}

void authoring_undosys_type(UndoType *type)
{
  authoring_type = type;
  type->identifier = "PROPERTY_AUTHORING";
  type->poll = nullptr;
  type->step_encode = encode;
  type->step_decode = decode;
  type->step_free = free_step;
  type->step_foreach_ID_ref = foreach_ref;
  type->flags = UNDOTYPE_FLAG_NEED_CONTEXT_FOR_ENCODE | UNDOTYPE_FLAG_DECODE_ACTIVE_STEP;
  type->step_size = sizeof(AuthoringUndoStep);
}
}  // namespace blender::ed
