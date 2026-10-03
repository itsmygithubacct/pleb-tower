/* Every route is playable in both directions, with independent geometry. */
#include "pt_test.h"
#include "pleb_tower.h"

#include <math.h>
#include <string.h>

void pt_test_maps(void)
{
    PT_CHECK_EQ_INT(PT_MAP_COUNT, 2);
    PT_CHECK(strcmp(pt_map(1u)->id, "rail-yard") == 0, "stable Rail Yard map id");
    PT_CHECK_EQ_INT(pt_map(1u)->pad_count, 20);
    PT_CHECK(pt_map_pad(1u, 21u) == NULL, "unused capacity is not a build pad");
    for (uint8_t map = 0u; map < PT_MAP_COUNT; ++map) {
        for (uint8_t campaign = 0u; campaign < PT_CAMPAIGN_COUNT; ++campaign) {
            pt_game game;
            pt_game_init_map(&game, map, campaign, 123u);
            game.headless = true;
            const pt_campaign_def *operation = pt_game_campaign(&game);
            PT_CHECK_EQ_INT(game.board.map, map);
            PT_CHECK_EQ_INT(game.board.spawn_cell,
                            pt_cell_index(operation->spawn_x, operation->spawn_y));
            unsigned int pads = 0u, lane = 0u;
            for (size_t cell = 0u; cell < PT_CELL_COUNT; ++cell) {
                if (game.board.pad_index[cell]) ++pads;
                if (game.board.lane[cell]) {
                    ++lane;
                    PT_CHECK(!game.board.pad_index[cell], "road never crosses a build pad");
                    PT_CHECK(game.board.distance[cell] != PT_DISTANCE_UNREACHABLE,
                              "every road cell reaches the goal");
                }
            }
            PT_CHECK_EQ_INT(pads, pt_map(map)->pad_count);
            PT_CHECK_EQ_INT(lane, map == 0u ? 51 : 61);

            /* Walk an actual enemy along every bend, including the reverse route. */
            pt_unit *unit = pt_units_spawn(&game.units, 0u,
                (float)operation->spawn_x + 0.5f, (float)operation->spawn_y + 0.5f);
            PT_CHECK(unit != NULL, "spawn route probe");
            if (!unit) continue;
            float farthest = 0.0f;
            for (unsigned int tick = 0u; unit->alive && tick < 18000u; ++tick) {
                pt_units_update(&game, PT_STEP_SECONDS);
                float nearest = 10000.0f;
                for (unsigned int cell = 0u; cell < PT_CELL_COUNT; ++cell) {
                    if (!game.board.lane[cell]) continue;
                    float x = (float)(cell % PT_COLUMNS) + 0.5f;
                    float y = (float)(cell / PT_COLUMNS) + 0.5f;
                    float dx = (float)game.board.flow_x[cell];
                    float dy = (float)game.board.flow_y[cell];
                    float t = fminf(1.0f, fmaxf(0.0f,
                        (unit->x - x) * dx + (unit->y - y) * dy));
                    float offset_x = unit->x - x - t * dx;
                    float offset_y = unit->y - y - t * dy;
                    nearest = fminf(nearest, offset_x * offset_x + offset_y * offset_y);
                }
                farthest = fmaxf(farthest, sqrtf(nearest));
            }
            /* Interpolated flow rounds the inside of bends. Allow the curb,
             * while proving the enemy never takes a shortcut across scenery. */
            PT_CHECK(farthest <= 0.65f, "map %u campaign %u: road deviation %.3f cells",
                      map, campaign, farthest);
            PT_CHECK(!unit->alive, "ground enemy reaches the selected goal");
            PT_CHECK_EQ_INT(game.economy.integrity,
                operation->starting_integrity - operation->units[0].mass);

            /* Air uses that map's endpoints, while still flying directly. */
            uint16_t air = 0u;
            while (air < operation->unit_count && !operation->units[air].air) ++air;
            PT_CHECK(air < operation->unit_count, "campaign has an air route");
            unit = pt_units_spawn(&game.units, air,
                (float)operation->spawn_x + 0.5f, (float)operation->spawn_y + 0.5f);
            PT_CHECK(unit != NULL, "spawn air route probe");
            if (!unit) continue;
            for (unsigned int tick = 0u; unit->alive && tick < 18000u; ++tick)
                pt_units_update(&game, PT_STEP_SECONDS);
            PT_CHECK(!unit->alive, "air reaches selected map's goal");
            PT_CHECK_EQ_INT(game.economy.integrity,
                operation->starting_integrity - operation->units[0].mass - operation->units[air].mass);
        }
    }
    pt_game game;
    pt_game_init_map(&game, 1u, 0u, 456u);
    game.headless = true;
    PT_CHECK(!pt_fixture_place(&game, 21u, 0u), "Rail Yard rejects Maple-only pad ids");
    PT_CHECK(pt_fixture_place(&game, 6u, 0u), "place gun at loading-yard bend");
    pt_unit *target = pt_units_spawn(&game.units, 0u, 24.5f, 10.5f);
    PT_CHECK(target != NULL, "spawn in Rail Yard gun range");
    pt_fixtures_update(&game, PT_STEP_SECONDS);
    PT_CHECK(game.projectiles.live > 0u, "gun fires from selected map's pad coordinates");
    for (unsigned int tick = 0u; tick < 120u && target && target->alive; ++tick)
        pt_combat_update(&game, PT_STEP_SECONDS);
    PT_CHECK(target && target->integrity < pt_unit_def_at(0u, 0u)->integrity,
              "projectile hits enemy on Rail Yard road");
    PT_CHECK_EQ_INT(pt_game_wave(&game, 0u)->total_units, 8);
    PT_CHECK(pt_game_wave(&game, 0u)->interval > pt_wave_def_at(0u, 0u)->interval,
              "Rail Yard uses its separately tuned opening");
}
