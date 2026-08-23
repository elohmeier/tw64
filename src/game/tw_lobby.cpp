/* Teeworlds 64: pure lobby and roster model. See tw_lobby.h. */

#include <string.h>

#include "tw_lobby.h"

namespace {

bool ValidPort(int Port) { return Port >= 0 && Port < TW64_MAX_HUMANS; }

int ClampTeam(int Team) {
  return Team == TW64_TEAM_BLUE ? TW64_TEAM_BLUE : TW64_TEAM_RED;
}

int WrapAvatar(int Avatar) {
  Avatar %= TW64_NUM_AVATARS;
  return Avatar < 0 ? Avatar + TW64_NUM_AVATARS : Avatar;
}

int FirstJoinedPort(const CTw64Lobby *pLobby) {
  for (int Port = 0; Port < TW64_MAX_HUMANS; ++Port)
    if (pLobby->m_aJoined[Port])
      return Port;
  return -1;
}

} // namespace

void Tw64LobbyReset(CTw64Lobby *pLobby) {
  memset(pLobby, 0, sizeof(*pLobby));
  pLobby->m_LeaderPort = -1;
  for (int Port = 0; Port < TW64_MAX_HUMANS; ++Port) {
    pLobby->m_aHumanTeam[Port] = TW64_TEAM_FIRST;
    pLobby->m_aHumanAvatar[Port] = Port % TW64_NUM_AVATARS;
  }
}

bool Tw64LobbyJoin(CTw64Lobby *pLobby, int Port, bool Teamplay) {
  if (!ValidPort(Port) || pLobby->m_aJoined[Port])
    return false;
  if (Teamplay) {
    /* Blue is the first party-facing side. Alternate towards whichever side
     * is smaller while resolving a tie in blue's favour. */
    const int Blue = Tw64LobbyHumanTeamCount(pLobby, TW64_TEAM_BLUE);
    const int Red = Tw64LobbyHumanTeamCount(pLobby, TW64_TEAM_RED);
    pLobby->m_aHumanTeam[Port] = Blue <= Red ? TW64_TEAM_BLUE : TW64_TEAM_RED;
  } else {
    /* The upstream free-for-all controller internally treats everyone as
     * red; the value is not presented as a team in that mode. */
    pLobby->m_aHumanTeam[Port] = TW64_TEAM_RED;
  }
  pLobby->m_aJoined[Port] = true;
  pLobby->m_aAvatarReady[Port] = false;
  if (pLobby->m_LeaderPort < 0)
    pLobby->m_LeaderPort = Port;
  return true;
}

bool Tw64LobbyLeave(CTw64Lobby *pLobby, int Port) {
  if (!ValidPort(Port) || !pLobby->m_aJoined[Port])
    return false;
  pLobby->m_aJoined[Port] = false;
  pLobby->m_aHumanTeam[Port] = TW64_TEAM_FIRST;
  pLobby->m_aAvatarReady[Port] = false;
  if (pLobby->m_LeaderPort == Port)
    pLobby->m_LeaderPort = FirstJoinedPort(pLobby);
  return true;
}

bool Tw64LobbySetHumanTeam(CTw64Lobby *pLobby, int Port, int Team) {
  if (!ValidPort(Port) || !pLobby->m_aJoined[Port])
    return false;
  pLobby->m_aHumanTeam[Port] = ClampTeam(Team);
  return true;
}

bool Tw64LobbyAdjustHumanAvatar(CTw64Lobby *pLobby, int Port, int Delta) {
  if (!ValidPort(Port) || !pLobby->m_aJoined[Port] || Delta == 0)
    return false;
  pLobby->m_aHumanAvatar[Port] =
      WrapAvatar(pLobby->m_aHumanAvatar[Port] + Delta);
  pLobby->m_aAvatarReady[Port] = false;
  return true;
}

bool Tw64LobbySetAvatarReady(CTw64Lobby *pLobby, int Port, bool Ready) {
  if (!ValidPort(Port) || !pLobby->m_aJoined[Port] ||
      pLobby->m_aAvatarReady[Port] == Ready)
    return false;
  pLobby->m_aAvatarReady[Port] = Ready;
  return true;
}

bool Tw64LobbyAllHumansReady(const CTw64Lobby *pLobby) {
  bool HasHuman = false;
  for (int Port = 0; Port < TW64_MAX_HUMANS; ++Port) {
    if (!pLobby->m_aJoined[Port])
      continue;
    HasHuman = true;
    if (!pLobby->m_aAvatarReady[Port])
      return false;
  }
  return HasHuman;
}

const char *Tw64AvatarName(int Avatar) {
  static const char *const s_apNames[TW64_NUM_AVATARS] = {
      "CLASSIC", "KITTY", "BEAR", "FOX", "KOALA", "MONKEY", "PIGGY", "SPIKY"};
  return s_apNames[WrapAvatar(Avatar)];
}

int Tw64LobbyHumanCount(const CTw64Lobby *pLobby) {
  int Count = 0;
  for (int Port = 0; Port < TW64_MAX_HUMANS; ++Port)
    if (pLobby->m_aJoined[Port])
      ++Count;
  return Count;
}

int Tw64LobbyHumanTeamCount(const CTw64Lobby *pLobby, int Team) {
  Team = ClampTeam(Team);
  int Count = 0;
  for (int Port = 0; Port < TW64_MAX_HUMANS; ++Port)
    if (pLobby->m_aJoined[Port] && pLobby->m_aHumanTeam[Port] == Team)
      ++Count;
  return Count;
}

int Tw64LobbyBotCount(const CTw64Lobby *pLobby) {
  return pLobby->m_aBots[TW64_TEAM_RED] + pLobby->m_aBots[TW64_TEAM_BLUE];
}

