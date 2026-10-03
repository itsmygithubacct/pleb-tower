/* Entry point: CLI dispatch and the fixed-step terminal host.
 *
 * kilix-game-kit owns the loop, the terminal lifecycle and the reversible
 * signal handling. This file only decides what the callbacks do. */
#include "pleb_tower.h"

#include "kilix_game_kit.h"
#include "kitty_input_posix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

typedef struct pt_app {
    pt_game        game;
    pt_renderer    renderer;
    pt_input_state input;
    pt_records     records;
    kittyin_gamepads gamepads;
    uint64_t input_ticks;
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
    kittyin_event mapped = *event;
    if (event->kind == KITTYIN_EVENT_MOUSE) {
        kittyts_session *session = kilix_game_host_terminal(host);
        int x = event->data.mouse.x;
        int y = event->data.mouse.y;
        if (!event->data.mouse.pixel_coordinates) {
            x *= kittyts_cell_width(session);
            y *= kittyts_cell_height(session);
        }
        x -= kittyts_origin_x(session);
        y -= kittyts_origin_y(session);
        if (!pt_render_pointer(&self->renderer, x, y, &x, &y)) return;
        mapped.data.mouse.x = x;
        mapped.data.mouse.y = y;
        mapped.data.mouse.pixel_coordinates = true;
    }
    pt_input_event(&self->input, &mapped);
    if (self->input.quit) kilix_game_host_request_stop(host);
}

static bool on_step(kilix_game_host *host, void *user, double step_seconds)
{
    pt_app *self = user;
    kittyin_gamepad_event event;
    ++self->input_ticks;
    kittyin_gamepads_poll(&self->gamepads, (int64_t)(self->input_ticks * 1000u / PT_TICK_HZ));
    while (kittyin_gamepads_next(&self->gamepads, &event))
        pt_input_gamepad_event(&self->input, &event);
    pt_input_apply(&self->game, &self->input);
    if (self->input.quit) kilix_game_host_request_stop(host);
    pt_game_advance(&self->game, step_seconds);
    pt_audio_sync(&self->game);
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
    kittyin_gamepads_close(&self->gamepads);
}

/* ----------------------------------------------------------- self tests -- */

static int selftest(void)
{
    pt_game *game = &app.game;
    const pt_campaign_def *def;
    int campaigns_checked = 0;

    for (uint8_t m = 0; m < PT_MAP_COUNT; ++m)
    for (uint8_t c = 0; c < PT_CAMPAIGN_COUNT; ++c) {
        pt_game_init_map(game, m, c, 0x1234u);
        def = pt_game_campaign(game);

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
            "usage: %s [--campaign N] [--map ID] [--mute]\n"
            "       %s [--map ID] --simulate N FILE\n"
            "       %s --selftest | --render-test FILE | --audio-test FILE\n",
            program,
            program,
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
    uint8_t map = 0u;
    const char *simulation_file = NULL;
    bool direct_campaign = false;

    if (argc == 2 && strcmp(argv[1], "--selftest") == 0) return selftest();
    if (argc == 3 && strcmp(argv[1], "--render-test") == 0)
        return render_test(argv[2]);
    if (argc == 3 && strcmp(argv[1], "--audio-test") == 0)
        return pt_audio_render_test(argv[2]) ? EXIT_SUCCESS : EXIT_FAILURE;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--mute") == 0) {
            pt_audio_set_muted(true);
        } else if (strcmp(argv[i], "--map") == 0 && i + 1 < argc) {
            const char *id = argv[++i];
            bool found = false;
            for (uint8_t m = 0u; m < PT_MAP_COUNT; ++m) {
                char number[8];
                (void)snprintf(number, sizeof number, "%u", (unsigned int)m);
                if (strcmp(id, pt_map(m)->id) == 0 || strcmp(id, number) == 0) {
                    map = m;
                    found = true;
                    break;
                }
            }
            if (!found) return usage(argv[0]);
        } else if (strcmp(argv[i], "--simulate") == 0 && i + 2 < argc) {
            const char *value = argv[++i];
            if (strcmp(value, "0") != 0 && strcmp(value, "1") != 0)
                return usage(argv[0]);
            campaign = (uint8_t)(value[0] - '0');
            simulation_file = argv[++i];
        } else if (strcmp(argv[i], "--campaign") == 0 && i + 1 < argc) {
            char *end;
            const char *value_string = argv[++i];
            errno = 0;
            long value = strtol(value_string, &end, 10);
            if (errno || end == value_string || *end != '\0' ||
                value < 0 || value >= PT_CAMPAIGN_COUNT) return usage(argv[0]);
            campaign = (uint8_t)value;
            direct_campaign = true;
        } else return usage(argv[0]);
    }

    if (simulation_file != NULL) return pt_simulate_map_file(map, campaign, simulation_file);

    memset(&host, 0, sizeof host);
    kilix_game_host_options_init(&options);
    options.input_fd = STDIN_FILENO;
    options.output_fd = STDOUT_FILENO;
    options.clock.step_ns = KILIX_GAME_NANOSECONDS_PER_SECOND / PT_TICK_HZ;
    options.install_signals = true;
    options.terminal.mouse_tracking = KITTYIN_MOUSE_TRACKING_MOTION;

    memset(&callbacks, 0, sizeof callbacks);
    callbacks.start = on_start;
    callbacks.event = on_event;
    callbacks.step = on_step;
    callbacks.render = on_render;
    callbacks.stop = on_stop;

    pt_save_load_records(&app.records);
    pt_audio_init();
    pt_game_init_map(&app.game, map, campaign, 0x504C4542u);
    pt_input_reset(&app.input);
    kittyin_gamepads_init(&app.gamepads);
    if (!direct_campaign) pt_game_set_phase(&app.game, PT_PHASE_TITLE);
    pt_audio_sync(&app.game);

    int status = kilix_game_host_run(&host, &options, &callbacks, &app);

    pt_audio_shutdown();
    if (status != EXIT_SUCCESS && host.terminal_errno != 0)
        fprintf(stderr, "Pleb Tower needs a Kitty graphics terminal: %s\n",
                strerror(host.terminal_errno));
    return status;
}
