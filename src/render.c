/* Draw into the authored canvas, then fit it to the terminal. Pointer picking
 * uses exactly the same viewport; no simulation coordinates depend on size. */
#include "pleb_tower.h"

#include "kilix_top_down_soft.h"
#include "kilix_assets.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COLOUR_BACKDROP  0x1B1E24u
#define COLOUR_LANE      0x3A3F48u
#define COLOUR_PAD       0x4A5240u
#define COLOUR_PAD_HOVER 0x7FA05Au
#define COLOUR_FIXTURE   0xC8B27Au
#define COLOUR_UNIT      0xB4544Cu
#define COLOUR_UNIT_AIR  0xC98A3Cu
#define COLOUR_GOAL      0x6FA8DCu
#define COLOUR_SPAWN     0x8A5A9Fu
#define COLOUR_HUD       0x121418u

static ki_td_soft_renderer soft_storage;
static kilix_asset_image grounds[PT_MAP_COUNT];
static kilix_asset_image fixture_images[PT_CAMPAIGN_COUNT];
static kilix_asset_image unit_images[PT_CAMPAIGN_COUNT];

static void load_image(kilix_asset_image *image, const char *relative,
                       uint32_t width, uint32_t height)
{
    char path[4096];
    if (pt_asset_path(relative, path, sizeof path) &&
        kilix_asset_image_load_png(image, path, NULL) == KILIX_ASSET_OK &&
        (image->width != width || image->height != height))
        kilix_asset_image_clear(image);
}

static bool draw_sprite(ki_td_soft_renderer *soft,
                         const kilix_asset_image *atlas, unsigned int cell,
                         float x, float y, int size, float alpha)
{
    ki_td_view view = {0};
    ki_td_rgba8 image;
    ki_td_rgba8 sprite;
    if (!kilix_asset_image_is_valid(atlas) || cell >= 9u) return false;
    view.scale = 1.0f;
    view.logical_width = PT_LOGICAL_WIDTH;
    view.logical_height = PT_LOGICAL_HEIGHT;
    image = ki_td_rgba8_make(atlas->pixels, 96, 96);
    sprite = ki_td_rgba8_subimage(
        &image, (int)(cell % 3u) * 32, (int)(cell / 3u) * 32, 32, 32);
    ki_td_soft_rgba_resized(soft, &view, x, y, &sprite, size, size, alpha);
    return true;
}

bool pt_render_init(pt_renderer *renderer, int width, int height)
{
    if (!renderer) return false;
    memset(renderer, 0, sizeof *renderer);
    memset(&soft_storage, 0, sizeof soft_storage);
    if (!ki_td_soft_renderer_init(
            &soft_storage, PT_LOGICAL_WIDTH, PT_LOGICAL_HEIGHT)) return false;
    renderer->soft = &soft_storage;
    if (!pt_render_resize(renderer, width, height)) {
        pt_render_shutdown(renderer);
        return false;
    }
    for (uint8_t map = 0u; map < PT_MAP_COUNT; ++map)
        load_image(&grounds[map], pt_map(map)->backdrop, 480u, 240u);
    load_image(&fixture_images[0], "graphics/atlases/fixtures-holdout.png", 96u, 96u);
    load_image(&fixture_images[1], "graphics/atlases/fixtures-cordon.png", 96u, 96u);
    load_image(&unit_images[0], "graphics/atlases/units-array.png", 96u, 96u);
    load_image(&unit_images[1], "graphics/atlases/units-civilian.png", 96u, 96u);
    return true;
}

void pt_render_shutdown(pt_renderer *renderer)
{
    if (!renderer || !renderer->soft) return;
    ki_td_soft_renderer_destroy(renderer->soft);
    free(renderer->display);
    renderer->display = NULL;
    for (uint8_t map = 0u; map < PT_MAP_COUNT; ++map)
        kilix_asset_image_clear(&grounds[map]);
    for (size_t i = 0u; i < PT_CAMPAIGN_COUNT; ++i) {
        kilix_asset_image_clear(&fixture_images[i]);
        kilix_asset_image_clear(&unit_images[i]);
    }
    renderer->soft = NULL;
    renderer->rgba = NULL;
}

