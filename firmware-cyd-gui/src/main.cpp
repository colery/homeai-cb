/*
 * Claude Buddy — CYD (ESP32-2432S028), LVGL GUI.
 *
 * Links: USB serial (Claude Code via the hub) and BLE (Claude Desktop), both newline-
 * delimited JSON, tracked per source. The UI is src/ui.cpp (pure LVGL); this file is
 * hardware + protocol + glue.
 */
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#include <ArduinoJson.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <esp_mac.h>
#include <esp_bt.h>
#include <Preferences.h>
#include <lvgl.h>
#include "ui.h"

SET_LOOP_TASK_STACK_SIZE(12 * 1024);   // LVGL's renderer recurses; the default 8 KB is tight

// ── Hardware ───────────────────────────────────────────────────────────────
#define LED_RED    4
#define LED_GREEN 16
#define LED_BLUE  17
#define BL_PIN    21
#define TOUCH_CS  33
#define TOUCH_IRQ 36
#define TOUCH_CLK 25
#define TOUCH_MOSI 32
#define TOUCH_MISO 39

#define SCR_W 240
#define SCR_H 320
#ifdef NEW_BOARD
  #define SCR_ROT 3      // ILI9342 panel is mounted 90 degrees off; rotation 3 gives 240x320 portrait
#else
  #define SCR_ROT 0
#endif
#define TS_ROT 0

#define TCH_X_MIN   230
#define TCH_X_MAX  3900
#define TCH_Y_MIN   230
#define TCH_Y_MAX  3900

#define NUS_SVC "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_RX  "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_TX  "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

TFT_eSPI            tft;
SPIClass            tspi(HSPI);
XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);

// ── BLE ────────────────────────────────────────────────────────────────────
#define BLE_BUF 4096
static uint8_t            ring[BLE_BUF];
static volatile uint16_t  rH = 0, rT = 0;
static BLECharacteristic* pTx = nullptr;
static volatile bool      bleConn = false, bleSec = false, showKey = false;
static volatile uint32_t  connAt = 0, bleKey = 0;
static char               devName[20] = "Claude-????";

static void rPush(const char* d, size_t n) { for (size_t i = 0; i < n; i++) { uint16_t nx = (rH + 1) % BLE_BUF; if (nx != rT) { ring[rH] = (uint8_t)d[i]; rH = nx; } } }
static int  rAvail() { return (rH + BLE_BUF - rT) % BLE_BUF; }
static char rRead()  { char c = (char)ring[rT]; rT = (rT + 1) % BLE_BUF; return c; }

static void bleTx(const char* s) {
  if (!bleConn || !bleSec || !pTx) return;
  char b[512]; size_t n = snprintf(b, sizeof(b), "%s\n", s);
  for (size_t o = 0; o < n; o += 20) { size_t k = n - o < 20 ? n - o : 20; pTx->setValue((uint8_t*)(b + o), k); pTx->notify(); }
}

class SecCB : public BLESecurityCallbacks {
  uint32_t onPassKeyRequest() override { Serial.println("# ble: passkey request"); return 0; }
  void onPassKeyNotify(uint32_t k) override { Serial.printf("# ble: passkey notify %06lu\n", (unsigned long)k); bleKey = k; showKey = true; }
  bool onConfirmPIN(uint32_t k) override { Serial.printf("# ble: confirm pin %06lu\n", (unsigned long)k); return true; }
  bool onSecurityRequest() override { Serial.println("# ble: security request"); return true; }
  void onAuthenticationComplete(esp_ble_auth_cmpl_t c) override {
    Serial.printf("# ble: auth complete success=%d reason=0x%x\n", (int)c.success, (unsigned)c.fail_reason);
    if (c.success) { bleSec = true; showKey = false; }
  }
};
class SrvCB : public BLEServerCallbacks {
  void onConnect(BLEServer*) override { Serial.println("# ble: connected"); bleConn = true; bleSec = false; showKey = false; connAt = millis(); }
  void onDisconnect(BLEServer*) override { Serial.println("# ble: disconnected"); bleConn = false; bleSec = false; showKey = false; BLEDevice::startAdvertising(); }
};
class RxCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* ch) override { std::string v = ch->getValue(); rPush(v.c_str(), v.size()); }
};

