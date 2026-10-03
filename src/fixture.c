/* Fixture placement, targeting, support auras, and firing.
 *
 * Target selection deliberately remains one linear pass over the fixed unit
 * pool.  All comparisons are deterministic and every tie resolves to the
 * lowest unit serial.
 */
#include "pleb_tower.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <string.h>

#define PT_PROJECTILE_SPEED 12.0f

typedef enum pt_status_effect {
    PT_STATUS_HOLD = 0,
    PT_STATUS_STUN
} pt_status_effect;

static uint32_t add_u32_saturating(uint32_t first, uint32_t second)
{
    if (second > UINT32_MAX - first) return UINT32_MAX;
    return first + second;
}

static bool pad_position(const pt_game *game, uint8_t pad_id, float *out_x, float *out_y)
{
    const pt_pad_def *pad = pt_game_pad(game, pad_id);

    if (pad == NULL || out_x == NULL || out_y == NULL) return false;
    *out_x = (float)pad->x + 0.5f;
    *out_y = (float)pad->y + 0.5f;
    return true;
}

static uint16_t pad_cell(const pt_game *game, uint8_t pad_id)
{
    const pt_pad_def *pad = pt_game_pad(game, pad_id);

    if (pad == NULL) return PT_CELL_COUNT;
    return pt_cell_index((int)pad->x, (int)pad->y);
}

static pt_fixture *fixture_slot(pt_game *game, uint8_t pad)
{
    if (game == NULL || pad == 0u || pad > PT_MAX_FIXTURES) return NULL;
    return &game->fixtures[pad - 1u];
}

static const pt_tier_def *fixture_tier(const pt_game *game,
                                       const pt_fixture *fixture,
                                       const pt_fixture_def **out_definition)
{
    const pt_fixture_def *definition;

    if (game == NULL || fixture == NULL ||
        fixture->tier >= PT_MAX_TIER)
        return NULL;
    definition = pt_fixture_def_at(game->campaign, fixture->kind);
    if (definition == NULL) return NULL;
    if (out_definition != NULL) *out_definition = definition;
    return &definition->tiers[fixture->tier];
}

static bool fixture_is_reroute(const pt_game *game,
                               const pt_fixture *fixture)
{
    const pt_fixture_def *definition;

    return fixture_tier(game, fixture, &definition) != NULL &&
           definition->role == PT_ROLE_REROUTE;
}

static bool rebuild_with_bias(pt_game *game, uint8_t pad, uint16_t bias,
                              uint16_t *old_bias)
{
    uint16_t cell;

    if (game == NULL) return false;
    cell = pad_cell(game, pad);
    if (cell >= PT_CELL_COUNT) return false;
    if (old_bias != NULL) *old_bias = game->board.bias[cell];
    pt_board_set_bias(&game->board, cell, bias);
    if (pt_board_rebuild(&game->board)) return true;
    if (old_bias != NULL)
        pt_board_set_bias(&game->board, cell, *old_bias);
    return false;
}

void pt_fixtures_reset(pt_game *game)
{
    bool rebuild = false;
    size_t index;

    if (game == NULL) return;
    for (index = 0u; index < PT_MAX_FIXTURES; ++index) {
        pt_fixture *fixture = &game->fixtures[index];
        uint16_t cell;

        if (!fixture->present || !fixture_is_reroute(game, fixture))
            continue;
        cell = pad_cell(game, fixture->pad);
        if (cell >= PT_CELL_COUNT || game->board.bias[cell] == 0u)
            continue;
        pt_board_set_bias(&game->board, cell, 0u);
        rebuild = true;
    }
    (void)memset(game->fixtures, 0, sizeof game->fixtures);
    for (index = 0u; index < PT_MAX_FIXTURES; ++index) {
        game->fixtures[index].damage_scale = 1.0f;
        game->fixtures[index].range_scale = 1.0f;
    }
    if (rebuild) (void)pt_board_rebuild(&game->board);
}

pt_fixture *pt_fixture_at_pad(pt_game *game, uint8_t pad)
{
    pt_fixture *fixture = fixture_slot(game, pad);

    if (fixture == NULL || !fixture->present || fixture->pad != pad)
        return NULL;
    return fixture;
}

