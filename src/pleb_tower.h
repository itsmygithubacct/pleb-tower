/* Pleb Tower — shared game types.
 *
 * This header is the interface contract for every module. Design reference:
 * ~/research/gpu_terminal/games/pleb-tower/design/GAME_DESIGN.md
 *
 * Rules that hold everywhere in this tree:
 *   - Zero allocation after load in the update and render paths.
 *   - Simulation is deterministic: fixed step, no float-dependent ordering,
 *     one game-owned PRNG seeded per run.
 *   - Libraries own mechanism; this game owns rules.
 */
#ifndef PLEB_TOWER_H
#define PLEB_TOWER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "content_generated.h"

/* Forward declaration: every subsystem takes the game by pointer, and the
 * full definition arrives further down this header. */
typedef struct pt_game pt_game;

/* ------------------------------------------------------------- geometry -- */

#define PT_LOGICAL_WIDTH   480
#define PT_LOGICAL_HEIGHT  270
#define PT_HUD_HEIGHT      30
#define PT_PLAYFIELD_HEIGHT (PT_LOGICAL_HEIGHT - PT_HUD_HEIGHT)
#define PT_CELL_PIXELS     16
#define PT_CELL_COUNT      (PT_COLUMNS * PT_ROWS)

#define PT_TICK_HZ         60
#define PT_STEP_SECONDS    (1.0 / (double)PT_TICK_HZ)

/* Fixed pools. Overflow is counted and asserted zero by the test suite. */
#define PT_MAX_UNITS       192
#define PT_MAX_PROJECTILES 96
#define PT_MAX_EFFECTS     64
#define PT_MAX_FIXTURES    PT_PAD_COUNT

#define PT_DISTANCE_UNREACHABLE UINT16_MAX

/* ---------------------------------------------------------------- board -- */

typedef struct pt_board {
    uint16_t distance[PT_CELL_COUNT];  /* BFS steps to goal */
    int8_t   flow_x[PT_CELL_COUNT];    /* negative gradient, -1/0/+1 */
    int8_t   flow_y[PT_CELL_COUNT];
    uint8_t  pad_index[PT_CELL_COUNT]; /* 0 = not a pad, else 1-based pad id */
    uint8_t  lane[PT_CELL_COUNT];      /* 1 = walkable lane cell */
    uint16_t bias[PT_CELL_COUNT];      /* decoy pull cost bias */
    uint16_t goal_cell;
    uint16_t spawn_cell;
    uint32_t rebuild_count;
} pt_board;

static inline uint16_t pt_cell_index(int x, int y)
{
    return (uint16_t)(y * PT_COLUMNS + x);
}

bool pt_board_init(pt_board *board, uint8_t campaign);
/* Rebuilds distance + flow from the current bias field. Returns false when the
 * goal became unreachable, in which case the previous field is left intact. */
bool pt_board_rebuild(pt_board *board);
void pt_board_set_bias(pt_board *board, uint16_t cell, uint16_t bias);
bool pt_board_flow_at(const pt_board *board, float x, float y,
                      float *out_dx, float *out_dy);

/* ---------------------------------------------------------------- units -- */

typedef struct pt_unit {
    float    x, y;              /* logical cell coordinates */
    int32_t  integrity;
    int32_t  shield;
    float    shield_idle;       /* seconds since last damage */
    float    hold_remaining;    /* entangle */
    float    stun_remaining;    /* EMP */
    float    emit_timer;
    float    attack_timer;
    uint16_t kind;              /* index into pt_units[] */
    uint16_t serial;            /* stable id for targeting determinism */
    uint8_t  halted;            /* suppressor has stopped to fire */
    uint8_t  threshold_fired;
    uint8_t  alive;
} pt_unit;

typedef struct pt_unit_pool {
    pt_unit  slots[PT_MAX_UNITS];
    uint16_t live;
    uint16_t next_serial;
    uint32_t overflow;          /* asserted zero on the authored waves */
} pt_unit_pool;

void pt_units_reset(pt_unit_pool *pool);
pt_unit *pt_units_spawn(pt_unit_pool *pool, uint16_t kind, float x, float y);
void pt_units_update(pt_game *game, double dt);

/* ------------------------------------------------------------- fixtures -- */

typedef enum pt_target_mode {
    PT_TARGET_FIRST = 0,
    PT_TARGET_LAST,
    PT_TARGET_STRONGEST,
    PT_TARGET_CLOSEST,
    PT_TARGET_MODE_COUNT
} pt_target_mode;

