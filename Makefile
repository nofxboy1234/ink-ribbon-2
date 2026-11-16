CC      := gcc
CXX     := g++
BUILD   := build
CFLAGS  := -std=gnu11 -g -O1 -Wall -Wextra -Wno-unused-parameter -DSOKOL_GLCORE -Isrc  -Ivendor/sokol -Ivendor/sokol/util -Ivendor/dcimgui/src $(shell pkg-config --cflags x11 xi xcursor)
CXXFLAGS:= -std=c++11 -g -O1 -w -Ivendor/dcimgui/src
LDFLAGS := -lGL -lX11 -lXi -lXcursor -ldl -lpthread -lm

IMGUI_SRC := vendor/dcimgui/src/imgui.cpp vendor/dcimgui/src/imgui_draw.cpp vendor/dcimgui/src/imgui_tables.cpp vendor/dcimgui/src/imgui_widgets.cpp vendor/dcimgui/src/imgui_demo.cpp vendor/dcimgui/src/cimgui.cpp
IMGUI_OBJ := $(patsubst vendor/dcimgui/src/%.cpp,$(BUILD)/%.o,$(IMGUI_SRC))
APP_OBJ   := $(BUILD)/main.o
CORE_OBJ  := $(BUILD)/geom.o $(BUILD)/level.o

.PHONY: all run test clean

all: $(BUILD)/editor

$(BUILD)/editor: $(APP_OBJ) $(IMGUI_OBJ)
	$(CXX) $^ $(LDFLAGS) -o $@

$(BUILD)/%.o: vendor/dcimgui/src/%.cpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/%.o: src/%.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: tests/%.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

run: $(BUILD)/editor
	./$(BUILD)/editor

$(BUILD)/test_level: $(BUILD)/test_level.o $(CORE_OBJ)
	$(CC) $^ -lm -o $@

test: $(BUILD)/test_level
	./$(BUILD)/test_level

clean:
	rm -rf $(BUILD)
