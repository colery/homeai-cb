# Claude Buddy

A small touchscreen companion that shows what your Claude sessions are doing: which are working or waiting, what they ran last, tokens used, and your plan usage. It listens on **two links at once** and keeps them separate:

| Link | Source | What it shows |
|---|---|---|
| **USB serial** | **Claude Code** on any machine, through a small hub service | state, current tool, activity, tokens, plan usage (5-hour / 7-day) |
| **Bluetooth LE** | **Claude Desktop** (macOS/Windows) | state, activity, tokens, permission prompts |

Based on Anthropic's [claude-desktop-buddy](https://github.com/anthropics/claude-desktop-buddy) BLE spec; the USB side is this project's own.

---

## What's in the repo

```
firmware-cyd-gui/   LVGL GUI firmware for the CYD  (current, recommended)
firmware-cyd/       earlier text/HUD firmware for the CYD (fallback)
firmware/           M5StickC firmware (legacy, BLE only, not used with the hub)
bridge.py           the hub: HTTP in from hooks, USB serial out to the board
hooks/buddy_hook.py Claude Code hook script (all events + status line -> hub)
deploy/             claude-buddy.service, flash.sh, flash-gui.sh, status line example
```

```
Claude Code (any machine) --hook--> HTTP POST :8765/event --> [hub: bridge.py] --USB serial--> CYD
Claude Desktop (Mac/PC) ---------------------- Bluetooth LE (Nordic UART) -----------------------> CYD
```

---

## The GUI (firmware-cyd-gui)

Dark card UI built on LVGL: rounded cards, anti-aliased text, icons, animation, touch swipe.

- **Status bar:** title, clock, and USB / Bluetooth icons that light when that link is live.
- **Buddy window (top):** a window with traffic-light dots, the current status line and context-window %. A vector cat sits in a ring:
  idle = blue, slow pulse, blinks and looks around; working = green spinning ring, squint, "thinking" dots;
  needs you = amber, `!` badge, wiggle; sleeping = gray, `z z`. Tap it and it bounces; its eyes follow your finger.
  Two game-style bars sit in its top corners: **`U`** (left) is the USB side's 5-hour plan usage, **`B`** (right) is Bluetooth's tokens today against its best day.
- **Two instance cards:** *Claude Code* (USB, teal) and *Claude Desktop* (Bluetooth, violet), each with its own state chip, current activity, latest event and token count. Both are visible at once.
- **Tabs** (swipe or tap): **Home**, **Stats** (plan usage with reset countdowns, BLE tokens, tokens today split USB/BLE, a 30-minute tool-calls-per-minute chart, sessions, allow/deny, system), **Log** (raw link traffic).
- **Permission prompts:** a modal that says which instance is asking. Claude Desktop prompts get Deny / Allow buttons; Claude Code prompts are display-only ("answer in your terminal", tap to dismiss).
- **Link screen:** shown until something connects; shows the Bluetooth pairing code whenever one is pending (even if USB is already linked).

The UI is one file (`firmware-cyd-gui/src/ui.cpp`) with no hardware access, so it also compiles on the PC:

```bash
cd firmware-cyd-gui/preview && make      # renders every screen to preview/out/*.png (sheet.png = contact sheet)
```

Use that to check layout without a board.

---

## Hardware

| Board | Display | Driver | PlatformIO env |
|---|---|---|---|
| **CYD (ESP32-2432S028)** | 2.8" 240×320 + XPT2046 touch | ILI9341 | `cyd` |
| **TPM408-2.8 / A2150** (looks identical) | same size, panel mounted 90° off | **ILI9342** | `cyd_new` |

The second board needs the ILI9342 driver (ILI9341 only addresses 75% of its panel); `cyd_new` handles that and rotates to the same 240×320 portrait. The GUI build for `cyd_new` compiles but has not been tried on hardware.

<details><summary>CYD pinout</summary>

| Function | GPIO | Function | GPIO |
|---|---|---|---|
| Display MOSI / SCLK / CS / DC / MISO | 13 / 14 / 15 / 2 / 12 | Touch CLK / MOSI / MISO | 25 / 32 / 39 |
| Backlight (HIGH = on) | 21 | Touch CS / IRQ | 33 / 36 |
| LED red / green / blue (active LOW) | 4 / 16 / 17 | | |
</details>

---

## Setup

### 1. The hub (on the machine the board is plugged into)

```bash
pip install pyserial
python3 bridge.py            # auto-detects the serial port, HTTP on :8765
curl localhost:8765/state    # sessions, device link, merged heartbeat
```

As a service: `deploy/claude-buddy.service` (runs as a user in `dialout`, restarts on failure, reconnects if the board is unplugged). Environment:

