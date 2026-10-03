/* Semantic input over kitty-input.
 *
 * Keyboard and gamepad bindings share kitty-input's action mapper. Mouse
 * picking stays coordinate based, because pad selection needs the SGR pointer
 * position rather than a semantic button alone.
 */
#include "pleb_tower.h"

#include "kilix_ui.h"
#include "kitty_input.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

enum pt_input_action {
    PT_ACTION_MOVE_UP = 1,
    PT_ACTION_MOVE_DOWN,
    PT_ACTION_MOVE_LEFT,
    PT_ACTION_MOVE_RIGHT,
    PT_ACTION_CONFIRM,
    PT_ACTION_CANCEL,
    PT_ACTION_CYCLE,
    PT_ACTION_UPGRADE,
    PT_ACTION_REPAIR,
    PT_ACTION_SELL,
    PT_ACTION_CALL_WAVE,
    PT_ACTION_PAUSE,
    PT_ACTION_ZOOM,
    PT_ACTION_HELP,
    PT_ACTION_MUTE,
    PT_ACTION_SPEED,
    PT_ACTION_INTEL,
    PT_ACTION_GAMEPAD_WAVE
};

enum pt_ui_panel_kind {
    PT_UI_PANEL_NONE = 0,
    PT_UI_PANEL_BUILD,
    PT_UI_PANEL_INSPECTOR
};

enum {
    PT_INSPECT_UPGRADE = 0,
    PT_INSPECT_REPAIR,
    PT_INSPECT_SELL,
    PT_INSPECT_FIRST,
    PT_INSPECT_LAST,
    PT_INSPECT_STRONGEST,
    PT_INSPECT_CLOSEST,
    PT_INSPECT_COUNT
};

typedef struct pt_ui_runtime {
    kilix_ui_focus focus;
    kilix_ui_focus pause_focus;
    kilix_ui_focus map_focus;
    uint8_t selected_campaign;
    pt_settings settings;
    pt_records records;
    int32_t pointer_x;
    int32_t pointer_y;
    uint8_t panel;
    uint8_t pause_return;
    uint8_t help_return;
    uint8_t intel_return;
    uint16_t intel_kind;
    bool pointer_pending;
    bool primary_click;
    bool secondary_click;
    bool zoom_2x;
    bool zoom_anchored;
    int zoom_x, zoom_y;
    bool left_bumper;
    bool left_bumper_used;
} pt_ui_runtime;

static kittyin_action_map action_map;
static pt_ui_runtime ui;

/* Internal presentation queries used by hud.c. They have external linkage so
 * the two owned modules can share one deterministic UI state without adding a
 * second public header. */
unsigned int pt_input_ui_panel(void);
size_t pt_input_ui_focus(void);
size_t pt_input_pause_focus(void);
unsigned int pt_input_text_scale(void);
bool pt_input_zoom_enabled(void);
bool pt_input_campaign_is_unlocked(uint8_t campaign);
void pt_input_gamepad_event(pt_input_state *input,
                            const struct kittyin_gamepad_event *event);

unsigned int pt_input_ui_panel(void)
{
    return (unsigned int)ui.panel;
}

size_t pt_input_ui_focus(void)
{
    return ui.focus.selected;
}

size_t pt_input_pause_focus(void) { return ui.pause_focus.selected; }
uint8_t pt_input_selected_map(void) { return (uint8_t)ui.map_focus.selected; }
uint8_t pt_input_selected_campaign(void) { return ui.selected_campaign; }
const pt_records *pt_input_records(void) { return &ui.records; }
uint16_t pt_input_intel_kind(void) { return ui.intel_kind; }
bool pt_input_reduced_motion(void) { return ui.settings.reduced_motion; }

unsigned int pt_input_text_scale(void)
{
    unsigned int scale = (unsigned int)ui.settings.text_scale;
    return scale >= 1u && scale <= 3u ? scale : 1u;
}

bool pt_input_zoom_enabled(void)
{
    return ui.zoom_2x;
}

void pt_input_zoom_origin(const pt_game *game, int *x, int *y)
{
    if (ui.zoom_anchored) {
        *x = ui.zoom_x;
        *y = ui.zoom_y;
    } else pt_render_zoom_origin(game, x, y);
}

static void center_zoom(const pt_game *game)
{
    pt_render_zoom_origin(game, &ui.zoom_x, &ui.zoom_y);
    ui.zoom_anchored = ui.zoom_2x;
}

bool pt_input_campaign_is_unlocked(uint8_t campaign)
{
    return pt_campaign_unlocked(&ui.records, campaign);
}

static void bind_key(uint16_t action, uint32_t key)
{
    (void)kittyin_action_map_bind_key(&action_map, action, key, 0u);
}

static void bind_button(uint16_t action, uint8_t button)
{
    (void)kittyin_action_map_bind_gamepad_button(
        &action_map, action, KITTYIN_GAMEPAD_ANY, button);
}