int Tw64LobbyPlayerCount(const CTw64Lobby *pLobby) {
  return Tw64LobbyHumanCount(pLobby) + Tw64LobbyBotCount(pLobby);
}

void Tw64LobbySetQuickStartBots(CTw64Lobby *pLobby, bool Teamplay,
                                int MaxPlayers) {
  pLobby->m_aBots[TW64_TEAM_RED] = 0;
  pLobby->m_aBots[TW64_TEAM_BLUE] = 0;
  if (MaxPlayers < 0)
    MaxPlayers = 0;

  if (Teamplay) {
    pLobby->m_aBots[TW64_TEAM_RED] =
        2 - Tw64LobbyHumanTeamCount(pLobby, TW64_TEAM_RED);
    pLobby->m_aBots[TW64_TEAM_BLUE] =
        2 - Tw64LobbyHumanTeamCount(pLobby, TW64_TEAM_BLUE);
    if (pLobby->m_aBots[TW64_TEAM_RED] < 0)
      pLobby->m_aBots[TW64_TEAM_RED] = 0;
    if (pLobby->m_aBots[TW64_TEAM_BLUE] < 0)
      pLobby->m_aBots[TW64_TEAM_BLUE] = 0;
  } else {
    pLobby->m_aBots[TW64_TEAM_RED] = 4 - Tw64LobbyHumanCount(pLobby);
    if (pLobby->m_aBots[TW64_TEAM_RED] < 0)
      pLobby->m_aBots[TW64_TEAM_RED] = 0;
  }

  while (Tw64LobbyPlayerCount(pLobby) > MaxPlayers) {
    const int Team =
        pLobby->m_aBots[TW64_TEAM_BLUE] > pLobby->m_aBots[TW64_TEAM_RED]
            ? TW64_TEAM_BLUE
            : TW64_TEAM_RED;
    if (pLobby->m_aBots[Team] > 0)
      --pLobby->m_aBots[Team];
    else
      break;
  }
}

bool Tw64LobbyAdjustBots(CTw64Lobby *pLobby, int Team, int Delta,
                         int MaxPlayers) {
  Team = ClampTeam(Team);
  const int Old = pLobby->m_aBots[Team];
  int Next = Old + Delta;
  if (Next < 0)
    Next = 0;
  const int Other = Team ^ 1;
  const int Available =
      MaxPlayers - Tw64LobbyHumanCount(pLobby) - pLobby->m_aBots[Other];
  if (Next > Available)
    Next = Available;
  const int BotAvailable = TW64_MAX_INTERACTIVE_BOTS - pLobby->m_aBots[Other];
  if (MaxPlayers <= TW64_MAX_INTERACTIVE_PLAYERS && Next > BotAvailable)
    Next = BotAvailable;
  if (Next < 0)
    Next = 0;
  pLobby->m_aBots[Team] = Next;
  return Next != Old;
}

int Tw64LobbyValidate(const CTw64Lobby *pLobby, bool Teamplay, int MaxPlayers) {
  if (Tw64LobbyHumanCount(pLobby) < 1)
    return TW64_LOBBY_NEEDS_HUMAN;
  if (MaxPlayers <= TW64_MAX_INTERACTIVE_PLAYERS &&
      Tw64LobbyBotCount(pLobby) > TW64_MAX_INTERACTIVE_BOTS)
    return TW64_LOBBY_TOO_MANY_PLAYERS;
  if (Tw64LobbyPlayerCount(pLobby) > MaxPlayers)
    return TW64_LOBBY_TOO_MANY_PLAYERS;
  if (Teamplay) {
    if (Tw64LobbyHumanTeamCount(pLobby, TW64_TEAM_BLUE) +
            pLobby->m_aBots[TW64_TEAM_BLUE] <
        1)
      return TW64_LOBBY_NEEDS_BLUE;
    if (Tw64LobbyHumanTeamCount(pLobby, TW64_TEAM_RED) +
            pLobby->m_aBots[TW64_TEAM_RED] <
        1)
      return TW64_LOBBY_NEEDS_RED;
  } else if (Tw64LobbyPlayerCount(pLobby) < 2) {
    return TW64_LOBBY_NEEDS_OPPONENT;
  }
  return TW64_LOBBY_VALID;
}

int Tw64LobbyBuildRoster(const CTw64Lobby *pLobby, bool Teamplay,
                         CTw64ActorSlot *pSlots, int Capacity) {
  int Count = 0;
  for (int Port = 0; Port < TW64_MAX_HUMANS && Count < Capacity; ++Port) {
    if (!pLobby->m_aJoined[Port])
      continue;
    pSlots[Count].m_Kind = TW64_ACTOR_HUMAN;
    pSlots[Count].m_ControllerPort = Port;
    pSlots[Count].m_Team =
        Teamplay ? pLobby->m_aHumanTeam[Port] : TW64_TEAM_RED;
    pSlots[Count].m_Avatar = pLobby->m_aHumanAvatar[Port];
    ++Count;
  }
  const int aTeamOrder[TW64_NUM_TEAMS] = {TW64_TEAM_FIRST, TW64_TEAM_SECOND};
  for (int Side = 0; Side < TW64_NUM_TEAMS; ++Side) {
    const int Team = aTeamOrder[Side];
    if (!Teamplay && Team == TW64_TEAM_BLUE)
      continue;
    for (int Bot = 0; Bot < pLobby->m_aBots[Team] && Count < Capacity; ++Bot) {
      pSlots[Count].m_Kind = TW64_ACTOR_BOT;
      pSlots[Count].m_ControllerPort = -1;
      pSlots[Count].m_Team = Teamplay ? Team : TW64_TEAM_RED;
      pSlots[Count].m_Avatar = Count % TW64_NUM_AVATARS;
      ++Count;
    }
  }
  return Count;
}
