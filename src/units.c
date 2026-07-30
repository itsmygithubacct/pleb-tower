/* Unit pool, wave execution, movement, and unit-authored behaviours. */
#include "pleb_tower.h"

#include <assert.h>
#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static void retire_unit(pt_unit_pool *pool, pt_unit *unit)
{
    if (!unit->alive) return;
    unit->alive = 0u;
    unit->halted = 0u;
    if (pool->live > 0u) --pool->live;
}

void pt_units_reset(pt_unit_pool *pool)
{
    if (!pool) return;
    memset(pool, 0, sizeof *pool);
    pool->next_serial = 1u;
}

pt_unit *pt_units_spawn(pt_unit_pool *pool, uint16_t kind, float x, float y)
{
    const pt_unit_def *def;

    if (!pool) return NULL;

    /* The public spawn signature has no campaign argument. Both 1.0
     * campaigns deliberately have stat-parity unit slots, so campaign zero
     * is the canonical initializer for the shared runtime representation. */
    def = pt_unit_def_at(0u, kind);
    if (!def) return NULL;

    if (pool->live >= PT_MAX_UNITS) {
        ++pool->overflow;
        return NULL;
    }

    for (size_t i = 0; i < PT_MAX_UNITS; ++i) {
        pt_unit *unit = &pool->slots[i];
        if (unit->alive) continue;

        memset(unit, 0, sizeof *unit);
        unit->x = x;
        unit->y = y;
        unit->integrity = def->integrity;
        unit->shield = def->shield;
        unit->kind = kind;
        if (pool->next_serial == 0u) pool->next_serial = 1u;
        unit->serial = pool->next_serial++;
        unit->alive = 1u;
        ++pool->live;
        return unit;
    }

    /* A mismatched live counter must still fail safely and visibly. */
    ++pool->overflow;
    return NULL;
}

/* Smooth weighted round-robin: before each choice every group gains its
 * authored count, the greatest score wins (ties use the lower group index),
 * and the winner loses the wave total. Replaying from ordinal zero makes the
 * rule stateless and byte-reproducible. A×4,B×2 is A,B,A,A,B,A. */
static uint16_t wave_kind_at(const pt_wave_def *wave, uint16_t ordinal)
{
    int32_t score[PT_MAX_WAVE_GROUPS] = { 0 };
    uint16_t total = 0u;
    uint16_t chosen = 0u;

    if (!wave || wave->group_count == 0u) return 0u;
    for (uint16_t group = 0u; group < wave->group_count; ++group)
        total = (uint16_t)(total + wave->groups[group].count);
    if (total == 0u) return wave->groups[0].type;

    for (uint16_t item = 0u; item <= ordinal; ++item) {
        chosen = 0u;
        for (uint16_t group = 0u; group < wave->group_count; ++group) {
            score[group] += (int32_t)wave->groups[group].count;
            if (score[group] > score[chosen]) chosen = group;
        }
        score[chosen] -= (int32_t)total;
    }
    return wave->groups[chosen].type;
}

static void spawn_scheduled_unit(pt_game *game, const pt_wave_def *wave)
{
    const pt_campaign_def *campaign = pt_campaign(game->campaign);
    uint16_t kind = wave_kind_at(wave, game->wave.spawned);
    float x = (float)campaign->spawn_x + 0.5f;
    float y = (float)campaign->spawn_y + 0.5f;

    (void)pt_units_spawn(&game->units, kind, x, y);
    ++game->wave.spawned;
}

static void update_wave_schedule(pt_game *game, double dt)
{
    const pt_wave_def *wave;

    if (!game->wave.active) return;
    wave = pt_wave_def_at(game->campaign, game->wave.index);
    if (!wave) return;

    if (game->wave.total == 0u) game->wave.total = wave->total_units;
    if (game->wave.spawned >= game->wave.total) return;

    /* A zero timer means "due now". Events exactly at the right edge of this
     * update are processed on the next update, avoiding a double spawn when a
     * test advances by one exact interval. */
    if (game->wave.spawn_timer <= 0.0) {
        spawn_scheduled_unit(game, wave);
        game->wave.spawn_timer += (double)wave->interval;
    }

    game->wave.spawn_timer -= dt;
    while (game->wave.spawned < game->wave.total &&
           game->wave.spawn_timer < 0.0) {
        spawn_scheduled_unit(game, wave);
        game->wave.spawn_timer += (double)wave->interval;
    }
}

static bool at_goal(const pt_game *game, const pt_unit *unit)
{
    int x = (int)floorf(unit->x);
    int y = (int)floorf(unit->y);

    if (x < 0 || y < 0 || x >= PT_COLUMNS || y >= PT_ROWS) return false;
    return pt_cell_index(x, y) == game->board.goal_cell;
}

