/* Board rendering. Programmer art through M4; the generated pack replaces
 * every colour here at M5 without changing the draw order. */
#include "pleb_tower.h"

#include "kilix_top_down_soft.h"

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

bool pt_render_init(pt_renderer *renderer, int width, int height)
{
    if (!renderer) return false;
    memset(renderer, 0, sizeof *renderer);
    memset(&soft_storage, 0, sizeof soft_storage);
    if (!ki_td_soft_renderer_init(&soft_storage, width, height)) return false;
    renderer->soft = &soft_storage;
    renderer->width = width;
    renderer->height = height;
    return true;
}

void pt_render_shutdown(pt_renderer *renderer)
{
    if (!renderer || !renderer->soft) return;
    ki_td_soft_renderer_destroy(renderer->soft);
    renderer->soft = NULL;
    renderer->rgba = NULL;
}

bool pt_render_resize(pt_renderer *renderer, int width, int height)
{
    if (!renderer || !renderer->soft) return false;
    if (!ki_td_soft_renderer_resize(renderer->soft, width, height))
        return false;
    renderer->width = width;
    renderer->height = height;
    return true;
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

void pt_render_frame(pt_renderer *renderer, const pt_game *game, double alpha)
{
    (void)alpha;
    if (!renderer || !renderer->soft || !game) return;
    ki_td_soft_renderer *soft = renderer->soft;
    const pt_board *board = &game->board;

    ki_td_soft_clear(soft, COLOUR_BACKDROP);

    for (int y = 0; y < PT_ROWS; ++y) {
        for (int x = 0; x < PT_COLUMNS; ++x) {
            uint16_t cell = pt_cell_index(x, y);
            if (board->lane[cell]) draw_cell(soft, x, y, COLOUR_LANE, 1.0f);
            uint8_t pad = board->pad_index[cell];
            if (pad) {
                bool hovered = game->cursor.pad == pad;
                draw_cell(soft, x, y,
                          hovered ? COLOUR_PAD_HOVER : COLOUR_PAD, 1.0f);
            }
        }
    }

    {
        int gx = board->goal_cell % PT_COLUMNS;
        int gy = board->goal_cell / PT_COLUMNS;
        int sx = board->spawn_cell % PT_COLUMNS;
        int sy = board->spawn_cell / PT_COLUMNS;
        draw_cell(soft, gx, gy, COLOUR_GOAL, 1.0f);
        draw_cell(soft, sx, sy, COLOUR_SPAWN, 1.0f);
    }

    for (size_t i = 0; i < PT_MAX_FIXTURES; ++i) {
        const pt_fixture *fixture = &game->fixtures[i];
        if (!fixture->present) continue;
        const pt_pad_def *pad = pt_pad(fixture->pad);
        if (!pad) continue;
        float damage = fixture->integrity_max > 0
            ? (float)fixture->integrity / (float)fixture->integrity_max
            : 1.0f;
        ki_td_soft_fill_rect_px(soft,
                                (float)(pad->x * PT_CELL_PIXELS) + 2.0f,
                                (float)(pad->y * PT_CELL_PIXELS) + 2.0f,
                                (float)PT_CELL_PIXELS - 4.0f,
                                (float)PT_CELL_PIXELS - 4.0f,
                                COLOUR_FIXTURE, 0.35f + 0.65f * damage);
    }

    for (size_t i = 0; i < PT_MAX_UNITS; ++i) {
        const pt_unit *unit = &game->units.slots[i];
        if (!unit->alive) continue;
        const pt_unit_def *def = pt_unit_def_at(game->campaign, unit->kind);
        float radius = def && def->mass >= 5u ? 7.0f : 4.0f;
        ki_td_soft_fill_circle_px(soft,
                                  unit->x * (float)PT_CELL_PIXELS,
                                  unit->y * (float)PT_CELL_PIXELS,
                                  radius,
                                  (def && def->air) ? COLOUR_UNIT_AIR
                                                    : COLOUR_UNIT,
                                  1.0f);
    }

    ki_td_soft_fill_rect_px(soft, 0.0f, (float)PT_PLAYFIELD_HEIGHT,
                            (float)PT_LOGICAL_WIDTH, (float)PT_HUD_HEIGHT,
                            COLOUR_HUD, 1.0f);
    pt_hud_draw(renderer, game);

    renderer->rgba = ki_td_soft_pack_rgba(soft);
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