static void bind_axis(uint16_t action, uint8_t axis, int direction)
{
    (void)kittyin_action_map_bind_gamepad_axis(
        &action_map, action, KITTYIN_GAMEPAD_ANY, axis, direction);
}

static void configure_actions(void)
{
    kittyin_action_map_init(&action_map);
    (void)kittyin_action_map_set_repeat(&action_map, 260u, 90u);
    (void)kittyin_action_map_set_dead_zone(&action_map, 12000);

    bind_key(PT_ACTION_MOVE_UP, (uint32_t)'w');
    bind_key(PT_ACTION_MOVE_UP, KITTYKB_KEY_UP);
    bind_key(PT_ACTION_MOVE_DOWN, (uint32_t)'s');
    bind_key(PT_ACTION_MOVE_DOWN, KITTYKB_KEY_DOWN);
    bind_key(PT_ACTION_MOVE_LEFT, (uint32_t)'a');
    bind_key(PT_ACTION_MOVE_LEFT, KITTYKB_KEY_LEFT);
    bind_key(PT_ACTION_MOVE_RIGHT, (uint32_t)'d');
    bind_key(PT_ACTION_MOVE_RIGHT, KITTYKB_KEY_RIGHT);
    bind_key(PT_ACTION_CONFIRM, KITTYKB_KEY_ENTER);
    bind_key(PT_ACTION_CONFIRM, (uint32_t)' ');
    bind_key(PT_ACTION_CANCEL, KITTYKB_KEY_ESCAPE);
    bind_key(PT_ACTION_CYCLE, (uint32_t)'t');
    bind_key(PT_ACTION_UPGRADE, (uint32_t)'u');
    bind_key(PT_ACTION_REPAIR, (uint32_t)'r');
    bind_key(PT_ACTION_SELL, KITTYKB_KEY_BACKSPACE);
    bind_key(PT_ACTION_CALL_WAVE, KITTYKB_KEY_TAB);
    bind_key(PT_ACTION_PAUSE, (uint32_t)'p');
    bind_key(PT_ACTION_ZOOM, (uint32_t)'z');
    bind_key(PT_ACTION_HELP, (uint32_t)'h');
    bind_key(PT_ACTION_HELP, (uint32_t)'?');
    bind_key(PT_ACTION_MUTE, (uint32_t)'m');
    bind_key(PT_ACTION_SPEED, (uint32_t)'f');
    bind_key(PT_ACTION_INTEL, (uint32_t)'i');
    bind_button(PT_ACTION_INTEL, 6u);        /* Back / Select */

    /* Linux's conventional Xbox-compatible button order. Both common right
     * stick-click indices are accepted because older js drivers expose one
     * fewer guide button. */
    bind_button(PT_ACTION_CONFIRM, 0u);       /* A */
    bind_button(PT_ACTION_CANCEL, 1u);        /* B */
    bind_button(PT_ACTION_CYCLE, 2u);         /* X */
    bind_button(PT_ACTION_UPGRADE, 3u);       /* Y */
    bind_button(PT_ACTION_GAMEPAD_WAVE, 5u);  /* RB: wave / combat speed */
    bind_button(PT_ACTION_PAUSE, 7u);         /* Start */
    bind_button(PT_ACTION_ZOOM, 9u);
    bind_button(PT_ACTION_ZOOM, 10u);         /* RS click */
    bind_button(PT_ACTION_MOVE_UP, 11u);      /* d-pad */
    bind_button(PT_ACTION_MOVE_DOWN, 12u);
    bind_button(PT_ACTION_MOVE_LEFT, 13u);
    bind_button(PT_ACTION_MOVE_RIGHT, 14u);

    bind_axis(PT_ACTION_MOVE_LEFT, 0u, -1);
    bind_axis(PT_ACTION_MOVE_RIGHT, 0u, 1);
    bind_axis(PT_ACTION_MOVE_UP, 1u, -1);
    bind_axis(PT_ACTION_MOVE_DOWN, 1u, 1);
    bind_axis(PT_ACTION_MOVE_LEFT, 6u, -1);
    bind_axis(PT_ACTION_MOVE_RIGHT, 6u, 1);
    bind_axis(PT_ACTION_MOVE_UP, 7u, -1);
    bind_axis(PT_ACTION_MOVE_DOWN, 7u, 1);
}

void pt_input_reset(pt_input_state *input)
{
    if (input != NULL) (void)memset(input, 0, sizeof *input);
    (void)memset(&ui, 0, sizeof ui);
    pt_settings_defaults(&ui.settings);
    (void)pt_save_load_settings(&ui.settings);
    (void)pt_save_load_records(&ui.records);
    ui.zoom_2x = ui.settings.zoom_default;
    configure_actions();
}

