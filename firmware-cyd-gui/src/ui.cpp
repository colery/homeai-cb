#include "ui.h"
#include <lvgl.h>
#include <stdio.h>
#include <string.h>

// ── Theme ──────────────────────────────────────────────────────────────────
#define COL_BG_TOP   0x0d1226
#define COL_BG_BOT   0x04060d
#define COL_CARD_T   0x1a2342
#define COL_CARD_B   0x121930
#define COL_BORDER   0x26315a
#define COL_TITLEBAR 0x1f2a4d
#define COL_TEXT     0xe8eeff
#define COL_TEXT2    0x93a0c8
#define COL_DIM      0x6f7ca8
#define COL_USB      0x2ee6d6
#define COL_BLE      0xb48cff
#define COL_OK       0x3ddc84
#define COL_BAD      0xff5d6c

static const uint32_t ACC[4] = {0x7b86a8, 0x4aa8ff, 0x3ddc84, 0xffb02e};
static const char*    LBL[4] = {"SLEEPING", "IDLE", "WORKING", "NEEDS YOU"};

#define F12 (&lv_font_montserrat_12)
#define F14 (&lv_font_montserrat_14)
#define F16 (&lv_font_montserrat_16)
#define F20 (&lv_font_montserrat_20)
#define F28 (&lv_font_montserrat_28)

static inline lv_color_t C(uint32_t h) { return lv_color_hex(h); }

// ── Small builders ─────────────────────────────────────────────────────────
static lv_obj_t* plain(lv_obj_t* parent, int x, int y, int w, int h) {
  lv_obj_t* o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  return o;
}

static lv_obj_t* card(lv_obj_t* parent, int x, int y, int w, int h) {
  lv_obj_t* o = plain(parent, x, y, w, h);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(o, C(COL_CARD_T), 0);
  lv_obj_set_style_bg_grad_color(o, C(COL_CARD_B), 0);
  lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_radius(o, 14, 0);
  lv_obj_set_style_border_width(o, 1, 0);
  lv_obj_set_style_border_color(o, C(COL_BORDER), 0);
  lv_obj_set_style_clip_corner(o, true, 0);
  return o;
}

static lv_obj_t* label(lv_obj_t* parent, const char* txt, const lv_font_t* f, uint32_t col) {
  lv_obj_t* l = lv_label_create(parent);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, C(col), 0);
  return l;
}

static lv_obj_t* dot(lv_obj_t* parent, int x, int y, int d, uint32_t col) {
  lv_obj_t* o = plain(parent, x, y, d, d);
  lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(o, C(col), 0);
  return o;
}

static void setFlag(lv_obj_t* o, lv_obj_flag_t f, bool on) {
  if (on) lv_obj_add_flag(o, f); else lv_obj_remove_flag(o, f);
}

static void setText(lv_obj_t* l, const char* t) {
  if (strcmp(lv_label_get_text(l), t) != 0) lv_label_set_text(l, t);
}

// The built-in "dots" long mode for labels loops forever while rendering on the ESP32 (it
// worked on the host), so labels are hard-clipped and long text is cut with "..." here.
static void clipText(lv_obj_t* l, const char* txt, int maxw) {
  const lv_font_t* f = lv_obj_get_style_text_font(l, LV_PART_MAIN);
  char buf[100];
  size_t n = strlen(txt);
  if (n >= sizeof buf - 4) n = sizeof buf - 4;
  memcpy(buf, txt, n);
  buf[n] = 0;
  if ((int)lv_text_get_width(buf, n, f, 0) > maxw) {
    int dots = (int)lv_text_get_width("...", 3, f, 0);
    while (n > 1 && (int)lv_text_get_width(buf, n, f, 0) + dots > maxw) n--;
    while (n > 0 && (buf[n] & 0xC0) == 0x80) n--;     // never cut inside a UTF-8 sequence
    buf[n] = 0;
    strcat(buf, "...");
  }
  setText(l, buf);
}

static void fmtTok(char* o, size_t n, uint32_t t) {
  if (t >= 1000000)      snprintf(o, n, "%.1fM", t / 1e6f);
  else if (t >= 10000)   snprintf(o, n, "%luK", (unsigned long)(t / 1000));
  else if (t >= 1000)    snprintf(o, n, "%.1fK", t / 1e3f);
  else                   snprintf(o, n, "%lu", (unsigned long)t);
}

// ── Widget handles ─────────────────────────────────────────────────────────
static UiPermCb     g_onPerm;
static UiDismissCb  g_onDismiss;
static UiState      g_state = (UiState)-1;
static int          g_tab = 0;
static lv_obj_t*    g_tv;

// status bar
static lv_obj_t *sb_time, *sb_usb, *sb_ble;

// home / hero
static lv_obj_t *hero, *hero_dots[3], *hero_title, *ring, *head, *eyeL, *eyeR, *mouth, *nose;
static lv_obj_t *whiskers[4], *badge_z, *badge_bang, *chip, *chip_lbl;
static lv_obj_t *earL, *earR;

// home / plan usage inside the buddy window
static lv_obj_t *gb_frame[2], *gb_fill[2], *gb_lbl[2], *hero_ctx;

// home / one card per connected Claude instance
struct Inst { lv_obj_t *dot, *name, *chip, *chip_lbl, *msg, *entry, *tok, *toktxt; };
static Inst inst[2];

// stats tab
static lv_obj_t *st_tok, *st_bar_u, *st_bar_b, *st_leg_u, *st_leg_b, *st_life;
static lv_obj_t *st_ses_u, *st_ses_b;
static lv_obj_t *st_allow_bar, *st_deny_bar, *st_allow_n, *st_deny_n, *st_rate;
static lv_obj_t *st_link, *st_up, *st_beat, *st_dev;

// log tab
static lv_obj_t* log_lbl;
static uint32_t  g_logSeq = 0xFFFFFFFF;

// overlays
static lv_obj_t *at_cap, *ov_attn, *at_card, *at_tool, *at_hint, *at_note, *at_btn_info, *at_btn_deny, *at_btn_allow;
static lv_obj_t *ov_link, *lk_name, *lk_status, *lk_key, *lk_spin, *lk_usb, *lk_ble;

// ── Mascot ─────────────────────────────────────────────────────────────────
#define MS_CX 112
#define MS_CY 63

LV_DRAW_BUF_DEFINE_STATIC(ear_buf_l, 24, 22, LV_COLOR_FORMAT_ARGB8888);
LV_DRAW_BUF_DEFINE_STATIC(ear_buf_r, 24, 22, LV_COLOR_FORMAT_ARGB8888);

static lv_obj_t* make_ear(lv_obj_t* parent, lv_draw_buf_t* buf, bool left) {
  lv_obj_t* c = lv_canvas_create(parent);
  lv_canvas_set_draw_buf(c, buf);
  lv_canvas_fill_bg(c, C(0x000000), LV_OPA_TRANSP);
  lv_layer_t layer;
  lv_canvas_init_layer(c, &layer);

  lv_draw_triangle_dsc_t t;
  lv_draw_triangle_dsc_init(&t);
  t.color = C(0x34437a);
  t.opa = LV_OPA_COVER;
  if (left) { t.p[0] = {1, 21};  t.p[1] = {4, 1};  t.p[2] = {22, 21}; }
  else      { t.p[0] = {22, 21}; t.p[1] = {19, 1}; t.p[2] = {1, 21}; }
  lv_draw_triangle(&layer, &t);

  lv_draw_triangle_dsc_t in;
  lv_draw_triangle_dsc_init(&in);
  in.color = C(0x8a4f78);
  in.opa = LV_OPA_COVER;
  if (left) { in.p[0] = {6, 20};  in.p[1] = {8, 8};  in.p[2] = {17, 20}; }
  else      { in.p[0] = {17, 20}; in.p[1] = {15, 8}; in.p[2] = {6, 20}; }
  lv_draw_triangle(&layer, &in);

  lv_canvas_finish_layer(c, &layer);
  return c;
}

static lv_obj_t* make_line(lv_obj_t* parent, const lv_point_precise_t* pts, int n, int w, uint32_t col) {
  lv_obj_t* l = lv_line_create(parent);
  lv_line_set_points(l, pts, n);
  lv_obj_set_style_line_width(l, w, 0);
  lv_obj_set_style_line_color(l, C(col), 0);
  lv_obj_set_style_line_rounded(l, true, 0);
  return l;
}

static lv_obj_t *eyes[2], *pups[2], *dots3[3], *mas, *mouth_o;
static inline lv_obj_t* mouth_arc() { return mouth_o; }

