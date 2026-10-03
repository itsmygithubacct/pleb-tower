/* Player-facing descriptions use the same tier data as combat and upgrades. */
#include "pleb_tower.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static void value(char out[24], double current, double next, bool compare)
{
    if (compare) (void)snprintf(out, 24, "%.3g>%.3g", current, next);
    else (void)snprintf(out, 24, "%.3g", current);
}

bool pt_fixture_describe(const pt_game *game, const pt_fixture *fixture,
                         bool upgrade,
                         char lines[PT_DESCRIPTION_LINES][PT_DESCRIPTION_CAPACITY])
{
    if (lines == NULL) return false;
    memset(lines, 0, PT_DESCRIPTION_LINES * PT_DESCRIPTION_CAPACITY);
    if (game == NULL || fixture == NULL || fixture->tier >= PT_MAX_TIER) return false;
    const pt_fixture_def *definition = pt_fixture_def_at(game->campaign, fixture->kind);
    if (definition == NULL || definition->role >= PT_ROLE_COUNT) return false;
    bool compare = upgrade && fixture->tier + 1u < PT_MAX_TIER;
    uint8_t next_tier = (uint8_t)(fixture->tier + (compare ? 1u : 0u));
    const pt_tier_def *now = &definition->tiers[fixture->tier];
    const pt_tier_def *next = &definition->tiers[next_tier];
    char range[24], hp[24], damage[24], rate[24], special[24], bonus[24], repair[24];
    float multiplier = fixture->present ? fixture->damage_scale : 1.0f;
    value(range, pt_fixture_range(game, fixture, fixture->tier),
          pt_fixture_range(game, fixture, next_tier), compare);
    value(hp, now->integrity, next->integrity, compare);
    value(damage, floorf((float)now->damage * multiplier),
          floorf((float)next->damage * multiplier), compare);
    value(rate, now->rate, next->rate, compare);
    (void)snprintf(lines[2], PT_DESCRIPTION_CAPACITY, "Integrity %s | %s", hp,
        compare ? "Upgrade fully repairs" : "Range is measured in cells");
    (void)snprintf(lines[3], PT_DESCRIPTION_CAPACITY, "%s",
        fixture->tier + 1u == PT_MAX_TIER ? "Maximum tier reached." :
        "Upgrades increase reach and tower durability.");

    switch ((pt_fixture_role)definition->role) {
    case PT_ROLE_RAPID:
    case PT_ROLE_ARTILLERY:
    case PT_ROLE_ANTIAIR:
        (void)snprintf(lines[0], PT_DESCRIPTION_CAPACITY, "%s",
            definition->role == PT_ROLE_RAPID ? "Rapid armor-piercing fire against ground enemies." :
            definition->role == PT_ROLE_ARTILLERY ? "Explosive ground attacks punish tightly packed groups." :
            "Long-range anti-air fire. Cannot shoot ground enemies.");
        (void)snprintf(lines[1], PT_DESCRIPTION_CAPACITY,
            "Damage %s | Shots/s %s | Range %s", damage, rate, range);
        if (definition->role == PT_ROLE_RAPID) {
            if ((now->targets & PT_TARGETS_AIR) != 0u)
                (void)snprintf(lines[0], PT_DESCRIPTION_CAPACITY,
                    "Rapid armor-piercing fire against ground and air.");
            (void)snprintf(lines[3], PT_DESCRIPTION_CAPACITY, "%s",
                (now->targets & PT_TARGETS_AIR) != 0u ? "Hits drones and ignores armor. Maximum tier." :
                (compare && (next->targets & PT_TARGETS_AIR) != 0u) ?
                "NEW: shoots air enemies, including drones." :
                "Ground only. Tier 3 unlocks air targeting.");
        } else if (definition->role == PT_ROLE_ARTILLERY) {
            value(special, now->splash_radius, next->splash_radius, compare);
            (void)snprintf(lines[2], PT_DESCRIPTION_CAPACITY,
                "Blast radius %s | Integrity %s", special, hp);
            (void)snprintf(lines[3], PT_DESCRIPTION_CAPACITY, "%s",
                compare ? "Wider blasts hit more enemies. Upgrade fully repairs." :
                "Targets ground. Shields resist blast damage.");
        }
        break;
    case PT_ROLE_CONTROL:
    case PT_ROLE_DISABLE:
        (void)snprintf(lines[0], PT_DESCRIPTION_CAPACITY, "%s",
            definition->role == PT_ROLE_CONTROL ?
            "Holds one ground enemy. Hardened enemies are immune." :
            "Stuns one ground or air enemy. Hardened immune.");
        value(special,
            definition->role == PT_ROLE_CONTROL ? now->hold_seconds : now->stun_seconds,
            definition->role == PT_ROLE_CONTROL ? next->hold_seconds : next->stun_seconds, compare);
        (void)snprintf(lines[1], PT_DESCRIPTION_CAPACITY,
            "%s %ss | Uses/s %s | Range %s",
            definition->role == PT_ROLE_CONTROL ? "Hold" : "Stun", special, rate, range);
        (void)snprintf(lines[3], PT_DESCRIPTION_CAPACITY, "%s",
            definition->role == PT_ROLE_CONTROL ?
            "Stops movement, not attacks. Deals no damage." :
            "Stops movement AND attacks. Deals no damage.");
        break;
    case PT_ROLE_REROUTE:
        (void)snprintf(lines[0], PT_DESCRIPTION_CAPACITY,
            "Shares one full stop across nearby ground enemies.");
        if (compare)
            (void)snprintf(lines[1], PT_DESCRIPTION_CAPACITY,
                "Radius %s | Integrity %s | Full repair", range, hp);
        else
            (void)snprintf(lines[1], PT_DESCRIPTION_CAPACITY,
                "1 stops; 2 move at 50%%; 3 at 67%%. Radius %s", range);
        (void)snprintf(lines[2], PT_DESCRIPTION_CAPACITY,
            "Air and hardened enemies are immune. No damage.");
        (void)snprintf(lines[3], PT_DESCRIPTION_CAPACITY, "%s",
            compare ? "More reach; same stop budget. Overlaps do not stack." :
            "Overlaps use the strongest slow. No backward pulling.");
        break;
    case PT_ROLE_VISION:
        (void)snprintf(lines[0], PT_DESCRIPTION_CAPACITY,
            "Blinds attackers, reducing their range and damage.");
        value(special, now->enemy_range_penalty * 100.0f,
            next->enemy_range_penalty * 100.0f, compare);
        value(bonus, now->enemy_accuracy_penalty * 100.0f,
            next->enemy_accuracy_penalty * 100.0f, compare);
        (void)snprintf(lines[1], PT_DESCRIPTION_CAPACITY,
            "Enemy range -%s%% | Enemy damage -%s%%", special, bonus);
        (void)snprintf(lines[2], PT_DESCRIPTION_CAPACITY,
            "Radius %s | Integrity %s", range, hp);
        (void)snprintf(lines[3], PT_DESCRIPTION_CAPACITY, "%s",
            compare ? "Upgrade fully repairs. Overlapping effects do not stack." :
            "Protects towers from shooters. Does not deal damage.");
        break;
    case PT_ROLE_SUPPORT:
        (void)snprintf(lines[0], PT_DESCRIPTION_CAPACITY,
            "Boosts and repairs nearby towers. Bonuses don't stack.");
        value(special, now->damage_buff * 100.0f, next->damage_buff * 100.0f, compare);
        value(bonus, now->range_buff * 100.0f, next->range_buff * 100.0f, compare);
        value(repair, now->repair_per_second, next->repair_per_second, compare);
        value(damage, now->currency_tick, next->currency_tick, compare);
        (void)snprintf(lines[1], PT_DESCRIPTION_CAPACITY,
            "Damage +%s%% | Range +%s%% | Repair %s HP/s", special, bonus, repair);
        (void)snprintf(lines[2], PT_DESCRIPTION_CAPACITY,
            "Radius %s | Kill bonus +%s | HP %s", range, damage, hp);
        (void)snprintf(lines[3], PT_DESCRIPTION_CAPACITY, "%s",
            compare ? "Boosted kills pay a bonus. Upgrade fully repairs." :
            "Bonus paid on boosted tower kills; no passive income.");
        break;
    case PT_ROLE_COUNT:
        return false;
    }
    return true;
}

