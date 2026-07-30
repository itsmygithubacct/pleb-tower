/* HUD queries, pointer picking, complete input bindings, and screen flow. */
#include "pt_test.h"
#include "pleb_tower.h"

#include "kitty_input.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct pt_hud_preview_item {
    uint16_t kind;
    uint16_t count;
    bool first_appearance;
} pt_hud_preview_item;

void pt_hud_build_enabled(const pt_game *game,
                          bool enabled[PT_ROLE_COUNT]);
size_t pt_hud_next_wave_preview(const pt_game *game,
                                pt_hud_preview_item *items,
                                size_t capacity);
bool pt_hud_next_wave_item_text(const pt_game *game, size_t item_index,
                                char *text, size_t capacity);
bool pt_hud_layout_audit(unsigned int text_scale);
bool pt_hud_menu_render_test(const char *path);
void pt_input_gamepad_event(pt_input_state *input,
                            const struct kittyin_gamepad_event *event);
unsigned int pt_input_ui_panel(void);
size_t pt_input_ui_focus(void);
unsigned int pt_input_text_scale(void);
bool pt_input_zoom_enabled(void);
void pt_test_hud(void);

static bool join_path(char *output, size_t capacity,
                      const char *prefix, const char *suffix)
{
    size_t prefix_length;
    size_t suffix_length;

    if (output == NULL || prefix == NULL || suffix == NULL ||
        capacity == 0u)
        return false;
    prefix_length = strlen(prefix);
    suffix_length = strlen(suffix);
    if (prefix_length >= capacity ||
        suffix_length >= capacity - prefix_length)
        return false;
    (void)memcpy(output, prefix, prefix_length);
    (void)memcpy(output + prefix_length, suffix, suffix_length + 1u);
    return true;
}

static void key_event(pt_input_state *input, uint32_t key,
                      kittykb_action action)
{
    kittyin_event event;

    (void)memset(&event, 0, sizeof event);
    event.kind = KITTYIN_EVENT_KEY;
    event.data.key.key = key;
    event.data.key.action = action;
    event.data.key.event_type_explicit = true;
    pt_input_event(input, &event);
}

static void tap_key(pt_game *game, pt_input_state *input, uint32_t key)
{
    key_event(input, key, KITTYKB_ACTION_PRESS);
    pt_input_apply(game, input);
    key_event(input, key, KITTYKB_ACTION_RELEASE);
    pt_input_apply(game, input);
}

static void gamepad_button(pt_input_state *input, uint8_t button,
                           int16_t value)
{
    kittyin_gamepad_event event;

    (void)memset(&event, 0, sizeof event);
    event.kind = KITTYIN_GAMEPAD_BUTTON;
    event.gamepad = 0u;
    event.control = button;
    event.value = value;
    pt_input_gamepad_event(input, &event);
}

static void tap_gamepad(pt_game *game, pt_input_state *input, uint8_t button)
{
    gamepad_button(input, button, 1);
    pt_input_apply(game, input);
    gamepad_button(input, button, 0);
    pt_input_apply(game, input);
}

static void test_cursor_clamps_at_all_edges(void)
{
    pt_game game;
    pt_input_state input;

    pt_game_init(&game, 0u, UINT64_C(0x2b01));
    pt_input_reset(&input);
    game.cursor.x = 0;
    game.cursor.y = 0;
    tap_key(&game, &input, (uint32_t)'a');
    PT_CHECK_EQ_INT(game.cursor.x, 0);
    tap_key(&game, &input, KITTYKB_KEY_LEFT);
    PT_CHECK_EQ_INT(game.cursor.x, 0);
    tap_key(&game, &input, (uint32_t)'w');
    PT_CHECK_EQ_INT(game.cursor.y, 0);
    tap_key(&game, &input, KITTYKB_KEY_UP);
    PT_CHECK_EQ_INT(game.cursor.y, 0);

    game.cursor.x = PT_COLUMNS - 1;
    game.cursor.y = PT_ROWS - 1;
    tap_key(&game, &input, (uint32_t)'d');
    PT_CHECK_EQ_INT(game.cursor.x, PT_COLUMNS - 1);
    tap_key(&game, &input, KITTYKB_KEY_RIGHT);
    PT_CHECK_EQ_INT(game.cursor.x, PT_COLUMNS - 1);
    tap_key(&game, &input, (uint32_t)'s');
    PT_CHECK_EQ_INT(game.cursor.y, PT_ROWS - 1);
    tap_key(&game, &input, KITTYKB_KEY_DOWN);
    PT_CHECK_EQ_INT(game.cursor.y, PT_ROWS - 1);
}

