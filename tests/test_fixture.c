/* Fixture economy, deterministic targeting, support caching, status immunity,
 * and transactional reroute tests. */
#include "pt_test.h"
#include "pleb_tower.h"

#include <math.h>
#include <string.h>

static void init_game(pt_game *game)
{
    pt_game_init(game, 0u, UINT64_C(0x1234));
}

static void test_place_upgrade_sell_arithmetic(void)
{
    pt_game game;
    pt_fixture *fixture;

    init_game(&game);
    PT_CHECK(pt_fixture_place(&game, 10u, 0u), "place Rail Spike");
    PT_CHECK_EQ_INT(game.economy.currency, 90);
    PT_CHECK(pt_fixture_upgrade(&game, 10u), "upgrade Rail Spike");
    PT_CHECK_EQ_INT(game.economy.currency, 20);
    fixture = pt_fixture_at_pad(&game, 10u);
    PT_CHECK(fixture != NULL, "upgraded fixture remains present");
    if (fixture != NULL) {
        PT_CHECK_EQ_INT(fixture->tier, 1);
        PT_CHECK_EQ_INT(fixture->invested, 130);
    }
    PT_CHECK(pt_fixture_sell(&game, 10u), "sell upgraded Rail Spike");
    PT_CHECK_EQ_INT(game.economy.currency, 111);
    PT_CHECK_EQ_INT(game.economy.spent_total, 130);
    PT_CHECK_EQ_INT(game.economy.earned_total, 0);
    PT_CHECK(pt_fixture_at_pad(&game, 10u) == NULL,
             "sold pad must be empty");
}

static void test_sell_refund_floors(void)
{
    pt_game game;
    pt_fixture *fixture;

    init_game(&game);
    PT_CHECK(pt_fixture_place(&game, 10u, 0u), "place for floor test");
    fixture = pt_fixture_at_pad(&game, 10u);
    PT_CHECK(fixture != NULL, "fixture exists for repair");
    if (fixture == NULL) return;
    --fixture->integrity;
    PT_CHECK(pt_fixture_repair(&game, 10u), "one-point partial repair");
    PT_CHECK_EQ_INT(fixture->invested, 61);
    PT_CHECK(pt_fixture_sell(&game, 10u), "sell 61 invested");
    /* floor(61 * 0.70) = 42, so 150 - 61 + 42 = 131. */
    PT_CHECK_EQ_INT(game.economy.currency, 131);
}

static pt_unit *put_unit(pt_game *game, size_t slot, float x, float y,
                         int32_t integrity, uint16_t serial)
{
    pt_unit *unit = &game->units.slots[slot];

    (void)memset(unit, 0, sizeof *unit);
    unit->x = x;
    unit->y = y;
    unit->integrity = integrity;
    unit->kind = 0u;
    unit->serial = serial;
    unit->alive = 1u;
    ++game->units.live;
    return unit;
}

static uint16_t fire_and_target(pt_game *game, pt_fixture *fixture,
                                pt_target_mode mode)
{
    size_t index;

    (void)memset(&game->projectiles, 0, sizeof game->projectiles);
    fixture->mode = (uint8_t)mode;
    fixture->cooldown = 0.0f;
    pt_fixtures_update(game, 0.0);
    for (index = 0u; index < PT_MAX_PROJECTILES; ++index)
        if (game->projectiles.slots[index].alive)
            return game->projectiles.slots[index].target_serial;
    return UINT16_MAX;
}

static void test_targeting_comparators(void)
{
    pt_game game;
    pt_fixture *fixture;
    pt_target_mode mode;

    init_game(&game);
    PT_CHECK(pt_fixture_place(&game, 10u, 0u), "place targeting fixture");
    fixture = pt_fixture_at_pad(&game, 10u);
    PT_CHECK(fixture != NULL, "targeting fixture exists");
    if (fixture == NULL) return;

    (void)memset(&game.units, 0, sizeof game.units);
    (void)put_unit(&game, 0u, 8.5f, 8.5f, 100, 30u);
    (void)put_unit(&game, 1u, 9.5f, 8.5f, 500, 20u);
    (void)put_unit(&game, 2u, 10.5f, 8.5f, 200, 10u);

    PT_CHECK_EQ_INT(fire_and_target(
                        &game, fixture, PT_TARGET_FIRST),
                    30);
    PT_CHECK_EQ_INT(fire_and_target(
                        &game, fixture, PT_TARGET_LAST),
                    10);
    PT_CHECK_EQ_INT(fire_and_target(
                        &game, fixture, PT_TARGET_STRONGEST),
                    20);
    PT_CHECK_EQ_INT(fire_and_target(
                        &game, fixture, PT_TARGET_CLOSEST),
                    10);

    /* Identical primary keys force every comparator through the serial rule. */
    (void)memset(&game.units, 0, sizeof game.units);
    (void)put_unit(&game, 0u, 10.5f, 8.5f, 250, 9u);
    (void)put_unit(&game, 1u, 10.5f, 8.5f, 250, 2u);
    for (mode = PT_TARGET_FIRST; mode < PT_TARGET_MODE_COUNT;
         mode = (pt_target_mode)((int)mode + 1))
        PT_CHECK_EQ_INT(fire_and_target(&game, fixture, mode), 2);
}

