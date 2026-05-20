/*
 * Claude Buddy — M5StickC (original, 80×160)
 *
 * BLE Nordic UART Service transport — pairs with Claude Desktop on macOS/Windows.
 *
 * To pair: open Claude Desktop → Help → Troubleshooting → Enable Developer Mode,
 * then Developer → Open Hardware Buddy…
 *
 * Button A (front): approve permission prompt
 * Button B (right): deny permission prompt
 * LED (GPIO10): blinks red during ATTENTION state
 */

#include <M5StickC.h>
#include <ArduinoJson.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <esp_mac.h>

// Nordic UART Service UUIDs (claude-desktop-buddy spec)
#define NUS_SVC "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_RX  "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_TX  "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

// Palette
#define BG       TFT_BLACK
#define FG       TFT_WHITE
#define DIM      0x7BEF
#define C_IDLE   TFT_WHITE
#define C_BUSY   TFT_GREEN
#define C_ATTN   TFT_YELLOW
#define C_SLEEP  0x39E7
#define C_PAIR   TFT_CYAN

// ── BLE ring buffer (written from BLE task, read from main loop) ────────────
#define BLE_BUF 2048
static uint8_t           ring[BLE_BUF];
static volatile uint16_t rHead = 0, rTail = 0;

static BLECharacteristic* pTxChar    = nullptr;
static volatile bool      bleConn    = false;
static volatile bool      bleSecure  = false;
static volatile uint32_t  connectedAt = 0;  // millis() when last connected
static volatile uint32_t  bleKey     = 0;
static volatile bool      showKey    = false;
static char               devName[20] = "Claude-????";

static void ringPush(const char* data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    uint16_t next = (rHead + 1) % BLE_BUF;
    if (next != rTail) { ring[rHead] = (uint8_t)data[i]; rHead = next; }
  }
}
static int  ringAvail() { return (rHead + BLE_BUF - rTail) % BLE_BUF; }
static char ringRead()  { char c = (char)ring[rTail]; rTail = (rTail+1)%BLE_BUF; return c; }

static void bleSend(const char* str) {
  if (!bleConn || !bleSecure || !pTxChar) return;
  // Build full line with newline, then send in 20-byte chunks (safe MTU floor)
  char buf[512];
  size_t len = snprintf(buf, sizeof(buf), "%s\n", str);
  for (size_t off = 0; off < len; off += 20) {
    size_t n = len - off < 20 ? len - off : 20;
    pTxChar->setValue((uint8_t*)(buf + off), n);
    pTxChar->notify();
  }
}

// ── BLE callbacks ──────────────────────────────────────────────────────────
class SecCB : public BLESecurityCallbacks {
  uint32_t onPassKeyRequest() override { return 0; }
  void onPassKeyNotify(uint32_t pk) override { bleKey = pk; showKey = true; }
  bool onConfirmPIN(uint32_t)       override { return true; }
  bool onSecurityRequest()          override { return true; }
  void onAuthenticationComplete(esp_ble_auth_cmpl_t c) override {
    if (c.success) { bleSecure = true; showKey = false; }
  }
};

class SrvCB : public BLEServerCallbacks {
  void onConnect(BLEServer*)    override { bleConn = true; bleSecure = false; showKey = false; connectedAt = millis(); }
  void onDisconnect(BLEServer*) override {
    bleConn = false; bleSecure = false; showKey = false;
    BLEDevice::startAdvertising();
  }
};

class RxCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* ch) override {
    std::string v = ch->getValue();
    ringPush(v.c_str(), v.size());
  }
};

// ── Buddy state ────────────────────────────────────────────────────────────
struct Buddy {
  bool     connected   = false;
  uint8_t  running     = 0;
  uint8_t  waiting     = 0;
  uint32_t tokensToday = 0;
  uint32_t lastBeat    = 0;
  char     msg[48]     = "";
  char     lines[3][52]   = {};
  uint8_t  nLines         = 0;
  char     promptId[40]   = "";
  char     promptTool[20] = "";
  char     promptHint[44] = "";
} g;

enum BState { S_SLEEP, S_IDLE, S_BUSY, S_ATTENTION };
BState gState = S_SLEEP;

