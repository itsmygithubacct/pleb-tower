/* Board: the lane grid, the cost-to-goal distance field, and the flow field
 * derived from it.
 *
 * One search runs from the goal and every unit reads the result — the Red Blob
 * Games formulation. No per-unit pathfinding runs anywhere in this game.
 *
 * The field is a *cost* field, not a step count: entering a cell costs
 * PT_STEP_COST minus that cell's bias, floored at 1. A Decoy Beacon writes a
 * bias to attract traffic. Because bias only ever discounts and never removes
 * a cell, the goal can never become unreachable through bias alone — but
 * pt_board_rebuild verifies reachability regardless, because that guarantee
 * stops holding the day a map lets the player build on the lane.
 */
#include "pleb_tower.h"

#include <math.h>
#include <string.h>

#define PT_STEP_COST 16u
#define PT_MIN_STEP_COST 1u

static uint16_t enter_cost(const pt_board *board, uint16_t cell)
{
    uint16_t bias = board->bias[cell];
    if (bias >= PT_STEP_COST - PT_MIN_STEP_COST) return PT_MIN_STEP_COST;
    return (uint16_t)(PT_STEP_COST - bias);
}

static bool cell_is_lane(const pt_board *board, int x, int y)
{
    if (x < 0 || y < 0 || x >= PT_COLUMNS || y >= PT_ROWS) return false;
    return board->lane[pt_cell_index(x, y)] != 0u;
}

/* Dijkstra from the goal with a linear minimum scan. 450 cells makes the
 * quadratic scan cheaper than maintaining a heap, and it is obviously
 * correct — which matters more here than the constant factor. */
static bool build_distance(pt_board *board, uint16_t distance[PT_CELL_COUNT])
{
    static uint8_t settled[PT_CELL_COUNT];
    size_t remaining = 0;

    memset(settled, 0, sizeof settled);
    for (size_t i = 0; i < PT_CELL_COUNT; ++i) {
        distance[i] = PT_DISTANCE_UNREACHABLE;
        if (board->lane[i]) ++remaining;
    }
    if (board->goal_cell >= PT_CELL_COUNT || !board->lane[board->goal_cell])
        return false;
    distance[board->goal_cell] = 0u;

    while (remaining-- > 0) {
        uint16_t best = PT_DISTANCE_UNREACHABLE;
        size_t pick = PT_CELL_COUNT;
        for (size_t i = 0; i < PT_CELL_COUNT; ++i) {
            if (!board->lane[i] || settled[i]) continue;
            if (distance[i] < best) {
                best = distance[i];
                pick = i;
            }
        }
        if (pick == PT_CELL_COUNT) break;  /* rest is unreachable */
        settled[pick] = 1u;

        int cx = (int)(pick % PT_COLUMNS);
        int cy = (int)(pick / PT_COLUMNS);
        static const int dx[4] = { 1, -1, 0, 0 };
        static const int dy[4] = { 0, 0, 1, -1 };
        for (int d = 0; d < 4; ++d) {
            int nx = cx + dx[d];
            int ny = cy + dy[d];
            if (!cell_is_lane(board, nx, ny)) continue;
            uint16_t n = pt_cell_index(nx, ny);
            if (settled[n]) continue;
            /* Cost is charged for entering the neighbour when walking toward
             * the goal, which is the direction units actually travel. */
            uint32_t candidate = (uint32_t)best + enter_cost(board, n);
            if (candidate < distance[n] && candidate < PT_DISTANCE_UNREACHABLE)
                distance[n] = (uint16_t)candidate;
        }
    }
    return distance[board->spawn_cell] != PT_DISTANCE_UNREACHABLE;
}

static void build_flow(pt_board *board, const uint16_t distance[PT_CELL_COUNT])
{
    static const int dx[4] = { 1, -1, 0, 0 };
    static const int dy[4] = { 0, 0, 1, -1 };

    memset(board->flow_x, 0, sizeof board->flow_x);
    memset(board->flow_y, 0, sizeof board->flow_y);

    for (int y = 0; y < PT_ROWS; ++y) {
        for (int x = 0; x < PT_COLUMNS; ++x) {
            uint16_t cell = pt_cell_index(x, y);
            if (!board->lane[cell]) continue;
            if (distance[cell] == PT_DISTANCE_UNREACHABLE) continue;

            uint16_t best = distance[cell];
            int best_dx = 0, best_dy = 0;
            for (int d = 0; d < 4; ++d) {
                int nx = x + dx[d];
                int ny = y + dy[d];
                if (!cell_is_lane(board, nx, ny)) continue;
                uint16_t n = pt_cell_index(nx, ny);
                if (distance[n] < best) {
                    best = distance[n];
                    best_dx = dx[d];
                    best_dy = dy[d];
                }
            }
            board->flow_x[cell] = (int8_t)best_dx;
            board->flow_y[cell] = (int8_t)best_dy;
        }
    }
}

