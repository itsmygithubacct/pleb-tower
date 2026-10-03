/* Pacing is deterministic; visual effects never change game rules. */
#include "pt_test.h"
#include "pleb_tower.h"

#include <math.h>
#include <string.h>

static bool has_effect(const pt_game *game, pt_effect_kind kind)
{
    for (size_t i = 0u; i < PT_MAX_EFFECTS; ++i)
        if (game->effects[i].remaining > 0.0f && game->effects[i].kind == kind)
            return true;
    return false;
}

static void test_fast_forward_matches_fixed_steps(void)
{
    for (uint8_t campaign = 0u; campaign < PT_CAMPAIGN_COUNT; ++campaign) {
        pt_game normal, fast;
        pt_game_init(&normal, campaign, UINT64_C(0xfa57));
        normal.headless = true;
        PT_CHECK(pt_fixture_place(&normal, 4u, 0u), "place speed test tower");
        PT_CHECK(pt_fixture_place(&normal, 5u, 0u), "place second speed test tower");
        pt_game_call_wave_early(&normal);
        pt_game_step(&normal, PT_STEP_SECONDS);
        fast = normal;
        fast.speed = 2u;
        for (int tick = 0; tick < 180; ++tick) {
            pt_game_advance(&fast, PT_STEP_SECONDS);
            pt_game_step(&normal, PT_STEP_SECONDS);
            pt_game_step(&normal, PT_STEP_SECONDS);
        }
        PT_CHECK_EQ_INT(fast.phase, PT_PHASE_WAVE);
        PT_CHECK_EQ_INT(fast.tick, normal.tick);
        fast.speed = 1u;
        PT_CHECK(memcmp(&normal, &fast, sizeof normal) == 0,
            "2x combat has identical movement, shots, effects, economy and RNG");

        fast.speed = 2u;
        pt_units_reset(&fast.units, campaign);
        fast.wave.spawned = fast.wave.total;
        pt_game_advance(&fast, PT_STEP_SECONDS);
        PT_CHECK_EQ_INT(fast.phase, PT_PHASE_BUILD);
        PT_CHECK_EQ_INT(fast.speed, 1);
        PT_CHECK(fast.wave.build_remaining == pt_wave_def_at(campaign, fast.wave.index)->build_seconds,
            "fast-forward stops at the wave boundary without consuming planning time");

        pt_game_set_phase(&fast, PT_PHASE_PAUSE);
        uint64_t tick = fast.tick;
        double elapsed = fast.elapsed;
        fast.speed = 2u;
        pt_game_advance(&fast, PT_STEP_SECONDS);
        PT_CHECK(fast.tick == tick && fast.elapsed == elapsed, "pause wins over fast-forward");
    }
}

static void test_campaign_spawn_stats(void)
{
    for (uint8_t campaign = 0u; campaign < PT_CAMPAIGN_COUNT; ++campaign) {
        pt_game game;
        pt_game_init(&game, campaign, UINT64_C(0x57a75));
        for (uint16_t kind = 0u; kind < pt_campaign(campaign)->unit_count; ++kind) {
            const pt_unit_def *definition = pt_unit_def_at(campaign, kind);
            pt_unit *unit = pt_units_spawn(&game.units, kind, 10.5f, 8.5f);
            PT_CHECK(unit != NULL, "spawn each campaign's roster");
            if (unit == NULL) continue;
            PT_CHECK_EQ_INT(unit->integrity, definition->integrity);
            PT_CHECK_EQ_INT(unit->shield, definition->shield);
        }
        /* Exercise scheduling as well as the pool initializer. */
        pt_units_reset(&game.units, campaign);
        game.wave.index = (uint16_t)(pt_campaign(campaign)->wave_count - 1u);
        game.wave.total = pt_wave_def_at(campaign, game.wave.index)->total_units;
        game.wave.active = true;
        pt_game_set_phase(&game, PT_PHASE_WAVE);
        for (int tick = 0; tick < 3000 && game.wave.spawned < game.wave.total; ++tick)
            pt_units_update(&game, PT_STEP_SECONDS);
        bool found = false;
        for (size_t slot = 0u; slot < PT_MAX_UNITS; ++slot) {
            const pt_unit *unit = &game.units.slots[slot];
            if (!unit->alive || unit->kind != 8u) continue;
            found = true;
            PT_CHECK_EQ_INT(unit->integrity, pt_unit_def_at(campaign, 8u)->integrity);
        }
        PT_CHECK(found, "final wave schedules a boss with the correct campaign stats");
    }
}