static void test_support_cache_invalidation(void)
{
    pt_game game;
    pt_fixture *target;

    init_game(&game);
    game.economy.currency = 5000;
    PT_CHECK(pt_fixture_place(&game, 18u, 0u), "place buff target");
    target = pt_fixture_at_pad(&game, 18u);
    PT_CHECK(target != NULL, "buff target exists");
    if (target == NULL) return;
    PT_CHECK(fabsf(target->damage_scale - 1.0f) < 0.0001f,
             "target starts unbuffed");

    /* Pads 18 and 19 are sqrt(2) cells apart. */
    PT_CHECK(pt_fixture_place(&game, 19u, 7u), "place adjacent support");
    PT_CHECK(fabsf(target->damage_scale - 1.15f) < 0.0001f,
             "placement refreshes support damage");
    PT_CHECK(fabsf(target->range_scale - 1.10f) < 0.0001f,
             "placement refreshes support range");
    PT_CHECK_EQ_INT(target->currency_tick, 1);

    PT_CHECK(pt_fixture_upgrade(&game, 19u), "upgrade support to tier two");
    PT_CHECK(fabsf(target->damage_scale - 1.25f) < 0.0001f,
             "upgrade refreshes support damage");
    PT_CHECK_EQ_INT(target->currency_tick, 2);
    PT_CHECK(pt_fixture_upgrade(&game, 19u), "upgrade support to tier three");
    PT_CHECK(fabsf(target->damage_scale - 1.40f) < 0.0001f,
             "largest upgraded bonus cached");
    PT_CHECK(pt_fixture_sell(&game, 19u), "sell adjacent support");
    PT_CHECK(fabsf(target->damage_scale - 1.0f) < 0.0001f,
             "selling support invalidates cache");

    /* Pads 3 and 5 both cover pad 4 only at tier three. */
    init_game(&game);
    game.economy.currency = 5000;
    PT_CHECK(pt_fixture_place(&game, 4u, 0u), "place overlap target");
    PT_CHECK(pt_fixture_place(&game, 3u, 7u), "place left support");
    PT_CHECK(pt_fixture_upgrade(&game, 3u), "upgrade left support twice 1");
    PT_CHECK(pt_fixture_upgrade(&game, 3u), "upgrade left support twice 2");
    PT_CHECK(pt_fixture_place(&game, 5u, 7u), "place right support");
    PT_CHECK(pt_fixture_upgrade(&game, 5u), "upgrade right support twice 1");
    PT_CHECK(pt_fixture_upgrade(&game, 5u), "upgrade right support twice 2");
    target = pt_fixture_at_pad(&game, 4u);
    PT_CHECK(target != NULL, "overlap target exists");
    if (target == NULL) return;
    PT_CHECK(fabsf(target->damage_scale - 1.40f) < 0.0001f,
             "two supports do not stack damage");
    PT_CHECK(fabsf(target->range_scale - 1.20f) < 0.0001f,
             "two supports do not stack range");
    PT_CHECK_EQ_INT(target->currency_tick, 3);
    PT_CHECK(pt_fixture_sell(&game, 3u), "sell one overlapping support");
    PT_CHECK(fabsf(target->damage_scale - 1.40f) < 0.0001f,
             "remaining support keeps largest cache");
    PT_CHECK(pt_fixture_sell(&game, 5u), "sell final overlapping support");
    PT_CHECK(fabsf(target->damage_scale - 1.0f) < 0.0001f,
             "final support sale clears cache");
}

static void test_hardened_refuses_hold_and_stun(void)
{
    pt_game game;
    pt_unit *unit;

    init_game(&game);
    PT_CHECK(pt_fixture_place(&game, 10u, 2u), "place Entangler");
    (void)memset(&game.units, 0, sizeof game.units);
    unit = put_unit(&game, 0u, 10.5f, 8.5f, 520, 1u);
    unit->kind = 2u; /* Breacher: hardened. */
    pt_fixtures_update(&game, 0.0);
    PT_CHECK(fabsf(unit->hold_remaining) < 0.0001f,
             "hardened unit refuses hold");

    init_game(&game);
    PT_CHECK(pt_fixture_place(&game, 10u, 5u), "place EMP Coil");
    (void)memset(&game.units, 0, sizeof game.units);
    unit = put_unit(&game, 0u, 10.5f, 8.5f, 520, 1u);
    unit->kind = 2u;
    pt_fixtures_update(&game, 0.0);
    PT_CHECK(fabsf(unit->stun_remaining) < 0.0001f,
             "hardened unit refuses stun");
}

static void test_reroute_failure_is_atomic(void)
{
    pt_game game;
    pt_board before;
    pt_economy economy_before;

    init_game(&game);
    game.board.lane[game.board.spawn_cell] = 0u;
    before = game.board;
    economy_before = game.economy;
    PT_CHECK(!pt_fixture_place(&game, 10u, 3u),
             "unreachable reroute placement is refused");
    PT_CHECK(memcmp(&game.board, &before, sizeof before) == 0,
             "failed reroute leaves board byte-identical");
    PT_CHECK(memcmp(&game.economy, &economy_before,
                    sizeof economy_before) == 0,
             "failed reroute refunds purchase atomically");
    PT_CHECK(pt_fixture_at_pad(&game, 10u) == NULL,
             "failed reroute leaves pad empty");
}

static void test_cycle_mode_wraps(void)
{
    pt_game game;
    pt_fixture *fixture;
    int count;

    init_game(&game);
    PT_CHECK(pt_fixture_place(&game, 10u, 0u), "place mode fixture");
    fixture = pt_fixture_at_pad(&game, 10u);
    if (fixture == NULL) return;
    for (count = 0; count < PT_TARGET_MODE_COUNT; ++count)
        pt_fixture_cycle_mode(&game, 10u);
    PT_CHECK_EQ_INT(fixture->mode, PT_TARGET_FIRST);
}

void pt_test_fixture(void)
{
    test_place_upgrade_sell_arithmetic();
    test_sell_refund_floors();
    test_targeting_comparators();
    test_support_cache_invalidation();
    test_hardened_refuses_hold_and_stun();
    test_reroute_failure_is_atomic();
    test_cycle_mode_wraps();
}
