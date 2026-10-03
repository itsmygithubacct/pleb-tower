/* Unit-pool, scheduling, movement, status, shield, and emit tests. */
#include "pt_test.h"
#include "pleb_tower.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

static void reset_game(pt_game *game)
{
    memset(game, 0, sizeof *game);
    game->campaign = 0u;
    PT_CHECK(pt_board_init(&game->board, game->campaign), "board init");
    pt_units_reset(&game->units, game->campaign);
    pt_combat_reset(game);
    pt_fixtures_reset(game);
    pt_economy_reset(game);
}

static void test_pool_overflow_is_bounded(void)
{
    struct guarded_pool {
        uint32_t before;
        pt_unit_pool pool;
        uint32_t after;
    } guarded;

    memset(&guarded, 0, sizeof guarded);
    guarded.before = UINT32_C(0x13579bdf);
    guarded.after = UINT32_C(0x2468ace0);
    pt_units_reset(&guarded.pool, 0u);

    for (size_t i = 0; i < PT_MAX_UNITS; ++i)
        PT_CHECK(pt_units_spawn(&guarded.pool, 0u, (float)i, 0.5f) != NULL,
                 "spawn %zu fits", i);

    PT_CHECK_EQ_INT(guarded.pool.live, PT_MAX_UNITS);
    PT_CHECK(pt_units_spawn(&guarded.pool, 0u, 999.0f, 999.0f) == NULL,
             "newest spawn is dropped");
    PT_CHECK_EQ_INT(guarded.pool.overflow, 1);
    PT_CHECK_EQ_INT(guarded.pool.live, PT_MAX_UNITS);
    PT_CHECK_EQ_INT(guarded.before, UINT32_C(0x13579bdf));
    PT_CHECK_EQ_INT(guarded.after, UINT32_C(0x2468ace0));
}

static void test_wave_interleave_is_byte_exact(void)
{
    static const uint16_t expected[15] = {
        0u, 4u, 0u, 0u, 7u, 0u, 4u, 0u,
        0u, 0u, 4u, 0u, 0u, 4u, 0u
    };
    uint16_t actual[15] = { 0u };
    pt_game game;

    reset_game(&game);
    /* HOLDOUT wave 10 is the constructed three-group case:
     * siege×1, walker×10, splitter×4. */
    game.wave.index = 9u;
    game.wave.total = 15u;
    game.wave.active = true;
    for (size_t spawn = 0; spawn < 15u; ++spawn) {
        game.wave.spawn_timer = 0.0;
        pt_units_update(&game, 0.0);
        actual[spawn] = game.units.slots[spawn].kind;
    }

    PT_CHECK(memcmp(actual, expected, sizeof expected) == 0,
             "three-group smooth weighted interleave is byte-exact");
    PT_CHECK_EQ_INT(game.wave.spawned, 15);
}

static void test_hardened_ignores_hold_and_stun(void)
{
    pt_game game;
    const pt_campaign_def *campaign;
    pt_unit *hardened;
    pt_unit *normal;
    float start_x;
    float start_y;

    reset_game(&game);
    campaign = pt_campaign(game.campaign);
    start_x = (float)campaign->spawn_x + 0.5f;
    start_y = (float)campaign->spawn_y + 0.5f;
    hardened = pt_units_spawn(&game.units, 2u, start_x, start_y);
    normal = pt_units_spawn(&game.units, 0u, start_x, start_y);
    PT_CHECK(hardened != NULL && normal != NULL, "status test spawns");
    if (!hardened || !normal) return;

    hardened->hold_remaining = 1.0f;
    hardened->stun_remaining = 1.0f;
    normal->hold_remaining = 1.0f;
    normal->stun_remaining = 1.0f;
    pt_units_update(&game, 0.5);

    PT_CHECK(hardened->x != start_x || hardened->y != start_y,
             "hardened unit moves through both effects");
    PT_CHECK_EQ_INT(hardened->hold_remaining, 0);
    PT_CHECK_EQ_INT(hardened->stun_remaining, 0);
    PT_CHECK(fabsf(normal->x - start_x) < 1e-6f &&
             fabsf(normal->y - start_y) < 1e-6f,
             "non-hardened unit is frozen");
    PT_CHECK(fabsf(normal->hold_remaining - 0.5f) < 1e-6f,
             "hold timer counts down");
    PT_CHECK(fabsf(normal->stun_remaining - 0.5f) < 1e-6f,
             "stun timer counts down");
}

static void test_shield_regenerates_at_exact_delay(void)
{
    pt_game game;
    pt_unit *repair;

    reset_game(&game);
    repair = pt_units_spawn(&game.units, 6u, 10.5f, 10.5f);
    PT_CHECK(repair != NULL, "repair unit spawn");
    if (!repair) return;
    repair->shield = 0;

    pt_units_update(&game, 2.999);
    PT_CHECK_EQ_INT(repair->shield, 0);
    pt_units_update(&game, 0.001);
    PT_CHECK_EQ_INT(repair->shield, 60);
}

