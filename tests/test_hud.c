/* HUD queries, pointer picking, complete input bindings, and screen flow. */
#include "pt_test.h"
#include "pleb_tower.h"

#include "kitty_input.h"
#include "soft_raster.h"

#include <stdbool.h>
#include <stdint.h>
#include <math.h>
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

static void pointer_event(pt_game *game, pt_input_state *input,
                           int x, int y, bool click)
{
    kittyin_event event = {0};
    event.kind = KITTYIN_EVENT_MOUSE;
    event.data.mouse.x = x;
    event.data.mouse.y = y;
    event.data.mouse.action = (uint8_t)(click ? KITTYIN_MOUSE_PRESS : KITTYIN_MOUSE_MOVE);
    event.data.mouse.button = click ? 1u : 0u;
    event.data.mouse.pixel_coordinates = true;
    pt_input_event(input, &event);
    pt_input_apply(game, input);
}

static void test_blank_menu_clicks_do_nothing(void)
{
    pt_game game;
    pt_input_state input;
    pt_game_init(&game, 0u, UINT64_C(0x2b10));
    pt_input_reset(&input);
    pointer_event(&game, &input, 72, 168, true);
    int32_t funds = game.economy.currency;
    pointer_event(&game, &input, 20, 230, true);
    PT_CHECK(pt_fixture_at_pad(&game, 4u) == NULL, "blank shop space does not buy");
    PT_CHECK_EQ_INT(game.economy.currency, funds);
    pointer_event(&game, &input, 20, 60, true);
    PT_CHECK(pt_fixture_at_pad(&game, 4u) != NULL, "valid shop row still buys");
    /* An unavailable upgrade stays focused; repeated confirmation is safe. */
    pt_game_init(&game, 0u, UINT64_C(0x2b14));
    pt_input_reset(&input);
    game.economy.currency = 60;
    PT_CHECK(pt_fixture_place(&game, 4u, 0u), "place inspector target with no funds left");
    pointer_event(&game, &input, 72, 168, true);
    PT_CHECK_EQ_INT(pt_input_ui_focus(), 0);
    tap_key(&game, &input, KITTYKB_KEY_ENTER);
    PT_CHECK(pt_fixture_at_pad(&game, 4u) != NULL,
        "confirming an unavailable upgrade does not sell the tower");
    pointer_event(&game, &input, 20, 230, true);
    PT_CHECK(pt_fixture_at_pad(&game, 4u) != NULL, "blank inspector space does not sell");
    PT_CHECK_EQ_INT(game.economy.currency, 0);
    pointer_event(&game, &input, 20, 26, true);
    PT_CHECK(pt_fixture_at_pad(&game, 4u) != NULL, "click outside inspector does not sell");
    pointer_event(&game, &input, 20, PT_INSPECTOR_Y + 5 + 2 * 18 + 6, true);
    PT_CHECK(pt_fixture_at_pad(&game, 4u) == NULL, "valid Sell row still sells");
}

static void test_hud_clicks_match_labels(void)
{
    pt_game game;
    pt_input_state input;
    pt_game_init(&game, 0u, UINT64_C(0x2b11));
    pt_input_reset(&input);
    const int preview_y = PT_PLAYFIELD_HEIGHT + PT_HUD_STATUS_HEIGHT + 8;
    pointer_event(&game, &input, PT_HUD_HELP_X + 8, preview_y, true);
    PT_CHECK_EQ_INT(game.phase, PT_PHASE_BUILD);
    pointer_event(&game, &input, PT_HUD_ACTION_X + 8, preview_y, true);
    PT_CHECK_EQ_INT(pt_input_ui_panel(), 0);
    PT_CHECK(game.wave.awaiting_call, "NEXT clicks do not trigger HUD actions or start the wave");
    pointer_event(&game, &input, PT_HUD_ACTION_X + 8,
        PT_LOGICAL_HEIGHT - PT_HUD_SELECTION_HEIGHT + 8, true);
    PT_CHECK_EQ_INT(pt_input_ui_panel(), 0);
    PT_CHECK(game.wave.awaiting_call, "selection footer is informational");
    pointer_event(&game, &input, 400, 262, true);
    PT_CHECK_EQ_INT(game.phase, PT_PHASE_HELP);
    PT_CHECK(game.wave.awaiting_call, "Help click does not start first wave");
    tap_key(&game, &input, KITTYKB_KEY_ESCAPE);
    pointer_event(&game, &input, 72, 168, false);
    pointer_event(&game, &input, 290, 262, true);
    PT_CHECK_EQ_INT(pt_input_ui_panel(), 1);
    PT_CHECK(game.wave.awaiting_call, "Build label opens shop without calling wave");
    pointer_event(&game, &input, 290, 262, true);
    PT_CHECK(pt_fixture_at_pad(&game, 4u) == NULL, "Build label does not confirm shop purchase");
    pointer_event(&game, &input, 250, 245, true);
    PT_CHECK(game.wave.awaiting_call, "currency area does not start wave");
    pointer_event(&game, &input, 290, 245, true);
    PT_CHECK(!game.wave.awaiting_call, "countdown click starts wave");
}