bool pt_board_rebuild(pt_board *board)
{
    static uint16_t candidate[PT_CELL_COUNT];
    if (!board) return false;
    if (!build_distance(board, candidate)) return false;
    memcpy(board->distance, candidate, sizeof candidate);
    build_flow(board, board->distance);
    ++board->rebuild_count;
    return true;
}

bool pt_board_init(pt_board *board, uint8_t campaign)
{
    const pt_campaign_def *def = pt_campaign(campaign);
    if (!board) return false;

    memset(board, 0, sizeof *board);
    for (int y = 0; y < PT_ROWS; ++y)
        for (int x = 0; x < PT_COLUMNS; ++x)
            board->lane[pt_cell_index(x, y)] = pt_lane_cells[y][x];

    for (uint8_t id = 1u; id <= PT_PAD_COUNT; ++id) {
        const pt_pad_def *pad = pt_pad(id);
        board->pad_index[pt_cell_index((int)pad->x, (int)pad->y)] = id;
    }

    board->spawn_cell = pt_cell_index((int)def->spawn_x, (int)def->spawn_y);
    board->goal_cell = pt_cell_index((int)def->goal_x, (int)def->goal_y);
    board->rebuild_count = 0u;
    return pt_board_rebuild(board);
}

void pt_board_set_bias(pt_board *board, uint16_t cell, uint16_t bias)
{
    if (!board || cell >= PT_CELL_COUNT) return;
    board->bias[cell] = bias;
}

/* Bilinear blend of the four nearest cell-centre flow vectors, so movement is
 * smooth instead of snapping between cells. Non-lane neighbours contribute
 * zero weight rather than dragging units off the road. */
bool pt_board_flow_at(const pt_board *board, float x, float y,
                      float *out_dx, float *out_dy)
{
    if (!board || !out_dx || !out_dy) return false;

    float fx = x - 0.5f;
    float fy = y - 0.5f;
    int x0 = (int)floorf(fx);
    int y0 = (int)floorf(fy);
    float tx = fx - (float)x0;
    float ty = fy - (float)y0;

    float sum_x = 0.0f, sum_y = 0.0f, sum_w = 0.0f;
    for (int j = 0; j < 2; ++j) {
        for (int i = 0; i < 2; ++i) {
            int cx = x0 + i;
            int cy = y0 + j;
            if (!cell_is_lane(board, cx, cy)) continue;
            uint16_t cell = pt_cell_index(cx, cy);
            if (board->distance[cell] == PT_DISTANCE_UNREACHABLE) continue;
            float wx = i ? tx : (1.0f - tx);
            float wy = j ? ty : (1.0f - ty);
            float w = wx * wy;
            if (w <= 0.0f) continue;
            sum_x += (float)board->flow_x[cell] * w;
            sum_y += (float)board->flow_y[cell] * w;
            sum_w += w;
        }
    }

    if (sum_w <= 0.0f) {
        /* Off-lane or fully unreachable: fall back to the containing cell so a
         * unit nudged off the road still has somewhere to go. */
        int cx = (int)floorf(x);
        int cy = (int)floorf(y);
        if (!cell_is_lane(board, cx, cy)) return false;
        uint16_t cell = pt_cell_index(cx, cy);
        sum_x = (float)board->flow_x[cell];
        sum_y = (float)board->flow_y[cell];
    } else {
        sum_x /= sum_w;
        sum_y /= sum_w;
    }

    float length = sqrtf(sum_x * sum_x + sum_y * sum_y);
    if (length <= 1e-5f) {
        *out_dx = 0.0f;
        *out_dy = 0.0f;
        return true;   /* at the goal */
    }
    *out_dx = sum_x / length;
    *out_dy = sum_y / length;
    return true;
}