static void test_combat_effect_lifecycle(void)
{
    pt_game game;
    pt_game_init(&game, 0u, UINT64_C(0xeffec7));
    game.headless = true;
    pt_unit *enemy = pt_units_spawn(&game.units, 6u, 5.5f, 8.5f);
    PT_CHECK(enemy != NULL, "spawn shielded effect target");
    if (enemy == NULL) return;
    pt_combat_apply_damage(&game, enemy, 1, PT_DAMAGE_PIERCE, 0u);
    PT_CHECK(has_effect(&game, PT_EFFECT_HIT), "successful hits create a brief spark");
    pt_combat_apply_damage(&game, enemy, 100, PT_DAMAGE_PIERCE, 0u);
    PT_CHECK(has_effect(&game, PT_EFFECT_SHIELD), "breaking a shield creates a distinct pulse");
    pt_combat_apply_damage(&game, enemy, 1000, PT_DAMAGE_PIERCE, 0u);
    PT_CHECK(has_effect(&game, PT_EFFECT_DESTROY), "a defeat creates a burst");
    pt_game_set_phase(&game, PT_PHASE_PAUSE);
    pt_effect before[PT_MAX_EFFECTS];
    memcpy(before, game.effects, sizeof before);
    pt_game_advance(&game, 1.0);
    PT_CHECK(memcmp(before, game.effects, sizeof before) == 0,
        "visual effects freeze while paused");
    uint64_t rng = game.rng;
    pt_economy economy = game.economy;
    for (int i = 0; i < PT_MAX_EFFECTS * 3; ++i)
        pt_effect_emit(&game, PT_EFFECT_BLAST, 5.0f, 8.0f, 1.0f);
    PT_CHECK(game.rng == rng && memcmp(&game.economy, &economy, sizeof economy) == 0,
        "overfull visual pool does not change RNG or economy");
    pt_effects_update(&game, 1.0);
    bool active = false;
    for (size_t i = 0u; i < PT_MAX_EFFECTS; ++i)
        active |= game.effects[i].remaining > 0.0f;
    PT_CHECK(!active, "all visual effects expire");

    pt_unit *target = pt_units_spawn(&game.units, 0u, 5.5f, 8.5f);
    if (target == NULL) return;
    pt_projectile *shot = &game.projectiles.slots[0];
    *shot = (pt_projectile){.x = target->x, .y = target->y,
        .target_x = target->x, .target_y = target->y,
        .speed = 12.0f, .damage = 20, .splash_radius = 1.1f,
        .target_serial = target->serial, .damage_type = PT_DAMAGE_SPLASH, .alive = 1u};
    game.projectiles.live = 1u;
    pt_combat_update(&game, PT_STEP_SECONDS);
    PT_CHECK(has_effect(&game, PT_EFFECT_BLAST), "artillery impact creates a blast ring");
    bool radius_matches = false;
    for (size_t i = 0u; i < PT_MAX_EFFECTS; ++i)
        if (game.effects[i].kind == PT_EFFECT_BLAST && game.effects[i].remaining > 0.0f)
            radius_matches = fabsf(game.effects[i].radius - 1.1f) < 0.0001f;
    PT_CHECK(radius_matches, "blast visual uses the actual damage radius");
}

void pt_test_feedback(void)
{
    test_fast_forward_matches_fixed_steps();
    test_campaign_spawn_stats();
    test_combat_effect_lifecycle();
}
