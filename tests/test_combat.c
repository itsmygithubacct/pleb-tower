/* Seven-step damage, projectile, death-hook, and fixture-damage tests. */
#include "pt_test.h"
#include "pleb_tower.h"

#include <math.h>
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

static pt_unit *spawn_unit(pt_game *game, uint16_t kind, float x, float y)
{
    pt_unit *unit = pt_units_spawn(&game->units, kind, x, y);
    PT_CHECK(unit != NULL, "spawn unit kind %u", (unsigned int)kind);
    return unit;
}

static pt_projectile *put_projectile(pt_game *game, const pt_unit *target,
                                     int32_t damage, uint8_t damage_type)
{
    pt_projectile *projectile = &game->projectiles.slots[0];

    memset(projectile, 0, sizeof *projectile);
    projectile->x = target->x;
    projectile->y = target->y;
    projectile->target_x = target->x;
    projectile->target_y = target->y;
    projectile->speed = 12.0f;
    projectile->damage = damage;
    projectile->target_serial = target->serial;
    projectile->damage_type = damage_type;
    projectile->alive = 1u;
    game->projectiles.live = 1u;
    return projectile;
}

static void test_step_1_base_damage_for_tier(void)
{
    pt_game game;
    pt_unit *unit;

    reset_game(&game);
    unit = spawn_unit(&game, 0u, 10.5f, 10.5f);
    if (!unit) return;
    PT_CHECK_EQ_INT(pt_combat_apply_damage(
                        &game, unit, 10, PT_DAMAGE_IMPACT, 0u),
                    10);
    PT_CHECK_EQ_INT(unit->integrity, 65);
}

static void test_step_2_largest_support_buff(void)
{
    pt_game game;
    pt_unit *unit;
    pt_fixture *source;
    int32_t applied;

    reset_game(&game);
    unit = spawn_unit(&game, 0u, 10.5f, 10.5f);
    if (!unit) return;
    source = &game.fixtures[0];
    source->present = 1u;
    source->pad = 1u;
    source->kind = 0u;
    source->damage_scale = 1.4f;
    source->range_scale = 1.0f;

    applied = pt_combat_apply_damage(
        &game, unit, 10, PT_DAMAGE_PIERCE, 1u);
    PT_CHECK_EQ_INT(applied, 14);
    PT_CHECK_EQ_INT(unit->integrity, 61);
}

static void test_step_3_vulnerability_hook_is_neutral_in_v1(void)
{
    pt_game game;
    pt_unit *unit;

    reset_game(&game);
    unit = spawn_unit(&game, 0u, 10.5f, 10.5f);
    if (!unit) return;
    PT_CHECK_EQ_INT(pt_combat_apply_damage(
                        &game, unit, 7, PT_DAMAGE_PIERCE, 0u),
                    7);
    PT_CHECK_EQ_INT(unit->integrity, 68);
}

static void test_step_4_armour_unless_pierce(void)
{
    pt_game game;
    pt_unit *impact_target;
    pt_unit *pierce_target;

    reset_game(&game);
    impact_target = spawn_unit(&game, 1u, 10.5f, 10.5f);
    pierce_target = spawn_unit(&game, 1u, 11.5f, 10.5f);
    if (!impact_target || !pierce_target) return;

    PT_CHECK_EQ_INT(pt_combat_apply_damage(
                        &game, impact_target, 10, PT_DAMAGE_IMPACT, 0u),
                    6);
    PT_CHECK_EQ_INT(impact_target->integrity, 154);
    PT_CHECK_EQ_INT(pt_combat_apply_damage(
                        &game, pierce_target, 10, PT_DAMAGE_PIERCE, 0u),
                    10);
    PT_CHECK_EQ_INT(pierce_target->integrity, 150);
}