bool pt_render_resize(pt_renderer *renderer, int width, int height)
{
    uint8_t *display;
    float scale;
    if (!renderer || !renderer->soft || width <= 0 || height <= 0 ||
        width > 8192 || height > 8192) return false;
    display = calloc((size_t)width * (size_t)height, 4u);
    if (!display) return false;
    free(renderer->display);
    renderer->display = display;
    renderer->rgba = NULL;
    renderer->width = width;
    renderer->height = height;
    scale = fminf((float)width / PT_LOGICAL_WIDTH,
                  (float)height / PT_LOGICAL_HEIGHT);
    renderer->view_width = (int)((float)PT_LOGICAL_WIDTH * scale);
    renderer->view_height = (int)((float)PT_LOGICAL_HEIGHT * scale);
    if (renderer->view_width < 1) renderer->view_width = 1;
    if (renderer->view_height < 1) renderer->view_height = 1;
    renderer->view_x = (width - renderer->view_width) / 2;
    renderer->view_y = (height - renderer->view_height) / 2;
    return true;
}

bool pt_render_pointer(const pt_renderer *renderer, int x, int y,
                       int *logical_x, int *logical_y)
{
    if (!renderer || !logical_x || !logical_y ||
        renderer->view_width <= 0 || renderer->view_height <= 0 ||
        x < renderer->view_x || y < renderer->view_y ||
        x >= renderer->view_x + renderer->view_width ||
        y >= renderer->view_y + renderer->view_height) return false;
    *logical_x = (x - renderer->view_x) * PT_LOGICAL_WIDTH /
                 renderer->view_width;
    *logical_y = (y - renderer->view_y) * PT_LOGICAL_HEIGHT /
                 renderer->view_height;
    return true;
}

void pt_render_zoom_origin(const pt_game *game, int *x, int *y)
{
    *x = (int)game->cursor.x * PT_CELL_PIXELS +
          PT_CELL_PIXELS / 2 - PT_LOGICAL_WIDTH / 4;
    *y = (int)game->cursor.y * PT_CELL_PIXELS +
          PT_CELL_PIXELS / 2 - PT_PLAYFIELD_HEIGHT / 4;
    if (*x < 0) *x = 0;
    if (*y < 0) *y = 0;
    if (*x > PT_LOGICAL_WIDTH / 2) *x = PT_LOGICAL_WIDTH / 2;
    if (*y > PT_PLAYFIELD_HEIGHT / 2) *y = PT_PLAYFIELD_HEIGHT / 2;
}

static void outline(ki_td_soft_renderer *soft, float x, float y,
                     float size, uint32_t colour)
{
    ki_td_soft_line_px(soft, x, y, x + size, y, 1.0f, colour, 1.0f);
    ki_td_soft_line_px(soft, x, y, x, y + size, 1.0f, colour, 1.0f);
    ki_td_soft_line_px(soft, x + size, y, x + size, y + size, 1.0f, colour, 1.0f);
    ki_td_soft_line_px(soft, x, y + size, x + size, y + size, 1.0f, colour, 1.0f);
}

static void health_bar(ki_td_soft_renderer *soft, float x, float y,
                        int width, int32_t value, int32_t maximum,
                        uint32_t colour)
{
    if (maximum <= 0) return;
    float fraction = fminf(1.0f, fmaxf(0.0f, (float)value / (float)maximum));
    ki_td_soft_fill_rect_px(soft, x, y, (float)width, 3.0f, 0x10151du, 1.0f);
    ki_td_soft_fill_rect_px(soft, x, y, (float)width * fraction, 2.0f, colour, 1.0f);
}

