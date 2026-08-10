#!/bin/sh
set -eu

binary=${1:?usage: verify_link.sh BINARY}
nm_tool=${NM:-nm}

check_one()
{
    symbol=$1
    count=$($nm_tool -g --defined-only "$binary" |
        awk -v wanted="$symbol" '$NF == wanted { count += 1 }
            END { print count + 0 }')
    if test "$count" -ne 1; then
        printf 'expected one definition of %s, found %s\n' \
            "$symbol" "$count" >&2
        exit 1
    fi
}

check_one ki_td_soft_renderer_init
check_one ki_td_view_visible_cells
check_one sr_canvas_init
check_one kilix_ui_draw_panel
check_one kilix_game_clock_init
check_one kilix_game_signals_install
check_one kittyts_start
check_one kittyin_action_map_gamepad
check_one kilixstate_store_init

printf '%s\n' \
    'PASS: one UI, top-down, raster, game runtime, input, state, and terminal stack linked'