static void update_status_timers(const pt_unit_def *def, pt_unit *unit,
                                 double dt, bool *frozen)
{
    if (def->hardened) {
        /* Effect application owns the first hardened check. Clear malformed
         * external state defensively, then assert the invariant here too. */
        unit->hold_remaining = 0.0f;
        unit->stun_remaining = 0.0f;
        *frozen = false;
        assert(unit->hold_remaining == 0.0f);
        assert(unit->stun_remaining == 0.0f);
        return;
    }

    *frozen = unit->hold_remaining > 0.0f ||
              unit->stun_remaining > 0.0f;
    if (unit->hold_remaining > 0.0f) {
        double remaining = (double)unit->hold_remaining - dt;
        unit->hold_remaining =
            remaining > 0.0 ? (float)remaining : 0.0f;
    }
    if (unit->stun_remaining > 0.0f) {
        double remaining = (double)unit->stun_remaining - dt;
        unit->stun_remaining =
            remaining > 0.0 ? (float)remaining : 0.0f;
    }
}

static void update_shield(const pt_unit_def *def, pt_unit *unit, double dt)
{
    double idle = (double)unit->shield_idle + dt;
    double tolerance;

    if (idle < 0.0) idle = 0.0;
    unit->shield_idle = (float)idle;
    if (def->shield <= 0 || unit->shield >= def->shield) return;

    tolerance = 8.0 * (double)FLT_EPSILON *
                fmax(1.0, (double)def->shield_regen_delay);
    if (idle + tolerance >= (double)def->shield_regen_delay) {
        unit->shield = def->shield;
        unit->shield_idle = def->shield_regen_delay;
    }
}

static int32_t accrue_whole_points(float *remainder, float per_second,
                                   double dt)
{
    double accrued;
    int32_t whole;

    if (per_second <= 0.0f || dt <= 0.0) return 0;
    accrued = (double)*remainder + (double)per_second * dt;
    if (accrued >= (double)INT32_MAX) {
        *remainder = 0.0f;
        return INT32_MAX;
    }
    whole = (int32_t)floor(accrued + 1e-9);
    *remainder = (float)(accrued - (double)whole);
    if (*remainder < 0.0f) *remainder = 0.0f;
    return whole;
}

static void update_emit(pt_game *game, pt_unit *unit,
                        const pt_unit_def *def, double dt)
{
    const pt_hook_def *emit = &def->emit;
    uint16_t emitted_total;

    if (!emit->active || emit->count == 0u || emit->value <= 0.0f) return;
    /* The generated format gained a per-emitter cap before pt_unit gained its
     * dedicated counter. Emitting unit kinds have no attack or aura in 1.0,
     * so attack_timer safely carries the exact integer count meanwhile. */
    if (!(unit->attack_timer > 0.0f))
        emitted_total = 0u;
    else if (unit->attack_timer >= (float)UINT16_MAX)
        emitted_total = UINT16_MAX;
    else
        emitted_total = (uint16_t)unit->attack_timer;
    unit->emit_timer += (float)dt;

    while ((double)unit->emit_timer + 1e-7 >= (double)emit->value) {
        unit->emit_timer =
            (float)((double)unit->emit_timer - (double)emit->value);
        if (unit->emit_timer < 0.0f) unit->emit_timer = 0.0f;
        for (uint16_t child = 0u; child < emit->count; ++child) {
            if (emit->max_total > 0u &&
                emitted_total >= emit->max_total)
                break;
            (void)pt_units_spawn(&game->units, emit->spawn,
                                 unit->x, unit->y);
            ++emitted_total;
        }
        unit->attack_timer = (float)emitted_total;
    }
}

static void update_aura(pt_game *game, pt_unit *source,
                        const pt_unit_def *def, double dt)
{
    int32_t amount;
    float radius_squared;

    if (def->aura_radius <= 0.0f || def->aura_per_second <= 0.0f) return;
    amount = accrue_whole_points(&source->attack_timer,
                                 def->aura_per_second, dt);
    if (amount <= 0) return;

    radius_squared = def->aura_radius * def->aura_radius;
    for (size_t i = 0; i < PT_MAX_UNITS; ++i) {
        pt_unit *target = &game->units.slots[i];
        const pt_unit_def *target_def;
        float dx;
        float dy;

        if (!target->alive) continue;
        dx = target->x - source->x;
        dy = target->y - source->y;
        if (dx * dx + dy * dy > radius_squared) continue;
        target_def = pt_unit_def_at(game->campaign, target->kind);
        if (!target_def || target->integrity >= target_def->integrity) continue;
        if (amount >= target_def->integrity - target->integrity)
            target->integrity = target_def->integrity;
        else
            target->integrity += amount;
    }
}

static pt_fixture *nearest_fixture(pt_game *game, const pt_unit *unit,
                                   float range)
{
    pt_fixture *nearest = NULL;
    float nearest_distance = 0.0f;
    float range_squared = range * range;

    for (size_t i = 0; i < PT_MAX_FIXTURES; ++i) {
        pt_fixture *fixture = &game->fixtures[i];
        const pt_pad_def *pad;
        float dx;
        float dy;
        float distance;

        if (!fixture->present) continue;
        pad = pt_pad(fixture->pad);
        if (!pad) continue;
        dx = ((float)pad->x + 0.5f) - unit->x;
        dy = ((float)pad->y + 0.5f) - unit->y;
        distance = dx * dx + dy * dy;
        if (distance > range_squared) continue;
        if (!nearest || distance < nearest_distance) {
            nearest = fixture;
            nearest_distance = distance;
        }
    }
    return nearest;
}

