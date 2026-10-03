/* HUD, menus, and game-state screens.
 *
 * Menus and meters are kilix-ui composites. The tiny generated panel skin is
 * a real nine-slice, so panels retain the same layout path as asset-backed
 * production skins without requiring M5 artwork.
 */
#include "pleb_tower.h"

#include "kilix_top_down_soft.h"
#include "kilix_ui.h"
#include "soft_raster.h"

#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define HUD_COLOUR_PANEL UINT32_C(0x121418)
#define HUD_COLOUR_BORDER UINT32_C(0x7fa05a)
#define HUD_COLOUR_TEXT UINT32_C(0xf3ead3)
#define HUD_COLOUR_MUTED UINT32_C(0x9ca3aa)
#define HUD_COLOUR_ACCENT UINT32_C(0xe8bd64)
#define HUD_COLOUR_GOOD UINT32_C(0x64bd7a)
#define HUD_COLOUR_DANGER UINT32_C(0xd85b52)
#define HUD_COLOUR_SCREEN UINT32_C(0x080a0d)

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

enum {
    HUD_PANEL_PADDING = 5,
    HUD_MENU_X = 8,
    HUD_BUILD_Y = 31,
    HUD_BUILD_WIDTH = 464,
    HUD_BUILD_HEIGHT = 207,
    HUD_INSPECTOR_Y = PT_INSPECTOR_Y,
    HUD_INSPECTOR_WIDTH = 464,
    HUD_INSPECTOR_HEIGHT = 207,
    HUD_PREVIEW_X = 3,
    HUD_PREVIEW_WIDTH = 474,
    HUD_SELECTED_X = 3,
    HUD_SELECTED_Y = PT_PLAYFIELD_HEIGHT + PT_HUD_STATUS_HEIGHT + PT_HUD_PREVIEW_HEIGHT,
    HUD_SELECTED_WIDTH = 474,
    HUD_SELECTED_HEIGHT = PT_HUD_SELECTION_HEIGHT - 2
};

typedef struct pt_hud_preview_item {
    uint16_t kind;
    uint16_t count;
    bool first_appearance;
} pt_hud_preview_item;

typedef struct pt_hud_preview_layout {
    int scale;
    int row_height;
    int rows;
    int height;
} pt_hud_preview_layout;

/* Shared presentation-state queries owned by input.c. */
unsigned int pt_input_ui_panel(void);
size_t pt_input_ui_focus(void);
size_t pt_input_pause_focus(void);
unsigned int pt_input_text_scale(void);
bool pt_input_zoom_enabled(void);
bool pt_input_campaign_is_unlocked(uint8_t campaign);
pt_phase pt_input_help_return_phase(void);

/* Pure query surfaces used by tests and by the renderer below. */
void pt_hud_build_enabled(const pt_game *game,
                          bool enabled[PT_ROLE_COUNT]);
size_t pt_hud_next_wave_preview(const pt_game *game,
                                pt_hud_preview_item *items,
                                size_t capacity);
bool pt_hud_next_wave_item_text(const pt_game *game, size_t item_index,
                                char *text, size_t capacity);
bool pt_hud_layout_audit(unsigned int text_scale);
bool pt_hud_menu_render_test(const char *path);

static const uint8_t panel_pixels[3u * 3u * 4u] = {
    0x7f, 0xa0, 0x5a, 0xff,  0x7f, 0xa0, 0x5a, 0xff,
    0x7f, 0xa0, 0x5a, 0xff,
    0x7f, 0xa0, 0x5a, 0xff,  0x12, 0x14, 0x18, 0xff,
    0x7f, 0xa0, 0x5a, 0xff,
    0x7f, 0xa0, 0x5a, 0xff,  0x7f, 0xa0, 0x5a, 0xff,
    0x7f, 0xa0, 0x5a, 0xff
};

static uint32_t zoom_snapshot[
    (size_t)PT_LOGICAL_WIDTH * (size_t)PT_PLAYFIELD_HEIGHT];

static const char *const target_mode_names[PT_TARGET_MODE_COUNT] = {
    "First", "Last", "Strongest", "Closest"
};

static ki_td_view logical_view(void)
{
    ki_td_view view;

    (void)memset(&view, 0, sizeof view);
    view.logical_width = PT_LOGICAL_WIDTH;
    view.logical_height = PT_LOGICAL_HEIGHT;
    view.scale = 1.0f;
    return view;
}

static const ki_td_nine_slice *panel_skin(void)
{
    static ki_td_nine_slice skin;
    static bool initialized;

    if (!initialized) {
        ki_td_rgba8 image =
            ki_td_rgba8_make(panel_pixels, 3, 3);
        initialized = ki_td_nine_slice_init(
            &skin, &image, 1, 1, 1, 1);
    }
    return initialized ? &skin : NULL;
}

static kilix_ui_style ui_style(void)
{
    kilix_ui_style style;
    unsigned int scale = pt_input_text_scale();

    kilix_ui_style_init(&style);
    style.panel_color = HUD_COLOUR_PANEL;
    style.border_color = HUD_COLOUR_BORDER;
    style.text_color = HUD_COLOUR_TEXT;
    style.muted_color = HUD_COLOUR_MUTED;
    style.accent_color = HUD_COLOUR_ACCENT;
    style.meter_color = HUD_COLOUR_GOOD;
    style.font_scale = (int)scale;
    style.row_height = 16 * (int)scale + 2;
    style.padding = 5;
    style.panel_alpha = 0.97f;
    return style;
}

static int clamp_text_scale(unsigned int scale)
{
    if (scale < 1u) return 1;
    if (scale > 3u) return 3;
    return (int)scale;
}

static void set_style_scale(kilix_ui_style *style, int scale)
{
    if (style == NULL) return;
    if (scale < 1) scale = 1;
    style->font_scale = scale;
    style->row_height = SR_FONT_H * scale + 2;
}

static int strings_scale_to_fit(const char *const *strings, size_t count,
                                int preferred_scale, int available_width)
{
    int scale;

    if (strings == NULL || available_width <= 0) return 0;
    if (preferred_scale < 1) preferred_scale = 1;
    if (preferred_scale > 3) preferred_scale = 3;
    for (scale = preferred_scale; scale >= 1; --scale) {
        size_t index;
        bool fits = true;

        for (index = 0u; index < count; ++index)
            if (strings[index] != NULL &&
                sr_text_width(strings[index], scale) > available_width) {
                fits = false;
                break;
            }
        if (fits) return scale;
    }
    return 0;
}

static bool fit_style_to_strings(kilix_ui_style *style,
                                 const char *const *strings, size_t count,
                                 int available_width)
{
    int scale;

    if (style == NULL) return false;
    scale = strings_scale_to_fit(
        strings, count, style->font_scale, available_width);
    if (scale == 0) return false;
    set_style_scale(style, scale);
    return true;
}

static int centered_text_scale(const char *text, int preferred_scale)
{
    const char *strings[1] = {text};
    int scale = strings_scale_to_fit(
        strings, 1u, preferred_scale, PT_LOGICAL_WIDTH - 16);

    return scale == 0 ? 1 : scale;
}

static void draw_text(pt_renderer *renderer, int x, int y,
                      const char *text, uint32_t colour, int scale)
{
    sr_canvas *canvas;

    if (renderer == NULL || renderer->soft == NULL || text == NULL) return;
    canvas = ki_td_soft_canvas(renderer->soft);
    if (canvas == NULL) return;
    sr_text(canvas, (float)x, (float)y, text, colour, 1.0f, scale);
}

static void draw_text_center(pt_renderer *renderer, int x, int y,
                             const char *text, uint32_t colour, int scale)
{
    sr_canvas *canvas;

    if (renderer == NULL || renderer->soft == NULL || text == NULL) return;
    canvas = ki_td_soft_canvas(renderer->soft);
    if (canvas == NULL) return;
    sr_text_center(canvas, (float)x, (float)y, text,
                   colour, 1.0f, scale);
}

static const pt_fixture *selected_fixture(const pt_game *game)
{
    size_t index;

    if (game == NULL || game->cursor.pad == 0u) return NULL;
    for (index = 0u; index < PT_MAX_FIXTURES; ++index)
        if (game->fixtures[index].present &&
            game->fixtures[index].pad == game->cursor.pad)
            return &game->fixtures[index];
    return NULL;
}