static void make_eye(lv_obj_t* head_, int i, int dx) {
  lv_obj_t* e = plain(head_, 0, 0, 10, 14);
  lv_obj_set_style_radius(e, 5, 0);
  lv_obj_set_style_bg_opa(e, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(e, C(0xeaf0ff), 0);
  lv_obj_align(e, LV_ALIGN_CENTER, dx, -4);
  lv_obj_t* p = plain(e, 0, 0, 6, 7);
  lv_obj_set_style_radius(p, 3, 0);
  lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(p, C(0x141b3a), 0);
  lv_obj_center(p);
  eyes[i] = e;
  pups[i] = p;
}

static uint32_t g_acc = 0x4aa8ff;
static void anim_ring_rot(void* o, int32_t v) { lv_arc_set_rotation((lv_obj_t*)o, v); }
// Pulses recolor instead of fading object opacity: an opacity < 255 makes LVGL render the
// object into a temporary layer buffer, which the ESP32 (BLE running) has no spare RAM for.
static void anim_ring_pulse(void* o, int32_t v) {
  lv_obj_set_style_arc_color((lv_obj_t*)o, lv_color_mix(C(g_acc), C(0x1c2546), (uint8_t)v), LV_PART_INDICATOR);
}
static void anim_bg_pulse(void* o, int32_t v) {
  lv_obj_set_style_bg_color((lv_obj_t*)o, lv_color_mix(C(ACC[UI_ATTN]), C(0x4a3208), (uint8_t)v), 0);
}
static void anim_y_off(void* o, int32_t v)  { lv_obj_set_style_translate_y((lv_obj_t*)o, v, 0); }
static void anim_x_off(void* o, int32_t v)  { lv_obj_set_style_translate_x((lv_obj_t*)o, v, 0); }
static void anim_bounce(void* o, int32_t v) { lv_obj_set_style_translate_y((lv_obj_t*)o, v, 0); }
static void anim_eye_h(void* o, int32_t v) {
  lv_obj_t* e = (lv_obj_t*)o;
  lv_obj_set_height(e, v);
  lv_obj_t* p = lv_obj_get_child(e, 0);
  if (p) { if (v < 8) lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN); else lv_obj_remove_flag(p, LV_OBJ_FLAG_HIDDEN); }
}
static void anim_shadow(void* o, int32_t v) { lv_obj_set_style_shadow_width((lv_obj_t*)o, v, 0); }

static int  g_eyeH = 14;
static bool g_happy = false;
static uint32_t g_happyUntil = 0, g_nextLook = 0, g_nextTwitch = 0;
static int  look_cx = 0, look_cy = 0, look_tx = 0, look_ty = 0;   // quarter pixels

static void apply_eyes(UiState s, bool happy) {
  bool sleep = s == UI_SLEEP, busy = s == UI_BUSY, attn = s == UI_ATTN;
  int ew = 10, eh = 14; bool pup = true;
  if (sleep)      { eh = 2;  pup = false; }
  else if (busy)  { ew = 12; eh = 8; }
  else if (attn)  { ew = 11; eh = 18; }
  if (happy)      { ew = 12; eh = 4; pup = false; }
  g_eyeH = eh;
  uint32_t ec = busy ? 0xc8ffe0 : (sleep ? 0x8794bd : 0xeaf0ff);
  for (int i = 0; i < 2; i++) {
    lv_anim_delete(eyes[i], anim_eye_h);
    lv_obj_set_size(eyes[i], ew, eh);
    lv_obj_set_style_bg_color(eyes[i], C(ec), 0);
    lv_obj_align(eyes[i], LV_ALIGN_CENTER, i ? 11 : -11, sleep ? -2 : -4);
    if (pup) lv_obj_remove_flag(pups[i], LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(pups[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_center(pups[i]);
  }
  // mouth
  if (happy)      { lv_arc_set_bg_angles(mouth_arc(), 15, 165); lv_arc_set_angles(mouth_arc(), 15, 165); lv_obj_set_size(mouth_arc(), 22, 22); lv_obj_align(mouth_arc(), LV_ALIGN_CENTER, 0, 5); }
  else if (attn)  { lv_arc_set_bg_angles(mouth_arc(), 0, 360); lv_arc_set_angles(mouth_arc(), 0, 360); lv_obj_set_size(mouth_arc(), 10, 10); lv_obj_align(mouth_arc(), LV_ALIGN_CENTER, 0, 12); }
  else if (sleep) { lv_arc_set_bg_angles(mouth_arc(), 60, 120); lv_arc_set_angles(mouth_arc(), 60, 120); lv_obj_set_size(mouth_arc(), 18, 18); lv_obj_align(mouth_arc(), LV_ALIGN_CENTER, 0, 6); }
  else            { lv_arc_set_bg_angles(mouth_arc(), 30, 150); lv_arc_set_angles(mouth_arc(), 30, 150); lv_obj_set_size(mouth_arc(), 18, 18); lv_obj_align(mouth_arc(), LV_ALIGN_CENTER, 0, 6); }
  lv_obj_set_style_arc_color(mouth_arc(), C(attn ? ACC[s] : 0xeaf0ff), LV_PART_INDICATOR);
}

static void blink_cb(lv_timer_t*) {
  if ((g_state != UI_IDLE && g_state != UI_ATTN) || g_happy) return;
  for (int i = 0; i < 2; i++) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, eyes[i]);
    lv_anim_set_exec_cb(&a, anim_eye_h);
    lv_anim_set_values(&a, g_eyeH, 2);
    lv_anim_set_duration(&a, 90);
    lv_anim_set_reverse_duration(&a, 120);
    lv_anim_start(&a);
  }
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void twitch_ear(lv_obj_t* ear) {
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, ear);
  lv_anim_set_exec_cb(&a, anim_y_off);
  lv_anim_set_values(&a, 0, -3);
  lv_anim_set_duration(&a, 110);
  lv_anim_set_reverse_duration(&a, 140);
  lv_anim_start(&a);
}

// Runs every 50 ms: where the pupils look (touch, idle wandering, "reading" while working),
// the thinking dots, and now and then an ear twitch.
static void mascot_tick(lv_timer_t*) {
  uint32_t now = lv_tick_get();

  bool touching = false; lv_point_t tp = {0, 0};
  lv_indev_t* in = lv_indev_get_next(NULL);
  if (in && lv_indev_get_state(in) == LV_INDEV_STATE_PRESSED) { lv_indev_get_point(in, &tp); touching = true; }

  lv_area_t ar; lv_obj_get_coords(head, &ar);
  int hx = (ar.x1 + ar.x2) / 2, hy = (ar.y1 + ar.y2) / 2;

  if (g_state == UI_SLEEP) { look_tx = look_ty = 0; }
  else if (touching)       { look_tx = clampi((tp.x - hx) / 10, -2, 2) * 4; look_ty = clampi((tp.y - hy) / 16, -3, 3) * 4; }
  else if (g_state == UI_BUSY) { look_tx = ((now / 480) & 1) ? 8 : -8; look_ty = 4; }
  else if (g_state == UI_ATTN) { look_tx = look_ty = 0; }
  else if (now > g_nextLook) {
    static const int8_t L[6][2] = {{0, 0}, {-8, 0}, {8, 0}, {-8, -8}, {8, -8}, {0, 8}};
    int k = lv_rand(0, 5);
    look_tx = L[k][0]; look_ty = L[k][1];
    g_nextLook = now + lv_rand(1600, 4200);
  }
  int dx = look_tx - look_cx, dy = look_ty - look_cy;
  look_cx += (dx > 1 || dx < -1) ? dx / 2 : dx;
  look_cy += (dy > 1 || dy < -1) ? dy / 2 : dy;
  for (int i = 0; i < 2; i++) {
    lv_obj_set_style_translate_x(pups[i], look_cx / 4, 0);
    lv_obj_set_style_translate_y(pups[i], look_cy / 4, 0);
  }

  bool happy = now < g_happyUntil;
  if (happy != g_happy) { g_happy = happy; apply_eyes(g_state, happy); }

  if (g_state == UI_BUSY) {
    int lit = (now / 260) % 4;
    for (int i = 0; i < 3; i++) lv_obj_set_style_bg_color(dots3[i], C(i < lit ? ACC[UI_BUSY] : 0x2c3a62), 0);
  }

  if ((g_state == UI_IDLE || g_state == UI_ATTN) && now > g_nextTwitch) {
    twitch_ear(lv_rand(0, 1) ? earL : earR);
    g_nextTwitch = now + lv_rand(3500, 8500);
  }
}

static void hero_tap_cb(lv_event_t*) {
  if (g_state == UI_SLEEP) return;
  g_happyUntil = lv_tick_get() + 900;
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, mas);
  lv_anim_set_exec_cb(&a, anim_bounce);
  lv_anim_set_values(&a, 0, -9);
  lv_anim_set_duration(&a, 170);
  lv_anim_set_reverse_duration(&a, 260);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_start(&a);
}

