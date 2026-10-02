LVGL := ../.pio/libdeps/cyd/lvgl
BUILD := build
SRCS := $(shell find $(LVGL)/src -name '*.c' -not -path '*/drivers/*' -not -path '*/others/vg_lite_tvg/*')
OBJS := $(patsubst $(LVGL)/%.c,$(BUILD)/lvgl/%.o,$(SRCS))
CFLAGS := -O1 -w -MMD -DLV_CONF_INCLUDE_SIMPLE -I../include -I$(LVGL) -I$(LVGL)/src -ffunction-sections

all: $(BUILD)/preview
	mkdir -p out && $(BUILD)/preview && python3 render.py

$(BUILD)/lvgl/%.o: $(LVGL)/%.c
	@mkdir -p $(dir $@)
	@gcc $(CFLAGS) -c $< -o $@

$(BUILD)/host_main.o: host_main.cpp ../src/ui.h
	@mkdir -p $(BUILD)
	g++ $(CFLAGS) -std=c++17 -c host_main.cpp -o $@
$(BUILD)/ui.o: ../src/ui.cpp ../src/ui.h
	@mkdir -p $(BUILD)
	g++ $(CFLAGS) -std=c++17 -c ../src/ui.cpp -o $@

$(BUILD)/preview: $(OBJS) $(BUILD)/host_main.o $(BUILD)/ui.o
	g++ -Wl,--gc-sections $^ -o $@ -lm
-include $(shell find $(BUILD) -name "*.d" 2>/dev/null)