void pt_hud_build_enabled(const pt_game *game,
                          bool enabled[PT_ROLE_COUNT])
{
    const pt_campaign_def *campaign;
    uint32_t balance;
    size_t index;

    if (enabled == NULL) return;
    campaign = game != NULL ? pt_game_campaign(game) : NULL;
    balance = game != NULL && game->economy.currency > 0 ?
              (uint32_t)game->economy.currency : 0u;
    for (index = 0u; index < PT_ROLE_COUNT; ++index) {
        const pt_fixture_def *fixture =
            game != NULL ? pt_fixture_def_at(game->campaign,
                                             (uint16_t)index) : NULL;
        enabled[index] = campaign != NULL && fixture != NULL &&
                         index < (size_t)campaign->fixture_count &&
                         fixture->tiers[0].cost <= balance;
    }
}

static uint16_t preview_wave_index(const pt_game *game,
                                   const pt_campaign_def *campaign)
{
    uint16_t index;

    if (game == NULL || campaign == NULL || campaign->wave_count == 0u)
        return UINT16_MAX;
    index = game->wave.index;
    if (game->phase == PT_PHASE_WAVE ||
        (game->phase == PT_PHASE_PAUSE && game->wave.active)) {
        if (index + 1u >= campaign->wave_count) return UINT16_MAX;
        ++index;
    } else if (game->phase != PT_PHASE_BUILD &&
               game->phase != PT_PHASE_PAUSE) {
        return UINT16_MAX;
    }
    if (index >= campaign->wave_count) return UINT16_MAX;
    return index;
}

size_t pt_hud_next_wave_preview(const pt_game *game,
                                pt_hud_preview_item *items,
                                size_t capacity)
{
    const pt_campaign_def *campaign;
    const pt_wave_def *wave;
    uint16_t wave_index;
    size_t count = 0u;
    size_t group_index;

    if (game == NULL) return 0u;
    campaign = pt_game_campaign(game);
    wave_index = preview_wave_index(game, campaign);
    if (wave_index == UINT16_MAX) return 0u;
    wave = pt_game_wave(game, wave_index);
    if (wave == NULL) return 0u;

    for (group_index = 0u; group_index < wave->group_count; ++group_index) {
        const pt_wave_group *group = &wave->groups[group_index];
        size_t existing;

        for (existing = 0u; existing < count; ++existing)
            if (items != NULL && existing < capacity &&
                items[existing].kind == group->type)
                break;
        if (existing < count && items != NULL && existing < capacity) {
            uint32_t combined =
                (uint32_t)items[existing].count + (uint32_t)group->count;
            items[existing].count =
                (uint16_t)(combined > UINT16_MAX ? UINT16_MAX : combined);
            continue;
        }
        if (count < capacity && items != NULL) {
            items[count].kind = group->type;
            items[count].count = group->count;
            items[count].first_appearance =
                group->type < 32u &&
                (wave->first_appearance &
                 (UINT32_C(1) << group->type)) != 0u;
        }
        ++count;
    }
    return count;
}

static bool format_preview_item(const pt_game *game,
                                const pt_hud_preview_item *item,
                                char *text, size_t capacity)
{
    const pt_unit_def *unit;
    int length;

    if (game == NULL || item == NULL || text == NULL || capacity == 0u)
        return false;
    unit = pt_unit_def_at(game->campaign, item->kind);
    length = snprintf(text, capacity, "%s%s x%u",
                      item->first_appearance ? "!" : "",
                      unit != NULL ? unit->name : "?",
                      (unsigned int)item->count);
    return length >= 0 && (size_t)length < capacity;
}

bool pt_hud_next_wave_item_text(const pt_game *game, size_t item_index,
                                char *text, size_t capacity)
{
    pt_hud_preview_item items[PT_MAX_WAVE_GROUPS];
    size_t count;

    if (text != NULL && capacity != 0u) text[0] = '\0';
    (void)memset(items, 0, sizeof items);
    count = pt_hud_next_wave_preview(
        game, items, sizeof items / sizeof items[0]);
    if (item_index >= count ||
        item_index >= sizeof items / sizeof items[0])
        return false;
    return format_preview_item(
        game, &items[item_index], text, capacity);
}

static kilix_ui_focus draw_focus(size_t item_count, size_t selected,
                                 size_t page_size)
{
    kilix_ui_focus focus;

    kilix_ui_focus_init(&focus, item_count, page_size);
    if (item_count == 0u) return focus;
    if (selected >= item_count) selected = item_count - 1u;
    focus.selected = selected;
    if (page_size != 0u && selected >= page_size)
        focus.first_visible = selected - page_size + 1u;
    return focus;
}

static uint32_t repair_cost(const pt_fixture *fixture)
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

static uint32_t sell_refund(const pt_fixture *fixture)
{
    uint64_t product;

    if (fixture == NULL) return 0u;
    product = (uint64_t)fixture->invested *
              (uint64_t)PT_SELL_REFUND_PERCENT;
    product /= UINT64_C(100);
    return product > UINT32_MAX ? UINT32_MAX : (uint32_t)product;
}

static void draw_integrity_meter(pt_renderer *renderer,
                                 const pt_game *game,
                                 const ki_td_view *view)
{
    kilix_ui_style style = ui_style();
    float fraction = 0.0f;
    char label[32];

    style.font_scale = 1;
    if (game->economy.integrity_max > 0)
        fraction = (float)game->economy.integrity /
                   (float)game->economy.integrity_max;
    if (fraction < 0.25f) {
        style.meter_color = HUD_COLOUR_DANGER;
        (void)snprintf(label, sizeof label, "! INTEGRITY");
    } else {
        style.meter_color = HUD_COLOUR_GOOD;
        (void)snprintf(label, sizeof label, "INTEGRITY");
    }
    kilix_ui_draw_meter(renderer->soft, view,
                        (ki_td_rect){3, PT_PLAYFIELD_HEIGHT + 4, 143, 22},
                        &style, (float)game->economy.integrity,
                        (float)game->economy.integrity_max, label);
}

static pt_hud_preview_layout preview_layout(
    const pt_game *game, unsigned int preferred_scale,
    char text[PT_MAX_WAVE_GROUPS][64], pt_hud_preview_item *items,
    size_t *item_count)
{
    pt_hud_preview_layout layout;
    size_t count;
    size_t index;
    int content_width =
        HUD_PREVIEW_WIDTH - HUD_PANEL_PADDING * 2;
    int scale;
    int x;
    int right = HUD_PREVIEW_X + HUD_PREVIEW_WIDTH - HUD_PANEL_PADDING;

    (void)memset(&layout, 0, sizeof layout);
    if (text == NULL || items == NULL || item_count == NULL) return layout;
    (void)memset(items, 0,
                 PT_MAX_WAVE_GROUPS * sizeof items[0]);
    count = pt_hud_next_wave_preview(
        game, items, PT_MAX_WAVE_GROUPS);
    if (count > PT_MAX_WAVE_GROUPS) count = PT_MAX_WAVE_GROUPS;
    *item_count = count;
    for (index = 0u; index < count; ++index)
        if (!format_preview_item(
                game, &items[index], text[index], sizeof text[index]))
            return layout;

    for (scale = clamp_text_scale(preferred_scale);
         scale >= 1; --scale) {
        bool fits = true;

        layout.scale = scale;
        layout.row_height = SR_FONT_H * scale + 2;
        layout.rows = 1;
        x = HUD_PREVIEW_X + HUD_PANEL_PADDING +
            sr_text_width("NEXT [I]", scale) + 8;
        for (index = 0u; index < count; ++index) {
            int width = sr_text_width(text[index], scale) + 4;

            if (width > content_width) {
                fits = false;
                break;
            }
            if (x + width > right) {
                ++layout.rows;
                x = HUD_PREVIEW_X + HUD_PANEL_PADDING;
            }
            x += width + 4;
        }
        layout.height =
            HUD_PANEL_PADDING * 2 + layout.rows * layout.row_height;
        if (fits && layout.height <= PT_HUD_PREVIEW_HEIGHT) return layout;
    }
    (void)memset(&layout, 0, sizeof layout);
    return layout;
}

