/* Content accessors over the generated tables. Bounds-checked, allocation
 * free, and the single place any module reaches into content_generated.h. */
#include "pleb_tower.h"

#include "kilix_game_audio.h"

bool pt_asset_path(const char *relative, char *path, size_t capacity)
{
    char root[4096];
    kilix_game_data_roots roots;

    if (!kilix_game_data_root_from_executable(
            "PLEB_TOWER_ASSETS", "assets", "../share/pleb-tower/assets",
            root, sizeof root))
        return false;
    kilix_game_data_roots_init(&roots);
    roots.environment_variable = "PLEB_TOWER_ASSETS";
    roots.local_root = root;
    return kilix_game_data_resolve(&roots, relative, path, capacity);
}

const pt_map_def *pt_map(uint8_t map)
{
    return &pt_maps[map < PT_MAP_COUNT ? map : 0u];
}

const pt_campaign_def *pt_campaign_on_map(uint8_t map, uint8_t campaign)
{
    return &pt_map(map)->campaigns[campaign < PT_CAMPAIGN_COUNT ? campaign : 0u];
}

const pt_campaign_def *pt_campaign(uint8_t campaign)
{
    return pt_campaign_on_map(0u, campaign);
}

const pt_campaign_def *pt_game_campaign(const pt_game *game)
{
    return game ? pt_campaign_on_map(game->board.map, game->campaign) : NULL;
}

const pt_wave_def *pt_game_wave(const pt_game *game, uint16_t wave)
{
    const pt_campaign_def *def = pt_game_campaign(game);
    return def && wave < def->wave_count ? &def->waves[wave] : NULL;
}

const pt_fixture_def *pt_fixture_def_at(uint8_t campaign, uint16_t kind)
{
    const pt_campaign_def *def = pt_campaign(campaign);
    if (kind >= def->fixture_count) return NULL;
    return &def->fixtures[kind];
}

const pt_unit_def *pt_unit_def_at(uint8_t campaign, uint16_t kind)
{
    const pt_campaign_def *def = pt_campaign(campaign);
    if (kind >= def->unit_count) return NULL;
    return &def->units[kind];
}

const pt_wave_def *pt_wave_def_at(uint8_t campaign, uint16_t wave)
{
    const pt_campaign_def *def = pt_campaign(campaign);
    if (wave >= def->wave_count) return NULL;
    return &def->waves[wave];
}

const pt_pad_def *pt_map_pad(uint8_t map, uint8_t pad_id)
{
    const pt_map_def *def = pt_map(map);
    if (pad_id == 0u || pad_id > def->pad_count) return NULL;
    return &def->pads[pad_id - 1u];
}

const pt_pad_def *pt_game_pad(const pt_game *game, uint8_t pad_id)
{
    return game ? pt_map_pad(game->board.map, pad_id) : NULL;
}

const pt_pad_def *pt_pad(uint8_t pad_id)
{
    return pt_map_pad(0u, pad_id);
}

const char *pt_content_hash_string(void)
{
    return pt_content_sha256;
}