static bool keyboard_neighbor(const pt_game *game, const pt_pad_def *pad,
                              int *x, int *y, uint32_t *key)
{
    static const int dx[4] = {-1, 1, 0, 0};
    static const int dy[4] = {0, 0, -1, 1};
    static const uint32_t keys[4] = {
        (uint32_t)'d', (uint32_t)'a', (uint32_t)'s', (uint32_t)'w'
    };
    size_t index;

    for (index = 0u; index < 4u; ++index) {
        int candidate_x = (int)pad->x + dx[index];
        int candidate_y = (int)pad->y + dy[index];
        if (candidate_x < 0 || candidate_x >= PT_COLUMNS ||
            candidate_y < 0 || candidate_y >= PT_ROWS)
            continue;
        if (game->board.pad_index[
                pt_cell_index(candidate_x, candidate_y)] != 0u)
            continue;
        *x = candidate_x;
        *y = candidate_y;
        *key = keys[index];
        return true;
    }
    return false;
}

static void test_keyboard_and_mouse_pad_selection(void)
{
    pt_game game;
    pt_input_state input;
    const pt_pad_def *keyboard_pad = NULL;
    const pt_pad_def *mouse_pad;
    uint32_t key = 0u;
    int x = 0;
    int y = 0;
    uint8_t pad_id;
    kittyin_event mouse;

    pt_game_init(&game, 0u, UINT64_C(0x2b02));
    pt_input_reset(&input);
    for (pad_id = 1u; pad_id <= PT_PAD_COUNT; ++pad_id) {
        keyboard_pad = pt_pad(pad_id);
        if (keyboard_pad != NULL &&
            keyboard_neighbor(&game, keyboard_pad, &x, &y, &key))
            break;
    }
    PT_CHECK(keyboard_pad != NULL && key != 0u,
             "find a keyboard-adjacent pad");
    if (keyboard_pad == NULL || key == 0u) return;
    game.cursor.x = (int8_t)x;
    game.cursor.y = (int8_t)y;
    game.cursor.pad = 0u;
    tap_key(&game, &input, key);
    PT_CHECK_EQ_INT(game.cursor.pad, keyboard_pad->id);

    mouse_pad = pt_pad(PT_PAD_COUNT);
    PT_CHECK(mouse_pad != NULL, "mouse target pad exists");
    if (mouse_pad == NULL) return;
    (void)memset(&mouse, 0, sizeof mouse);
    mouse.kind = KITTYIN_EVENT_MOUSE;
    mouse.data.mouse.x =
        (int32_t)mouse_pad->x * PT_CELL_PIXELS + PT_CELL_PIXELS / 2;
    mouse.data.mouse.y =
        (int32_t)mouse_pad->y * PT_CELL_PIXELS + PT_CELL_PIXELS / 2;
    mouse.data.mouse.button = 1u;
    mouse.data.mouse.action = (uint8_t)KITTYIN_MOUSE_PRESS;
    mouse.data.mouse.pixel_coordinates = true;
    pt_input_event(&input, &mouse);
    pt_input_apply(&game, &input);
    PT_CHECK_EQ_INT(game.cursor.pad, mouse_pad->id);
    PT_CHECK_EQ_INT(game.cursor.x, mouse_pad->x);
    PT_CHECK_EQ_INT(game.cursor.y, mouse_pad->y);

    mouse.data.mouse.x = 20;
    mouse.data.mouse.y = 60;
    pt_input_event(&input, &mouse);
    pt_input_apply(&game, &input);
    PT_CHECK(pt_fixture_at_pad(&game, (uint8_t)mouse_pad->id) != NULL,
             "mouse activates an affordable build-menu row");

    mouse.data.mouse.y = 194;
    pt_input_event(&input, &mouse);
    pt_input_apply(&game, &input);
    {
        pt_fixture *fixture =
            pt_fixture_at_pad(&game, (uint8_t)mouse_pad->id);
        PT_CHECK(fixture != NULL &&
                     fixture->mode == PT_TARGET_CLOSEST,
                 "mouse activates the Closest targeting chip");
    }
}

