/* Content accessors over the generated tables. Bounds-checked, allocation
 * free, and the single place any module reaches into content_generated.h. */
#include "pleb_tower.h"

const pt_campaign_def *pt_campaign(uint8_t campaign)
{
    if (campaign >= PT_CAMPAIGN_COUNT) return &pt_campaigns[0];
    return &pt_campaigns[campaign];
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

const pt_pad_def *pt_pad(uint8_t pad_id)
{
    if (pad_id == 0u || pad_id > PT_PAD_COUNT) return NULL;
    return &pt_pads[pad_id - 1u];
}

const char *pt_content_hash_string(void)
{
    return pt_content_sha256;
}
