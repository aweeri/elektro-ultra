# elektro-ultra - Elektro-L LRIT/HRIT demodulator & decoder
# Plain Makefile. No CMake. Optional deps are auto-detected and only change
# which front-ends get built.

CXX      ?= g++
CXXSTD   := -std=c++20
WARN     := -Wall -Wextra -Wno-unused-parameter -Wshadow
# Vectorisation matters in this code: the NCO mix, the polyphase resampler and the
# RRC are per-sample loops. -march=native is worth a few percent on top of -O3
# here (measured 5 %, not the 1.6x it is easy to assume). Set ARCH=none for a
# portable binary, or ARCH=armv8-a when cross-building for the Pi.
ARCH     ?= native
ifeq ($(ARCH),none)
ARCHFLAG :=
else
ARCHFLAG := -march=$(ARCH)
endif
OPT      ?= -O3 $(ARCHFLAG) -fno-math-errno -funroll-loops
CPPFLAGS += -Isrc -Ithird_party -Ithird_party/imgui -Ithird_party/decompwt
CXXFLAGS += $(CXXSTD) $(WARN) $(OPT) -MMD -MP
LDFLAGS  +=
LDLIBS   += -lpthread -ldl -lz

BUILD    := build
BIN      := elektro-ultra

CORE_SRC := $(wildcard src/core/*.cpp) $(wildcard src/dsp/*.cpp) \
            $(wildcard src/fec/*.cpp) $(wildcard src/xrit/*.cpp) \
            $(wildcard third_party/decompwt/*.cpp)
CORE_OBJ := $(patsubst %.cpp,$(BUILD)/%.o,$(CORE_SRC))

CLI_SRC  := $(wildcard src/cli/*.cpp)
CLI_OBJ  := $(patsubst %.cpp,$(BUILD)/%.o,$(CLI_SRC))

# --- GUI (ImGui). Built into the same binary: the software rasteriser needs no
#     GL and no display, so `--gui` works everywhere the CLI does.
GUI_SRC  := $(wildcard src/gui/*.cpp) $(wildcard third_party/imgui/*.cpp)
GUI_OBJ  := $(patsubst %.cpp,$(BUILD)/%.o,$(GUI_SRC))
GUI_LIBS :=

TEST_SRC := $(wildcard tests/*.cpp)
TEST_BIN := $(patsubst tests/%.cpp,$(BUILD)/tests/%,$(TEST_SRC))

.PHONY: all clean test selftest gui debug asan dirs fmt corpus info

all: dirs $(BIN)

dirs:
	@mkdir -p $(BUILD)/src/core $(BUILD)/src/dsp $(BUILD)/src/fec \
	          $(BUILD)/src/xrit $(BUILD)/src/cli $(BUILD)/src/gui $(BUILD)/tests \
	          $(BUILD)/third_party/imgui

$(BIN): $(CORE_OBJ) $(CLI_OBJ) $(GUI_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)
	@echo "  built $@"

# --- `make gui` is now just `make`: the viewer is part of the binary.
gui: all
	@echo "  $(BIN) has the ImGui viewer (software rasteriser, no GL needed)"

debug: OPT := -O1 -g -fsanitize=address,undefined
debug: dirs $(BIN)

asan: OPT := -O1 -g -fsanitize=address,undefined
asan: dirs $(BIN)

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

$(BUILD)/tests/%: tests/%.cpp $(CORE_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $< $(CORE_OBJ) $(LDFLAGS) $(LDLIBS)

test: dirs $(TEST_BIN)
	@fail=0; for t in $(TEST_BIN); do echo "== $$t"; timeout 300 $$t || fail=1; done; \
	 exit $$fail

selftest: $(BIN)
	./$(BIN) selftest

corpus: $(BIN)
	./tools/triage.sh

clean:
	rm -rf $(BUILD) $(BIN)

fmt:
	@which clang-format >/dev/null 2>&1 || { echo "clang-format not installed"; exit 0; }
	clang-format -i $$(find src tests -name '*.cpp' -o -name '*.h')

info:
	@echo "CXX      = $(CXX)"
	@echo "core     = $(words $(CORE_SRC)) files"
	@echo "cli      = $(words $(CLI_SRC)) files"
	@echo "glfw     = $(shell pkg-config --exists glfw3 && echo yes || echo no)"

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