static void test_build_affordability_exact(void)
{
    pt_game game;
    pt_input_state input;
    bool enabled[PT_ROLE_COUNT];
    size_t index;
    size_t affordable = 0u;
    size_t enabled_count = 0u;
    size_t first_enabled = PT_ROLE_COUNT;
    size_t next_enabled = PT_ROLE_COUNT;
    const pt_pad_def *pad;

    pt_game_init(&game, 0u, UINT64_C(0x2b03));
    game.economy.currency = 100;
    pt_hud_build_enabled(&game, enabled);
    for (index = 0u; index < PT_ROLE_COUNT; ++index) {
        const pt_fixture_def *fixture =
            pt_fixture_def_at(game.campaign, (uint16_t)index);
        bool expected =
            fixture != NULL && fixture->tiers[0].cost <= 100u;
        if (expected) ++affordable;
        if (expected && first_enabled == PT_ROLE_COUNT)
            first_enabled = index;
        if (enabled[index]) ++enabled_count;
        PT_CHECK(enabled[index] == expected,
                 "role %zu affordability matches cost", index);
    }
    PT_CHECK_EQ_INT(enabled_count, affordable);

    for (index = first_enabled + 1u; index < PT_ROLE_COUNT; ++index)
        if (enabled[index]) {
            next_enabled = index;
            break;
        }
    pt_input_reset(&input);
    pad = pt_pad(10u);
    PT_CHECK(pad != NULL, "affordability menu pad exists");
    if (pad != NULL) {
        game.cursor.x = (int8_t)pad->x;
        game.cursor.y = (int8_t)pad->y;
        game.cursor.pad = 10u;
        input.confirm = true;
        pt_input_apply(&game, &input);
        PT_CHECK_EQ_INT(pt_input_ui_focus(), first_enabled);
        tap_key(&game, &input, KITTYKB_KEY_DOWN);
        PT_CHECK_EQ_INT(pt_input_ui_focus(), next_enabled);
    }

    game.economy.currency = -1;
    pt_hud_build_enabled(&game, enabled);
    for (index = 0u; index < PT_ROLE_COUNT; ++index)
        PT_CHECK(!enabled[index],
                 "negative balance disables role %zu", index);
}

static void test_accessibility_settings_and_zoom_bindings(void)
{
    pt_settings settings;
    pt_game game;
    pt_input_state input;
    kittyin_event wheel;

    pt_settings_defaults(&settings);
    settings.text_scale = 3u;
    settings.reduced_motion = true;
    settings.zoom_default = true;
    PT_CHECK(pt_save_store_settings(&settings),
             "store accessibility settings for HUD");
    pt_game_init(&game, 0u, UINT64_C(0x2b08));
    pt_input_reset(&input);
    PT_CHECK_EQ_INT(pt_input_text_scale(), 3);
    PT_CHECK(pt_input_zoom_enabled(),
             "zoom default reaches presentation state");

    tap_key(&game, &input, (uint32_t)'z');
    PT_CHECK(!pt_input_zoom_enabled(), "keyboard toggles zoom");
    (void)memset(&wheel, 0, sizeof wheel);
    wheel.kind = KITTYIN_EVENT_MOUSE;
    wheel.data.mouse.action = (uint8_t)KITTYIN_MOUSE_WHEEL;
    wheel.data.mouse.wheel_y = 1;
    pt_input_event(&input, &wheel);
    pt_input_apply(&game, &input);
    PT_CHECK(pt_input_zoom_enabled(), "mouse wheel toggles zoom");
    tap_gamepad(&game, &input, 10u);
    PT_CHECK(!pt_input_zoom_enabled(), "gamepad RS toggles zoom");

    pt_settings_defaults(&settings);
    PT_CHECK(pt_save_store_settings(&settings),
             "restore default accessibility settings");
    pt_input_reset(&input);
}

