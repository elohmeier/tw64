# Human-centered menu design

## Player goal and action cycle

The menu exists to get a group from “we want to play” to a match they can
predict. The player chooses rules, gathers controllers, assigns sides, chooses
a funny tee, adds opponents, chooses their skill, recognizes a level, and
starts. After every input the screen must make the changed state and next valid
action obvious.

Global choices belong to the controller that selected the game mode. Joining,
leaving, and choosing a side belong to each physical controller. This keeps
one host responsible for the match-wide plan without making guests ask the
host to join on their behalf.

## Screen review and redesign

| Screen | Player's job | Highest-risk usability issue | Design response |
| --- | --- | --- | --- |
| Boot/loading | Understand whether the game is progressing | Internal labels look like choices and failures offer no useful interpretation | Show only progress or a recovery action; reserve diagnostics for logs |
| Game mode | Choose the kind of match | Mode acronyms and a gameplay controls legend compete with the win condition | Keep recognizable game names plus the selected win condition and team model |
| Players | Join and understand who controls setup | Repeated roster explanations obscure the controller rows | Keep ownership on the host row, show `B LEAVE` only on guest rows, keep shared actions in one footer, and make disconnected rows actionable |
| Tee selection | Recognize a character and express identity | A desktop six-part skin editor would turn party setup into settings work; one host-controlled choice would erase player agency | Show eight bundled silhouettes in a shared 4×2 grid, give every controller its own cursor and lock state, allow duplicates, and keep Blue/Red tint in team matches |
| Bots | Build a valid matchup | Bare numbers do not reveal that left/right changes them or why confirmation is blocked | Put angle signifiers around counts, show the resulting roster, and explain only the repair needed before confirmation |
| Bot skill | Choose an understandable challenge | Internal policy names are presented as if players should understand them | Lead with Easy/Medium/Hard and one short behavior description; keep policy identifiers in logs |
| Level | Recognize where the group wants to play | An eight-row filename list consumes more area than the selected visual preview | Make the preview the full 320×240 background, browse spatially with left/right, and use the environment as the only level name |
| Match result | Understand who won and decide what next | “Match over” does not name the winner; the only action discards the setup | Name the winning player/team, keep the score table, offer `A` rematch and `B` change setup |

The UI deliberately omits workflow numbers, autoplay labels, internal mode and
map IDs, AI policy names, benchmark caps, and pad/rumble diagnostics. The level
preview is stored at 160×120 and drawn at an exact 2× scale. This
uses much less resident memory than sixteen full-size frames while avoiding a
fractional scale on the N64. Translucent top and bottom overlays preserve the
level as the dominant object instead of turning it back into a thumbnail.

## States and recovery

- Empty/disconnected controller rows say how to join or reconnect.
- The host is visible, and ownership transfers when that controller leaves.
- Team colors and left/right mapping remain consistent throughout setup and
  play. Internal protocol numbering does not leak into the interface.
- Each joined player locks their own tee. Changing it clears only that
  player's ready state, and duplicate choices do not create a conflict.
- Invalid free-for-all and empty-team rosters are constrained at the bot step
  with a specific repair message; entered choices remain intact.
- A zero-bot roster skips bot difficulty in both directions.
- Back navigation returns to the previous meaningful screen without resetting
  selections.
- Level navigation wraps and accepts up/down as a forgiving fallback while
  visibly teaching left/right.
- Rematch reuses the complete immutable match configuration; changing setup
  returns to the menu.

## Representative usability tasks

1. Two people create a team-mode one-human-versus-one-human match with no
   bots, verify P1 starts Blue and P2 Red, choose and lock different tees,
   browse levels by recognition, go back once, and start.
2. Two people join Blue, choose duplicate or distinct tees without conflict,
   add four Red bots, choose a difficulty and level, then start.
3. A guest leaves, the host disconnects, and the remaining joined controller
   can understand the new ownership state.
4. A player tries to start free-for-all alone or a team match with an empty
   side and can repair it from the visible message without losing work.
5. A finished match is replayed once, then the group returns to setup.

## Validation criteria

- Native tests cover forward/back transitions through tee selection, the
  zero-bot skip, circular selection, horizontal level mapping, deterministic
  team ordering, avatar wrapping, lock invalidation, and roster persistence.
- Autoplay applies each target choice while the page is visible, so capture
  evidence agrees with the match that starts.
- The playable and deterministic ROM variants pass structural verification.
- A Gopher64 capture visibly covers mode, players, tee selection, bots,
  difficulty, the full-screen level browser, match start, gameplay, result,
  and rematch where the scenario duration permits it.
- Guest frame timing remains a gameplay claim; menu capture throughput is not
  used as N64 performance evidence.
