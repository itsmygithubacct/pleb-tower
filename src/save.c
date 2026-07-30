/* Durable campaign records through kilix-state.
 *
 * The on-disk payload is explicitly versioned and endian-stable.  Loading is
 * fail-soft: absent, inaccessible, unknown, truncated, or corrupt data always
 * publishes safe defaults so persistence can never prevent play.
 */
#include "pleb_tower.h"

#include "kilix_state.h"
#include "kilix_state_codec.h"

#include <limits.h>
#include <math.h>
#include <string.h>

#define PT_SAVE_APP_ID "pleb-tower"
#define PT_RECORDS_FILENAME "records.state"
#define PT_RECORDS_VERSION UINT32_C(1)
#define PT_RECORDS_MAX_PAYLOAD 256u
#define PT_RECORDS_V1_BYTES \
    (4u + (size_t)PT_CAMPAIGN_COUNT * (2u + 4u + 4u + 4u + 1u))
#define PT_SETTINGS_FILENAME "settings.state"
#define PT_SETTINGS_VERSION UINT32_C(1)
#define PT_SETTINGS_MAX_PAYLOAD 64u
#define PT_SETTINGS_V1_BYTES (4u + 4u * 4u + 1u + 1u + 1u)

typedef struct pt_records_decode_context {
    pt_records records;
} pt_records_decode_context;

typedef struct pt_settings_decode_context {
    pt_settings settings;
} pt_settings_decode_context;

_Static_assert(sizeof(float) == sizeof(uint32_t),
               "settings persistence requires 32-bit float");

static void records_defaults(pt_records *records)
{
    (void)memset(records, 0, sizeof *records);
}

static bool records_store_open(kilixstate_store *store)
{
    kilixstate_options options;

    if (store == NULL) return false;
    (void)memset(store, 0, sizeof *store);
    kilixstate_options_init(&options);
    options.app_id = PT_SAVE_APP_ID;
    options.filename = PT_RECORDS_FILENAME;
    options.max_payload = PT_RECORDS_MAX_PAYLOAD;
    options.format = KILIXSTATE_FORMAT_CRC32;
    return kilixstate_store_init(store, &options) == KILIXSTATE_OK;
}

static bool settings_store_open(kilixstate_store *store)
{
    kilixstate_options options;

    if (store == NULL) return false;
    (void)memset(store, 0, sizeof *store);
    kilixstate_options_init(&options);
    options.app_id = PT_SAVE_APP_ID;
    options.filename = PT_SETTINGS_FILENAME;
    options.max_payload = PT_SETTINGS_MAX_PAYLOAD;
    options.format = KILIXSTATE_FORMAT_CRC32;
    return kilixstate_store_init(store, &options) == KILIXSTATE_OK;
}

void pt_settings_defaults(pt_settings *settings)
{
    if (settings == NULL) return;
    settings->gain_music = 1.0f;
    settings->gain_ambience = 1.0f;
    settings->gain_sfx = 1.0f;
    settings->gain_ui = 1.0f;
    settings->text_scale = 1u;
    settings->reduced_motion = false;
    settings->zoom_default = false;
}

static bool settings_valid(const pt_settings *settings)
{
    return settings != NULL &&
           isfinite(settings->gain_music) &&
           settings->gain_music >= 0.0f &&
           settings->gain_music <= 1.0f &&
           isfinite(settings->gain_ambience) &&
           settings->gain_ambience >= 0.0f &&
           settings->gain_ambience <= 1.0f &&
           isfinite(settings->gain_sfx) &&
           settings->gain_sfx >= 0.0f &&
           settings->gain_sfx <= 1.0f &&
           isfinite(settings->gain_ui) &&
           settings->gain_ui >= 0.0f &&
           settings->gain_ui <= 1.0f &&
           settings->text_scale >= 1u &&
           settings->text_scale <= 3u;
}

static bool write_gain(kilixstate_writer *writer, float gain)
{
    uint32_t bits;

    (void)memcpy(&bits, &gain, sizeof bits);
    return kilixstate_write_u32(writer, bits);
}

static bool read_gain(kilixstate_reader *reader, float *gain)
{
    uint32_t bits;

    if (gain == NULL || !kilixstate_read_u32(reader, &bits))
        return false;
    (void)memcpy(gain, &bits, sizeof bits);
    return true;
}