// ASCII art [state][frame][line]
static const char* ART[4][2][3] = {
  {{ " /\\_/\\  ", " (-.-) z", "  >   <  " },
   { " /\\_/\\  ", " ( -.-) Z", "  >   <  " }},
  {{ " /\\_/\\  ", " ( o.o ) ", "  >   <  " },
   { " /\\_/\\  ", " ( o.o ) ", "  >   <  " }},
  {{ " /\\_/\\  ", " ( >.< ) ", "  >[#]<  " },
   { " /\\_/\\  ", " ( <.> ) ", "  >[#]<  " }},
  {{ " /\\_/\\  ", " (>O.O<) ", "  > ! <  " },
   { " /\\_/\\  ", " (>O.O<) ", "  >!!!<  " }},
};
static const char* STATE_LABEL[] = { "sleeping", "idle    ", "working!", "APPROVE?" };
static uint16_t    STATE_COLOR[] = { C_SLEEP, C_IDLE, C_BUSY, C_ATTN };

static char     lineBuf[2048];  // large enough for turn events with content
static int      lineBufLen = 0;
static uint32_t lastDraw   = 0;
static uint32_t lastLed    = 0;
static uint8_t  animFrame  = 0;
static bool     ledOn      = false;
static uint32_t turnBusy   = 0;   // millis() until we hold "busy" after a user turn

// ── Display ────────────────────────────────────────────────────────────────
static void drawAdvertising() {
  M5.Lcd.fillScreen(BG);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(C_PAIR, BG);
  // Center the device name
  int nameW = strlen(devName) * 6;
  M5.Lcd.setCursor((160 - nameW) / 2, 8);
  M5.Lcd.print(devName);
  M5.Lcd.setTextColor(DIM, BG);
  M5.Lcd.setCursor(16, 24);
  M5.Lcd.print("Advertising...");
  M5.Lcd.setCursor(4, 44);
  M5.Lcd.print("Claude Desktop:");
  M5.Lcd.setCursor(4, 55);
  M5.Lcd.print("Help>Troubleshoot");
  M5.Lcd.setCursor(4, 66);
  M5.Lcd.print("Dev mode, then");
  M5.Lcd.setCursor(4, 77);  // extra line won't show but ok
}

static void drawPasskey() {
  M5.Lcd.fillScreen(BG);
  M5.Lcd.setTextColor(C_PAIR, BG);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setCursor(34, 6);
  M5.Lcd.print("BLE Pairing");
  // Large 6-digit passkey
  char pk[8];
  snprintf(pk, sizeof(pk), "%06lu", bleKey);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setTextColor(FG, BG);
  int pw = strlen(pk) * 12;
  M5.Lcd.setCursor((160 - pw) / 2, 26);
  M5.Lcd.print(pk);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(DIM, BG);
  M5.Lcd.setCursor(14, 58);
  M5.Lcd.print("Enter on desktop");
}

static void drawConnecting() {
  M5.Lcd.fillScreen(BG);
  M5.Lcd.setTextColor(C_PAIR, BG);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setCursor(28, 36);
  M5.Lcd.print("Connecting...");
}

static void drawBuddy() {
  M5.Lcd.fillScreen(BG);
  M5.Lcd.setTextSize(1);
  uint16_t col = STATE_COLOR[gState];
  uint8_t  f   = animFrame & 1;

  // Left panel: pet art
  M5.Lcd.setTextColor(col, BG);
  for (int i = 0; i < 3; i++) {
    M5.Lcd.setCursor(1, 4 + i * 12);
    M5.Lcd.print(ART[gState][f][i]);
  }
  M5.Lcd.setCursor(4, 46);
  M5.Lcd.print(STATE_LABEL[gState]);

  M5.Lcd.drawFastVLine(79, 0, 80, DIM);

  // Right panel: info
  char tmp[13];
  M5.Lcd.setTextColor(col, BG);
  M5.Lcd.setCursor(82, 4);
  strncpy(tmp, g.msg, 12); tmp[12] = 0;
  M5.Lcd.print(tmp);

  if (gState == S_ATTENTION && g.promptTool[0]) {
    M5.Lcd.setTextColor(C_ATTN, BG);
    M5.Lcd.setCursor(82, 18);
    strncpy(tmp, g.promptTool, 12); tmp[12] = 0;
    M5.Lcd.print(tmp);
    M5.Lcd.setTextColor(DIM, BG);
    M5.Lcd.setCursor(82, 30);
    strncpy(tmp, g.promptHint, 12); tmp[12] = 0;
    M5.Lcd.print(tmp);
  } else {
    M5.Lcd.setTextColor(DIM, BG);
    for (int i = 0; i < g.nLines && i < 3; i++) {
      M5.Lcd.setCursor(82, 18 + i * 10);
      strncpy(tmp, g.lines[i], 12); tmp[12] = 0;
      M5.Lcd.print(tmp);
    }
  }

  // Show raw r/w values from Claude Desktop for diagnostics
  M5.Lcd.setTextColor(DIM, BG);
  M5.Lcd.setCursor(82, 56);
  char tok[13];
  snprintf(tok, 13, "r=%u w=%u", g.running, g.waiting);
  M5.Lcd.print(tok);

  M5.Lcd.setCursor(82, 68);
  if (gState == S_ATTENTION) {
    M5.Lcd.setTextColor(C_BUSY, BG); M5.Lcd.print("A=ok ");
    M5.Lcd.setTextColor(C_ATTN, BG); M5.Lcd.print("B=no");
  } else {
    M5.Lcd.setTextColor(DIM, BG);
    M5.Lcd.print("A=ok B=no");
  }
}

