# Claude Buddy

A physical companion device that pairs with **Claude Desktop** (macOS/Windows) over BLE and shows live session state — what Claude is doing, token usage, permission prompts — on a display. Touch input lets you approve or deny tool calls directly from the device.

Based on the [claude-desktop-buddy](https://github.com/anthropics/claude-desktop-buddy) open hardware spec from Anthropic.

---

## Hardware

| Board | Display | Driver | PlatformIO env | Status |
|-------|---------|--------|----------------|--------|
| **CYD (ESP32-2432S028)** | 2.8" 240×320 + XPT2046 | ILI9341 | `cyd` | Primary — working |
| **TPM408-2.8** (Amazon A2150 / X004QKM5EP) | 2.8" 240×320 + XPT2046 | **ILI9342** | `cyd_new` | Working — requires different driver |
| M5StickC (original) | 80×160 LCD | — | — | Secondary — BLE firmware in `firmware/` |

> **Board identification:** The TPM408-2.8 has "TPM408-2.8" printed on the PCB near the USB port. It looks identical to the original CYD but requires the ILI9342 driver — using ILI9341 only addresses 75% of the panel.

---

## CYD — what it looks like

**Dark sci-fi HUD** aesthetic: deep navy background, state-reactive neon accents, pulsing glow rings around the animated cat, corner-bracket panel decorations, glowing separator lines.

**State colors** — the accent changes throughout the entire UI:

| State | Color | Meaning |
|-------|-------|---------|
| SLEEPING | Gray | No heartbeat / disconnected |
| IDLE | Cyan | Connected, Claude not running |
| WORKING | Green | Claude is generating / running tools |
| APPROVE? | Amber | Tool needs your permission — LED blinks |

**Screen layout:**
```
┌──────────────────────────────────────┐
│ Claude-1B4C        12:34         [●] │  top bar
├ ─ ─ ─ ─ ─ ─ glow line ─ ─ ─ ─ ─ ─ ┤
│ ┌·                             ·┐   │
│ │    ◎ ◎ ◎  glow rings  ◎ ◎ ◎  │   │
│ │          /\_/\                │   │  hero sprite
│ │         ( o.o )               │   │  (240×148 px)
│ │          >   <                │   │
│ │            IDLE               │   │
│ └·             ok:3 no:1       ·┘   │
├ ─ ─ ─ ─ ─ ─ glow line ─ ─ ─ ─ ─ ─ ┤
│  r:0  w:0  ┃  1.2K tok  ┃  IDLE    │  stat row
├ ─ ─ ─ ─ ─ ─ glow line ─ ─ ─ ─ ─ ─ ┤
│ ► 12:31 Bash: git status             │
│ ► 12:30 Edit: main.cpp               │  activity feed
│ ► 12:29 Bash: npm test               │  (fades oldest→dimmest)
│ ► 12:28 Read: config.py              │
│ ► 12:27 Bash: ls -la                 │
├ ─ ─ ─ ─ ─ ─ glow line ─ ─ ─ ─ ─ ─ ┤
│ [●LIVE 3s ago]   [████░  72%]        │  beat + approval rate
├ ─ ─ ─ ─ ─ ─ glow line ─ ─ ─ ─ ─ ─ ┤
│    BUDDY    ┃    STATS    ┃    LOG   │  tab bar
└──────────────────────────────────────┘
```

**Three tabs** (swipe left/right or tap tab bar):
- **BUDDY** — animated cat + live stats
- **STATS** — sessions, token usage bar, decision history, system info
- **LOG** — terminal-style live BLE data stream (shows every raw message Claude Desktop sends)

**Permission dialog** (APPROVE? state):
- Full-screen alert — left half = Don't Allow, right half = Allow
- Corner brackets + pulsing dots
- Tool name, command hint, request ID all visible

---

## Prerequisites

### Claude Desktop setup
1. Install [Claude Desktop](https://claude.ai/download) on macOS or Windows
2. Enable Developer Mode: **Help → Troubleshooting → Enable Developer Mode**
3. Open: **Developer → Open Hardware Buddy…**

### Build tools (only needed to reflash)
```bash
# Install PlatformIO
curl -fsSL https://raw.githubusercontent.com/platformio/platformio-core-installer/master/get-platformio.py | python3
export PATH="$HOME/.platformio/penv/bin:$PATH"
```

### Linux serial port access
```bash
sudo usermod -aG dialout $USER   # then log out/in, or:
sudo chmod 666 /dev/ttyUSB0      # one-shot for the current session
```

---

## CYD — Flash

Two boards share the same source. Use the correct environment for your board.

**Board A — Original CYD (ESP32-2432S028, ILI9341)**
```bash
sudo chmod 666 /dev/ttyUSB0
cd firmware-cyd
pio run -e cyd --target upload --upload-port /dev/ttyUSB0
```

**Board B — TPM408-2.8 / A2150 (ILI9342)**
```bash
sudo chmod 666 /dev/ttyUSB0
cd firmware-cyd
pio run -e cyd_new --target upload --upload-port /dev/ttyUSB0
```

The `cyd_new` environment injects `-DNEW_BOARD`, switches to `ILI9342_DRIVER`, and sets `TFT_WIDTH=320, TFT_HEIGHT=240` (landscape-native dimensions for the ILI9342). Rotation 3 then produces the correct portrait right-side-up orientation with full 240×320 coverage.

### CYD pinout

| Function | GPIO | Notes |
|----------|------|-------|
| Display MOSI | 13 | VSPI |
| Display SCLK | 14 | |
| Display CS | 15 | |
| Display DC | 2 | |
| Display MISO | 12 | |
| Backlight | 21 | HIGH = on |
| Touch CLK | 25 | HSPI |
| Touch MOSI | 32 | |
| Touch MISO | 39 | |
| Touch CS | 33 | |
| Touch IRQ | 36 | |
| LED Red | 4 | active LOW, blinks in APPROVE? |
| LED Green | 16 | active LOW, pulses when connected |
| LED Blue | 17 | active LOW, unused |

### Pairing with Claude Desktop
1. Power the CYD — it shows the advertising screen with your device name (e.g. `Claude-1B4C`)
2. In Claude Desktop → **Developer → Open Hardware Buddy…**
3. Select your device
4. If prompted for a passkey, enter the 6-digit code shown on the CYD screen
5. Connection is remembered — auto-reconnects on next boot

### Touch calibration
If taps register in the wrong location, edit the top of `firmware-cyd/src/main.cpp`:
```cpp
#define TCH_SWAP_XY false   // true if X/Y axes are transposed
#define TCH_FLIP_X  false   // true if left/right is mirrored
#define TCH_FLIP_Y  false   // true if top/bottom is mirrored
```

---

## M5StickC (original) — Flash

```bash
cd firmware
pio run --target upload --upload-port /dev/ttyUSB0
```

Same pairing process. Device advertises as `Claude-XXYY`.

**Button A** (front face): Approve  
**Button B** (right side): Deny  
**LED** (GPIO 10, active LOW): blinks red in APPROVE? state

---

## USB Serial mode (Linux / Claude Code)

For **Claude Code** (the CLI) instead of Claude Desktop. The CYD and M5StickC firmware both accept the same JSON lines over USB serial that they accept over BLE; the CYD uses whichever link is live (shown as `USB`/`BLE` in the top bar).

```
Claude Code (any machine) --hooks--> HTTP POST :8765/event --> [hub: bridge.py] --USB serial--> buddy
```

The **hub** (`bridge.py`) runs on the machine the buddy is plugged into. Hooks on any machine on the network post small events to it, so several machines and several concurrent sessions all feed one display.

### Hub
```bash
python3 bridge.py                        # auto-detects the serial port, HTTP on :8765
python3 bridge.py --port /dev/ttyUSB0 --http-port 8765
curl localhost:8765/state                # sessions, device link, merged heartbeat
```
Needs `pyserial`. As a service: `deploy/claude-buddy.service` (runs as a user in the `dialout` group, restarts on failure, reconnects if the device is unplugged). The port is opened without asserting DTR/RTS so connecting does not reset the board.

### Status model
State is tracked per session and merged, and **everything in progress expires**, so an interrupted turn cannot leave the display stuck on WORKING:

| Signal | Source | Effect |
|---|---|---|
| `UserPromptSubmit` | hook | session is thinking (WORKING) |
| `PreToolUse` / `PostToolUse` | hook | tool shown in the activity feed |
| `Notification` (permission) | hook | APPROVE? screen: tool + command, "answer in terminal"; tap dismisses |
| `Notification` (idle) / `Stop` | hook | back to IDLE (the idle notification also recovers from an Esc interrupt) |
| no events | expiry | thinking 120 s, tool call 10 min, permission prompt 10 min, session 1 h |

`r:` is the number of sessions currently working, `w:` the number waiting on a permission prompt. Tokens-today is counted from each session's transcript at Stop (input + output + cache writes).

### Claude Code hook
One script handles every event and always exits 0 silently (a down hub costs one short timeout, then it backs off for 20 s). In `~/.claude/settings.json`:
```json
"hooks": {
  "SessionStart":     [{"hooks":[{"type":"command","command":"python3 /path/to/hooks/buddy_hook.py"}]}],
  "UserPromptSubmit": [{"hooks":[{"type":"command","command":"python3 /path/to/hooks/buddy_hook.py"}]}],
  "PreToolUse":       [{"matcher":"*","hooks":[{"type":"command","command":"python3 /path/to/hooks/buddy_hook.py"}]}],
  "PostToolUse":      [{"matcher":"*","hooks":[{"type":"command","command":"python3 /path/to/hooks/buddy_hook.py"}]}],
  "Notification":     [{"hooks":[{"type":"command","command":"python3 /path/to/hooks/buddy_hook.py"}]}],
  "Stop":             [{"hooks":[{"type":"command","command":"python3 /path/to/hooks/buddy_hook.py"}]}],
  "SessionEnd":       [{"hooks":[{"type":"command","command":"python3 /path/to/hooks/buddy_hook.py"}]}]
}
```
Set `CLAUDE_BUDDY_URL` (default `http://100.64.149.28:8765/event`) to point at a different hub.

### Flashing from the hub machine
Build with PlatformIO, copy `bootloader/partitions/firmware.bin` (+ `boot_app0.bin`) to the hub, and run `deploy/flash.sh cyd|cyd_new`; it stops the hub, flashes with `esptool`, and restarts it.

---

## Wire protocol

The firmware speaks the [claude-desktop-buddy BLE wire protocol](https://github.com/anthropics/claude-desktop-buddy/blob/main/REFERENCE.md) over Nordic UART Service (NUS):

- **Service UUID**: `6e400001-b5a3-f393-e0a9-e50e24dcca9e`
- **RX** (host → device): `6e400002-…` — Claude Desktop writes heartbeat JSON
- **TX** (device → host): `6e400003-…` — device notifies permission responses

One JSON object per `\n`-terminated line.

**Heartbeat** (Claude Desktop → device):
```json
{"total":1,"running":1,"waiting":0,"msg":"Running: Bash",
 "entries":["14:31 Bash"],"tokens":184502,"tokens_today":31200}
```

**Permission prompt** (embedded in heartbeat):
```json
{"prompt":{"id":"req_abc","tool":"Bash","hint":"rm -rf /tmp/foo"}}
```

**Permission response** (device → Claude Desktop):
```json
{"cmd":"permission","id":"req_abc","decision":"once"}
{"cmd":"permission","id":"req_abc","decision":"deny"}
```

---

## Display refresh architecture (no flicker)

Three-tier render strategy:

| Trigger | What runs | Cost |
|---------|-----------|------|
| State/tab change | Full redraw (`drawBuddy()` etc.) | Heavy, infrequent |
| Heartbeat data change | `updateBuddyText()` — targeted small `fillRect` calls | Light |
| Animation tick (700ms) | `pushCatSprite()` — 240×148 sprite DMA push | Zero-flicker |

Additional optimizations:
- **DMA enabled** (`tft.initDMA(true)`) — SPI transfers run in hardware while CPU prepares next frame
- **40 MHz SPI** — proven stable ceiling for ILI9341 over ESP32 GPIO matrix (55 MHz caused instability)
- **`startWrite()`/`endWrite()` batching** — CS stays pinned low for the entire render, eliminating per-call toggling overhead
- **`fillScreen()` never called** during data updates — `drawBuddy()` fills only the six small desktop gaps between windows on full redraws

---

## File structure

```
claude-buddy/
├── README.md
├── HANDOFF.md
│
├── firmware-cyd/              ← CYD boards (primary)
│   ├── platformio.ini         ← [env:cyd] ILI9341 + [env:cyd_new] ILI9342
│   └── src/main.cpp           ← sci-fi HUD UI, BLE, sprite animation (#ifdef NEW_BOARD for TPM408)
│
├── firmware/                  ← M5StickC original (BLE)
│   ├── platformio.ini
│   └── src/main.cpp
│
├── hooks/
│   └── buddy_hook.py          ← Claude Code hook (all events -> hub)
│
├── bridge.py                  ← hub: HTTP in from hooks, USB serial out to the buddy
├── deploy/                    ← claude-buddy.service, flash.sh
```

---

## Troubleshooting

**State stays IDLE during plain chat**  
Claude Desktop only sets `running > 0` during tool-assisted sessions (web search, file access, computer use). For regular chat, the device detects `evt:turn` messages and briefly shows WORKING. Check the LOG tab to see exactly what your Claude Desktop version sends.

**Device shows SLEEPING after connecting**  
Wait 10 seconds for the first heartbeat. If it persists, check Developer Mode is enabled in Claude Desktop.

**Permission dialog taps are swapped**  
The XPT2046 raw X axis may be inverted. Try `#define TCH_FLIP_X true` at the top of `firmware-cyd/src/main.cpp`, reflash, and test again.

**Port permission denied on Linux after reboot**  
`sudo chmod 666 /dev/ttyUSB0`, or log out and back in (you were added to `dialout`).

**Reflashing when bridge is running**  
Stop the hub before flashing (`deploy/flash.sh` does this) — it holds the serial port open.
