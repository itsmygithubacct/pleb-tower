/* Headless fixed-step playthroughs for balance verification.
 *
 * This deliberately drives the real game state machine.  It owns no combat,
 * movement, fixture, or economy rules; its only policy is when to apply the
 * supplied build-order entries and how to serialize the resulting trace.
 */
#include "pleb_tower.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define PT_SIM_SEED UINT64_C(0x53494d554c415445) /* "SIMULATE" */
#define PT_SIM_WAVE_TICK_LIMIT (UINT64_C(900) * PT_TICK_HZ)

/* This forward typedef and prototype are compatible with the shared-header
 * declarations requested in implementation/HEADER_REQUESTS.md.  Keeping the
 * order layout private lets this file compile while that request is being
 * reconciled, without defining the shared struct in an owned file. */
int pt_simulate_run(uint8_t campaign, const pt_sim_order_entry *order,
                    size_t order_count, bool verbose);

typedef struct pt_sim_order_layout {
    uint16_t wave;
    uint8_t  pad;
    uint16_t kind;
    uint8_t  tier;
} pt_sim_order_layout;

typedef struct pt_sim_unit_snapshot {
    uint16_t serial;
    uint16_t kind;
    uint8_t  alive;
    uint8_t  predicted_leak;
} pt_sim_unit_snapshot;

typedef enum pt_sim_result {
    PT_SIM_CLEARED = 0,
    PT_SIM_LOST = 1,
    PT_SIM_ERROR = 2
} pt_sim_result;

static pt_sim_order_layout order_entry_at(const pt_sim_order_entry *order,
                                           size_t index)
{
    pt_sim_order_layout entry;
    const unsigned char *bytes = (const unsigned char *)(const void *)order;

    (void)memcpy(&entry, bytes + index * sizeof entry, sizeof entry);
    return entry;
}

static bool unit_at_goal(const pt_game *game, float x, float y)
{
    int cell_x = (int)floorf(x);
    int cell_y = (int)floorf(y);

    if (cell_x < 0 || cell_y < 0 ||
        cell_x >= PT_COLUMNS || cell_y >= PT_ROWS)
        return false;
    return pt_cell_index(cell_x, cell_y) == game->board.goal_cell;
}

static bool fixture_in_range(const pt_game *game, const pt_unit *unit,
                             float range)
{
    float range_squared = range * range;

    for (size_t index = 0u; index < PT_MAX_FIXTURES; ++index) {
        const pt_fixture *fixture = &game->fixtures[index];
        const pt_pad_def *pad;
        float dx;
        float dy;

        if (!fixture->present) continue;
        pad = pt_game_pad(game, fixture->pad);
        if (pad == NULL) continue;
        dx = (float)pad->x + 0.5f - unit->x;
        dy = (float)pad->y + 0.5f - unit->y;
        if (dx * dx + dy * dy <= range_squared) return true;
    }
    return false;
}

/* Predict leaks before pt_game_step so deaths later in that same step cannot
 * be mistaken for leaks.  The fallback in record_step_leaks handles the rare
 * case where an earlier unit destroys the fixture halting a later unit. */
static bool unit_will_leak(const pt_game *game, const pt_unit *unit)
{
    const pt_unit_def *definition;
    float next_x;
    float next_y;
    float step;

    if (unit_at_goal(game, unit->x, unit->y)) return true;
    definition = pt_unit_def_at(game->campaign, unit->kind);
    if (definition == NULL) return false;
    if (!definition->hardened &&
        (unit->hold_remaining > 0.0f || unit->stun_remaining > 0.0f))
        return false;
    if (definition->attack_kind == PT_ATTACK_FIXTURE &&
        fixture_in_range(game, unit, definition->attack_range))
        return false;

    step = definition->speed * (1.0f - unit->decoy_slow) * (float)PT_STEP_SECONDS;
    if (!(step > 0.0f)) return false;
    next_x = unit->x;
    next_y = unit->y;

    if (definition->air) {
        const pt_campaign_def *campaign = pt_game_campaign(game);
        float goal_x = (float)campaign->goal_x + 0.5f;
        float goal_y = (float)campaign->goal_y + 0.5f;
        float dx = goal_x - next_x;
        float dy = goal_y - next_y;
        float distance = sqrtf(dx * dx + dy * dy);

        if (distance <= step || distance <= 1e-6f) {
            next_x = goal_x;
            next_y = goal_y;
        } else {
            next_x += dx / distance * step;
            next_y += dy / distance * step;
        }
    } else {
        float dx = 0.0f;
        float dy = 0.0f;

        if (!pt_board_flow_at(&game->board, next_x, next_y, &dx, &dy))
            return false;
        next_x += dx * step;
        next_y += dy * step;
    }
    return unit_at_goal(game, next_x, next_y);
}

