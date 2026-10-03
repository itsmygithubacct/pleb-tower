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
    if (game->phase != (uint8_t)phase && !game->headless) {
        if (phase == PT_PHASE_WAVE) pt_audio_cue(PT_CUE_STATE_WAVE_INCOMING);
        if (phase == PT_PHASE_VICTORY) pt_audio_cue(PT_CUE_STATE_VICTORY);
        if (phase == PT_PHASE_DEFEAT) pt_audio_cue(PT_CUE_STATE_DEFEAT);
        if (phase == PT_PHASE_PAUSE) pt_audio_cue(PT_CUE_STATE_PAUSE);
    }
    game->phase = (uint8_t)phase;
    if (phase == PT_PHASE_BUILD || phase == PT_PHASE_TITLE ||
        phase == PT_PHASE_VICTORY || phase == PT_PHASE_DEFEAT)
        game->speed = 1u;
}

static void begin_build_phase(pt_game *game)
{
    const pt_wave_def *wave = pt_game_wave(game, game->wave.index);
    game->wave.active = false;
    game->wave.spawned = 0u;
    game->wave.spawn_timer = 0.0;
    game->wave.total = wave ? wave->total_units : 0u;
    game->wave.build_remaining = wave ? (double)wave->build_seconds : 0.0;
    game->wave.awaiting_call = game->wave.index == 0u;
    pt_game_set_phase(game, PT_PHASE_BUILD);
}

void pt_game_init(pt_game *game, uint8_t campaign, uint64_t seed)
{
    pt_game_init_map(game, 0u, campaign, seed);
}

void pt_game_init_map(pt_game *game, uint8_t map, uint8_t campaign, uint64_t seed)
{
    if (!game) return;
    memset(game, 0, sizeof *game);
    game->campaign = campaign < PT_CAMPAIGN_COUNT ? campaign : 0u;
    game->rng = seed ? seed : 0x504C4542544F5752ull;  /* "PLEBTOWR" */

    pt_board_init_map(&game->board, map, game->campaign);
    pt_units_reset(&game->units, game->campaign);
    pt_combat_reset(game);
    pt_fixtures_reset(game);
    pt_economy_reset(game);

    const pt_campaign_def *def = pt_game_campaign(game);
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
    game->wave.awaiting_call = false;
}

static bool wave_is_finished(const pt_game *game)
{
    return game->wave.spawned >= game->wave.total && game->units.live == 0u;
}

static void advance_wave(pt_game *game)
{
    const pt_campaign_def *def = pt_game_campaign(game);
    if (!game->headless) pt_audio_cue(PT_CUE_STATE_WAVE_CLEARED);
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
    pt_effects_update(game, dt);

    if (game->phase == PT_PHASE_BUILD) {
        if (game->wave.awaiting_call && game->wave.build_remaining > 0.0 &&
            !game->headless) {
            pt_fixtures_update(game, dt);
            return;
        }
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
    pt_fixtures_update(game, dt);
    pt_combat_update(game, dt);

    if (game->economy.integrity <= 0) {
        game->economy.integrity = 0;
        pt_game_set_phase(game, PT_PHASE_DEFEAT);
        return;
    }
    if (wave_is_finished(game)) advance_wave(game);
}

void pt_game_advance(pt_game *game, double dt)
{
    if (game == NULL) return;
    unsigned int steps = game->phase == PT_PHASE_WAVE && game->speed == 2u ? 2u : 1u;
    for (unsigned int step = 0u; step < steps; ++step) {
        pt_game_step(game, dt);
        /* A finished wave always gives the player a full build interval. */
        if (game->phase != PT_PHASE_WAVE) break;
    }
}
