/* Entry point: CLI dispatch and the fixed-step terminal host.
 *
 * kilix-game-kit owns the loop, the terminal lifecycle and the reversible
 * signal handling. This file only decides what the callbacks do. */
#include "pleb_tower.h"

#include "kilix_game_kit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct pt_app {
    pt_game        game;
    pt_renderer    renderer;
    pt_input_state input;
    pt_records     records;
} pt_app;

static pt_app app;

/* ------------------------------------------------------------ callbacks -- */

static bool on_start(kilix_game_host *host, void *user)
{
    pt_app *self = user;
    kittyts_session *session = kilix_game_host_terminal(host);
    int width = kittyts_width(session);
    int height = kittyts_height(session);
    if (width <= 0 || height <= 0) {
        width = PT_LOGICAL_WIDTH;
        height = PT_LOGICAL_HEIGHT;
    }
    return pt_render_init(&self->renderer, width, height);
}

static void on_event(kilix_game_host *host, void *user,
                     const kittyin_event *event)
{
    pt_app *self = user;
    pt_input_event(&self->input, event);
    if (self->input.quit) kilix_game_host_request_stop(host);
}

static bool on_step(kilix_game_host *host, void *user, double step_seconds)
{
    pt_app *self = user;
    (void)host;
    pt_input_apply(&self->game, &self->input);
    pt_game_step(&self->game, step_seconds);
    pt_audio_update(step_seconds);
    return true;
}

static bool on_render(kilix_game_host *host, void *user, double alpha)
{
    pt_app *self = user;
    kittyts_session *session = kilix_game_host_terminal(host);
    int width = 0, height = 0;

    if (kittyts_check_resize(session, &width, &height) && width > 0 &&
        height > 0)
        pt_render_resize(&self->renderer, width, height);

    pt_render_frame(&self->renderer, &self->game, alpha);
    if (!self->renderer.rgba) return true;
    return kittyts_present(session, self->renderer.rgba,
                           self->renderer.width, self->renderer.height);
}

static void on_stop(kilix_game_host *host, void *user)
{
    pt_app *self = user;
    (void)host;
    pt_render_shutdown(&self->renderer);
}

/* ----------------------------------------------------------- self tests -- */

static int selftest(void)
{
    pt_game *game = &app.game;
    const pt_campaign_def *def;
    int campaigns_checked = 0;

    for (uint8_t c = 0; c < PT_CAMPAIGN_COUNT; ++c) {
        pt_game_init(game, c, 0x1234u);
        def = pt_campaign(c);

        if (game->board.distance[game->board.spawn_cell] ==
            PT_DISTANCE_UNREACHABLE) {
            fprintf(stderr, "FAIL %s: goal unreachable from spawn\n", def->id);
            return EXIT_FAILURE;
        }
        if (game->board.distance[game->board.goal_cell] != 0u) {
            fprintf(stderr, "FAIL %s: goal distance is not zero\n", def->id);
            return EXIT_FAILURE;
        }
        if (game->economy.integrity != def->starting_integrity ||
            game->economy.currency != def->starting_currency) {
            fprintf(stderr, "FAIL %s: economy did not reset\n", def->id);
            return EXIT_FAILURE;
        }
        if (def->wave_count == 0u || def->fixture_count != PT_ROLE_COUNT) {
            fprintf(stderr, "FAIL %s: content table shape\n", def->id);
            return EXIT_FAILURE;
        }
        ++campaigns_checked;
    }

    /* The flow field must lead somewhere from every lane cell. */
    pt_game_init(game, 0u, 0x1234u);
    for (int y = 0; y < PT_ROWS; ++y) {
        for (int x = 0; x < PT_COLUMNS; ++x) {
            uint16_t cell = pt_cell_index(x, y);
            if (!game->board.lane[cell]) continue;
            if (cell == game->board.goal_cell) continue;
            if (game->board.flow_x[cell] == 0 &&
                game->board.flow_y[cell] == 0) {
                fprintf(stderr, "FAIL flow field stalls at %d,%d\n", x, y);
                return EXIT_FAILURE;
            }
        }
    }

    printf("PASS selftest campaigns=%d pads=%d content=%.12s\n",
           campaigns_checked, PT_PAD_COUNT, pt_content_hash_string());
    return EXIT_SUCCESS;
}

static int render_test(const char *path)
{
    int status = EXIT_FAILURE;
    pt_game_init(&app.game, 0u, 0x1234u);
    if (!pt_render_init(&app.renderer, PT_LOGICAL_WIDTH, PT_LOGICAL_HEIGHT)) {
        fprintf(stderr, "FAIL render init\n");
        return EXIT_FAILURE;
    }
    pt_render_frame(&app.renderer, &app.game, 0.0);
    if (pt_render_write_ppm(&app.renderer, path)) {
        printf("PASS render-test %s %dx%d\n", path, app.renderer.width,
               app.renderer.height);
        status = EXIT_SUCCESS;
    } else {
        fprintf(stderr, "FAIL render-test write %s\n", path);
    }
    pt_render_shutdown(&app.renderer);
    return status;
}

static int usage(const char *program)
{
    fprintf(stderr,
            "usage: %s [--selftest | --render-test FILE | --campaign N]\n",
            program);
    return EXIT_FAILURE;
}

/* ----------------------------------------------------------------- main -- */

int main(int argc, char **argv)
{
    kilix_game_host host;
    kilix_game_host_options options;
    kilix_game_host_callbacks callbacks;
    uint8_t campaign = 0u;

    if (argc == 2 && strcmp(argv[1], "--selftest") == 0) return selftest();
    if (argc == 3 && strcmp(argv[1], "--render-test") == 0)
        return render_test(argv[2]);
    if (argc == 3 && strcmp(argv[1], "--campaign") == 0) {
        long value = strtol(argv[2], NULL, 10);
        if (value < 0 || value >= PT_CAMPAIGN_COUNT) return usage(argv[0]);
        campaign = (uint8_t)value;
    } else if (argc != 1) {
        return usage(argv[0]);
    }

    memset(&host, 0, sizeof host);
    kilix_game_host_options_init(&options);
    options.input_fd = STDIN_FILENO;
    options.output_fd = STDOUT_FILENO;
    options.clock.step_ns = KILIX_GAME_NANOSECONDS_PER_SECOND / PT_TICK_HZ;
    options.install_signals = true;

    memset(&callbacks, 0, sizeof callbacks);
    callbacks.start = on_start;
    callbacks.event = on_event;
    callbacks.step = on_step;
    callbacks.render = on_render;
    callbacks.stop = on_stop;

    pt_save_load_records(&app.records);
    pt_audio_init();
    pt_game_init(&app.game, campaign, 0x504C4542u);
    pt_input_reset(&app.input);

    int status = kilix_game_host_run(&host, &options, &callbacks, &app);

    pt_audio_shutdown();
    return status;
}
