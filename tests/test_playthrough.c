/* Use the player's input path and actual starting economy to clear both
 * campaigns. A pool test with unlimited lives cannot establish playability. */
#include "pt_test.h"
#include "pleb_tower.h"
#include "kitty_input.h"

#include <string.h>

typedef pt_sim_order_entry purchase;

#include "orders_generated.h"

unsigned int pt_input_ui_panel(void);
size_t pt_input_ui_focus(void);

static void tap(pt_game *game, pt_input_state *input, uint32_t key)
{
    kittyin_event event = {0};
    event.kind = KITTYIN_EVENT_KEY;
    event.data.key.key = key;
    event.data.key.action = KITTYKB_ACTION_PRESS;
    event.data.key.event_type_explicit = true;
    pt_input_event(input, &event);
    pt_input_apply(game, input);
    event.data.key.action = KITTYKB_ACTION_RELEASE;
    pt_input_event(input, &event);
    pt_input_apply(game, input);
}

static bool buy(pt_game *game, pt_input_state *input, const purchase *item)
{
    const pt_pad_def *pad = pt_game_pad(game, item->pad);
    pt_fixture *fixture;
    if (!pad) return false;
    if (pt_input_ui_panel() != 0u) tap(game, input, KITTYKB_KEY_ESCAPE);
    while (game->cursor.x != (int8_t)pad->x)
        tap(game, input, game->cursor.x < (int8_t)pad->x ? 'd' : 'a');
    while (game->cursor.y != (int8_t)pad->y)
        tap(game, input, game->cursor.y < (int8_t)pad->y ? 's' : 'w');
    fixture = pt_fixture_at_pad(game, item->pad);
    if (item->tier == PT_SIM_REPAIR_TIER) {
        if (!fixture || fixture->integrity == fixture->integrity_max) return true;
        int32_t cost = (fixture->integrity_max - fixture->integrity + PT_REPAIR_INTEGRITY_PER_UNIT - 1) / PT_REPAIR_INTEGRITY_PER_UNIT;
        if (game->economy.currency < cost) return true;
        tap(game, input, 'r');
        return fixture->integrity == fixture->integrity_max;
    }
    if (!fixture) {
        const pt_fixture_def *definition = pt_fixture_def_at(game->campaign, item->kind);
        if (definition->tiers[0].cost > (uint32_t)game->economy.currency)
            return true; /* The plan is a spending priority, bounded by income. */
        tap(game, input, KITTYKB_KEY_ENTER);
        for (size_t attempt = 0u; attempt < PT_ROLE_COUNT &&
             pt_input_ui_focus() != item->kind; ++attempt)
            tap(game, input, KITTYKB_KEY_DOWN);
        if (pt_input_ui_focus() != item->kind) return false;
        tap(game, input, KITTYKB_KEY_ENTER);
        fixture = pt_fixture_at_pad(game, item->pad);
    }
    if (!fixture || fixture->kind != item->kind) return false;
    while (fixture->tier < item->tier) {
        const pt_fixture_def *definition = pt_fixture_def_at(game->campaign, item->kind);
        if (definition->tiers[fixture->tier + 1u].cost > (uint32_t)game->economy.currency)
            break;
        uint8_t before = fixture->tier;
        tap(game, input, 'u');
        if (fixture->tier == before) return false;
    }
    if (pt_input_ui_panel() != 0u) tap(game, input, KITTYKB_KEY_ESCAPE);
    return true;
}

static void screenshot(pt_game *game, const char *path)
{
    pt_renderer renderer;
    if (!pt_render_init(&renderer, 960, 540)) {
        PT_CHECK(false, "initialize playthrough render");
        return;
    }
    pt_render_frame(&renderer, game, 0.0);
    PT_CHECK(pt_render_write_ppm(&renderer, path), "write %s", path);
    pt_render_shutdown(&renderer);
}

static void play_campaign(pt_game *game, pt_input_state *input,
                           const purchase *order, size_t count)
{
    size_t next = 0u;
    uint64_t ticks = 0u;
    bool rendered = false;
    while ((game->phase == PT_PHASE_BUILD || game->phase == PT_PHASE_WAVE) &&
           ticks < UINT64_C(120000)) {
        if (game->phase == PT_PHASE_BUILD) {
            while (next < count && order[next].wave == game->wave.index + 1u) {
                bool bought = buy(game, input, &order[next]);
                PT_CHECK(bought, "campaign %u wave %u: buy kind %u tier %u on pad %u (funds %d)",
                    game->campaign, order[next].wave, order[next].kind,
                    order[next].tier, order[next].pad, game->economy.currency);
                if (!bought) return;
                ++next;
            }
            tap(game, input, KITTYKB_KEY_TAB);
        }
        if (game->phase == PT_PHASE_WAVE && game->wave.index % 2u == 1u && game->speed != 2u)
            tap(game, input, 'f');
        pt_game_advance(game, PT_STEP_SECONDS);
        if (!rendered && game->units.live >= 3u && game->projectiles.live > 0u) {
            screenshot(game, game->campaign == 0u ?
                "build/holdout-combat.ppm" : "build/cordon-combat.ppm");
            rendered = true;
        }
        ++ticks;
    }
    pt_input_apply(game, input); /* Publish the score and durable unlock. */
    PT_CHECK_EQ_INT(next, count);
    PT_CHECK_EQ_INT(game->phase, PT_PHASE_VICTORY);
    PT_CHECK(game->economy.integrity > 0, "campaign ends with real lives remaining");
    PT_CHECK_EQ_INT(game->units.overflow, 0);
    PT_CHECK_EQ_INT(game->projectiles.overflow, 0);
    printf("PLAY map=%u campaign=%u integrity=%d/%d fixtures_lost=%u time=%.1fs\n",
        game->board.map, game->campaign, game->economy.integrity, game->economy.integrity_max,
        game->fixtures_lost, game->elapsed);
}