static bool decode_settings_v1(kilixstate_reader *reader, void *context)
{
    pt_settings_decode_context *decoded = context;
    pt_settings candidate;

    if (reader == NULL || decoded == NULL) return false;
    pt_settings_defaults(&candidate);
    if (!read_gain(reader, &candidate.gain_music) ||
        !read_gain(reader, &candidate.gain_ambience) ||
        !read_gain(reader, &candidate.gain_sfx) ||
        !read_gain(reader, &candidate.gain_ui) ||
        !kilixstate_read_u8(reader, &candidate.text_scale) ||
        !kilixstate_read_bool(reader, &candidate.reduced_motion) ||
        !kilixstate_read_bool(reader, &candidate.zoom_default) ||
        !kilixstate_reader_require_finished(reader) ||
        !settings_valid(&candidate))
        return false;
    decoded->settings = candidate;
    return true;
}

bool pt_save_load_settings(pt_settings *settings)
{
    static const kilixstate_migration migrations[] = {
        {PT_SETTINGS_VERSION, PT_SETTINGS_V1_BYTES, false,
         decode_settings_v1}
    };
    kilixstate_store store;
    kilixstate_result load_result;
    kilixstate_codec_result codec_result;
    pt_settings_decode_context decoded;
    uint8_t payload[PT_SETTINGS_MAX_PAYLOAD];
    size_t payload_size = 0u;

    if (settings == NULL) return false;
    pt_settings_defaults(settings);
    if (!settings_store_open(&store)) return true;
    load_result = kilixstate_load(
        &store, payload, sizeof payload, &payload_size);
    kilixstate_store_close(&store);
    if (load_result != KILIXSTATE_OK) return true;

    pt_settings_defaults(&decoded.settings);
    codec_result = kilixstate_migrate(
        payload, payload_size, migrations,
        sizeof migrations / sizeof migrations[0], &decoded, NULL);
    if (codec_result == KILIXSTATE_CODEC_OK)
        *settings = decoded.settings;
    return true;
}

bool pt_save_store_settings(const pt_settings *settings)
{
    kilixstate_store store;
    kilixstate_writer writer;
    kilixstate_result save_result;
    uint8_t payload[PT_SETTINGS_MAX_PAYLOAD];
    size_t payload_size;

    if (!settings_valid(settings) || !settings_store_open(&store))
        return false;
    kilixstate_writer_init(&writer, payload, sizeof payload);
    if (!kilixstate_write_u32(&writer, PT_SETTINGS_VERSION) ||
        !write_gain(&writer, settings->gain_music) ||
        !write_gain(&writer, settings->gain_ambience) ||
        !write_gain(&writer, settings->gain_sfx) ||
        !write_gain(&writer, settings->gain_ui) ||
        !kilixstate_write_u8(&writer, settings->text_scale) ||
        !kilixstate_write_bool(&writer, settings->reduced_motion) ||
        !kilixstate_write_bool(&writer, settings->zoom_default) ||
        kilixstate_writer_result(&writer) != KILIXSTATE_CODEC_OK) {
        kilixstate_store_close(&store);
        return false;
    }
    payload_size = kilixstate_writer_size(&writer);
    save_result = kilixstate_save(&store, payload, payload_size);
    kilixstate_store_close(&store);
    return save_result == KILIXSTATE_OK;
}

static bool decode_records_v1(kilixstate_reader *reader, void *context)
{
    pt_records_decode_context *decoded = context;
    pt_records candidate;
    size_t campaign;

    if (reader == NULL || decoded == NULL) return false;
    records_defaults(&candidate);
    for (campaign = 0u; campaign < PT_CAMPAIGN_COUNT; ++campaign) {
        bool cleared;

        if (!kilixstate_read_u16(
                reader, &candidate.best_integrity[campaign]) ||
            !kilixstate_read_u32(
                reader, &candidate.fastest_clear_ms[campaign]) ||
            !kilixstate_read_u32(
                reader, &candidate.best_unspent[campaign]) ||
            !kilixstate_read_u32(reader, &candidate.runs[campaign]) ||
            !kilixstate_read_bool(reader, &cleared))
            return false;
        candidate.cleared[campaign] = cleared ? 1u : 0u;
    }
    if (!kilixstate_reader_require_finished(reader)) return false;
    decoded->records = candidate;
    return true;
}

