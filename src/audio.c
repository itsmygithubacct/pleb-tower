/* Load the actual authored bank once. Semantic IDs stay game-owned, and the
 * mixer supplies variants, voices, and crossfades. No device is required. */
#include "pleb_tower.h"
#include "kilix_game_audio.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct pt_audio_metadata {
    uint32_t id;
    uint32_t slot;
    kilix_game_audio_bus bus;
    float cooldown;
} pt_audio_metadata;

#include "audio_generated.h"

static kilix_game_audio audio;
static float cooldowns[PCMMIX_BANK_CUES_MAX];
static bool muted;

static bool init_audio(bool offline)
{
    kilix_game_audio_options options;
    pt_settings settings;
    char root[4096];
    char error[256];

    if (!kilix_game_data_root_from_executable(
            "PLEB_TOWER_ASSETS", "assets", "../share/pleb-tower/assets",
            root, sizeof root)) return false;
    kilix_game_audio_options_init(&options);
    options.cue_count = PT_AUDIO_BANK_COUNT;
    options.cues = cue_specs;
    options.cue_spec_count = sizeof cue_specs / sizeof cue_specs[0];
    options.scenes = scene_specs;
    options.scene_count = sizeof scene_specs / sizeof scene_specs[0];
    options.data.environment_variable = "PLEB_TOWER_ASSETS";
    options.data.local_root = root;
    options.mixer.max_voices = 24u;
    options.mixer.offline = offline;
    options.require_mixer = offline;
    if (!kilix_game_audio_init(&audio, &options, error, sizeof error))
        return false;
    memset(cooldowns, 0, sizeof cooldowns);
    pt_settings_defaults(&settings);
    (void)pt_save_load_settings(&settings);
    kilix_game_audio_set_bus(&audio, KILIX_GAME_AUDIO_BUS_MASTER, 0.75f);
    kilix_game_audio_set_bus(&audio, KILIX_GAME_AUDIO_BUS_MUSIC, 0.62f * settings.gain_music);
    kilix_game_audio_set_bus(&audio, KILIX_GAME_AUDIO_BUS_SFX, 0.6f * settings.gain_sfx);
    kilix_game_audio_set_bus(&audio, KILIX_GAME_AUDIO_BUS_UI, settings.gain_ui);
    return true;
}

bool pt_audio_init(void)
{
    if (!muted) (void)init_audio(false);
    return true; /* Unavailable assets or device never prevent playing. */
}

void pt_audio_shutdown(void)
{
    kilix_game_audio_shutdown(&audio);
}

void pt_audio_set_muted(bool value)
{
    muted = value;
    if (!muted && !audio.ready) (void)init_audio(false);
    kilix_game_audio_set_bus(&audio, KILIX_GAME_AUDIO_BUS_MASTER,
                             muted ? 0.0f : 0.75f);
}

bool pt_audio_is_muted(void) { return muted; }

void pt_audio_sync(const pt_game *game)
{
    uint32_t scene = PT_SCENE_TITLE;
    if (!game || !audio.ready) return;
    switch ((pt_phase)game->phase) {
    case PT_PHASE_BUILD: scene = PT_SCENE_BUILD; break;
    case PT_PHASE_WAVE:
        scene = game->wave.index + 1u >= pt_game_campaign(game)->wave_count ?
                PT_SCENE_BOSS : PT_SCENE_WAVE;
        break;
    case PT_PHASE_VICTORY: scene = PT_SCENE_VICTORY; break;
    case PT_PHASE_DEFEAT: scene = PT_SCENE_DEFEAT; break;
    case PT_PHASE_PAUSE:
    case PT_PHASE_HELP:
    case PT_PHASE_INTEL:
        return;
    default: break;
    }
    if (scene != audio.current_scene) pt_audio_scene(scene);
}

void pt_audio_cue(uint32_t cue)
{
    if (!audio.ready) return;
    for (size_t i = 0; i < sizeof cue_metadata / sizeof cue_metadata[0]; ++i) {
        const pt_audio_metadata *meta = &cue_metadata[i];
        if (meta->id != cue) continue;
        if (cooldowns[meta->slot] > 0.0f) return;
        (void)kilix_game_audio_play(&audio, meta->slot, meta->bus, 1.0f, 1.0f);
        cooldowns[meta->slot] = fmaxf(meta->cooldown, 0.06f);
        return;
    }
}

void pt_audio_scene(uint32_t scene)
{
    if (audio.ready) (void)kilix_game_audio_set_scene(&audio, scene);
}

void pt_audio_update(double dt)
{
    if (!audio.ready || !isfinite(dt) || dt <= 0.0 || dt > 1.0) return;
    for (size_t i = 0; i < PCMMIX_BANK_CUES_MAX; ++i)
        cooldowns[i] = fmaxf(0.0f, cooldowns[i] - (float)dt);
    kilix_game_audio_update(&audio, (float)dt);
}

bool pt_audio_render_test(const char *path)
{
    int16_t samples[44100];
    bool audible = false;
    bool result = init_audio(true);
    if (!result) return false;
    result = audio.loaded_cues == PT_AUDIO_BANK_COUNT &&
             audio.loaded_variants == sizeof cue_specs / sizeof cue_specs[0];
    pt_audio_scene(PT_SCENE_BUILD);
    pt_audio_cue(PT_CUE_BUILD_PLACE);
    pcmmix_mix_block(&audio.mixer, samples, 44100u);
    for (size_t i = 0u; i < 44100u; ++i)
        if (samples[i] != 0) audible = true;
    FILE *file = fopen(path, "wb");
    /* A portable PCM WAV: never write host-endian integers or structs. */
    const uint8_t header[44] = {
        'R','I','F','F', 0xac,0x58,0x01,0, 'W','A','V','E',
        'f','m','t',' ', 16,0,0,0, 1,0,1,0, 0x44,0xac,0,0,
        0x88,0x58,0x01,0, 2,0,16,0, 'd','a','t','a', 0x88,0x58,0x01,0
    };
    result = result && audible && file != NULL;
    if (file) {
        if (fwrite(header, 1u, sizeof header, file) != sizeof header) result = false;
        for (size_t i = 0u; i < 44100u; ++i) {
            uint16_t sample = (uint16_t)samples[i];
            uint8_t bytes[2] = {(uint8_t)sample, (uint8_t)(sample >> 8u)};
            if (fwrite(bytes, 1u, 2u, file) != 2u) result = false;
        }
        if (fclose(file) != 0) result = false;
    }
    pt_audio_shutdown();
    if (result) printf("PASS audio-test cues=%u variants=%zu %s\n",
                        PT_AUDIO_BANK_COUNT,
                        sizeof cue_specs / sizeof cue_specs[0], path);
    return result;
}
