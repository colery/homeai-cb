#pragma once
// Claude Buddy GUI. Pure LVGL: no hardware access, so the same file renders on the
// ESP32 and in the host preview (preview/), which draws it to PNGs.
#include <stdint.h>
#include <stdbool.h>

enum UiState { UI_SLEEP = 0, UI_IDLE, UI_BUSY, UI_ATTN };

#define UI_ENTRIES   5
#define UI_LOG_LINES 50
#define UI_SPARK     30

struct UiLimits {
  bool     has5, has7;
  uint8_t  pct5, pct7;
  int32_t  rst5, rst7;                // seconds until the window resets, -1 unknown
};

struct UiModel {
  UiState  state;
  char     devName[20];
  char     time[8];
  bool     usbLive, bleConn, bleSec, showKey;
  uint32_t bleKey;
  // per source: [0] = USB (Claude Code via the hub), [1] = BLE (Claude Desktop)
  uint8_t  run[2], wait[2];
  uint32_t tok[2], tokLife;
  char     msg[64];                    // most recent status line from either source (hero title)
  // per source, same indexing as run/wait/tok: [0] = Claude Code (USB), [1] = Claude Desktop (BLE)
  char     msgS[2][64];
  char     entriesS[2][UI_ENTRIES][52];
  uint8_t  nEntriesS[2];
  uint8_t  pSrc;                       // source of the pending permission prompt
  // plan usage from Claude Code's status line (via the hub): [0] = USB-side host(s), [1] = the other machine(s)
  UiLimits lim[2];
  int8_t   ctx;                        // context window fill %, -1 unknown
  bool     hasPrompt, pInfo;
  char     pTool[20], pHint[44], pId[40];
  uint16_t approvals, denials;
  uint32_t uptimeS;
  int32_t  beatAgeS;                 // -1 = never
  const char* logLines[UI_LOG_LINES];  // newest first
  uint8_t  logCount;
  uint32_t logSeq;                   // bump when the log changes
  uint8_t  spark[UI_SPARK];          // tool calls per minute, oldest first (from the hub)
  uint8_t  nSpark;
};

typedef void (*UiPermCb)(bool allow);
typedef void (*UiDismissCb)(void);

void ui_init(UiPermCb onPermission, UiDismissCb onDismiss);
void ui_update(const UiModel& m);
void ui_set_tab(int tab);            // 0 home, 1 stats, 2 log