static void test_step_5_shield_splash_and_overflow(void)
{
    pt_game game;
    pt_unit *shielded;
    pt_unit *unshielded;
    pt_unit *overflow;

    reset_game(&game);
    shielded = spawn_unit(&game, 0u, 10.5f, 10.5f);
    unshielded = spawn_unit(&game, 0u, 11.5f, 10.5f);
    overflow = spawn_unit(&game, 0u, 12.5f, 10.5f);
    if (!shielded || !unshielded || !overflow) return;
    shielded->shield = 20;
    overflow->shield = 3;

    PT_CHECK_EQ_INT(pt_combat_apply_damage(
                        &game, shielded, 10, PT_DAMAGE_SPLASH, 0u),
                    5);
    PT_CHECK_EQ_INT(shielded->shield, 15);
    PT_CHECK_EQ_INT(shielded->integrity, 75);

    PT_CHECK_EQ_INT(pt_combat_apply_damage(
                        &game, unshielded, 10, PT_DAMAGE_SPLASH, 0u),
                    10);
    PT_CHECK_EQ_INT(unshielded->integrity, 65);

    PT_CHECK_EQ_INT(pt_combat_apply_damage(
                        &game, overflow, 10, PT_DAMAGE_SPLASH, 0u),
                    5);
    PT_CHECK_EQ_INT(overflow->shield, 0);
    PT_CHECK_EQ_INT(overflow->integrity, 73);
}

static void test_step_6_floor_at_one(void)
{
    pt_game game;
    pt_unit *armoured;

    reset_game(&game);
    /* The authored maximum flat armour is 10; it exercises the same
     * 1-damage-into-overwhelming-armour floor as the normative 99 example. */
    armoured = spawn_unit(&game, 8u, 10.5f, 10.5f);
    if (!armoured) return;
    armoured->shield = 0;
    PT_CHECK_EQ_INT(pt_combat_apply_damage(
                        &game, armoured, 1, PT_DAMAGE_IMPACT, 0u),
                    1);
    PT_CHECK_EQ_INT(armoured->integrity, 6499);
}

static void test_step_7_death_award_support_tick_and_hook(void)
{
    pt_game game;
    pt_unit *walker;
    pt_projectile *projectile;

    reset_game(&game);
    game.economy.currency = 0;
    game.economy.earned_total = 0u;
    game.fixtures[0].present = 1u;
    game.fixtures[0].pad = 1u;
    game.fixtures[0].kind = 0u;
    game.fixtures[0].damage_scale = 1.0f;
    game.fixtures[0].range_scale = 1.0f;
    game.fixtures[0].currency_tick = 3u;
    walker = spawn_unit(&game, 0u, 10.5f, 10.5f);
    if (!walker) return;
    walker->integrity = 1;
    projectile = put_projectile(&game, walker, 1, PT_DAMAGE_PIERCE);
    projectile->source_pad = 1u;
    pt_combat_update(&game, 0.0);
    PT_CHECK_EQ_INT(game.economy.currency, 9);
    PT_CHECK_EQ_INT(game.economy.earned_total, 9);
    PT_CHECK_EQ_INT(game.units.live, 0);

    reset_game(&game);
    game.economy.currency = 0;
    game.economy.earned_total = 0u;
    {
        pt_unit *splitter = spawn_unit(&game, 4u, 10.5f, 10.5f);
        if (!splitter) return;
        splitter->integrity = 1;
        (void)pt_combat_apply_damage(
            &game, splitter, 1, PT_DAMAGE_PIERCE, 0u);
    }
    PT_CHECK_EQ_INT(game.economy.currency, 8);
    PT_CHECK_EQ_INT(game.units.live, 2);
    PT_CHECK_EQ_INT(game.units.slots[0].kind, 0);
    PT_CHECK_EQ_INT(game.units.slots[0].integrity, 40);
    PT_CHECK_EQ_INT(game.units.slots[1].kind, 0);
    PT_CHECK_EQ_INT(game.units.slots[1].integrity, 40);
}

static void test_threshold_fires_once(void)
{
    pt_game game;
    pt_unit *boss;

    reset_game(&game);
    boss = spawn_unit(&game, 8u, 10.5f, 10.5f);
    if (!boss) return;
    boss->shield = 0;

    (void)pt_combat_apply_damage(
        &game, boss, 3300, PT_DAMAGE_PIERCE, 0u);
    PT_CHECK_EQ_INT(boss->threshold_fired, 1);
    PT_CHECK_EQ_INT(game.units.live, 5);
    (void)pt_combat_apply_damage(
        &game, boss, 1, PT_DAMAGE_PIERCE, 0u);
    PT_CHECK_EQ_INT(game.units.live, 5);
}