static void update_halt_attack(pt_game *game, pt_unit *unit,
                               const pt_unit_def *def, double dt)
{
    pt_fixture *target = nearest_fixture(game, unit, def->attack_range);
    int32_t damage;

    unit->halted = target ? 1u : 0u;
    if (!target) {
        unit->attack_timer = 0.0f;
        return;
    }

    damage = accrue_whole_points(&unit->attack_timer, def->attack_dps, dt);
    if (damage > 0) {
        pt_combat_damage_fixture(game, target, damage);
        if (!target->present &&
            nearest_fixture(game, unit, def->attack_range) == NULL)
            unit->halted = 0u;
    }
}

static void update_passing_attack(pt_game *game, pt_unit *unit,
                                  const pt_unit_def *def, double dt)
{
    bool in_range = false;
    int32_t damage;

    for (size_t i = 0; i < PT_MAX_FIXTURES; ++i) {
        const pt_fixture *fixture = &game->fixtures[i];
        const pt_pad_def *pad;
        float dx;
        float dy;

        if (!fixture->present) continue;
        pad = pt_pad(fixture->pad);
        if (!pad) continue;
        dx = ((float)pad->x + 0.5f) - unit->x;
        dy = ((float)pad->y + 0.5f) - unit->y;
        if (dx * dx + dy * dy <= 1.0f) {
            in_range = true;
            break;
        }
    }
    if (!in_range) {
        unit->attack_timer = 0.0f;
        return;
    }

    damage = accrue_whole_points(&unit->attack_timer, def->attack_dps, dt);
    if (damage <= 0) return;

    for (size_t i = 0; i < PT_MAX_FIXTURES; ++i) {
        pt_fixture *fixture = &game->fixtures[i];
        const pt_pad_def *pad;
        float dx;
        float dy;

        if (!fixture->present) continue;
        pad = pt_pad(fixture->pad);
        if (!pad) continue;
        dx = ((float)pad->x + 0.5f) - unit->x;
        dy = ((float)pad->y + 0.5f) - unit->y;
        if (dx * dx + dy * dy <= 1.0f)
            pt_combat_damage_fixture(game, fixture, damage);
    }
}

static void move_unit(pt_game *game, pt_unit *unit,
                      const pt_unit_def *def, double dt)
{
    float step = def->speed * (float)dt;

    if (step <= 0.0f) return;
    if (def->air) {
        const pt_campaign_def *campaign = pt_campaign(game->campaign);
        float goal_x = (float)campaign->goal_x + 0.5f;
        float goal_y = (float)campaign->goal_y + 0.5f;
        float dx = goal_x - unit->x;
        float dy = goal_y - unit->y;
        float distance = sqrtf(dx * dx + dy * dy);

        if (distance <= step || distance <= 1e-6f) {
            unit->x = goal_x;
            unit->y = goal_y;
        } else {
            unit->x += dx / distance * step;
            unit->y += dy / distance * step;
        }
        return;
    }

    {
        float dx = 0.0f;
        float dy = 0.0f;
        if (pt_board_flow_at(&game->board, unit->x, unit->y, &dx, &dy)) {
            unit->x += dx * step;
            unit->y += dy * step;
        }
    }
}

void pt_units_update(pt_game *game, double dt)
{
    uint8_t update_slot[PT_MAX_UNITS];

    if (!game || !isfinite(dt) || dt < 0.0 ||
        dt > (double)FLT_MAX)
        return;
    update_wave_schedule(game, dt);

    /* Units emitted during this update begin acting on the next tick. */
    for (size_t i = 0; i < PT_MAX_UNITS; ++i)
        update_slot[i] = game->units.slots[i].alive;

    for (size_t i = 0; i < PT_MAX_UNITS; ++i) {
        pt_unit *unit = &game->units.slots[i];
        const pt_unit_def *def;
        bool frozen = false;

        if (!update_slot[i] || !unit->alive) continue;
        def = pt_unit_def_at(game->campaign, unit->kind);
        if (!def) {
            retire_unit(&game->units, unit);
            continue;
        }

        if (at_goal(game, unit)) {
            pt_economy_leak(game, unit);
            retire_unit(&game->units, unit);
            continue;
        }

        update_status_timers(def, unit, dt, &frozen);
        update_shield(def, unit, dt);
        update_emit(game, unit, def, dt);
        update_aura(game, unit, def, dt);

        if (def->attack_kind == PT_ATTACK_FIXTURE)
            update_halt_attack(game, unit, def, dt);
        else
            unit->halted = 0u;

        if (!frozen && !unit->halted) move_unit(game, unit, def, dt);

        if (def->attack_kind == PT_ATTACK_PASSING)
            update_passing_attack(game, unit, def, dt);

        if (unit->alive && at_goal(game, unit)) {
            pt_economy_leak(game, unit);
            retire_unit(&game->units, unit);
        }
    }
}