bool pt_hud_air_warning_text(const pt_game *game,
    char lines[PT_AIR_WARNING_LINES][PT_AIR_WARNING_LINE_CAPACITY])
{
    if (lines == NULL) return false;
    memset(lines, 0, PT_AIR_WARNING_LINES * PT_AIR_WARNING_LINE_CAPACITY);
    if (game == NULL) return false;
    pt_phase phase = game->phase == PT_PHASE_HELP ?
        pt_input_help_return_phase() : (pt_phase)game->phase;
    if (phase != PT_PHASE_BUILD && phase != PT_PHASE_WAVE) return false;
    const pt_campaign_def *campaign = pt_game_campaign(game);
    const pt_unit_def *air = NULL;
    uint16_t first_wave = 0u;
    for (; first_wave < campaign->wave_count && air == NULL; ++first_wave) {
        const pt_wave_def *wave = pt_game_wave(game, first_wave);
        for (uint16_t group = 0u; group < wave->group_count; ++group) {
            const pt_unit_def *unit = pt_unit_def_at(game->campaign, wave->groups[group].type);
            if (unit != NULL && unit->air && wave->groups[group].count > 0u) {
                air = unit;
                break;
            }
        }
    }
    /* first_wave is now the one-based number of the first air wave. Give a
     * full round's notice, then retain it while preparing that air wave. */
    if (air == NULL ||
        !((game->wave.index + 2u == first_wave) ||
          (phase == PT_PHASE_BUILD && game->wave.index + 1u == first_wave)))
        return false;
    const pt_fixture_def *antiair = NULL, *rapid = NULL;
    for (uint16_t kind = 0u; kind < campaign->fixture_count; ++kind) {
        const pt_fixture_def *fixture = pt_fixture_def_at(game->campaign, kind);
        if (fixture->role == PT_ROLE_ANTIAIR) antiair = fixture;
        if (fixture->role == PT_ROLE_RAPID) rapid = fixture;
    }
    (void)snprintf(lines[0], PT_AIR_WARNING_LINE_CAPACITY,
        "AIR ALERT: %s in wave %u", air->name, (unsigned int)first_wave);
    (void)snprintf(lines[1], PT_AIR_WARNING_LINE_CAPACITY,
        "They fly to the goal. Ground-only towers can't hit them.");
    if (antiair != NULL && rapid != NULL &&
        (rapid->tiers[PT_MAX_TIER - 1u].targets & PT_TARGETS_AIR) != 0u)
        (void)snprintf(lines[2], PT_AIR_WARNING_LINE_CAPACITY,
            "Use %s or Tier %u %s.", antiair->name, PT_MAX_TIER, rapid->name);
    else
        (void)snprintf(lines[2], PT_AIR_WARNING_LINE_CAPACITY,
            "Build %s to shoot them.", antiair != NULL ? antiair->name : "anti-air towers");
    return true;
}

static void draw_wave_preview(pt_renderer *renderer, const pt_game *game,
                              const ki_td_view *view)
{
    pt_hud_preview_item items[PT_MAX_WAVE_GROUPS];
    char text[PT_MAX_WAVE_GROUPS][64];
    pt_hud_preview_layout layout;
    kilix_ui_style style = ui_style();
    size_t count = 0u;
    size_t index;
    int top;
    int x;
    int y;
    int right = HUD_PREVIEW_X + HUD_PREVIEW_WIDTH - HUD_PANEL_PADDING;

    (void)memset(text, 0, sizeof text);
    layout = preview_layout(game, pt_input_text_scale(), text, items, &count);
    if (layout.scale == 0) return;
    set_style_scale(&style, layout.scale);
    top = PT_PLAYFIELD_HEIGHT + PT_HUD_STATUS_HEIGHT;
    kilix_ui_draw_panel(
        renderer->soft, view,
        (ki_td_rect){HUD_PREVIEW_X, top,
                     HUD_PREVIEW_WIDTH, PT_HUD_PREVIEW_HEIGHT - 2},
        &style, panel_skin());
    x = HUD_PREVIEW_X + HUD_PANEL_PADDING;
    y = top + HUD_PANEL_PADDING;
    draw_text(renderer, x, y, "NEXT [I]", HUD_COLOUR_MUTED, layout.scale);
    x += sr_text_width("NEXT [I]", layout.scale) + 8;
    if (count == 0u) {
        draw_text(renderer, x, y, "--", HUD_COLOUR_MUTED, layout.scale);
        return;
    }
    for (index = 0u; index < count; ++index) {
        uint32_t colour = items[index].first_appearance ?
                          HUD_COLOUR_ACCENT : HUD_COLOUR_TEXT;
        int width = sr_text_width(text[index], layout.scale) + 4;

        if (x + width > right) {
            x = HUD_PREVIEW_X + HUD_PANEL_PADDING;
            y += layout.row_height;
        }
        ki_td_soft_fill_rect_px(
            renderer->soft, (float)(x - 1), (float)(y - 1),
            (float)width, (float)(SR_FONT_H * layout.scale + 2),
            colour, items[index].first_appearance ? 0.28f : 0.10f);
        draw_text(renderer, x + 1, y, text[index], colour, layout.scale);
        x += width + 4;
    }
}

bool pt_hud_preview_pick(const pt_game *game, int px, int py, uint16_t *kind)
{
    if (game == NULL || kind == NULL ||
        (game->phase != PT_PHASE_BUILD && game->phase != PT_PHASE_WAVE)) return false;
    pt_hud_preview_item items[PT_MAX_WAVE_GROUPS];
    char text[PT_MAX_WAVE_GROUPS][64];
    size_t count = 0u;
    pt_hud_preview_layout layout = preview_layout(game, pt_input_text_scale(), text, items, &count);
    if (layout.scale == 0) return false;
    int x = HUD_PREVIEW_X + HUD_PANEL_PADDING;
    int y = PT_PLAYFIELD_HEIGHT + PT_HUD_STATUS_HEIGHT + HUD_PANEL_PADDING;
    int label_width = sr_text_width("NEXT [I]", layout.scale);
    if (px >= x && px < x + label_width && py >= y && py < y + SR_FONT_H * layout.scale) {
        *kind = count > 0u ? items[0].kind : 0u;
        return true;
    }
    x += label_width + 8;
    for (size_t i = 0u; i < count; ++i) {
        int width = sr_text_width(text[i], layout.scale) + 4;
        if (x + width > HUD_PREVIEW_X + HUD_PREVIEW_WIDTH - HUD_PANEL_PADDING) {
            x = HUD_PREVIEW_X + HUD_PANEL_PADDING;
            y += layout.row_height;
        }
        if (px >= x - 1 && px < x - 1 + width && py >= y - 1 &&
            py < y + SR_FONT_H * layout.scale + 1) {
            *kind = items[i].kind;
            return true;
        }
        x += width + 4;
    }
    return false;
}

static void draw_hud_strip(pt_renderer *renderer, const pt_game *game,
                           const ki_td_view *view)
{
    const pt_campaign_def *campaign = pt_game_campaign(game);
    const pt_wave_def *wave =
        pt_game_wave(game, game->wave.index);
    char currency[64];
    char wave_number[32];
    char warning[PT_AIR_WARNING_LINES][PT_AIR_WARNING_LINE_CAPACITY];
    bool show_warning = pt_hud_air_warning_text(game, warning);

    draw_integrity_meter(renderer, game, view);
    (void)snprintf(currency, sizeof currency, "%s %d",
                   campaign != NULL ? campaign->currency_name : "Funds",
                   game->economy.currency);
    draw_text(renderer, 151, PT_PLAYFIELD_HEIGHT + 1,
              currency, HUD_COLOUR_ACCENT, 1);
    (void)snprintf(wave_number, sizeof wave_number, "WAVE %u/%u%s",
                   (unsigned int)(game->wave.index + 1u),
                   campaign != NULL ?
                       (unsigned int)campaign->wave_count : 0u,
                   show_warning ? " AIR!" : "");
    draw_text(renderer, 151, PT_PLAYFIELD_HEIGHT + 14,
              wave_number, show_warning ? HUD_COLOUR_ACCENT : HUD_COLOUR_TEXT, 1);

    if (game->phase == PT_PHASE_BUILD) {
        char countdown[48];
        uint32_t bonus =
            pt_economy_early_call_bonus(game->wave.build_remaining);
        int seconds = game->wave.build_remaining > 0.0 ?
                      (int)ceil(game->wave.build_remaining) : 0;

        if (game->wave.awaiting_call)
            (void)snprintf(countdown, sizeof countdown, "READY [TAB] Start");
        else
            (void)snprintf(countdown, sizeof countdown,
                           "BUILD %ds  [TAB] +%u", seconds,
                           (unsigned int)bonus);
        draw_text(renderer, PT_HUD_ACTION_X, PT_PLAYFIELD_HEIGHT + 1,
                  countdown, HUD_COLOUR_ACCENT, 1);
    } else if (game->phase == PT_PHASE_WAVE && wave != NULL) {
        char progress[40];

        unsigned int incoming = wave->total_units > game->wave.spawned ?
            (unsigned int)(wave->total_units - game->wave.spawned) : 0u;
        (void)snprintf(progress, sizeof progress, "LIVE %u  IN %u",
                       (unsigned int)game->units.live, incoming);
        draw_text(renderer, PT_HUD_ACTION_X, PT_PLAYFIELD_HEIGHT + 1,
                  progress, HUD_COLOUR_TEXT, 1);
        draw_text(renderer, PT_HUD_SPEED_X, PT_PLAYFIELD_HEIGHT + 1,
            game->speed == 2u ? "[F]2x" : "[F]1x", HUD_COLOUR_ACCENT, 1);
    }
    draw_text(renderer, PT_HUD_ACTION_X, PT_HUD_ACTION_Y,
              selected_fixture(game) != NULL ? "[ENTER] Info" : "[ENTER] Build",
              HUD_COLOUR_MUTED, 1);
    draw_text(renderer, PT_HUD_HELP_X, PT_HUD_ACTION_Y,
              show_warning ? "[H] Air!" : "[H] Help",
              show_warning ? HUD_COLOUR_ACCENT : HUD_COLOUR_MUTED, 1);
    draw_wave_preview(renderer, game, view);
}

