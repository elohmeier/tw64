#include <cstdlib>
#include <iostream>
#include <string>

#include "src/game/tw_lobby.h"

namespace {

void Require(bool Condition, const char *Message) {
  if (!Condition) {
    std::cerr << "lobby_test: " << Message << '\n';
    std::exit(1);
  }
}

} // namespace

int main() {
  CTw64Lobby Lobby;
  Tw64LobbyReset(&Lobby);
  Require(Lobby.m_LeaderPort == -1, "reset leader");
  Require(Tw64LobbyJoin(&Lobby, 2, true), "first join");
  Require(Lobby.m_LeaderPort == 2, "first join owns lobby");
  Require(Tw64LobbyJoin(&Lobby, 0, true), "second join");
  Require(Lobby.m_aHumanTeam[2] == TW64_TEAM_BLUE &&
              Lobby.m_aHumanTeam[0] == TW64_TEAM_RED,
          "team joins auto-balance blue first");

  Require(!Tw64LobbyAllHumansReady(&Lobby), "avatars begin unlocked");
  Require(Tw64LobbyAdjustHumanAvatar(&Lobby, 2, -3) &&
              Lobby.m_aHumanAvatar[2] == TW64_AVATAR_SPIKY,
          "avatar selection wraps backward");
  Require(Tw64LobbySetAvatarReady(&Lobby, 2, true) &&
              Tw64LobbySetAvatarReady(&Lobby, 0, true) &&
              Tw64LobbyAllHumansReady(&Lobby),
          "all joined players lock avatars");
  Require(Tw64LobbyAdjustHumanAvatar(&Lobby, 2, 1) &&
              !Tw64LobbyAllHumansReady(&Lobby),
          "changing an avatar unlocks that player");
  Require(std::string(Tw64AvatarName(TW64_AVATAR_KITTY)) == "KITTY",
          "avatar has player-facing name");

  /* Exact 1v1: one controller on each side and no bots. */
  Require(Tw64LobbyValidate(&Lobby, true, TW64_MAX_INTERACTIVE_PLAYERS) ==
              TW64_LOBBY_VALID,
          "human versus human is valid");

  /* Requested asymmetric case: two humans together against four bots. */
  Require(Tw64LobbySetHumanTeam(&Lobby, 0, TW64_TEAM_BLUE),
          "move second human blue");
  Lobby.m_aBots[TW64_TEAM_BLUE] = 0;
  Lobby.m_aBots[TW64_TEAM_RED] = 4;
  Require(Tw64LobbyValidate(&Lobby, true, TW64_MAX_INTERACTIVE_PLAYERS) ==
              TW64_LOBBY_VALID,
          "two humans versus four bots is valid");
  CTw64ActorSlot aRoster[TW64_MAX_MATCH_PLAYERS];
  const int RosterCount =
      Tw64LobbyBuildRoster(&Lobby, true, aRoster, TW64_MAX_MATCH_PLAYERS);
  Require(RosterCount == 6, "asymmetric roster size");
  Require(aRoster[0].m_Kind == TW64_ACTOR_HUMAN &&
              aRoster[0].m_ControllerPort == 0 &&
              aRoster[0].m_Team == TW64_TEAM_BLUE &&
              aRoster[0].m_Avatar == TW64_AVATAR_CLASSIC,
          "humans are stable physical-port order");
  Require(aRoster[1].m_Kind == TW64_ACTOR_HUMAN &&
              aRoster[1].m_ControllerPort == 2,
          "second human roster entry");
  for (int i = 2; i < RosterCount; ++i)
    Require(aRoster[i].m_Kind == TW64_ACTOR_BOT &&
                aRoster[i].m_Team == TW64_TEAM_RED &&
                aRoster[i].m_Avatar >= 0 &&
                aRoster[i].m_Avatar < TW64_NUM_AVATARS,
            "red bot roster entries have bounded avatars");

  Require(Tw64LobbyAdjustBots(&Lobby, TW64_TEAM_RED, 20,
                              TW64_MAX_INTERACTIVE_PLAYERS),
          "bot adjustment clamps");
  Require(Tw64LobbyBotCount(&Lobby) == TW64_MAX_INTERACTIVE_BOTS,
          "measured interactive bot cap");
  ++Lobby.m_aBots[TW64_TEAM_RED];
  Require(Tw64LobbyValidate(&Lobby, true, TW64_MAX_INTERACTIVE_PLAYERS) ==
              TW64_LOBBY_TOO_MANY_PLAYERS,
          "roster validation enforces measured bot cap");
  --Lobby.m_aBots[TW64_TEAM_RED];

  Require(Tw64LobbyLeave(&Lobby, 2), "leader leaves");
  Require(Lobby.m_LeaderPort == 0, "leadership transfers by port order");

  Tw64LobbyReset(&Lobby);
  Require(Tw64LobbyJoin(&Lobby, 3, false), "ffa join");
  Tw64LobbySetQuickStartBots(&Lobby, false, TW64_MAX_INTERACTIVE_PLAYERS);
  Require(Lobby.m_aBots[TW64_TEAM_RED] == 3 &&
              Lobby.m_aBots[TW64_TEAM_BLUE] == 0,
          "ffa quick start keeps four actors");
  Require(Tw64LobbyValidate(&Lobby, false, TW64_MAX_INTERACTIVE_PLAYERS) ==
              TW64_LOBBY_VALID,
          "ffa quick start valid");

  Tw64LobbyReset(&Lobby);
  Require(Tw64LobbyJoin(&Lobby, 0, true), "team quick start join");
  Tw64LobbySetQuickStartBots(&Lobby, true, TW64_MAX_INTERACTIVE_PLAYERS);
  Require(Lobby.m_aBots[TW64_TEAM_BLUE] == 1 &&
              Lobby.m_aBots[TW64_TEAM_RED] == 2,
          "team quick start makes two per side");
  Lobby.m_aBots[TW64_TEAM_RED] = 0;
  Require(Tw64LobbyValidate(&Lobby, true, TW64_MAX_INTERACTIVE_PLAYERS) ==
              TW64_LOBBY_NEEDS_RED,
          "empty team rejected");

  std::cout << "lobby verification passed: team ordering, avatars, join "
               "ownership, 1v1, asymmetric rosters and caps\n";
  return 0;
}