static void consume_action(pt_input_state *input,
                           const kittyin_action_event *action)
{
    bool repeatable;

    if (input == NULL || action == NULL ||
        action->phase == KITTYIN_ACTION_RELEASE)
        return;
    repeatable = action->action >= PT_ACTION_MOVE_UP &&
                 action->action <= PT_ACTION_MOVE_RIGHT;
    if (action->phase == KITTYIN_ACTION_REPEAT && !repeatable) return;

    switch (action->action) {
    case PT_ACTION_MOVE_UP:
        input->move_y = -1;
        break;
    case PT_ACTION_MOVE_DOWN:
        input->move_y = 1;
        break;
    case PT_ACTION_MOVE_LEFT:
        input->move_x = -1;
        break;
    case PT_ACTION_MOVE_RIGHT:
        input->move_x = 1;
        break;
    case PT_ACTION_CONFIRM:
        input->confirm = true;
        break;
    case PT_ACTION_CANCEL:
        input->cancel = true;
        break;
    case PT_ACTION_CYCLE:
        input->cycle_mode = true;
        break;
    case PT_ACTION_UPGRADE:
        input->upgrade = true;
        break;
    case PT_ACTION_REPAIR:
        input->repair = true;
        break;
    case PT_ACTION_SELL:
        input->sell = true;
        break;
    case PT_ACTION_CALL_WAVE:
        input->call_wave = true;
        break;
    case PT_ACTION_PAUSE:
        input->pause = true;
        break;
    case PT_ACTION_ZOOM:
        input->zoom = true;
        break;
    case PT_ACTION_HELP: input->help = true; break;
    case PT_ACTION_MUTE: input->mute = true; break;
    case PT_ACTION_SPEED: input->speed = true; break;
    case PT_ACTION_INTEL: input->intel = true; break;
    case PT_ACTION_GAMEPAD_WAVE: input->gamepad_wave = true; break;
    default:
        break;
    }
}

static void consume_actions(pt_input_state *input,
                            const kittyin_action_event *actions,
                            size_t count)
{
    size_t index;

    for (index = 0u; index < count; ++index)
        consume_action(input, &actions[index]);
}

static void handle_mouse(pt_input_state *input,
                         const kittyin_mouse_event *mouse)
{
    if (input == NULL || mouse == NULL) return;
    if (mouse->action == (uint8_t)KITTYIN_MOUSE_WHEEL) {
        if (mouse->wheel_x != 0 || mouse->wheel_y != 0) input->zoom = true;
        return;
    }

    ui.pointer_x = mouse->x;
    ui.pointer_y = mouse->y;
    ui.pointer_pending = true;
    if (mouse->action != (uint8_t)KITTYIN_MOUSE_PRESS) return;
    if (mouse->button == 1u) {
        ui.primary_click = true;
        input->confirm = true;
    } else if (mouse->button == 3u) {
        ui.secondary_click = true;
        input->cancel = true;
    }
}

void pt_input_event(pt_input_state *input, const kittyin_event *event)
{
    kittyin_action_event actions[8];
    size_t count;

    if (input == NULL || event == NULL) return;
    if (event->kind == KITTYIN_EVENT_MOUSE) {
        handle_mouse(input, &event->data.mouse);
        return;
    }
    if (event->kind != KITTYIN_EVENT_KEY) return;
    count = kittyin_action_map_key(
        &action_map, &event->data.key, actions,
        sizeof actions / sizeof actions[0]);
    consume_actions(input, actions, count);
}

void pt_input_gamepad_event(pt_input_state *input,
                            const kittyin_gamepad_event *event)
{
    kittyin_action_event actions[8];
    size_t count;
    bool pressed;

    if (input == NULL || event == NULL) return;
    pressed = event->kind == KITTYIN_GAMEPAD_BUTTON && event->value != 0 &&
              !event->initial;
    if (event->kind == KITTYIN_GAMEPAD_BUTTON && event->control == 4u) {
        if (event->value != 0) {
            ui.left_bumper = true;
            ui.left_bumper_used = false;
        } else {
            if (ui.left_bumper && !ui.left_bumper_used)
                input->repair = true;
            ui.left_bumper = false;
            ui.left_bumper_used = false;
        }
    }

    count = kittyin_action_map_gamepad(
        &action_map, event, actions, sizeof actions / sizeof actions[0]);
    consume_actions(input, actions, count);

    /* LB + A is Sell. Clear the individual LB/A meanings when the chord is
     * completed in the same input batch. */
    if (pressed && event->control == 0u && ui.left_bumper) {
        input->confirm = false;
        input->sell = true;
        ui.left_bumper_used = true;
    }
}

