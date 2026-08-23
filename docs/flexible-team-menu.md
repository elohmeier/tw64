# Flexible local teams and bot benchmark

## Interaction model

The setup flow uses the console-party-lobby pattern rather than treating the
roster as one global numeric form:

1. Any controller can browse modes. The controller that confirms a mode is
   the lobby leader and owns the remaining global options.
2. On **Players**, every controller acts on its own row: **A** joins, **B**
   leaves, and left/right chooses Blue or Red in team modes. Initial joins
   alternate between the two sides. The host uses **Start** to continue and
   **B** to go back.
3. On **Choose Your Tee**, every joined controller moves its own cursor over
   the shared eight-character grid. **A** locks in, **B** unlocks, and the host
   uses **Start** after everyone is ready. Duplicate silhouettes are allowed.
4. On **Add Bots**, the host adjusts one bot count in free-for-all modes or
   independent Blue/Red counts in team modes.
5. Difficulty (when bots are present) and map remain ordinary
   leader-controlled pages. A zero-bot match skips the irrelevant difficulty
   step.

This separates controller-local intent (join, leave, choose my side) from
match-wide policy (bots, difficulty, map). It also keeps the common case fast:
the initial bot proposal preserves the previous four-actor quick start, while
every count can be reduced to zero or increased explicitly.

The lobby rejects a free-for-all without an opponent and a team match with an
empty side. Uneven teams are intentional and supported. Examples include:

- one human Blue versus one human Red, with no bots;
- two humans Blue versus five Red bots;
- one human and one bot Blue versus a human and three bots Red;
- four-human free-for-all with no bots.

## Runtime model

Controller ports, viewports, client IDs, actor kinds, avatars, and teams are
separate fields. The lobby produces an immutable roster of up to eight
interactive actors. Humans are assigned stable client IDs in physical-port
order, Blue bots are appended before Red bots, and every team and tee
silhouette is recorded explicitly. Connection order is no longer used as a
hidden team-selection mechanism.

Only humans receive viewports (up to the N64's four controller ports). Input
and Rumble Pak feedback use the roster's port-to-client mapping. The upstream
rules still support sixteen actors; that wider range is retained only by the
diagnostic benchmark ROM.

The pure lobby implementation is shared with a native test. It covers leader
selection/transfer, deterministic team balancing, avatar wrapping and ready
state, exact 1v1, asymmetric 2v4, bot and actor caps, invalid empty teams, and
stable roster order.

## Bot performance boundary

`make benchmark-bots` builds `auto-bot-bench` and runs a deterministic sweep
on the installed Gopher64. Each case uses DM1, one neutral human, one viewport,
the hard `hunter140` policy, and a 500-tick (10 game-second) measurement
window. The ROM measures simulation and render submission with the guest CPU
cycle counter. The analyzer defines high FPS as:

- at least 50 submitted frames per game second;
- no dropped catch-up windows;
- worst simulation tick no greater than the fixed 20 ms tick budget.

Corrected emulator result from 2026-08-24:

| Bots | Actors | FPS | Sim avg | Sim max | Dropped | High FPS |
| ---: | ---: | ---: | ---: | ---: | ---: | :---: |
| 1 | 2 | 60.1 | 3.076 ms | 3.971 ms | 0 | yes |
| 3 | 4 | 60.0 | 5.732 ms | 8.291 ms | 0 | yes |
| 5 | 6 | 60.0 | 9.580 ms | 13.656 ms | 0 | yes |
| 7 | 8 | 45.0 | 14.239 ms | 21.873 ms | 2 | no |
| 9 | 10 | 27.5 | 18.358 ms | 27.526 ms | 42 | no |
| 11 | 12 | 18.5 | 22.377 ms | 32.459 ms | 92 | no |
| 13 | 14 | 14.6 | 26.505 ms | 39.622 ms | 148 | no |
| 15 | 16 | 13.0 | 30.035 ms | 44.864 ms | 185 | no |

Five hard bots are therefore the interactive cap. Total interactive actors
remain capped at eight, so four humans can still play with four bots and two
humans can play against five bots.

These are guest-timed emulator results, not a physical-console claim. The
accelerated video pipeline only transports execution and is excluded from the
FPS calculation. General ModRetro M64 + SummerCart64 operation is
user-confirmed, but this performance sweep has not been recorded on that
hardware; original Nintendo 64 qualification remains unrun.

## Reproduction

```sh
make verify-fp-safety
make benchmark-bots
make ci
```

The benchmark leaves its video and full log under `build/benchmark/`; the
summary is produced by `scripts/analyze_bot_benchmark.py` and fails if five
bots no longer meet the high-FPS definition.
