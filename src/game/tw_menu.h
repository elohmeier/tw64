/* Teeworlds 64: pure menu-flow helpers.
 *
 * Controller polling and rendering stay in tw_game.cpp. The navigation rules
 * live here so the target and native tests agree about optional pages, back
 * behavior, wrapping, and the level browser's horizontal mapping. */
#ifndef TW64_GAME_TW_MENU_H
#define TW64_GAME_TW_MENU_H

#include <stdbool.h>

enum ETw64MenuPage {
  TW64_PAGE_MODE = 0,
  TW64_PAGE_LOBBY,
  TW64_PAGE_AVATAR,
  TW64_PAGE_BOTS,
  TW64_PAGE_DIFFICULTY,
  TW64_PAGE_MAP,
  TW64_NUM_PAGES
};

/* Difficulty is relevant only when bots are present. */
int Tw64MenuNextPage(int Page, int BotCount);
int Tw64MenuPreviousPage(int Page, int BotCount);

/* Circular list movement, including negative and multi-step deltas. */
int Tw64MenuWrapSelection(int Selection, int Delta, int EntryCount);

/* Lists map up/down to rows. The visual level browser maps left/right to its
 * previous/next spatial layout, while still accepting up/down as a forgiving
 * fallback. Bot left/right input is deliberately not consumed here because it
 * changes the selected team's count instead. */
int Tw64MenuNavigationDelta(int Page, bool Up, bool Down, bool Left,
                            bool Right);

#endif