static void check_preview_matches_wave(pt_game *game, uint16_t wave_index)
{
    const pt_wave_def *wave =
        pt_wave_def_at(game->campaign, wave_index);
    pt_hud_preview_item items[PT_MAX_WAVE_GROUPS];
    size_t count;
    size_t index;

    PT_CHECK(wave != NULL, "preview source wave %u exists",
             (unsigned int)wave_index);
    if (wave == NULL) return;
    (void)memset(items, 0, sizeof items);
    count = pt_hud_next_wave_preview(
        game, items, sizeof items / sizeof items[0]);
    PT_CHECK_EQ_INT(count, wave->group_count);
    for (index = 0u; index < count && index < PT_MAX_WAVE_GROUPS;
         ++index) {
        bool first = wave->groups[index].type < 32u &&
            (wave->first_appearance &
             (UINT32_C(1) << wave->groups[index].type)) != 0u;
        PT_CHECK_EQ_INT(items[index].kind, wave->groups[index].type);
        PT_CHECK_EQ_INT(items[index].count, wave->groups[index].count);
        PT_CHECK(items[index].first_appearance == first,
                 "wave %u group %zu first-appearance marker",
                 (unsigned int)wave_index, index);
    }
}

static void test_next_wave_composition_and_first_flags(void)
{
    pt_game game;
    const pt_campaign_def *campaign;
    uint16_t wave_index;

    pt_game_init(&game, 0u, UINT64_C(0x2b04));
    campaign = pt_campaign(game.campaign);
    PT_CHECK(campaign != NULL, "campaign exists for previews");
    if (campaign == NULL) return;

    pt_game_set_phase(&game, PT_PHASE_BUILD);
    for (wave_index = 0u; wave_index < campaign->wave_count;
         ++wave_index) {
        game.wave.index = wave_index;
        check_preview_matches_wave(&game, wave_index);
    }

    if (campaign->wave_count > 1u) {
        game.wave.index = 0u;
        game.wave.active = true;
        pt_game_set_phase(&game, PT_PHASE_WAVE);
        check_preview_matches_wave(&game, 1u);
    }
    game.wave.index = (uint16_t)(campaign->wave_count - 1u);
    pt_game_set_phase(&game, PT_PHASE_WAVE);
    PT_CHECK_EQ_INT(pt_hud_next_wave_preview(&game, NULL, 0u), 0);
}

static void test_preview_rendered_text_exact(void)
{
    pt_game game;
    char text[64];

    pt_game_init(&game, 0u, UINT64_C(0x2c01));
    pt_game_set_phase(&game, PT_PHASE_BUILD);
    game.wave.index = 0u;
    PT_CHECK(pt_hud_next_wave_item_text(
                 &game, 0u, text, sizeof text),
             "format first preview item");
    PT_CHECK(strcmp(text, "!Walker x8") == 0,
             "known preview renders exactly '!Walker x8' (got '%s')", text);

    game.wave.index = 2u;
    PT_CHECK(pt_hud_next_wave_item_text(
                 &game, 1u, text, sizeof text),
             "format multi-type preview item");
    PT_CHECK(strcmp(text, "!Shielded Unit x3") == 0,
             "full multi-type name and count remain legible (got '%s')",
             text);
}

static void check_render_frame(pt_renderer *renderer, const pt_game *game,
                               unsigned int scale, const char *surface)
{
    pt_render_frame(renderer, game, 0.0);
    PT_CHECK(renderer->rgba != NULL,
             "text scale %u renders %s", scale, surface);
}