// ── USB serial link (Claude Code, via the bridge hub) ──────────────────────
static char     sbuf[4096]; static int slen = 0;
static uint32_t usbAt = 0;
static inline bool usbLive() { return usbAt && (int32_t)(millis() - usbAt) < 15000; }
static inline bool online()  { return usbLive() || (bleConn && bleSec); }
static void linkTx(const char* s) { bleTx(s); if (usbLive()) Serial.println(s); }

// ── State ──────────────────────────────────────────────────────────────────
struct Buddy {
  bool conn = false; uint8_t running = 0, waiting = 0;
  uint32_t tokToday = 0, tokTotal = 0, lastBeat = 0;
  uint8_t runS[2] = {}, waitS[2] = {}; uint32_t tokS[2] = {}; uint8_t pSrc = 0;   // [0]=USB, [1]=BLE
  char msg[64] = "";                                    // latest from either source (hero title)
  char msgS[2][64] = {"", ""}; char entriesS[2][UI_ENTRIES][52] = {}; uint8_t nEntriesS[2] = {0, 0};
  struct Lim { bool has5 = false, has7 = false; float pct5 = 0, pct7 = 0; int32_t rst5 = -1, rst7 = -1; } lim[2];
  int ctx = -1; uint32_t limAt = 0;
  uint32_t tokMaxB = 0;                                  // biggest BLE day seen (persisted)
  char pId[40] = "", pTool[20] = "", pHint[44] = ""; bool pInfo = false;
  uint16_t approvals = 0, denials = 0;
  uint8_t spark[UI_SPARK] = {}; uint8_t nSpark = 0;
  uint32_t epoch = 0, epochAt = 0; int32_t tzOff = 0;
} g;
enum BS { S_SLEEP, S_IDLE, S_BUSY, S_ATTN };
static BS gState = S_SLEEP;
static uint32_t turnEnd = 0;
static char dismissedId[40] = "";

#define LOG_N 50
#define LOG_W 60
static char     log_buf[LOG_N][LOG_W + 1];
static int      logI = 0, logTotal = 0;
static uint32_t logSeq = 1;
static void logLine(const char* s) {
  strncpy(log_buf[logI], s, LOG_W); log_buf[logI][LOG_W] = 0;
  logI = (logI + 1) % LOG_N; if (logTotal < LOG_N) logTotal++; logSeq++;
}

static void sumSources() {
  bool on[2] = {usbLive(), (bool)bleConn};
  g.running = g.waiting = 0; g.tokToday = 0;
  for (int i = 0; i < 2; i++) {
    if (on[i]) { g.running += g.runS[i]; g.waiting += g.waitS[i]; }
    g.tokToday += g.tokS[i];
  }
}
static void recompute() {
  sumSources();
  if (!g.conn || (int32_t)(millis() - g.lastBeat) > 15000) gState = S_SLEEP;
  else if (g.waiting > 0) gState = S_ATTN;
  else if (g.running > 0) gState = S_BUSY;
  else                    gState = S_IDLE;
}

static void getTime(char* o) {
  if (!g.epoch) { strcpy(o, "--:--"); return; }
  uint32_t s = g.epoch + (millis() - g.epochAt) / 1000 + g.tzOff;
  snprintf(o, 8, "%02lu:%02lu", (unsigned long)((s % 86400) / 3600), (unsigned long)((s % 3600) / 60));
}

// ── Protocol ───────────────────────────────────────────────────────────────
static void sendPerm(const char* id, const char* dec) {
  char b[128]; snprintf(b, 128, "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"%s\"}", id, dec);
  linkTx(b);
}

