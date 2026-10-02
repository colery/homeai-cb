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
#define COL_DIM      0x4d5a85
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

// home / stats strip
static lv_obj_t *run_val, *wait_val, *tok_val;
static lv_obj_t *run_u, *run_b, *wait_u, *wait_b;
static lv_obj_t *tok_bar_u, *tok_bar_b;
static lv_obj_t *act_rows[3], *act_dots[3];

// stats tab
static lv_obj_t *st_tok, *st_bar_u, *st_bar_b, *st_leg_u, *st_leg_b, *st_life;
static lv_obj_t *st_ses_u, *st_ses_b;
static lv_obj_t *st_allow_bar, *st_deny_bar, *st_allow_n, *st_deny_n, *st_rate;
static lv_obj_t *st_link, *st_up, *st_beat, *st_dev;

// log tab
static lv_obj_t* log_lbl;
static uint32_t  g_logSeq = 0xFFFFFFFF;

// overlays
static lv_obj_t *ov_attn, *at_card, *at_tool, *at_hint, *at_note, *at_btn_info, *at_btn_deny, *at_btn_allow;
static lv_obj_t *ov_link, *lk_name, *lk_status, *lk_key, *lk_spin, *lk_usb, *lk_ble;

// ── Mascot ─────────────────────────────────────────────────────────────────
LV_DRAW_BUF_DEFINE_STATIC(ear_buf_l, 20, 18, LV_COLOR_FORMAT_ARGB8888);
LV_DRAW_BUF_DEFINE_STATIC(ear_buf_r, 20, 18, LV_COLOR_FORMAT_ARGB8888);

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
  if (left) { t.p[0] = {1, 17}; t.p[1] = {3, 1};  t.p[2] = {18, 17}; }
  else      { t.p[0] = {18, 17}; t.p[1] = {16, 1}; t.p[2] = {1, 17}; }
  lv_draw_triangle(&layer, &t);

  lv_draw_triangle_dsc_t in;
  lv_draw_triangle_dsc_init(&in);
  in.color = C(0x8a4f78);
  in.opa = LV_OPA_COVER;
  if (left) { in.p[0] = {5, 16}; in.p[1] = {6, 7};  in.p[2] = {13, 16}; }
  else      { in.p[0] = {14, 16}; in.p[1] = {13, 7}; in.p[2] = {6, 16}; }
  lv_draw_triangle(&layer, &in);

  lv_canvas_finish_layer(c, &layer);
  return c;
}

static lv_obj_t* make_eye(lv_obj_t* head_, int dx) {
  lv_obj_t* e = plain(head_, 0, 0, 8, 12);
  lv_obj_set_style_radius(e, 4, 0);
  lv_obj_set_style_bg_opa(e, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(e, C(0xeaf0ff), 0);
  lv_obj_align(e, LV_ALIGN_CENTER, dx, -3);
  return e;
}

static lv_obj_t* make_line(lv_obj_t* parent, const lv_point_precise_t* pts, int n, int w, uint32_t col) {
  lv_obj_t* l = lv_line_create(parent);
  lv_line_set_points(l, pts, n);
  lv_obj_set_style_line_width(l, w, 0);
  lv_obj_set_style_line_color(l, C(col), 0);
  lv_obj_set_style_line_rounded(l, true, 0);
  return l;
}

static void anim_ring_rot(void* o, int32_t v) { lv_arc_set_rotation((lv_obj_t*)o, v); }
// Pulses recolor instead of fading object opacity: an opacity < 255 makes LVGL render the
// object into a temporary layer buffer, which the ESP32 (BLE running) has no spare RAM for.
static uint32_t g_acc = 0x4aa8ff;
static void anim_ring_pulse(void* o, int32_t v) {
  lv_obj_set_style_arc_color((lv_obj_t*)o, lv_color_mix(C(g_acc), C(0x1c2546), (uint8_t)v), LV_PART_INDICATOR);
}
static void anim_bg_pulse(void* o, int32_t v) {
  lv_obj_set_style_bg_color((lv_obj_t*)o, lv_color_mix(C(ACC[UI_ATTN]), C(0x4a3208), (uint8_t)v), 0);
}
static void anim_h(void* o, int32_t v)        { lv_obj_set_height((lv_obj_t*)o, v); }
static void anim_y_off(void* o, int32_t v)    { lv_obj_set_style_translate_y((lv_obj_t*)o, v, 0); }

static void blink_cb(lv_timer_t*) {
  if (g_state != UI_IDLE && g_state != UI_ATTN) return;
  lv_obj_t* eyes[2] = {eyeL, eyeR};
  int h0 = (g_state == UI_ATTN) ? 16 : 12;
  for (auto e : eyes) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, e);
    lv_anim_set_exec_cb(&a, anim_h);
    lv_anim_set_values(&a, h0, 2);
    lv_anim_set_duration(&a, 90);
    lv_anim_set_reverse_duration(&a, 110);
    lv_anim_start(&a);
  }
}