static void selected_summary(const pt_game *game, const pt_fixture *fixture,
                              char *text, size_t capacity)
{
    const pt_fixture_def *definition = pt_fixture_def_at(game->campaign, fixture->kind);
    const char *mode = fixture->mode < PT_TARGET_MODE_COUNT ?
        target_mode_names[fixture->mode] : "?";
    if (definition->tiers[fixture->tier].targets == PT_TARGETS_NONE) mode = "Area";
    (void)snprintf(text, capacity, "%s T%u  %s  Range %.1f  HP %d/%d",
        definition->name, (unsigned int)fixture->tier + 1u, mode,
        (double)pt_fixture_range(game, fixture, fixture->tier),
        fixture->integrity, fixture->integrity_max);
}

static void draw_selected_card(pt_renderer *renderer, const pt_game *game,
                               const ki_td_view *view)
{
    const pt_fixture *fixture = selected_fixture(game);
    const pt_fixture_def *definition;
    kilix_ui_style style;
    char detail[96];

    definition = fixture != NULL ? pt_fixture_def_at(game->campaign, fixture->kind) : NULL;
    if (definition != NULL && pt_input_ui_panel() == PT_UI_PANEL_INSPECTOR) {
        (void)snprintf(detail, sizeof detail, "%s  T%u  |  HP %d/%d",
            definition->name, (unsigned int)fixture->tier + 1u,
            fixture->integrity, fixture->integrity_max);
        ki_td_soft_fill_rect_px(renderer->soft, 8, 4, 464, 23, HUD_COLOUR_PANEL, 0.96f);
        draw_text(renderer, 14, 8, detail, HUD_COLOUR_ACCENT, 1);
    }
    style = ui_style();
    style.font_scale = 1;
    style.row_height = 18;
    kilix_ui_draw_panel(renderer->soft, view,
                        (ki_td_rect){HUD_SELECTED_X, HUD_SELECTED_Y,
                                     HUD_SELECTED_WIDTH,
                                     HUD_SELECTED_HEIGHT},
                        &style, panel_skin());
    if (definition != NULL)
        selected_summary(game, fixture, detail, sizeof detail);
    else if (game->cursor.pad != 0u)
        (void)snprintf(detail, sizeof detail, "Pad %u: empty. [ENTER] Build a weapon.",
            (unsigned int)game->cursor.pad);
    else if (game->wave.awaiting_call)
        (void)snprintf(detail, sizeof detail, "%s", game->campaign == 0u ?
            "Pick a green pad. Build two Rail Spikes. [H] Help" :
            "Pick a green pad. Build two Turrets. [H] Help");
    else
        (void)snprintf(detail, sizeof detail, "Select a pad to build or inspect. [ESC] Menu");
    draw_text(renderer, HUD_SELECTED_X + 5, HUD_SELECTED_Y + 3,
              detail, HUD_COLOUR_ACCENT, 1);
}

static void draw_build_menu(pt_renderer *renderer, const pt_game *game,
                            const ki_td_view *view)
{
    const pt_campaign_def *campaign = pt_game_campaign(game);
    kilix_ui_shop_item items[PT_ROLE_COUNT];
    char layout_text[PT_ROLE_COUNT + 1u][160];
    const char *layout_strings[PT_ROLE_COUNT + 1u];
    bool enabled[PT_ROLE_COUNT];
    kilix_ui_style style = ui_style();
    kilix_ui_focus focus;
    size_t page_size;
    size_t index;

    (void)memset(layout_text, 0, sizeof layout_text);
    pt_hud_build_enabled(game, enabled);
    (void)snprintf(layout_text[0], sizeof layout_text[0], "%s: %d",
                   campaign != NULL ? campaign->currency_name : "Funds",
                   game->economy.currency);
    layout_strings[0] = layout_text[0];
    for (index = 0u; index < PT_ROLE_COUNT; ++index) {
        const pt_fixture_def *definition =
            pt_fixture_def_at(game->campaign, (uint16_t)index);
        int owned = 0;
        size_t slot;
        uint32_t price = definition != NULL ?
                         definition->tiers[0].cost : 0u;

        for (slot = 0u; slot < PT_MAX_FIXTURES; ++slot)
            if (game->fixtures[slot].present &&
                game->fixtures[slot].kind == index)
                ++owned;
        items[index].name = definition != NULL ? definition->name : "?";
        items[index].price =
            price > (uint32_t)INT_MAX ? INT_MAX : (int)price;
        items[index].owned = owned;
        items[index].enabled = enabled[index];
        (void)snprintf(layout_text[index + 1u],
                       sizeof layout_text[index + 1u],
                       "%s  %d  owned %d", items[index].name,
                       items[index].price, items[index].owned);
        layout_strings[index + 1u] = layout_text[index + 1u];
    }
    (void)fit_style_to_strings(
        &style, layout_strings, PT_ROLE_COUNT + 1u,
        HUD_BUILD_WIDTH - style.padding - 12);
    page_size = (size_t)((HUD_BUILD_HEIGHT - style.padding * 2 -
                          style.row_height) / style.row_height);
    if (page_size < 1u) page_size = 1u;
    if (page_size > PT_ROLE_COUNT) page_size = PT_ROLE_COUNT;
    focus = draw_focus(
        PT_ROLE_COUNT, pt_input_ui_focus(), page_size);
    kilix_ui_draw_shop(
        renderer->soft, view,
        (ki_td_rect){HUD_MENU_X, HUD_BUILD_Y,
                     HUD_BUILD_WIDTH, HUD_BUILD_HEIGHT},
        &style, panel_skin(), &focus, items, PT_ROLE_COUNT,
        campaign != NULL ? campaign->currency_name : "Funds",
        game->economy.currency);
    char description[PT_DESCRIPTION_LINES][PT_DESCRIPTION_CAPACITY];
    pt_fixture preview = {0};
    preview.kind = (uint16_t)pt_input_ui_focus();
    if (pt_fixture_describe(game, &preview, false, description)) {
        ki_td_soft_line_px(renderer->soft, 14, 200, 466, 200, 1, HUD_COLOUR_BORDER, 0.7f);
        for (int line = 0; line < 2; ++line)
            draw_text(renderer, 14, 202 + line * 17, description[line],
                line == 0 ? HUD_COLOUR_ACCENT : HUD_COLOUR_TEXT, 1);
    }
}