bool pt_fixture_place(pt_game *game, uint8_t pad, uint16_t kind)
{
    const pt_fixture_def *definition;
    const pt_tier_def *tier;
    pt_fixture *fixture;
    pt_fixture previous;
    pt_economy economy_before;
    uint16_t old_bias = 0u;

    if (game == NULL || pt_game_pad(game, pad) == NULL) return false;
    fixture = fixture_slot(game, pad);
    definition = pt_fixture_def_at(game->campaign, kind);
    if (fixture == NULL || fixture->present || definition == NULL)
        return false;
    tier = &definition->tiers[0];

    previous = *fixture;
    economy_before = game->economy;
    if (!pt_economy_spend(game, tier->cost)) return false;

    (void)memset(fixture, 0, sizeof *fixture);
    fixture->kind = kind;
    fixture->tier = 0u;
    fixture->pad = pad;
    fixture->mode = (uint8_t)PT_TARGET_FIRST;
    fixture->present = 1u;
    fixture->integrity = tier->integrity;
    fixture->integrity_max = tier->integrity;
    fixture->invested = tier->cost;
    fixture->damage_scale = 1.0f;
    fixture->range_scale = 1.0f;

    if (definition->role == PT_ROLE_REROUTE &&
        !rebuild_with_bias(game, pad, tier->pull_cells, &old_bias)) {
        *fixture = previous;
        game->economy = economy_before;
        return false;
    }

    pt_fixtures_refresh_support(game);
    if (!game->headless) pt_audio_cue(PT_CUE_BUILD_PLACE);
    return true;
}

bool pt_fixture_upgrade(pt_game *game, uint8_t pad)
{
    const pt_fixture_def *definition;
    const pt_tier_def *new_tier;
    pt_fixture *fixture = pt_fixture_at_pad(game, pad);
    pt_fixture previous;
    pt_economy economy_before;
    uint8_t new_tier_index;
    uint16_t old_bias = 0u;

    if (fixture == NULL ||
        fixture_tier(game, fixture, &definition) == NULL ||
        fixture->tier + 1u >= PT_MAX_TIER)
        return false;
    new_tier_index = (uint8_t)(fixture->tier + 1u);
    new_tier = &definition->tiers[new_tier_index];
    previous = *fixture;
    economy_before = game->economy;

    if (!pt_economy_spend(game, new_tier->cost)) return false;

    fixture->tier = new_tier_index;
    fixture->integrity_max = new_tier->integrity;
    fixture->integrity = new_tier->integrity;
    fixture->invested =
        add_u32_saturating(fixture->invested, new_tier->cost);

    if (definition->role == PT_ROLE_REROUTE &&
        !rebuild_with_bias(game, pad, new_tier->pull_cells, &old_bias)) {
        *fixture = previous;
        game->economy = economy_before;
        return false;
    }

    pt_fixtures_refresh_support(game);
    if (!game->headless) pt_audio_cue(PT_CUE_BUILD_UPGRADE);
    return true;
}

static void refund_currency(pt_game *game, uint32_t refund)
{
    int64_t currency;

    currency = (int64_t)game->economy.currency + (int64_t)refund;
    if (currency > INT32_MAX) currency = INT32_MAX;
    if (currency < 0) currency = 0;
    game->economy.currency = (int32_t)currency;
}

bool pt_fixture_sell(pt_game *game, uint8_t pad)
{
    pt_fixture *fixture = pt_fixture_at_pad(game, pad);
    uint32_t refund;
    uint16_t old_bias = 0u;

    if (fixture == NULL) return false;
    if (fixture_is_reroute(game, fixture) &&
        !rebuild_with_bias(game, pad, 0u, &old_bias))
        return false;

    refund = (uint32_t)(((uint64_t)fixture->invested *
                         PT_SELL_REFUND_PERCENT) /
                        UINT64_C(100));
    refund_currency(game, refund);
    (void)memset(fixture, 0, sizeof *fixture);
    fixture->damage_scale = 1.0f;
    fixture->range_scale = 1.0f;
    pt_fixtures_refresh_support(game);
    if (!game->headless) pt_audio_cue(PT_CUE_BUILD_SELL);
    return true;
}