static void parseLine(const char* line, bool usb = false) {
  if (!usb) logLine(line);
  JsonDocument doc;
  if (deserializeJson(doc, line) != DeserializationError::Ok) return;
  if (usb) logLine(line);
  g.conn = true; g.lastBeat = millis();
  if (usb) usbAt = millis();

  JsonArray ta = doc["time"].as<JsonArray>();
  if (!ta.isNull() && ta.size() >= 2) { g.epoch = ta[0].as<uint32_t>(); g.epochAt = millis(); g.tzOff = ta[1].as<int32_t>(); }

  const char* evt = doc["evt"];
  if (evt && strcmp(evt, "turn") == 0) {
    const char* role = doc["role"];
    if (role && strcmp(role, "user") == 0) { strncpy(g.msg, "thinking...", 63); strncpy(g.msgS[1], "thinking...", 63); turnEnd = millis() + 30000; g.runS[1] = 1; }
    else if (role && strcmp(role, "assistant") == 0) { strncpy(g.msg, "done", 63); strncpy(g.msgS[1], "done", 63); turnEnd = 0; g.runS[1] = 0; }
    recompute();
    return;
  }

  const uint8_t src = usb ? 0 : 1;
  if (!doc["running"].isNull())      g.runS[src]  = doc["running"].as<uint8_t>();
  if (!doc["waiting"].isNull())      g.waitS[src] = doc["waiting"].as<uint8_t>();
  if (!doc["tokens_today"].isNull()) g.tokS[src]  = doc["tokens_today"].as<uint32_t>();
  if (!doc["tokens"].isNull())       g.tokTotal   = doc["tokens"].as<uint32_t>();
  const char* m = doc["msg"]; if (m) { strncpy(g.msg, m, 63); g.msg[63] = 0; strncpy(g.msgS[src], m, 63); g.msgS[src][63] = 0; }

  JsonArray ea = doc["entries"].as<JsonArray>();
  if (!ea.isNull()) {
    g.nEntriesS[src] = 0;
    for (JsonVariant v : ea) { uint8_t& n = g.nEntriesS[src]; if (n >= UI_ENTRIES) break; strncpy(g.entriesS[src][n], v.as<const char*>(), 51); g.entriesS[src][n++][51] = 0; }
  }

  if (usb) {
    JsonObject lm = doc["limits"].as<JsonObject>();
    auto rd = [](JsonObject o, Buddy::Lim& L) {
      L.has5 = !o["h5"].isNull(); L.pct5 = o["h5"] | 0.0f; L.rst5 = o["h5s"] | -1;
      L.has7 = !o["d7"].isNull(); L.pct7 = o["d7"] | 0.0f; L.rst7 = o["d7s"] | -1;
    };
    if (!lm.isNull()) {
      rd(lm, g.lim[0]);
      JsonObject lb = lm["b"].as<JsonObject>();
      if (!lb.isNull()) rd(lb, g.lim[1]); else g.lim[1] = Buddy::Lim();
      g.ctx = lm["cx"].isNull() ? -1 : (int)(lm["cx"].as<float>() + 0.5f);
      g.limAt = millis();
    } else { g.lim[0] = g.lim[1] = Buddy::Lim(); g.ctx = -1; }
  }

  JsonArray sp = doc["spark"].as<JsonArray>();
  if (!sp.isNull()) {
    g.nSpark = 0;
    for (JsonVariant v : sp) { if (g.nSpark >= UI_SPARK) break; int n = v.as<int>(); g.spark[g.nSpark++] = n < 0 ? 0 : n > 255 ? 255 : n; }
  }

  JsonObject p = doc["prompt"].as<JsonObject>();
  if (!p.isNull()) {
    const char* pid = p["id"] | "";
    if (dismissedId[0] && !strcmp(pid, dismissedId)) { g.waitS[src] = 0; }
    else { strncpy(g.pId, pid, 39); strncpy(g.pTool, p["tool"] | "", 19); strncpy(g.pHint, p["hint"] | "", 43); g.pInfo = p["info"] | false; g.waitS[src] = 1; g.pSrc = src; }
  } else if (src == g.pSrc && !doc["waiting"].isNull() && doc["waiting"].as<uint8_t>() == 0) {
    g.pId[0] = g.pTool[0] = g.pHint[0] = 0; g.pInfo = false; dismissedId[0] = 0;
  }

  const char* cmd = doc["cmd"];
  if (cmd) {
    if      (!strcmp(cmd, "status")) { char r[128]; snprintf(r, 128, "{\"ack\":\"status\",\"ok\":true,\"data\":{\"name\":\"%s\",\"sec\":%s}}", devName, bleSec ? "true" : "false"); linkTx(r); }
    else if (!strcmp(cmd, "name"))   linkTx("{\"ack\":\"name\",\"ok\":true}");
    else if (!strcmp(cmd, "owner"))  linkTx("{\"ack\":\"owner\",\"ok\":true}");
    else if (!strcmp(cmd, "unpair")) linkTx("{\"ack\":\"unpair\",\"ok\":true}");
  }
  recompute();
}