static const char *role_name(uint8_t campaign, pt_fixture_role role)
{
    const pt_campaign_def *definition = pt_campaign(campaign);
    if (definition == NULL) return "?";
    for (uint16_t i = 0u; i < definition->fixture_count; ++i) {
        const pt_fixture_def *fixture = pt_fixture_def_at(campaign, i);
        if (fixture != NULL && fixture->role == role) return fixture->name;
    }
    return "?";
}

bool pt_unit_describe(uint8_t campaign, uint16_t kind,
                      char lines[PT_INTEL_LINES][PT_INTEL_LINE_CAPACITY])
{
    if (lines == NULL) return false;
    memset(lines, 0, PT_INTEL_LINES * PT_INTEL_LINE_CAPACITY);
    const pt_unit_def *unit = pt_unit_def_at(campaign, kind);
    if (unit == NULL) return false;
    (void)snprintf(lines[0], PT_INTEL_LINE_CAPACITY, "Health %d   Shield %d   Armor %d",
        unit->integrity, unit->shield, unit->armor);
    (void)snprintf(lines[1], PT_INTEL_LINE_CAPACITY,
        "Speed %.2g cells/s   Bounty %u   Leak cost %u",
        (double)unit->speed, (unsigned int)unit->currency, (unsigned int)unit->mass);
    (void)snprintf(lines[2], PT_INTEL_LINE_CAPACITY, "%s", unit->air ?
        "Flies directly to the hub, ignoring the road." :
        "Follows the road toward your hub.");
    (void)snprintf(lines[3], PT_INTEL_LINE_CAPACITY, "%s",
        unit->armor > 0 ? "Armor reduces normal hits; piercing bypasses it." :
        "No armor. Rapid fire is effective against it.");
    (void)snprintf(lines[4], PT_INTEL_LINE_CAPACITY, "Recommended: %s.",
        role_name(campaign, unit->air ? PT_ROLE_ANTIAIR : PT_ROLE_RAPID));
    (void)snprintf(lines[5], PT_INTEL_LINE_CAPACITY, "%s",
        unit->hardened ? "Use damage towers; control cannot stop this enemy." :
        "Hold or stun it to give damage towers more time.");
    (void)snprintf(lines[6], PT_INTEL_LINE_CAPACITY, "%s", unit->hardened ?
        "Immune to hold, stun and decoy slowdown." : unit->air ?
        "Ground-only weapons and decoys cannot stop it." :
        "Vulnerable to entangle, stun and decoy slowdown.");

    if (unit->air) {
        (void)snprintf(lines[3], PT_INTEL_LINE_CAPACITY, "Requires a weapon with air targeting.");
        (void)snprintf(lines[5], PT_INTEL_LINE_CAPACITY,
            "Tier 3 %s can also hit air.", role_name(campaign, PT_ROLE_RAPID));
    } else if (unit->on_threshold.active) {
        const pt_unit_def *child = pt_unit_def_at(campaign, unit->on_threshold.spawn);
        (void)snprintf(lines[2], PT_INTEL_LINE_CAPACITY,
            "Boss: damages nearby towers as it passes.");
        (void)snprintf(lines[3], PT_INTEL_LINE_CAPACITY,
            "At %.0f%% health: releases %u x %s.", (double)unit->on_threshold.value * 100.0,
            (unsigned int)unit->on_threshold.count, child != NULL ? child->name : "allies");
        (void)snprintf(lines[5], PT_INTEL_LINE_CAPACITY, "Protect your towers with %s.",
            role_name(campaign, PT_ROLE_VISION));
    } else if (unit->attack_kind == PT_ATTACK_FIXTURE) {
        (void)snprintf(lines[2], PT_INTEL_LINE_CAPACITY, "Stops on the road to fire at your towers.");
        (void)snprintf(lines[3], PT_INTEL_LINE_CAPACITY, "Attack: %.2g damage/s within %.2g cells.",
            (double)unit->attack_dps, (double)unit->attack_range);
        (void)snprintf(lines[5], PT_INTEL_LINE_CAPACITY, "Reduce its attacks with %s.",
            role_name(campaign, PT_ROLE_VISION));
    } else if (unit->aura_per_second > 0.0f) {
        (void)snprintf(lines[2], PT_INTEL_LINE_CAPACITY, "Repairs allies: %.2g health/s within %.2g cells.",
            (double)unit->aura_per_second, (double)unit->aura_radius);
        (void)snprintf(lines[3], PT_INTEL_LINE_CAPACITY, "Shields regenerate after %.2gs without damage.",
            (double)unit->shield_regen_delay);
        (void)snprintf(lines[5], PT_INTEL_LINE_CAPACITY, "Focus your fire so repairs cannot undo chip damage.");
    } else if (unit->on_death.active) {
        const pt_unit_def *child = pt_unit_def_at(campaign, unit->on_death.spawn);
        (void)snprintf(lines[2], PT_INTEL_LINE_CAPACITY, "On defeat: splits into %u x %s.",
            (unsigned int)unit->on_death.count, child != NULL ? child->name : "enemies");
        (void)snprintf(lines[3], PT_INTEL_LINE_CAPACITY, "Leave enough road coverage to catch the children.");
        (void)snprintf(lines[5], PT_INTEL_LINE_CAPACITY, "Use %s against the group.",
            role_name(campaign, PT_ROLE_ARTILLERY));
    } else if (unit->emit.active) {
        const pt_unit_def *child = pt_unit_def_at(campaign, unit->emit.spawn);
        (void)snprintf(lines[2], PT_INTEL_LINE_CAPACITY, "Releases %u x %s every %.2gs while advancing.",
            (unsigned int)unit->emit.count, child != NULL ? child->name : "allies", (double)unit->emit.value);
        (void)snprintf(lines[3], PT_INTEL_LINE_CAPACITY, "Reinforcement limit: %u. Destroy it early.",
            (unsigned int)unit->emit.max_total);
        (void)snprintf(lines[5], PT_INTEL_LINE_CAPACITY, "Use %s to clear its escorts.",
            role_name(campaign, PT_ROLE_ARTILLERY));
    }
    return true;
}
