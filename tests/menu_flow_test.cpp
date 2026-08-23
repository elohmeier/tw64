#include <cstdlib>
#include <iostream>

#include "src/game/tw_menu.h"

namespace {

void Require(bool Condition, const char *pMessage) {
  if (Condition)
    return;
  std::cerr << "menu_flow_test: " << pMessage << '\n';
  std::exit(1);
}

} // namespace

int main() {
  Require(Tw64MenuNextPage(TW64_PAGE_MODE, 0) == TW64_PAGE_LOBBY,
          "mode continues to players");
  Require(Tw64MenuNextPage(TW64_PAGE_LOBBY, 0) == TW64_PAGE_AVATAR,
          "players continues to avatars");
  Require(Tw64MenuNextPage(TW64_PAGE_AVATAR, 0) == TW64_PAGE_BOTS,
          "avatars continue to bots");
  Require(Tw64MenuNextPage(TW64_PAGE_BOTS, 0) == TW64_PAGE_MAP,
          "zero bots skips difficulty");
  Require(Tw64MenuNextPage(TW64_PAGE_BOTS, 1) == TW64_PAGE_DIFFICULTY,
          "bots include difficulty");
  Require(Tw64MenuNextPage(TW64_PAGE_DIFFICULTY, 1) == TW64_PAGE_MAP,
          "difficulty continues to level");

  Require(Tw64MenuPreviousPage(TW64_PAGE_MAP, 0) == TW64_PAGE_BOTS,
          "zero-bot level back skips difficulty");
  Require(Tw64MenuPreviousPage(TW64_PAGE_MAP, 3) == TW64_PAGE_DIFFICULTY,
          "bot level back restores difficulty");
  Require(Tw64MenuPreviousPage(TW64_PAGE_DIFFICULTY, 3) == TW64_PAGE_BOTS,
          "difficulty back restores bots");
  Require(Tw64MenuPreviousPage(TW64_PAGE_BOTS, 3) == TW64_PAGE_AVATAR,
          "bots back restores avatars");
  Require(Tw64MenuPreviousPage(TW64_PAGE_AVATAR, 3) == TW64_PAGE_LOBBY,
          "avatars back restores players");
  Require(Tw64MenuPreviousPage(TW64_PAGE_LOBBY, 3) == TW64_PAGE_MODE,
          "players back restores mode");

  Require(Tw64MenuWrapSelection(0, -1, 5) == 4, "selection wraps backward");
  Require(Tw64MenuWrapSelection(4, 1, 5) == 0, "selection wraps forward");
  Require(Tw64MenuWrapSelection(1, 7, 5) == 3,
          "selection supports multi-step delta");
  Require(Tw64MenuWrapSelection(4, -7, 5) == 2,
          "selection supports negative multi-step delta");
  Require(Tw64MenuWrapSelection(9, 1, 0) == 0,
          "empty list has stable selection");

  Require(Tw64MenuNavigationDelta(TW64_PAGE_MODE, true, false, false, false) ==
              -1,
          "lists map up to previous");
  Require(Tw64MenuNavigationDelta(TW64_PAGE_BOTS, false, false, true, false) ==
              0,
          "bot left changes value rather than row");
  Require(Tw64MenuNavigationDelta(TW64_PAGE_MAP, false, false, true, false) ==
              -1,
          "level browser maps left to previous");
  Require(Tw64MenuNavigationDelta(TW64_PAGE_MAP, false, false, false, true) ==
              1,
          "level browser maps right to next");
  Require(Tw64MenuNavigationDelta(TW64_PAGE_MAP, true, false, false, false) ==
              -1,
          "level browser accepts up as fallback");

  std::cout << "menu flow verification passed: optional difficulty, reversible "
               "back path, wrapping and spatial level navigation\n";
  return 0;
}
