/* Teeworlds 64: pure menu-flow helpers. See tw_menu.h. */

#include "tw_menu.h"

int Tw64MenuNextPage(int Page, int BotCount) {
  switch (Page) {
  case TW64_PAGE_MODE:
    return TW64_PAGE_LOBBY;
  case TW64_PAGE_LOBBY:
    return TW64_PAGE_AVATAR;
  case TW64_PAGE_AVATAR:
    return TW64_PAGE_BOTS;
  case TW64_PAGE_BOTS:
    return BotCount > 0 ? TW64_PAGE_DIFFICULTY : TW64_PAGE_MAP;
  case TW64_PAGE_DIFFICULTY:
  default:
    return TW64_PAGE_MAP;
  }
}

int Tw64MenuPreviousPage(int Page, int BotCount) {
  switch (Page) {
  case TW64_PAGE_LOBBY:
    return TW64_PAGE_MODE;
  case TW64_PAGE_AVATAR:
    return TW64_PAGE_LOBBY;
  case TW64_PAGE_BOTS:
    return TW64_PAGE_AVATAR;
  case TW64_PAGE_DIFFICULTY:
    return TW64_PAGE_BOTS;
  case TW64_PAGE_MAP:
    return BotCount > 0 ? TW64_PAGE_DIFFICULTY : TW64_PAGE_BOTS;
  case TW64_PAGE_MODE:
  default:
    return TW64_PAGE_MODE;
  }
}

int Tw64MenuWrapSelection(int Selection, int Delta, int EntryCount) {
  if (EntryCount <= 0)
    return 0;
  Selection = (Selection + Delta) % EntryCount;
  return Selection < 0 ? Selection + EntryCount : Selection;
}

int Tw64MenuNavigationDelta(int Page, bool Up, bool Down, bool Left,
                            bool Right) {
  int Delta = 0;
  if (Page == TW64_PAGE_MAP) {
    if (Right || Down)
      ++Delta;
    if (Left || Up)
      --Delta;
  } else {
    if (Down)
      ++Delta;
    if (Up)
      --Delta;
  }
  return Delta;
}
