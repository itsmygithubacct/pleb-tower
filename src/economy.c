/* Currency and integrity accounting.
 *
 * Every purchase passes through pt_economy_spend so failed purchases are
 * atomic and lifetime spending remains reproducible for the score screen and
 * balance simulator.
 */
#include "pleb_tower.h"

#include <limits.h>

static uint32_t add_u32_saturating(uint32_t first, uint32_t second)
{
    if (second > UINT32_MAX - first) return UINT32_MAX;
    return first + second;
}

void pt_economy_reset(pt_game *game)
{
    const pt_campaign_def *campaign;

    if (game == NULL) return;
    campaign = pt_game_campaign(game);
    game->economy.integrity = campaign->starting_integrity;
    game->economy.integrity_max = campaign->starting_integrity;
    game->economy.currency = campaign->starting_currency;
    game->economy.earned_total = 0u;
    game->economy.spent_total = 0u;
}

bool pt_economy_spend(pt_game *game, uint32_t amount)
{
    if (game == NULL || game->economy.currency < 0 ||
        amount > (uint32_t)game->economy.currency)
        return false;

    game->economy.currency -= (int32_t)amount;
    game->economy.spent_total =
        add_u32_saturating(game->economy.spent_total, amount);
    return true;
}

void pt_economy_award(pt_game *game, uint32_t amount)
{
    int64_t currency;

    if (game == NULL) return;
    currency = (int64_t)game->economy.currency + (int64_t)amount;
    if (currency > INT32_MAX) currency = INT32_MAX;
    if (currency < 0) currency = 0;
    game->economy.currency = (int32_t)currency;
    game->economy.earned_total =
        add_u32_saturating(game->economy.earned_total, amount);
}

void pt_economy_leak(pt_game *game, const pt_unit *unit)
{
    const pt_unit_def *definition;
    int32_t mass;

    if (game == NULL || unit == NULL) return;
    definition = pt_unit_def_at(game->campaign, unit->kind);
    if (definition == NULL) return;

    mass = (int32_t)definition->mass;
    if (!game->headless) pt_audio_cue(PT_CUE_STATE_INTEGRITY_LOST);
    if (game->economy.integrity <= mass)
        game->economy.integrity = 0;
    else
        game->economy.integrity -= mass;
}

uint32_t pt_economy_wave_stipend(uint16_t wave_index)
{
    return PT_STIPEND_BASE +
           PT_STIPEND_PER_WAVE * (uint32_t)wave_index;
}

uint32_t pt_economy_early_call_bonus(double seconds_remaining)
{
    uint32_t whole_seconds;
    uint32_t seconds_to_cap;
    uint64_t bonus;

    if (!(seconds_remaining > 0.0) || PT_EARLY_CALL_PER_SECOND == 0u)
        return 0u;

    seconds_to_cap =
        (PT_EARLY_CALL_CAP + PT_EARLY_CALL_PER_SECOND - 1u) /
        PT_EARLY_CALL_PER_SECOND;
    if (seconds_remaining >= (double)seconds_to_cap)
        return PT_EARLY_CALL_CAP;

    whole_seconds = (uint32_t)seconds_remaining;
    bonus = (uint64_t)whole_seconds * PT_EARLY_CALL_PER_SECOND;
    return bonus > PT_EARLY_CALL_CAP
               ? PT_EARLY_CALL_CAP
               : (uint32_t)bonus;
}