static void draw_inspector(pt_renderer *renderer, const pt_game *game,
                           const ki_td_view *view)
{
    const pt_fixture *fixture = selected_fixture(game);
    const pt_fixture_def *definition;
    kilix_ui_command commands[PT_INSPECT_COUNT];
    bool enabled[PT_INSPECT_COUNT];
    char costs[PT_INSPECT_COUNT][48];
    char labels[PT_TARGET_MODE_COUNT][48];
    char layout_text[PT_INSPECT_COUNT][160];
    const char *layout_strings[PT_INSPECT_COUNT];
    kilix_ui_style style = ui_style();
    kilix_ui_focus focus;
    size_t page_size;
    size_t index;
    uint32_t currency =
        (uint32_t)(game->economy.currency < 0 ? 0 : game->economy.currency);

    if (fixture == NULL) return;
    definition = pt_fixture_def_at(game->campaign, fixture->kind);
    if (definition == NULL) return;
    (void)memset(commands, 0, sizeof commands);
    (void)memset(enabled, 0, sizeof enabled);
    (void)memset(costs, 0, sizeof costs);
    (void)memset(labels, 0, sizeof labels);
    (void)memset(layout_text, 0, sizeof layout_text);

    commands[PT_INSPECT_UPGRADE].key = "U";
    commands[PT_INSPECT_UPGRADE].label = "Upgrade";
    if (fixture->tier + 1u < PT_MAX_TIER) {
        uint32_t cost = definition->tiers[fixture->tier + 1u].cost;
        (void)snprintf(costs[PT_INSPECT_UPGRADE],
                       sizeof costs[PT_INSPECT_UPGRADE],
                       "cost %u  range %.1f > %.1f", (unsigned int)cost,
                       (double)pt_fixture_range(game, fixture, fixture->tier),
                       (double)pt_fixture_range(game, fixture, fixture->tier + 1u));
        enabled[PT_INSPECT_UPGRADE] = cost <= currency;
    } else {
        (void)snprintf(costs[PT_INSPECT_UPGRADE],
                       sizeof costs[PT_INSPECT_UPGRADE], "MAX TIER");
    }
    commands[PT_INSPECT_UPGRADE].cost = costs[PT_INSPECT_UPGRADE];

    commands[PT_INSPECT_REPAIR].key = "R";
    commands[PT_INSPECT_REPAIR].label = "Repair";
    {
        uint32_t cost = repair_cost(fixture);
        (void)snprintf(costs[PT_INSPECT_REPAIR],
                       sizeof costs[PT_INSPECT_REPAIR],
                       "cost %u", (unsigned int)cost);
        enabled[PT_INSPECT_REPAIR] = cost > 0u && cost <= currency;
    }
    commands[PT_INSPECT_REPAIR].cost = costs[PT_INSPECT_REPAIR];

    commands[PT_INSPECT_SELL].key = "BACKSPACE";
    commands[PT_INSPECT_SELL].label = "Sell";
    (void)snprintf(costs[PT_INSPECT_SELL],
                   sizeof costs[PT_INSPECT_SELL],
                   "refund %u", (unsigned int)sell_refund(fixture));
    commands[PT_INSPECT_SELL].cost = costs[PT_INSPECT_SELL];
    enabled[PT_INSPECT_SELL] = true;

    for (index = 0u; index < PT_TARGET_MODE_COUNT; ++index) {
        size_t command_index = PT_INSPECT_FIRST + index;
        (void)snprintf(labels[index], sizeof labels[index], "%s%s",
                       target_mode_names[index],
                       fixture->mode == index ? " [ACTIVE]" : "");
        commands[command_index].key = "T";
        commands[command_index].label = labels[index];
        commands[command_index].cost =
            fixture->mode == index ? "selected" : "target mode";
        enabled[command_index] = true;
    }
    for (index = 0u; index < PT_INSPECT_COUNT; ++index)
        commands[index].enabled = enabled[index];

    for (index = 0u; index < PT_INSPECT_COUNT; ++index) {
        (void)snprintf(layout_text[index], sizeof layout_text[index],
                       "[%s] %s%s%s",
                       commands[index].key != NULL ?
                           commands[index].key : "",
                       commands[index].label != NULL ?
                           commands[index].label : "",
                       commands[index].cost != NULL &&
                           commands[index].cost[0] != '\0' ? "  " : "",
                       commands[index].cost != NULL ?
                           commands[index].cost : "");
        layout_strings[index] = layout_text[index];
    }
    (void)fit_style_to_strings(
        &style, layout_strings, PT_INSPECT_COUNT,
        HUD_INSPECTOR_WIDTH - style.padding - 12);
    page_size = (size_t)((HUD_INSPECTOR_HEIGHT - style.padding * 2) /
                         style.row_height);
    if (page_size < 1u) page_size = 1u;
    if (page_size > PT_INSPECT_COUNT) page_size = PT_INSPECT_COUNT;
    focus = draw_focus(
        PT_INSPECT_COUNT, pt_input_ui_focus(), page_size);
    kilix_ui_draw_commands(
        renderer->soft, view,
        (ki_td_rect){HUD_MENU_X, HUD_INSPECTOR_Y,
                     HUD_INSPECTOR_WIDTH, HUD_INSPECTOR_HEIGHT},
        &style, panel_skin(), &focus, commands, PT_INSPECT_COUNT);
    char description[PT_DESCRIPTION_LINES][PT_DESCRIPTION_CAPACITY];
    if (pt_fixture_describe(game, fixture, true, description)) {
        ki_td_soft_line_px(renderer->soft, 14, 163, 466, 163, 1, HUD_COLOUR_BORDER, 0.7f);
        for (int line = 0; line < PT_DESCRIPTION_LINES; ++line)
            draw_text(renderer, 14, 166 + line * 17, description[line],
                line == 0 ? HUD_COLOUR_ACCENT : HUD_COLOUR_TEXT, 1);
    }
}

static void dim_playfield(pt_renderer *renderer)
{
    ki_td_soft_fill_rect_px(renderer->soft, 0.0f, 0.0f,
                            (float)PT_LOGICAL_WIDTH,
                            (float)PT_PLAYFIELD_HEIGHT,
                            HUD_COLOUR_SCREEN, 0.88f);
}

static void apply_cursor_zoom(pt_renderer *renderer, const pt_game *game)
{
    sr_canvas *canvas;
    int source_x;
    int source_y;
    int y;

    if (renderer == NULL || renderer->soft == NULL || game == NULL ||
        !renderer->zoom_2x ||
        (game->phase != PT_PHASE_BUILD &&
         game->phase != PT_PHASE_WAVE &&
         game->phase != PT_PHASE_PAUSE))
        return;
    canvas = ki_td_soft_canvas(renderer->soft);
    if (canvas == NULL || canvas->px == NULL ||
        canvas->w < PT_LOGICAL_WIDTH ||
        canvas->h < PT_PLAYFIELD_HEIGHT)
        return;

    for (y = 0; y < PT_PLAYFIELD_HEIGHT; ++y)
        (void)memcpy(
            zoom_snapshot + (size_t)y * (size_t)PT_LOGICAL_WIDTH,
            canvas->px + (size_t)y * (size_t)canvas->w,
            (size_t)PT_LOGICAL_WIDTH * sizeof zoom_snapshot[0]);

    pt_input_zoom_origin(game, &source_x, &source_y);

    for (y = 0; y < PT_PLAYFIELD_HEIGHT; ++y) {
        int x;
        int sampled_y = source_y + y / 2;
        for (x = 0; x < PT_LOGICAL_WIDTH; ++x) {
            int sampled_x = source_x + x / 2;
            canvas->px[(size_t)y * (size_t)canvas->w + (size_t)x] =
                zoom_snapshot[
                    (size_t)sampled_y * (size_t)PT_LOGICAL_WIDTH +
                    (size_t)sampled_x];
        }
    }
}

static void draw_title_screen(pt_renderer *renderer,
                              const ki_td_view *view)
{
    kilix_ui_style style = ui_style();
    const char *lines[3] = {
        "One block. Two sides. Hold the line.",
        "Every wave is shown before it arrives.",
        "Fixtures can be destroyed. Plan repairs."
    };
    const char *layout_strings[5] = {
        "PLEB TOWER",
        "One block. Two sides. Hold the line.",
        "Every wave is shown before it arrives.",
        "Fixtures can be destroyed. Plan repairs.",
        "[ENTER / A] Campaigns   [ESC] Menu"
    };

    /* kilix-ui's dialogue prompt reserves a fixed 16px at the panel bottom,
     * so dialogue composites must use the base font height to avoid a
     * vertically clipped prompt.
     */
    set_style_scale(&style, 1);
    (void)fit_style_to_strings(
        &style, layout_strings,
        sizeof layout_strings / sizeof layout_strings[0],
        464 - style.padding * 2);
    dim_playfield(renderer);
    kilix_ui_draw_dialogue(
        renderer->soft, view, (ki_td_rect){8, 39, 464, 162},
        &style, panel_skin(), NULL, "PLEB TOWER",
        lines, sizeof lines / sizeof lines[0],
        "[ENTER / A] Campaigns   [ESC] Menu");
    draw_text_center(renderer, PT_LOGICAL_WIDTH / 2, 216,
                     "[H] How to play   [M] Mute", HUD_COLOUR_ACCENT, 1);
}

