# Claude Buddy — Handoff

## What this is
A physical Claude Desktop companion device. An ESP32 microcontroller with a display pairs over BLE to Claude Desktop and shows live session state with animations, and lets the user approve/deny tool permission prompts by touching the screen.

Based on: https://github.com/anthropics/claude-desktop-buddy

---

## Hardware currently in use
- **Primary: CYD** (Cheap Yellow Display / ESP32-2432S028) — 2.8" ILI9341 240×320 + XPT2046 resistive touch, RGB LEDs
- **Secondary: M5StickC original** — 80×160 LCD, BLE, two physical buttons

Both flashed with BLE firmware, pair with Claude Desktop on macOS/Windows.

---

## Current state

### What's working
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

1. **Push to GitHub** — user is creating a repo; push current state
2. **Verify permission flow end-to-end** — confirm a tool approval from the CYD actually unblocks Claude Desktop
3. **Protocol investigation** — use the LOG tab to photograph what Claude Desktop sends during active use, identify if `running`/`waiting` are used for regular chat
4. **Auto-start on USB** (Linux) — udev rule to start `bridge.py` when the M5StickC is plugged in
5. **GIF character support** (optional) — the upstream repo's folder-push protocol for streaming custom animated characters over BLE
