# Pleb Tower

An autonomous pacification force — **the Array** — has designated Maple Loop a
non-compliant sector. It does not hate anyone. It is running a procedure.

Waves of machines walk up the loop to process the block, and the residents have
garages, a hardware store, a ham radio tower, a great deal of extension cord,
and about forty minutes before the relay convoy can reach them.

Pleb Tower is a terminal-native, top-down tower defense built on the shared
Kilix game stack. It runs in Kitty-protocol terminals — Kilix, kitty, Ghostty,
WezTerm.

## Two campaigns, one map

The same board is played from both ends.

| | **HOLDOUT** | **CORDON** |
| --- | --- | --- |
| You are | The residents | A Sector Controller of the Array |
| Advancing | Array machines | Civilian groups |
| Defending | The block hub | The relay node |
| Waves | 15, ending in a boss | 12, ending in a boss |
| Currency | Salvage | Allocation |

CORDON unlocks when HOLDOUT is cleared. The direction swap genuinely re-roles
the map: the Approach is HOLDOUT's last line of defence and CORDON's first, and
the Curb Hairpin — the one pad cluster that covers two lane runs at once — is
the strongest ground on the board in either direction.

## Build and run

```sh
make
./pleb-tower
```

Needs a C11 compiler, zlib, libm, pthreads, and a Kitty-protocol terminal.

```sh
./pleb-tower --campaign 1        # start on CORDON
./pleb-tower --selftest          # headless integrity check
./pleb-tower --render-test f.ppm # one deterministic frame
```

## Controls

| Action | Keyboard | Mouse | Gamepad |
| --- | --- | --- | --- |
| Move cursor | WASD / arrows | pointer | stick / d-pad |
| Place / confirm | Enter / Space | left click | A |
| Cancel | Escape | right click | B |
| Cycle targeting | T | mode chip | X |
| Upgrade | U | Upgrade | Y |
| Repair | R | Repair | LB |
| Sell | Backspace | Sell | LB + A |
| Call wave early | Tab | countdown | RB |
| Pause | P | — | Start |
| 2x zoom | Z | wheel | RS click |

## The eight roles

Every fixture occupies exactly one slot in the taxonomy, and the content
compiler enforces that as a structural invariant.

`rapid` · `artillery` · `control` · `reroute` · `antiair` · `disable` ·
`vision` · `support`

Nine unit types answer them: armour beats unaugmented rapid fire, **hardened**
frames ignore stun and entangle outright, splitters punish pure single-target,
air punishes a ground-only board, repair units punish chip damage, and
Suppressors halt at range and shoot your fixtures.

## Verify

```sh
make test            # unit + integration suites
make sanitize        # ASan/UBSan
make test-content    # content compiler, 23 validation rules
make graphics        # cook atlases from immutable masters
make verify-graphics # manifest, dimensions, checksums
make audio           # rebuild the sound bank
make verify-audio    # format, hashes, levels, loop seams
make balance         # headless simulation of both campaigns
make release-gate    # all of the above
```

`make balance` is a second, independent implementation of the combat and
economy rules driven by the same `content/campaigns.json`. Two implementations
that agree are evidence; one checking itself is not. Best play clears HOLDOUT
at 20/20 integrity with zero leaks in 8.0 minutes, and every deliberately
flawed build order fails in the specific way the design predicts.

## Content

`content/campaigns.json` is the single source of truth for the map, both
fixture sets, both rosters and both wave scripts. `tools/compile_content.py`
validates it against 23 rules and generates a C header. Generated content is
never checked in.

## Provenance

All art was generated with Google Gemini from original prompts written for this
project; the score is an original MiniMax instrumental generated from a
project-authored prompt with no reference audio, lyrics or vocals supplied;
sound effects are procedurally synthesised or built from approved
CC0/public-domain recordings. Checksums, models and verbatim prompts are pinned
in `assets/graphics/manifest.json` and `assets/audio/provenance.json`. These are
provenance records, not legal opinions.

## Licence

See `LICENSE`.