static void draw_help_screen(pt_renderer *renderer, const pt_game *game,
                             const ki_td_view *view)
{
    const char *lines[] = {
        "Protect the blue hub. Leaks cost Integrity.",
        "Select a green pad; ENTER opens the shop.",
        "Arrows / WASD: move    ENTER / click: choose",
        "ESC: menu / back      P: pause    Z: zoom",
        "U: upgrade   R: repair   BACKSPACE: sell",
        "T: aim  TAB: wave  F: speed  I: intel  M: mute",
        "Two Rail Spikes at the lower-left hairpin.",
        "Drones: Jammers. Shooters: Floodlights.",
        "Workshops boost and repair nearby towers.",
        "First wave waits for TAB. Take your time."
    };
    kilix_ui_style style = ui_style();
    char warning[PT_AIR_WARNING_LINES][PT_AIR_WARNING_LINE_CAPACITY];
    bool show_warning = pt_hud_air_warning_text(game, warning);
    if (show_warning) {
        lines[6] = warning[0];
        lines[7] = warning[1];
        lines[8] = warning[2];
        lines[9] = "Briefing pauses the game. Close it to resume.";
    }
    set_style_scale(&style, 1);
    dim_playfield(renderer);
    kilix_ui_draw_panel(renderer->soft, view,
        (ki_td_rect){8, 4, 464, 232}, &style, panel_skin());
    draw_text_center(renderer, 240, 10, "HOW TO HOLD THE BLOCK",
                     HUD_COLOUR_ACCENT, 1);
    for (size_t i = 0u; i < sizeof lines / sizeof lines[0]; ++i)
        draw_text(renderer, 20, 34 + (int)i * 18, lines[i],
            show_warning && i == 6u ? HUD_COLOUR_ACCENT : HUD_COLOUR_TEXT, 1);
    draw_text_center(renderer, 240, 219,
        "[H / ENTER / ESC] Back", HUD_COLOUR_ACCENT, 1);
}

static void draw_intel_screen(pt_renderer *renderer, const pt_game *game,
                               const ki_td_view *view)
{
    uint16_t kind = pt_input_intel_kind();
    const pt_unit_def *unit = pt_unit_def_at(game->campaign, kind);
    char lines[PT_INTEL_LINES][PT_INTEL_LINE_CAPACITY];
    if (unit == NULL || !pt_unit_describe(game->campaign, kind, lines)) return;
    kilix_ui_style style = ui_style();
    set_style_scale(&style, 1);
    dim_playfield(renderer);
    kilix_ui_draw_panel(renderer->soft, view,
        (ki_td_rect){8, 4, 464, PT_LOGICAL_HEIGHT - 8}, &style, panel_skin());
    draw_text_center(renderer, 240, 12, "ENEMY FIELD GUIDE - PAUSED", HUD_COLOUR_MUTED, 1);
    pt_render_unit_icon(renderer, game->campaign, kind, 28, 36, 40);
    draw_text_center(renderer, 240, 38, unit->name, HUD_COLOUR_ACCENT,
        strings_scale_to_fit(&unit->name, 1u, 2, 432));
    draw_text_center(renderer, 240, 73, unit->air ? "AIR / FLIES OVER THE ROAD" :
        unit->hardened ? "GROUND / HARDENED" : "GROUND", HUD_COLOUR_MUTED, 1);
    for (int row = 0; row < PT_INTEL_LINES; ++row) {
        int y = 98 + row * 23;
        draw_text(renderer, 20, y, lines[row],
            row == 4 || row == 5 ? HUD_COLOUR_ACCENT : HUD_COLOUR_TEXT, 1);
    }
    char position[64];
    (void)snprintf(position, sizeof position, "Enemy %u / %u  |  Arrows or D-pad to browse",
        (unsigned int)kind + 1u, (unsigned int)pt_game_campaign(game)->unit_count);
    draw_text_center(renderer, 240, 274, position, HUD_COLOUR_MUTED, 1);
    const char *labels[] = {"< Previous", "Next >", "[ESC] Back"};
    const int xs[] = {16, 176, 336};
    const int widths[] = {144, 144, 128};
    for (int i = 0; i < 3; ++i) {
        kilix_ui_draw_panel(renderer->soft, view,
            (ki_td_rect){xs[i], PT_INTEL_NAV_Y, widths[i], 24}, &style, panel_skin());
        draw_text_center(renderer, xs[i] + widths[i] / 2, PT_INTEL_NAV_Y + 4,
            labels[i], HUD_COLOUR_ACCENT, 1);
    }
}

static void draw_campaign_screen(pt_renderer *renderer,
                                 const ki_td_view *view)
{
    const char *items[PT_CAMPAIGN_COUNT];
    char labels[PT_CAMPAIGN_COUNT][96];
    bool enabled[PT_CAMPAIGN_COUNT];
    kilix_ui_style style = ui_style();
    kilix_ui_focus focus;
    const char *layout_strings[PT_CAMPAIGN_COUNT + 1u];
    size_t index;

    for (index = 0u; index < PT_CAMPAIGN_COUNT; ++index) {
        const pt_campaign_def *campaign = pt_campaign((uint8_t)index);
        enabled[index] =
            pt_input_campaign_is_unlocked((uint8_t)index);
        (void)snprintf(labels[index], sizeof labels[index], "%s  [%s]",
                       campaign != NULL ? campaign->name : "?",
                       enabled[index] ? "OPEN" :
                           "LOCKED: clear HOLDOUT");
        items[index] = labels[index];
        layout_strings[index] = labels[index];
    }
    /* kilix-ui reserves one additional character per prompt while packing. */
    layout_strings[PT_CAMPAIGN_COUNT] =
        "[ENTER / A] Maps  [ESC / B] Back";
    (void)fit_style_to_strings(
        &style, layout_strings, PT_CAMPAIGN_COUNT + 1u,
        420);
    focus = draw_focus(
        PT_CAMPAIGN_COUNT, pt_input_ui_focus(), PT_CAMPAIGN_COUNT);
    dim_playfield(renderer);
    draw_text_center(renderer, PT_LOGICAL_WIDTH / 2, 14,
                     "SELECT CAMPAIGN", HUD_COLOUR_ACCENT,
                     centered_text_scale(
                         "SELECT CAMPAIGN",
                         clamp_text_scale(pt_input_text_scale())));
    kilix_ui_draw_list(
        renderer->soft, view, (ki_td_rect){8, 70, 464, 104},
        &style, panel_skin(), &focus, items, enabled, PT_CAMPAIGN_COUNT);
    {
        kilix_ui_prompt prompts[2] = {
            {"ENTER / A", "Maps", true},
            {"ESC / B", "Back", true}
        };
        kilix_ui_draw_prompts(
            renderer->soft, view, 30, 188, 420, &style,
            prompts, sizeof prompts / sizeof prompts[0]);
    }
}

static void draw_map_screen(pt_renderer *renderer, const ki_td_view *view)
{
    uint8_t map = pt_input_selected_map();
    uint8_t campaign = pt_input_selected_campaign();
    const pt_map_def *level = pt_map(map);
    const pt_records *records = pt_input_records();
    kilix_ui_style style = ui_style();
    char text[128];
    set_style_scale(&style, 1);
    ki_td_soft_fill_rect_px(renderer->soft, 0, 0, PT_LOGICAL_WIDTH,
                            PT_LOGICAL_HEIGHT, HUD_COLOUR_PANEL, 1.0f);
    (void)snprintf(text, sizeof text, "%s / SELECT MAP", pt_campaign(campaign)->name);
    draw_text_center(renderer, 240, 10, text, HUD_COLOUR_ACCENT, 1);
    for (uint8_t i = 0u; i < PT_MAP_COUNT; ++i) {
        int x = 8 + (int)i * PT_MAP_TAB_WIDTH;
        kilix_ui_draw_panel(renderer->soft, view,
            (ki_td_rect){x, PT_MAP_TABS_Y, PT_MAP_TAB_WIDTH - 8, 24}, &style, panel_skin());
        (void)snprintf(text, sizeof text, "%s%s", i == map ? "> " : "", pt_map(i)->name);
        draw_text_center(renderer, x + (PT_MAP_TAB_WIDTH - 8) / 2, PT_MAP_TABS_Y + 5,
                          text, i == map ? HUD_COLOUR_ACCENT : HUD_COLOUR_MUTED, 1);
    }
    pt_render_map_preview(renderer, map, campaign, 82, 64, 316);
    draw_text_center(renderer, 240, 230, level->description, HUD_COLOUR_TEXT, 1);
    (void)snprintf(text, sizeof text, "%u build pads  |  Purple: entry  |  Blue: defend",
                    (unsigned int)level->pad_count);
    draw_text_center(renderer, 240, 247, text, HUD_COLOUR_MUTED, 1);
    if (records->cleared[map][campaign]) {
        uint32_t seconds = records->fastest_clear_ms[map][campaign] / 1000u;
        (void)snprintf(text, sizeof text, "BEST  HP %u/20  Time %02u:%02u  Bank %u",
            (unsigned int)records->best_integrity[map][campaign],
            (unsigned int)(seconds / 60u), (unsigned int)(seconds % 60u),
            (unsigned int)records->best_unspent[map][campaign]);
    } else {
        (void)snprintf(text, sizeof text, "No clear yet  |  %u runs on this map",
                        (unsigned int)records->runs[map][campaign]);
    }
    draw_text_center(renderer, 240, 268, text, HUD_COLOUR_GOOD, 1);
    draw_text_center(renderer, 240, 285, "ARROWS / D-PAD: choose map", HUD_COLOUR_MUTED, 1);
    kilix_ui_draw_panel(renderer->soft, view,
        (ki_td_rect){16, PT_MAP_DEPLOY_Y, 304, 24}, &style, panel_skin());
    kilix_ui_draw_panel(renderer->soft, view,
        (ki_td_rect){336, PT_MAP_DEPLOY_Y, 128, 24}, &style, panel_skin());
    draw_text_center(renderer, 168, PT_MAP_DEPLOY_Y + 5, "[ENTER / A] Deploy", HUD_COLOUR_ACCENT, 1);
    draw_text_center(renderer, 400, PT_MAP_DEPLOY_Y + 5, "[ESC / B] Back", HUD_COLOUR_MUTED, 1);
}

