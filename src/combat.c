/* Projectile simulation and the fixed seven-step damage pipeline. */
#include "pleb_tower.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static pt_unit *unit_with_serial(pt_game *game, uint16_t serial)
{
    for (size_t i = 0; i < PT_MAX_UNITS; ++i) {
        pt_unit *unit = &game->units.slots[i];
        if (unit->alive && unit->serial == serial) return unit;
    }
    return NULL;
}

static void retire_projectile(pt_projectile_pool *pool,
                              pt_projectile *projectile)
{
    if (!projectile->alive) return;
    projectile->alive = 0u;
    if (pool->live > 0u) --pool->live;
}

void pt_combat_reset(pt_game *game)
{
    if (!game) return;
    memset(&game->projectiles, 0, sizeof game->projectiles);
}

static uint8_t source_currency_tick(pt_game *game, uint16_t source_pad)
{
    pt_fixture *source;

    if (source_pad == 0u || source_pad > UINT8_MAX) return 0u;
    source = pt_fixture_at_pad(game, (uint8_t)source_pad);
    return source ? source->currency_tick : 0u;
}

static void spawn_hook_units(pt_game *game, const pt_hook_def *hook,
                             float x, float y, bool override_integrity)
{
    for (uint16_t child_index = 0u; child_index < hook->count;
         ++child_index) {
        pt_unit *child = pt_units_spawn(&game->units, hook->spawn, x, y);

        if (!child || !override_integrity || !(hook->value > 0.0f))
            continue;
        if (hook->value >= (float)INT32_MAX)
            child->integrity = INT32_MAX;
        else
            child->integrity = (int32_t)floorf(hook->value);
    }
}

static void kill_unit_with_tick(pt_game *game, pt_unit *unit,
                                uint8_t currency_tick)
{
    const pt_unit_def *def;
    pt_hook_def on_death;
    float x;
    float y;

    if (!game || !unit || !unit->alive) return;
    def = pt_unit_def_at(game->campaign, unit->kind);
    if (!def) {
        unit->alive = 0u;
        if (game->units.live > 0u) --game->units.live;
        return;
    }

    on_death = def->on_death;
    x = unit->x;
    y = unit->y;
    unit->integrity = 0;
    unit->shield = 0;
    unit->alive = 0u;
    unit->halted = 0u;
    if (game->units.live > 0u) --game->units.live;

    /* Step 7: base currency, then the one cached largest support tick. */
    pt_economy_award(game, def->currency);
    if (currency_tick > 0u)
        pt_economy_award(game, (uint32_t)currency_tick);

    if (on_death.active && on_death.count > 0u)
        spawn_hook_units(game, &on_death, x, y, true);
}

void pt_combat_kill_unit(pt_game *game, pt_unit *unit)
{
    kill_unit_with_tick(game, unit, 0u);
}

static void fire_threshold_hook(pt_game *game, pt_unit *unit,
                                const pt_unit_def *def,
                                int32_t previous_integrity)
{
    const pt_hook_def *threshold = &def->on_threshold;
    double boundary;

    if (!threshold->active || unit->threshold_fired) return;
    boundary = (double)def->integrity * (double)threshold->value;
    if ((double)previous_integrity > boundary &&
        (double)unit->integrity <= boundary) {
        unit->threshold_fired = 1u;
        spawn_hook_units(game, threshold, unit->x, unit->y, false);
    }
}

static int32_t apply_source_damage_scale(pt_game *game, int32_t damage,
                                         uint16_t source_pad)
{
    pt_fixture *source;
    const pt_fixture_def *source_def;
    const pt_tier_def *tier;
    float scaled;
    int32_t cached_damage;

    if (source_pad == 0u || source_pad > UINT8_MAX) return damage;
    source = pt_fixture_at_pad(game, (uint8_t)source_pad);
    if (!source || source->tier >= PT_MAX_TIER ||
        !(source->damage_scale > 0.0f))
        return damage;
    source_def = pt_fixture_def_at(game->campaign, source->kind);
    if (!source_def) return damage;
    tier = &source_def->tiers[source->tier];

    scaled = (float)tier->damage * source->damage_scale;
    if (scaled >= (float)INT32_MAX)
        cached_damage = INT32_MAX;
    else if (!(scaled > 0.0f))
        cached_damage = 0;
    else
        cached_damage = (int32_t)floorf(scaled);

    /* fixture.c currently snapshots the cached multiplier into projectiles.
     * Treat that canonical value as already resolved; direct callers pass the
     * tier base and are scaled here. This keeps step 2 idempotent across both
     * entry paths. */
    if (damage == cached_damage) return damage;

    scaled = (float)damage * source->damage_scale;
    if (scaled >= (float)INT32_MAX) return INT32_MAX;
    if (!(scaled > 0.0f)) return 0;
    return (int32_t)floorf(scaled);
}