static void test_air_ignores_flow_field(void)
{
    pt_game game;
    const pt_campaign_def *campaign;
    const pt_unit_def *definition;
    pt_unit *air;
    float start_x = 5.5f;
    float start_y = 5.5f;
    float goal_x;
    float goal_y;
    float dx;
    float dy;
    float length;
    float expected_x;
    float expected_y;

    reset_game(&game);
    memset(game.board.flow_x, -1, sizeof game.board.flow_x);
    memset(game.board.flow_y, 1, sizeof game.board.flow_y);
    campaign = pt_campaign(game.campaign);
    definition = pt_unit_def_at(game.campaign, 3u);
    air = pt_units_spawn(&game.units, 3u, start_x, start_y);
    PT_CHECK(air != NULL && definition != NULL, "air unit spawn");
    if (!air || !definition) return;

    goal_x = (float)campaign->goal_x + 0.5f;
    goal_y = (float)campaign->goal_y + 0.5f;
    dx = goal_x - start_x;
    dy = goal_y - start_y;
    length = sqrtf(dx * dx + dy * dy);
    expected_x = start_x + dx / length * definition->speed;
    expected_y = start_y + dy / length * definition->speed;

    pt_units_update(&game, 1.0);
    PT_CHECK(fabsf(air->x - expected_x) < 1e-5f,
             "air x follows the straight segment");
    PT_CHECK(fabsf(air->y - expected_y) < 1e-5f,
             "air y follows the straight segment");
}

static void test_emit_produces_exact_count_each_period(void)
{
    pt_game game;
    pt_unit *siege;

    reset_game(&game);
    siege = pt_units_spawn(&game.units, 7u, 10.5f, 10.5f);
    PT_CHECK(siege != NULL, "siege spawn");
    if (!siege) return;

    pt_units_update(&game, 3.999);
    PT_CHECK_EQ_INT(game.units.live, 1);
    pt_units_update(&game, 0.001);
    PT_CHECK_EQ_INT(game.units.live, 3);
    PT_CHECK_EQ_INT(game.units.slots[1].kind, 0);
    PT_CHECK_EQ_INT(game.units.slots[2].kind, 0);

    pt_units_update(&game, 4.0);
    PT_CHECK_EQ_INT(game.units.live, 5);
    PT_CHECK_EQ_INT(game.units.overflow, 0);

    /* Authored emitters cap at ten children per emitter. */
    pt_units_update(&game, 12.0);
    PT_CHECK_EQ_INT(game.units.live, 11);
    pt_units_update(&game, 4.0);
    PT_CHECK_EQ_INT(game.units.live, 11);
}

static void test_stun_stops_ranged_attacks(void)
{
    pt_game game;
    reset_game(&game);
    game.economy.currency = 1000;
    PT_CHECK(pt_fixture_place(&game, 10u, 0u), "place stun-test target");
    const pt_pad_def *pad = pt_pad(10u);
    pt_fixture *fixture = pt_fixture_at_pad(&game, 10u);
    pt_unit *enemy = pt_units_spawn(&game.units, 5u,
        (float)pad->x + 0.5f, (float)pad->y + 0.5f);
    if (!fixture || !enemy) return;
    int32_t before = fixture->integrity;
    enemy->stun_remaining = 1.0f;
    pt_units_update(&game, 0.5);
    pt_units_update(&game, 0.5);
    PT_CHECK_EQ_INT(fixture->integrity, before);
    pt_units_update(&game, 0.5);
    PT_CHECK(fixture->integrity < before, "ranged attacks resume after stun expires");
}

static void test_vision_reduces_range_and_accuracy(void)
{
    pt_game game;
    reset_game(&game);
    game.economy.currency = 1000;
    PT_CHECK(pt_fixture_place(&game, 8u, 6u), "place Floodlight");
    PT_CHECK(pt_fixture_upgrade(&game, 8u), "upgrade its coverage");
    pt_fixture *fixture = pt_fixture_at_pad(&game, 8u);
    const pt_pad_def *pad = pt_pad(8u);
    pt_unit *enemy = pt_units_spawn(&game.units, 5u,
        (float)pad->x + 0.5f, (float)pad->y + 0.5f - 2.9f);
    if (!fixture || !enemy) return;
    int32_t before = fixture->integrity;
    pt_units_update(&game, 0.0);
    PT_CHECK_EQ_INT(enemy->halted, 0);
    PT_CHECK_EQ_INT(fixture->integrity, before);
    /* At contact distance the range reduction cannot save the tower, but
     * accuracy still reduces deterministic damage rather than rolling RNG. */
    enemy->x = (float)pad->x + 0.5f;
    enemy->y = (float)pad->y + 0.5f;
    pt_units_update(&game, 1.0);
    const pt_unit_def *definition = pt_unit_def_at(0u, 5u);
    const pt_tier_def *tier = &pt_fixture_def_at(0u, 6u)->tiers[1];
    int32_t expected = (int32_t)floorf(
        definition->attack_dps * (1.0f - tier->enemy_accuracy_penalty));
    PT_CHECK_EQ_INT(fixture->integrity, before - expected);
}

