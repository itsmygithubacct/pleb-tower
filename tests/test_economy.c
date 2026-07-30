/* Economy formula and repair-price tests. */
#include "pt_test.h"
#include "pleb_tower.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool join_test_path(char *output, size_t output_size,
                           const char *base, const char *suffix)
{
    size_t base_length;
    size_t suffix_length;

    if (output == NULL || base == NULL || suffix == NULL)
        return false;
    base_length = strlen(base);
    suffix_length = strlen(suffix);
    if (base_length >= output_size ||
        suffix_length >= output_size - base_length)
        return false;
    (void)memcpy(output, base, base_length);
    (void)memcpy(output + base_length, suffix, suffix_length + 1u);
    return true;
}

static void test_spend_refuses_when_short(void)
{
    pt_game game;
    pt_economy before;

    pt_game_init(&game, 0u, UINT64_C(0x1234));
    game.economy.currency = 9;
    before = game.economy;
    PT_CHECK(!pt_economy_spend(&game, 10u), "short spend is refused");
    PT_CHECK(memcmp(&game.economy, &before, sizeof before) == 0,
             "refused spend mutates nothing");
}

static void test_income_formulas(void)
{
    PT_CHECK_EQ_INT(pt_economy_wave_stipend(1u), 30);
    PT_CHECK_EQ_INT(pt_economy_wave_stipend(12u), 85);
    PT_CHECK_EQ_INT(pt_economy_early_call_bonus(0.99), 0);
    PT_CHECK_EQ_INT(pt_economy_early_call_bonus(12.99), 12);
    PT_CHECK_EQ_INT(pt_economy_early_call_bonus(40.0), 40);
    PT_CHECK_EQ_INT(pt_economy_early_call_bonus(9999.0), 40);
    PT_CHECK_EQ_INT(pt_economy_early_call_bonus(-2.0), 0);
}

static void test_leak_clamps_at_zero(void)
{
    pt_game game;
    pt_unit unit;

    pt_game_init(&game, 0u, UINT64_C(0x1234));
    (void)memset(&unit, 0, sizeof unit);
    unit.kind = 2u; /* Breacher mass = 3. */
    game.economy.integrity = 2;
    pt_economy_leak(&game, &unit);
    PT_CHECK_EQ_INT(game.economy.integrity, 0);
    pt_economy_leak(&game, &unit);
    PT_CHECK_EQ_INT(game.economy.integrity, 0);
}

static void test_repair_cost_rounds_partial_unit(void)
{
    pt_game game;
    pt_fixture *fixture;

    pt_game_init(&game, 0u, UINT64_C(0x1234));
    PT_CHECK(pt_fixture_place(&game, 10u, 0u), "place repair fixture");
    fixture = pt_fixture_at_pad(&game, 10u);
    PT_CHECK(fixture != NULL, "repair fixture exists");
    if (fixture == NULL) return;

    fixture->integrity = fixture->integrity_max - 4;
    PT_CHECK(pt_fixture_repair(&game, 10u), "repair four integrity");
    PT_CHECK_EQ_INT(fixture->integrity, fixture->integrity_max);
    PT_CHECK_EQ_INT(game.economy.currency, 88);
    PT_CHECK_EQ_INT(fixture->invested, 62);

    fixture->integrity = fixture->integrity_max - 1;
    PT_CHECK(pt_fixture_repair(&game, 10u), "partial repair costs one");
    PT_CHECK_EQ_INT(game.economy.currency, 87);
    PT_CHECK_EQ_INT(fixture->invested, 63);

    fixture->integrity = fixture->integrity_max - 3;
    game.economy.currency = 0;
    {
        pt_fixture before = *fixture;
        pt_economy economy_before = game.economy;
        PT_CHECK(!pt_fixture_repair(&game, 10u),
                 "unaffordable repair is refused");
        PT_CHECK(memcmp(fixture, &before, sizeof before) == 0,
                 "refused repair leaves fixture unchanged");
        PT_CHECK(memcmp(&game.economy, &economy_before,
                        sizeof economy_before) == 0,
                 "refused repair leaves economy unchanged");
    }
}

