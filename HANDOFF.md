# Claude Buddy — Handoff

Setup and usage are in `README.md`. This file is the state of play for whoever picks it up next.

## Where things run (as of 2026-10-02)

- **Board:** original CYD (ESP32-2432S028, ILI9341), USB-C into the **AM21** (`100.64.149.28`, appears as `/dev/ttyUSB0`, CH340). Device name `Claude-E72E`. Running the LVGL GUI firmware (`firmware-cyd-gui`, `cyd` env).
- **Hub:** `bridge.py` as the `claude-buddy` systemd service on the AM21 (code in `~/claude-buddy`, `:8765`). Flash scripts live there too (`flash.sh` old UI, `flash-gui.sh` GUI; staged firmware in `~/claude-buddy/fw/` and `fw-gui/`).
- **Claude Code hooks:** registered in `~/.claude/settings.json` on the **Acer**; status line forwards plan usage (`~/.claude/statusline-command.sh`, original saved as `.bak-buddy-20261002`). Not set up on other machines.
- **Claude Desktop:** the MacBook, over Bluetooth. A different Claude account from the Acer's Claude Code, by design — which is why the `B` bar shows tokens, not plan usage.
- A copy of this project also exists at `~/claude-buddy` on the Optiplex (old, ignore it).

## Design decisions worth knowing

- **Two sources, kept separate.** Everything is tracked per source (index 0 = USB/Claude Code, 1 = BLE/Claude Desktop): running, waiting, tokens, status text, activity. Mixing them into one set of counters made the numbers flicker between sources.
- **`U` bar = plan usage** (Claude Code status line `rate_limits.five_hour`), **`B` bar = Bluetooth tokens today vs best day** (best day persisted in the board's flash, 100K floor). Claude Desktop sends no plan usage over BLE. The hub also supports per-host usage (`BUDDY_U_HOSTS`, hosts outside it feed a `limits.b` field) but the UI doesn't use it.
- **Claude Code permission prompts are display-only** (the hook just sees a `Notification`); only Claude Desktop prompts can be answered from the board. Idea: a `PermissionRequest` hook can return a decision, so the hub could hold the request until the board answers.
- **Everything in progress expires in the hub** (thinking 120 s, tool 10 min, prompt 10 min, session 1 h), so an interrupted turn can't leave it stuck on WORKING.
- **Token counting:** the hook sums each session's transcript growth at `Stop` (input + output + cache writes). A session that predates the hook starts counting from its first `Stop`.

## Things that bit us (don't relearn them)

- **Heap:** with BLE running ~50 KB is free. Release classic-BT RAM before BLE init; keep LVGL render stripes at 24 lines; never fade object opacity (allocates a layer; froze the UI inside `malloc`).
- **LVGL on ESP32:** `LV_LABEL_LONG_DOT` loops forever at render time (fine on the host!) — clip text manually (`clipText` in `ui.cpp`). `pushPixelsDMA` flushing hung after ~3 s — still blocking, ~110 flushes/s.
- **Host preview doesn't catch everything:** the zero-data and memory cases only show up on the board. Use the serial debug lines (`# alive … heap=…`) and the watchdog's stuck-PC dump.
- **Opening the serial port resets the board** (DTR/RTS), so restarting the hub reboots it; the hub resends the clock on the board's `hello`.
- **macOS pairing:** removing the device in Claude Desktop leaves the Mac's Bluetooth pairing in place and blocks reconnecting — forget it in System Settings too. The board's passkey screen must show even with USB linked (it does).
- **LVGL config:** the template's enable guard is `#if 0 /* Set this to "1" to enable content */`; if it isn't flipped, `lv_conf.h` is silently ignored (default fonts, "undefined reference" link errors). Delete stale `.o` files after changing it.
- A TN panel looks washed out off-axis; photos taken at an angle exaggerate it.

## Open items / ideas

1. Confirm the `cyd_new` (ILI9342) GUI build on the real board — it compiles, untested on hardware.
2. Smoother animation: find why the DMA flush hangs, or keep blocking and lower the animation rate.
3. Approving Claude Code permissions from the board (see above).
4. Plan usage for the Bluetooth/Mac account would need Claude Code on that machine with the same status-line forward; Desktop alone can't provide it.
5. Optional backlight dimming when asleep (deliberately not done).
6. `firmware/` (M5StickC) is legacy and not connected to the hub.