bool pt_fixture_repair(pt_game *game, uint8_t pad)
{
    pt_fixture *fixture = pt_fixture_at_pad(game, pad);
    uint64_t missing;
    uint64_t cost_wide;
    uint32_t cost;

    if (fixture == NULL || fixture->integrity_max <= 0 ||
        fixture->integrity >= fixture->integrity_max ||
        PT_REPAIR_INTEGRITY_PER_UNIT <= 0)
        return false;

    missing = (uint64_t)((int64_t)fixture->integrity_max -
                         (int64_t)fixture->integrity);
    cost_wide =
        (missing + (uint64_t)PT_REPAIR_INTEGRITY_PER_UNIT - 1u) /
        (uint64_t)PT_REPAIR_INTEGRITY_PER_UNIT;
    if (cost_wide > UINT32_MAX) return false;
    cost = (uint32_t)cost_wide;
    if (!pt_economy_spend(game, cost)) return false;

    fixture->integrity = fixture->integrity_max;
    fixture->invested = add_u32_saturating(fixture->invested, cost);
    if (!game->headless) pt_audio_cue(PT_CUE_BUILD_REPAIR);
    return true;
}

void pt_fixture_cycle_mode(pt_game *game, uint8_t pad)
{
    pt_fixture *fixture = pt_fixture_at_pad(game, pad);

    if (fixture == NULL) return;
    fixture->mode =
        (uint8_t)(((unsigned int)fixture->mode + 1u) %
                  (unsigned int)PT_TARGET_MODE_COUNT);
    if (!game->headless) pt_audio_cue(PT_CUE_BUILD_MODE);
}

void pt_fixtures_refresh_support(pt_game *game)
{
    size_t target_index;

    if (game == NULL) return;
    for (target_index = 0u; target_index < PT_MAX_FIXTURES;
         ++target_index) {
        pt_fixture *target = &game->fixtures[target_index];
        float target_x;
        float target_y;
        size_t support_index;

        target->damage_scale = 1.0f;
        target->range_scale = 1.0f;
        target->currency_tick = 0u;
        if (!target->present ||
            !pad_position(game, target->pad, &target_x, &target_y))
            continue;

        for (support_index = 0u; support_index < PT_MAX_FIXTURES;
             ++support_index) {
            const pt_fixture *support = &game->fixtures[support_index];
            const pt_fixture_def *definition;
            const pt_tier_def *tier;
            float support_x;
            float support_y;
            float dx;
            float dy;
            float damage_scale;
            float range_scale;

            if (!support->present) continue;
            tier = fixture_tier(game, support, &definition);
            if (tier == NULL || definition->role != PT_ROLE_SUPPORT ||
                !pad_position(game, support->pad, &support_x, &support_y))
                continue;
            dx = target_x - support_x;
            dy = target_y - support_y;
            if (dx * dx + dy * dy > tier->radius * tier->radius)
                continue;

            damage_scale = 1.0f + tier->damage_buff;
            range_scale = 1.0f + tier->range_buff;
            if (damage_scale > target->damage_scale)
                target->damage_scale = damage_scale;
            if (range_scale > target->range_scale)
                target->range_scale = range_scale;
            if (tier->currency_tick > target->currency_tick)
                target->currency_tick = (uint8_t)(
                    tier->currency_tick > UINT8_MAX
                        ? UINT8_MAX
                        : tier->currency_tick);
        }
    }
}

static bool unit_plane_is_targeted(const pt_tier_def *tier,
                                   const pt_unit_def *unit_definition)
{
    uint8_t plane;

    if (tier == NULL || unit_definition == NULL) return false;
    plane = unit_definition->air ? (uint8_t)PT_TARGETS_AIR
                                 : (uint8_t)PT_TARGETS_GROUND;
    return (tier->targets & plane) != 0u;
}