static void test_records_round_trip_and_corrupt_defaults(void)
{
    char directory_template[] = "/tmp/pleb-tower-records-XXXXXX";
    char app_directory[512];
    char records_path[512];
    char settings_path[512];
    const char *old_data_home = getenv("XDG_DATA_HOME");
    char *old_data_home_copy =
        old_data_home != NULL ? strdup(old_data_home) : NULL;
    char *directory = mkdtemp(directory_template);
    pt_records records;
    pt_records loaded;
    pt_settings settings;
    pt_settings loaded_settings;
    pt_game game;
    FILE *file;

    PT_CHECK(directory != NULL, "create temporary records directory");
    if (directory == NULL) {
        free(old_data_home_copy);
        return;
    }
    PT_CHECK(setenv("XDG_DATA_HOME", directory, 1) == 0,
             "route records to temporary directory");

    (void)memset(&records, 0, sizeof records);
    PT_CHECK(pt_save_load_records(&records),
             "missing records resolve to defaults");
    {
        pt_records defaults;
        (void)memset(&defaults, 0, sizeof defaults);
        PT_CHECK(memcmp(&records, &defaults, sizeof defaults) == 0,
                 "missing records are all safe defaults");
    }

    pt_game_init(&game, 0u, UINT64_C(0x1234));
    game.economy.integrity = 17;
    game.economy.currency = 321;
    game.elapsed = 12.345;
    pt_save_note_run(&records, &game, true);
    PT_CHECK_EQ_INT(records.runs[0], 1);
    PT_CHECK_EQ_INT(records.best_integrity[0], 17);
    PT_CHECK_EQ_INT(records.fastest_clear_ms[0], 12345);
    PT_CHECK_EQ_INT(records.best_unspent[0], 321);
    PT_CHECK(pt_campaign_unlocked(&records, 1u),
             "cleared prerequisite unlocks campaign");
    PT_CHECK(pt_save_store_records(&records), "store records");

    (void)memset(&loaded, 0xa5, sizeof loaded);
    PT_CHECK(pt_save_load_records(&loaded), "load stored records");
    PT_CHECK(memcmp(&loaded, &records, sizeof records) == 0,
             "records round trip exactly");

    PT_CHECK(join_test_path(app_directory, sizeof app_directory,
                            directory, "/pleb-tower"),
             "construct temporary app path");
    PT_CHECK(join_test_path(records_path, sizeof records_path,
                            app_directory, "/records.state"),
             "construct records path");
    PT_CHECK(join_test_path(settings_path, sizeof settings_path,
                            app_directory, "/settings.state"),
             "construct settings path");

    pt_settings_defaults(&settings);
    settings.gain_music = 0.125f;
    settings.gain_ambience = 0.375f;
    settings.gain_sfx = 0.625f;
    settings.gain_ui = 0.875f;
    settings.text_scale = 3u;
    settings.reduced_motion = true;
    settings.zoom_default = true;
    PT_CHECK(pt_save_store_settings(&settings), "store independent settings");
    (void)memset(&loaded_settings, 0, sizeof loaded_settings);
    PT_CHECK(pt_save_load_settings(&loaded_settings), "load settings");
    PT_CHECK(loaded_settings.gain_music == settings.gain_music &&
                 loaded_settings.gain_ambience == settings.gain_ambience &&
                 loaded_settings.gain_sfx == settings.gain_sfx &&
                 loaded_settings.gain_ui == settings.gain_ui &&
                 loaded_settings.text_scale == settings.text_scale &&
                 loaded_settings.reduced_motion ==
                     settings.reduced_motion &&
                 loaded_settings.zoom_default == settings.zoom_default,
             "settings round trip exactly");
    settings.gain_sfx = NAN;
    PT_CHECK(!pt_save_store_settings(&settings),
             "non-finite settings are refused");

    file = fopen(records_path, "wb");
    PT_CHECK(file != NULL, "open records for corruption fixture");
    if (file != NULL) {
        static const unsigned char corrupt[] = {0x13u, 0x37u, 0xffu};
        PT_CHECK(fwrite(corrupt, 1u, sizeof corrupt, file) ==
                     sizeof corrupt,
                 "write corrupt records fixture");
        PT_CHECK(fclose(file) == 0, "close corrupt records fixture");
    }
    (void)memset(&loaded, 0xa5, sizeof loaded);
    PT_CHECK(pt_save_load_records(&loaded),
             "corrupt records still permit startup");
    {
        pt_records defaults;
        (void)memset(&defaults, 0, sizeof defaults);
        PT_CHECK(memcmp(&loaded, &defaults, sizeof defaults) == 0,
                 "corrupt records publish safe defaults");
    }

    file = fopen(settings_path, "wb");
    PT_CHECK(file != NULL, "open settings for corruption fixture");
    if (file != NULL) {
        static const unsigned char corrupt[] = {0xdeu, 0xadu};
        PT_CHECK(fwrite(corrupt, 1u, sizeof corrupt, file) ==
                     sizeof corrupt,
                 "write corrupt settings fixture");
        PT_CHECK(fclose(file) == 0, "close corrupt settings fixture");
    }
    (void)memset(&loaded_settings, 0, sizeof loaded_settings);
    PT_CHECK(pt_save_load_settings(&loaded_settings),
             "corrupt settings still permit startup");
    PT_CHECK(loaded_settings.gain_music == 1.0f &&
                 loaded_settings.gain_ambience == 1.0f &&
                 loaded_settings.gain_sfx == 1.0f &&
                 loaded_settings.gain_ui == 1.0f &&
                 loaded_settings.text_scale == 1u &&
                 !loaded_settings.reduced_motion &&
                 !loaded_settings.zoom_default,
             "corrupt settings publish safe defaults");

    if (old_data_home_copy != NULL) {
        (void)setenv("XDG_DATA_HOME", old_data_home_copy, 1);
    } else {
        (void)unsetenv("XDG_DATA_HOME");
    }
    free(old_data_home_copy);
    (void)unlink(records_path);
    (void)unlink(settings_path);
    (void)rmdir(app_directory);
    (void)rmdir(directory);
}

void pt_test_economy(void)
{
    test_spend_refuses_when_short();
    test_income_formulas();
    test_leak_clamps_at_zero();
    test_repair_cost_rounds_partial_unit();
    test_records_round_trip_and_corrupt_defaults();
}