static void test_zoom_pointer_does_not_recenter(void)
{
    pt_game game;
    pt_input_state input;
    pt_game_init(&game, 0u, UINT64_C(0x2b12));
    pt_input_reset(&input);
    game.cursor.x = 15;
    game.cursor.y = 7;
    tap_key(&game, &input, 'z');
    pt_renderer renderer;
    PT_CHECK(pt_render_init(&renderer, 960, 540), "initialize zoom renderer");
    pt_render_frame(&renderer, &game, 0.0);
    pointer_event(&game, &input, 400, 120, false);
    pt_render_frame(&renderer, &game, 0.0);
    int first_x = game.cursor.x, first_y = game.cursor.y;
    pointer_event(&game, &input, 400, 120, false);
    pt_render_frame(&renderer, &game, 0.0);
    PT_CHECK_EQ_INT(game.cursor.x, first_x);
    PT_CHECK_EQ_INT(game.cursor.y, first_y);
    int origin_x, origin_y;
    pt_input_zoom_origin(&game, &origin_x, &origin_y);
    tap_key(&game, &input, 'd');
    PT_CHECK_EQ_INT(game.cursor.x, first_x + 1);
    int moved_x, moved_y;
    pt_input_zoom_origin(&game, &moved_x, &moved_y);
    PT_CHECK(moved_x != origin_x || moved_y != origin_y,
        "keyboard movement still pans the zoomed camera");
    pt_render_shutdown(&renderer);
}