| Variable | Default | Meaning |
|---|---|---|
| `BUDDY_PORT` | auto-detect | serial port |
| `BUDDY_HTTP_PORT` / `BUDDY_BIND` | `8765` / `0.0.0.0` | where hooks post |
| `BUDDY_U_HOSTS` | `acer-ai` | hostnames whose plan usage feeds the `U` bar (others are tracked but the UI doesn't use them) |

Notes: the hub resends the clock when the board boots, saves plan-usage windows to `limits.json` so they survive restarts, and writes the board's Bluetooth events to its log (`journalctl -u claude-buddy | grep "board: ble"`). Opening the serial port (including restarting the hub) resets the board.

### 2. Claude Code hook (every machine you want to show up)

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

Set `CLAUDE_BUDDY_URL` (default `http://100.64.149.28:8765/event`) to point at your hub.

### 3. Plan usage (optional, Claude Pro/Max)

Claude Code passes `rate_limits` (5-hour and 7-day windows) and context fill to its **status line** command. Add one line that forwards that JSON to the same script (see `deploy/statusline-command.sh.example`):

```sh
input=$(cat)
printf '%s' "$input" | python3 /path/to/hooks/buddy_hook.py >/dev/null 2>&1 &
```

The 5-hour row is empty right after a window resets, until Claude Code's next reply.

### 4. Flash the board

```bash
cd firmware-cyd-gui
pio run -e cyd        # or -e cyd_new
```

It needs the `huge_app` partition table (set in `platformio.ini`); the default one is too small. To flash from the hub machine (which holds the serial port), copy `bootloader.bin`, `partitions.bin`, `firmware.bin` (from `.pio/build/<env>/`) and `boot_app0.bin` (from the PlatformIO ESP32 framework's `tools/partitions/`) into `fw-gui/<env>/` next to `deploy/flash-gui.sh`, then run `deploy/flash-gui.sh cyd|cyd_new`. It stops the hub, flashes with `esptool`, and restarts it. `deploy/flash.sh` does the same for the older `firmware-cyd` build.

### 5. Pair Claude Desktop

1. Claude Desktop → **Help → Troubleshooting → Enable Developer Mode**, then **Developer → Open Hardware Buddy…**
2. Click **Connect** and pick the board (`Claude-XXXX`).
3. Type the 6-digit code shown on the board.

Bonds are remembered; it reconnects on its own. The board talks to **one** Bluetooth device at a time.

---

## Wire protocol

Newline-delimited JSON, identical over USB serial and over BLE (Nordic UART: service `6e400001-…`, RX `6e400002-…`, TX `6e400003-…`). Claude Desktop's side follows the [claude-desktop-buddy spec](https://github.com/anthropics/claude-desktop-buddy/blob/main/REFERENCE.md); it carries token counts only, no plan usage.

Heartbeat (host → board). The hub adds `spark` and `limits`:
```json
{"total":2,"running":1,"waiting":0,"msg":"Tool: Bash",
 "entries":["14:32 Bash pio run"],"tokens":0,"tokens_today":48210,
 "spark":[0,0,1,3,...],                       // tool calls per minute, last 30 min, oldest first
 "limits":{"h5":24.0,"h5s":7997,"d7":41.0,"d7s":249997,"cx":18.0},   // % used + seconds to reset; cx = context %
 "prompt":{"id":"cc:abc:1","tool":"Bash","hint":"git status","info":true}}   // info = display-only
```
Board → host: `{"cmd":"permission","id":"…","decision":"once"|"deny"}`, plus `{"hello":"claude-buddy","name":"Claude-XXXX"}` on boot (which makes the hub resend the clock).
Debug lines starting with `#` are printed on the serial port and ignored by the hub (BLE events are echoed into its log).

State is tracked per session and merged, and **everything in progress expires**, so an interrupted turn can't leave the display stuck on WORKING:

| Signal | Effect |
|---|---|
| `UserPromptSubmit` | session thinking (WORKING) |
| `PreToolUse` / `PostToolUse` | current tool, activity feed, calls/min chart |
| `Notification` (permission) | needs-you modal, display-only |
| `Notification` (idle) / `Stop` | back to IDLE (idle also recovers from an Esc interrupt) |
| silence | thinking 120 s, tool call 10 min, permission prompt 10 min, session 1 h |

---

## Troubleshooting

**Claude Desktop shows "disconnected" / nothing reaches the board.** Removing the device inside Claude Desktop does *not* remove the Mac's own Bluetooth pairing. Forget it in **System Settings → Bluetooth**, quit and reopen Claude Desktop, then connect again. To also clear the board's side: `esptool.py erase_region 0x9000 0x5000` (then re-pair).

**Board shows "Waiting for a link".** Nothing is connected. USB: check `systemctl status claude-buddy` and `curl hub:8765/state` (`device.connected`). Bluetooth: pair as above.

**USB+BLE both connected but only one shows data.** Each card is its own source; "OFFLINE" means that link isn't live.

**Plan usage rows are empty.** Needs Claude Pro/Max and the status line line from step 3. The `B` bar never shows plan usage: Claude Desktop doesn't send it, so it shows Bluetooth tokens instead.

**Inspect the board.** Stop the hub (it holds the port), then open `/dev/ttyUSB0` at 115200 (pyserial, DTR/RTS off). `# alive …` lines show heap and frame count; a stall prints the stuck program counter and reboots (watchdog).

**Colors look washed out/blue in photos.** The panel is a TN display; it lifts dark colors when seen off-axis or photographed from an angle.

**Serial port permission denied.** `sudo usermod -aG dialout $USER` (log out/in), or `sudo chmod 666 /dev/ttyUSB0` once.

**Firmware-development gotchas (LVGL on ESP32).** With BLE running only ~50 KB of heap is free, so: classic-BT RAM is released before BLE starts, render buffers are small (24-line stripes), and animations recolor instead of fading (opacity makes LVGL allocate a layer buffer, which froze the UI). LVGL's built-in "dots" label truncation loops forever on the board, so text is clipped by hand. A DMA display flush hung after a few seconds and is not used.

---

## Earlier firmware

`firmware-cyd/` is the first, text-style "sci-fi HUD" UI (it also reads USB serial). Flash with `pio run -e cyd --target upload` in that folder. `firmware/` is the M5StickC build (BLE; button A approve, button B deny) and is not wired to the hub.