static void test_projectile_advance_and_pierce_chain(void)
{
    pt_game game;
    pt_unit *first;
    pt_unit *second;
    pt_projectile *projectile;

    reset_game(&game);
    first = spawn_unit(&game, 0u, 2.5f, 5.5f);
    second = spawn_unit(&game, 0u, 4.5f, 5.5f);
    if (!first || !second) return;
    projectile = put_projectile(&game, first, 10, PT_DAMAGE_PIERCE);
    projectile->x = 0.5f;
    projectile->y = 5.5f;
    projectile->speed = 2.0f;
    projectile->pierce_remaining = 1u;

    pt_combat_update(&game, 0.5);
    PT_CHECK_EQ_INT(first->integrity, 75);
    PT_CHECK(projectile->alive, "projectile advances before impact");
    pt_combat_update(&game, 0.5);
    PT_CHECK_EQ_INT(first->integrity, 65);
    PT_CHECK(projectile->alive, "pierce retargets after first hit");
    PT_CHECK_EQ_INT(projectile->target_serial, second->serial);
    pt_combat_update(&game, 1.0);
    PT_CHECK_EQ_INT(second->integrity, 65);
    PT_CHECK(!projectile->alive, "pierce expires at its limit");
    PT_CHECK_EQ_INT(game.projectiles.live, 0);
}

static void test_projectile_splash_radius(void)
{
    pt_game game;
    pt_unit *primary;
    pt_unit *nearby;
    pt_unit *outside;
    pt_projectile *projectile;

    reset_game(&game);
    primary = spawn_unit(&game, 0u, 5.0f, 5.0f);
    nearby = spawn_unit(&game, 0u, 5.5f, 5.0f);
    outside = spawn_unit(&game, 0u, 7.0f, 5.0f);
    if (!primary || !nearby || !outside) return;
    projectile = put_projectile(
        &game, primary, 10, PT_DAMAGE_SPLASH);
    projectile->splash_radius = 1.0f;
    pt_combat_update(&game, 0.0);

    PT_CHECK_EQ_INT(primary->integrity, 65);
    PT_CHECK_EQ_INT(nearby->integrity, 65);
    PT_CHECK_EQ_INT(outside->integrity, 75);
}

static void test_suppressor_destroys_fixture_on_time(void)
{
    pt_game game;
    pt_fixture *fixture;
    const pt_pad_def *pad;
    const pt_unit_def *definition;
    pt_unit *suppressor;
    int total_ticks;

    reset_game(&game);
    fixture = &game.fixtures[9];
    pad = pt_pad(10u);
    PT_CHECK(pad != NULL, "pad 10 exists");
    if (!pad) return;
    fixture->kind = 0u;
    fixture->tier = 0u;
    fixture->pad = 10u;
    fixture->present = 1u;
    fixture->integrity = 120;
    fixture->integrity_max = 120;
    fixture->damage_scale = 1.0f;
    fixture->range_scale = 1.0f;
    suppressor = spawn_unit(&game, 5u,
                            (float)pad->x + 0.5f,
                            (float)pad->y + 0.5f);
    definition = pt_unit_def_at(game.campaign, 5u);
    if (!suppressor || !definition) return;
    total_ticks = (int)ceil(
        (double)fixture->integrity /
        ((double)definition->attack_dps * PT_STEP_SECONDS));

    for (int tick = 0; tick < total_ticks - 1; ++tick)
        pt_units_update(&game, PT_STEP_SECONDS);
    PT_CHECK(fixture->present,
             "fixture survives until the final expected damage tick");
    PT_CHECK_EQ_INT(fixture->integrity, 1);

    pt_units_update(&game, PT_STEP_SECONDS);
    PT_CHECK(!fixture->present, "fixture falls at 120/18 seconds");
    PT_CHECK_EQ_INT(fixture->pad, 0);
    PT_CHECK_EQ_INT(game.fixtures_lost, 1);
    PT_CHECK_EQ_INT(suppressor->halted, 0);
}

void pt_test_combat(void)
{
    test_step_1_base_damage_for_tier();
    test_step_2_largest_support_buff();
    test_step_3_vulnerability_hook_is_neutral_in_v1();
    test_step_4_armour_unless_pierce();
    test_step_5_shield_splash_and_overflow();
    test_step_6_floor_at_one();
    test_step_7_death_award_support_tick_and_hook();
    test_threshold_fires_once();
    test_projectile_advance_and_pierce_chain();
    test_projectile_splash_radius();
    test_suppressor_destroys_fixture_on_time();
}
