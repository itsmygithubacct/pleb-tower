/* The Decoy Beacon's gather effect.
 *
 * Why this exists: the `reroute` role was specified as route manipulation —
 * bias the cost field, rebuild, let traffic take a different way round. That is
 * sound on a branching map and a no-op on Maple Loop, whose lane is a single
 * non-branching path with no alternative for a bias to select. See DECISIONS.md
 * D-05.
 *
 * The role is now an area gather: units inside the beacon's radius are drawn
 * along the lane toward the beacon's anchor cell and linger there briefly.
 * That works on any topology, is distinct from `control` (which immobilises one
 * unit where it stands), and sets up the artillery role by bunching a cluster
 * onto a known cell.
 *
 * Units are never pulled off the lane. The anchor is the lane cell nearest the
 * beacon's pad, so a gathered unit stays somewhere the flow field can sample.
 */
#include "pleb_tower.h"

#include <math.h>

#define PT_GATHER_PULL_SPEED  1.4f   /* cells per second toward the anchor */
#define PT_GATHER_LINGER      0.35f  /* seconds of hold refreshed each tick */

/* Lane cell nearest a pad, resolved once per pad. */
static bool anchor_for_pad(const pt_board *board, const pt_pad_def *pad,
                           float *out_x, float *out_y)
{
    float best = 1e9f;
    bool found = false;
    for (int y = 0; y < PT_ROWS; ++y) {
        for (int x = 0; x < PT_COLUMNS; ++x) {
            if (!board->lane[pt_cell_index(x, y)]) continue;
            float dx = (float)x - (float)pad->x;
            float dy = (float)y - (float)pad->y;
            float d2 = dx * dx + dy * dy;
            if (d2 < best) {
                best = d2;
                *out_x = (float)x + 0.5f;
                *out_y = (float)y + 0.5f;
                found = true;
            }
        }
    }
    return found;
}

void pt_gather_update(pt_game *game, double dt)
{
    if (!game) return;

    for (size_t f = 0; f < PT_MAX_FIXTURES; ++f) {
        const pt_fixture *fixture = &game->fixtures[f];
        if (!fixture->present) continue;

        const pt_fixture_def *def =
            pt_fixture_def_at(game->campaign, fixture->kind);
        if (!def || def->role != PT_ROLE_REROUTE) continue;
        if (fixture->tier >= PT_MAX_TIER) continue;

        const pt_pad_def *pad = pt_pad(fixture->pad);
        if (!pad) continue;

        float radius = (float)def->tiers[fixture->tier].pull_cells;
        if (radius <= 0.0f) continue;

        float anchor_x = 0.0f, anchor_y = 0.0f;
        if (!anchor_for_pad(&game->board, pad, &anchor_x, &anchor_y)) continue;

        for (size_t u = 0; u < PT_MAX_UNITS; ++u) {
            pt_unit *unit = &game->units.slots[u];
            if (!unit->alive) continue;

            const pt_unit_def *udef =
                pt_unit_def_at(game->campaign, unit->kind);
            if (!udef) continue;
            /* Air units answer to nothing on the ground, and hardened frames
             * ignore the spoofed signature outright — the same counter that
             * defeats hold and stun. */
            if (udef->air || udef->hardened) continue;

            float dx = anchor_x - unit->x;
            float dy = anchor_y - unit->y;
            float distance = sqrtf(dx * dx + dy * dy);
            if (distance > radius || distance <= 0.001f) continue;

            float step = PT_GATHER_PULL_SPEED * (float)dt;
            if (step > distance) step = distance;
            unit->x += dx / distance * step;
            unit->y += dy / distance * step;

            /* Linger, so the cluster holds together long enough for splash to
             * land on it rather than dispersing the instant it forms. */
            if (unit->hold_remaining < PT_GATHER_LINGER)
                unit->hold_remaining = PT_GATHER_LINGER;
        }
    }
}