static int clamp_int(int value, int minimum, int maximum)
{
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

static void resolve_cursor_pad(pt_game *game)
{
    game->cursor.x = (int8_t)clamp_int(
        (int)game->cursor.x, 0, PT_COLUMNS - 1);
    game->cursor.y = (int8_t)clamp_int(
        (int)game->cursor.y, 0, PT_ROWS - 1);
    game->cursor.pad =
        game->board.pad_index[pt_cell_index((int)game->cursor.x,
                                            (int)game->cursor.y)];
}

static void apply_pointer_to_board(pt_game *game)
{
    int x;
    int y;

    if (!ui.pointer_pending || game == NULL) return;
    x = clamp_int((int)ui.pointer_x, 0, PT_LOGICAL_WIDTH - 1);
    y = clamp_int((int)ui.pointer_y, 0, PT_LOGICAL_HEIGHT - 1);
    if (ui.panel != PT_UI_PANEL_NONE) return;
    if (y < PT_PLAYFIELD_HEIGHT &&
        (game->phase == PT_PHASE_BUILD || game->phase == PT_PHASE_WAVE)) {
        if (ui.zoom_2x) {
            int source_x, source_y;
            pt_input_zoom_origin(game, &source_x, &source_y);
            x = source_x + x / 2;
            y = source_y + y / 2;
        }
        game->cursor.x = (int8_t)(x / PT_CELL_PIXELS);
        game->cursor.y = (int8_t)(y / PT_CELL_PIXELS);
        resolve_cursor_pad(game);
    }
}

static int ui_row_height(void)
{
    /* hud.c measures the complete authored shop/command rows. Their longest
     * required row selects scale 1 at every 1..3 preference, so pointer
     * geometry must follow the fitted 18px row rather than the preference.
     */
    return 18;
}

static void apply_pointer_to_controls(pt_game *game, pt_input_state *input)
{
    int x;
    int y;
    int row_height;
    int first_row;
    int visual_row;
    size_t item;

    if (game == NULL || input == NULL) return;
    x = clamp_int((int)ui.pointer_x, 0, PT_LOGICAL_WIDTH - 1);
    y = clamp_int((int)ui.pointer_y, 0, PT_LOGICAL_HEIGHT - 1);
    row_height = ui_row_height();

    /* Hover previews a weapon without spending funds or opening a panel. */
    if (!ui.primary_click) {
        first_row = 31 + 5 + row_height;
        if (ui.pointer_pending && ui.panel == PT_UI_PANEL_BUILD &&
            (game->phase == PT_PHASE_BUILD || game->phase == PT_PHASE_WAVE) &&
            x >= 8 && x < 472 && y >= first_row &&
            y < first_row + row_height * PT_ROLE_COUNT)
            ui.focus.selected = (size_t)((y - first_row) / row_height);
        return;
    }

    if (game->phase == PT_PHASE_PAUSE) {
        if (x >= 8 && x < 472 && y >= 78 && y < 78 + PT_PAUSE_MENU_COUNT * row_height)
            ui.pause_focus.selected = (size_t)((y - 78) / row_height);
        else input->confirm = false;
        return;
    }
    if (game->phase == PT_PHASE_INTEL) {
        input->confirm = false;
        if (y >= PT_INTEL_NAV_Y && y < PT_INTEL_NAV_Y + 24) {
            if (x >= 16 && x < 160) input->move_x = -1;
            else if (x >= 176 && x < 320) input->move_x = 1;
            else if (x >= 336 && x < 464) input->cancel = true;
        }
        return;
    }

    if ((game->phase == PT_PHASE_BUILD || game->phase == PT_PHASE_WAVE) &&
        y >= PT_PLAYFIELD_HEIGHT) {
        input->confirm = false;
        if (y >= PT_PLAYFIELD_HEIGHT + PT_HUD_STATUS_HEIGHT) {
            if (pt_hud_preview_pick(game, x, y, &ui.intel_kind)) input->intel = true;
            return;
        }
        if (y >= PT_HUD_ACTION_Y && x >= PT_HUD_HELP_X && x < 472)
            input->help = true;
        else if (y >= PT_HUD_ACTION_Y && x >= PT_HUD_ACTION_X &&
                 x < PT_HUD_HELP_X - 8)
            input->confirm = ui.panel == PT_UI_PANEL_NONE;
        else if (y < PT_HUD_ACTION_Y && x >= PT_HUD_ACTION_X &&
                 game->phase == PT_PHASE_BUILD)
            input->call_wave = true;
        else if (y < PT_HUD_ACTION_Y && x >= PT_HUD_SPEED_X &&
                 game->phase == PT_PHASE_WAVE)
            input->speed = true;
        return;
    }
    if (game->phase == PT_PHASE_MAP_SELECT) {
        input->confirm = false;
        if (y >= PT_MAP_TABS_Y && y < PT_MAP_TABS_Y + 24 && x >= 8 && x < 472) {
            size_t map = (size_t)((x - 8) / PT_MAP_TAB_WIDTH);
            if (map < PT_MAP_COUNT && (x - 8) % PT_MAP_TAB_WIDTH < PT_MAP_TAB_WIDTH - 8)
                ui.map_focus.selected = map;
        } else if (y >= PT_MAP_DEPLOY_Y && y < PT_MAP_DEPLOY_Y + 24) {
            if (x >= 16 && x < 320) input->confirm = true;
            else if (x >= 336 && x < 464) input->cancel = true;
        }
        return;
    }
    if (game->phase == PT_PHASE_CAMPAIGN_SELECT) {
        input->confirm = false;
        if (x < 8 || x >= 472 || y < 75 ||
            y >= 75 + row_height * PT_CAMPAIGN_COUNT) return;
        first_row = 70 + 5;
        visual_row = (y - first_row) / row_height;
        item = ui.focus.first_visible +
               (size_t)(visual_row < 0 ? 0 : visual_row);
        if (item < PT_CAMPAIGN_COUNT &&
            pt_input_campaign_is_unlocked((uint8_t)item)) {
            ui.focus.selected = item;
            input->confirm = true;
        } else {
            if (!game->headless) pt_audio_cue(PT_CUE_UI_INVALID);
        }
        return;
    }
    if (ui.panel == PT_UI_PANEL_BUILD) {
        input->confirm = false;
        first_row = 31 + 5 + row_height;
        if (x < 8 || x >= 472 || y < first_row ||
            y >= first_row + row_height * PT_ROLE_COUNT) return;
        visual_row = (y - first_row) / row_height;
        item = ui.focus.first_visible + (size_t)visual_row;
        if (item < PT_ROLE_COUNT) {
            ui.focus.selected = item;
            input->confirm = true;
        }
        return;
    }
    if (ui.panel == PT_UI_PANEL_INSPECTOR) {
        input->confirm = false;
        first_row = PT_INSPECTOR_Y + 5;
        if (x < 8 || x >= 472 || y < first_row ||
            y >= first_row + row_height * PT_INSPECT_COUNT) return;
        visual_row = (y - first_row) / row_height;
        item = ui.focus.first_visible +
               (size_t)(visual_row < 0 ? 0 : visual_row);
        if (item < PT_INSPECT_COUNT) {
            ui.focus.selected = item;
            input->confirm = true;
        }
    }
}

static void build_enabled(const pt_game *game, bool enabled[PT_ROLE_COUNT])
{
    const pt_campaign_def *campaign;
    size_t index;

    if (enabled == NULL) return;
    campaign = game != NULL ? pt_game_campaign(game) : NULL;
    for (index = 0u; index < PT_ROLE_COUNT; ++index) {
        const pt_fixture_def *fixture =
            game != NULL ? pt_fixture_def_at(game->campaign,
                                             (uint16_t)index) : NULL;
        enabled[index] = campaign != NULL && fixture != NULL &&
                         index < (size_t)campaign->fixture_count &&
                         fixture->tiers[0].cost <=
                             (uint32_t)(game->economy.currency < 0 ?
                                            0 : game->economy.currency);
    }
}

static uint32_t fixture_repair_cost(const pt_fixture *fixture)
{
    int32_t missing;

    if (fixture == NULL || fixture->integrity >= fixture->integrity_max)
        return 0u;
    missing = fixture->integrity_max - fixture->integrity;
    if (missing <= 0) return 0u;
    return ((uint32_t)missing +
            (uint32_t)PT_REPAIR_INTEGRITY_PER_UNIT - 1u) /
           (uint32_t)PT_REPAIR_INTEGRITY_PER_UNIT;
}

static void inspector_enabled(const pt_game *game,
                              bool enabled[PT_INSPECT_COUNT])
{
    pt_fixture *fixture;
    const pt_fixture_def *definition;
    uint32_t currency;
    uint32_t repair;
    size_t index;

    for (index = 0u; index < PT_INSPECT_COUNT; ++index)
        enabled[index] = false;
    if (game == NULL || game->cursor.pad == 0u) return;
    fixture = pt_fixture_at_pad((pt_game *)game, game->cursor.pad);
    if (fixture == NULL) return;
    definition = pt_fixture_def_at(game->campaign, fixture->kind);
    if (definition == NULL) return;
    currency = (uint32_t)(game->economy.currency < 0 ?
                              0 : game->economy.currency);
    enabled[PT_INSPECT_UPGRADE] =
        fixture->tier + 1u < PT_MAX_TIER &&
        definition->tiers[fixture->tier + 1u].cost <= currency;
    repair = fixture_repair_cost(fixture);
    enabled[PT_INSPECT_REPAIR] = repair > 0u && repair <= currency;
    enabled[PT_INSPECT_SELL] = true;
    for (index = PT_INSPECT_FIRST; index < PT_INSPECT_COUNT; ++index)
        enabled[index] = true;
}

static size_t focus_page_size(size_t item_count)
{
    /* Both owned panels fit every item after HUD width fitting. Keeping the
     * complete list visible also keeps keyboard, gamepad, and mouse focus in
     * the same coordinate space.
     */
    return item_count;
}

static void open_panel(pt_game *game)
{
    pt_fixture *fixture;

    if (game == NULL || game->cursor.pad == 0u) return;
    fixture = pt_fixture_at_pad(game, game->cursor.pad);
    if (fixture == NULL) {
        ui.panel = PT_UI_PANEL_BUILD;
        kilix_ui_focus_init(&ui.focus, PT_ROLE_COUNT,
                            focus_page_size(PT_ROLE_COUNT));
    } else {
        ui.panel = PT_UI_PANEL_INSPECTOR;
        kilix_ui_focus_init(&ui.focus, PT_INSPECT_COUNT,
                            focus_page_size(PT_INSPECT_COUNT));
    }
}

static kilix_ui_action focus_action(const pt_input_state *input)
{
    if (input->move_y < 0 || input->move_x < 0)
        return KILIX_UI_ACTION_UP;
    if (input->move_y > 0 || input->move_x > 0)
        return KILIX_UI_ACTION_DOWN;
    return KILIX_UI_ACTION_NONE;
}

static void move_panel_focus(const pt_input_state *input)
{
    kilix_ui_action action = focus_action(input);

    if (action == KILIX_UI_ACTION_NONE) return;
    /* Every row can be inspected. Affordability gates activation only, so
     * opening an inspector never jumps to Sell when an upgrade is unavailable. */
    (void)kilix_ui_focus_apply(&ui.focus, action, NULL);
}

static void move_board_cursor(pt_game *game, const pt_input_state *input)
{
    int x = (int)game->cursor.x + (int)input->move_x;
    int y = (int)game->cursor.y + (int)input->move_y;

    game->cursor.x = (int8_t)clamp_int(x, 0, PT_COLUMNS - 1);
    game->cursor.y = (int8_t)clamp_int(y, 0, PT_ROWS - 1);
    resolve_cursor_pad(game);
    if (ui.zoom_2x && (input->move_x || input->move_y)) center_zoom(game);
}

static void activate_inspector(pt_game *game)
{
    pt_fixture *fixture;

    if (game == NULL || game->cursor.pad == 0u) return;
    fixture = pt_fixture_at_pad(game, game->cursor.pad);
    if (fixture == NULL) {
        ui.panel = PT_UI_PANEL_NONE;
        return;
    }
    switch (ui.focus.selected) {
    case PT_INSPECT_UPGRADE:
        (void)pt_fixture_upgrade(game, game->cursor.pad);
        break;
    case PT_INSPECT_REPAIR:
        (void)pt_fixture_repair(game, game->cursor.pad);
        break;
    case PT_INSPECT_SELL:
        if (pt_fixture_sell(game, game->cursor.pad))
            ui.panel = PT_UI_PANEL_NONE;
        break;
    case PT_INSPECT_FIRST:
    case PT_INSPECT_LAST:
    case PT_INSPECT_STRONGEST:
    case PT_INSPECT_CLOSEST:
        fixture->mode =
            (uint8_t)(ui.focus.selected - PT_INSPECT_FIRST);
        break;
    default:
        break;
    }
}

static void activate_panel(pt_game *game)
{
    bool enabled[PT_ROLE_COUNT];

    if (ui.panel == PT_UI_PANEL_BUILD) {
        build_enabled(game, enabled);
        if (kilix_ui_focus_accepts(
                &ui.focus, KILIX_UI_ACTION_ACCEPT, enabled) &&
            ui.focus.selected <= (size_t)UINT16_MAX &&
            pt_fixture_place(game, game->cursor.pad,
                             (uint16_t)ui.focus.selected)) {
            ui.panel = PT_UI_PANEL_NONE;
            open_panel(game);
        }
    } else if (ui.panel == PT_UI_PANEL_INSPECTOR) {
        inspector_enabled(game, enabled);
        if (kilix_ui_focus_accepts(
                &ui.focus, KILIX_UI_ACTION_ACCEPT, enabled))
            activate_inspector(game);
    }
}

static void direct_fixture_actions(pt_game *game,
                                   const pt_input_state *input)
{
    if (game->cursor.pad == 0u) return;
    if (input->upgrade)
        (void)pt_fixture_upgrade(game, game->cursor.pad);
    if (input->repair)
        (void)pt_fixture_repair(game, game->cursor.pad);
    if (input->cycle_mode)
        pt_fixture_cycle_mode(game, game->cursor.pad);
    if (input->sell && pt_fixture_sell(game, game->cursor.pad))
        ui.panel = PT_UI_PANEL_NONE;
}

static void note_outcome(pt_game *game)
{
    bool cleared;

    if (game->phase != PT_PHASE_VICTORY &&
        game->phase != PT_PHASE_DEFEAT)
        return;
    if (game->outcome_recorded) return;
    cleared = game->phase == PT_PHASE_VICTORY;
    pt_save_note_run(&ui.records, game, cleared);
    (void)pt_save_store_records(&ui.records);
    game->outcome_recorded = true;
}

static void begin_campaign_select(void)
{
    bool enabled[PT_CAMPAIGN_COUNT];
    size_t index;

    for (index = 0u; index < PT_CAMPAIGN_COUNT; ++index)
        enabled[index] = pt_input_campaign_is_unlocked((uint8_t)index);
    kilix_ui_focus_init(&ui.focus, PT_CAMPAIGN_COUNT, PT_CAMPAIGN_COUNT);
    (void)kilix_ui_focus_set_items(
        &ui.focus, PT_CAMPAIGN_COUNT, enabled);
    ui.panel = PT_UI_PANEL_NONE;
}

static void open_pause_menu(pt_game *game)
{
    ui.pause_return = game->phase;
    kilix_ui_focus_init(&ui.pause_focus, PT_PAUSE_MENU_COUNT, PT_PAUSE_MENU_COUNT);
    pt_game_set_phase(game, PT_PHASE_PAUSE);
}

static void apply_title(pt_game *game, pt_input_state *input)
{
    if (input->confirm) {
        begin_campaign_select();
        pt_game_set_phase(game, PT_PHASE_CAMPAIGN_SELECT);
    } else if (input->cancel) {
        open_pause_menu(game);
    }
}

static void apply_campaign_select(pt_game *game, pt_input_state *input)
{
    bool enabled[PT_CAMPAIGN_COUNT];
    size_t index;
    kilix_ui_action action;

    for (index = 0u; index < PT_CAMPAIGN_COUNT; ++index)
        enabled[index] = pt_input_campaign_is_unlocked((uint8_t)index);
    (void)kilix_ui_focus_set_items(
        &ui.focus, PT_CAMPAIGN_COUNT, enabled);
    action = focus_action(input);
    if (action != KILIX_UI_ACTION_NONE)
        (void)kilix_ui_focus_apply(&ui.focus, action, enabled);
    if (input->cancel) {
        pt_game_set_phase(game, PT_PHASE_TITLE);
    } else if (input->confirm &&
               kilix_ui_focus_accepts(
                   &ui.focus, KILIX_UI_ACTION_ACCEPT, enabled) &&
               ui.focus.selected < PT_CAMPAIGN_COUNT) {
        ui.selected_campaign = (uint8_t)ui.focus.selected;
        kilix_ui_focus_init(&ui.map_focus, PT_MAP_COUNT, PT_MAP_COUNT);
        ui.map_focus.selected = game->board.map;
        pt_game_set_phase(game, PT_PHASE_MAP_SELECT);
    }
}

static void apply_map_select(pt_game *game, pt_input_state *input)
{
    kilix_ui_action action = focus_action(input);
    if (action != KILIX_UI_ACTION_NONE)
        (void)kilix_ui_focus_apply(&ui.map_focus, action, NULL);
    if (input->cancel) {
        begin_campaign_select();
        ui.focus.selected = ui.selected_campaign;
        pt_game_set_phase(game, PT_PHASE_CAMPAIGN_SELECT);
    } else if (input->confirm && ui.map_focus.selected < PT_MAP_COUNT) {
        uint64_t seed = game->rng;
        pt_game_init_map(game, (uint8_t)ui.map_focus.selected, ui.selected_campaign, seed);
        center_zoom(game);
        ui.panel = PT_UI_PANEL_NONE;
        game->outcome_recorded = false;
    }
}

static void apply_play(pt_game *game, pt_input_state *input)
{
    if ((input->speed || input->gamepad_wave) && game->phase == PT_PHASE_WAVE)
        game->speed = game->speed == 2u ? 1u : 2u;
    if (input->pause) {
        open_pause_menu(game);
        return;
    }
    if (ui.panel != PT_UI_PANEL_NONE)
        move_panel_focus(input);
    else
        move_board_cursor(game, input);

    direct_fixture_actions(game, input);
    if (input->call_wave || input->gamepad_wave) pt_game_call_wave_early(game);
    if (input->cancel) {
        if (ui.panel != PT_UI_PANEL_NONE)
            ui.panel = PT_UI_PANEL_NONE;
        else {
            open_pause_menu(game);
        }
    } else if (input->confirm) {
        if (ui.panel == PT_UI_PANEL_NONE)
            open_panel(game);
        else
            activate_panel(game);
    }
}

static void apply_pause(pt_game *game, pt_input_state *input)
{
    bool enabled[PT_PAUSE_MENU_COUNT] = {true, true, true, true};
    kilix_ui_action action = focus_action(input);
    if (action != KILIX_UI_ACTION_NONE)
        (void)kilix_ui_focus_apply(&ui.pause_focus, action, enabled);
    if (!input->pause && !input->cancel && !input->confirm) return;
    if (input->confirm && ui.pause_focus.selected == 3u) {
        input->quit = true;
        return;
    }
    if (input->confirm && ui.pause_focus.selected == 1u) {
        uint8_t campaign = game->campaign;
        uint64_t seed = game->rng;
        pt_game_init_map(game, game->board.map, campaign, seed);
        center_zoom(game);
        ui.panel = PT_UI_PANEL_NONE;
        game->outcome_recorded = false;
        return;
    }
    if (input->confirm && ui.pause_focus.selected == 2u) {
        begin_campaign_select();
        pt_game_set_phase(game, PT_PHASE_CAMPAIGN_SELECT);
        return;
    }
    if (ui.pause_return != PT_PHASE_TITLE &&
        ui.pause_return != PT_PHASE_BUILD &&
        ui.pause_return != PT_PHASE_WAVE)
        ui.pause_return = game->wave.active ?
                          PT_PHASE_WAVE : PT_PHASE_BUILD;
    pt_game_set_phase(game, (pt_phase)ui.pause_return);
}

static void clear_transient(pt_input_state *input)
{
    bool quit = input->quit;

    (void)memset(input, 0, sizeof *input);
    input->quit = quit;
    ui.pointer_pending = false;
    ui.primary_click = false;
    ui.secondary_click = false;
}

pt_phase pt_input_help_return_phase(void)
{
    return (pt_phase)ui.help_return;
}

void pt_input_apply(pt_game *game, pt_input_state *input)
{
    kittyin_action_event repeats[8];
    size_t repeat_count;

    if (game == NULL || input == NULL) return;
    repeat_count = kittyin_action_map_advance(
        &action_map, 1000u / PT_TICK_HZ, repeats,
        sizeof repeats / sizeof repeats[0]);
    consume_actions(input, repeats, repeat_count);
    if (ui.zoom_2x && !ui.zoom_anchored) center_zoom(game);
    apply_pointer_to_board(game);
    apply_pointer_to_controls(game, input);
    if (input->zoom) {
        ui.zoom_2x = !ui.zoom_2x;
        center_zoom(game);
    }
    if (input->mute) pt_audio_set_muted(!pt_audio_is_muted());
    if (!game->headless) {
        if (input->move_x || input->move_y) pt_audio_cue(PT_CUE_UI_CURSOR);
        if (input->confirm) pt_audio_cue(PT_CUE_UI_CONFIRM);
        if (input->cancel) pt_audio_cue(PT_CUE_UI_CANCEL);
    }
    note_outcome(game);
    if (input->intel && (game->phase == PT_PHASE_BUILD || game->phase == PT_PHASE_WAVE)) {
        ui.intel_return = game->phase;
        if (!ui.primary_click) {
            uint16_t next = (uint16_t)(game->wave.index + (game->phase == PT_PHASE_WAVE ? 1u : 0u));
            const pt_wave_def *wave = pt_game_wave(game, next);
            ui.intel_kind = wave != NULL && wave->group_count > 0u ? wave->groups[0].type : 0u;
        }
        pt_game_set_phase(game, PT_PHASE_INTEL);
        clear_transient(input);
        return;
    }
    if (input->help) {
        if (game->phase == PT_PHASE_HELP)
            pt_game_set_phase(game, (pt_phase)ui.help_return);
        else {
            ui.help_return = game->phase;
            pt_game_set_phase(game, PT_PHASE_HELP);
        }
        clear_transient(input);
        return;
    }

    switch ((pt_phase)game->phase) {
    case PT_PHASE_TITLE:
        apply_title(game, input);
        break;
    case PT_PHASE_CAMPAIGN_SELECT:
        apply_campaign_select(game, input);
        break;
    case PT_PHASE_MAP_SELECT:
        apply_map_select(game, input);
        break;
    case PT_PHASE_BUILD:
    case PT_PHASE_WAVE:
        apply_play(game, input);
        break;
    case PT_PHASE_PAUSE:
        apply_pause(game, input);
        break;
    case PT_PHASE_HELP:
        if (input->confirm || input->cancel)
            pt_game_set_phase(game, (pt_phase)ui.help_return);
        break;
    case PT_PHASE_INTEL: {
        uint16_t count = pt_game_campaign(game)->unit_count;
        if (input->cancel || input->confirm || input->intel)
            pt_game_set_phase(game, (pt_phase)ui.intel_return);
        else if (input->pause) {
            pt_game_set_phase(game, (pt_phase)ui.intel_return);
            open_pause_menu(game);
        } else if (count > 0u) {
            if (input->move_x < 0 || input->move_y < 0)
                ui.intel_kind = ui.intel_kind == 0u ? (uint16_t)(count - 1u) :
                    (uint16_t)(ui.intel_kind - 1u);
            else if (input->move_x > 0 || input->move_y > 0)
                ui.intel_kind = (uint16_t)((ui.intel_kind + 1u) % count);
        }
        break;
    }
    case PT_PHASE_VICTORY:
    case PT_PHASE_DEFEAT:
        if (input->confirm || input->cancel) {
            ui.panel = PT_UI_PANEL_NONE;
            pt_game_set_phase(game, PT_PHASE_TITLE);
        }
        break;
    default:
        break;
    }
    clear_transient(input);
}