static void draw_pause_screen(pt_renderer *renderer,
                              const ki_td_view *view)
{
    kilix_ui_style style = ui_style();
    const char *items[PT_PAUSE_MENU_COUNT] = {
        "Resume operation", "Restart campaign", "Choose campaign", "Exit game"
    };
    const char *layout_strings[5] = {
        "Resume operation",
        "Restart campaign", "Choose campaign", "Exit game",
        "[P / ESC] Resume  [ENTER / A] Choose"
    };
    kilix_ui_focus focus = draw_focus(PT_PAUSE_MENU_COUNT, pt_input_pause_focus(), PT_PAUSE_MENU_COUNT);
    bool enabled[PT_PAUSE_MENU_COUNT] = {true, true, true, true};

    set_style_scale(&style, 1);
    (void)fit_style_to_strings(
        &style, layout_strings,
        sizeof layout_strings / sizeof layout_strings[0], 420);
    dim_playfield(renderer);
    draw_text_center(renderer, PT_LOGICAL_WIDTH / 2, 38,
                     "PAUSED", HUD_COLOUR_ACCENT,
                     centered_text_scale(
                         "PAUSED",
                         clamp_text_scale(pt_input_text_scale())));
    kilix_ui_draw_list(
        renderer->soft, view, (ki_td_rect){8, 73, 464, 94},
        &style, panel_skin(), &focus, items, enabled, PT_PAUSE_MENU_COUNT);
    {
        kilix_ui_prompt prompts[2] = {
            {"P / ESC", "Resume", true}, {"ENTER / A", "Choose", true}
        };
        kilix_ui_draw_prompts(
            renderer->soft, view, 30, 169, 420, &style,
            prompts, sizeof prompts / sizeof prompts[0]);
    }
}

static void draw_outcome_screen(pt_renderer *renderer,
                                const pt_game *game,
                                const ki_td_view *view)
{
    kilix_ui_style style = ui_style();
    char operation[96];
    char integrity[64];
    char elapsed[64];
    char currency[64];
    const char *lines[4] = {operation, integrity, elapsed, currency};
    const char *layout_strings[5];
    const pt_campaign_def *campaign = pt_game_campaign(game);
    uint64_t total_seconds =
        game->elapsed > 0.0 ? (uint64_t)game->elapsed : UINT64_C(0);

    (void)snprintf(operation, sizeof operation, "%s / %s",
                   pt_map(game->board.map)->name, campaign->name);
    (void)snprintf(integrity, sizeof integrity,
                   "Integrity remaining: %d / %d",
                   game->economy.integrity, game->economy.integrity_max);
    (void)snprintf(elapsed, sizeof elapsed,
                   "Clear time: %02llu:%02llu",
                   (unsigned long long)(total_seconds / UINT64_C(60)),
                   (unsigned long long)(total_seconds % UINT64_C(60)));
    (void)snprintf(currency, sizeof currency,
                   "Unspent %s: %d",
                   campaign != NULL ? campaign->currency_name : "currency",
                   game->economy.currency);
    layout_strings[0] = game->phase == PT_PHASE_VICTORY ?
                        "VICTORY - SCORE" : "DEFEAT - SCORE";
    layout_strings[1] = integrity;
    layout_strings[2] = elapsed;
    layout_strings[3] = currency;
    layout_strings[4] = "[ENTER / A / ESC / B] Title";
    set_style_scale(&style, 1);
    (void)fit_style_to_strings(
        &style, layout_strings,
        sizeof layout_strings / sizeof layout_strings[0],
        464 - style.padding * 2);
    dim_playfield(renderer);
    kilix_ui_draw_dialogue(
        renderer->soft, view, (ki_td_rect){8, 38, 464, 172},
        &style, panel_skin(), NULL,
        game->phase == PT_PHASE_VICTORY ?
            "VICTORY - SCORE" : "DEFEAT - SCORE",
        lines, sizeof lines / sizeof lines[0],
        "[ENTER / A / ESC / B] Title");
}

static bool audit_string_group(const char *const *strings, size_t count,
                               int preferred_scale, int available_width)
{
    return strings_scale_to_fit(
               strings, count, preferred_scale, available_width) != 0;
}