static void test_layout_audit_and_surface_rendering(void)
{
    unsigned int scale;

    for (scale = 1u; scale <= 3u; ++scale) {
        pt_settings settings;
        pt_game game;
        pt_input_state input;
        pt_renderer renderer;
        const pt_pad_def *pad;

        PT_CHECK(pt_hud_layout_audit(scale),
                 "all HUD strings fit at text-scale preference %u", scale);
        pt_settings_defaults(&settings);
        settings.text_scale = (uint8_t)scale;
        PT_CHECK(pt_save_store_settings(&settings),
                 "store text-scale preference %u for render audit", scale);
        pt_game_init(
            &game, 0u, UINT64_C(0x2c100000) + (uint64_t)scale);
        pt_input_reset(&input);
        PT_CHECK(pt_render_init(
                     &renderer, PT_LOGICAL_WIDTH, PT_LOGICAL_HEIGHT),
                 "initialize text-scale %u HUD renderer", scale);
        if (renderer.soft == NULL) continue;

        pad = pt_pad(10u);
        PT_CHECK(pad != NULL, "render-audit pad exists at scale %u", scale);
        if (pad != NULL && pad->id <= UINT8_MAX) {
            game.cursor.x = (int8_t)pad->x;
            game.cursor.y = (int8_t)pad->y;
            game.cursor.pad = (uint8_t)pad->id;
            input.confirm = true;
            pt_input_apply(&game, &input);
            PT_CHECK_EQ_INT(pt_input_ui_panel(), 1);
            check_render_frame(
                &renderer, &game, scale, "open build menu");

            input.confirm = true;
            pt_input_apply(&game, &input);
            PT_CHECK_EQ_INT(pt_input_ui_panel(), 2);
            check_render_frame(
                &renderer, &game, scale,
                "inspector and selected-fixture card");
        }

        pt_game_set_phase(&game, PT_PHASE_TITLE);
        check_render_frame(&renderer, &game, scale, "title");
        input.confirm = true;
        pt_input_apply(&game, &input);
        PT_CHECK_EQ_INT(game.phase, PT_PHASE_CAMPAIGN_SELECT);
        check_render_frame(
            &renderer, &game, scale, "campaign selection");

        pt_game_set_phase(&game, PT_PHASE_BUILD);
        input.pause = true;
        pt_input_apply(&game, &input);
        PT_CHECK_EQ_INT(game.phase, PT_PHASE_PAUSE);
        check_render_frame(&renderer, &game, scale, "pause");

        pt_game_set_phase(&game, PT_PHASE_VICTORY);
        check_render_frame(&renderer, &game, scale, "victory score");
        pt_game_set_phase(&game, PT_PHASE_DEFEAT);
        check_render_frame(&renderer, &game, scale, "defeat score");
        pt_render_shutdown(&renderer);
    }

    {
        pt_settings settings;

        pt_settings_defaults(&settings);
        PT_CHECK(pt_save_store_settings(&settings),
                 "restore default settings after render audit");
        pt_input_reset(NULL);
    }
}

static void test_menu_render_helper(void)
{
    char path[] = "/tmp/pleb-tower-menu-XXXXXX";
    unsigned char magic[2] = {0u, 0u};
    int descriptor = mkstemp(path);
    FILE *file;

    PT_CHECK(descriptor >= 0, "reserve menu-render test path");
    if (descriptor < 0) return;
    PT_CHECK(close(descriptor) == 0, "close reserved menu-render path");
    PT_CHECK(unlink(path) == 0, "release menu-render path for writer");
    PT_CHECK(pt_hud_menu_render_test(path),
             "menu-render helper writes build menu frame");
    file = fopen(path, "rb");
    PT_CHECK(file != NULL, "open menu-render PPM");
    if (file != NULL) {
        PT_CHECK(fread(magic, 1u, sizeof magic, file) == sizeof magic,
                 "read menu-render PPM signature");
        PT_CHECK(fclose(file) == 0, "close menu-render PPM");
    }
    PT_CHECK(magic[0] == (unsigned char)'P' &&
                 magic[1] == (unsigned char)'6',
             "menu-render helper emits a binary PPM");
    (void)unlink(path);
}