static void draw_cell(ki_td_soft_renderer *soft, int x, int y, uint32_t rgb,
                      float alpha)
{
    ki_td_soft_fill_rect_px(soft,
                            (float)(x * PT_CELL_PIXELS),
                            (float)(y * PT_CELL_PIXELS),
                            (float)PT_CELL_PIXELS, (float)PT_CELL_PIXELS,
                            rgb, alpha);
}

void pt_render_unit_icon(pt_renderer *renderer, uint8_t campaign, uint16_t kind,
                          int x, int y, int size)
{
    const pt_unit_def *unit = pt_unit_def_at(campaign, kind);
    if (renderer == NULL || renderer->soft == NULL || unit == NULL || size <= 0) return;
    (void)draw_sprite(renderer->soft, &unit_images[campaign], unit->atlas_row,
        (float)x, (float)y, size, 1.0f);
}

static void draw_combat_effects(ki_td_soft_renderer *soft, const pt_game *game)
{
    static const uint32_t colours[PT_EFFECT_COUNT] = {
        0xffe3abu, 0xffb154u, 0x83d9ffu, 0xb4df7bu, 0xef8055u, 0x93e3ffu
    };
    if (pt_input_reduced_motion()) return;
    for (size_t i = 0u; i < PT_MAX_EFFECTS; ++i) {
        const pt_effect *effect = &game->effects[i];
        if (effect->remaining <= 0.0f || effect->duration <= 0.0f ||
            effect->kind >= PT_EFFECT_COUNT) continue;
        float fade = effect->remaining / effect->duration;
        float radius = effect->radius * PT_CELL_PIXELS * (1.0f - 0.75f * fade);
        float cx = effect->x * PT_CELL_PIXELS, cy = effect->y * PT_CELL_PIXELS;
        uint32_t colour = colours[effect->kind];
        if (effect->kind == PT_EFFECT_BLAST)
            ki_td_soft_fill_circle_px(soft, cx, cy, radius, colour, fade * 0.16f);
        if (effect->kind == PT_EFFECT_HIT || effect->kind == PT_EFFECT_DESTROY) {
            for (int ray = 0; ray < 6; ++ray) {
                float angle = (float)ray * 6.2831853f / 6.0f + 0.3f;
                float dx = cosf(angle), dy = sinf(angle);
                ki_td_soft_line_px(soft,
                    cx + dx * radius * 0.4f, cy + dy * radius * 0.4f,
                    cx + dx * radius, cy + dy * radius, 1.0f, colour, fade * 0.9f);
            }
        } else {
            for (int segment = 0; segment < 24; ++segment) {
                float a = (float)segment * 6.2831853f / 24.0f;
                float b = (float)(segment + 1) * 6.2831853f / 24.0f;
                ki_td_soft_line_px(soft, cx + cosf(a) * radius, cy + sinf(a) * radius,
                    cx + cosf(b) * radius, cy + sinf(b) * radius, 1.0f, colour, fade * 0.8f);
            }
            if (effect->kind == PT_EFFECT_STUN || effect->kind == PT_EFFECT_SHIELD) {
                ki_td_soft_line_px(soft, cx + 2, cy - 5, cx - 2, cy,
                    1.0f, colour, fade);
                ki_td_soft_line_px(soft, cx - 2, cy, cx + 2, cy,
                    1.0f, colour, fade);
                ki_td_soft_line_px(soft, cx + 2, cy, cx - 2, cy + 5,
                    1.0f, colour, fade);
            }
        }
    }
}