static int32_t apply_damage_from(pt_game *game, pt_unit *unit,
                                 int32_t damage, uint8_t damage_type,
                                 uint16_t source_pad)
{
    const pt_unit_def *def;
    int32_t resolved;
    int32_t remaining;
    int32_t applied = 0;
    int32_t previous_integrity;

    if (!game || !unit || !unit->alive || damage <= 0 ||
        damage_type == PT_DAMAGE_NONE)
        return 0;
    def = pt_unit_def_at(game->campaign, unit->kind);
    if (!def) return 0;

    /* Steps 1–2: begin with tier damage and apply the firing fixture's one
     * cached largest support multiplier. */
    resolved = apply_source_damage_scale(game, damage, source_pad);
    /* Step 3: version 1.0 has no vulnerability marks. */

    /* Step 4: flat armour, except for pierce. */
    if (damage_type != PT_DAMAGE_PIERCE) {
        if (def->armor >= resolved)
            resolved = 1;
        else
            resolved -= def->armor;
    }

    /* Step 6's floor is established before routing the hit so armour and the
     * splash shield modifier can never reduce a positive hit to zero. */
    if (resolved < 1) resolved = 1;
    remaining = resolved;
    previous_integrity = unit->integrity;
    unit->shield_idle = 0.0f;

    /* Step 5: shield first. Splash has half effect while a shield is taking
     * the hit; any remainder from that shield-effective amount continues into
     * integrity during this same call. */
    if (unit->shield > 0) {
        int32_t shield_effect = remaining;
        int32_t absorbed;

        if (damage_type == PT_DAMAGE_SPLASH) {
            shield_effect /= 2;
            if (shield_effect < 1) shield_effect = 1;
        }
        absorbed = shield_effect < unit->shield
                       ? shield_effect
                       : unit->shield;
        unit->shield -= absorbed;
        applied += absorbed;
        remaining = shield_effect - absorbed;
    }

    if (remaining > 0 && unit->integrity > 0) {
        int32_t integrity_damage =
            remaining < unit->integrity ? remaining : unit->integrity;
        unit->integrity -= integrity_damage;
        applied += integrity_damage;
    }

    fire_threshold_hook(game, unit, def, previous_integrity);
    if (unit->integrity <= 0)
        kill_unit_with_tick(game, unit,
                            source_currency_tick(game, source_pad));
    return applied;
}

int32_t pt_combat_apply_damage(pt_game *game, pt_unit *unit,
                               int32_t damage, uint8_t damage_type,
                               uint16_t source_pad)
{
    return apply_damage_from(game, unit, damage, damage_type, source_pad);
}

void pt_combat_damage_fixture(pt_game *game, pt_fixture *fixture,
                              int32_t damage)
{
    uint8_t destroyed_pad;
    bool was_reroute = false;

    if (!game || !fixture || !fixture->present || damage <= 0) return;
    if (damage < fixture->integrity) {
        fixture->integrity -= damage;
        return;
    }

    destroyed_pad = fixture->pad;
    {
        const pt_fixture_def *def =
            pt_fixture_def_at(game->campaign, fixture->kind);
        was_reroute = def && def->role == PT_ROLE_REROUTE;
    }

    memset(fixture, 0, sizeof *fixture);
    fixture->damage_scale = 1.0f;
    fixture->range_scale = 1.0f;
    if (game->fixtures_lost < UINT32_MAX) ++game->fixtures_lost;

    if (was_reroute) {
        const pt_pad_def *pad = pt_pad(destroyed_pad);
        if (pad) {
            uint16_t cell = pt_cell_index((int)pad->x, (int)pad->y);
            pt_board_set_bias(&game->board, cell, 0u);
            (void)pt_board_rebuild(&game->board);
        }
    }
    pt_fixtures_refresh_support(game);
}

