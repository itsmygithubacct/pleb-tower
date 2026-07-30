/* Game state machine, phase flow, wave scheduling handoff, and the one PRNG.
 *
 * Determinism is a hard requirement: fixed step, no float-dependent ordering,
 * and every random draw comes from pt_rand seeded per run. The balance
 * simulator reproduces this exactly, so nothing here may consult the clock. */
#include "pleb_tower.h"

#include <string.h>

/* splitmix64 — small, fast, and identical across platforms and compilers,
 * which matters more here than statistical perfection. */
uint32_t pt_rand(pt_game *game)
{
    uint64_t z = (game->rng += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return (uint32_t)(z >> 32);
}

double pt_rand_unit(pt_game *game)
{
    return (double)pt_rand(game) / 4294967296.0;
}

void pt_game_set_phase(pt_game *game, pt_phase phase)
{
    if (!game || phase >= PT_PHASE_COUNT) return;
    game->phase = (uint8_t)phase;
}

static void begin_build_phase(pt_game *game)
{
    const pt_wave_def *wave = pt_wave_def_at(game->campaign, game->wave.index);
    game->wave.active = false;
    game->wave.spawned = 0u;
    game->wave.spawn_timer = 0.0;
    game->wave.total = wave ? wave->total_units : 0u;
    game->wave.build_remaining = wave ? (double)wave->build_seconds : 0.0;
    pt_game_set_phase(game, PT_PHASE_BUILD);
}

void pt_game_init(pt_game *game, uint8_t campaign, uint64_t seed)
{
    if (!game) return;
    memset(game, 0, sizeof *game);
    game->campaign = campaign < PT_CAMPAIGN_COUNT ? campaign : 0u;
    game->rng = seed ? seed : 0x504C4542544F5752ull;  /* "PLEBTOWR" */

    pt_board_init(&game->board, game->campaign);
    pt_units_reset(&game->units);
    pt_combat_reset(game);
    pt_fixtures_reset(game);
    pt_economy_reset(game);

    const pt_campaign_def *def = pt_campaign(game->campaign);
    game->cursor.x = (int8_t)def->spawn_x;
    game->cursor.y = (int8_t)def->spawn_y;
    game->wave.index = 0u;
    begin_build_phase(game);
}

void pt_game_call_wave_early(pt_game *game)
{
    if (!game || game->phase != PT_PHASE_BUILD) return;
    if (game->wave.build_remaining > 0.0)
        pt_economy_award(game,
                         pt_economy_early_call_bonus(game->wave.build_remaining));
    game->wave.build_remaining = 0.0;
}

static bool wave_is_finished(const pt_game *game)
{
    return game->wave.spawned >= game->wave.total && game->units.live == 0u;
}

static void advance_wave(pt_game *game)
{
    const pt_campaign_def *def = pt_campaign(game->campaign);
    pt_economy_award(game, pt_economy_wave_stipend(
                         (uint16_t)(game->wave.index + 1u)));
    if (game->wave.index + 1u >= def->wave_count) {
        pt_game_set_phase(game, PT_PHASE_VICTORY);
        return;
    }
    ++game->wave.index;
    begin_build_phase(game);
}

void pt_game_step(pt_game *game, double dt)
{
    if (!game) return;
    if (game->phase != PT_PHASE_BUILD && game->phase != PT_PHASE_WAVE) return;

    ++game->tick;
    game->elapsed += dt;

    if (game->phase == PT_PHASE_BUILD) {
        game->wave.build_remaining -= dt;
        if (game->wave.build_remaining <= 0.0) {
            game->wave.build_remaining = 0.0;
            game->wave.active = true;
            pt_game_set_phase(game, PT_PHASE_WAVE);
        }
        /* Fixtures still tick during the build phase so support repair can
         * bring a damaged board back before the next wave lands. */
        pt_fixtures_update(game, dt);
        return;
    }

    pt_units_update(game, dt);
    pt_gather_update(game, dt);
    pt_fixtures_update(game, dt);
    pt_combat_update(game, dt);

    if (game->economy.integrity <= 0) {
        game->economy.integrity = 0;
        pt_game_set_phase(game, PT_PHASE_DEFEAT);
        return;
    }
    if (wave_is_finished(game)) advance_wave(game);
}
