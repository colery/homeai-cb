// Host-side renderer: builds the real UI (src/ui.cpp) against LVGL with a RAM framebuffer
// and writes one raw RGB565 frame per scenario. render.py turns them into PNGs.
#include <lvgl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "../src/ui.h"

#define W 240
#define H 320
static uint16_t fb[W * H];
static uint32_t now_ms = 0;
static uint32_t tick_cb(void) { return now_ms; }

static void flush_cb(lv_display_t* d, const lv_area_t* a, uint8_t* px) {
  uint16_t* p = (uint16_t*)px;
  for (int y = a->y1; y <= a->y2; y++)
    for (int x = a->x1; x <= a->x2; x++) fb[y * W + x] = *p++;
  lv_display_flush_ready(d);
}

static void run(int ms) {
  for (int i = 0; i < ms; i += 20) { now_ms += 20; lv_timer_handler(); }
}

static void shot(const char* name) {
  run(60);
  lv_obj_invalidate(lv_screen_active());
  lv_obj_invalidate(lv_layer_top());
  run(60);
  char path[128]; snprintf(path, sizeof path, "out/%s.rgb565", name);
  FILE* f = fopen(path, "wb"); fwrite(fb, 1, sizeof fb, f); fclose(f);
  printf("wrote %s\n", path);
}

static void on_perm(bool) {}
static void on_dismiss() {}

static const char* LOG[] = {
  "{\"total\":2,\"running\":1,\"waiting\":0,\"msg\":\"Tool: Bash\"}",
  "{\"total\":2,\"running\":1,\"waiting\":0,\"msg\":\"thinking...\"}",
  "{\"cmd\":\"permission\",\"id\":\"cc:aaaaaa\",\"decision\":\"once\"}",
  "PERM Bash: allow",
  "{\"evt\":\"turn\",\"role\":\"user\"}",
  "{\"total\":1,\"running\":0,\"waiting\":0,\"msg\":\"idle\"}",
};

static UiModel base() {
  UiModel m; memset(&m, 0, sizeof m);
  strcpy(m.devName, "Claude-E72E"); strcpy(m.time, "14:32");
  m.state = UI_IDLE; m.usbLive = true; m.bleConn = true; m.bleSec = true;
  strcpy(m.msg, "idle");
  m.tok[0] = 48210; m.tok[1] = 12400; m.tokLife = 1420000;
  m.nEntries = 5;
  strcpy(m.entries[0], "14:32 Bash pio run -e cyd");
  strcpy(m.entries[1], "14:31 Edit ui.cpp");
  strcpy(m.entries[2], "14:31 acer-ai Read main.cpp");
  strcpy(m.entries[3], "14:30 > prompt");
  strcpy(m.entries[4], "14:29 Grep lv_obj");
  m.approvals = 14; m.denials = 3; m.uptimeS = 5025; m.beatAgeS = 1;
  m.logCount = 6; for (int i = 0; i < 6; i++) m.logLines[i] = LOG[i];
  m.logSeq = 1;
  return m;
}

int main() {
  lv_init();
  lv_tick_set_cb(tick_cb);
  lv_display_t* d = lv_display_create(W, H);
  static uint8_t buf1[W * 40 * 2], buf2[W * 40 * 2];
  lv_display_set_buffers(d, buf1, buf2, sizeof buf1, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(d, flush_cb);
  ui_init(on_perm, on_dismiss);

  UiModel m = base();
  ui_update(m); run(300); shot("idle");

  m.state = UI_BUSY; strcpy(m.msg, "Tool: Bash"); m.run[0] = 1; m.run[1] = 1;
  ui_update(m); run(500); shot("busy");

  m.state = UI_ATTN; m.hasPrompt = true; m.pInfo = true; m.wait[0] = 1;
  strcpy(m.pTool, "Bash"); strcpy(m.pHint, "git push origin main --force-with-lease"); strcpy(m.pId, "cc:aaaaaa:1");
  ui_update(m); run(400); shot("attn_info");

  m.pInfo = false; strcpy(m.pTool, "Edit"); strcpy(m.pHint, "/home/ryan/github/homeai-cb/main.cpp");
  ui_update(m); run(300); shot("attn_action");

  m = base(); m.state = UI_SLEEP; m.usbLive = false; m.bleConn = true; m.bleSec = true; m.msg[0] = 0; m.nEntries = 0;
  ui_update(m); run(300); shot("sleep");

  m = base(); ui_update(m); ui_set_tab(1); run(500); shot("stats");
  ui_set_tab(2); run(500); shot("log");
  ui_set_tab(0);

  m = base(); m.usbLive = false; m.bleConn = false; m.bleSec = false;
  ui_update(m); run(300); shot("link_wait");
  m.bleConn = true; m.showKey = true; m.bleKey = 482913;
  ui_update(m); run(300); shot("link_key");
  return 0;
}
