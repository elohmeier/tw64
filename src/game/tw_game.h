/* Teeworlds 64: the playable game shell.
 *
 * Owns the engine bring-up, the menu, the fixed 50 Hz match loop and the
 * marker protocol. The deterministic simulation itself is unchanged upstream
 * Teeworlds code; this layer only feeds it inputs and draws the result. */
#ifndef TW64_GAME_TW_GAME_H
#define TW64_GAME_TW_GAME_H

#ifdef __cplusplus
extern "C" {
#endif

/* Build-variant selector, defined by n64/src/game/tw_variant.c. The value is
 * baked in at link time so one object tree can produce every ROM flavour.
 * Every autoplay mode walks the same menu pages a player would; the mode only
 * chooses which entry is confirmed on each page. See g_aTw64AutoplaySpecs in
 * tw_game.cpp for the exact roster/map/difficulty of each. */
extern const int g_Tw64AutoplayMode;

enum {
  TW64_AUTOPLAY_OFF = 0,
  /* DM on dm1, one bot-driven human slot plus three bots. */
  TW64_AUTOPLAY_1P_EASY = 1,
  TW64_AUTOPLAY_1P_MEDIUM = 2,
  TW64_AUTOPLAY_1P_HARD = 3,
  /* DM on dm1, four bot-driven human slots: the 4-way split screen. */
  TW64_AUTOPLAY_4VIEW = 4,
  /* Short looping DM matches so the match teardown/restart path gets
   * exercised unattended. */
  TW64_AUTOPLAY_SOAK = 5,
  /* DM on dm6, the hazard map: death tiles under the whole lower level. */
  TW64_AUTOPLAY_1P_HARD_DM6 = 6,
  /* Two and three bot-driven human slots: the remaining split-screen
   * layouts (halves, and quadrants plus a scoreboard quadrant). */
  TW64_AUTOPLAY_2VIEW = 7,
  TW64_AUTOPLAY_3VIEW = 8,
  /* CTF 2v2 on ctf1 with the flag-capable ladder, four viewports. */
  TW64_AUTOPLAY_CTF = 9,
  /* The same CTF roster on the larger ctf5 geometry. */
  TW64_AUTOPLAY_CTF5 = 10,
  /* TDM 2v2 on dm2: team scoring and team spawns without flags. */
  TW64_AUTOPLAY_TDM = 11,
  /* The two survival modes, which end on rounds instead of a score. */
  TW64_AUTOPLAY_LMS = 12,
  TW64_AUTOPLAY_LTS = 13,
  /* CTF on ctf1 with two viewports and two bots, five minutes of game time.
   * The objective lineage's opportunistic flag gate needs contact to lapse,
   * so a 90-second match almost never grabs; this variant exists to give the
   * grab/carry/return funnel enough time to execute on target. */
  TW64_AUTOPLAY_CTF_LONG = 14,
  /* One short match per staged map, mode switching with the map, looping
   * forever: the proof that every map in the ROM filesystem loads and that
   * the map-reload path survives repetition. */
  TW64_AUTOPLAY_MAPS = 15,
  /* TDM showcase: controller slots P1/P2 share blue against four red bots. */
  TW64_AUTOPLAY_FLEX_TEAMS = 16,
  /* Guest-timed sweep over 2..16 actors. Intended for logs, not releases. */
  TW64_AUTOPLAY_BOT_BENCH = 17,
  /* TDM with one bot-driven human on each side and no bots. */
  TW64_AUTOPLAY_HUMAN_1V1 = 18,
  /* Short looping 2v4 scenario with readable menu dwell for UI evidence. */
  TW64_AUTOPLAY_MENU_REVIEW = 19,
  TW64_NUM_AUTOPLAY_MODES = 20
};

/* Runs menu -> match -> end screen forever. Never returns. */
void Tw64RunGame(void);

#ifdef __cplusplus
}
#endif

#endif