static void build_mascot(lv_obj_t* parent, int cx, int cy) {
  ring = lv_arc_create(parent);
  lv_obj_set_size(ring, 82, 82);
  lv_obj_set_pos(ring, cx - 41, cy - 41);
  lv_arc_set_bg_angles(ring, 0, 360);
  lv_arc_set_angles(ring, 0, 360);
  lv_obj_remove_style(ring, NULL, LV_PART_KNOB);
  lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(ring, 4, LV_PART_MAIN);
  lv_obj_set_style_arc_width(ring, 4, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(ring, C(0x1c2546), LV_PART_MAIN);
  lv_obj_set_style_arc_rounded(ring, true, LV_PART_INDICATOR);

  // everything that bounces when tapped lives in one container (local origin = (cx-48, cy-42))
  mas = plain(parent, cx - 48, cy - 42, 96, 84);
  lv_obj_remove_flag(mas, LV_OBJ_FLAG_CLICKABLE);
  const int ox = 48, oy = 42;

  earL = make_ear(mas, &ear_buf_l, true);
  earR = make_ear(mas, &ear_buf_r, false);
  lv_obj_set_pos(earL, ox - 28, oy - 35);
  lv_obj_set_pos(earR, ox + 4,  oy - 35);

  head = plain(mas, ox - 29, oy - 20, 58, 48);
  lv_obj_set_style_radius(head, 21, 0);
  lv_obj_set_style_bg_opa(head, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(head, C(0x2b3868), 0);
  lv_obj_set_style_bg_grad_color(head, C(0x1a2347), 0);
  lv_obj_set_style_bg_grad_dir(head, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_border_width(head, 2, 0);

  make_eye(head, 0, -11);
  make_eye(head, 1, 11);
  eyeL = eyes[0]; eyeR = eyes[1];

  nose = plain(head, 0, 0, 6, 4);
  lv_obj_set_style_radius(nose, 2, 0);
  lv_obj_set_style_bg_opa(nose, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(nose, C(0xe98fb4), 0);
  lv_obj_align(nose, LV_ALIGN_CENTER, 0, 8);

  mouth = lv_arc_create(head);
  mouth_o = mouth;
  lv_obj_set_size(mouth, 18, 18);
  lv_arc_set_bg_angles(mouth, 30, 150);
  lv_arc_set_angles(mouth, 30, 150);
  lv_obj_remove_style(mouth, NULL, LV_PART_KNOB);
  lv_obj_remove_flag(mouth, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(mouth, 2, LV_PART_MAIN);
  lv_obj_set_style_arc_width(mouth, 2, LV_PART_INDICATOR);
  lv_obj_set_style_arc_opa(mouth, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_arc_color(mouth, C(0xeaf0ff), LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(mouth, true, LV_PART_INDICATOR);
  lv_obj_align(mouth, LV_ALIGN_CENTER, 0, 6);

  static const lv_point_precise_t wl1[] = {{0, 2}, {12, 0}};
  static const lv_point_precise_t wl2[] = {{0, 0}, {12, 5}};
  static const lv_point_precise_t wr1[] = {{0, 0}, {12, 2}};
  static const lv_point_precise_t wr2[] = {{0, 5}, {12, 0}};
  whiskers[0] = make_line(mas, wl1, 2, 1, 0x7e8cb8);
  whiskers[1] = make_line(mas, wl2, 2, 1, 0x7e8cb8);
  whiskers[2] = make_line(mas, wr1, 2, 1, 0x7e8cb8);
  whiskers[3] = make_line(mas, wr2, 2, 1, 0x7e8cb8);
  lv_obj_set_pos(whiskers[0], ox - 46, oy + 3);
  lv_obj_set_pos(whiskers[1], ox - 46, oy + 10);
  lv_obj_set_pos(whiskers[2], ox + 34, oy + 3);
  lv_obj_set_pos(whiskers[3], ox + 34, oy + 10);

  badge_z = label(parent, "z Z", F14, 0x9fb0e0);
  lv_obj_set_pos(badge_z, cx + 20, cy - 40);

  badge_bang = plain(parent, cx + 24, cy - 42, 18, 18);
  lv_obj_set_style_radius(badge_bang, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(badge_bang, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(badge_bang, C(ACC[UI_ATTN]), 0);
  lv_obj_t* bl = label(badge_bang, "!", F14, 0x1a1200);
  lv_obj_center(bl);

  for (int i = 0; i < 3; i++) {
    dots3[i] = dot(parent, cx + 26 + i * 8, cy - 38, 5, 0x2c3a62);
    lv_obj_add_flag(dots3[i], LV_OBJ_FLAG_HIDDEN);
  }

  lv_obj_add_flag(hero, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(hero, hero_tap_cb, LV_EVENT_CLICKED, NULL);

  lv_timer_create(blink_cb, 3600, NULL);
  lv_timer_create(mascot_tick, 50, NULL);
}

static void mascot_state(UiState s) {
  uint32_t acc = ACC[s];
  lv_anim_delete(ring, anim_ring_rot);
  lv_anim_delete(ring, anim_ring_pulse);
  lv_anim_delete(badge_bang, anim_bg_pulse);
  lv_anim_delete(badge_z, anim_y_off);
  lv_anim_delete(head, anim_y_off);
  lv_anim_delete(head, anim_x_off);
  lv_obj_set_style_translate_y(head, 0, 0);
  lv_obj_set_style_translate_x(head, 0, 0);
  g_acc = acc;

  lv_obj_set_style_arc_color(ring, C(acc), LV_PART_INDICATOR);
  lv_obj_set_style_border_color(head, C(acc), 0);

  bool sleep = s == UI_SLEEP, busy = s == UI_BUSY, attn = s == UI_ATTN;
  setFlag(badge_z, LV_OBJ_FLAG_HIDDEN, !sleep);
  setFlag(badge_bang, LV_OBJ_FLAG_HIDDEN, !attn);
  for (int i = 0; i < 3; i++) setFlag(dots3[i], LV_OBJ_FLAG_HIDDEN, !busy);

  apply_eyes(s, g_happy && !sleep);

  // ring + animation
  lv_anim_t a;
  if (busy) {
    lv_arc_set_angles(ring, 0, 110);
    lv_anim_init(&a);
    lv_anim_set_var(&a, ring);
    lv_anim_set_exec_cb(&a, anim_ring_rot);
    lv_anim_set_values(&a, 0, 359);
    lv_anim_set_duration(&a, 1100);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_start(&a);
  } else {
    lv_arc_set_rotation(ring, 0);
    lv_arc_set_angles(ring, 0, sleep ? 0 : 360);
    if (!sleep) {
      lv_anim_init(&a);
      lv_anim_set_var(&a, ring);
      lv_anim_set_exec_cb(&a, anim_ring_pulse);
      lv_anim_set_values(&a, attn ? 70 : 110, 255);
      lv_anim_set_duration(&a, attn ? 520 : 1800);
      lv_anim_set_reverse_duration(&a, attn ? 520 : 1800);
      lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
      lv_anim_start(&a);
    }
  }
  if (attn) {
    lv_anim_init(&a);
    lv_anim_set_var(&a, badge_bang);
    lv_anim_set_exec_cb(&a, anim_bg_pulse);
    lv_anim_set_values(&a, 40, 255);
    lv_anim_set_duration(&a, 450);
    lv_anim_set_reverse_duration(&a, 450);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
    // a short wiggle to get attention
    lv_anim_init(&a);
    lv_anim_set_var(&a, head);
    lv_anim_set_exec_cb(&a, anim_x_off);
    lv_anim_set_values(&a, -2, 2);
    lv_anim_set_duration(&a, 70);
    lv_anim_set_reverse_duration(&a, 70);
    lv_anim_set_repeat_count(&a, 8);
    lv_anim_start(&a);
  }
  if (sleep) {
    lv_anim_init(&a);
    lv_anim_set_var(&a, badge_z);
    lv_anim_set_exec_cb(&a, anim_y_off);
    lv_anim_set_values(&a, 0, -5);
    lv_anim_set_duration(&a, 1400);
    lv_anim_set_reverse_duration(&a, 1400);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
  }
  // breathing (slow, deep asleep) / typing bob (working)
  if (!attn) {
    lv_anim_init(&a);
    lv_anim_set_var(&a, head);
    lv_anim_set_exec_cb(&a, anim_y_off);
    lv_anim_set_values(&a, 0, sleep ? 2 : -1);
    lv_anim_set_duration(&a, sleep ? 2600 : (busy ? 260 : 1900));
    lv_anim_set_reverse_duration(&a, sleep ? 2600 : (busy ? 260 : 1900));
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
  }

  // hero chrome
  for (int i = 0; i < 3; i++) lv_obj_set_style_opa(hero_dots[i], sleep ? LV_OPA_40 : LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(chip, C(acc), 0);
  lv_obj_set_style_bg_opa(chip, LV_OPA_20, 0);
  lv_obj_set_style_border_color(chip, C(acc), 0);
  lv_obj_set_style_text_color(chip_lbl, C(acc), 0);
  setText(chip_lbl, LBL[s]);
}


// ── Smooth value changes ───────────────────────────────────────────────────
struct NumAnim { lv_obj_t* lbl; uint32_t cur, tgt; };
static NumAnim n_tok_stats;
static void num_exec(void* v, int32_t x) {
  NumAnim* n = (NumAnim*)v;
  n->cur = (uint32_t)x;
  char t[16]; fmtTok(t, sizeof t, n->cur);
  setText(n->lbl, t);
}
static void setNum(NumAnim& n, uint32_t target) {
  if (n.tgt == target) return;
  n.tgt = target;
  lv_anim_delete(&n, num_exec);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, &n);
  lv_anim_set_exec_cb(&a, num_exec);
  lv_anim_set_values(&a, (int32_t)n.cur, (int32_t)target);
  lv_anim_set_duration(&a, 700);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_start(&a);
}

struct SplitBar { lv_obj_t *u, *b; int su, sb, tu, tb, cu, cb; };
static SplitBar sb_stats;
static void split_exec(void* v, int32_t p) {
  SplitBar* s = (SplitBar*)v;
  s->cu = s->su + (s->tu - s->su) * p / 256;
  s->cb = s->sb + (s->tb - s->sb) * p / 256;
  lv_obj_set_width(s->u, s->cu);
  lv_obj_set_x(s->b, s->cu);
  lv_obj_set_width(s->b, s->cb);
}
static void setSplit(SplitBar& s, int w, uint32_t u, uint32_t b) {
  uint32_t t = u + b;
  int tu = t ? (int)((uint64_t)u * w / t) : 0;
  int tb = t ? w - tu : 0;
  if (tu == s.tu && tb == s.tb) return;
  s.su = s.cu; s.sb = s.cb; s.tu = tu; s.tb = tb;
  lv_anim_delete(&s, split_exec);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, &s);
  lv_anim_set_exec_cb(&a, split_exec);
  lv_anim_set_values(&a, 0, 256);
  lv_anim_set_duration(&a, 600);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_start(&a);
}

struct WBar { lv_obj_t* o; int cur, tgt; };
static WBar wb_allow, wb_deny;
static void wbar_exec(void* v, int32_t x) { WBar* w = (WBar*)v; w->cur = x; lv_obj_set_width(w->o, x); }
static void setWBar(WBar& w, int target) {
  if (w.tgt == target) return;
  w.tgt = target;
  lv_anim_delete(&w, wbar_exec);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, &w);
  lv_anim_set_exec_cb(&a, wbar_exec);
  lv_anim_set_values(&a, w.cur, target);
  lv_anim_set_duration(&a, 500);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_start(&a);
}

// ── Pieces ─────────────────────────────────────────────────────────────────
static void build_statusbar(lv_obj_t* scr) {
  lv_obj_t* sb = plain(scr, 0, 0, 240, 26);
  lv_obj_t* t = label(sb, "Claude Buddy", F14, COL_TEXT);
  lv_obj_align(t, LV_ALIGN_LEFT_MID, 10, 0);
  sb_time = label(sb, "--:--", F14, COL_TEXT2);
  lv_obj_align(sb_time, LV_ALIGN_RIGHT_MID, -52, 0);
  sb_usb = label(sb, LV_SYMBOL_USB, F14, COL_DIM);
  lv_obj_align(sb_usb, LV_ALIGN_RIGHT_MID, -28, 0);
  sb_ble = label(sb, LV_SYMBOL_BLUETOOTH, F14, COL_DIM);
  lv_obj_align(sb_ble, LV_ALIGN_RIGHT_MID, -10, 0);
}

static lv_obj_t* track_bar(lv_obj_t* c, int x, int y, int w, int h, uint32_t col);
static WBar wb_hp[2];
static NumAnim n_tok_inst[2];

static void build_inst(lv_obj_t* tab, int i, int y) {
  uint32_t col = i ? COL_BLE : COL_USB;
  lv_obj_t* c = card(tab, 8, y, 224, 60);
  inst[i].dot = dot(c, 12, 10, 8, col);
  inst[i].name = label(c, i ? "Claude Desktop" : "Claude Code", F12, COL_TEXT);
  lv_obj_set_pos(inst[i].name, 26, 6);
  inst[i].chip = plain(c, 134, 5, 80, 16);
  lv_obj_set_style_radius(inst[i].chip, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(inst[i].chip, 1, 0);
  inst[i].chip_lbl = label(inst[i].chip, "OFFLINE", F12, COL_DIM);
  lv_obj_center(inst[i].chip_lbl);
  inst[i].msg = label(c, "", F14, COL_TEXT);
  lv_obj_set_size(inst[i].msg, 200, 18);
  lv_label_set_long_mode(inst[i].msg, LV_LABEL_LONG_CLIP);
  lv_obj_set_pos(inst[i].msg, 12, 24);
  inst[i].entry = label(c, "", F12, COL_TEXT2);
  lv_obj_set_size(inst[i].entry, 142, 14);
  lv_label_set_long_mode(inst[i].entry, LV_LABEL_LONG_CLIP);
  lv_obj_set_pos(inst[i].entry, 12, 43);
  inst[i].toktxt = label(c, "tok", F12, COL_DIM);
  lv_obj_align(inst[i].toktxt, LV_ALIGN_TOP_RIGHT, -12, 43);
  inst[i].tok = label(c, "0", F12, col);
  lv_obj_set_size(inst[i].tok, 48, 14);
  lv_obj_set_style_text_align(inst[i].tok, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_align(inst[i].tok, LV_ALIGN_TOP_RIGHT, -34, 43);
  n_tok_inst[i] = {inst[i].tok, 0, 0};
}

static void build_home(lv_obj_t* tab) {
  lv_obj_set_style_pad_all(tab, 0, 0);

  // hero "window"
  hero = card(tab, 8, 2, 224, 128);
  lv_obj_t* bar = plain(hero, 0, 0, 224, 22);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(bar, C(COL_TITLEBAR), 0);
  static const uint32_t tl[3] = {0xff5f57, 0xffbd2e, 0x28c840};
  for (int i = 0; i < 3; i++) hero_dots[i] = dot(bar, 10 + i * 14, 7, 8, tl[i]);
  hero_title = label(bar, "starting", F12, COL_TEXT2);
  lv_obj_set_size(hero_title, 112, 14);
  lv_label_set_long_mode(hero_title, LV_LABEL_LONG_CLIP);
  lv_obj_set_style_text_align(hero_title, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(hero_title, LV_ALIGN_CENTER, -3, 0);
  hero_ctx = label(bar, "", F12, COL_DIM);
  lv_obj_align(hero_ctx, LV_ALIGN_RIGHT_MID, -10, 0);

  build_mascot(hero, MS_CX, MS_CY);

  chip = plain(hero, 62, 106, 100, 18);
  lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(chip, 1, 0);
  chip_lbl = label(chip, "IDLE", F12, COL_TEXT);
  lv_obj_center(chip_lbl);

  // video-game style bars in the top corners: U (USB side) left, B (Bluetooth side) right.
  // Each shows the 5-hour plan window of that side; the right one fills from the right edge.
  for (int i = 0; i < 2; i++) {
    const int bw = 54, bh = 10;
    int bx = i ? 224 - 22 - bw : 22;
    gb_lbl[i] = label(hero, i ? "B" : "U", F12, i ? COL_BLE : COL_USB);
    lv_obj_set_pos(gb_lbl[i], i ? 224 - 14 : 8, 26);
    gb_frame[i] = plain(hero, bx, 28, bw, bh);
    lv_obj_set_style_radius(gb_frame[i], 5, 0);
    lv_obj_set_style_bg_opa(gb_frame[i], LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(gb_frame[i], C(0x070b18), 0);
    lv_obj_set_style_border_width(gb_frame[i], 1, 0);
    lv_obj_set_style_border_color(gb_frame[i], C(0x3a4a80), 0);
    lv_obj_set_style_clip_corner(gb_frame[i], true, 0);
    gb_fill[i] = plain(gb_frame[i], 0, 0, 0, bh - 2);
    lv_obj_set_style_bg_opa(gb_fill[i], LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(gb_fill[i], C(COL_OK), 0);
    lv_obj_align(gb_fill[i], i ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, 0, 0);
    wb_hp[i] = {gb_fill[i], 0, 0};
    for (int k = 1; k <= 3; k++) {                       // segment ticks at 25/50/75%
      lv_obj_t* tk = plain(gb_frame[i], (bw - 2) * k / 4, 0, 1, bh - 2);
      lv_obj_set_style_bg_opa(tk, LV_OPA_COVER, 0);
      lv_obj_set_style_bg_color(tk, C(0x070b18), 0);
    }
  }

  build_inst(tab, 0, 134);
  build_inst(tab, 1, 198);
}

static lv_obj_t* split_bar(lv_obj_t* parent, int x, int y, int w, lv_obj_t** u, lv_obj_t** b) {
  lv_obj_t* track = plain(parent, x, y, w, 10);
  lv_obj_set_style_radius(track, 5, 0);
  lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(track, C(0x0b1020), 0);
  lv_obj_set_style_clip_corner(track, true, 0);
  *u = plain(track, 0, 0, 0, 10);
  lv_obj_set_style_bg_opa(*u, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(*u, C(COL_USB), 0);
  *b = plain(track, 0, 0, 0, 10);
  lv_obj_set_style_bg_opa(*b, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(*b, C(COL_BLE), 0);
  return track;
}

static lv_obj_t* kv(lv_obj_t* c, int y, const char* k) {
  lv_obj_t* kl = label(c, k, F12, COL_TEXT2);
  lv_obj_set_pos(kl, 12, y);
  lv_obj_t* vl = label(c, "-", F12, COL_TEXT);
  lv_obj_set_width(vl, 140);
  lv_obj_set_style_text_align(vl, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_align(vl, LV_ALIGN_TOP_RIGHT, -12, y);
  return vl;
}

static lv_obj_t *st_chart, *st_peak;
static lv_chart_series_t* st_ser;
static int32_t g_spark[UI_SPARK];

static lv_obj_t* track_bar(lv_obj_t* c, int x, int y, int w, int h, uint32_t col) {
  lv_obj_t* t = plain(c, x, y, w, h);
  lv_obj_set_style_radius(t, h / 2, 0);
  lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(t, C(0x0b1020), 0);
  lv_obj_set_style_clip_corner(t, true, 0);
  lv_obj_t* f = plain(t, 0, 0, 0, h);
  lv_obj_set_style_bg_opa(f, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(f, C(col), 0);
  return f;
}

static lv_obj_t *pl_pct[2][2], *pl_rst[2][2], *pl_fill[2][2], *pl_ctx;
static WBar wb_pl[2][2];

static void build_plan_card(lv_obj_t* tab, int y, int src) {
  uint32_t col = src ? COL_BLE : COL_USB;
  lv_obj_t* pc = card(tab, 8, y, 224, 90);
  dot(pc, 12, 12, 8, col);
  lv_obj_t* ph = label(pc, src ? "BLE USAGE" : "USB USAGE", F12, COL_TEXT2);
  lv_obj_set_pos(ph, 26, 8);
  if (src == 0) {
    pl_ctx = label(pc, "", F12, COL_DIM);
    lv_obj_align(pl_ctx, LV_ALIGN_TOP_RIGHT, -12, 8);
  }
  static const char* pn[2] = {"5 hour", "7 day"};
  for (int i = 0; i < 2; i++) {
    int yy = 26 + i * 30;
    lv_obj_t* l = label(pc, pn[i], F12, COL_TEXT);
    lv_obj_set_pos(l, 12, yy);
    pl_fill[src][i] = track_bar(pc, 62, yy + 3, 110, 8, COL_OK);
    wb_pl[src][i] = {pl_fill[src][i], 0, 0};
    pl_pct[src][i] = label(pc, "--", F12, COL_TEXT);
    lv_obj_set_width(pl_pct[src][i], 40);
    lv_obj_set_style_text_align(pl_pct[src][i], LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(pl_pct[src][i], LV_ALIGN_TOP_RIGHT, -12, yy);
    pl_rst[src][i] = label(pc, "", F12, COL_DIM);
    lv_obj_set_pos(pl_rst[src][i], 12, yy + 14);
  }
}

static lv_obj_t *bt_val, *bt_best, *bt_fill;
static WBar wb_bt;

static void build_ble_card(lv_obj_t* tab, int y) {
  lv_obj_t* c = card(tab, 8, y, 224, 66);
  dot(c, 12, 12, 8, COL_BLE);
  lv_obj_t* h = label(c, "BLE TOKENS", F12, COL_TEXT2);
  lv_obj_set_pos(h, 26, 8);
  lv_obj_t* l = label(c, "Today", F12, COL_TEXT);
  lv_obj_set_pos(l, 12, 26);
  bt_fill = track_bar(c, 62, 29, 110, 8, COL_BLE);
  wb_bt = {bt_fill, 0, 0};
  bt_val = label(c, "0", F12, COL_TEXT);
  lv_obj_set_width(bt_val, 40);
  lv_obj_set_style_text_align(bt_val, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_align(bt_val, LV_ALIGN_TOP_RIGHT, -12, 26);
  l = label(c, "Best day", F12, COL_DIM);
  lv_obj_set_pos(l, 12, 44);
  bt_best = label(c, "0", F12, COL_DIM);
  lv_obj_set_width(bt_best, 60);
  lv_obj_set_style_text_align(bt_best, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_align(bt_best, LV_ALIGN_TOP_RIGHT, -12, 44);
}

static void build_stats(lv_obj_t* tab) {
  lv_obj_set_style_pad_all(tab, 0, 0);
  lv_obj_set_scroll_dir(tab, LV_DIR_VER);

  build_plan_card(tab, 2, 0);
  build_ble_card(tab, 98);

  lv_obj_t* c = card(tab, 8, 170, 224, 96);
  lv_obj_t* h = label(c, LV_SYMBOL_CHARGE " TOKENS TODAY", F12, COL_TEXT2);
  lv_obj_set_pos(h, 12, 8);
  st_tok = label(c, "0", F28, COL_TEXT);
  lv_obj_set_pos(st_tok, 12, 24);
  n_tok_stats = {st_tok, 0, 0};
  st_life = label(c, "", F12, COL_DIM);
  lv_obj_align(st_life, LV_ALIGN_TOP_RIGHT, -12, 36);
  split_bar(c, 12, 62, 200, &st_bar_u, &st_bar_b);
  sb_stats = {st_bar_u, st_bar_b, 0, 0, 0, 0, 0, 0};
  dot(c, 12, 79, 7, COL_USB);
  st_leg_u = label(c, "USB 0", F12, COL_TEXT);
  lv_obj_set_pos(st_leg_u, 24, 74);
  dot(c, 118, 79, 7, COL_BLE);
  st_leg_b = label(c, "BLE 0", F12, COL_TEXT);
  lv_obj_set_pos(st_leg_b, 130, 74);

  // tool calls per minute, last 30 minutes (from the hub)
  c = card(tab, 8, 272, 224, 92);
  h = label(c, LV_SYMBOL_PLAY " TOOL CALLS / MIN", F12, COL_TEXT2);
  lv_obj_set_pos(h, 12, 8);
  st_peak = label(c, "", F12, COL_DIM);
  lv_obj_align(st_peak, LV_ALIGN_TOP_RIGHT, -12, 8);
  st_chart = lv_chart_create(c);
  lv_obj_set_pos(st_chart, 10, 26);
  lv_obj_set_size(st_chart, 204, 46);
  lv_chart_set_type(st_chart, LV_CHART_TYPE_BAR);
  lv_chart_set_point_count(st_chart, UI_SPARK);
  lv_chart_set_div_line_count(st_chart, 0, 0);
  lv_chart_set_range(st_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 4);
  lv_obj_set_style_bg_opa(st_chart, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(st_chart, 0, 0);
  lv_obj_set_style_pad_all(st_chart, 0, 0);
  lv_obj_set_style_pad_column(st_chart, 1, 0);
  lv_obj_set_style_radius(st_chart, 2, LV_PART_ITEMS);
  st_ser = lv_chart_add_series(st_chart, C(COL_USB), LV_CHART_AXIS_PRIMARY_Y);
  lv_chart_set_ext_y_array(st_chart, st_ser, g_spark);
  lv_obj_t* l = label(c, "30 min ago", F12, COL_DIM);
  lv_obj_set_pos(l, 12, 74);
  l = label(c, "now", F12, COL_DIM);
  lv_obj_align(l, LV_ALIGN_TOP_RIGHT, -12, 74);

  c = card(tab, 8, 370, 224, 66);
  h = label(c, LV_SYMBOL_PLAY " SESSIONS", F12, COL_TEXT2);
  lv_obj_set_pos(h, 12, 8);
  dot(c, 12, 31, 7, COL_USB);
  l = label(c, "Claude Code", F12, COL_TEXT);
  lv_obj_set_pos(l, 24, 26);
  st_ses_u = label(c, "", F12, COL_TEXT2);
  lv_obj_align(st_ses_u, LV_ALIGN_TOP_RIGHT, -12, 26);
  dot(c, 12, 49, 7, COL_BLE);
  l = label(c, "Claude Desktop", F12, COL_TEXT);
  lv_obj_set_pos(l, 24, 44);
  st_ses_b = label(c, "", F12, COL_TEXT2);
  lv_obj_align(st_ses_b, LV_ALIGN_TOP_RIGHT, -12, 44);

  c = card(tab, 8, 442, 224, 84);
  h = label(c, LV_SYMBOL_OK " DECISIONS", F12, COL_TEXT2);
  lv_obj_set_pos(h, 12, 8);
  l = label(c, "Allow", F12, COL_TEXT);
  lv_obj_set_pos(l, 12, 26);
  st_allow_bar = track_bar(c, 56, 29, 126, 8, COL_OK);
  wb_allow = {st_allow_bar, 0, 0};
  st_allow_n = label(c, "0", F12, COL_TEXT);
  lv_obj_align(st_allow_n, LV_ALIGN_TOP_RIGHT, -12, 26);
  l = label(c, "Deny", F12, COL_TEXT);
  lv_obj_set_pos(l, 12, 44);
  st_deny_bar = track_bar(c, 56, 47, 126, 8, COL_BAD);
  wb_deny = {st_deny_bar, 0, 0};
  st_deny_n = label(c, "0", F12, COL_TEXT);
  lv_obj_align(st_deny_n, LV_ALIGN_TOP_RIGHT, -12, 44);
  st_rate = label(c, "Allow rate  --", F12, COL_TEXT2);
  lv_obj_set_pos(st_rate, 12, 62);

  c = card(tab, 8, 532, 224, 84);
  h = label(c, LV_SYMBOL_SETTINGS " SYSTEM", F12, COL_TEXT2);
  lv_obj_set_pos(h, 12, 8);
  st_dev  = kv(c, 26, "Device");
  st_link = kv(c, 40, "Link");
  st_up   = kv(c, 54, "Uptime");
  st_beat = kv(c, 68, "Last beat");
  lv_obj_t* pad = plain(tab, 8, 618, 1, 8);
  (void)pad;
}

static void build_log(lv_obj_t* tab) {
  lv_obj_set_style_pad_all(tab, 0, 0);
  lv_obj_set_scroll_dir(tab, LV_DIR_VER);
  lv_obj_t* c = card(tab, 8, 2, 224, 252);
  lv_obj_set_style_bg_color(c, C(0x0a0e1c), 0);
  lv_obj_set_style_bg_grad_color(c, C(0x0a0e1c), 0);
  lv_obj_t* h = label(c, LV_SYMBOL_EDIT " LINK LOG", F12, COL_TEXT2);
  lv_obj_set_pos(h, 12, 8);
  lv_obj_t* body = plain(c, 0, 26, 224, 224);
  lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(body, LV_DIR_VER);
  log_lbl = label(body, "", F12, 0xa7e8c0);
  lv_obj_set_width(log_lbl, 200);
  lv_obj_set_pos(log_lbl, 12, 0);
  lv_label_set_long_mode(log_lbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_line_space(log_lbl, 2, 0);
}

// ── Overlays ───────────────────────────────────────────────────────────────
static void perm_cb(lv_event_t* e) {
  bool allow = (bool)(intptr_t)lv_event_get_user_data(e);
  if (g_onPerm) g_onPerm(allow);
}
static void dismiss_cb(lv_event_t*) { if (g_onDismiss) g_onDismiss(); }

static lv_obj_t* big_button(lv_obj_t* parent, const char* txt, int x, int y, int w, int h, uint32_t col, lv_event_cb_t cb, void* ud) {
  lv_obj_t* b = lv_button_create(parent);
  lv_obj_set_pos(b, x, y);
  lv_obj_set_size(b, w, h);
  lv_obj_set_style_radius(b, 14, 0);
  lv_obj_set_style_bg_color(b, C(col), 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_set_style_border_width(b, 0, 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_STATE_PRESSED);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
  lv_obj_t* l = label(b, txt, F16, 0x06130c);
  lv_obj_center(l);
  return b;
}


static void build_attn(void) {
  ov_attn = plain(lv_layer_top(), 0, 0, 240, 320);
  lv_obj_set_style_bg_opa(ov_attn, LV_OPA_80, 0);
  lv_obj_set_style_bg_color(ov_attn, C(0x000000), 0);
  lv_obj_add_flag(ov_attn, LV_OBJ_FLAG_CLICKABLE);

  at_card = card(ov_attn, 14, 38, 212, 244);
  lv_obj_set_style_border_color(at_card, C(ACC[UI_ATTN]), 0);
  lv_obj_set_style_border_width(at_card, 2, 0);
  lv_obj_set_style_shadow_width(at_card, 24, 0);
  lv_obj_set_style_shadow_color(at_card, C(ACC[UI_ATTN]), 0);
  lv_obj_set_style_shadow_opa(at_card, LV_OPA_50, 0);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, at_card);
  lv_anim_set_exec_cb(&a, anim_shadow);
  lv_anim_set_values(&a, 6, 18);
  lv_anim_set_duration(&a, 700);
  lv_anim_set_reverse_duration(&a, 700);
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_start(&a);

  lv_obj_t* head_ = plain(at_card, 0, 0, 212, 34);
  lv_obj_set_style_bg_opa(head_, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(head_, C(0x3a2a08), 0);
  lv_obj_t* hl = label(head_, LV_SYMBOL_WARNING "  Permission needed", F16, ACC[UI_ATTN]);
  lv_obj_center(hl);

  at_cap = label(at_card, "TOOL", F12, COL_TEXT2);
  lv_obj_set_pos(at_cap, 16, 44);
  at_tool = label(at_card, "", F28, COL_TEXT);
  lv_obj_set_size(at_tool, 180, 34);
  lv_label_set_long_mode(at_tool, LV_LABEL_LONG_CLIP);
  lv_obj_set_pos(at_tool, 16, 58);

  lv_obj_t* box = plain(at_card, 14, 98, 184, 50);
  lv_obj_set_style_radius(box, 10, 0);
  lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(box, C(0x080b16), 0);
  lv_obj_set_style_border_width(box, 1, 0);
  lv_obj_set_style_border_color(box, C(COL_BORDER), 0);
  at_hint = label(box, "", F12, 0xa7e8c0);
  lv_obj_set_width(at_hint, 168);
  lv_label_set_long_mode(at_hint, LV_LABEL_LONG_WRAP);
  lv_obj_set_height(at_hint, 36);
  lv_obj_set_pos(at_hint, 8, 7);

  at_note = label(at_card, "Answer in your terminal", F14, COL_TEXT2);
  lv_obj_set_pos(at_note, 16, 156);

  at_btn_info  = big_button(at_card, "Dismiss", 14, 180, 184, 50, 0x6d7aa8, dismiss_cb, NULL);
  at_btn_deny  = big_button(at_card, LV_SYMBOL_CLOSE "  Deny", 14, 176, 88, 54, COL_BAD, perm_cb, (void*)0);
  at_btn_allow = big_button(at_card, LV_SYMBOL_OK "  Allow", 110, 176, 88, 54, COL_OK, perm_cb, (void*)1);
  lv_obj_add_flag(ov_attn, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t* link_row(lv_obj_t* parent, int y, const char* sym, uint32_t col, const char* title, const char* sub) {
  lv_obj_t* c = card(parent, 12, y, 216, 44);
  lv_obj_t* ic = label(c, sym, F20, col);
  lv_obj_align(ic, LV_ALIGN_LEFT_MID, 12, 0);
  lv_obj_t* t = label(c, title, F14, COL_TEXT);
  lv_obj_set_pos(t, 44, 6);
  lv_obj_t* s = label(c, sub, F12, COL_TEXT2);
  lv_obj_set_size(s, 164, 14);
  lv_label_set_long_mode(s, LV_LABEL_LONG_CLIP);
  clipText(s, sub, 164);
  lv_obj_set_pos(s, 44, 25);
  return c;
}

static void build_link(void) {
  ov_link = plain(lv_layer_top(), 0, 0, 240, 320);
  lv_obj_set_style_bg_opa(ov_link, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(ov_link, C(COL_BG_TOP), 0);
  lv_obj_set_style_bg_grad_color(ov_link, C(COL_BG_BOT), 0);
  lv_obj_set_style_bg_grad_dir(ov_link, LV_GRAD_DIR_VER, 0);
  lv_obj_add_flag(ov_link, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t* t = label(ov_link, "Claude Buddy", F28, COL_TEXT);
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 22);
  lk_name = label(ov_link, "Claude-????", F14, COL_USB);
  lv_obj_align(lk_name, LV_ALIGN_TOP_MID, 0, 58);

  lk_spin = lv_spinner_create(ov_link);
  lv_spinner_set_anim_params(lk_spin, 1100, 90);
  lv_obj_set_size(lk_spin, 52, 52);
  lv_obj_align(lk_spin, LV_ALIGN_TOP_MID, 0, 88);
  lv_obj_set_style_arc_width(lk_spin, 5, LV_PART_MAIN);
  lv_obj_set_style_arc_width(lk_spin, 5, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(lk_spin, C(0x1c2546), LV_PART_MAIN);
  lv_obj_set_style_arc_color(lk_spin, C(COL_USB), LV_PART_INDICATOR);

  lk_status = label(ov_link, "Waiting for a link", F16, COL_TEXT);
  lv_obj_set_width(lk_status, 216);
  lv_obj_set_style_text_align(lk_status, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(lk_status, LV_ALIGN_TOP_MID, 0, 150);

  lk_key = label(ov_link, "", F28, ACC[UI_ATTN]);
  lv_obj_set_style_text_letter_space(lk_key, 4, 0);
  lv_obj_align(lk_key, LV_ALIGN_TOP_MID, 0, 100);
  lv_obj_add_flag(lk_key, LV_OBJ_FLAG_HIDDEN);

  lk_usb = link_row(ov_link, 190, LV_SYMBOL_USB, COL_USB, "USB hub", "Plug into the hub host");
  lk_ble = link_row(ov_link, 240, LV_SYMBOL_BLUETOOTH, COL_BLE, "Bluetooth", "Desktop: Developer > Buddy");
}

// ── Public API ─────────────────────────────────────────────────────────────
void ui_set_tab(int tab) {
  g_tab = tab;
  lv_tabview_set_active(g_tv, tab, LV_ANIM_OFF);
}

void ui_init(UiPermCb onPermission, UiDismissCb onDismiss) {
  g_onPerm = onPermission;
  g_onDismiss = onDismiss;

  lv_obj_t* scr = lv_screen_active();
  lv_obj_remove_style_all(scr);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(scr, C(COL_BG_TOP), 0);
  lv_obj_set_style_bg_grad_color(scr, C(COL_BG_BOT), 0);
  lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  build_statusbar(scr);

  g_tv = lv_tabview_create(scr);
  lv_tabview_set_tab_bar_position(g_tv, LV_DIR_BOTTOM);
  lv_tabview_set_tab_bar_size(g_tv, 34);
  lv_obj_set_pos(g_tv, 0, 26);
  lv_obj_set_size(g_tv, 240, 294);
  lv_obj_set_style_bg_opa(g_tv, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(g_tv, 0, 0);
  lv_obj_t* t0 = lv_tabview_add_tab(g_tv, LV_SYMBOL_HOME "  Home");
  lv_obj_t* t1 = lv_tabview_add_tab(g_tv, LV_SYMBOL_CHARGE "  Stats");
  lv_obj_t* t2 = lv_tabview_add_tab(g_tv, LV_SYMBOL_LIST "  Log");
  lv_obj_t* content = lv_tabview_get_content(g_tv);
  lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);

  lv_obj_t* bar = lv_tabview_get_tab_bar(g_tv);
  lv_obj_set_style_bg_color(bar, C(0x0a0f20), 0);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(bar, 1, 0);
  lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
  lv_obj_set_style_border_color(bar, C(COL_BORDER), 0);
  lv_obj_set_style_text_font(bar, F14, LV_PART_ITEMS);
  lv_obj_set_style_text_color(bar, C(COL_DIM), LV_PART_ITEMS);
  lv_obj_set_style_text_color(bar, C(COL_USB), LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_border_width(bar, 2, LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_border_color(bar, C(COL_USB), LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_radius(bar, 0, LV_PART_ITEMS);

  build_home(t0);
  build_stats(t1);
  build_log(t2);
  build_attn();
  build_link();
}

static void split(lv_obj_t* bu, lv_obj_t* bb, int w, uint32_t u, uint32_t b) {
  uint32_t t = u + b;
  int uw = t ? (int)((uint64_t)u * w / t) : 0;
  int bw = t ? w - uw : 0;
  if (u == 0) uw = 0;
  lv_obj_set_width(bu, uw);
  lv_obj_set_x(bb, uw);
  lv_obj_set_width(bb, bw);
}

void ui_update(const UiModel& m) {
  char t[64], t2[16], t3[16];

  bool online = m.usbLive || (m.bleConn && m.bleSec);
  UiState st = online ? m.state : UI_SLEEP;
  if (st != g_state) { g_state = st; mascot_state(st); }

  // status bar
  setText(sb_time, m.time);
  lv_obj_set_style_text_color(sb_usb, C(m.usbLive ? COL_USB : COL_DIM), 0);
  lv_obj_set_style_text_color(sb_ble, C(m.bleConn ? (m.bleSec ? COL_BLE : ACC[UI_ATTN]) : COL_DIM), 0);

  // hero
  const char* title = m.msg[0] ? m.msg : (st == UI_SLEEP ? "no activity" : "idle");
  clipText(hero_title, title, 112);

  // one card per connected Claude instance
  uint32_t tokTotal = m.tok[0] + m.tok[1];
  for (int i = 0; i < 2; i++) {
    bool linked = i == 0 ? m.usbLive : m.bleConn;
    int is = !linked ? UI_SLEEP : (m.wait[i] ? UI_ATTN : (m.run[i] ? UI_BUSY : UI_IDLE));
    const char* lab = !linked ? "OFFLINE" : (m.wait[i] ? "NEEDS YOU" : (m.run[i] ? "WORKING" : "IDLE"));
    uint32_t ac = ACC[is];
    lv_obj_set_style_bg_color(inst[i].dot, C(linked ? (i ? COL_BLE : COL_USB) : 0x2c3a62), 0);
    lv_obj_set_style_text_color(inst[i].name, C(linked ? COL_TEXT : COL_DIM), 0);
    lv_obj_set_style_bg_color(inst[i].chip, C(ac), 0);
    lv_obj_set_style_bg_opa(inst[i].chip, LV_OPA_20, 0);
    lv_obj_set_style_border_color(inst[i].chip, C(ac), 0);
    lv_obj_set_style_text_color(inst[i].chip_lbl, C(ac), 0);
    setText(inst[i].chip_lbl, lab);
    const char* mtxt = !linked ? "not connected" : (m.msgS[i][0] ? m.msgS[i] : "idle");
    clipText(inst[i].msg, mtxt, 200);
    lv_obj_set_style_text_color(inst[i].msg, C(!linked ? COL_DIM : (is == UI_BUSY || is == UI_ATTN ? COL_TEXT : COL_TEXT2)), 0);
    clipText(inst[i].entry, linked && m.nEntriesS[i] ? m.entriesS[i][0] : "", 142);
    setNum(n_tok_inst[i], m.tok[i]);
  }

  // plan usage: the corner bars in the buddy window (5-hour, U left / B right) + a card per side on Stats
  for (int src = 0; src < 1; src++) {
    const UiLimits& L = m.lim[src];
    bool has[2] = {L.has5, L.has7};
    uint8_t pc_[2] = {L.pct5, L.pct7};
    int32_t rs[2] = {L.rst5, L.rst7};
    for (int i = 0; i < 2; i++) {
      if (has[i]) {
        snprintf(t, sizeof t, "%u%%", pc_[i]); setText(pl_pct[src][i], t);
        setWBar(wb_pl[src][i], pc_[i] * 110 / 100);
        uint32_t col = pc_[i] >= 90 ? COL_BAD : (pc_[i] >= 70 ? 0xffc83d : COL_OK);
        lv_obj_set_style_bg_color(pl_fill[src][i], C(col), 0);
        lv_obj_set_style_text_color(pl_pct[src][i], C(col), 0);
        if (rs[i] < 0) setText(pl_rst[src][i], "");
        else {
          int32_t d = rs[i];
          if (d >= 86400)      snprintf(t, sizeof t, "resets in %ldd %ldh", (long)(d / 86400), (long)((d % 86400) / 3600));
          else if (d >= 3600)  snprintf(t, sizeof t, "resets in %ldh %02ldm", (long)(d / 3600), (long)((d % 3600) / 60));
          else                 snprintf(t, sizeof t, "resets in %ldm", (long)(d / 60));
          setText(pl_rst[src][i], t);
        }
      } else {
        setText(pl_pct[src][i], "--");
        setWBar(wb_pl[src][i], 0);
        lv_obj_set_style_text_color(pl_pct[src][i], C(COL_DIM), 0);
        setText(pl_rst[src][i], i == 0 ? "no active window yet" : "");
      }
    }
    // corner bar
    if (L.has5) {
      uint32_t col = L.pct5 >= 90 ? COL_BAD : (L.pct5 >= 70 ? 0xffc83d : COL_OK);
      setWBar(wb_hp[src], L.pct5 * 52 / 100);
      lv_obj_set_style_bg_color(gb_fill[src], C(col), 0);
      lv_obj_set_style_text_color(gb_lbl[src], C(src ? COL_BLE : COL_USB), 0);
    } else {
      setWBar(wb_hp[src], 0);
      lv_obj_set_style_text_color(gb_lbl[src], C(COL_DIM), 0);
    }
  }
  // B: the Bluetooth side has no plan usage (Claude Desktop only reports tokens), so it shows
  // today's tokens against the biggest day seen, scaled so a quiet history still moves the bar
  {
    uint32_t best = m.tokBestB > m.tok[1] ? m.tokBestB : m.tok[1];
    if (best < 100000) best = 100000;       // floor: a quiet history shouldn't read as a full bar
    uint32_t tb = m.bleConn ? m.tok[1] : 0;
    setWBar(wb_hp[1], (int)((uint64_t)tb * 52 / best));
    lv_obj_set_style_bg_color(gb_fill[1], C(COL_BLE), 0);
    lv_obj_set_style_text_color(gb_lbl[1], C(m.bleConn ? COL_BLE : COL_DIM), 0);
    setWBar(wb_bt, (int)((uint64_t)m.tok[1] * 110 / best));
    fmtTok(t2, sizeof t2, m.tok[1]); setText(bt_val, t2);
    fmtTok(t2, sizeof t2, m.tokBestB > m.tok[1] ? m.tokBestB : m.tok[1]); setText(bt_best, t2);
  }
  if (m.ctx >= 0) { snprintf(t, sizeof t, "context %d%%", m.ctx); setText(pl_ctx, t); snprintf(t, sizeof t, "ctx %d%%", m.ctx); setText(hero_ctx, t); }
  else { setText(pl_ctx, ""); setText(hero_ctx, ""); }

  // stats tab
  setNum(n_tok_stats, tokTotal);
  if (m.tokLife) { fmtTok(t2, sizeof t2, m.tokLife); snprintf(t, sizeof t, "lifetime %s", t2); setText(st_life, t); }
  fmtTok(t2, sizeof t2, m.tok[0]); snprintf(t, sizeof t, "USB  %s", t2); setText(st_leg_u, t);
  fmtTok(t2, sizeof t2, m.tok[1]); snprintf(t, sizeof t, "BLE  %s", t2); setText(st_leg_b, t);
  setSplit(sb_stats, 200, m.tok[0], m.tok[1]);
  snprintf(t, sizeof t, "run %u  wait %u", m.usbLive ? m.run[0] : 0, m.usbLive ? m.wait[0] : 0);
  setText(st_ses_u, m.usbLive ? t : "not linked");
  snprintf(t, sizeof t, "run %u  wait %u", m.bleConn ? m.run[1] : 0, m.bleConn ? m.wait[1] : 0);
  setText(st_ses_b, m.bleConn ? t : "not linked");
  uint32_t tot = m.approvals + m.denials;
  setWBar(wb_allow, tot ? (int)((uint64_t)m.approvals * 126 / tot) : 0);
  setWBar(wb_deny, tot ? (int)((uint64_t)m.denials * 126 / tot) : 0);

  // activity chart
  {
    int32_t peak = 0;
    int off = UI_SPARK - m.nSpark;
    bool changed = false;
    for (int i = 0; i < UI_SPARK; i++) {
      int32_t v = i >= off ? m.spark[i - off] : 0;
      if (g_spark[i] != v) { g_spark[i] = v; changed = true; }
      if (v > peak) peak = v;
    }
    if (changed) {
      lv_chart_set_range(st_chart, LV_CHART_AXIS_PRIMARY_Y, 0, peak < 4 ? 4 : peak);
      lv_chart_refresh(st_chart);
      snprintf(t, sizeof t, "peak %ld", (long)peak);
      setText(st_peak, t);
    }
  }
  snprintf(t, sizeof t, "%u", m.approvals); setText(st_allow_n, t);
  snprintf(t, sizeof t, "%u", m.denials);   setText(st_deny_n, t);
  if (tot) snprintf(t, sizeof t, "Allow rate  %u%%", (unsigned)(m.approvals * 100 / tot));
  else     snprintf(t, sizeof t, "Allow rate  --");
  setText(st_rate, t);
  setText(st_dev, m.devName);
  setText(st_link, m.usbLive ? (m.bleConn && m.bleSec ? "USB + BLE" : "USB") : (m.bleConn ? (m.bleSec ? "BLE" : "BLE pairing") : "offline"));
  snprintf(t, sizeof t, "%02lu:%02lu:%02lu", (unsigned long)(m.uptimeS / 3600), (unsigned long)((m.uptimeS % 3600) / 60), (unsigned long)(m.uptimeS % 60));
  setText(st_up, t);
  if (m.beatAgeS >= 0) { snprintf(t, sizeof t, "%lds ago", (long)m.beatAgeS); setText(st_beat, t); } else setText(st_beat, "never");
  lv_obj_set_style_text_color(st_beat, C(m.beatAgeS > 8 || m.beatAgeS < 0 ? COL_BAD : COL_TEXT), 0);

  // log
  if (m.logSeq != g_logSeq) {
    g_logSeq = m.logSeq;
    static char buf[UI_LOG_LINES * 68];
    buf[0] = 0;
    size_t n = 0;
    for (int i = 0; i < m.logCount; i++) {
      int w = snprintf(buf + n, sizeof buf - n, "%s%s", i ? "\n" : "", m.logLines[i]);
      if (w < 0 || (size_t)w >= sizeof buf - n) break;
      n += w;
    }
    lv_label_set_text(log_lbl, buf[0] ? buf : "(no traffic yet)");
  }

  // attention overlay
  bool attn = online && st == UI_ATTN;
  if (attn) {
    clipText(at_tool, m.pTool[0] ? m.pTool : "Tool", 180);
    setText(at_cap, m.pSrc == 0 ? "TOOL  -  Claude Code (USB)" : "TOOL  -  Claude Desktop (BLE)");
    lv_obj_set_style_text_color(at_cap, C(m.pSrc == 0 ? COL_USB : COL_BLE), 0);
    snprintf(t, sizeof t, "%s", m.pHint);
    setText(at_hint, t[0] ? t : m.pId);
    setFlag(at_note, LV_OBJ_FLAG_HIDDEN, !m.pInfo);
    setFlag(at_btn_info, LV_OBJ_FLAG_HIDDEN, !m.pInfo);
    setFlag(at_btn_deny, LV_OBJ_FLAG_HIDDEN, m.pInfo);
    setFlag(at_btn_allow, LV_OBJ_FLAG_HIDDEN, m.pInfo);
  }
  setFlag(ov_attn, LV_OBJ_FLAG_HIDDEN, !attn);

  // link overlay: also while a Bluetooth passkey is pending, even if USB is already linked --
  // otherwise the code you must type into the other machine is never shown
  bool showLink = !online || (m.bleConn && m.showKey);
  setFlag(ov_link, LV_OBJ_FLAG_HIDDEN, !showLink);
  if (showLink) {
    setText(lk_name, m.devName);
    bool key = m.bleConn && m.showKey;
    setFlag(lk_key, LV_OBJ_FLAG_HIDDEN, !key);
    setFlag(lk_spin, LV_OBJ_FLAG_HIDDEN, key);
    if (key) {
      snprintf(t, sizeof t, "%06lu", (unsigned long)m.bleKey);
      setText(lk_key, t);
      lv_obj_align(lk_key, LV_ALIGN_TOP_MID, 0, 100);
      setText(lk_status, "Enter this code in\nClaude Desktop");
    } else {
      setText(lk_status, m.bleConn ? "Pairing..." : "Waiting for a link");
    }
    lv_obj_set_style_border_color(lk_ble, C(m.bleConn ? COL_BLE : COL_BORDER), 0);
  }
}
