#include "pt_test.h"
#include "pleb_tower.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define PT_SIM_CAPTURE_CAPACITY 32768u
#define PT_SIM_TEST_WAVE_TICK_LIMIT (UINT64_C(900) * PT_TICK_HZ)

typedef struct pt_sim_order_entry pt_sim_order_entry;
int pt_simulate_run(uint8_t campaign, const pt_sim_order_entry *order,
                    size_t order_count, bool verbose);
void pt_test_simulate(void);

typedef struct pt_sim_test_order {
    uint16_t wave;
    uint8_t  pad;
    uint16_t kind;
    uint8_t  tier;
} pt_sim_test_order;

static bool capture_simulation(
    uint8_t campaign,
    const pt_sim_test_order *order,
    size_t order_count,
    bool verbose,
    char output[PT_SIM_CAPTURE_CAPACITY],
    size_t *output_size,
    int *result)
{
    FILE *capture;
    int saved_stdout;
    bool ok = false;
    size_t size;

    if (fflush(stdout) != 0) return false;
    capture = tmpfile();
    if (capture == NULL) return false;
    saved_stdout = dup(STDOUT_FILENO);
    if (saved_stdout < 0) {
        (void)fclose(capture);
        return false;
    }
    if (dup2(fileno(capture), STDOUT_FILENO) < 0) goto cleanup;

    *result = pt_simulate_run(
        campaign, (const pt_sim_order_entry *)(const void *)order,
        order_count, verbose);
    if (fflush(stdout) != 0) goto restore;
    if (fseek(capture, 0L, SEEK_SET) != 0) goto restore;
    size = fread(output, 1u, PT_SIM_CAPTURE_CAPACITY - 1u, capture);
    if (ferror(capture)) goto restore;
    output[size] = '\0';
    *output_size = size;
    ok = true;

restore:
    if (dup2(saved_stdout, STDOUT_FILENO) < 0) ok = false;
cleanup:
    (void)close(saved_stdout);
    (void)fclose(capture);
    return ok;
}

static uint32_t wave_mass(uint8_t campaign, uint16_t wave_index)
{
    const pt_wave_def *wave = pt_wave_def_at(campaign, wave_index);
    uint32_t mass = 0u;

    if (wave == NULL) return 0u;
    for (uint16_t group = 0u; group < wave->group_count; ++group) {
        const pt_unit_def *unit =
            pt_unit_def_at(campaign, wave->groups[group].type);

        if (unit != NULL)
            mass += (uint32_t)wave->groups[group].count * unit->mass;
    }
    return mass;
}

static void test_empty_order_loses_on_mass_wave(void)
{
    char output[PT_SIM_CAPTURE_CAPACITY];
    char expected[64];
    size_t output_size = 0u;
    uint32_t cumulative_mass = 0u;
    const pt_campaign_def *campaign = pt_campaign(0u);
    uint16_t expected_wave = 0u;
    int result = -1;
    bool captured;

    for (uint16_t wave = 0u; wave < campaign->wave_count; ++wave) {
        cumulative_mass += wave_mass(0u, wave);
        if (cumulative_mass >= (uint32_t)campaign->starting_integrity) {
            expected_wave = (uint16_t)(wave + 1u);
            break;
        }
    }
    PT_CHECK_EQ_INT(expected_wave, 2);

    captured = capture_simulation(
        0u, NULL, 0u, true, output, &output_size, &result);
    PT_CHECK(captured, "capture empty-order simulation");
    if (!captured) return;
    PT_CHECK(output_size > 0u, "empty-order trace is non-empty");
    PT_CHECK_EQ_INT(result, 1);
    (void)snprintf(expected, sizeof expected,
                   "campaign=holdout status=LOST waves=%u",
                   (unsigned)expected_wave);
    PT_CHECK(strstr(output, expected) != NULL,
             "empty order loses on mass-predicted wave: %s", output);
    PT_CHECK(strstr(output, "leak_count=20") != NULL,
             "empty order leaks exactly the integrity budget");
    PT_CHECK(strstr(output, "integrity=0/20") != NULL,
             "empty order exhausts integrity");
}

static void test_trace_is_byte_deterministic(void)
{
    static const pt_sim_test_order order[] = {
        { 1u, 4u, 0u, 0u },
        { 1u, 5u, 0u, 0u },
        { 2u, 6u, 0u, 0u },
        { 2u, 3u, 0u, 0u },
        { 3u, 4u, 0u, 1u },
        { 3u, 5u, 0u, 1u },
    };
    char first[PT_SIM_CAPTURE_CAPACITY];
    char second[PT_SIM_CAPTURE_CAPACITY];
    size_t first_size = 0u;
    size_t second_size = 0u;
    int first_result = -1;
    int second_result = -1;
    bool first_captured;
    bool second_captured;

    first_captured = capture_simulation(
        0u, order, sizeof order / sizeof order[0], true,
        first, &first_size, &first_result);
    second_captured = capture_simulation(
        0u, order, sizeof order / sizeof order[0], true,
        second, &second_size, &second_result);
    PT_CHECK(first_captured && second_captured,
             "capture both deterministic traces");
    if (!first_captured || !second_captured) return;
    PT_CHECK_EQ_INT(first_result, second_result);
    PT_CHECK_EQ_INT(first_size, second_size);
    if (first_size == second_size)
        PT_CHECK(memcmp(first, second, first_size) == 0,
                 "same fixed seed/order produces byte-identical output");
}

static void test_campaign_pool_does_not_overflow(uint8_t campaign_index)
{
    pt_game game;
    const pt_campaign_def *campaign = pt_campaign(campaign_index);
    uint64_t ticks = 0u;
    uint64_t tick_limit =
        (uint64_t)campaign->wave_count * PT_SIM_TEST_WAVE_TICK_LIMIT;

    pt_game_init(&game, campaign_index, UINT64_C(0x53494d504f4f4c));
    game.headless = true;
    game.economy.integrity = INT_MAX;
    game.economy.integrity_max = INT_MAX;

    while ((game.phase == PT_PHASE_BUILD || game.phase == PT_PHASE_WAVE) &&
           ticks < tick_limit) {
        if (game.phase == PT_PHASE_BUILD) game.wave.build_remaining = 0.0;
        pt_game_step(&game, PT_STEP_SECONDS);
        ++ticks;
    }

    PT_CHECK_EQ_INT(game.phase, PT_PHASE_VICTORY);
    PT_CHECK_EQ_INT(game.wave.index, campaign->wave_count - 1u);
    PT_CHECK_EQ_INT(game.units.overflow, 0);
    PT_CHECK(ticks < tick_limit, "%s completes before the safety cap",
             campaign->id);
}

void pt_test_simulate(void)
{
    test_empty_order_loses_on_mass_wave();
    test_trace_is_byte_deterministic();
    test_campaign_pool_does_not_overflow(0u);
    test_campaign_pool_does_not_overflow(1u);
}
