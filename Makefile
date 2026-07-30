CC ?= cc
PYTHON ?= python3
.DEFAULT_GOAL := all

# ---------------------------------------------------------------- libraries --
KILIX_GAME_KIT_DIR ?= third_party/kilix-game-kit
KILIX_GAME_KIT_ROOT := $(abspath $(KILIX_GAME_KIT_DIR))
include $(KILIX_GAME_KIT_DIR)/mk/game-kit.mk

KILIX_TOP_DOWN_DIR ?= third_party/kilix-top-down-engine
include $(KILIX_TOP_DOWN_DIR)/mk/kilix-top-down.mk

KILIX_ASSETS_DIR ?= third_party/kilix-assets
include $(KILIX_ASSETS_DIR)/mk/kilix-assets.mk

KILIX_UI_DIR ?= third_party/kilix-ui
include $(KILIX_UI_DIR)/mk/kilix-ui.mk

KILIX_WORLD_DIR ?= third_party/kilix-world
include $(KILIX_WORLD_DIR)/mk/kilix-world.mk

KILIX_STORY_DIR ?= third_party/kilix-story
include $(KILIX_STORY_DIR)/mk/kilix-story.mk

# ------------------------------------------------------------------- flags ---
override CPPFLAGS += -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L \
	-Ibuild -Isrc \
	$(KILIX_GAME_KIT_CPPFLAGS) $(KILIX_TD_CPPFLAGS) \
	$(KILIX_ASSETS_CPPFLAGS) $(KILIX_UI_CPPFLAGS) \
	$(KILIX_WORLD_CPPFLAGS) $(KILIX_STORY_CPPFLAGS)

WARNINGS := -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
	-Wstrict-prototypes -Wmissing-prototypes -Wformat=2
SANITIZE_CFLAGS := -O1 -g -std=c11 -pthread $(WARNINGS) \
	-fsanitize=address,undefined -fno-omit-frame-pointer
CFLAGS ?= -O2 -g
override CFLAGS += -std=c11 -pthread $(WARNINGS) -MMD -MP

LDLIBS := $(KILIX_ASSETS_LDLIBS) $(KILIX_WORLD_LDLIBS) $(KILIX_GAME_KIT_LDLIBS)
LIBS := $(KILIX_UI_LIB) $(KILIX_TD_LIBS) $(KILIX_ASSETS_LIB) \
	$(KILIX_STORY_LIB) $(KILIX_WORLD_LIBS) $(KILIX_GAME_KIT_LIB)

# ------------------------------------------------------------------ sources --
BIN := pleb-tower
SRC := src/main.c src/game.c src/board.c src/units.c src/fixture.c \
	src/combat.c src/economy.c src/render.c src/hud.c src/input.c \
	src/audio.c src/save.c src/content.c src/gather.c \
	src/simulate.c
OBJ := $(patsubst src/%.c,build/%.o,$(SRC))
DEPENDENCIES := $(OBJ:.o=.d)

CONTENT_SRC := content/campaigns.json content/stable_ids.json
CONTENT_HDR := build/content_generated.h
CONTENT_TOOL := tools/compile_content.py

TEST_SRC := tests/test_main.c tests/test_board.c tests/test_units.c \
	tests/test_fixture.c tests/test_combat.c tests/test_economy.c \
	tests/test_simulate.c tests/test_hud.c
TEST_OBJ := $(patsubst tests/%.c,build/tests/%.o,$(TEST_SRC))
GAME_OBJ_NO_MAIN := $(filter-out build/main.o,$(OBJ))
TEST_BIN := build/pleb-tower-tests
TEST_DEPENDENCIES := $(TEST_OBJ:.o=.d)

# ----------------------------------------------------------------- targets ---
all: $(BIN)

$(BIN): $(OBJ) $(LIBS)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LIBS) $(LDLIBS)

$(CONTENT_HDR): $(CONTENT_SRC) $(CONTENT_TOOL) | build
	$(PYTHON) $(CONTENT_TOOL) --out $@

content: $(CONTENT_HDR)

test-content: $(CONTENT_TOOL) $(CONTENT_SRC)
	$(PYTHON) $(CONTENT_TOOL) --check

build/%.o: src/%.c $(CONTENT_HDR) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

build/tests/%.o: tests/%.c $(CONTENT_HDR) | build/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(TEST_BIN): $(TEST_OBJ) $(GAME_OBJ_NO_MAIN) $(LIBS)
	$(CC) $(CFLAGS) -o $@ $(TEST_OBJ) $(GAME_OBJ_NO_MAIN) $(LIBS) $(LDLIBS)

build build/tests:
	mkdir -p $@

test: test-content $(BIN) $(TEST_BIN)
	$(TEST_BIN)
	./$(BIN) --selftest
	./$(BIN) --render-test build/preview.ppm

test-deps:
	$(MAKE) -C $(KILIX_GAME_KIT_ROOT) test
	$(MAKE) -C $(KILIX_TOP_DOWN_ROOT) SOFT_RASTER_DIR="$(SOFT_RASTER_DIR)" test
	$(MAKE) -C $(KILIX_WORLD_ROOT) test
	$(MAKE) -C $(KILIX_STORY_ROOT) test

verify-link: $(BIN)
	tools/verify_link.sh ./$(BIN)

# Cleans both before and after: instrumented objects left in build/ would
# otherwise fail to link into the next ordinary build with undefined __asan_*.
sanitize:
	$(MAKE) clean
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1 \
		$(MAKE) test CC=clang CFLAGS="$(SANITIZE_CFLAGS)"
	$(MAKE) clean

graphics:
	$(PYTHON) tools/compile_graphics.py

verify-graphics:
	$(PYTHON) tools/compile_graphics.py --check

audio:
	$(PYTHON) tools/generate_audio.py

verify-audio:
	$(PYTHON) tools/verify_audio.py

balance: $(CONTENT_HDR)
	$(PYTHON) tools/balance_sim.py --all

release-gate: test sanitize test-content verify-graphics verify-audio balance
	@echo "release gate: PASS"

clean:
	$(RM) -r build $(BIN)

.PHONY: all clean content test test-content test-deps sanitize balance \
	verify-link graphics verify-graphics audio verify-audio release-gate

-include $(DEPENDENCIES)
-include $(TEST_DEPENDENCIES)
