/* Semantic audio over kilix-game-kit. Cue and scene meaning is game-owned;
 * the bank arrives at M6. Audio failure is always a silent, playable
 * fallback — never a reason the game does not start. */
#include "pleb_tower.h"

static bool audio_ready;

bool pt_audio_init(void)
{
    audio_ready = false;   /* M6 wires kilix_game_audio; silence until then. */
    return true;
}

void pt_audio_shutdown(void) { audio_ready = false; }
void pt_audio_cue(uint32_t cue) { (void)cue; }
void pt_audio_scene(uint32_t scene) { (void)scene; }
void pt_audio_update(double dt) { (void)dt; }
