/* Teeworlds 64: pure lobby and roster model.
 *
 * This file deliberately has no libdragon or Teeworlds dependencies. The
 * menu uses it on target and tests compile the same implementation natively.
 * Controller ports own human slots; client IDs are assigned only when the
 * immutable match roster is built. */
#ifndef TW64_GAME_TW_LOBBY_H
#define TW64_GAME_TW_LOBBY_H

#include <stdbool.h>

enum {
  TW64_MAX_HUMANS = 4,
  TW64_MAX_MATCH_PLAYERS = 16,
  /* Eight result rows fit between the page heading and footer at 320x240.
   * The benchmark can exercise all sixteen upstream player slots, while the
   * interactive lobby stays at the measured/reviewable half of that range. */
  TW64_MAX_INTERACTIVE_PLAYERS = 8,
  /* Guest benchmark gate: five hunter140 bots sustain 60 fps with no dropped
   * windows; seven fall below 50 fps and exceed the 20 ms sim budget. */
  TW64_MAX_INTERACTIVE_BOTS = 5,
  TW64_TEAM_RED = 0,
  TW64_TEAM_BLUE = 1,
  TW64_NUM_TEAMS = 2,
  /* Teeworlds keeps red at protocol value zero. Presentation deliberately
   * starts with blue, so the menu and party vocabulary never inherit that
   * implementation order by accident. */
  TW64_TEAM_FIRST = TW64_TEAM_BLUE,
  TW64_TEAM_SECOND = TW64_TEAM_RED,
  TW64_AVATAR_CLASSIC = 0,
  TW64_AVATAR_KITTY,
  TW64_AVATAR_BEAR,
  TW64_AVATAR_FOX,
  TW64_AVATAR_KOALA,
  TW64_AVATAR_MONKEY,
  TW64_AVATAR_PIGGY,
  TW64_AVATAR_SPIKY,
  TW64_NUM_AVATARS
};

enum ETw64ActorKind { TW64_ACTOR_HUMAN = 0, TW64_ACTOR_BOT = 1 };

struct CTw64ActorSlot {
  int m_Kind;
  int m_ControllerPort; /* -1 for a bot. */
  int m_Team;           /* red for free-for-all; ignored by its controller. */
  int m_Avatar;         /* one of the bounded, target-staged tee silhouettes. */
};

struct CTw64Lobby {
  int m_LeaderPort;
  bool m_aJoined[TW64_MAX_HUMANS];
  int m_aHumanTeam[TW64_MAX_HUMANS];
  int m_aHumanAvatar[TW64_MAX_HUMANS];
  bool m_aAvatarReady[TW64_MAX_HUMANS];
  int m_aBots[TW64_NUM_TEAMS];
};

enum ETw64LobbyValidity {
  TW64_LOBBY_VALID = 0,
  TW64_LOBBY_NEEDS_HUMAN,
  TW64_LOBBY_NEEDS_OPPONENT,
  TW64_LOBBY_NEEDS_RED,
  TW64_LOBBY_NEEDS_BLUE,
  TW64_LOBBY_TOO_MANY_PLAYERS
};

void Tw64LobbyReset(CTw64Lobby *pLobby);
bool Tw64LobbyJoin(CTw64Lobby *pLobby, int Port, bool Teamplay);
bool Tw64LobbyLeave(CTw64Lobby *pLobby, int Port);
bool Tw64LobbySetHumanTeam(CTw64Lobby *pLobby, int Port, int Team);
bool Tw64LobbyAdjustHumanAvatar(CTw64Lobby *pLobby, int Port, int Delta);
bool Tw64LobbySetAvatarReady(CTw64Lobby *pLobby, int Port, bool Ready);
bool Tw64LobbyAllHumansReady(const CTw64Lobby *pLobby);
const char *Tw64AvatarName(int Avatar);

int Tw64LobbyHumanCount(const CTw64Lobby *pLobby);
int Tw64LobbyHumanTeamCount(const CTw64Lobby *pLobby, int Team);
int Tw64LobbyBotCount(const CTw64Lobby *pLobby);
int Tw64LobbyPlayerCount(const CTw64Lobby *pLobby);

/* Establishes the old quick-start shape after the humans are known: four
 * total actors in FFA, or two actors per side in a team mode. */
void Tw64LobbySetQuickStartBots(CTw64Lobby *pLobby, bool Teamplay,
                                int MaxPlayers);

/* Adjusts one side's bot count without allowing the total to exceed the
 * caller's cap. In free-for-all callers always pass red. */
bool Tw64LobbyAdjustBots(CTw64Lobby *pLobby, int Team, int Delta,
                         int MaxPlayers);

int Tw64LobbyValidate(const CTw64Lobby *pLobby, bool Teamplay, int MaxPlayers);

/* Humans are emitted first, in physical-port order, then blue and red bots.
 * Stable human client IDs keep viewport and input bookkeeping simple while
 * explicit m_Team values make connect order irrelevant. */
int Tw64LobbyBuildRoster(const CTw64Lobby *pLobby, bool Teamplay,
                         CTw64ActorSlot *pSlots, int Capacity);

#endif
