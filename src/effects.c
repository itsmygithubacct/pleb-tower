/* Brief, bounded visual feedback. Dropping an old effect never affects combat. */
#include "pleb_tower.h"

#include <math.h>

void pt_effect_emit(pt_game *game, pt_effect_kind kind, float x, float y, float radius)
{
    if (game == NULL || kind >= PT_EFFECT_COUNT || !isfinite(x) ||
        !isfinite(y) || !isfinite(radius) || radius <= 0.0f) return;
    pt_effect *slot = &game->effects[0];
    for (size_t i = 0u; i < PT_MAX_EFFECTS; ++i) {
        if (game->effects[i].remaining <= 0.0f) {
            slot = &game->effects[i];
            break;
        }
        if (game->effects[i].remaining < slot->remaining) slot = &game->effects[i];
    }
    float duration = kind == PT_EFFECT_HIT ? 0.16f : 0.36f;
    *slot = (pt_effect){.x = x, .y = y, .radius = radius,
        .remaining = duration, .duration = duration, .kind = (uint8_t)kind};
}

void pt_effects_update(pt_game *game, double dt)
{
    if (game == NULL || !isfinite(dt) || dt <= 0.0) return;
    for (size_t i = 0u; i < PT_MAX_EFFECTS; ++i) {
        pt_effect *effect = &game->effects[i];
        if (effect->remaining <= 0.0f) continue;
        effect->remaining = dt >= (double)effect->remaining ? 0.0f :
            effect->remaining - (float)dt;
    }
}