static void test_decoy_shares_stopping_power(void)
{
    static const size_t counts[] = {1u, 2u, 3u, 4u, 8u, PT_MAX_UNITS};
    for (uint8_t campaign = 0u; campaign < PT_CAMPAIGN_COUNT; ++campaign) {
        for (uint8_t tier = 0u; tier < PT_MAX_TIER; ++tier) {
            pt_game game;
            pt_game_init(&game, campaign, UINT64_C(0xdec0));
            game.headless = true;
            game.economy.currency = 1000;
            PT_CHECK(pt_fixture_place(&game, 8u, 3u), "place decoy fixture");
            for (uint8_t upgrade = 0u; upgrade < tier; ++upgrade)
                PT_CHECK(pt_fixture_upgrade(&game, 8u), "upgrade decoy coverage");
            const float speed = pt_unit_def_at(campaign, 0u)->speed;
            for (size_t cohort = 0u; cohort < sizeof counts / sizeof counts[0]; ++cohort) {
                pt_units_reset(&game.units, game.campaign);
                size_t count = counts[cohort];
                for (size_t u = 0u; u < count; ++u)
                    PT_CHECK(pt_units_spawn(&game.units, 0u, 5.5f, 8.5f) != NULL,
                        "spawn decoy target");
                pt_units_update(&game, 0.1);
                float expected = speed * 0.1f * (1.0f - 1.0f / (float)count);
                bool correct = true, on_lane = true;
                for (size_t u = 0u; u < count; ++u) {
                    const pt_unit *unit = &game.units.slots[u];
                    float travelled = hypotf(unit->x - 5.5f, unit->y - 8.5f);
                    if (fabsf(travelled - expected) > 0.00001f ||
                        unit->hold_remaining != 0.0f) correct = false;
                    int x = (int)floorf(unit->x), y = (int)floorf(unit->y);
                    if (!game.board.lane[pt_cell_index(x, y)]) on_lane = false;
                }
                PT_CHECK(correct, "campaign %u tier %u shares one stop across %zu units",
                    campaign, tier, count);
                PT_CHECK(on_lane, "slowed units continue along the road");
            }

            pt_units_reset(&game.units, game.campaign);
            pt_unit *first = pt_units_spawn(&game.units, 0u, 5.5f, 8.5f);
            pt_unit *second = pt_units_spawn(&game.units, 0u, 5.5f, 8.5f);
            if (!first || !second) { PT_CHECK(false, "spawn changing decoy targets"); continue; }
            pt_units_update(&game, 0.1);
            second->x = 23.5f; second->y = 2.5f;
            float x = first->x, y = first->y;
            pt_units_update(&game, 0.1);
            PT_CHECK(first->x == x && first->y == y, "one remaining target stops immediately");
            PT_CHECK(second->decoy_slow == 0.0f, "leaving coverage restores full speed");
            second->x = first->x; second->y = first->y;
            pt_units_update(&game, 0.1);
            PT_CHECK(fabsf(hypotf(first->x - x, first->y - y) - speed * 0.05f) < 0.00001f,
                "a returning target immediately shares the stop");
            second->alive = 0u; --game.units.live;
            x = first->x; y = first->y;
            for (int tick = 0; tick < 240; ++tick) pt_units_update(&game, PT_STEP_SECONDS);
            PT_CHECK(first->x == x && first->y == y, "a dead target no longer dilutes the full stop");
            PT_CHECK(pt_fixture_sell(&game, 8u), "remove decoy fixture");
            pt_units_update(&game, 0.1);
            PT_CHECK(fabsf(hypotf(first->x - x, first->y - y) - speed * 0.1f) < 0.00001f,
                "selling a decoy restores full speed on the next tick without a lingering hold");
            for (int tick = 0; tick < 6000 && first->alive; ++tick)
                pt_units_update(&game, PT_STEP_SECONDS);
            PT_CHECK(!first->alive, "released decoy target reaches the goal");
        }
    }
}