static void build_mascot(lv_obj_t* parent, int cx, int cy) {
  ring = lv_arc_create(parent);
  lv_obj_set_size(ring, 76, 76);
  lv_obj_set_pos(ring, cx - 38, cy - 38);
  lv_arc_set_bg_angles(ring, 0, 360);
  lv_arc_set_angles(ring, 0, 360);
  lv_obj_remove_style(ring, NULL, LV_PART_KNOB);
  lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(ring, 4, LV_PART_MAIN);
  lv_obj_set_style_arc_width(ring, 4, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(ring, C(0x1c2546), LV_PART_MAIN);
  lv_obj_set_style_arc_rounded(ring, true, LV_PART_INDICATOR);

  earL = make_ear(parent, &ear_buf_l, true);
  earR = make_ear(parent, &ear_buf_r, false);
  lv_obj_set_pos(earL, cx - 21, cy - 27);
  lv_obj_set_pos(earR, cx + 1,  cy - 27);

  head = plain(parent, cx - 24, cy - 16, 48, 40);
  lv_obj_set_style_radius(head, 19, 0);
  lv_obj_set_style_bg_opa(head, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(head, C(0x2b3868), 0);
  lv_obj_set_style_bg_grad_color(head, C(0x1a2347), 0);
  lv_obj_set_style_bg_grad_dir(head, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_border_width(head, 2, 0);

  eyeL = make_eye(head, -10);
  eyeR = make_eye(head, 10);

  nose = plain(head, 0, 0, 5, 4);
  lv_obj_set_style_radius(nose, 2, 0);
  lv_obj_set_style_bg_opa(nose, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(nose, C(0xe98fb4), 0);
  lv_obj_align(nose, LV_ALIGN_CENTER, 0, 6);

  mouth = lv_arc_create(head);
  lv_obj_set_size(mouth, 16, 16);
  lv_arc_set_bg_angles(mouth, 30, 150);
  lv_arc_set_angles(mouth, 30, 150);
  lv_obj_remove_style(mouth, NULL, LV_PART_KNOB);
  lv_obj_remove_flag(mouth, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(mouth, 2, LV_PART_MAIN);
  lv_obj_set_style_arc_width(mouth, 2, LV_PART_INDICATOR);
  lv_obj_set_style_arc_opa(mouth, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_arc_color(mouth, C(0xeaf0ff), LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(mouth, true, LV_PART_INDICATOR);
  lv_obj_align(mouth, LV_ALIGN_CENTER, 0, 5);

  static const lv_point_precise_t wl1[] = {{0, 2}, {10, 0}};
  static const lv_point_precise_t wl2[] = {{0, 0}, {10, 4}};
  static const lv_point_precise_t wr1[] = {{0, 0}, {10, 2}};
  static const lv_point_precise_t wr2[] = {{0, 4}, {10, 0}};
  whiskers[0] = make_line(parent, wl1, 2, 1, 0x7e8cb8);
  whiskers[1] = make_line(parent, wl2, 2, 1, 0x7e8cb8);
  whiskers[2] = make_line(parent, wr1, 2, 1, 0x7e8cb8);
  whiskers[3] = make_line(parent, wr2, 2, 1, 0x7e8cb8);
  lv_obj_set_pos(whiskers[0], cx - 36, cy + 3);
  lv_obj_set_pos(whiskers[1], cx - 36, cy + 9);
  lv_obj_set_pos(whiskers[2], cx + 26, cy + 3);
  lv_obj_set_pos(whiskers[3], cx + 26, cy + 9);

  badge_z = label(parent, "z Z", F14, 0x9fb0e0);
  lv_obj_set_pos(badge_z, cx + 16, cy - 36);

  badge_bang = plain(parent, cx + 18, cy - 38, 18, 18);
  lv_obj_set_style_radius(badge_bang, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(badge_bang, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(badge_bang, C(ACC[UI_ATTN]), 0);
  lv_obj_t* bl = label(badge_bang, "!", F14, 0x1a1200);
  lv_obj_center(bl);

  lv_timer_create(blink_cb, 3800, NULL);
}

static void mascot_state(UiState s) {
  uint32_t acc = ACC[s];
  lv_anim_delete(ring, anim_ring_rot);
  lv_anim_delete(ring, anim_ring_pulse);
  lv_anim_delete(badge_bang, anim_bg_pulse);
  g_acc = ACC[s];
  lv_anim_delete(badge_z, anim_y_off);
  lv_obj_set_style_arc_color(ring, C(acc), LV_PART_INDICATOR);
  lv_obj_set_style_border_color(head, C(acc), 0);

  bool sleep = s == UI_SLEEP, busy = s == UI_BUSY, attn = s == UI_ATTN;
  setFlag(badge_z, LV_OBJ_FLAG_HIDDEN, !sleep);
  setFlag(badge_bang, LV_OBJ_FLAG_HIDDEN, !attn);

  // eyes
  int ew = busy ? 11 : 8;
  int eh = sleep ? 2 : busy ? 5 : attn ? 16 : 12;
  lv_obj_set_size(eyeL, ew, eh);
  lv_obj_set_size(eyeR, ew, eh);
  uint32_t ec = busy ? acc : (sleep ? 0x8794bd : 0xeaf0ff);
  lv_obj_set_style_bg_color(eyeL, C(ec), 0);
  lv_obj_set_style_bg_color(eyeR, C(ec), 0);
  lv_obj_align(eyeL, LV_ALIGN_CENTER, busy ? -9 : -10, sleep ? -2 : -3);
  lv_obj_align(eyeR, LV_ALIGN_CENTER, busy ?  9 :  10, sleep ? -2 : -3);

  // mouth
  if (attn)       { lv_arc_set_bg_angles(mouth, 0, 360); lv_arc_set_angles(mouth, 0, 360); lv_obj_set_size(mouth, 9, 9); lv_obj_align(mouth, LV_ALIGN_CENTER, 0, 11); }
  else if (sleep) { lv_arc_set_bg_angles(mouth, 60, 120); lv_arc_set_angles(mouth, 60, 120); lv_obj_set_size(mouth, 16, 16); lv_obj_align(mouth, LV_ALIGN_CENTER, 0, 6); }
  else            { lv_arc_set_bg_angles(mouth, 30, 150); lv_arc_set_angles(mouth, 30, 150); lv_obj_set_size(mouth, 16, 16); lv_obj_align(mouth, LV_ALIGN_CENTER, 0, 5); }
  lv_obj_set_style_arc_color(mouth, C(attn ? acc : 0xeaf0ff), LV_PART_INDICATOR);

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

  // hero chrome
  for (int i = 0; i < 3; i++) lv_obj_set_style_opa(hero_dots[i], sleep ? LV_OPA_40 : LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(chip, C(acc), 0);
  lv_obj_set_style_bg_opa(chip, LV_OPA_20, 0);
  lv_obj_set_style_border_color(chip, C(acc), 0);
  lv_obj_set_style_text_color(chip_lbl, C(acc), 0);
  setText(chip_lbl, LBL[s]);
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

static lv_obj_t* stat_card(lv_obj_t* parent, int x, const char* icon, const char* name, lv_obj_t** val) {
  lv_obj_t* c = card(parent, x, 134, 72, 60);
  char buf[32];
  snprintf(buf, sizeof buf, "%s %s", icon, name);
  lv_obj_t* l = label(c, buf, F12, COL_TEXT2);
  lv_obj_set_pos(l, 8, 5);
  *val = label(c, "0", F20, COL_TEXT);
  lv_obj_set_pos(*val, 8, 19);
  return c;
}

static void src_pair(lv_obj_t* c, lv_obj_t** u, lv_obj_t** b) {
  dot(c, 8, 49, 6, COL_USB);
  *u = label(c, "0", F12, COL_TEXT2);
  lv_obj_set_pos(*u, 17, 44);
  dot(c, 38, 49, 6, COL_BLE);
  *b = label(c, "0", F12, COL_TEXT2);
  lv_obj_set_pos(*b, 47, 44);
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
  lv_obj_set_width(hero_title, 150);
  lv_label_set_long_mode(hero_title, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(hero_title, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(hero_title, LV_ALIGN_CENTER, 14, 0);

  build_mascot(hero, 112, 61);

  chip = plain(hero, 62, 101, 100, 20);
  lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(chip, 1, 0);
  chip_lbl = label(chip, "IDLE", F12, COL_TEXT);
  lv_obj_center(chip_lbl);

  // stats strip
  lv_obj_t* c1 = stat_card(tab, 8,   LV_SYMBOL_PLAY, "RUN",  &run_val);
  src_pair(c1, &run_u, &run_b);
  lv_obj_t* c2 = stat_card(tab, 84,  LV_SYMBOL_BELL, "WAIT", &wait_val);
  src_pair(c2, &wait_u, &wait_b);
  lv_obj_t* c3 = stat_card(tab, 160, LV_SYMBOL_CHARGE, "TOK", &tok_val);
  lv_obj_t* track = plain(c3, 8, 48, 56, 6);
  lv_obj_set_style_radius(track, 3, 0);
  lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(track, C(0x0b1020), 0);
  lv_obj_set_style_clip_corner(track, true, 0);
  tok_bar_u = plain(track, 0, 0, 0, 6);
  lv_obj_set_style_bg_opa(tok_bar_u, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(tok_bar_u, C(COL_USB), 0);
  tok_bar_b = plain(track, 0, 0, 0, 6);
  lv_obj_set_style_bg_opa(tok_bar_b, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(tok_bar_b, C(COL_BLE), 0);

  // activity
  lv_obj_t* act = card(tab, 8, 198, 224, 60);
  lv_obj_t* at = label(act, LV_SYMBOL_LIST " ACTIVITY", F12, COL_TEXT2);
  lv_obj_set_pos(at, 10, 2);
  for (int i = 0; i < 3; i++) {
    act_dots[i] = dot(act, 12, 22 + i * 13, 5, COL_DIM);
    act_rows[i] = label(act, "", F12, COL_TEXT);
    lv_obj_set_width(act_rows[i], 190);
    lv_label_set_long_mode(act_rows[i], LV_LABEL_LONG_DOT);
    lv_obj_set_pos(act_rows[i], 24, 16 + i * 13);
  }
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

static void build_stats(lv_obj_t* tab) {
  lv_obj_set_style_pad_all(tab, 0, 0);
  lv_obj_set_scroll_dir(tab, LV_DIR_VER);

  lv_obj_t* c = card(tab, 8, 2, 224, 96);
  lv_obj_t* h = label(c, LV_SYMBOL_CHARGE " TOKENS TODAY", F12, COL_TEXT2);
  lv_obj_set_pos(h, 12, 8);
  st_tok = label(c, "0", F28, COL_TEXT);
  lv_obj_set_pos(st_tok, 12, 24);
  st_life = label(c, "", F12, COL_DIM);
  lv_obj_align(st_life, LV_ALIGN_TOP_RIGHT, -12, 36);
  split_bar(c, 12, 62, 200, &st_bar_u, &st_bar_b);
  dot(c, 12, 79, 7, COL_USB);
  st_leg_u = label(c, "USB 0", F12, COL_TEXT);
  lv_obj_set_pos(st_leg_u, 24, 74);
  dot(c, 118, 79, 7, COL_BLE);
  st_leg_b = label(c, "BLE 0", F12, COL_TEXT);
  lv_obj_set_pos(st_leg_b, 130, 74);

  c = card(tab, 8, 104, 224, 66);
  h = label(c, LV_SYMBOL_PLAY " SESSIONS", F12, COL_TEXT2);
  lv_obj_set_pos(h, 12, 8);
  dot(c, 12, 31, 7, COL_USB);
  lv_obj_t* l = label(c, "Claude Code", F12, COL_TEXT);
  lv_obj_set_pos(l, 24, 26);
  st_ses_u = label(c, "", F12, COL_TEXT2);
  lv_obj_align(st_ses_u, LV_ALIGN_TOP_RIGHT, -12, 26);
  dot(c, 12, 49, 7, COL_BLE);
  l = label(c, "Claude Desktop", F12, COL_TEXT);
  lv_obj_set_pos(l, 24, 44);
  st_ses_b = label(c, "", F12, COL_TEXT2);
  lv_obj_align(st_ses_b, LV_ALIGN_TOP_RIGHT, -12, 44);

  c = card(tab, 8, 176, 224, 84);
  h = label(c, LV_SYMBOL_OK " DECISIONS", F12, COL_TEXT2);
  lv_obj_set_pos(h, 12, 8);
  l = label(c, "Allow", F12, COL_TEXT);
  lv_obj_set_pos(l, 12, 26);
  lv_obj_t* t1 = plain(c, 56, 29, 126, 8);
  lv_obj_set_style_radius(t1, 4, 0); lv_obj_set_style_bg_opa(t1, LV_OPA_COVER, 0); lv_obj_set_style_bg_color(t1, C(0x0b1020), 0);
  lv_obj_set_style_clip_corner(t1, true, 0);
  st_allow_bar = plain(t1, 0, 0, 0, 8);
  lv_obj_set_style_bg_opa(st_allow_bar, LV_OPA_COVER, 0); lv_obj_set_style_bg_color(st_allow_bar, C(COL_OK), 0);
  st_allow_n = label(c, "0", F12, COL_TEXT);
  lv_obj_align(st_allow_n, LV_ALIGN_TOP_RIGHT, -12, 26);
  l = label(c, "Deny", F12, COL_TEXT);
  lv_obj_set_pos(l, 12, 44);
  lv_obj_t* t2 = plain(c, 56, 47, 126, 8);
  lv_obj_set_style_radius(t2, 4, 0); lv_obj_set_style_bg_opa(t2, LV_OPA_COVER, 0); lv_obj_set_style_bg_color(t2, C(0x0b1020), 0);
  lv_obj_set_style_clip_corner(t2, true, 0);
  st_deny_bar = plain(t2, 0, 0, 0, 8);
  lv_obj_set_style_bg_opa(st_deny_bar, LV_OPA_COVER, 0); lv_obj_set_style_bg_color(st_deny_bar, C(COL_BAD), 0);
  st_deny_n = label(c, "0", F12, COL_TEXT);
  lv_obj_align(st_deny_n, LV_ALIGN_TOP_RIGHT, -12, 44);
  st_rate = label(c, "Allow rate  --", F12, COL_TEXT2);
  lv_obj_set_pos(st_rate, 12, 62);

  c = card(tab, 8, 266, 224, 84);
  h = label(c, LV_SYMBOL_SETTINGS " SYSTEM", F12, COL_TEXT2);
  lv_obj_set_pos(h, 12, 8);
  st_dev  = kv(c, 26, "Device");
  st_link = kv(c, 40, "Link");
  st_up   = kv(c, 54, "Uptime");
  st_beat = kv(c, 68, "Last beat");
  lv_obj_t* pad = plain(tab, 8, 350, 1, 8);
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

static void anim_shadow(void* o, int32_t v) { lv_obj_set_style_shadow_width((lv_obj_t*)o, v, 0); }

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

  lv_obj_t* tl = label(at_card, "TOOL", F12, COL_TEXT2);
  lv_obj_set_pos(tl, 16, 44);
  at_tool = label(at_card, "", F28, COL_TEXT);
  lv_obj_set_width(at_tool, 180);
  lv_label_set_long_mode(at_tool, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(at_tool, 16, 58);

  lv_obj_t* box = plain(at_card, 14, 98, 184, 50);
  lv_obj_set_style_radius(box, 10, 0);
  lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(box, C(0x080b16), 0);
  lv_obj_set_style_border_width(box, 1, 0);
  lv_obj_set_style_border_color(box, C(COL_BORDER), 0);
  at_hint = label(box, "", F12, 0xa7e8c0);
  lv_obj_set_width(at_hint, 168);
  lv_label_set_long_mode(at_hint, LV_LABEL_LONG_DOT);
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
  lv_obj_set_width(s, 164);
  lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
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

static void srcs(lv_obj_t* lu, lv_obj_t* lb, uint8_t u, uint8_t b) {
  char t[8];
  snprintf(t, sizeof t, "%u", u); setText(lu, t);
  lv_obj_set_style_text_color(lu, C(u ? COL_USB : COL_DIM), 0);
  snprintf(t, sizeof t, "%u", b); setText(lb, t);
  lv_obj_set_style_text_color(lb, C(b ? COL_BLE : COL_DIM), 0);
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
  setText(hero_title, title);

  // strip
  uint8_t run = (m.usbLive ? m.run[0] : 0) + (m.bleConn ? m.run[1] : 0);
  uint8_t wait = (m.usbLive ? m.wait[0] : 0) + (m.bleConn ? m.wait[1] : 0);
  snprintf(t, sizeof t, "%u", run);  setText(run_val, t);
  lv_obj_set_style_text_color(run_val, C(run ? ACC[UI_BUSY] : COL_TEXT), 0);
  snprintf(t, sizeof t, "%u", wait); setText(wait_val, t);
  lv_obj_set_style_text_color(wait_val, C(wait ? ACC[UI_ATTN] : COL_TEXT), 0);
  srcs(run_u, run_b, m.usbLive ? m.run[0] : 0, m.bleConn ? m.run[1] : 0);
  srcs(wait_u, wait_b, m.usbLive ? m.wait[0] : 0, m.bleConn ? m.wait[1] : 0);
  uint32_t tokTotal = m.tok[0] + m.tok[1];
  fmtTok(t, sizeof t, tokTotal); setText(tok_val, t);
  split(tok_bar_u, tok_bar_b, 56, m.tok[0], m.tok[1]);

  // activity
  static const uint32_t fade[3] = {COL_TEXT, 0xaab4d6, COL_DIM};
  for (int i = 0; i < 3; i++) {
    bool has = i < m.nEntries;
    setText(act_rows[i], has ? m.entries[i] : (i == 0 ? "nothing yet" : ""));
    lv_obj_set_style_text_color(act_rows[i], C(has ? fade[i] : COL_DIM), 0);
    lv_obj_set_style_bg_color(act_dots[i], C(has ? (i == 0 ? ACC[st] : COL_DIM) : 0x1c2546), 0);
  }

  // stats tab
  fmtTok(t, sizeof t, tokTotal); setText(st_tok, t);
  if (m.tokLife) { fmtTok(t2, sizeof t2, m.tokLife); snprintf(t, sizeof t, "lifetime %s", t2); setText(st_life, t); }
  fmtTok(t2, sizeof t2, m.tok[0]); snprintf(t, sizeof t, "USB  %s", t2); setText(st_leg_u, t);
  fmtTok(t2, sizeof t2, m.tok[1]); snprintf(t, sizeof t, "BLE  %s", t2); setText(st_leg_b, t);
  split(st_bar_u, st_bar_b, 200, m.tok[0], m.tok[1]);
  snprintf(t, sizeof t, "run %u  wait %u", m.usbLive ? m.run[0] : 0, m.usbLive ? m.wait[0] : 0);
  setText(st_ses_u, m.usbLive ? t : "not linked");
  snprintf(t, sizeof t, "run %u  wait %u", m.bleConn ? m.run[1] : 0, m.bleConn ? m.wait[1] : 0);
  setText(st_ses_b, m.bleConn ? t : "not linked");
  uint32_t tot = m.approvals + m.denials;
  lv_obj_set_width(st_allow_bar, tot ? (int)((uint64_t)m.approvals * 126 / tot) : 0);
  lv_obj_set_width(st_deny_bar, tot ? (int)((uint64_t)m.denials * 126 / tot) : 0);
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
    setText(at_tool, m.pTool[0] ? m.pTool : "Tool");
    snprintf(t, sizeof t, "%s", m.pHint);
    setText(at_hint, t[0] ? t : m.pId);
    setFlag(at_note, LV_OBJ_FLAG_HIDDEN, !m.pInfo);
    setFlag(at_btn_info, LV_OBJ_FLAG_HIDDEN, !m.pInfo);
    setFlag(at_btn_deny, LV_OBJ_FLAG_HIDDEN, m.pInfo);
    setFlag(at_btn_allow, LV_OBJ_FLAG_HIDDEN, m.pInfo);
  }
  setFlag(ov_attn, LV_OBJ_FLAG_HIDDEN, !attn);

  // link overlay
  setFlag(ov_link, LV_OBJ_FLAG_HIDDEN, online);
  if (!online) {
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