static void snapshot_units(const pt_game *game,
                           pt_sim_unit_snapshot snapshot[PT_MAX_UNITS])
{
    for (size_t index = 0u; index < PT_MAX_UNITS; ++index) {
        const pt_unit *unit = &game->units.slots[index];

        snapshot[index].serial = unit->serial;
        snapshot[index].kind = unit->kind;
        snapshot[index].alive = unit->alive;
        snapshot[index].predicted_leak =
            unit->alive && unit_will_leak(game, unit);
    }
}

static bool snapshot_unit_disappeared(
    const pt_game *game, size_t index,
    const pt_sim_unit_snapshot snapshot[PT_MAX_UNITS])
{
    const pt_unit *after = &game->units.slots[index];

    return snapshot[index].alive &&
           (!after->alive || after->serial != snapshot[index].serial);
}

static void record_step_leaks(
    const pt_game *game,
    const pt_sim_unit_snapshot snapshot[PT_MAX_UNITS],
    int32_t integrity_before,
    uint32_t wave_leaks[PT_UNITS_PER_CAMPAIGN])
{
    bool recorded_prediction = false;

    if (game->economy.integrity >= integrity_before) return;

    for (size_t index = 0u; index < PT_MAX_UNITS; ++index) {
        uint16_t kind;

        if (!snapshot[index].predicted_leak ||
            !snapshot_unit_disappeared(game, index, snapshot))
            continue;
        kind = snapshot[index].kind;
        if (kind < PT_UNITS_PER_CAMPAIGN) ++wave_leaks[kind];
        recorded_prediction = true;
    }
    if (recorded_prediction) return;

    /* A halt target can disappear earlier in the unit pass and let a later
     * unit move unexpectedly.  In that case, the only pre-combat disappearance
     * whose mass fits the integrity loss is the leak.  Prefer the first stable
     * slot/serial match; authored spawn spacing makes this unambiguous. */
    {
        int32_t loss = integrity_before - game->economy.integrity;

        for (size_t index = 0u; index < PT_MAX_UNITS; ++index) {
            const pt_unit_def *definition;
            uint16_t kind;

            if (!snapshot_unit_disappeared(game, index, snapshot)) continue;
            kind = snapshot[index].kind;
            definition = pt_unit_def_at(game->campaign, kind);
            if (definition == NULL || definition->mass != (uint16_t)loss)
                continue;
            if (kind < PT_UNITS_PER_CAMPAIGN) ++wave_leaks[kind];
            return;
        }
    }
}

static bool order_is_valid(uint8_t map, uint8_t campaign,
                           const pt_sim_order_entry *order,
                           size_t order_count)
{
    const pt_campaign_def *definition = pt_campaign_on_map(map, campaign);

    if (map >= PT_MAP_COUNT || campaign >= PT_CAMPAIGN_COUNT) return false;
    if (order_count > 0u && order == NULL) return false;
    for (size_t index = 0u; index < order_count; ++index) {
        pt_sim_order_layout entry = order_entry_at(order, index);

        if (entry.wave == 0u || entry.wave > definition->wave_count ||
            pt_map_pad(map, entry.pad) == NULL ||
            entry.kind >= definition->fixture_count ||
            (entry.tier >= PT_MAX_TIER && entry.tier != PT_SIM_REPAIR_TIER))
            return false;
    }
    return true;
}

static bool apply_order_entry(pt_game *game,
                              const pt_sim_order_layout *entry)
{
    pt_fixture *fixture = pt_fixture_at_pad(game, entry->pad);

    if (entry->tier == PT_SIM_REPAIR_TIER) {
        if (fixture == NULL || fixture->kind != entry->kind) return false;
        return fixture->integrity == fixture->integrity_max || pt_fixture_repair(game, entry->pad);
    }
    if (fixture == NULL) {
        if (!pt_fixture_place(game, entry->pad, entry->kind)) return false;
        fixture = pt_fixture_at_pad(game, entry->pad);
    }
    if (fixture == NULL || fixture->kind != entry->kind) return false;
    while (fixture->tier < entry->tier) {
        if (!pt_fixture_upgrade(game, entry->pad)) return false;
        fixture = pt_fixture_at_pad(game, entry->pad);
        if (fixture == NULL) return false;
    }
    return true;
}

static uint32_t apply_order_for_wave(pt_game *game,
                                     const pt_sim_order_entry *order,
                                     size_t order_count,
                                     uint16_t wave)
{
    uint32_t failures = 0u;

    for (size_t index = 0u; index < order_count; ++index) {
        pt_sim_order_layout entry = order_entry_at(order, index);

        if (entry.wave == wave && !apply_order_entry(game, &entry))
            ++failures;
    }
    return failures;
}