/* Exact interactive foundations stay aligned with picking and tower positions. */
static void draw_foundation(ki_td_soft_renderer *soft, float x, float y, float cell)
{
    float scale = cell / PT_CELL_PIXELS;
    ki_td_soft_fill_rect_px(soft, x + scale, y + 2 * scale,
        14 * scale, 14 * scale, 0x303639u, 0.85f);
    ki_td_soft_fill_rect_px(soft, x + scale, y + scale,
        13 * scale, 13 * scale, 0x999d8du, 1.0f);
    ki_td_soft_fill_rect_px(soft, x + 2 * scale, y + 2 * scale,
        11 * scale, 10 * scale, 0xb6b7a5u, 1.0f);
    for (int row = 0; row < 2; ++row)
        for (int col = 0; col < 2; ++col) {
            float bx = x + (col ? 11.0f : 2.0f) * scale;
            float by = y + (row ? 11.0f : 2.0f) * scale;
            ki_td_soft_fill_rect_px(soft, bx, by, 2 * scale, 2 * scale, 0xd1b96cu, 1.0f);
            ki_td_soft_fill_rect_px(soft, bx, by, scale, scale, 0x454947u, 1.0f);
        }
}

void pt_render_map_preview(pt_renderer *renderer, uint8_t map, uint8_t campaign,
                            int x, int y, int width)
{
    if (!renderer || !renderer->soft || map >= PT_MAP_COUNT) return;
    const pt_map_def *level = pt_map(map);
    const pt_campaign_def *operation = pt_campaign_on_map(map, campaign);
    ki_td_view view = {0};
    view.scale = 1.0f;
    view.logical_width = PT_LOGICAL_WIDTH;
    view.logical_height = PT_LOGICAL_HEIGHT;
    float cell = (float)width / PT_COLUMNS;
    if (kilix_asset_image_is_valid(&grounds[map])) {
        ki_td_rgba8 image = ki_td_rgba8_make(grounds[map].pixels, 480, 240);
        ki_td_soft_rgba_resized(renderer->soft, &view, (float)x, (float)y,
                                &image, width, width / 2, 1.0f);
    } else {
        for (int row = 0; row < PT_ROWS; ++row)
            for (int col = 0; col < PT_COLUMNS; ++col)
                ki_td_soft_fill_rect_px(renderer->soft,
                    (float)x + (float)col * cell, (float)y + (float)row * cell,
                    cell, cell, level->lane[row][col] ? COLOUR_LANE : COLOUR_BACKDROP, 1.0f);
    }
    if (level->runtime_foundations)
        for (uint8_t id = 1u; id <= level->pad_count; ++id) {
            const pt_pad_def *pad = pt_map_pad(map, id);
            draw_foundation(renderer->soft, (float)x + (float)pad->x * cell,
                             (float)y + (float)pad->y * cell, cell);
        }
    float sx = (float)x + ((float)operation->spawn_x + 0.5f) * cell;
    float sy = (float)y + ((float)operation->spawn_y + 0.5f) * cell;
    float gx = (float)x + ((float)operation->goal_x + 0.5f) * cell;
    float gy = (float)y + ((float)operation->goal_y + 0.5f) * cell;
    ki_td_soft_fill_rect_px(renderer->soft, sx - 3, sy - 3, 6, 6, COLOUR_SPAWN, 1.0f);
    ki_td_soft_fill_rect_px(renderer->soft, gx - 3, gy - 3, 6, 6, COLOUR_GOAL, 1.0f);
}

