# Team and avatar design

## Product decision

Blue is the first party-facing team and Red is the second. This order is used
for automatic joins, player controls, bot rows, roster summaries, the HUD,
scoreboards, and results. The upstream protocol still stores Red as zero and
Blue as one; changing those values would create compatibility risk without
improving the player experience, so presentation order is explicit instead.

The quick-match flow now includes an eight-character **Choose Your Tee** page.
It is intentionally a preset selector, not a reduced desktop settings panel.
The player's goal in this moment is to recognize a funny character, claim it,
and see that choice in the match.

## Character-select interaction

The selector follows the useful part of the kart-racing pattern:

- Classic, Kitty, Bear, Fox, Koala, Monkey, Piggy, and Spiky appear together
  in a 4×2 recognition grid.
- Every joined controller moves its own cursor with the D-pad. Left/right moves
  one cell and up/down moves one row.
- **A** locks that player's choice. **B** unlocks it; an unlocked host can use
  **B** again to return to the player screen.
- The host uses **Start** when all joined players are ready. Readiness is shown
  per player rather than inferred from inactivity.
- Duplicate silhouettes are allowed. Friends do not have to race for a
  favorite, and joining late cannot invalidate another player's choice.
- Back navigation, disconnects, and validation errors preserve selections for
  players who remain joined.

In free-for-all, each silhouette uses a recognizable signature colour. In
team modes, silhouette carries personal identity while Blue/Red tint carries
side identity. Bots receive deterministic silhouettes from the same catalog,
so they are more readable without adding configuration work.

The N64 renderer stages one 56×32 IA8 atlas per silhouette. Each atlas is one
TMEM upload and contains body, feet, and eyes. All eight compressed ROM files
together occupy under 5 KiB; the seven additional decoded atlases cost about
12.5 KiB over the previous single-atlas renderer.

## What Teeworlds can customize

The pinned desktop source has six skin-part slots: body, marking, decoration,
hands, feet, and eyes. Its bundled presets combine named part files with
independent custom colours. Teeworlds 64 currently renders the body silhouette,
feet, and eyes; markings, decorations, hands, per-part colours, custom names,
and saved user skins are not yet exposed.

A later **Tee Workshop** should remain separate from quick match:

1. Start from a preset, keeping recognition immediate.
2. Offer body/marking/decoration as large visual tabs.
3. Offer a small curated palette instead of numeric HSL controls.
4. Include **Randomize**, **Reset**, and a live team-colour preview.
5. Persist only after save-media behavior is explicitly designed and tested.

This preserves the fast party flow while leaving room for deeper expression.

## Why three-way teams are not a menu toggle

The pinned Teeworlds rules are structurally two-team:

- `datasrc/network.py` declares only `TEAM_RED`, `TEAM_BLUE`, and
  `NUM_TEAMS`, and network fields validate that exact range.
- `GameDataTeam` contains explicit Red and Blue score fields.
- `IGameController` stores two team sizes and two team scores; balancing,
  enough-player checks, win checks, shuffling, and snapshots name both sides.
- TDM indexes scoring with `team & 1`, which would collapse a third team into
  an existing score.
- Maps expose neutral, Red, and Blue spawn entities. CTF exposes exactly two
  flag stands and two flags.
- Projectile team ownership, spectator flag modes, the HUD, and results also
  encode the two-side model.

Therefore **2 humans vs 2 bots vs 2 bots is not supported by current TDM**.
Putting a third bot row in the menu would create a configuration the rules
cannot represent correctly.

## Safe path to Squad Deathmatch

Three- or four-way alliances are feasible as a deliberate new mode, tentatively
**Squad Deathmatch**, with Blue, Red, Green, and Yellow presentation order. It
should initially exclude CTF and Last Team Standing.

The implementation slice would need to:

1. Extend the generated team range and replace every two-entry controller,
   protocol, scoring, friendly-fire, projectile, and renderer assumption.
2. Give TDM a squad-aware score array and win check instead of `team & 1`.
3. Preserve bot target filtering by squad and add deterministic native tests
   for friendly fire, target choice, score attribution, and ties.
4. Use neutral map spawns for Green/Yellow until maps contain dedicated spawn
   entities; measure spawn fairness rather than silently reusing a colored
   base.
5. Generalize the lobby from two rows to a bounded list while retaining Blue
   first and Red second.
6. Exercise the representative six-actor `2h vs 2b vs 2b` replay, re-run the
   guest performance sweep, and qualify HUD/results readability at 320×240.

The smallest honest prototype is Squad Deathmatch on a neutral DM map with
three squads and no objectives. It should not ship under the existing Team
Deathmatch label until those rules and tests pass.