typedef struct pt_fixture {
    uint16_t kind;              /* index into the campaign's fixture table */
    uint8_t  tier;              /* 0..PT_MAX_TIER-1 */
    uint8_t  pad;               /* 1-based pad id, 0 = empty slot */
    uint8_t  mode;              /* pt_target_mode */
    uint8_t  present;
    int32_t  integrity;
    int32_t  integrity_max;
    uint32_t invested;          /* for the 70% refund */
    float    cooldown;
    float    damage_scale;      /* cached largest support buff */
    float    range_scale;
    uint8_t  currency_tick;     /* cached largest support tick */
} pt_fixture;

void pt_fixtures_reset(pt_game *game);
bool pt_fixture_place(pt_game *game, uint8_t pad, uint16_t kind);
bool pt_fixture_upgrade(pt_game *game, uint8_t pad);
bool pt_fixture_sell(pt_game *game, uint8_t pad);
bool pt_fixture_repair(pt_game *game, uint8_t pad);
void pt_fixture_cycle_mode(pt_game *game, uint8_t pad);
pt_fixture *pt_fixture_at_pad(pt_game *game, uint8_t pad);
/* Recomputes every fixture's cached support buffs. Call after any board or
 * fixture change; never call per shot. */
void pt_fixtures_refresh_support(pt_game *game);
void pt_fixtures_update(pt_game *game, double dt);

/* Decoy Beacon gather (DECISIONS.md D-05). Runs after unit movement. */
void pt_gather_update(pt_game *game, double dt);

/* --------------------------------------------------------------- combat -- */

typedef struct pt_projectile {
    float    x, y;
    float    target_x, target_y;
    float    speed;
    int32_t  damage;
    float    splash_radius;
    uint16_t target_serial;
    uint16_t source_pad;
    uint8_t  damage_type;       /* pt_damage_type from content_generated.h */
    uint8_t  pierce_remaining;
    uint8_t  alive;
} pt_projectile;

typedef struct pt_projectile_pool {
    pt_projectile slots[PT_MAX_PROJECTILES];
    uint16_t      live;
    uint32_t      overflow;
} pt_projectile_pool;

void pt_combat_reset(pt_game *game);
void pt_combat_update(pt_game *game, double dt);
/* The seven-step resolution from GAME_DESIGN.md §9. Returns damage applied.
 * source_pad is the firing fixture's 1-based pad, or 0 for sourceless damage;
 * steps 2 and 7 need it for the cached support buff and currency tick. */
int32_t pt_combat_apply_damage(pt_game *game, pt_unit *unit,
                               int32_t damage, uint8_t damage_type,
                               uint16_t source_pad);
void pt_combat_kill_unit(pt_game *game, pt_unit *unit);
/* combat.c owns fixture damage and destruction; units.c routes halt-and-fire
 * and passing attacks through here rather than touching fixtures directly. */
void pt_combat_damage_fixture(pt_game *game, pt_fixture *fixture,
                              int32_t damage);

/* -------------------------------------------------------------- economy -- */

typedef struct pt_economy {
    int32_t  integrity;
    int32_t  integrity_max;
    int32_t  currency;
    uint32_t earned_total;
    uint32_t spent_total;
} pt_economy;

void pt_economy_reset(pt_game *game);
bool pt_economy_spend(pt_game *game, uint32_t amount);
void pt_economy_award(pt_game *game, uint32_t amount);
void pt_economy_leak(pt_game *game, const pt_unit *unit);
uint32_t pt_economy_wave_stipend(uint16_t wave_index);
uint32_t pt_economy_early_call_bonus(double seconds_remaining);

/* ----------------------------------------------------------------- game -- */

typedef enum pt_phase {
    PT_PHASE_TITLE = 0,
    PT_PHASE_CAMPAIGN_SELECT,
    PT_PHASE_BUILD,
    PT_PHASE_WAVE,
    PT_PHASE_PAUSE,
    PT_PHASE_VICTORY,
    PT_PHASE_DEFEAT,
    PT_PHASE_COUNT
} pt_phase;

typedef struct pt_wave_runtime {
    uint16_t index;             /* 0-based into the campaign's wave table */
    uint16_t spawned;
    uint16_t total;
    double   spawn_timer;
    double   build_remaining;
    bool     active;
} pt_wave_runtime;