// ── UI callbacks ───────────────────────────────────────────────────────────
static void onPermission(bool allow) {
  if (!g.pId[0]) { g.waitS[g.pSrc] = 0; recompute(); return; }
  char pl[48]; snprintf(pl, 48, "PERM %s: %s", g.pTool, allow ? "allow" : "deny"); logLine(pl);
  sendPerm(g.pId, allow ? "once" : "deny");
  if (allow) g.approvals++; else g.denials++;
  g.waitS[g.pSrc] = 0; g.pId[0] = 0; recompute();
}
static void onDismiss() {
  strncpy(dismissedId, g.pId, 39); dismissedId[39] = 0;
  g.waitS[g.pSrc] = 0; g.pId[0] = 0; g.pInfo = false; recompute();
}

// ── LVGL glue ──────────────────────────────────────────────────────────────
static uint8_t lvBuf1[SCR_W * 24 * 2], lvBuf2[SCR_W * 24 * 2];   // small stripes: LVGL layers (rounded clips) are sized by the stripe

static uint32_t flushes = 0;
static void flushCb(lv_display_t* d, const lv_area_t* a, uint8_t* px) {
  flushes++;
  uint32_t w = a->x2 - a->x1 + 1, h = a->y2 - a->y1 + 1;
  tft.startWrite();
  tft.setAddrWindow(a->x1, a->y1, w, h);
  lv_draw_sw_rgb565_swap(px, w * h);
  tft.pushPixels((uint16_t*)px, w * h);
  tft.endWrite();
  lv_display_flush_ready(d);
}