static uint32_t fixture_count(const pt_game *game)
{
    uint32_t count = 0u;

    for (size_t index = 0u; index < PT_MAX_FIXTURES; ++index)
        if (game->fixtures[index].present) ++count;
    return count;
}

static uint32_t leak_count(
    const uint32_t leaks[PT_UNITS_PER_CAMPAIGN],
    uint16_t unit_count)
{
    uint32_t count = 0u;

    for (uint16_t kind = 0u; kind < unit_count; ++kind)
        count += leaks[kind];
    return count;
}

static void print_leaks(const pt_campaign_def *campaign,
                        const uint32_t leaks[PT_UNITS_PER_CAMPAIGN])
{
    (void)printf(" leaks=");
    for (uint16_t kind = 0u; kind < campaign->unit_count; ++kind) {
        const char *separator = kind == 0u ? "" : ",";
        (void)printf("%s%s:%" PRIu32, separator,
                     campaign->units[kind].id, leaks[kind]);
    }
}

static void print_invalid_result(uint8_t campaign)
{
    (void)printf(
        "PT_SIM_RESULT campaign=%" PRIu8
        " status=INVALID waves=0 leak_count=0 integrity=0/0"
        " earned=0 spent=0 unspent=0 fixtures_lost=0 seconds=0.000"
        " orders_failed=0 unit_overflow=0 projectile_overflow=0\n",
        campaign);
}

int pt_simulate_run(uint8_t campaign, const pt_sim_order_entry *order,
                    size_t order_count, bool verbose)
{
    return pt_simulate_map_run(0u, campaign, order, order_count, verbose);
}