typedef struct pt_cursor {
    int8_t  x, y;
    uint8_t pad;                /* 1-based pad under the cursor, 0 = none */
} pt_cursor;

struct pt_game {
    pt_board            board;
    pt_unit_pool        units;
    pt_projectile_pool  projectiles;
    pt_fixture          fixtures[PT_MAX_FIXTURES];
    pt_economy          economy;
    pt_wave_runtime     wave;
    pt_cursor           cursor;
    uint8_t             campaign;      /* index into pt_campaigns[] */
    uint8_t             phase;
    uint64_t            tick;
    uint64_t            rng;           /* game-owned deterministic PRNG */
    double              elapsed;
    uint32_t            fixtures_lost;
    bool                headless;
};

void pt_game_init(pt_game *game, uint8_t campaign, uint64_t seed);
void pt_game_step(pt_game *game, double dt);
void pt_game_set_phase(pt_game *game, pt_phase phase);
void pt_game_call_wave_early(pt_game *game);
uint32_t pt_rand(pt_game *game);
/* Deterministic [0,1) — never use rand() or drand48() anywhere in this tree. */
double pt_rand_unit(pt_game *game);

/* --------------------------------------------------------------- render -- */

struct ki_td_soft_renderer;

typedef struct pt_renderer {
    struct ki_td_soft_renderer *soft;
    const uint8_t *rgba;
    int width;
    int height;
    bool zoom_2x;
} pt_renderer;

bool pt_render_init(pt_renderer *renderer, int width, int height);
void pt_render_shutdown(pt_renderer *renderer);
bool pt_render_resize(pt_renderer *renderer, int width, int height);
void pt_render_frame(pt_renderer *renderer, const pt_game *game, double alpha);
bool pt_render_write_ppm(const pt_renderer *renderer, const char *path);

/* ------------------------------------------------------------------ hud -- */

void pt_hud_draw(pt_renderer *renderer, const pt_game *game);

/* ---------------------------------------------------------------- input -- */

struct kittyin_event;

typedef struct pt_input_state {
    int8_t move_x, move_y;
    bool   confirm, cancel, upgrade, sell, repair, cycle_mode;
    bool   call_wave, pause, zoom, quit;
} pt_input_state;

void pt_input_reset(pt_input_state *input);
void pt_input_event(pt_input_state *input, const struct kittyin_event *event);
void pt_input_apply(pt_game *game, pt_input_state *input);

/* ---------------------------------------------------------------- audio -- */

bool pt_audio_init(void);
void pt_audio_shutdown(void);
void pt_audio_cue(uint32_t cue);
void pt_audio_scene(uint32_t scene);
void pt_audio_update(double dt);

/* ----------------------------------------------------------------- save -- */

typedef struct pt_records {
    uint16_t best_integrity[PT_CAMPAIGN_COUNT];
    uint32_t fastest_clear_ms[PT_CAMPAIGN_COUNT];
    uint32_t best_unspent[PT_CAMPAIGN_COUNT];
    uint32_t runs[PT_CAMPAIGN_COUNT];
    uint8_t  cleared[PT_CAMPAIGN_COUNT];
} pt_records;

typedef struct pt_settings {
    float   gain_music;         /* all gains bounded to [0,1] */
    float   gain_ambience;
    float   gain_sfx;
    float   gain_ui;
    uint8_t text_scale;         /* 1..3 */
    bool    reduced_motion;
    bool    zoom_default;
} pt_settings;

void pt_settings_defaults(pt_settings *settings);
bool pt_save_load_settings(pt_settings *settings);
bool pt_save_store_settings(const pt_settings *settings);

bool pt_save_load_records(pt_records *records);
bool pt_save_store_records(const pt_records *records);
void pt_save_note_run(pt_records *records, const pt_game *game, bool cleared);
bool pt_campaign_unlocked(const pt_records *records, uint8_t campaign);

/* -------------------------------------------------------------- content -- */

const pt_campaign_def *pt_campaign(uint8_t campaign);
const pt_fixture_def  *pt_fixture_def_at(uint8_t campaign, uint16_t kind);
const pt_unit_def     *pt_unit_def_at(uint8_t campaign, uint16_t kind);
const pt_wave_def     *pt_wave_def_at(uint8_t campaign, uint16_t wave);
const pt_pad_def      *pt_pad(uint8_t pad_id);
const char            *pt_content_hash_string(void);

#endif /* PLEB_TOWER_H */