void pt_render_frame(pt_renderer *renderer, const pt_game *game, double alpha)
{
    (void)alpha;
    if (!renderer || !renderer->soft || !game) return;
    ki_td_soft_renderer *soft = renderer->soft;
    const pt_board *board = &game->board;
    const kilix_asset_image *ground = &grounds[board->map < PT_MAP_COUNT ? board->map : 0u];

    ki_td_soft_clear(soft, COLOUR_BACKDROP);
    if (kilix_asset_image_is_valid(ground)) {
        ki_td_rgba8 image = ki_td_rgba8_make(ground->pixels, 480, 240);
        ki_td_soft_rgba_px(soft, 0, 0, &image, 1.0f);
    }

    for (int y = 0; y < PT_ROWS; ++y) {
        for (int x = 0; x < PT_COLUMNS; ++x) {
            uint16_t cell = pt_cell_index(x, y);
            if (board->lane[cell]) {
                /* The backdrop's road is authored against this exact lane.
                 * Keep its asphalt, markings and curbs visible. */
                if (!kilix_asset_image_is_valid(ground))
                    draw_cell(soft, x, y, COLOUR_LANE, 1.0f);
                /* Chevrons show which end to defend in either campaign. */
                if (board->distance[cell] % 3u == 1u) {
                    float cx = (float)(x * PT_CELL_PIXELS + 8);
                    float cy = (float)(y * PT_CELL_PIXELS + 8);
                    float dx = (float)board->flow_x[cell];
                    float dy = (float)board->flow_y[cell];
                    ki_td_soft_line_px(soft, cx - dx * 2.0f - dy * 2.0f,
                        cy - dy * 2.0f + dx * 2.0f, cx + dx * 2.0f,
                        cy + dy * 2.0f, 1.0f, 0xe6d39bu, 0.65f);
                    ki_td_soft_line_px(soft, cx - dx * 2.0f + dy * 2.0f,
                        cy - dy * 2.0f - dx * 2.0f, cx + dx * 2.0f,
                        cy + dy * 2.0f, 1.0f, 0xe6d39bu, 0.65f);
                }
            }
            uint8_t pad = board->pad_index[cell];
            if (pad) {
                if (pt_map(board->map)->runtime_foundations)
                    draw_foundation(soft, (float)(x * PT_CELL_PIXELS),
                                     (float)(y * PT_CELL_PIXELS), PT_CELL_PIXELS);
                bool hovered = game->cursor.pad == pad;
                draw_cell(soft, x, y,
                          hovered ? COLOUR_PAD_HOVER : COLOUR_PAD,
                          kilix_asset_image_is_valid(ground) ?
                              (hovered ? 0.55f : 0.14f) : 1.0f);
                outline(soft, (float)(x * PT_CELL_PIXELS + 1),
                    (float)(y * PT_CELL_PIXELS + 1), 13.0f,
                    hovered ? 0xffe4a0u : 0xafbc83u);
            }
        }
    }

    /* Range is meaningful before buying as well as when inspecting. */
    if (game->cursor.pad != 0u) {
        const pt_pad_def *pad = pt_game_pad(game, game->cursor.pad);
        const pt_fixture *fixture = &game->fixtures[game->cursor.pad - 1u];
        const pt_fixture_def *def = pt_fixture_def_at(
            game->campaign, fixture->present ? fixture->kind : 0u);
        if (pad && def) {
            float range = pt_fixture_range(game, fixture,
                fixture->present ? fixture->tier : 0u);
            float cx = (float)(pad->x * PT_CELL_PIXELS + 8);
            float cy = (float)(pad->y * PT_CELL_PIXELS + 8);
            for (int segment = 0; segment < 48; ++segment) {
                float a = (float)segment * 6.2831853f / 48.0f;
                float b = (float)(segment + 1) * 6.2831853f / 48.0f;
                ki_td_soft_line_px(soft,
                    cx + cosf(a) * range * PT_CELL_PIXELS,
                    cy + sinf(a) * range * PT_CELL_PIXELS,
                    cx + cosf(b) * range * PT_CELL_PIXELS,
                    cy + sinf(b) * range * PT_CELL_PIXELS,
                    1.0f, 0xffe4a0u, 0.45f);
            }
        }
    }

    {
        int gx = board->goal_cell % PT_COLUMNS;
        int gy = board->goal_cell / PT_COLUMNS;
        int sx = board->spawn_cell % PT_COLUMNS;
        int sy = board->spawn_cell / PT_COLUMNS;
        const int marker_x[2] = {gx, sx}, marker_y[2] = {gy, sy};
        const uint32_t marker_colour[2] = {COLOUR_GOAL, COLOUR_SPAWN};
        for (int marker = 0; marker < 2; ++marker) {
            float cx = (float)(marker_x[marker] * PT_CELL_PIXELS + 8);
            float cy = (float)(marker_y[marker] * PT_CELL_PIXELS + 8);
            ki_td_soft_fill_circle_px(soft, cx, cy, 6, marker_colour[marker], 1.0f);
            ki_td_soft_fill_circle_px(soft, cx, cy, 4, COLOUR_LANE, 1.0f);
            ki_td_soft_fill_rect_px(soft, cx - 1, cy - 1, 3, 3, marker_colour[marker], 1.0f);
        }
    }

    for (size_t i = 0; i < PT_MAX_FIXTURES; ++i) {
        const pt_fixture *fixture = &game->fixtures[i];
        if (!fixture->present) continue;
        const pt_pad_def *pad = pt_game_pad(game, fixture->pad);
        if (!pad) continue;
        float damage = fixture->integrity_max > 0
            ? (float)fixture->integrity / (float)fixture->integrity_max
            : 1.0f;
        const pt_fixture_def *def = pt_fixture_def_at(game->campaign, fixture->kind);
        if (!def || !draw_sprite(soft, &fixture_images[game->campaign],
                def->atlas_row, (float)(pad->x * PT_CELL_PIXELS - 2),
                (float)(pad->y * PT_CELL_PIXELS - 4), 20,
                0.55f + 0.45f * damage))
            ki_td_soft_fill_rect_px(soft,
                                (float)(pad->x * PT_CELL_PIXELS) + 2.0f,
                                (float)(pad->y * PT_CELL_PIXELS) + 2.0f,
                                (float)PT_CELL_PIXELS - 4.0f,
                                (float)PT_CELL_PIXELS - 4.0f,
                                COLOUR_FIXTURE, 0.35f + 0.65f * damage);
        health_bar(soft, (float)(pad->x * PT_CELL_PIXELS),
            (float)(pad->y * PT_CELL_PIXELS + 15), 16,
            fixture->integrity, fixture->integrity_max,
            damage < 0.35f ? 0xff6655u : 0x7cce88u);
        for (uint8_t tier = 0u; tier <= fixture->tier; ++tier)
            ki_td_soft_fill_rect_px(soft,
                (float)(pad->x * PT_CELL_PIXELS + (int)tier * 4),
                (float)(pad->y * PT_CELL_PIXELS - 2), 3.0f, 2.0f,
                0xffd886u, 1.0f);
    }

    for (size_t i = 0; i < PT_MAX_UNITS; ++i) {
        const pt_unit *unit = &game->units.slots[i];
        if (!unit->alive) continue;
        const pt_unit_def *def = pt_unit_def_at(game->campaign, unit->kind);
        int size = def && def->mass >= 20u ? 26 :
                   def && def->mass >= 5u ? 22 : 16;
        float cx = unit->x * PT_CELL_PIXELS;
        float cy = unit->y * PT_CELL_PIXELS;
        float radius = def && def->mass >= 5u ? 7.0f : 4.0f;
        ki_td_soft_fill_ellipse_px(soft, cx, cy + 5.0f,
            (float)size * 0.4f, 3.0f, 0x000000u, 0.4f);
        if (!def || !draw_sprite(soft, &unit_images[game->campaign],
                def->atlas_row, cx - (float)size / 2.0f,
                cy - (float)size / 2.0f, size, 1.0f))
            ki_td_soft_fill_circle_px(soft,
                                  unit->x * (float)PT_CELL_PIXELS,
                                  unit->y * (float)PT_CELL_PIXELS,
                                  radius,
                                  (def && def->air) ? COLOUR_UNIT_AIR
                                                    : COLOUR_UNIT,
                                  1.0f);
        if (def) {
            health_bar(soft, cx - (float)size / 2.0f,
                cy - (float)size / 2.0f - 3.0f, size,
                unit->integrity, def->integrity, 0xeb8b67u);
            if (unit->shield > 0)
                health_bar(soft, cx - (float)size / 2.0f,
                    cy - (float)size / 2.0f - 5.0f, size,
                    unit->shield, def->shield, 0x83d9ffu);
        }
        if (unit->hold_remaining > 0.0f || unit->stun_remaining > 0.0f ||
            unit->decoy_slow > 0.0f)
            outline(soft, cx - 8.0f, cy - 8.0f, 16.0f,
                unit->stun_remaining > 0.0f ? 0x83d9ffu : 0xb4df7bu);
    }

    for (size_t i = 0u; i < PT_MAX_PROJECTILES; ++i) {
        const pt_projectile *p = &game->projectiles.slots[i];
        if (!p->alive) continue;
        uint32_t colour = p->splash_radius > 0.0f ? 0xff9453u : 0xffe7a4u;
        float dx = p->target_x - p->x, dy = p->target_y - p->y;
        float length = sqrtf(dx * dx + dy * dy);
        if (length > 0.001f)
            ki_td_soft_line_px(soft, p->x * PT_CELL_PIXELS - dx / length * 5.0f,
                p->y * PT_CELL_PIXELS - dy / length * 5.0f,
                p->x * PT_CELL_PIXELS, p->y * PT_CELL_PIXELS, 1.0f, colour, 0.6f);
        ki_td_soft_fill_circle_px(soft, p->x * PT_CELL_PIXELS,
            p->y * PT_CELL_PIXELS, p->splash_radius > 0.0f ? 2.5f : 1.5f,
            colour, 1.0f);
    }
    draw_combat_effects(soft, game);
    if (game->phase == PT_PHASE_BUILD || game->phase == PT_PHASE_WAVE)
        outline(soft, (float)(game->cursor.x * PT_CELL_PIXELS),
            (float)(game->cursor.y * PT_CELL_PIXELS), 15.0f, 0xffe4a0u);

    ki_td_soft_fill_rect_px(soft, 0.0f, (float)PT_PLAYFIELD_HEIGHT,
                            (float)PT_LOGICAL_WIDTH, (float)PT_HUD_HEIGHT,
                            COLOUR_HUD, 1.0f);
    pt_hud_draw(renderer, game);

    const uint8_t *logical = ki_td_soft_pack_rgba(soft);
    if (!logical || !renderer->display) return;
    for (int y = 0; y < renderer->height; ++y) {
        for (int x = 0; x < renderer->width; ++x) {
            uint8_t *dest = renderer->display +
                ((size_t)y * (size_t)renderer->width + (size_t)x) * 4u;
            int lx, ly;
            if (pt_render_pointer(renderer, x, y, &lx, &ly))
                memcpy(dest, logical +
                    ((size_t)ly * PT_LOGICAL_WIDTH + (size_t)lx) * 4u, 4u);
            else {
                dest[0] = 0x0b; dest[1] = 0x10; dest[2] = 0x14; dest[3] = 0xff;
            }
        }
    }
    renderer->rgba = renderer->display;
}

bool pt_render_write_ppm(const pt_renderer *renderer, const char *path)
{
    if (!renderer || !renderer->rgba || !path) return false;
    FILE *file = fopen(path, "wb");
    if (!file) return false;
    if (fprintf(file, "P6\n%d %d\n255\n", renderer->width,
                renderer->height) < 0) {
        fclose(file);
        return false;
    }
    size_t pixels = (size_t)renderer->width * (size_t)renderer->height;
    for (size_t i = 0; i < pixels; ++i) {
        if (fwrite(renderer->rgba + i * 4u, 1u, 3u, file) != 3u) {
            fclose(file);
            return false;
        }
    }
    return fclose(file) == 0;
}