static void test_support_circle_covers_supported_pads(void)
{
    pt_game game;
    pt_input_state input;
    pt_renderer renderer;
    pt_game_init(&game, 0u, UINT64_C(0x2b13));
    pt_input_reset(&input);
    game.economy.currency = 1000;
    PT_CHECK(pt_fixture_place(&game, 8u, 7u), "place Workshop for coverage render");
    PT_CHECK(pt_fixture_place(&game, 4u, 0u), "place fixture within Workshop coverage");
    PT_CHECK(game.fixtures[3].damage_scale > 1.0f, "hairpin fixture receives support");
    if (!pt_render_init(&renderer, PT_LOGICAL_WIDTH, PT_LOGICAL_HEIGHT)) {
        PT_CHECK(false, "coverage renderer"); return;
    }
    for (uint8_t tier = 0u; tier < PT_MAX_TIER; ++tier) {
        if (tier > 0u) PT_CHECK(pt_fixture_upgrade(&game, 8u), "upgrade Workshop coverage");
        const pt_tier_def *definition = &pt_fixture_def_at(0u, 7u)->tiers[tier];
        int x = (int)roundf(88.0f - definition->radius * PT_CELL_PIXELS);
        size_t offset = ((size_t)120 * 480u + (size_t)x) * 4u;
        game.cursor.pad = 0;
        pt_render_frame(&renderer, &game, 0.0);
        uint8_t before[4];
        memcpy(before, renderer.rgba + offset, 4u);
        game.cursor.x = 5;
        game.cursor.y = 7;
        game.cursor.pad = 8;
        pt_render_frame(&renderer, &game, 0.0);
        PT_CHECK(memcmp(before, renderer.rgba + offset, 4u) != 0,
            "tier %u circle marks the actual support boundary", tier);
    }
    pt_render_shutdown(&renderer);
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

    mouse.data.mouse.y = PT_INSPECTOR_Y + 5 + 6 * 18 + 8;
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
        if (enabled[index]) ++enabled_count;
        PT_CHECK(enabled[index] == expected,
                 "role %zu affordability matches cost", index);
    }
    PT_CHECK_EQ_INT(enabled_count, affordable);

    pt_input_reset(&input);
    pad = pt_pad(10u);
    PT_CHECK(pad != NULL, "affordability menu pad exists");
    if (pad != NULL) {
        game.cursor.x = (int8_t)pad->x;
        game.cursor.y = (int8_t)pad->y;
        game.cursor.pad = 10u;
        input.confirm = true;
        pt_input_apply(&game, &input);
        PT_CHECK_EQ_INT(pt_input_ui_focus(), 0);
        tap_key(&game, &input, KITTYKB_KEY_DOWN);
        PT_CHECK_EQ_INT(pt_input_ui_focus(), 1);
        PT_CHECK(!enabled[1], "highlighted artillery is unaffordable");
        tap_key(&game, &input, KITTYKB_KEY_ENTER);
        PT_CHECK(pt_fixture_at_pad(&game, 10u) == NULL,
            "browsing an unaffordable weapon cannot purchase it");
        PT_CHECK_EQ_INT(game.economy.currency, 100);
        pointer_event(&game, &input, 20, 60 + 7 * 18, false);
        PT_CHECK_EQ_INT(pt_input_ui_focus(), 7);
        PT_CHECK(pt_fixture_at_pad(&game, 10u) == NULL,
            "hover previews an affordable weapon without buying it");
        pointer_event(&game, &input, 20, 230, false);
        PT_CHECK_EQ_INT(pt_input_ui_focus(), 7);
        pointer_event(&game, &input, 20, 60 + 18, false);
        PT_CHECK_EQ_INT(pt_input_ui_focus(), 1);
        game.economy.currency = (int32_t)pt_fixture_def_at(game.campaign, 1u)->tiers[0].cost;
        tap_key(&game, &input, KITTYKB_KEY_ENTER);
        PT_CHECK(pt_fixture_at_pad(&game, 10u) != NULL &&
            pt_fixture_at_pad(&game, 10u)->kind == 1u,
            "highlighted weapon can be purchased once funds are available");
        PT_CHECK_EQ_INT(game.economy.currency, 0);
        PT_CHECK_EQ_INT(pt_input_ui_focus(), 0);
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

static void test_selection_stays_off_battlefield(void)
{
    pt_game game;
    pt_input_state input;
    pt_renderer renderer;
    pt_game_init(&game, 0u, UINT64_C(0xf007));
    pt_input_reset(&input);
    PT_CHECK(pt_fixture_place(&game, 4u, 0u), "place footer inspection tower");
    pt_fixture *fixture = pt_fixture_at_pad(&game, 4u);
    if (fixture == NULL) return;
    game.cursor.pad = 4u;
    game.cursor.x = 4;
    game.cursor.y = 10;
    if (!pt_render_init(&renderer, PT_LOGICAL_WIDTH, PT_LOGICAL_HEIGHT)) {
        PT_CHECK(false, "initialize selection footer renderer");
        return;
    }
    size_t board_bytes = (size_t)PT_LOGICAL_WIDTH * PT_PLAYFIELD_HEIGHT * 4u;
    size_t frame_bytes = (size_t)PT_LOGICAL_WIDTH * PT_LOGICAL_HEIGHT * 4u;
    uint8_t *before = malloc(frame_bytes);
    PT_CHECK(before != NULL, "allocate footer comparison");
    if (before != NULL) {
        pt_render_frame(&renderer, &game, 0.0);
        memcpy(before, renderer.rgba, frame_bytes);
        fixture->mode = PT_TARGET_STRONGEST;
        pt_render_frame(&renderer, &game, 0.0);
        PT_CHECK(memcmp(before, renderer.rgba, board_bytes) == 0,
            "selected tower information leaves the entire battlefield unobstructed");
        PT_CHECK(memcmp(before + board_bytes, renderer.rgba + board_bytes,
            frame_bytes - board_bytes) != 0,
            "selected tower information updates in the footer");
        PT_CHECK(pt_render_write_ppm(&renderer, "build/selection-footer.ppm"),
            "write selection footer for visual review");
        free(before);
    }
    pt_render_shutdown(&renderer);
}

static void test_weapon_descriptions(void)
{
    char lines[PT_DESCRIPTION_LINES][PT_DESCRIPTION_CAPACITY];
    for (uint8_t campaign = 0u; campaign < PT_CAMPAIGN_COUNT; ++campaign) {
        pt_game game;
        pt_game_init(&game, campaign, UINT64_C(0xd35c));
        for (uint16_t kind = 0u; kind < pt_campaign(campaign)->fixture_count; ++kind) {
            for (uint8_t tier = 0u; tier < PT_MAX_TIER; ++tier) {
                pt_fixture fixture = {.kind = kind, .tier = tier};
                for (int upgrade = 0; upgrade < 2; ++upgrade) {
                    PT_CHECK(pt_fixture_describe(&game, &fixture, upgrade != 0, lines),
                        "describe campaign %u kind %u tier %u", campaign, kind, tier);
                    for (int line = 0; line < PT_DESCRIPTION_LINES; ++line)
                        PT_CHECK(lines[line][0] != '\0' && sr_text_width(lines[line], 1) <= 452,
                            "description fits: %s", lines[line]);
                }
            }
        }
        pt_fixture fixture = {.kind = 0u, .tier = 1u};
        PT_CHECK(pt_fixture_describe(&game, &fixture, true, lines), "describe final rapid upgrade");
        PT_CHECK(strstr(lines[1], "14>18") && strstr(lines[1], "2.8>3"),
            "rapid upgrade explains damage and range changes");
        PT_CHECK(strstr(lines[3], "NEW:") && strstr(lines[3], "air"),
            "rapid tier 3 highlights newly unlocked air targeting");
        fixture.tier = 2u;
        pt_fixture_describe(&game, &fixture, true, lines);
        PT_CHECK(strstr(lines[0], "ground and air") && strchr(lines[1], '>') == NULL,
            "max tier describes current capabilities without a fictional upgrade");
        fixture.kind = 3u; fixture.tier = 0u;
        pt_fixture_describe(&game, &fixture, false, lines);
        PT_CHECK(strstr(lines[1], "2 move at 50%") && strstr(lines[2], "hardened"),
            "decoy description explains crowd sharing and immunity");
        fixture.kind = 7u;
        pt_fixture_describe(&game, &fixture, true, lines);
        PT_CHECK(strstr(lines[1], "15>25%") && strstr(lines[2], "3.2>3.5") &&
                 strstr(lines[3], "kills"), "support upgrade explains buffs, radius and kill income");
        fixture.kind = 0u; fixture.present = 1u;
        fixture.damage_scale = 1.25f; fixture.range_scale = 1.2f;
        pt_fixture_describe(&game, &fixture, true, lines);
        PT_CHECK(strstr(lines[1], "Damage 12>17") && strstr(lines[1], "Range 3>3.36"),
            "descriptions include actual support bonuses and damage rounding");
    }
}

static void test_air_warning_before_first_drones(void)
{
    for (uint8_t campaign = 0u; campaign < PT_CAMPAIGN_COUNT; ++campaign) {
        pt_game game;
        pt_input_state input;
        char lines[PT_AIR_WARNING_LINES][PT_AIR_WARNING_LINE_CAPACITY];
        pt_game_init(&game, campaign, UINT64_C(0xa17));
        pt_input_reset(&input);
        game.wave.awaiting_call = false;
        const uint16_t air_wave = campaign == 0u ? 5u : 3u;
        game.wave.index = (uint16_t)(air_wave - 2u);
        PT_CHECK(!pt_hud_air_warning_text(&game, lines), "no drone warning two rounds early");
        game.wave.index = (uint16_t)(air_wave - 1u);
        PT_CHECK(pt_hud_air_warning_text(&game, lines), "warn while building in the preceding round");
        PT_CHECK(strcmp(lines[0], campaign == 0u ?
            "AIR ALERT: Scout Drone in wave 6" :
            "AIR ALERT: Camera Drone in wave 4") == 0,
            "warning names the correct enemy and upcoming wave");
        PT_CHECK(strstr(lines[1], "goal") != NULL && strstr(lines[1], "Ground-only") != NULL,
            "warning explains flight and why ground towers miss");
        PT_CHECK(strcmp(lines[2], campaign == 0u ?
            "Use Jammer Mast or Tier 3 Rail Spike." :
            "Use Aerial Interceptor or Tier 3 Interdiction Turret.") == 0,
            "warning recommends this campaign's anti-air options");
        for (size_t line = 0u; line < PT_AIR_WARNING_LINES; ++line)
            PT_CHECK(sr_text_width(lines[line], 1) <= 464, "air warning line fits the panel");
        pt_renderer renderer;
        if (pt_render_init(&renderer, PT_LOGICAL_WIDTH * 2, PT_LOGICAL_HEIGHT * 2)) {
            const size_t band_offset = 0u;
            const size_t band_bytes = (size_t)PT_LOGICAL_WIDTH * PT_PLAYFIELD_HEIGHT * 4u * 4u;
            uint8_t *before = malloc(band_bytes);
            game.wave.index = (uint16_t)(air_wave - 2u);
            pt_render_frame(&renderer, &game, 0.0);
            PT_CHECK(before != NULL, "allocate battlefield comparison");
            if (before != NULL) memcpy(before, renderer.rgba + band_offset, band_bytes);
            game.wave.index = (uint16_t)(air_wave - 1u);
            pt_render_frame(&renderer, &game, 0.0);
            if (before != NULL) {
                PT_CHECK(memcmp(before, renderer.rgba + band_offset, band_bytes) == 0,
                    "NEXT and air alerts leave the entire battlefield unobstructed");
                free(before);
            }
            PT_CHECK(pt_render_write_ppm(&renderer, campaign == 0u ?
                "build/holdout-air-warning.ppm" : "build/cordon-air-warning.ppm"),
                "render the drone warning for review");
            tap_key(&game, &input, 'h');
            PT_CHECK_EQ_INT(game.phase, PT_PHASE_HELP);
            PT_CHECK(pt_hud_air_warning_text(&game, lines), "H opens the full air briefing");
            double remaining = game.wave.build_remaining;
            uint64_t tick = game.tick;
            pt_game_step(&game, 1.0);
            PT_CHECK(game.wave.build_remaining == remaining && game.tick == tick,
                "reading the briefing pauses play");
            pt_render_frame(&renderer, &game, 0.0);
            PT_CHECK(pt_render_write_ppm(&renderer, campaign == 0u ?
                "build/holdout-air-briefing.ppm" : "build/cordon-air-briefing.ppm"),
                "render full paused air briefing for review");
            tap_key(&game, &input, 'h');
            PT_CHECK_EQ_INT(game.phase, PT_PHASE_BUILD);
            pt_render_shutdown(&renderer);
        } else PT_CHECK(false, "initialize warning renderer");
        pt_game_set_phase(&game, PT_PHASE_WAVE);
        PT_CHECK(pt_hud_air_warning_text(&game, lines), "warn during the preceding round");
        pointer_event(&game, &input, PT_HUD_HELP_X + 8, PT_HUD_ACTION_Y + 4, true);
        PT_CHECK_EQ_INT(game.phase, PT_PHASE_HELP);
        PT_CHECK(pt_hud_air_warning_text(&game, lines), "clicking Air opens the briefing during combat");
        uint64_t combat_tick = game.tick;
        pt_game_step(&game, 1.0);
        PT_CHECK(game.tick == combat_tick, "briefing pauses combat simulation");
        tap_key(&game, &input, KITTYKB_KEY_ESCAPE);
        PT_CHECK_EQ_INT(game.phase, PT_PHASE_WAVE);
        game.wave.index = air_wave;
        pt_game_set_phase(&game, PT_PHASE_BUILD);
        PT_CHECK(pt_hud_air_warning_text(&game, lines), "retain warning until the first air wave starts");
        pt_game_set_phase(&game, PT_PHASE_WAVE);
        PT_CHECK(!pt_hud_air_warning_text(&game, lines), "hide the introduction once drones arrive");
        ++game.wave.index;
        pt_game_set_phase(&game, PT_PHASE_BUILD);
        PT_CHECK(!pt_hud_air_warning_text(&game, lines), "do not repeat after their introduction");
        pt_game_set_phase(&game, PT_PHASE_HELP);
        PT_CHECK(!pt_hud_air_warning_text(&game, lines), "help does not repeat an expired air briefing");
    }
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
        input.confirm = true;
        pt_input_apply(&game, &input);
        PT_CHECK_EQ_INT(game.phase, PT_PHASE_MAP_SELECT);
        check_render_frame(&renderer, &game, scale, "map selection");
        input.move_x = 1;
        pt_input_apply(&game, &input);
        check_render_frame(&renderer, &game, scale, "Rail Yard selection");

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

static void test_exit_requires_menu_selection(void)
{
    const pt_phase phases[] = {PT_PHASE_TITLE, PT_PHASE_BUILD, PT_PHASE_WAVE};
    for (size_t phase = 0u; phase < sizeof phases / sizeof phases[0]; ++phase) {
        pt_game game;
        pt_input_state input;
        pt_game_init(&game, 0u, UINT64_C(0xe817));
        pt_input_reset(&input);
        pt_game_set_phase(&game, phases[phase]);
        tap_key(&game, &input, 'q');
        tap_key(&game, &input, 'Q');
        PT_CHECK(!input.quit && game.phase == phases[phase], "Q never quits or changes screens");
        tap_key(&game, &input, KITTYKB_KEY_ESCAPE);
        PT_CHECK(game.phase == PT_PHASE_PAUSE && !input.quit, "Escape opens the menu without exiting");
        tap_key(&game, &input, 'q');
        PT_CHECK(!input.quit && game.phase == PT_PHASE_PAUSE, "Q does nothing in the menu");
        for (int row = 0; row < 3; ++row) tap_key(&game, &input, KITTYKB_KEY_DOWN);
        tap_key(&game, &input, KITTYKB_KEY_ESCAPE);
        PT_CHECK(!input.quit && game.phase == phases[phase], "Escape cancels even with Exit highlighted");
        tap_key(&game, &input, KITTYKB_KEY_ESCAPE);
        tap_key(&game, &input, KITTYKB_KEY_ENTER);
        PT_CHECK(!input.quit && game.phase == phases[phase], "reopening the menu defaults to Resume");
        tap_key(&game, &input, KITTYKB_KEY_ESCAPE);
        pointer_event(&game, &input, 20, 158, true);
        PT_CHECK(!input.quit && game.phase == PT_PHASE_PAUSE, "blank space below Exit is inert");
        if (phase == 0u) {
            for (int row = 0; row < 3; ++row) tap_key(&game, &input, KITTYKB_KEY_DOWN);
            tap_key(&game, &input, KITTYKB_KEY_ENTER);
        } else {
            pointer_event(&game, &input, 20, 78 + 3 * 18 + 6, true);
        }
        PT_CHECK(input.quit, "choosing Exit with keyboard or mouse requests shutdown");
    }
}

static void test_map_selection_controls(void)
{
    pt_game game;
    pt_input_state input;
    pt_game_init(&game, 0u, 987u);
    pt_input_reset(&input);
    pt_game_set_phase(&game, PT_PHASE_TITLE);
    tap_key(&game, &input, KITTYKB_KEY_ENTER);
    tap_key(&game, &input, KITTYKB_KEY_ENTER);
    PT_CHECK_EQ_INT(game.phase, PT_PHASE_MAP_SELECT);
    pointer_event(&game, &input, 300, PT_MAP_TABS_Y + 8, true);
    PT_CHECK_EQ_INT(pt_input_selected_map(), 1);
    PT_CHECK_EQ_INT(game.board.map, 0); /* preview does not mutate the current run */
    pointer_event(&game, &input, 200, 240, true);
    PT_CHECK_EQ_INT(game.phase, PT_PHASE_MAP_SELECT);
    tap_key(&game, &input, 'h');
    PT_CHECK_EQ_INT(game.phase, PT_PHASE_HELP);
    tap_key(&game, &input, KITTYKB_KEY_ESCAPE);
    PT_CHECK_EQ_INT(game.phase, PT_PHASE_MAP_SELECT);
    PT_CHECK_EQ_INT(pt_input_selected_map(), 1);
    tap_key(&game, &input, KITTYKB_KEY_ESCAPE);
    PT_CHECK_EQ_INT(game.phase, PT_PHASE_CAMPAIGN_SELECT);
    tap_key(&game, &input, KITTYKB_KEY_ENTER);
    tap_key(&game, &input, KITTYKB_KEY_RIGHT);
    pointer_event(&game, &input, 100, PT_MAP_DEPLOY_Y + 8, true);
    PT_CHECK_EQ_INT(game.phase, PT_PHASE_BUILD);
    PT_CHECK_EQ_INT(game.board.map, 1);
    PT_CHECK_EQ_INT(game.economy.currency, pt_game_campaign(&game)->starting_currency);
    pointer_event(&game, &input, 3 * 16 + 8, 10 * 16 + 8, true);
    PT_CHECK_EQ_INT(game.cursor.pad, 1);
    PT_CHECK_EQ_INT(pt_input_ui_panel(), 1);
    tap_key(&game, &input, KITTYKB_KEY_ENTER);
    PT_CHECK(pt_fixture_at_pad(&game, 1u) != NULL, "Rail Yard mouse pad buys a tower");
    tap_key(&game, &input, KITTYKB_KEY_ESCAPE); /* close inspector */
    tap_key(&game, &input, KITTYKB_KEY_ESCAPE); /* pause */
    tap_key(&game, &input, KITTYKB_KEY_DOWN);
    tap_key(&game, &input, KITTYKB_KEY_ENTER); /* restart */
    PT_CHECK_EQ_INT(game.board.map, 1);
    PT_CHECK_EQ_INT(game.phase, PT_PHASE_BUILD);
    PT_CHECK(pt_fixture_at_pad(&game, 1u) == NULL, "restart clears placed towers");
    PT_CHECK(game.wave.awaiting_call, "map restart restores first-wave planning");
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
    PT_CHECK_EQ_INT(game.phase, PT_PHASE_MAP_SELECT);
    tap_key(&game, &input, KITTYKB_KEY_ENTER);
    PT_CHECK_EQ_INT(game.phase, PT_PHASE_BUILD);
    PT_CHECK_EQ_INT(game.campaign, 0);

    tap_key(&game, &input, KITTYKB_KEY_ESCAPE);
    PT_CHECK_EQ_INT(game.phase, PT_PHASE_PAUSE);
    tap_key(&game, &input, KITTYKB_KEY_DOWN);
    tap_key(&game, &input, KITTYKB_KEY_DOWN);
    tap_key(&game, &input, KITTYKB_KEY_ENTER);
    PT_CHECK_EQ_INT(game.phase, PT_PHASE_CAMPAIGN_SELECT);
    tap_key(&game, &input, KITTYKB_KEY_ENTER);
    PT_CHECK_EQ_INT(game.phase, PT_PHASE_MAP_SELECT);
    tap_key(&game, &input, KITTYKB_KEY_ENTER);
    PT_CHECK_EQ_INT(game.phase, PT_PHASE_BUILD);

    tap_key(&game, &input, (uint32_t)'p');
    PT_CHECK_EQ_INT(game.phase, PT_PHASE_PAUSE);
    tap_key(&game, &input, (uint32_t)'p');
    PT_CHECK_EQ_INT(game.phase, PT_PHASE_BUILD);

    game.economy.integrity = 3;
    game.economy.currency = 0;
    tap_key(&game, &input, (uint32_t)'p');
    tap_key(&game, &input, KITTYKB_KEY_DOWN);
    tap_key(&game, &input, KITTYKB_KEY_ENTER);
    PT_CHECK_EQ_INT(game.phase, PT_PHASE_BUILD);
    PT_CHECK_EQ_INT(game.economy.integrity, pt_campaign(0u)->starting_integrity);
    PT_CHECK_EQ_INT(game.economy.currency, pt_campaign(0u)->starting_currency);
    PT_CHECK(game.wave.awaiting_call, "restart restores first-wave planning");

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

static void test_field_guide_and_speed_controls(void)
{
    char lines[PT_INTEL_LINES][PT_INTEL_LINE_CAPACITY];
    for (uint8_t campaign = 0u; campaign < PT_CAMPAIGN_COUNT; ++campaign) {
        for (uint16_t kind = 0u; kind < pt_campaign(campaign)->unit_count; ++kind) {
            PT_CHECK(pt_unit_describe(campaign, kind, lines), "describe campaign enemy");
            for (size_t line = 0u; line < PT_INTEL_LINES; ++line)
                PT_CHECK(lines[line][0] != '\0' && sr_text_width(lines[line], 1) <= 440,
                    "field guide text fits: %s", lines[line]);
            char health[32];
            snprintf(health, sizeof health, "Health %d ", pt_unit_def_at(campaign, kind)->integrity);
            PT_CHECK(strstr(lines[0], health) != NULL, "field guide uses this campaign's health");
        }
        pt_unit_describe(campaign, 3u, lines);
        PT_CHECK(strstr(lines[2], "ignoring the road") && strstr(lines[5], "Tier 3"),
            "guide explains air movement and counters");
        pt_unit_describe(campaign, 2u, lines);
        PT_CHECK(strstr(lines[6], "Immune") != NULL, "guide explains hardened immunity");
    }

    pt_game game;
    pt_input_state input;
    pt_game_init(&game, 0u, UINT64_C(0x1a7e1));
    pt_input_reset(&input);
    game.wave.awaiting_call = false;
    game.wave.index = 5u;
    tap_key(&game, &input, 'f');
    PT_CHECK_EQ_INT(game.speed, 1);
    pointer_event(&game, &input, 100, PT_PLAYFIELD_HEIGHT + PT_HUD_STATUS_HEIGHT + 10, true);
    PT_CHECK_EQ_INT(game.phase, PT_PHASE_INTEL);
    PT_CHECK_EQ_INT(pt_input_intel_kind(), 3);
    pt_game frozen = game;
    for (int tick = 0; tick < 180; ++tick) pt_game_advance(&game, PT_STEP_SECONDS);
    PT_CHECK(memcmp(&frozen, &game, sizeof game) == 0, "reading intel freezes the build countdown");
    pointer_event(&game, &input, 20, 130, true);
    PT_CHECK_EQ_INT(game.phase, PT_PHASE_INTEL);
    pointer_event(&game, &input, 230, PT_INTEL_NAV_Y + 8, true);
    PT_CHECK_EQ_INT(pt_input_intel_kind(), 4);
    tap_key(&game, &input, KITTYKB_KEY_LEFT);
    PT_CHECK_EQ_INT(pt_input_intel_kind(), 3);
    pt_renderer renderer;
    if (pt_render_init(&renderer, 960, 684)) {
        pt_render_frame(&renderer, &game, 0.0);
        PT_CHECK(pt_render_write_ppm(&renderer, "build/enemy-field-guide.ppm"), "capture air field guide");
        pt_render_shutdown(&renderer);
    } else PT_CHECK(false, "initialize field-guide renderer");
    tap_key(&game, &input, KITTYKB_KEY_ESCAPE);
    PT_CHECK_EQ_INT(game.phase, PT_PHASE_BUILD);
    pointer_event(&game, &input, 72, 168, true);
    PT_CHECK_EQ_INT(pt_input_ui_panel(), 1);
    tap_gamepad(&game, &input, 6u);
    PT_CHECK_EQ_INT(game.phase, PT_PHASE_INTEL);
    for (int i = 0; i < PT_UNITS_PER_CAMPAIGN; ++i)
        tap_key(&game, &input, KITTYKB_KEY_RIGHT);
    PT_CHECK_EQ_INT(pt_input_intel_kind(), 3);
    tap_gamepad(&game, &input, 1u);
    PT_CHECK_EQ_INT(game.phase, PT_PHASE_BUILD);
    PT_CHECK_EQ_INT(pt_input_ui_panel(), 1);
    tap_key(&game, &input, KITTYKB_KEY_ESCAPE);

    pt_game_set_phase(&game, PT_PHASE_WAVE);
    game.wave.active = true;
    tap_key(&game, &input, 'f');
    PT_CHECK_EQ_INT(game.speed, 2);
    pointer_event(&game, &input, PT_HUD_SPEED_X + 10, PT_PLAYFIELD_HEIGHT + 5, true);
    PT_CHECK_EQ_INT(game.speed, 1);
    tap_key(&game, &input, KITTYKB_KEY_TAB);
    PT_CHECK_EQ_INT(game.speed, 1);
    tap_gamepad(&game, &input, 5u);
    PT_CHECK_EQ_INT(game.speed, 2);
    tap_key(&game, &input, 'i');
    frozen = game;
    pt_game_advance(&game, 1.0);
    PT_CHECK(memcmp(&frozen, &game, sizeof game) == 0, "intel freezes combat even at 2x");
    tap_key(&game, &input, 'h');
    PT_CHECK_EQ_INT(game.phase, PT_PHASE_HELP);
    tap_key(&game, &input, KITTYKB_KEY_ESCAPE);
    PT_CHECK_EQ_INT(game.phase, PT_PHASE_INTEL);
    pointer_event(&game, &input, 380, PT_INTEL_NAV_Y + 8, true);
    PT_CHECK_EQ_INT(game.phase, PT_PHASE_WAVE);
    PT_CHECK_EQ_INT(game.speed, 2);
}

static void test_effects_render_and_reduced_motion(void)
{
    pt_game game;
    pt_game_init(&game, 0u, UINT64_C(0xefec7));
    pt_input_reset(NULL);
    pt_renderer renderer;
    if (!pt_render_init(&renderer, PT_LOGICAL_WIDTH, PT_LOGICAL_HEIGHT)) {
        PT_CHECK(false, "initialize combat effects renderer");
        return;
    }
    size_t bytes = (size_t)PT_LOGICAL_WIDTH * PT_LOGICAL_HEIGHT * 4u;
    size_t board_bytes = (size_t)PT_LOGICAL_WIDTH * PT_PLAYFIELD_HEIGHT * 4u;
    uint8_t *before = malloc(bytes);
    PT_CHECK(before != NULL, "allocate effect comparison");
    if (before != NULL) {
        pt_render_frame(&renderer, &game, 0.0);
        memcpy(before, renderer.rgba, bytes);
        for (int kind = 0; kind < PT_EFFECT_COUNT; ++kind)
            pt_effect_emit(&game, (pt_effect_kind)kind, 5.0f + (float)kind * 3.0f, 8.5f, 1.0f);
        pt_effects_update(&game, 0.08);
        pt_render_frame(&renderer, &game, 0.0);
        PT_CHECK(memcmp(before, renderer.rgba, board_bytes) != 0, "combat effects are visible");
        PT_CHECK(memcmp(before + board_bytes, renderer.rgba + board_bytes, bytes - board_bytes) == 0,
            "combat effects never cover the HUD");
        PT_CHECK(pt_render_write_ppm(&renderer, "build/combat-effects.ppm"), "capture effect styles");
        pt_settings settings;
        pt_settings_defaults(&settings);
        settings.reduced_motion = true;
        PT_CHECK(pt_save_store_settings(&settings), "enable reduced motion");
        pt_input_reset(NULL);
        pt_render_frame(&renderer, &game, 0.0);
        PT_CHECK(memcmp(before, renderer.rgba, bytes) == 0, "reduced motion suppresses combat bursts");
        settings.reduced_motion = false;
        PT_CHECK(pt_save_store_settings(&settings), "restore motion preference");
        pt_input_reset(NULL);
        free(before);
    }
    pt_render_shutdown(&renderer);
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
    test_air_warning_before_first_drones();
    test_weapon_descriptions();
    test_selection_stays_off_battlefield();
    test_layout_audit_and_surface_rendering();
    test_menu_render_helper();
    test_accessibility_settings_and_zoom_bindings();
    test_blank_menu_clicks_do_nothing();
    test_hud_clicks_match_labels();
    test_zoom_pointer_does_not_recenter();
    test_support_circle_covers_supported_pads();
    test_screen_state_transitions();
    test_map_selection_controls();
    test_exit_requires_menu_selection();
    test_gamepad_navigation_and_sell_chord();
    test_field_guide_and_speed_controls();
    test_effects_render_and_reduced_motion();
    pt_test_playthrough();

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
