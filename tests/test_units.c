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
    pt_units_reset(&game->units);
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
    pt_units_reset(&guarded.pool);

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

void pt_test_units(void)
{
    test_pool_overflow_is_bounded();
    test_wave_interleave_is_byte_exact();
    test_hardened_ignores_hold_and_stun();
    test_shield_regenerates_at_exact_delay();
    test_air_ignores_flow_field();
    test_emit_produces_exact_count_each_period();
}
