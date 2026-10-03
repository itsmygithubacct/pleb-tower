/* Decoy Beacon / Compliance Broadcast: share one unit's stopping power
 * between susceptible ground units inside the displayed coverage circle.
 * Legacy content calls this role `reroute`; it now slows forward movement.
 */
#include "pleb_tower.h"

#include <math.h>

void pt_decoy_update(pt_game *game)
{
    if (game == NULL) return;
    for (size_t u = 0u; u < PT_MAX_UNITS; ++u)
        game->units.slots[u].decoy_slow = 0.0f;

    for (size_t f = 0u; f < PT_MAX_FIXTURES; ++f) {
        const pt_fixture *fixture = &game->fixtures[f];
        if (!fixture->present || fixture->tier >= PT_MAX_TIER) continue;
        const pt_fixture_def *definition =
            pt_fixture_def_at(game->campaign, fixture->kind);
        if (definition == NULL || definition->role != PT_ROLE_REROUTE) continue;
        const pt_pad_def *pad = pt_game_pad(game, fixture->pad);
        if (pad == NULL) continue;
        float radius = pt_fixture_range(game, fixture, fixture->tier);
        if (!(radius > 0.0f) || !isfinite(radius)) continue;

        bool affected[PT_MAX_UNITS] = {false};
        size_t count = 0u;
        for (size_t u = 0u; u < PT_MAX_UNITS; ++u) {
            const pt_unit *unit = &game->units.slots[u];
            if (!unit->alive || !isfinite(unit->x) || !isfinite(unit->y)) continue;
            const pt_unit_def *enemy = pt_unit_def_at(game->campaign, unit->kind);
            if (enemy == NULL || enemy->air || enemy->hardened) continue;
            float dx = unit->x - ((float)pad->x + 0.5f);
            float dy = unit->y - ((float)pad->y + 0.5f);
            if (dx * dx + dy * dy > radius * radius) continue;
            affected[u] = true;
            ++count;
        }
        if (count == 0u) continue;
        float slow = 1.0f / (float)count;
        for (size_t u = 0u; u < PT_MAX_UNITS; ++u) {
            pt_unit *unit = &game->units.slots[u];
            /* Overlapping beacons apply the strongest slowdown rather than
             * adding up to a full stop for an entire crowd. */
            if (affected[u] && slow > unit->decoy_slow) unit->decoy_slow = slow;
        }
    }
}