static float unit_progress(const pt_game *game, const pt_unit *unit,
                           const pt_unit_def *definition)
{
    if (definition->air) {
        const pt_campaign_def *campaign = pt_game_campaign(game);
        float goal_x = (float)campaign->goal_x + 0.5f;
        float goal_y = (float)campaign->goal_y + 0.5f;
        float dx = unit->x - goal_x;
        float dy = unit->y - goal_y;

        return sqrtf(dx * dx + dy * dy);
    }

    return pt_board_distance_at(&game->board, unit->x, unit->y);
}

static bool target_is_better(const pt_game *game,
                             const pt_fixture *fixture,
                             const pt_unit *candidate,
                             const pt_unit_def *candidate_definition,
                             float candidate_distance_sq,
                             const pt_unit *best,
                             const pt_unit_def *best_definition,
                             float best_distance_sq)
{
    bool better = false;
    bool tied = false;

    switch ((pt_target_mode)fixture->mode) {
    case PT_TARGET_LAST: {
        float candidate_progress =
            unit_progress(game, candidate, candidate_definition);
        float best_progress = unit_progress(game, best, best_definition);
        better = candidate_progress > best_progress;
        tied = candidate_progress == best_progress;
        break;
    }
    case PT_TARGET_STRONGEST:
        better = candidate->integrity > best->integrity;
        tied = candidate->integrity == best->integrity;
        break;
    case PT_TARGET_CLOSEST:
        better = candidate_distance_sq < best_distance_sq;
        tied = candidate_distance_sq == best_distance_sq;
        break;
    case PT_TARGET_FIRST:
    default: {
        float candidate_progress =
            unit_progress(game, candidate, candidate_definition);
        float best_progress = unit_progress(game, best, best_definition);
        better = candidate_progress < best_progress;
        tied = candidate_progress == best_progress;
        break;
    }
    }
    return better || (tied && candidate->serial < best->serial);
}

float pt_fixture_range(const pt_game *game, const pt_fixture *fixture, uint8_t tier)
{
    if (game == NULL || fixture == NULL || tier >= PT_MAX_TIER) return 0.0f;
    const pt_fixture_def *definition = pt_fixture_def_at(game->campaign, fixture->kind);
    if (definition == NULL) return 0.0f;
    const pt_tier_def *stats = &definition->tiers[tier];
    if (definition->role == PT_ROLE_SUPPORT) return stats->radius;
    if (definition->role == PT_ROLE_REROUTE) return (float)stats->pull_cells;
    return stats->range * (fixture->present ? fixture->range_scale : 1.0f);
}

static pt_unit *acquire_target(pt_game *game, const pt_fixture *fixture,
                               const pt_tier_def *tier)
{
    pt_unit *best = NULL;
    const pt_unit_def *best_definition = NULL;
    float fixture_x;
    float fixture_y;
    float best_distance_sq = 0.0f;
    float range;
    float range_sq;
    size_t index;

    if (game == NULL || fixture == NULL || tier == NULL ||
        !pad_position(game, fixture->pad, &fixture_x, &fixture_y))
        return NULL;
    range = tier->range * fixture->range_scale;
    if (!(range >= 0.0f) || !isfinite(range)) return NULL;
    range_sq = range * range;

    for (index = 0u; index < PT_MAX_UNITS; ++index) {
        pt_unit *candidate = &game->units.slots[index];
        const pt_unit_def *candidate_definition;
        float dx;
        float dy;
        float distance_sq;

        if (!candidate->alive || !isfinite(candidate->x) ||
            !isfinite(candidate->y))
            continue;
        candidate_definition =
            pt_unit_def_at(game->campaign, candidate->kind);
        if (!unit_plane_is_targeted(tier, candidate_definition))
            continue;
        if (candidate_definition->hardened &&
            (tier->hold_seconds > 0.0f || tier->stun_seconds > 0.0f))
            continue;
        dx = candidate->x - fixture_x;
        dy = candidate->y - fixture_y;
        distance_sq = dx * dx + dy * dy;
        if (distance_sq > range_sq) continue;
        if (best == NULL ||
            target_is_better(game, fixture, candidate,
                             candidate_definition, distance_sq, best,
                             best_definition, best_distance_sq)) {
            best = candidate;
            best_definition = candidate_definition;
            best_distance_sq = distance_sq;
        }
    }
    return best;
}