bool pt_save_load_records(pt_records *records)
{
    static const kilixstate_migration migrations[] = {
        {PT_RECORDS_VERSION, PT_RECORDS_V1_BYTES, false,
         decode_records_v1}
    };
    kilixstate_store store;
    kilixstate_result load_result;
    kilixstate_codec_result codec_result;
    pt_records_decode_context decoded;
    uint8_t payload[PT_RECORDS_MAX_PAYLOAD];
    size_t payload_size = 0u;

    if (records == NULL) return false;
    records_defaults(records);
    if (!records_store_open(&store)) return true;

    load_result = kilixstate_load(
        &store, payload, sizeof payload, &payload_size);
    kilixstate_store_close(&store);
    if (load_result != KILIXSTATE_OK) return true;

    records_defaults(&decoded.records);
    codec_result = kilixstate_migrate(
        payload, payload_size, migrations,
        sizeof migrations / sizeof migrations[0], &decoded, NULL);
    if (codec_result == KILIXSTATE_CODEC_OK)
        *records = decoded.records;
    return true;
}

bool pt_save_store_records(const pt_records *records)
{
    kilixstate_store store;
    kilixstate_writer writer;
    kilixstate_result save_result;
    uint8_t payload[PT_RECORDS_MAX_PAYLOAD];
    size_t campaign;
    size_t payload_size;

    if (records == NULL || !records_store_open(&store)) return false;
    kilixstate_writer_init(&writer, payload, sizeof payload);
    if (!kilixstate_write_u32(&writer, PT_RECORDS_VERSION)) {
        kilixstate_store_close(&store);
        return false;
    }
    for (campaign = 0u; campaign < PT_CAMPAIGN_COUNT; ++campaign) {
        if (!kilixstate_write_u16(
                &writer, records->best_integrity[campaign]) ||
            !kilixstate_write_u32(
                &writer, records->fastest_clear_ms[campaign]) ||
            !kilixstate_write_u32(
                &writer, records->best_unspent[campaign]) ||
            !kilixstate_write_u32(&writer, records->runs[campaign]) ||
            !kilixstate_write_bool(
                &writer, records->cleared[campaign] != 0u)) {
            kilixstate_store_close(&store);
            return false;
        }
    }
    if (kilixstate_writer_result(&writer) != KILIXSTATE_CODEC_OK) {
        kilixstate_store_close(&store);
        return false;
    }
    payload_size = kilixstate_writer_size(&writer);
    save_result = kilixstate_save(&store, payload, payload_size);
    kilixstate_store_close(&store);
    return save_result == KILIXSTATE_OK;
}

static uint16_t record_integrity(int32_t integrity)
{
    if (integrity <= 0) return 0u;
    if (integrity >= (int32_t)UINT16_MAX) return UINT16_MAX;
    return (uint16_t)integrity;
}

static uint32_t record_currency(int32_t currency)
{
    return currency > 0 ? (uint32_t)currency : 0u;
}

static uint32_t record_elapsed_ms(double elapsed)
{
    double milliseconds;

    if (!(elapsed > 0.0) || !isfinite(elapsed)) return 0u;
    if (elapsed >= (double)UINT32_MAX / 1000.0) return UINT32_MAX;
    milliseconds = elapsed * 1000.0;
    return (uint32_t)floor(milliseconds + 0.5);
}

void pt_save_note_run(pt_records *records, const pt_game *game, bool cleared)
{
    uint8_t campaign;
    uint16_t integrity;
    uint32_t unspent;
    uint32_t elapsed_ms;

    if (records == NULL || game == NULL ||
        game->campaign >= PT_CAMPAIGN_COUNT)
        return;
    campaign = game->campaign;
    if (records->runs[campaign] != UINT32_MAX)
        ++records->runs[campaign];
    if (!cleared) return;

    integrity = record_integrity(game->economy.integrity);
    unspent = record_currency(game->economy.currency);
    elapsed_ms = record_elapsed_ms(game->elapsed);
    if (integrity > records->best_integrity[campaign])
        records->best_integrity[campaign] = integrity;
    if (unspent > records->best_unspent[campaign])
        records->best_unspent[campaign] = unspent;
    if (elapsed_ms > 0u &&
        (records->fastest_clear_ms[campaign] == 0u ||
         elapsed_ms < records->fastest_clear_ms[campaign]))
        records->fastest_clear_ms[campaign] = elapsed_ms;
    records->cleared[campaign] = 1u;
}

bool pt_campaign_unlocked(const pt_records *records, uint8_t campaign)
{
    const pt_campaign_def *definition;
    int16_t prerequisite;

    if (campaign >= PT_CAMPAIGN_COUNT) return false;
    definition = pt_campaign(campaign);
    prerequisite = definition->unlocked_by;
    if (prerequisite < 0) return true;
    if (records == NULL || prerequisite >= PT_CAMPAIGN_COUNT)
        return false;
    return records->cleared[(uint16_t)prerequisite] != 0u;
}