int pt_simulate_map_run(uint8_t map, uint8_t campaign, const pt_sim_order_entry *order,
                        size_t order_count, bool verbose)
{
    pt_game game;
    const pt_campaign_def *campaign_definition;
    uint32_t total_leaks[PT_UNITS_PER_CAMPAIGN] = { 0u };
    uint64_t total_wave_ticks = 0u;
    uint32_t total_order_failures = 0u;
    uint16_t waves_run = 0u;
    bool stalled = false;

    if (!order_is_valid(map, campaign, order, order_count)) {
        print_invalid_result(campaign);
        return PT_SIM_ERROR;
    }

    campaign_definition = pt_campaign_on_map(map, campaign);
    pt_game_init_map(&game, map, campaign, PT_SIM_SEED);
    game.headless = true;

    while (game.phase == PT_PHASE_BUILD || game.phase == PT_PHASE_WAVE) {
        const pt_wave_def *wave;
        pt_sim_unit_snapshot snapshot[PT_MAX_UNITS];
        uint32_t wave_leaks[PT_UNITS_PER_CAMPAIGN] = { 0u };
        uint32_t earned_before;
        uint32_t spent_before;
        uint32_t fixtures_lost_before;
        uint32_t order_failures;
        uint64_t wave_ticks = 0u;
        const char *wave_status;

        if (game.phase != PT_PHASE_BUILD) {
            stalled = true;
            break;
        }
        wave = pt_game_wave(&game, game.wave.index);
        if (wave == NULL) {
            stalled = true;
            break;
        }

        earned_before = game.economy.earned_total;
        spent_before = game.economy.spent_total;
        fixtures_lost_before = game.fixtures_lost;
        order_failures = apply_order_for_wave(
            &game, order, order_count, wave->index);
        total_order_failures += order_failures;

        pt_game_call_wave_early(&game);
        pt_game_step(&game, PT_STEP_SECONDS);
        if (game.phase != PT_PHASE_WAVE) {
            stalled = true;
            break;
        }

        while (game.phase == PT_PHASE_WAVE &&
               wave_ticks < PT_SIM_WAVE_TICK_LIMIT) {
            int32_t integrity_before = game.economy.integrity;

            pt_decoy_update(&game);
            snapshot_units(&game, snapshot);
            pt_game_step(&game, PT_STEP_SECONDS);
            record_step_leaks(&game, snapshot, integrity_before, wave_leaks);
            ++wave_ticks;
        }
        if (game.phase == PT_PHASE_WAVE) stalled = true;

        for (uint16_t kind = 0u;
             kind < campaign_definition->unit_count; ++kind)
            total_leaks[kind] += wave_leaks[kind];
        total_wave_ticks += wave_ticks;
        ++waves_run;

        if (game.phase == PT_PHASE_DEFEAT)
            wave_status = "LOST";
        else if (stalled)
            wave_status = "STALLED";
        else
            wave_status = "COMPLETE";

        if (verbose) {
            (void)printf(
                "PT_SIM_WAVE campaign=%s wave=%" PRIu16
                " status=%s leak_count=%" PRIu32,
                campaign_definition->id, wave->index, wave_status,
                leak_count(wave_leaks, campaign_definition->unit_count));
            print_leaks(campaign_definition, wave_leaks);
            (void)printf(
                " integrity=%" PRId32 "/%" PRId32
                " earned=%" PRIu32 " spent=%" PRIu32
                " unspent=%" PRId32 " fixtures=%" PRIu32
                " fixtures_lost=%" PRIu32 " seconds=%.3f"
                " orders_failed=%" PRIu32 " unit_overflow=%" PRIu32
                " projectile_overflow=%" PRIu32 "\n",
                game.economy.integrity, game.economy.integrity_max,
                game.economy.earned_total - earned_before,
                game.economy.spent_total - spent_before,
                game.economy.currency, fixture_count(&game),
                game.fixtures_lost - fixtures_lost_before,
                (double)wave_ticks / (double)PT_TICK_HZ,
                order_failures, game.units.overflow,
                game.projectiles.overflow);
            (void)printf("PT_SIM_DEFENSES map=%s campaign=%s wave=%u",
                pt_map(map)->id, campaign_definition->id, (unsigned int)wave->index);
            for (size_t slot = 0u; slot < PT_MAX_FIXTURES; ++slot) {
                const pt_fixture *fixture = &game.fixtures[slot];
                if (!fixture->present) continue;
                (void)printf(" %u:%s:T%u:%dhp", (unsigned int)fixture->pad,
                    pt_fixture_def_at(campaign, fixture->kind)->id,
                    (unsigned int)fixture->tier + 1u, fixture->integrity);
            }
            (void)printf("\n");
        }
        if (stalled || game.phase == PT_PHASE_DEFEAT ||
            game.phase == PT_PHASE_VICTORY)
            break;
    }

    {
        const char *status;
        int result;

        if (game.phase == PT_PHASE_VICTORY) {
            status = "CLEARED";
            result = PT_SIM_CLEARED;
        } else if (game.phase == PT_PHASE_DEFEAT) {
            status = "LOST";
            result = PT_SIM_LOST;
        } else {
            status = "STALLED";
            result = PT_SIM_ERROR;
        }

        (void)printf(
            "PT_SIM_RESULT map=%s campaign=%s status=%s waves=%" PRIu16
            " leak_count=%" PRIu32,
            pt_map(map)->id, campaign_definition->id, status, waves_run,
            leak_count(total_leaks, campaign_definition->unit_count));
        print_leaks(campaign_definition, total_leaks);
        (void)printf(
            " integrity=%" PRId32 "/%" PRId32
            " earned=%" PRIu32 " spent=%" PRIu32
            " unspent=%" PRId32 " fixtures_lost=%" PRIu32
            " seconds=%.3f orders_failed=%" PRIu32
            " unit_overflow=%" PRIu32 " projectile_overflow=%" PRIu32
            "\n",
            game.economy.integrity, game.economy.integrity_max,
            game.economy.earned_total, game.economy.spent_total,
            game.economy.currency, game.fixtures_lost,
            (double)total_wave_ticks / (double)PT_TICK_HZ,
            total_order_failures, game.units.overflow,
            game.projectiles.overflow);
        return result;
    }
}

int pt_simulate_file(uint8_t campaign, const char *path)
{
    return pt_simulate_map_file(0u, campaign, path);
}

int pt_simulate_map_file(uint8_t map, uint8_t campaign, const char *path)
{
    pt_sim_order_entry order[256];
    size_t count = 0u;
    char line[256];
    FILE *file = fopen(path, "r");
    if (!file || campaign >= PT_CAMPAIGN_COUNT) {
        if (file) (void)fclose(file);
        return PT_SIM_ERROR;
    }
    while (fgets(line, sizeof line, file)) {
        unsigned int wave, pad, kind, tier;
        char extra;
        if (line[0] == '#' || line[0] == '\n') continue;
        if (count == sizeof order / sizeof order[0] ||
            sscanf(line, "%u %u %u %u %c", &wave, &pad, &kind, &tier, &extra) != 4 ||
            wave > UINT16_MAX || pad > UINT8_MAX ||
            kind > UINT16_MAX || tier > UINT8_MAX) {
            (void)fclose(file);
            return PT_SIM_ERROR;
        }
        order[count++] = (pt_sim_order_entry){
            (uint16_t)wave, (uint8_t)pad, (uint16_t)kind, (uint8_t)tier};
    }
    bool ok = !ferror(file);
    if (fclose(file) != 0) ok = false;
    if (!ok) return PT_SIM_ERROR;
    return pt_simulate_map_run(map, campaign, order, count, true);
}
