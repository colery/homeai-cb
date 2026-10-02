# Claude Buddy — LVGL GUI firmware (CYD)

Windowed/card UI for the CYD (ESP32-2432S028). Same links and protocol as `../firmware-cyd`
(USB serial from the hub + BLE from Claude Desktop, tracked per source); only the presentation
is different.

- `src/ui.cpp`, `src/ui.h` — the whole UI, pure LVGL, no hardware access.
- `src/main.cpp` — hardware, BLE/serial protocol, state, LVGL glue.
- `include/lv_conf.h` — LVGL config (shared with the host preview).
- `preview/` — compiles `ui.cpp` on the host and renders every screen to PNG: `cd preview && make`
  (output in `preview/out/`, `sheet.png` is a contact sheet). Use this to check layout without a board.

Build: `pio run -e cyd` (original ILI9341 board) or `-e cyd_new` (ILI9342 variant). Flash with
`deploy/flash.sh`-style esptool, or on the hub host `~/claude-buddy/flash-gui.sh cyd`.
Needs the `huge_app` partition table (set in platformio.ini); the default one is too small.

Memory notes (learned the hard way): with BLE running only ~50 KB of heap is left, so
`esp_bt_controller_mem_release(CLASSIC_BT)` runs before BLE init, and animations recolor
instead of changing object opacity (opacity < 255 makes LVGL allocate a layer buffer, which
froze the UI before). Debug lines start with `#` on the serial port; the hub ignores them.