static void splash_impact(pt_game *game, const pt_projectile *projectile,
                          float impact_x, float impact_y)
{
    uint16_t indices[PT_MAX_UNITS];
    uint16_t serials[PT_MAX_UNITS];
    size_t count = 0u;
    float radius_squared =
        projectile->splash_radius * projectile->splash_radius;

    /* Snapshot candidates so a death hook cannot make a just-spawned child
     * inherit the remainder of its parent's explosion. */
    for (size_t i = 0; i < PT_MAX_UNITS; ++i) {
        const pt_unit *unit = &game->units.slots[i];
        float dx;
        float dy;

        if (!unit->alive) continue;
        dx = unit->x - impact_x;
        dy = unit->y - impact_y;
        if (dx * dx + dy * dy > radius_squared) continue;
        indices[count] = (uint16_t)i;
        serials[count] = unit->serial;
        ++count;
    }

    for (size_t candidate = 0u; candidate < count; ++candidate) {
        pt_unit *unit = &game->units.slots[indices[candidate]];
        if (!unit->alive || unit->serial != serials[candidate]) continue;
        (void)apply_damage_from(game, unit, projectile->damage,
                                projectile->damage_type,
                                projectile->source_pad);
    }
}

static pt_unit *next_pierce_target(pt_game *game, uint16_t hit_serial,
                                   float impact_x, float impact_y,
                                   float direction_x, float direction_y)
{
    pt_unit *best = NULL;
    float best_distance = 0.0f;

    for (size_t i = 0; i < PT_MAX_UNITS; ++i) {
        pt_unit *candidate = &game->units.slots[i];
        float dx;
        float dy;
        float forward;
        float distance;

        if (!candidate->alive || candidate->serial == hit_serial) continue;
        dx = candidate->x - impact_x;
        dy = candidate->y - impact_y;
        forward = dx * direction_x + dy * direction_y;
        if (forward <= 1e-5f) continue;
        distance = dx * dx + dy * dy;
        if (!best || distance < best_distance ||
            (distance == best_distance &&
             candidate->serial < best->serial)) {
            best = candidate;
            best_distance = distance;
        }
    }
    return best;
}

static void resolve_impact(pt_game *game, pt_projectile *projectile,
                           float direction_x, float direction_y)
{
    uint16_t hit_serial = projectile->target_serial;
    pt_unit *target = unit_with_serial(game, hit_serial);

    projectile->x = projectile->target_x;
    projectile->y = projectile->target_y;

    if (projectile->splash_radius > 0.0f) {
        splash_impact(game, projectile, projectile->x, projectile->y);
    } else if (target) {
        (void)apply_damage_from(game, target, projectile->damage,
                                projectile->damage_type,
                                projectile->source_pad);
    }

    if (projectile->pierce_remaining > 0u) {
        pt_unit *next = next_pierce_target(
            game, hit_serial, projectile->x, projectile->y,
            direction_x, direction_y);
        if (next) {
            --projectile->pierce_remaining;
            projectile->target_serial = next->serial;
            projectile->target_x = next->x;
            projectile->target_y = next->y;
            return;
        }
    }
    retire_projectile(&game->projectiles, projectile);
}

void pt_combat_update(pt_game *game, double dt)
{
    float step_seconds;

    if (!game || !isfinite(dt) || dt < 0.0 || dt > (double)FLT_MAX)
        return;
    step_seconds = (float)dt;

    for (size_t i = 0; i < PT_MAX_PROJECTILES; ++i) {
        pt_projectile *projectile = &game->projectiles.slots[i];
        pt_unit *target;
        float dx;
        float dy;
        float distance;
        float direction_x;
        float direction_y;
        float step;

        if (!projectile->alive) continue;
        target = unit_with_serial(game, projectile->target_serial);
        if (target) {
            projectile->target_x = target->x;
            projectile->target_y = target->y;
        }

        dx = projectile->target_x - projectile->x;
        dy = projectile->target_y - projectile->y;
        distance = sqrtf(dx * dx + dy * dy);
        if (distance > 1e-6f) {
            direction_x = dx / distance;
            direction_y = dy / distance;
        } else {
            direction_x = 1.0f;
            direction_y = 0.0f;
        }

        step = projectile->speed * step_seconds;
        if (!(step > 0.0f) || step >= distance ||
            distance <= 1e-6f) {
            resolve_impact(game, projectile,
                           direction_x, direction_y);
        } else {
            projectile->x += direction_x * step;
            projectile->y += direction_y * step;
        }
    }
}