static void test_decoy_immunity_overlap_and_coverage(void)
{
    pt_game game;
    reset_game(&game);
    game.headless = true;
    game.economy.currency = 2000;
    PT_CHECK(pt_fixture_place(&game, 8u, 3u), "place immunity test decoy");
    pt_unit *normal = pt_units_spawn(&game.units, 0u, 5.5f, 8.5f);
    pt_unit *hardened = pt_units_spawn(&game.units, 2u, 5.5f, 8.5f);
    pt_unit *air = pt_units_spawn(&game.units, 3u, 5.5f, 8.5f);
    if (!normal || !hardened || !air) { PT_CHECK(false, "spawn immunity targets"); return; }
    pt_units_update(&game, 0.1);
    PT_CHECK(normal->x == 5.5f && normal->y == 8.5f,
        "immune enemies do not consume the ground target's stopping power");
    PT_CHECK(hardened->decoy_slow == 0.0f && air->decoy_slow == 0.0f,
        "air and hardened units remain immune");
    PT_CHECK(hypotf(hardened->x - 5.5f, hardened->y - 8.5f) > 0.0f &&
        hypotf(air->x - 5.5f, air->y - 8.5f) > 0.0f, "immune enemies keep moving");

    pt_units_reset(&game.units, game.campaign);
    PT_CHECK(pt_fixture_place(&game, 4u, 3u), "place overlapping decoy");
    normal = pt_units_spawn(&game.units, 0u, 5.5f, 8.5f);
    pt_unit *second = pt_units_spawn(&game.units, 0u, 5.5f, 8.5f);
    if (!normal || !second) return;
    pt_units_update(&game, 0.1);
    PT_CHECK(fabsf(hypotf(normal->x - 5.5f, normal->y - 8.5f) -
        pt_unit_def_at(0u, 0u)->speed * 0.05f) < 0.00001f,
        "overlapping decoys cannot stack into a crowd-wide full stop");
    normal->hold_remaining = 0.2f;
    float x = normal->x, y = normal->y;
    pt_units_update(&game, 0.1);
    PT_CHECK(normal->x == x && normal->y == y && normal->hold_remaining > 0.0f,
        "decoy sharing preserves a separate entangle hold");

    PT_CHECK(pt_fixture_sell(&game, 4u), "remove overlapping decoy");
    pt_units_reset(&game.units, game.campaign);
    const pt_pad_def *pad = pt_pad(8u);
    normal = pt_units_spawn(&game.units, 0u, (float)pad->x + 3.5f, (float)pad->y + 0.5f);
    if (!normal) return;
    pt_decoy_update(&game);
    PT_CHECK(normal->decoy_slow == 1.0f, "displayed radius boundary is included");
    normal->x += 0.001f;
    pt_decoy_update(&game);
    PT_CHECK(normal->decoy_slow == 0.0f, "outside the displayed circle is unaffected");
    PT_CHECK(pt_fixture_upgrade(&game, 8u), "upgrade decoy radius");
    pt_decoy_update(&game);
    PT_CHECK(normal->decoy_slow == 1.0f, "upgrading extends the decoy effect's coverage");
    pt_combat_damage_fixture(&game, pt_fixture_at_pad(&game, 8u), INT32_MAX);
    pt_decoy_update(&game);
    PT_CHECK(normal->decoy_slow == 0.0f, "destroying a decoy clears its effect");
}

static void test_decoy_counts_new_spawns_before_movement(void)
{
    pt_game game;
    reset_game(&game);
    game.headless = true;
    game.economy.currency = 1000;
    PT_CHECK(pt_fixture_place(&game, 4u, 3u), "place decoy near spawn");
    PT_CHECK(pt_fixture_upgrade(&game, 4u), "extend decoy to spawn");
    const pt_campaign_def *campaign = pt_campaign(0u);
    float x = (float)campaign->spawn_x + 0.5f;
    float y = (float)campaign->spawn_y + 0.5f;
    pt_unit *waiting = pt_units_spawn(&game.units, 0u, x, y);
    if (!waiting) return;
    game.wave.active = true;
    pt_units_update(&game, 0.1);
    PT_CHECK_EQ_INT(game.units.live, 2);
    PT_CHECK(fabsf(hypotf(waiting->x - x, waiting->y - y) -
        pt_unit_def_at(0u, 0u)->speed * 0.05f) < 0.00001f,
        "this tick's scheduled spawn shares the stopping power immediately");
    PT_CHECK(fabsf(game.units.slots[1].x - waiting->x) < 0.00001f,
        "all targets get the same slowdown regardless of slot order");
}

void pt_test_units(void)
{
    test_pool_overflow_is_bounded();
    test_wave_interleave_is_byte_exact();
    test_hardened_ignores_hold_and_stun();
    test_shield_regenerates_at_exact_delay();
    test_air_ignores_flow_field();
    test_emit_produces_exact_count_each_period();
    test_stun_stops_ranged_attacks();
    test_vision_reduces_range_and_accuracy();
    test_decoy_shares_stopping_power();
    test_decoy_immunity_overlap_and_coverage();
    test_decoy_counts_new_spawns_before_movement();
}