// ── State machine ──────────────────────────────────────────────────────────
static void recompute() {
  if (!g.connected || (millis() - g.lastBeat) > 15000) gState = S_SLEEP;
  else if (g.waiting > 0) gState = S_ATTENTION;
  else if (g.running > 0) gState = S_BUSY;
  else                     gState = S_IDLE;
}

// ── Protocol ───────────────────────────────────────────────────────────────
static void parseLine(const char* line) {
  JsonDocument doc;
  if (deserializeJson(doc, line) != DeserializationError::Ok) return;

  g.connected = true;
  g.lastBeat  = millis();

  // Turn events — Claude Desktop sends these on every message exchange.
  // Use them to drive the busy animation when heartbeat running field stays 0.
  const char* evt = doc["evt"];
  if (evt && strcmp(evt, "turn") == 0) {
    const char* role = doc["role"];
    if (role && strcmp(role, "user") == 0) {
      // User just sent a message — Claude is about to generate.
      strncpy(g.msg, "thinking...", 47);
      turnBusy = millis() + 30000;  // hold busy up to 30s
      g.running = 1;
    } else if (role && strcmp(role, "assistant") == 0) {
      // Claude finished responding.
      strncpy(g.msg, "done", 47);
      turnBusy = 0;
      g.running = 0;
    }
    recompute();
    return;
  }

  if (!doc["running"].isNull()) g.running     = doc["running"].as<uint8_t>();
  if (!doc["waiting"].isNull()) g.waiting     = doc["waiting"].as<uint8_t>();
  if (!doc["tokens_today"].isNull()) g.tokensToday = doc["tokens_today"].as<uint32_t>();

  const char* m = doc["msg"];
  if (m) { strncpy(g.msg, m, 47); g.msg[47] = 0; }

  JsonArray arr = doc["entries"].as<JsonArray>();
  if (!arr.isNull()) {
    g.nLines = 0;
    for (JsonVariant v : arr) {
      if (g.nLines >= 3) break;
      strncpy(g.lines[g.nLines], v.as<const char*>(), 51);
      g.lines[g.nLines++][51] = 0;
    }
  }

  JsonObject p = doc["prompt"].as<JsonObject>();
  if (!p.isNull()) {
    strncpy(g.promptId,   p["id"]   | "", 39);
    strncpy(g.promptTool, p["tool"] | "", 19);
    strncpy(g.promptHint, p["hint"] | "", 43);
    g.waiting = 1;
  } else {
    g.promptId[0] = g.promptTool[0] = g.promptHint[0] = 0;
  }

  // Command responses
  const char* cmd = doc["cmd"];
  if (cmd) {
    if (strcmp(cmd, "status") == 0) {
      char resp[128];
      snprintf(resp, sizeof(resp),
        "{\"ack\":\"status\",\"ok\":true,\"data\":{\"name\":\"%s\",\"sec\":%s}}",
        devName, bleSecure ? "true" : "false");
      bleSend(resp);
    } else if (strcmp(cmd, "name") == 0) {
      bleSend("{\"ack\":\"name\",\"ok\":true}");
    } else if (strcmp(cmd, "owner") == 0) {
      bleSend("{\"ack\":\"owner\",\"ok\":true}");
    } else if (strcmp(cmd, "unpair") == 0) {
      bleSend("{\"ack\":\"unpair\",\"ok\":true}");
      // Bond cleared on next restart; disconnect now so desktop can re-pair
    }
  }

  recompute();
}

static void sendPermission(const char* id, const char* decision) {
  char buf[128];
  snprintf(buf, sizeof(buf),
    "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"%s\"}", id, decision);
  bleSend(buf);
}