/* Hardened gating for both status roles lives here so the two effects cannot
 * drift apart as content evolves. */
static void apply_status_effect(pt_game *game, pt_unit *unit,
                                pt_status_effect effect, float seconds)
{
    const pt_unit_def *definition;

    if (game == NULL || unit == NULL || !(seconds > 0.0f)) return;
    definition = pt_unit_def_at(game->campaign, unit->kind);
    if (definition == NULL || definition->hardened) return;
    pt_effect_emit(game, effect == PT_STATUS_HOLD ? PT_EFFECT_HOLD : PT_EFFECT_STUN,
        unit->x, unit->y, 0.7f);
    if (effect == PT_STATUS_HOLD) {
        if (seconds > unit->hold_remaining)
            unit->hold_remaining = seconds;
    } else if (seconds > unit->stun_remaining) {
        unit->stun_remaining = seconds;
    }
}

static int32_t scaled_damage(const pt_fixture *fixture,
                             const pt_tier_def *tier)
{
    float damage = (float)tier->damage * fixture->damage_scale;

    if (!(damage > 0.0f)) return 0;
    if (damage >= (float)INT32_MAX) return INT32_MAX;
    return (int32_t)floorf(damage);
}

static void emit_projectile(pt_game *game, const pt_fixture *fixture,
                            const pt_tier_def *tier, const pt_unit *target)
{
    size_t index;

    for (index = 0u; index < PT_MAX_PROJECTILES; ++index) {
        pt_projectile *projectile = &game->projectiles.slots[index];
        float source_x;
        float source_y;

        if (projectile->alive) continue;
        if (!pad_position(game, fixture->pad, &source_x, &source_y)) return;
        (void)memset(projectile, 0, sizeof *projectile);
        projectile->x = source_x;
        projectile->y = source_y;
        projectile->target_x = target->x;
        projectile->target_y = target->y;
        projectile->speed = PT_PROJECTILE_SPEED;
        projectile->damage = scaled_damage(fixture, tier);
        projectile->splash_radius = tier->splash_radius;
        projectile->target_serial = target->serial;
        projectile->source_pad = fixture->pad;
        projectile->damage_type = tier->damage_type;
        projectile->alive = 1u;
        if (game->projectiles.live < PT_MAX_PROJECTILES)
            ++game->projectiles.live;
        return;
    }
    ++game->projectiles.overflow;
}

static void fire_fixture(pt_game *game, const pt_fixture *fixture,
                         const pt_fixture_def *definition,
                         const pt_tier_def *tier, pt_unit *target)
{
    if (!game->headless) {
        const uint32_t cues[PT_ROLE_COUNT] = {
            PT_CUE_FIRE_RAPID, PT_CUE_FIRE_ARTILLERY, PT_CUE_FIRE_CONTROL,
            PT_CUE_BUILD_REROUTE, PT_CUE_FIRE_ANTIAIR, PT_CUE_FIRE_DISABLE,
            0u, 0u
        };
        if (definition->role < PT_ROLE_COUNT)
            pt_audio_cue(cues[definition->role]);
    }
    switch ((pt_fixture_role)definition->role) {
    case PT_ROLE_CONTROL:
        apply_status_effect(game, target, PT_STATUS_HOLD,
                            tier->hold_seconds);
        break;
    case PT_ROLE_DISABLE:
        apply_status_effect(game, target, PT_STATUS_STUN,
                            tier->stun_seconds);
        break;
    case PT_ROLE_RAPID:
    case PT_ROLE_ARTILLERY:
    case PT_ROLE_ANTIAIR:
        emit_projectile(game, fixture, tier, target);
        break;
    case PT_ROLE_REROUTE:
    case PT_ROLE_VISION:
    case PT_ROLE_SUPPORT:
    case PT_ROLE_COUNT:
    default:
        break;
    }
}