static void touchCb(lv_indev_t*, lv_indev_data_t* data) {
  if (ts.tirqTouched() && ts.touched()) {
    TS_Point p = ts.getPoint();
    data->point.x = constrain(map(p.x, TCH_X_MIN, TCH_X_MAX, 0, SCR_W - 1), 0, SCR_W - 1);
    data->point.y = constrain(map(p.y, TCH_Y_MIN, TCH_Y_MAX, 0, SCR_H - 1), 0, SCR_H - 1);
    data->state = LV_INDEV_STATE_PRESSED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

static UiModel um;
static void fillModel() {
  um.state = (UiState)gState;
  strlcpy(um.devName, devName, sizeof um.devName);
  getTime(um.time);
  um.usbLive = usbLive(); um.bleConn = bleConn; um.bleSec = bleSec; um.showKey = showKey; um.bleKey = bleKey;
  for (int i = 0; i < 2; i++) { um.run[i] = g.runS[i]; um.wait[i] = g.waitS[i]; um.tok[i] = g.tokS[i]; }
  um.tokLife = g.tokTotal;
  um.tokBestB = g.tokMaxB;
  strlcpy(um.msg, g.msg, sizeof um.msg);
  for (int s = 0; s < 2; s++) {
    strlcpy(um.msgS[s], g.msgS[s], sizeof um.msgS[s]);
    um.nEntriesS[s] = g.nEntriesS[s];
    for (int i = 0; i < g.nEntriesS[s] && i < UI_ENTRIES; i++) strlcpy(um.entriesS[s][i], g.entriesS[s][i], sizeof um.entriesS[s][i]);
  }
  um.pSrc = g.pSrc;
  uint32_t el = (millis() - g.limAt) / 1000;
  for (int i = 0; i < 2; i++) {
    const Buddy::Lim& L = g.lim[i];
    um.lim[i].has5 = L.has5; um.lim[i].has7 = L.has7;
    um.lim[i].pct5 = (uint8_t)(L.pct5 + 0.5f); um.lim[i].pct7 = (uint8_t)(L.pct7 + 0.5f);
    um.lim[i].rst5 = L.rst5 >= 0 ? (L.rst5 > (int32_t)el ? L.rst5 - el : 0) : -1;
    um.lim[i].rst7 = L.rst7 >= 0 ? (L.rst7 > (int32_t)el ? L.rst7 - el : 0) : -1;
  }
  um.ctx = g.ctx;
  um.hasPrompt = g.pId[0] != 0; um.pInfo = g.pInfo;
  strlcpy(um.pTool, g.pTool, sizeof um.pTool); strlcpy(um.pHint, g.pHint, sizeof um.pHint); strlcpy(um.pId, g.pId, sizeof um.pId);
  um.approvals = g.approvals; um.denials = g.denials;
  um.uptimeS = millis() / 1000;
  um.beatAgeS = g.lastBeat ? (int32_t)(millis() - g.lastBeat) / 1000 : -1;
  um.logCount = logTotal;
  for (int i = 0; i < logTotal; i++) um.logLines[i] = log_buf[(logI - 1 - i + LOG_N * 2) % LOG_N];
  um.logSeq = logSeq;
  um.nSpark = g.nSpark; memcpy(um.spark, g.spark, sizeof um.spark);
}

// ── BLE init ───────────────────────────────────────────────────────────────
static void initBLE() {
  esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);   // BLE only: hand the classic-BT RAM back to the heap
  BLEDevice::init(devName);
  BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT);
  BLEDevice::setSecurityCallbacks(new SecCB());
  BLESecurity* pSec = new BLESecurity();
  pSec->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);
  pSec->setCapability(ESP_IO_CAP_OUT);
  pSec->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  pSec->setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  pSec->setKeySize(16);
  BLEServer* srv = BLEDevice::createServer(); srv->setCallbacks(new SrvCB());
  BLEService* svc = srv->createService(BLEUUID(NUS_SVC));
  pTx = svc->createCharacteristic(BLEUUID(NUS_TX), BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  pTx->addDescriptor(new BLE2902());
  BLECharacteristic* rx = svc->createCharacteristic(BLEUUID(NUS_RX), BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  rx->setCallbacks(new RxCB());
  svc->start();
  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(BLEUUID(NUS_SVC)); adv->setScanResponse(true);
  adv->setMinPreferred(0x06); adv->setMaxPreferred(0x12);
  BLEDevice::startAdvertising();
}

// Debug: if loop() stops advancing, dump the stalled task's saved PC and code-looking stack words.
static volatile uint32_t loopTicks = 0;
static Preferences prefs;
static void samplerTask(void*) {
  uint32_t last = 0; int stuck = 0;
  for (;;) {
    vTaskDelay(500 / portTICK_PERIOD_MS);
    if (loopTicks == last) {
      if (++stuck == 5) {
        TaskHandle_t h = xTaskGetHandle("loopTask");
        vTaskSuspend(h);
        uint32_t* sp = *(uint32_t**)h;
        Serial.printf("# STUCK pc=0x%08x a0=0x%08x\n", sp[1], sp[3]);
        for (int i = 0; i < 400; i++) {
          uint32_t w = sp[i];
          if ((w >= 0x400D0000 && w < 0x40400000) || (w >= 0x40080000 && w < 0x400A0000)) Serial.printf("# w%d=0x%08x\n", i, w);
        }
        vTaskResume(h);
      }
    } else { stuck = 0; last = loopTicks; }
  }
}

// ── Arduino ────────────────────────────────────────────────────────────────
void setup() {
  Serial.setRxBufferSize(2048);
  Serial.begin(115200);
  pinMode(LED_RED, OUTPUT);   digitalWrite(LED_RED, HIGH);
  pinMode(LED_GREEN, OUTPUT); digitalWrite(LED_GREEN, HIGH);
  pinMode(LED_BLUE, OUTPUT);  digitalWrite(LED_BLUE, HIGH);
  pinMode(BL_PIN, OUTPUT);    digitalWrite(BL_PIN, HIGH);

  tft.init(); tft.setRotation(SCR_ROT); tft.fillScreen(0x0841);
  tft.setTextColor(0x07FF, 0x0841); tft.setTextSize(2); tft.setCursor(20, 140); tft.print("Claude Buddy");
  tft.setTextSize(1); tft.setCursor(20, 164); tft.print("starting GUI...");
  Serial.println("# tft ok");
  tspi.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  ts.begin(tspi); ts.setRotation(TS_ROT);

  uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_BT);
  snprintf(devName, sizeof(devName), "Claude-%02X%02X", mac[4], mac[5]);

  initBLE();
  Serial.printf("# ble ok heap=%u max=%u\n", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());

  lv_log_register_print_cb([](lv_log_level_t, const char* m) { Serial.print("# lv: "); Serial.print(m); });
  lv_init();
  Serial.printf("# lv_init ok heap=%u\n", (unsigned)ESP.getFreeHeap());
  lv_tick_set_cb([]() -> uint32_t { return millis(); });
  lv_display_t* disp = lv_display_create(SCR_W, SCR_H);
  lv_display_set_buffers(disp, lvBuf1, lvBuf2, sizeof lvBuf1, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(disp, flushCb);
  lv_indev_t* indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, touchCb);

  ui_init(onPermission, onDismiss);
  Serial.printf("# ui_init ok heap=%u max=%u\n", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  prefs.begin("buddy", false);
  g.tokMaxB = prefs.getUInt("tokmaxB", 0);
  xTaskCreatePinnedToCore(samplerTask, "samp", 4096, NULL, 1, NULL, 0);
  enableLoopWDT();   // a stall prints a backtrace and reboots instead of hanging silently
  Serial.printf("\n{\"hello\":\"claude-buddy\",\"name\":\"%s\"}\n", devName);
}

void loop() {
  loopTicks++;
  feedLoopWDT();
  uint32_t now = millis();

  while (rAvail() > 0) {
    static char lbuf[4096]; static int llen = 0;
    char c = rRead();
    if (c == '\n' || c == '\r') { if (llen > 0) { lbuf[llen] = 0; parseLine(lbuf); llen = 0; } }
    else if (llen < (int)sizeof(lbuf) - 1) lbuf[llen++] = c;
  }
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') { if (slen > 0) { sbuf[slen] = 0; parseLine(sbuf, true); slen = 0; } }
    else if (slen < (int)sizeof(sbuf) - 1) sbuf[slen++] = c;
    else slen = 0;
  }

  if (bleConn && !bleSec && !showKey && (now - connAt) > 3000) bleSec = true;
  if (turnEnd > 0 && now > turnEnd) { turnEnd = 0; g.runS[1] = 0; strncpy(g.msg, "idle", 63); strncpy(g.msgS[1], "idle", 63); }
  if (g.conn && (int32_t)(now - g.lastBeat) > 15000) g.conn = false;
  recompute();

  // remember the biggest Bluetooth (Claude Desktop) day, for scaling the B bar
  static uint32_t lastSave = 0; static bool maxDirty = false;
  if (g.tokS[1] > g.tokMaxB) { g.tokMaxB = g.tokS[1]; maxDirty = true; }
  if (maxDirty && now - lastSave > 30000) { prefs.putUInt("tokmaxB", g.tokMaxB); maxDirty = false; lastSave = now; }

  // LEDs
  static uint32_t lastLed = 0; static bool ledOn = false;
  if (gState == S_ATTN) {
    if (now - lastLed > 400) { lastLed = now; ledOn = !ledOn; digitalWrite(LED_RED, ledOn ? LOW : HIGH); digitalWrite(LED_GREEN, HIGH); }
  } else if (online()) {
    if (now - lastLed > 2000) { lastLed = now; ledOn = !ledOn; digitalWrite(LED_RED, HIGH); digitalWrite(LED_GREEN, ledOn ? LOW : HIGH); }
  } else { digitalWrite(LED_RED, HIGH); digitalWrite(LED_GREEN, HIGH); }

  static uint32_t lastUi = 0;
  if (now - lastUi >= 80) { lastUi = now; fillModel(); ui_update(um); }

  static uint32_t lastDbg = 0;
  if (now - lastDbg > 10000) { lastDbg = now; Serial.printf("# alive t=%lus flushes=%lu heap=%u min=%u\n", (unsigned long)(now / 1000), (unsigned long)flushes, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap()); }
  uint32_t wait = lv_timer_handler();
  delay(wait > 10 ? 10 : (wait ? wait : 1));
}