bool pt_hud_layout_audit(unsigned int text_scale)
{
    static const char *const title_strings[] = {
        "PLEB TOWER",
        "One block. Two sides. Hold the line.",
        "Every wave is shown before it arrives.",
        "Fixtures can be destroyed. Plan repairs.",
        "[ENTER / A] Campaigns   [ESC] Menu"
    };
    static const char *const pause_strings[] = {
        "Resume operation",
        "[P / ESC / A] Resume "
    };
    static const char *const inspector_strings[] = {
        "[U] Upgrade  cost 4294967295  range 5.0 > 5.5",
        "[R] Repair  cost 4294967295",
        "[BACKSPACE] Sell  refund 4294967295",
        "[T] First [ACTIVE]  selected",
        "[T] Last [ACTIVE]  selected",
        "[T] Strongest [ACTIVE]  selected",
        "[T] Closest [ACTIVE]  selected"
    };
    char campaign_labels[PT_CAMPAIGN_COUNT][96];
    const char *campaign_strings[PT_CAMPAIGN_COUNT + 1u];
    int preferred;
    size_t campaign_index;

    if (text_scale < 1u || text_scale > 3u) return false;
    preferred = (int)text_scale;
    for (uint8_t map = 0u; map < PT_MAP_COUNT; ++map) {
        const pt_map_def *level = pt_map(map);
        if (sr_text_width(level->name, 1) > PT_MAP_TAB_WIDTH - 24 ||
            sr_text_width(level->description, 1) > PT_LOGICAL_WIDTH - 16)
            return false;
    }
    if (!audit_string_group(
            title_strings,
            sizeof title_strings / sizeof title_strings[0],
            1, 464 - HUD_PANEL_PADDING * 2) ||
        !audit_string_group(
            pause_strings,
            sizeof pause_strings / sizeof pause_strings[0],
            preferred, 420) ||
        !audit_string_group(
            inspector_strings,
            sizeof inspector_strings / sizeof inspector_strings[0],
            preferred,
            HUD_INSPECTOR_WIDTH - HUD_PANEL_PADDING - 12))
        return false;

    for (campaign_index = 0u;
         campaign_index < PT_CAMPAIGN_COUNT; ++campaign_index) {
        const pt_campaign_def *campaign =
            pt_campaign((uint8_t)campaign_index);

        if (campaign == NULL) return false;
        (void)snprintf(
            campaign_labels[campaign_index],
            sizeof campaign_labels[campaign_index],
            "%s  [LOCKED: clear HOLDOUT]", campaign->name);
        campaign_strings[campaign_index] =
            campaign_labels[campaign_index];
    }
    campaign_strings[PT_CAMPAIGN_COUNT] =
        "[ENTER / A] Deploy  [ESC / B] Back";
    if (!audit_string_group(
            campaign_strings, PT_CAMPAIGN_COUNT + 1u,
            preferred, 420))
        return false;
    {
        int campaign_heading =
            centered_text_scale("SELECT CAMPAIGN", preferred);
        int pause_heading =
            centered_text_scale("PAUSED", preferred);

        if (campaign_heading < 1 ||
            campaign_heading * SR_FONT_H > 70 - 14 ||
            pause_heading < 1 ||
            pause_heading * SR_FONT_H > 96 - 38)
            return false;
    }

    for (campaign_index = 0u;
         campaign_index < PT_CAMPAIGN_COUNT; ++campaign_index) {
        const pt_campaign_def *campaign =
            pt_campaign((uint8_t)campaign_index);
        pt_game game;
        char core_text[4][96];
        const char *core_strings[4];
        char shop_text[PT_ROLE_COUNT + 1u][160];
        const char *shop_strings[PT_ROLE_COUNT + 1u];
        char outcome_text[5][96];
        const char *outcome_strings[5];
        size_t index;

        if (campaign == NULL) return false;
        pt_game_init(
            &game, (uint8_t)campaign_index,
            UINT64_C(0x2c000000) + (uint64_t)campaign_index);
        (void)snprintf(core_text[0], sizeof core_text[0],
                       "! INTEGRITY %d/%d",
                       campaign->starting_integrity,
                       campaign->starting_integrity);
        (void)snprintf(core_text[1], sizeof core_text[1],
                       "%s %d", campaign->currency_name,
                       campaign->starting_currency);
        (void)snprintf(core_text[2], sizeof core_text[2],
                       "WAVE %u/%u AIR!", (unsigned int)campaign->wave_count,
                       (unsigned int)campaign->wave_count);
        (void)snprintf(core_text[3], sizeof core_text[3],
                       "BUILD 20s  [TAB] +%u",
                       (unsigned int)pt_economy_early_call_bonus(20.0));
        for (index = 0u; index < 4u; ++index)
            core_strings[index] = core_text[index];
        if (sr_text_width(core_strings[0], 1) > 140 ||
            sr_text_width(core_strings[1], 1) > 129 ||
            sr_text_width(core_strings[2], 1) > 129 ||
            sr_text_width(core_strings[3], 1) > 200)
            return false;

        (void)snprintf(shop_text[0], sizeof shop_text[0],
                       "%s: %d", campaign->currency_name, INT_MAX);
        shop_strings[0] = shop_text[0];
        for (index = 0u; index < PT_ROLE_COUNT; ++index) {
            const pt_fixture_def *fixture =
                pt_fixture_def_at((uint8_t)campaign_index,
                                  (uint16_t)index);
            size_t tier;
            size_t mode;

            if (fixture == NULL) return false;
            (void)snprintf(
                shop_text[index + 1u],
                sizeof shop_text[index + 1u],
                "%s  %d  owned %d", fixture->name, INT_MAX,
                PT_MAX_FIXTURES);
            shop_strings[index + 1u] = shop_text[index + 1u];
            for (tier = 0u; tier < PT_MAX_TIER; ++tier) {
                for (mode = 0u; mode < PT_TARGET_MODE_COUNT; ++mode) {
                    char detail[96];
                    const char *selected_strings[] = {detail};
                    pt_fixture selected = {
                        .kind = (uint16_t)index, .tier = (uint8_t)tier,
                        .mode = (uint8_t)mode, .present = 1u, .range_scale = 1.2f,
                        .integrity = fixture->tiers[tier].integrity,
                        .integrity_max = fixture->tiers[tier].integrity
                    };
                    selected_summary(&game, &selected, detail, sizeof detail);
                    if (!audit_string_group(
                            selected_strings, 1u, 1,
                            HUD_SELECTED_WIDTH - 10))
                        return false;
                }
            }
        }
        if (!audit_string_group(
                shop_strings, PT_ROLE_COUNT + 1u, preferred,
                HUD_BUILD_WIDTH - HUD_PANEL_PADDING - 12))
            return false;

        for (index = 0u; index < campaign->wave_count; ++index) {
            pt_hud_preview_item items[PT_MAX_WAVE_GROUPS];
            char preview_text[PT_MAX_WAVE_GROUPS][64];
            pt_hud_preview_layout layout;
            size_t preview_count = 0u;
            const pt_wave_def *wave =
                pt_wave_def_at((uint8_t)campaign_index,
                               (uint16_t)index);

            game.wave.index = (uint16_t)index;
            pt_game_set_phase(&game, PT_PHASE_BUILD);
            (void)memset(preview_text, 0, sizeof preview_text);
            layout = preview_layout(
                &game, text_scale, preview_text, items, &preview_count);
            if (wave == NULL || layout.scale < 1 ||
                layout.scale > preferred ||
                layout.rows < 1 ||
                layout.height > PT_HUD_PREVIEW_HEIGHT ||
                preview_count != wave->group_count)
                return false;
        }

        (void)snprintf(
            outcome_text[0], sizeof outcome_text[0],
            "VICTORY - SCORE");
        (void)snprintf(
            outcome_text[1], sizeof outcome_text[1],
            "Integrity remaining: %d / %d", INT_MIN, INT_MAX);
        (void)snprintf(
            outcome_text[2], sizeof outcome_text[2],
            "Clear time: %02llu:%02llu",
            (unsigned long long)(UINT64_MAX / UINT64_C(60)),
            (unsigned long long)(UINT64_MAX % UINT64_C(60)));
        (void)snprintf(
            outcome_text[3], sizeof outcome_text[3],
            "Unspent %s: %d", campaign->currency_name, INT_MIN);
        (void)snprintf(
            outcome_text[4], sizeof outcome_text[4],
            "[ENTER / A / ESC / B] Title");
        for (index = 0u; index < 5u; ++index)
            outcome_strings[index] = outcome_text[index];
        if (!audit_string_group(
                outcome_strings, 5u, 1,
                464 - HUD_PANEL_PADDING * 2))
            return false;
    }
    return true;
}

bool pt_hud_menu_render_test(const char *path)
{
    pt_game game;
    pt_input_state input;
    pt_renderer renderer;
    const pt_pad_def *pad;
    bool written;

    if (path == NULL || path[0] == '\0') return false;
    pt_game_init(&game, 0u, UINT64_C(0x2c4d454e55));
    game.headless = true;
    pt_input_reset(&input);
    if (pt_input_zoom_enabled()) {
        input.zoom = true;
        pt_input_apply(&game, &input);
    }
    pad = pt_pad(10u);
    if (pad == NULL || pad->id > UINT8_MAX) return false;
    game.cursor.x = (int8_t)pad->x;
    game.cursor.y = (int8_t)pad->y;
    game.cursor.pad = (uint8_t)pad->id;
    input.confirm = true;
    pt_input_apply(&game, &input);
    if (pt_input_ui_panel() != PT_UI_PANEL_BUILD) return false;
    if (!pt_render_init(
            &renderer, PT_LOGICAL_WIDTH, PT_LOGICAL_HEIGHT))
        return false;
    pt_render_frame(&renderer, &game, 0.0);
    written = pt_render_write_ppm(&renderer, path);
    pt_render_shutdown(&renderer);
    return written;
}

void pt_hud_draw(pt_renderer *renderer, const pt_game *game)
{
    ki_td_view view;

    if (renderer == NULL || renderer->soft == NULL || game == NULL) return;
    renderer->zoom_2x = pt_input_zoom_enabled();
    view = logical_view();
    apply_cursor_zoom(renderer, game);
    if (game->phase == PT_PHASE_BUILD ||
        game->phase == PT_PHASE_WAVE ||
        game->phase == PT_PHASE_PAUSE)
        draw_hud_strip(renderer, game, &view);

    if (game->phase == PT_PHASE_BUILD || game->phase == PT_PHASE_WAVE) {
        draw_selected_card(renderer, game, &view);
        if (pt_input_ui_panel() == PT_UI_PANEL_BUILD)
            draw_build_menu(renderer, game, &view);
        else if (pt_input_ui_panel() == PT_UI_PANEL_INSPECTOR)
            draw_inspector(renderer, game, &view);
    }

    switch ((pt_phase)game->phase) {
    case PT_PHASE_TITLE:
        draw_title_screen(renderer, &view);
        break;
    case PT_PHASE_HELP:
        draw_help_screen(renderer, game, &view);
        break;
    case PT_PHASE_INTEL:
        draw_intel_screen(renderer, game, &view);
        break;
    case PT_PHASE_CAMPAIGN_SELECT:
        draw_campaign_screen(renderer, &view);
        break;
    case PT_PHASE_MAP_SELECT:
        draw_map_screen(renderer, &view);
        break;
    case PT_PHASE_PAUSE:
        draw_pause_screen(renderer, &view);
        break;
    case PT_PHASE_VICTORY:
    case PT_PHASE_DEFEAT:
        draw_outcome_screen(renderer, game, &view);
        break;
    default:
        break;
    }
}