static void update_weapon(pt_game *game, pt_fixture *fixture,
                          const pt_fixture_def *definition,
                          const pt_tier_def *tier, float dt)
{
    float interval;
    unsigned int shots = 0u;

    if (!(tier->rate > 0.0f) || tier->targets == PT_TARGETS_NONE)
        return;
    fixture->cooldown -= dt;
    interval = 1.0f / tier->rate;
    while (fixture->cooldown <= 0.0f &&
           shots <= PT_MAX_PROJECTILES) {
        pt_unit *target = acquire_target(game, fixture, tier);

        if (target == NULL) {
            fixture->cooldown = 0.0f;
            break;
        }
        fire_fixture(game, fixture, definition, tier, target);
        fixture->cooldown += interval;
        ++shots;
    }
}

static void repair_fixture_by(pt_fixture *fixture, int32_t repair)
{
    int64_t integrity;

    if (fixture == NULL || repair <= 0 ||
        fixture->integrity >= fixture->integrity_max)
        return;
    integrity = (int64_t)fixture->integrity + repair;
    if (integrity > fixture->integrity_max)
        integrity = fixture->integrity_max;
    fixture->integrity = (int32_t)integrity;
}

static void update_support(pt_game *game, pt_fixture *support,
                           const pt_tier_def *tier, float dt)
{
    float support_x;
    float support_y;
    int32_t repair;
    size_t index;

    if (!(tier->repair_per_second > 0.0f) ||
        !pad_position(game, support->pad, &support_x, &support_y))
        return;
    support->cooldown += tier->repair_per_second * dt;
    if (support->cooldown < 1.0f) return;
    if (support->cooldown >= (float)INT32_MAX) {
        repair = INT32_MAX;
        support->cooldown = 0.0f;
    } else {
        repair = (int32_t)floorf(support->cooldown);
        support->cooldown -= (float)repair;
    }

    for (index = 0u; index < PT_MAX_FIXTURES; ++index) {
        pt_fixture *target = &game->fixtures[index];
        float target_x;
        float target_y;
        float dx;
        float dy;

        if (!target->present ||
            !pad_position(game, target->pad, &target_x, &target_y))
            continue;
        dx = target_x - support_x;
        dy = target_y - support_y;
        if (dx * dx + dy * dy > tier->radius * tier->radius)
            continue;
        repair_fixture_by(target, repair);
    }
}

static void sync_reroute_biases(pt_game *game)
{
    uint16_t old_bias[PT_PAD_COUNT];
    bool changed = false;
    uint8_t pad_id;

    for (pad_id = 1u; pad_id <= pt_map(game->board.map)->pad_count; ++pad_id) {
        const pt_fixture *fixture = pt_fixture_at_pad(game, pad_id);
        const pt_fixture_def *definition = NULL;
        const pt_tier_def *tier = NULL;
        uint16_t expected = 0u;
        uint16_t cell = pad_cell(game, pad_id);

        old_bias[pad_id - 1u] = game->board.bias[cell];
        if (fixture != NULL) {
            tier = fixture_tier(game, fixture, &definition);
            if (tier != NULL && definition->role == PT_ROLE_REROUTE)
                expected = tier->pull_cells;
        }
        if (game->board.bias[cell] != expected) {
            pt_board_set_bias(&game->board, cell, expected);
            changed = true;
        }
    }
    if (!changed || pt_board_rebuild(&game->board)) return;
    for (pad_id = 1u; pad_id <= pt_map(game->board.map)->pad_count; ++pad_id)
        pt_board_set_bias(&game->board, pad_cell(game, pad_id),
                          old_bias[pad_id - 1u]);
}

void pt_fixtures_update(pt_game *game, double dt)
{
    float step;
    size_t index;

    if (game == NULL || !isfinite(dt) || dt < 0.0 ||
        dt > (double)FLT_MAX)
        return;
    step = (float)dt;
    sync_reroute_biases(game);

    for (index = 0u; index < PT_MAX_FIXTURES; ++index) {
        pt_fixture *fixture = &game->fixtures[index];
        const pt_fixture_def *definition;
        const pt_tier_def *tier;

        if (!fixture->present) continue;
        tier = fixture_tier(game, fixture, &definition);
        if (tier == NULL) continue;
        if (definition->role == PT_ROLE_SUPPORT)
            update_support(game, fixture, tier, step);
        else
            update_weapon(game, fixture, definition, tier, step);
    }
}