void pt_test_playthrough(void)
{
    pt_game game;
    pt_input_state input;
    pt_records records;
    memset(&records, 0, sizeof records);
    PT_CHECK(pt_save_store_records(&records), "start playthrough with a fresh profile");
    PT_CHECK(!pt_campaign_unlocked(&records, 1u), "CORDON begins locked");
    pt_game_init(&game, 0u, UINT64_C(0x504c4159));
    pt_input_reset(&input);
    pt_game_set_phase(&game, PT_PHASE_TITLE);
    screenshot(&game, "build/title.ppm");
    tap(&game, &input, 'h');
    PT_CHECK_EQ_INT(game.phase, PT_PHASE_HELP);
    screenshot(&game, "build/help.ppm");
    tap(&game, &input, KITTYKB_KEY_ESCAPE);
    tap(&game, &input, KITTYKB_KEY_ENTER);
    tap(&game, &input, KITTYKB_KEY_ENTER);
    PT_CHECK_EQ_INT(game.phase, PT_PHASE_MAP_SELECT);
    screenshot(&game, "build/map-selection.ppm");
    tap(&game, &input, KITTYKB_KEY_ENTER);
    PT_CHECK_EQ_INT(game.phase, PT_PHASE_BUILD);
    double remaining = game.wave.build_remaining;
    for (int i = 0; i < 1800; ++i) pt_game_step(&game, PT_STEP_SECONDS);
    PT_CHECK_EQ_INT(game.phase, PT_PHASE_BUILD);
    PT_CHECK(game.wave.build_remaining == remaining, "first wave waits for the player");
    tap(&game, &input, 'h');
    double elapsed = game.elapsed;
    for (int i = 0; i < 120; ++i) pt_game_step(&game, PT_STEP_SECONDS);
    PT_CHECK(game.elapsed == elapsed, "help freezes simulation");
    tap(&game, &input, 'h');
    play_campaign(&game, &input, maple_loop_holdout_orders, sizeof maple_loop_holdout_orders / sizeof maple_loop_holdout_orders[0]);
    if (game.phase != PT_PHASE_VICTORY) return;
    screenshot(&game, "build/victory.ppm");
    PT_CHECK(pt_save_load_records(&records), "read completed campaign records");
    PT_CHECK(pt_campaign_unlocked(&records, 1u), "actual HOLDOUT clear unlocks CORDON");
    tap(&game, &input, KITTYKB_KEY_ENTER);
    pt_input_reset(&input); /* A restart must keep the unlock. */
    tap(&game, &input, KITTYKB_KEY_ENTER);
    tap(&game, &input, KITTYKB_KEY_DOWN);
    tap(&game, &input, KITTYKB_KEY_ENTER);
    PT_CHECK_EQ_INT(game.phase, PT_PHASE_MAP_SELECT);
    tap(&game, &input, KITTYKB_KEY_ENTER);
    PT_CHECK_EQ_INT(game.campaign, 1);
    PT_CHECK_EQ_INT(game.phase, PT_PHASE_BUILD);
    play_campaign(&game, &input, maple_loop_cordon_orders, sizeof maple_loop_cordon_orders / sizeof maple_loop_cordon_orders[0]);
    for (uint8_t campaign = 0u; campaign < PT_CAMPAIGN_COUNT; ++campaign) {
        if (game.phase != PT_PHASE_VICTORY) return;
        tap(&game, &input, KITTYKB_KEY_ENTER); /* outcome -> title */
        tap(&game, &input, KITTYKB_KEY_ENTER); /* title -> campaigns */
        if (campaign != 0u) tap(&game, &input, KITTYKB_KEY_DOWN);
        tap(&game, &input, KITTYKB_KEY_ENTER);
        PT_CHECK_EQ_INT(game.phase, PT_PHASE_MAP_SELECT);
        if (pt_input_selected_map() == 0u) tap(&game, &input, KITTYKB_KEY_RIGHT);
        screenshot(&game, "build/rail-yard-selection.ppm");
        tap(&game, &input, KITTYKB_KEY_ENTER);
        PT_CHECK_EQ_INT(game.board.map, 1);
        PT_CHECK_EQ_INT(game.campaign, campaign);
        PT_CHECK_EQ_INT(game.phase, PT_PHASE_BUILD);
        screenshot(&game, "build/rail-yard-ready.ppm");
        if (campaign == 0u)
            play_campaign(&game, &input, rail_yard_holdout_orders,
                sizeof rail_yard_holdout_orders / sizeof rail_yard_holdout_orders[0]);
        else
            play_campaign(&game, &input, rail_yard_cordon_orders,
                sizeof rail_yard_cordon_orders / sizeof rail_yard_cordon_orders[0]);
    }
    tap(&game, &input, 'h');
    tap(&game, &input, KITTYKB_KEY_ESCAPE);
    pt_input_reset(&input);
    pt_input_apply(&game, &input);
    PT_CHECK(pt_save_load_records(&records), "load all map records after help and UI reload");
    for (uint8_t map = 0u; map < PT_MAP_COUNT; ++map)
        for (uint8_t campaign = 0u; campaign < PT_CAMPAIGN_COUNT; ++campaign) {
            PT_CHECK(records.cleared[map][campaign] != 0u, "each map/campaign has its own clear");
            PT_CHECK_EQ_INT(records.runs[map][campaign], 1);
        }

}