static void test_screen_state_transitions(void)
{
    pt_game game;
    pt_input_state input;
    const pt_campaign_def *campaign;

    pt_game_init(&game, 0u, UINT64_C(0x2b05));
    pt_input_reset(&input);
    pt_game_set_phase(&game, PT_PHASE_TITLE);
    tap_key(&game, &input, KITTYKB_KEY_ENTER);
    PT_CHECK_EQ_INT(game.phase, PT_PHASE_CAMPAIGN_SELECT);
    tap_key(&game, &input, KITTYKB_KEY_ESCAPE);
    PT_CHECK_EQ_INT(game.phase, PT_PHASE_TITLE);

    tap_key(&game, &input, KITTYKB_KEY_ENTER);
    PT_CHECK_EQ_INT(game.phase, PT_PHASE_CAMPAIGN_SELECT);
    tap_key(&game, &input, KITTYKB_KEY_DOWN);
    tap_key(&game, &input, KITTYKB_KEY_ENTER);
    PT_CHECK_EQ_INT(game.phase, PT_PHASE_BUILD);
    PT_CHECK_EQ_INT(game.campaign, 0);

    tap_key(&game, &input, KITTYKB_KEY_ESCAPE);
    PT_CHECK_EQ_INT(game.phase, PT_PHASE_CAMPAIGN_SELECT);
    tap_key(&game, &input, KITTYKB_KEY_ENTER);
    PT_CHECK_EQ_INT(game.phase, PT_PHASE_BUILD);

    tap_key(&game, &input, (uint32_t)'p');
    PT_CHECK_EQ_INT(game.phase, PT_PHASE_PAUSE);
    tap_key(&game, &input, (uint32_t)'p');
    PT_CHECK_EQ_INT(game.phase, PT_PHASE_BUILD);

    game.wave.active = true;
    pt_game_set_phase(&game, PT_PHASE_WAVE);
    tap_key(&game, &input, (uint32_t)'p');
    PT_CHECK_EQ_INT(game.phase, PT_PHASE_PAUSE);
    tap_key(&game, &input, KITTYKB_KEY_ESCAPE);
    PT_CHECK_EQ_INT(game.phase, PT_PHASE_WAVE);

    campaign = pt_campaign(game.campaign);
    PT_CHECK(campaign != NULL, "campaign exists for outcome transitions");
    if (campaign == NULL) return;
    game.wave.index = (uint16_t)(campaign->wave_count - 1u);
    game.wave.total = 1u;
    game.wave.spawned = 1u;
    game.units.live = 0u;
    pt_game_step(&game, PT_STEP_SECONDS);
    PT_CHECK_EQ_INT(game.phase, PT_PHASE_VICTORY);
    tap_key(&game, &input, KITTYKB_KEY_ENTER);
    PT_CHECK_EQ_INT(game.phase, PT_PHASE_TITLE);

    pt_game_init(&game, 0u, UINT64_C(0x2b06));
    pt_game_set_phase(&game, PT_PHASE_WAVE);
    game.wave.active = true;
    game.wave.total = 1u;
    game.wave.spawned = 0u;
    game.economy.integrity = 0;
    pt_game_step(&game, PT_STEP_SECONDS);
    PT_CHECK_EQ_INT(game.phase, PT_PHASE_DEFEAT);
    tap_key(&game, &input, KITTYKB_KEY_ESCAPE);
    PT_CHECK_EQ_INT(game.phase, PT_PHASE_TITLE);
}