// ── BLE init ───────────────────────────────────────────────────────────────
static void initBLE() {
  BLEDevice::init(devName);
  BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT);
  BLEDevice::setSecurityCallbacks(new SecCB());

  BLESecurity* pSec = new BLESecurity();
  pSec->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);
  pSec->setCapability(ESP_IO_CAP_OUT);
  pSec->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  pSec->setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  pSec->setKeySize(16);

  BLEServer*  pSrv = BLEDevice::createServer();
  pSrv->setCallbacks(new SrvCB());

  BLEService* pSvc = pSrv->createService(BLEUUID(NUS_SVC));

  // TX: device → desktop (no char-level permission — connection-level security suffices)
  pTxChar = pSvc->createCharacteristic(
    BLEUUID(NUS_TX),
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );
  pTxChar->addDescriptor(new BLE2902());

  // RX: desktop → device (no char-level permission)
  BLECharacteristic* pRx = pSvc->createCharacteristic(
    BLEUUID(NUS_RX),
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
  );
  pRx->setCallbacks(new RxCB());

  pSvc->start();

  BLEAdvertising* pAdv = BLEDevice::getAdvertising();
  pAdv->addServiceUUID(BLEUUID(NUS_SVC));
  pAdv->setScanResponse(true);
  pAdv->setMinPreferred(0x06);
  pAdv->setMaxPreferred(0x12);
  BLEDevice::startAdvertising();
}

// ── Arduino ────────────────────────────────────────────────────────────────
void setup() {
  M5.begin();
  M5.Lcd.setRotation(1);  // landscape 160x80, USB on right
  M5.Lcd.fillScreen(BG);

  pinMode(10, OUTPUT);
  digitalWrite(10, HIGH);  // LED off (active LOW)

  // Boot splash
  M5.Lcd.setTextColor(FG, BG);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setCursor(34, 36);
  M5.Lcd.print("Claude Buddy");

  // Derive name from BT MAC (last 2 bytes)
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_BT);
  snprintf(devName, sizeof(devName), "Claude-%02X%02X", mac[4], mac[5]);

  initBLE();
  delay(800);
  drawAdvertising();
}

void loop() {
  M5.update();
  uint32_t now = millis();

  // ── Drain BLE ring buffer into line parser ─────────────────────────────
  while (ringAvail() > 0) {
    char c = ringRead();
    if (c == '\n' || c == '\r') {
      if (lineBufLen > 0) {
        lineBuf[lineBufLen] = 0;
        parseLine(lineBuf);
        lineBufLen = 0;
      }
    } else if (lineBufLen < 598) {
      lineBuf[lineBufLen++] = c;
    }
  }

  // ── Auto-advance to secure if auth callback didn't fire (already bonded) ─
  if (bleConn && !bleSecure && !showKey && (now - connectedAt) > 3000)
    bleSecure = true;

  // ── Turn-event busy timer expiry ──────────────────────────────────────
  if (turnBusy > 0 && now > turnBusy) {
    turnBusy = 0;
    g.running = 0;
    strncpy(g.msg, "idle", 47);
    recompute();
  }

  // ── Heartbeat timeout → sleep ──────────────────────────────────────────
  if (g.connected && (now - g.lastBeat) > 15000) {
    g.connected = false;
    recompute();
  }

  // ── LED blink during ATTENTION ─────────────────────────────────────────
  if (gState == S_ATTENTION) {
    if (now - lastLed > 500) {
      lastLed = now; ledOn = !ledOn;
      digitalWrite(10, ledOn ? LOW : HIGH);
    }
  } else {
    digitalWrite(10, HIGH); ledOn = false;
  }

  // ── Refresh display ────────────────────────────────────────────────────
  uint32_t interval;
  if (!bleConn)          interval = 1500;  // slow pulse while advertising
  else if (showKey)      interval = 50;    // keep passkey fresh
  else if (!bleSecure)   interval = 500;
  else                   interval = (gState == S_SLEEP) ? 1000 : 400;

  if (now - lastDraw > interval) {
    lastDraw = now;
    if (!bleConn) {
      animFrame++;
      drawAdvertising();
    } else if (showKey) {
      drawPasskey();
    } else if (!bleSecure) {
      drawConnecting();
    } else {
      animFrame++;
      drawBuddy();
    }
  }

  // ── Buttons ────────────────────────────────────────────────────────────
  if (M5.BtnA.wasPressed()) {
    if (bleSecure && gState == S_ATTENTION && g.promptId[0])
      sendPermission(g.promptId, "once");
    g.waiting = 0; g.promptId[0] = 0;
    recompute();
    if (bleSecure) drawBuddy();
  }

  if (M5.BtnB.wasPressed()) {
    if (bleSecure && gState == S_ATTENTION && g.promptId[0])
      sendPermission(g.promptId, "deny");
    g.waiting = 0; g.promptId[0] = 0;
    recompute();
    if (bleSecure) drawBuddy();
  }
}
