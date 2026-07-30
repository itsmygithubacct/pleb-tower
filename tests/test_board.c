/* Board tests: the distance field, the flow field, bias, and the
 * reachability guarantee. */
#include "pt_test.h"
#include "pleb_tower.h"

#include <math.h>
#include <string.h>

static pt_board board;

static void test_field_shape(void)
{
    PT_CHECK(pt_board_init(&board, 0u), "holdout board init");
    PT_CHECK_EQ_INT(board.distance[board.goal_cell], 0);
    PT_CHECK(board.distance[board.spawn_cell] != PT_DISTANCE_UNREACHABLE,
             "spawn must reach the goal");

    size_t lane_cells = 0;
    for (size_t i = 0; i < PT_CELL_COUNT; ++i)
        if (board.lane[i]) ++lane_cells;
    /* The authored lane is 51 cells; the compiler and this test agree
     * independently, which is the point of checking it from both sides. */
    PT_CHECK_EQ_INT(lane_cells, 51);

    size_t pads = 0;
    for (size_t i = 0; i < PT_CELL_COUNT; ++i)
        if (board.pad_index[i]) ++pads;
    PT_CHECK_EQ_INT(pads, PT_PAD_COUNT);

    /* No pad may sit on the lane. */
    for (size_t i = 0; i < PT_CELL_COUNT; ++i)
        PT_CHECK(!(board.pad_index[i] && board.lane[i]),
                 "pad %zu overlaps the lane", i);
}

static void test_flow_never_stalls(void)
{
    PT_CHECK(pt_board_init(&board, 0u), "board init");
    for (size_t i = 0; i < PT_CELL_COUNT; ++i) {
        if (!board.lane[i] || i == board.goal_cell) continue;
        PT_CHECK(board.flow_x[i] != 0 || board.flow_y[i] != 0,
                 "flow stalls at cell %zu", i);
    }
}

/* Walking the flow field from spawn must terminate at the goal, and the
 * distance must decrease monotonically on the way. */
static void test_flow_reaches_goal(void)
{
    PT_CHECK(pt_board_init(&board, 0u), "board init");
    int x = (int)(board.spawn_cell % PT_COLUMNS);
    int y = (int)(board.spawn_cell / PT_COLUMNS);
    uint16_t previous = board.distance[pt_cell_index(x, y)];
    int steps = 0;

    while (pt_cell_index(x, y) != board.goal_cell && steps < PT_CELL_COUNT) {
        uint16_t cell = pt_cell_index(x, y);
        x += board.flow_x[cell];
        y += board.flow_y[cell];
        uint16_t next = board.distance[pt_cell_index(x, y)];
        PT_CHECK(next < previous, "distance did not decrease at step %d",
                 steps);
        previous = next;
        ++steps;
    }
    PT_CHECK_EQ_INT(pt_cell_index(x, y), board.goal_cell);
    /* 51 lane cells means at most 50 steps from one end to the other. */
    PT_CHECK(steps > 0 && steps < 51, "walk took %d steps", steps);
}

static void test_both_campaigns_differ(void)
{
    pt_board holdout, cordon;
    PT_CHECK(pt_board_init(&holdout, 0u), "holdout init");
    PT_CHECK(pt_board_init(&cordon, 1u), "cordon init");

    PT_CHECK_EQ_INT(holdout.spawn_cell, cordon.goal_cell);
    PT_CHECK_EQ_INT(holdout.goal_cell, cordon.spawn_cell);
    PT_CHECK(memcmp(holdout.lane, cordon.lane, sizeof holdout.lane) == 0,
             "both campaigns must share one lane");
    PT_CHECK(memcmp(holdout.distance, cordon.distance,
                    sizeof holdout.distance) != 0,
             "reversed campaigns must produce different fields");
}

static void test_bias_rebuild_is_safe(void)
{
    PT_CHECK(pt_board_init(&board, 0u), "board init");
    uint32_t rebuilds = board.rebuild_count;

    /* Bias only ever discounts, so the goal can never become unreachable and
     * every rebuild must succeed. */
    for (size_t i = 0; i < PT_CELL_COUNT; ++i)
        if (board.lane[i]) pt_board_set_bias(&board, (uint16_t)i, 15u);

    PT_CHECK(pt_board_rebuild(&board), "rebuild under maximum bias");
    PT_CHECK_EQ_INT(board.rebuild_count, (long)rebuilds + 1);
    PT_CHECK(board.distance[board.spawn_cell] != PT_DISTANCE_UNREACHABLE,
             "spawn still reaches the goal under bias");
    PT_CHECK_EQ_INT(board.distance[board.goal_cell], 0);
}

static void test_flow_sampling(void)
{
    float dx = 0.0f, dy = 0.0f;
    PT_CHECK(pt_board_init(&board, 0u), "board init");

    int sx = (int)(board.spawn_cell % PT_COLUMNS);
    int sy = (int)(board.spawn_cell / PT_COLUMNS);
    PT_CHECK(pt_board_flow_at(&board, (float)sx + 0.5f, (float)sy + 0.5f,
                              &dx, &dy),
             "sampling at the spawn centre");
    float length = sqrtf(dx * dx + dy * dy);
    PT_CHECK(fabsf(length - 1.0f) < 0.001f, "flow is normalised (%f)",
             (double)length);

    /* Sampling off the lane entirely must fail rather than invent a heading. */
    PT_CHECK(!pt_board_flow_at(&board, 0.5f, 0.5f, &dx, &dy),
             "off-lane sampling must fail");
}

void pt_test_board(void)
{
    test_field_shape();
    test_flow_never_stalls();
    test_flow_reaches_goal();
    test_both_campaigns_differ();
    test_bias_rebuild_is_safe();
    test_flow_sampling();
}