static void test_gamepad_navigation_and_sell_chord(void)
{
    pt_game game;
    pt_input_state input;
    const pt_pad_def *pad = pt_pad(10u);
    int original_x;

    pt_game_init(&game, 0u, UINT64_C(0x2b07));
    pt_input_reset(&input);
    original_x = game.cursor.x;
    {
        kittyin_gamepad_event axis;
        (void)memset(&axis, 0, sizeof axis);
        axis.kind = KITTYIN_GAMEPAD_AXIS;
        axis.gamepad = 0u;
        axis.control = 0u;
        axis.value = -20000;
        pt_input_gamepad_event(&input, &axis);
        pt_input_apply(&game, &input);
        PT_CHECK_EQ_INT(game.cursor.x,
                        original_x > 0 ? original_x - 1 : 0);
        axis.value = 0;
        pt_input_gamepad_event(&input, &axis);
        pt_input_apply(&game, &input);
    }

    tap_gamepad(&game, &input, 7u);
    PT_CHECK_EQ_INT(game.phase, PT_PHASE_PAUSE);
    tap_gamepad(&game, &input, 1u);
    PT_CHECK_EQ_INT(game.phase, PT_PHASE_BUILD);

    PT_CHECK(pad != NULL, "gamepad target pad exists");
    if (pad == NULL) return;
    game.cursor.x = (int8_t)pad->x;
    game.cursor.y = (int8_t)pad->y;
    game.cursor.pad = 10u;
    tap_gamepad(&game, &input, 0u);
    tap_gamepad(&game, &input, 0u);
    PT_CHECK(pt_fixture_at_pad(&game, 10u) != NULL,
             "A opens build menu then places focused fixture");

    gamepad_button(&input, 4u, 1);
    gamepad_button(&input, 0u, 1);
    pt_input_apply(&game, &input);
    PT_CHECK(pt_fixture_at_pad(&game, 10u) == NULL,
             "LB + A sells selected fixture");
    gamepad_button(&input, 0u, 0);
    gamepad_button(&input, 4u, 0);
    pt_input_apply(&game, &input);
}

void pt_test_hud(void)
{
    char data_root[] = "/tmp/pleb-tower-hud-XXXXXX";
    char records_path[256];
    char settings_path[256];
    char app_path[256];
    const char *existing_root = getenv("XDG_DATA_HOME");
    char *saved_root = existing_root != NULL ? strdup(existing_root) : NULL;
    char *created = mkdtemp(data_root);

    PT_CHECK(existing_root == NULL || saved_root != NULL,
             "preserve existing data root");
    PT_CHECK(created != NULL, "create isolated HUD profile directory");
    if (created == NULL || (existing_root != NULL && saved_root == NULL)) {
        free(saved_root);
        return;
    }
    app_path[0] = '\0';
    records_path[0] = '\0';
    settings_path[0] = '\0';
    PT_CHECK(setenv("XDG_DATA_HOME", created, 1) == 0,
             "select isolated HUD profile");

    test_cursor_clamps_at_all_edges();
    test_keyboard_and_mouse_pad_selection();
    test_build_affordability_exact();
    test_next_wave_composition_and_first_flags();
    test_preview_rendered_text_exact();
    test_layout_audit_and_surface_rendering();
    test_menu_render_helper();
    test_accessibility_settings_and_zoom_bindings();
    test_screen_state_transitions();
    test_gamepad_navigation_and_sell_chord();

    PT_CHECK(join_path(app_path, sizeof app_path,
                       created, "/pleb-tower"),
             "construct HUD app profile path");
    PT_CHECK(join_path(records_path, sizeof records_path,
                       app_path, "/records.state"),
             "construct HUD records path");
    PT_CHECK(join_path(settings_path, sizeof settings_path,
                       app_path, "/settings.state"),
             "construct HUD settings path");
    if (records_path[0] != '\0') (void)unlink(records_path);
    if (settings_path[0] != '\0') (void)unlink(settings_path);
    if (app_path[0] != '\0') (void)rmdir(app_path);
    (void)rmdir(created);
    if (saved_root != NULL)
        (void)setenv("XDG_DATA_HOME", saved_root, 1);
    else
        (void)unsetenv("XDG_DATA_HOME");
    free(saved_root);
}
