# Claude Buddy — Handoff

## What this is
A physical Claude Desktop companion device. An ESP32 microcontroller with a display pairs over BLE to Claude Desktop and shows live session state with animations, and lets the user approve/deny tool permission prompts by touching the screen.

Based on: https://github.com/anthropics/claude-desktop-buddy

---

## Hardware currently in use
- **Primary: CYD** (Cheap Yellow Display / ESP32-2432S028) — 2.8" ILI9341 240×320 + XPT2046 resistive touch, RGB LEDs
- **Secondary: M5StickC original** — 80×160 LCD, BLE, two physical buttons
- **New board (unresolved):** A second CYD/BYD variant, also labelled 240×320, 2.8". Physically different panel mounting — needs different rotation + layout redesign. See "New Board Issue" below.

---

## Current state

### What's working (original CYD board)
- BLE pairing with Claude Desktop (Nordic UART Service, `Claude-XXYY` advertising name)
- CYD: dark sci-fi HUD — animated ASCII cat with pulsing glow rings, state-reactive neon accent color (cyan/green/amber/gray), corner-bracket panels, glowing separators, fading activity feed, 3-tab navigation (swipe or tap)
- M5StickC: landscape buddy with cat animation and button A/B approve/deny
- State machine: SLEEPING → IDLE → WORKING → APPROVE?
- `evt:turn` handling shows WORKING during user→assistant message exchange (bridges gap where `running` isn't set for plain chat)
- Permission prompt: correctly persists until explicitly cleared, not wiped by unrelated heartbeats
- Touch left/right for Don't Allow/Allow (correct mapping, tested)
- Permission sends logged to BLE LOG tab for verification
- LED: blinks red in APPROVE? state, pulses green while connected
- Approval/denial tally tracked and displayed
- Clock displayed from Claude Desktop time-sync
- LOG tab: scrollable live BLE data stream, shows exactly what Claude Desktop sends

### Flicker — solved
Three-tier render: full redraw only on state/tab change → `updateBuddyText()` for data-only changes → sprite-only push for animation. DMA + 40 MHz SPI + `startWrite`/`endWrite` batching. Full redraws use gap fills instead of `fillScreen`.

### Open question
Claude Desktop only sets `running > 0` for tool-assisted sessions, not plain chat. The LOG tab on the CYD shows every raw BLE line — a photo of it during active use will reveal exactly what the user's Claude Desktop version sends. Once known, `parseLine()` can be updated if needed.

---

## New Board Issue (unresolved)

A second CYD/BYD variant (also 2.8", also labelled 240×320) has its display panel physically mounted at 90° relative to the original CYD. The current firmware (`setRotation(0)`) shows portrait correctly on the original board but landscape on the new board.

**Rotation test results on new board:**
| setRotation | Visual result |
|-------------|--------------|
| 0 | Landscape (sideways) |
| 1 | Portrait upside-down, bottom 25% of screen unused |
| 2 | Landscape (other direction) |
| 3 | Portrait right-side-up orientation, but bottom ~25% of UI content clipped |

**Root cause:** Rotations 1 and 3 swap the logical coordinate space to 320w×240h (landscape logical). The layout is hardcoded for 240w×320h (portrait logical). Content drawn at y > 239 is clipped, and x > 239 leaves 80px unused. So neither rotation 1 nor 3 fills the screen correctly with the current layout.

**Fix needed:** For the new board, use `setRotation(3)` + redesign the entire layout using 320 logical width × 240 logical height. Every hardcoded pixel coordinate in `drawBuddy()`, `drawStats()`, `drawLog()`, `drawAttn()`, `updateBuddyText()`, `pushCatSprite()`, sprite dimensions, etc. must be updated. Consider a compile-time `#define NEW_BOARD` flag to keep both layouts.

**Current firmware state:** `setRotation(0)` — correct for original CYD, wrong for new board.

---

## To flash original CYD board
Firmware is already set to `setRotation(0)` — just flash:
```bash
export PATH="$HOME/.platformio/penv/bin:$PATH"
sudo chmod 666 /dev/ttyUSB0
cd firmware-cyd
pio run --target upload --upload-port /dev/ttyUSB0
```

## To flash new board variant (once layout redesign is done)
Change both `setRotation(0)` → `setRotation(3)` calls in `setup()` (tft and ts), then redesign layout for 320w×240h before flashing. Not yet implemented.

---

## Architecture

```
Claude Desktop (macOS/Windows)
       │  BLE Nordic UART Service (NUS)
       │  RX (6e400002): heartbeat JSON from Desktop → device
       │  TX (6e400003): permission decisions from device → Desktop
       ▼
  CYD firmware (firmware-cyd/src/main.cpp)
       • parseLine()  — JSON protocol handler
       • recompute()  — state machine (SLEEP/IDLE/BUSY/ATTN)
       • pushCatSprite() — 240×148 TFT_eSprite, atomic DMA push
       • dirty / textDirty flags — control render tier
       • swipe + tap touch handling
```

For **Linux + Claude Code** (no Claude Desktop):
```
Claude Code CLI
  → hooks (buddy_*.py) write /tmp/claude_buddy_state.json
  → bridge.py reads file, sends heartbeats over /dev/ttyUSB0
  → M5StickC USB serial firmware (same JSON protocol, no BLE)
```

---

## Key files

| Path | Purpose |
|------|---------|
| `firmware-cyd/src/main.cpp` | CYD BLE firmware — all display, touch, BLE, protocol |
| `firmware-cyd/platformio.ini` | esp32dev, TFT_eSPI, XPT2046, ArduinoJson, 40MHz DMA |
| `firmware/src/main.cpp` | M5StickC BLE firmware |
| `firmware/platformio.ini` | m5stick-c board |
| `bridge.py` | USB serial bridge for Linux/Claude Code mode |
| `start.sh` / `stop.sh` | Bridge daemon management |
| `hooks/buddy_pre_tool.py` | PreToolUse — writes tool info to state file |
| `hooks/buddy_post_tool.py` | PostToolUse — decrements running count |
| `hooks/buddy_stop.py` | Stop — resets to idle |
| `README.md` | Full setup, protocol, and troubleshooting docs |

---

## Key implementation notes

**BLE security**: `ESP_LE_AUTH_REQ_SC_MITM_BOND` + `ESP_IO_CAP_OUT` (DisplayOnly). Auth callback sets `bleSec=true`; 3-second fallback timer handles already-bonded reconnects where the callback may not fire.

**Characteristic permissions**: NOT using `ESP_GATT_PERM_WRITE_ENCRYPTED` on the RX characteristic — this was found to block Claude Desktop writes in testing.

**Prompt clearing**: only clears `g.pId/pTool/pHint` when a heartbeat explicitly sends `waiting: 0`, not when arbitrary messages arrive without a `prompt` field.

**Touch**: `startState` captured at touch-down to avoid race where state changes mid-touch and the tap handler sees the wrong state.

**Sprite**: 240×148 px, covers the entire hero zone y=19-166. `catSpr.fillSprite(BG)` + glow rings + cat text + `catSpr.pushSprite()` — the push is one atomic SPI transaction via DMA.

**Render tiers**:
1. `dirty=true` (state or tab changed) → full redraw with `startWrite`/`endWrite`
2. `textDirty=true` (same state, data changed) → `updateBuddyText()` + `pushCatSprite()`
3. Animation tick (700ms) → `pushCatSprite()` + clock update only

**PlatformIO**: `~/.platformio/penv/bin/pio`. Always `sudo chmod 666 /dev/ttyUSB0` before flashing on this system (or `./stop.sh` if bridge is running first).

---

## Next steps (in priority order)

1. **New board layout redesign** — fix coordinate mismatch for new CYD/BYD variant: use `setRotation(3)` + redesign all draw coordinates for 320w×240h logical space (see "New Board Issue" above)
2. **Verify permission flow end-to-end** — confirm a tool approval from the CYD actually unblocks Claude Desktop
3. **Protocol investigation** — use the LOG tab to photograph what Claude Desktop sends during active use
4. **Push updated firmware to GitHub** — current firmware-cyd/src/main.cpp not yet pushed to colery/homeai-cb
5. **Auto-start on USB** (Linux) — udev rule to start `bridge.py` when M5StickC is plugged in
6. **GIF character support** (optional) — upstream repo's folder-push protocol for streaming custom animated characters over BLE
