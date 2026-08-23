/* Teeworlds 64: the playable game shell. See tw_game.h.
 *
 * The engine bring-up is the same sequence the deterministic harness in
 * n64/src/sim/tw_match.cpp uses -- same IServer stub, same kernel
 * registration order, same map lookup -- because that sequence is the one
 * proven to reproduce host results bit-for-bit on target. What is new here is
 * everything above it: a real-time fixed-step loop, controller-driven human
 * clients, split-screen viewports, a mode/map/difficulty menu and the five
 * vanilla game modes.
 *
 * Timing model: the simulation runs at a hard 50 Hz off the CPU count
 * register. Rendering is decoupled and happens whenever a framebuffer is free
 * and the simulation is not behind. If a tick overruns its 20 ms slot the
 * renderer drops a frame; the tick rate itself is never scaled, so gameplay
 * physics stay identical to the host reference. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <debug.h>
#include <display.h>
#include <graphics.h>
#include <joypad.h>
#include <n64sys.h>
#include <surface.h>

#include <base/math.h>
#include <base/system.h>

#include <engine/config.h>
#include <engine/console.h>
#include <engine/kernel.h>
#include <engine/map.h>
#include <engine/server.h>
#include <engine/shared/config.h>
#include <engine/shared/protocol.h>
#include <engine/storage.h>

#include <game/server/bot.h>
#include <game/server/entities/character.h>
#include <game/server/gamecontext.h>
#include <game/server/gamecontroller.h>
#include <game/server/player.h>

#include "tw_audio.h"
#include "tw_game.h"
#include "tw_input.h"
#include "tw_render.h"

namespace {

enum {
  TW64_TICK_US = 1000000 / SERVER_TICK_SPEED,
  /* At most two simulation ticks are replayed in one wall-clock frame. Beyond
   * that the backlog is discarded and a render frame is dropped, so a slow
   * tick can never snowball into a runaway catch-up loop. */
  TW64_MAX_CATCHUP = 2,
  /* Even while permanently behind, render at least every third iteration so
   * the picture never freezes. */
  TW64_MAX_SKIP_STREAK = 3,
  TW64_STAT_INTERVAL = 500,
  /* Kills for DM, team kills for TDM. */
  TW64_SCORE_LIMIT = 20,
  /* CTF's own m_aTeamscore packs 100 per capture plus 1 per at-stand grab, so
   * the shell counts captures out of the per-player flag stats instead: an
   * exact number that cannot be contaminated by grab churn. */
  TW64_CTF_CAPTURE_LIMIT = 5,
  /* LMS counts rounds in the player score, LTS in the team score. */
  TW64_ROUND_LIMIT = 5,
  TW64_TIME_LIMIT_TICKS = 5 * 60 * SERVER_TICK_SPEED,
  TW64_AUTOPLAY_TIME_LIMIT_TICKS = 90 * SERVER_TICK_SPEED,
  TW64_SOAK_TIME_LIMIT_TICKS = 20 * SERVER_TICK_SPEED,
  /* The long CTF variant exists to catch a flag grab. The objective
   * lineage's opportunistic gate only opens when the flag is within 600
   * units or contact has lapsed for ten seconds, which in a crowded match is
   * rare: measured on the host, a four-actor ctf1 match grabs in 0 of 5 runs
   * by tick 4500, and a 1v1 in 3-4 of 10. Five minutes of game time is the
   * honest duration for that evidence. */
  TW64_LONG_TIME_LIMIT_TICKS = 300 * SERVER_TICK_SPEED,
  /* One short match per staged map; sixteen of them have to fit in one
   * capture. */
  TW64_MAPSOAK_TIME_LIMIT_TICKS = 15 * SERVER_TICK_SPEED,
  TW64_AUTOPLAY_MENU_FRAMES = 30,
  TW64_AUTOPLAY_END_FRAMES = 60,
  TW64_MAX_LOCAL = TW64_MAX_VIEWPORTS
};

/* ------------------------------------------------------------------ */
/* Modes and maps                                                     */
/* ------------------------------------------------------------------ */

enum {
  TW64_MODE_DM = 0,
  TW64_MODE_TDM,
  TW64_MODE_CTF,
  TW64_MODE_LMS,
  TW64_MODE_LTS,
  TW64_NUM_MODES
};

struct CTw64ModeInfo {
  const char *m_pId;    /* sv_gametype, and the mode field of the markers */
  const char *m_pLabel; /* menu entry */
  const char *m_pRule;  /* the win condition, in the menu and on the end page */
  bool m_Flags;         /* needs flag stands, so only ctf* maps qualify */
  bool m_Teams;
  bool m_Survival; /* round based: the controller ends rounds, not the match */
};

/* Rules copied from the controllers in src/game/server/gamemodes: DM ends on
 * the top player
 * score, TDM and CTF on a team score (CTF's in units of 100 per capture),
 * LMS and LTS run rounds and increment a score per round won. "mod" is the
 * upstream template controller and is deliberately not offered. */
const CTw64ModeInfo s_aModes[TW64_NUM_MODES] = {
    {"dm", "DEATHMATCH", "FIRST TO 20 KILLS", false, false, false},
    {"tdm", "TEAM DEATHMATCH", "TEAM TO 20 KILLS", false, true, false},
    {"ctf", "CAPTURE THE FLAG", "TEAM TO 5 CAPTURES", true, true, false},
    {"lms", "LAST MAN STANDING", "FIRST TO 5 ROUNDS", false, false, true},
    {"lts", "LAST TEAM STANDING", "TEAM TO 5 ROUNDS", false, true, true}};

struct CTw64MapEntry {
  const char *m_pName;
  bool m_Flags; /* has ENTITY_FLAGSTAND_RED/BLUE, i.e. is playable as CTF */
  /* Menu-only label for the tileset family the map is built from, plus a
   * warning where the map also carries death tiles. Cosmetic: it is the
   * human-readable form of the tileset scan in
   * scripts/convert_n64_assets.py (scan_map_tilesets), and nothing but the
   * map-select page reads it. */
  const char *m_pTheme;
};

/* Every map staged into the ROM filesystem by n64/Makefile. The two lists
 * must stay in sync; a missing file is reported as GAME_FAIL reason=map. */
const CTw64MapEntry s_aMaps[] = {
    {"dm1", false, "GRASS"},           {"dm2", false, "GRASS"},
    {"dm3", false, "WINTER"},          {"dm6", false, "DESERT + HAZARDS"},
    {"dm7", false, "GRASS"},           {"dm8", false, "WINTER + HAZARDS"},
    {"dm9", false, "JUNGLE"},          {"lms1", false, "GRASS"},
    {"ctf1", true, "GRASS"},           {"ctf2", true, "WINTER"},
    {"ctf3", true, "DESERT"},          {"ctf4", true, "JUNGLE"},
    {"ctf5", true, "GRASS + HAZARDS"}, {"ctf6", true, "JUNGLE + HAZARDS"},
    {"ctf7", true, "GRASS"},           {"ctf8", true, "JUNGLE"}};

const int TW64_NUM_MAPS = (int)(sizeof(s_aMaps) / sizeof(s_aMaps[0]));
static_assert(TW64_NUM_MAPS == 16,
              "map preview catalog must match the generated sprite set");

int MapCount(bool Flags) {
  int Count = 0;
  for (int i = 0; i < TW64_NUM_MAPS; ++i)
    if (s_aMaps[i].m_Flags == Flags)
      ++Count;
  return Count;
}

int MapCatalogIndex(bool Flags, int Index) {
  for (int i = 0; i < TW64_NUM_MAPS; ++i) {
    if (s_aMaps[i].m_Flags != Flags)
      continue;
    if (Index-- == 0)
      return i;
  }
  for (int i = 0; i < TW64_NUM_MAPS; ++i)
    if (s_aMaps[i].m_Flags == Flags)
      return i;
  return 0;
}

const char *MapAt(bool Flags, int Index) {
  return s_aMaps[MapCatalogIndex(Flags, Index)].m_pName;
}

const char *MapThemeAt(bool Flags, int Index) {
  return s_aMaps[MapCatalogIndex(Flags, Index)].m_pTheme;
}

int MapIndex(bool Flags, const char *pName) {
  int Index = 0;
  for (int i = 0; i < TW64_NUM_MAPS; ++i) {
    if (s_aMaps[i].m_Flags != Flags)
      continue;
    if (!str_comp(s_aMaps[i].m_pName, pName))
      return Index;
    ++Index;
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* Autoplay roster                                                    */
/* ------------------------------------------------------------------ */

struct CTw64AutoplaySpec {
  int m_Mode;
  int m_NumHumans; /* bot-driven human slots, i.e. viewports */
  int m_Difficulty;
  const char *m_pMap;
  bool m_Loop; /* soak: return to the menu and play again */
  /* Map-rotation soak: ignore m_Mode/m_pMap and walk the whole staged map
   * list, one map per match, switching mode with the map so ctf* maps are
   * entered as CTF. This is what proves every map in the ROM filesystem
   * actually loads, and it re-exercises the map-reload path each time. */
  bool m_RotateMaps;
  int m_TimeLimitTicks;
};

/* Indexed by g_Tw64AutoplayMode; entry 0 is the interactive ROM and unused. */
const CTw64AutoplaySpec g_aTw64AutoplaySpecs[TW64_NUM_AUTOPLAY_MODES] = {
    {TW64_MODE_DM, 1, 1, "dm1", false, false, TW64_TIME_LIMIT_TICKS},
    {TW64_MODE_DM, 1, 0, "dm1", false, false, TW64_AUTOPLAY_TIME_LIMIT_TICKS},
    {TW64_MODE_DM, 1, 1, "dm1", false, false, TW64_AUTOPLAY_TIME_LIMIT_TICKS},
    {TW64_MODE_DM, 1, 2, "dm1", false, false, TW64_AUTOPLAY_TIME_LIMIT_TICKS},
    {TW64_MODE_DM, 4, 1, "dm1", false, false, TW64_AUTOPLAY_TIME_LIMIT_TICKS},
    {TW64_MODE_DM, 1, 1, "dm1", true, false, TW64_SOAK_TIME_LIMIT_TICKS},
    {TW64_MODE_DM, 1, 2, "dm6", false, false, TW64_AUTOPLAY_TIME_LIMIT_TICKS},
    {TW64_MODE_DM, 2, 1, "dm1", false, false, TW64_AUTOPLAY_TIME_LIMIT_TICKS},
    {TW64_MODE_DM, 3, 1, "dm1", false, false, TW64_AUTOPLAY_TIME_LIMIT_TICKS},
    {TW64_MODE_CTF, 4, 2, "ctf1", false, false, TW64_AUTOPLAY_TIME_LIMIT_TICKS},
    {TW64_MODE_CTF, 4, 2, "ctf5", false, false, TW64_AUTOPLAY_TIME_LIMIT_TICKS},
    {TW64_MODE_TDM, 4, 2, "dm2", false, false, TW64_AUTOPLAY_TIME_LIMIT_TICKS},
    {TW64_MODE_LMS, 1, 2, "lms1", false, false, TW64_AUTOPLAY_TIME_LIMIT_TICKS},
    {TW64_MODE_LTS, 4, 2, "dm7", false, false, TW64_AUTOPLAY_TIME_LIMIT_TICKS},
    {TW64_MODE_CTF, 2, 2, "ctf1", false, false, TW64_LONG_TIME_LIMIT_TICKS},
    {TW64_MODE_DM, 1, 0, "dm1", true, true, TW64_MAPSOAK_TIME_LIMIT_TICKS}};

const CTw64AutoplaySpec *AutoplaySpec() {
  if (g_Tw64AutoplayMode <= 0 || g_Tw64AutoplayMode >= TW64_NUM_AUTOPLAY_MODES)
    return 0;
  return &g_aTw64AutoplaySpecs[g_Tw64AutoplayMode];
}

/* ------------------------------------------------------------------ */
/* Server stub                                                        */
/* ------------------------------------------------------------------ */

/* Same shape as the benchmark stub in n64/src/sim/tw_match.cpp: the game
 * rules need an IServer, but nothing here snapshots or sends anything. */
class CTw64Server : public IServer {
  enum { MAX_RECYCLED_IDS = 4096 };

  char m_aaNames[MAX_CLIENTS][MAX_NAME_ARRAY_SIZE];
  int m_aScores[MAX_CLIENTS];
  bool m_aIngame[MAX_CLIENTS];
  int m_aFreeIDs[MAX_RECYCLED_IDS];
  int m_NumFreeIDs;
  int m_NextID;

public:
  CTw64Server() {
    m_CurrentGameTick = 0;
    m_TickSpeed = SERVER_TICK_SPEED;
    m_NumFreeIDs = 0;
    m_NextID = 0;
    mem_zero(m_aaNames, sizeof(m_aaNames));
    mem_zero(m_aScores, sizeof(m_aScores));
    mem_zero(m_aIngame, sizeof(m_aIngame));
  }

  void AdvanceTick() { ++m_CurrentGameTick; }
  void SetIngame(int ClientID, bool Ingame) { m_aIngame[ClientID] = Ingame; }

  virtual const char *ClientName(int ClientID) const {
    return m_aaNames[ClientID];
  }
  virtual const char *ClientClan(int ClientID) const { return "local"; }
  virtual int ClientCountry(int ClientID) const { return -1; }
  virtual bool ClientIngame(int ClientID) const {
    return ClientID >= 0 && ClientID < MAX_CLIENTS && m_aIngame[ClientID];
  }
  virtual int GetClientInfo(int ClientID, CClientInfo *pInfo) const {
    if (!ClientIngame(ClientID))
      return 0;
    pInfo->m_pName = ClientName(ClientID);
    pInfo->m_Latency = 0;
    return 1;
  }
  virtual void GetClientAddr(int ClientID, char *pAddrStr, int Size) const {
    str_format(pAddrStr, Size, "local-%d", ClientID);
  }
  virtual int GetClientVersion(int ClientID) const { return 0x0704; }
  virtual int SendMsg(CMsgPacker *pMsg, int Flags, int ClientID) { return 0; }
  virtual void SetClientName(int ClientID, const char *pName) {
    str_copy(m_aaNames[ClientID], pName, sizeof(m_aaNames[ClientID]));
  }
  virtual void SetClientClan(int ClientID, const char *pClan) {}
  virtual void SetClientCountry(int ClientID, int Country) {}
  virtual void SetClientScore(int ClientID, int Score) {
    m_aScores[ClientID] = Score;
  }
  virtual int SnapNewID() {
    if (m_NumFreeIDs)
      return m_aFreeIDs[--m_NumFreeIDs];
    return m_NextID++;
  }
  virtual void SnapFreeID(int ID) {
    if (m_NumFreeIDs < MAX_RECYCLED_IDS)
      m_aFreeIDs[m_NumFreeIDs++] = ID;
  }
  virtual void *SnapNewItem(int Type, int ID, int Size) { return 0; }
  virtual void SnapSetStaticsize(int ItemType, int Size) {}
  virtual void SetRconCID(int ClientID) {}
  virtual bool IsAuthed(int ClientID) const { return false; }
  virtual bool IsBanned(int ClientID) { return false; }
  virtual void Kick(int ClientID, const char *pReason) {
    m_aIngame[ClientID] = false;
  }
  virtual void ChangeMap(const char *pMap) {}
  virtual void DemoRecorder_HandleAutoStart() {}
  virtual bool DemoRecorder_IsRecording() { return false; }
};

/* ------------------------------------------------------------------ */
/* Engine and match state (all fixed size)                            */
/* ------------------------------------------------------------------ */

IKernel *g_pKernel;
CTw64Server *g_pServer;
IEngineMap *g_pEngineMap;
CGameContext *g_pGameServer;
IConsole *g_pConsole;
IStorage *g_pStorage;
IConfigManager *g_pConfigManager;
bool g_WorldInitialized;
int g_MatchCount;
int g_HeapBase;
int g_HeapPeak;
char g_aLoadedMap[32];

struct CMatchConfig {
  int m_Mode;
  int m_NumHumans;
  int m_NumPlayers;
  int m_Difficulty;
  const char *m_pMap;
  const char *m_pBotPolicy;    /* opponents */
  const char *m_pDriverPolicy; /* autoplay stand-in for a human, or 0 */
  char m_aLabel[24];           /* "ctf-4p-hard" */
  int m_TimeLimitTicks;
};

CMatchConfig g_Match;
CTw64Viewport g_aViewports[TW64_MAX_LOCAL];
int g_NumViewports;
CTw64HumanInput g_aHumanInput[TW64_MAX_LOCAL];
IBot *g_apAutoDrivers[TW64_MAX_LOCAL];
char g_aaPlayerNames[TW64_MAX_LOCAL][MAX_NAME_ARRAY_SIZE];
const char *g_apPlayerNames[TW64_MAX_LOCAL];

int g_MatchTick;
bool g_MatchOver;

uint64_t g_SimWindowCycles;
uint32_t g_SimWindowMaxCycles;
uint32_t g_SimWindowTicks;
uint64_t g_RenderWindowCycles;
uint32_t g_RenderWindowMaxCycles;
uint32_t g_RenderWindowFrames;
uint32_t g_WindowDropped;
uint32_t g_TotalDropped;
uint32_t g_MatchMaxSimCycles;

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

uint32_t CyclesToUs(uint64_t Cycles) {
  /* TICKS_PER_SECOND is 46.875 MHz => microseconds = cycles * 8 / 375. */
  return (uint32_t)((Cycles * 8ULL) / 375ULL);
}

int HeapUsed() {
  heap_stats_t Stats;
  sys_get_heap_stats(&Stats);
  return Stats.used;
}

void TrackHeap() {
  const int Used = HeapUsed();
  if (Used > g_HeapPeak)
    g_HeapPeak = Used;
}

int ConnectedPads() {
  int Count = 0;
  for (int Port = 0; Port < JOYPAD_PORT_COUNT; ++Port)
    if (joypad_is_connected((joypad_port_t)Port))
      ++Count;
  return Count;
}

/* Menu navigation accepts any controller so a second player can drive it. */
joypad_buttons_t AnyPadPressed() {
  joypad_buttons_t Result;
  mem_zero(&Result, sizeof(Result));
  for (int Port = 0; Port < JOYPAD_PORT_COUNT; ++Port) {
    if (!joypad_is_connected((joypad_port_t)Port))
      continue;
    const joypad_buttons_t Pressed =
        joypad_get_buttons_pressed((joypad_port_t)Port);
    Result.raw |= Pressed.raw;
  }
  return Result;
}

bool AnyPadStartHeld() {
  for (int Port = 0; Port < JOYPAD_PORT_COUNT; ++Port) {
    if (!joypad_is_connected((joypad_port_t)Port))
      continue;
    if (joypad_get_buttons((joypad_port_t)Port).start)
      return true;
  }
  return false;
}

/* Two measured-distinct ladders.
 *
 * DM/TDM/LMS/LTS use the deathmatch lineage: hunter13 and hunter93 play
 * byte-identically in this mix and plain "hunter" scores zero kills in 90
 * seconds, so the shipped ladder is hunter13 (fights, weak) -> hunter119 (4p
 * cadence and recovery machinery) -> hunter140 (the DM champion).
 *
 * CTF uses the objective-routing lineage instead, because a flag-blind bot
 * never touches a flag: hunter142 (first flag-capable policy) -> hunter145
 * (committed flag runs on the body-clearance graph) -> hunter147 (adds
 * carrier hook-flight, stand defense and the escort role). hunter148 and
 * later add a per-five-tick physics search whose cost has not been measured
 * on target, so they are deliberately not in the ladder. */
const char *DifficultyPolicy(int Mode, int Difficulty) {
  if (Mode == TW64_MODE_CTF) {
    switch (Difficulty) {
    case 0:
      return "hunter142";
    case 1:
      return "hunter145";
    default:
      return "hunter147";
    }
  }
  switch (Difficulty) {
  case 0:
    return "hunter13";
  case 1:
    return "hunter119";
  default:
    return "hunter140";
  }
}

/* What the selected player count does to the screen. The pages with only three
 * or four entries would otherwise leave half the body empty, and this is the
 * one thing about a local-multiplayer console game a player actually wants to
 * know before confirming. */
const char *ViewportNote(int Humans) {
  switch (Humans) {
  case 1:
    return "ONE FULL-SCREEN VIEW";
  case 2:
    return "TWO HORIZONTAL HALF-SCREEN VIEWS";
  case 3:
    return "THREE VIEWS PLUS A LIVE SCOREBOARD";
  default:
    return "FOUR QUARTER-SCREEN VIEWS";
  }
}

/* The ladder, in words. The policy name is already in the row's note column;
 * this says what the player is about to be up against. */
const char *DifficultyNote(int Difficulty) {
  switch (Difficulty) {
  case 0:
    return "FIGHTS BACK, BUT GIVES GROUND";
  case 1:
    return "TRAINED MID-LADDER OPPONENTS";
  default:
    return "THE PROMOTED CHAMPION POLICY";
  }
}

/* ASCII upper case, for the breadcrumb and the map list: the desktop names
 * are lower case and the page reads as one voice when they are not. */
void UpperCopy(char *pDst, int Size, const char *pSrc) {
  str_copy(pDst, pSrc, Size);
  for (int i = 0; pDst[i]; ++i)
    if (pDst[i] >= 'a' && pDst[i] <= 'z')
      pDst[i] = (char)(pDst[i] - 'a' + 'A');
}

const char *DifficultyName(int Difficulty) {
  switch (Difficulty) {
  case 0:
    return "easy";
  case 1:
    return "medium";
  default:
    return "hard";
  }
}

int TeamScore(int Team) {
  return g_pGameServer->m_pController
             ? g_pGameServer->m_pController->GetTeamScore(Team)
             : 0;
}

int BestTeamScore() {
  const int Red = TeamScore(TEAM_RED);
  const int Blue = TeamScore(TEAM_BLUE);
  return Red > Blue ? Red : Blue;
}

/* Exact capture count per team, summed from the per-player flag stats that
 * CGameControllerCTF maintains. */
int TeamCaptures(int Team) {
  int Total = 0;
  for (int i = 0; i < g_Match.m_NumPlayers; ++i) {
    CPlayer *pPlayer = g_pGameServer->m_apPlayers[i];
    if (pPlayer && pPlayer->GetTeam() == Team)
      Total += g_pGameServer->m_aPlayerStats[i].m_FlagCaptures;
  }
  return Total;
}

/* What the HUD, the end screen and the win condition all agree to call the
 * team score: captures in CTF, the controller's own team score elsewhere. */
int DisplayTeamScore(int Team) {
  return g_Match.m_Mode == TW64_MODE_CTF ? TeamCaptures(Team)
                                         : TeamScore(Team);
}

int TopPlayerScore() {
  int Top = 0;
  for (int i = 0; i < g_Match.m_NumPlayers; ++i) {
    CPlayer *pPlayer = g_pGameServer->m_apPlayers[i];
    if (pPlayer && pPlayer->m_Score > Top)
      Top = pPlayer->m_Score;
  }
  return Top;
}

/* The shell owns the win conditions (sv_scorelimit/sv_timelimit stay 0) so it
 * can stop the world cleanly instead of letting the controller cycle into a
 * fresh match. Each mode is asked on its own terms. */
bool ScoreLimitReached() {
  switch (g_Match.m_Mode) {
  case TW64_MODE_TDM:
    return BestTeamScore() >= TW64_SCORE_LIMIT;
  case TW64_MODE_CTF: {
    const int Red = TeamCaptures(TEAM_RED);
    const int Blue = TeamCaptures(TEAM_BLUE);
    return (Red > Blue ? Red : Blue) >= TW64_CTF_CAPTURE_LIMIT;
  }
  case TW64_MODE_LMS:
    return TopPlayerScore() >= TW64_ROUND_LIMIT;
  case TW64_MODE_LTS:
    return BestTeamScore() >= TW64_ROUND_LIMIT;
  default:
    return TopPlayerScore() >= TW64_SCORE_LIMIT;
  }
}

int ScoreLimitForHud() {
  switch (g_Match.m_Mode) {
  case TW64_MODE_CTF:
    return TW64_CTF_CAPTURE_LIMIT;
  case TW64_MODE_LMS:
  case TW64_MODE_LTS:
    return TW64_ROUND_LIMIT;
  default:
    return TW64_SCORE_LIMIT;
  }
}

/* ------------------------------------------------------------------ */
/* Engine bring-up                                                    */
/* ------------------------------------------------------------------ */

bool LoadMap(const char *pMap) {
  if (!str_comp(g_aLoadedMap, pMap) && g_pEngineMap->IsLoaded())
    return true;

  if (g_pEngineMap->IsLoaded())
    g_pEngineMap->Unload();
  g_aLoadedMap[0] = 0;

  char aMapPath[IO_MAX_PATH_LENGTH];
  str_format(aMapPath, sizeof(aMapPath), "maps/%s.map", pMap);
  if (!g_pEngineMap->Load(aMapPath, g_pStorage)) {
    str_format(aMapPath, sizeof(aMapPath), "datasrc/maps/%s.map", pMap);
    if (!g_pEngineMap->Load(aMapPath, g_pStorage)) {
      debugf("TW64 GAME_FAIL reason=map map=%s\n", pMap);
      return false;
    }
  }
  str_copy(g_aLoadedMap, pMap, sizeof(g_aLoadedMap));
  TrackHeap();
  return true;
}

bool BootEngine() {
  g_HeapBase = HeapUsed();
  g_HeapPeak = g_HeapBase;

  srand(0x54573634u);

  g_pKernel = IKernel::Create();
  g_pServer = new CTw64Server;
  g_pEngineMap = CreateEngineMap();
  g_pGameServer = new CGameContext;
  g_pConsole = CreateConsole(CFGFLAG_SERVER);
  g_pStorage = CreateTestStorage();
  g_pConfigManager = CreateConfigManager();

  bool Failed = false;
  Failed = Failed ||
           !g_pKernel->RegisterInterface(static_cast<IServer *>(g_pServer));
  Failed = Failed ||
           !g_pKernel->RegisterInterface(static_cast<IEngineMap *>(g_pEngineMap));
  Failed =
      Failed || !g_pKernel->RegisterInterface(static_cast<IMap *>(g_pEngineMap));
  Failed = Failed ||
           !g_pKernel->RegisterInterface(static_cast<IGameServer *>(g_pGameServer));
  Failed = Failed || !g_pKernel->RegisterInterface(g_pConsole);
  Failed = Failed || !g_pKernel->RegisterInterface(g_pStorage);
  Failed = Failed || !g_pKernel->RegisterInterface(g_pConfigManager);
  if (Failed) {
    debugf("TW64 GAME_FAIL reason=register\n");
    return false;
  }

  g_pConfigManager->Init(CFGFLAG_SERVER);
  g_pConsole->Init();

  CConfig *pConfig = g_pConfigManager->Values();
  str_copy(pConfig->m_SvMap, s_aMaps[0].m_pName, sizeof(pConfig->m_SvMap));
  str_copy(pConfig->m_SvGametype, "dm", sizeof(pConfig->m_SvGametype));
  pConfig->m_SvWarmup = 0;
  pConfig->m_SvCountdown = 0;
  /* The shell owns the win conditions so it can override them per mode and
   * stop the world cleanly instead of cycling into a new match. */
  pConfig->m_SvScorelimit = 0;
  pConfig->m_SvTimelimit = 0;
  pConfig->m_SvInactiveKickTime = 0;
  pConfig->m_SvTeambalanceTime = 0;
  pConfig->m_SvPlayerSlots = TW64_MAX_LOCAL;
  pConfig->m_SvMaxClients = TW64_MAX_LOCAL;
  pConfig->m_Debug = 0;

  if (!LoadMap(s_aMaps[0].m_pName))
    return false;

  g_pGameServer->OnConsoleInit();
  TrackHeap();
  return true;
}

void ReleaseAutoDrivers() {
  for (int i = 0; i < TW64_MAX_LOCAL; ++i) {
    delete g_apAutoDrivers[i];
    g_apAutoDrivers[i] = 0;
  }
}

bool StartMatch(const CMatchConfig &Config) {
  ReleaseAutoDrivers();
  Tw64AudioResetMatch(0);

  if (g_WorldInitialized) {
    /* OnShutdown() ends in CGameContext::Clear(), which destroys and
     * placement-news the context in place -- zeroing the kernel back-pointer
     * that OnInit() immediately dereferences. The dedicated server hits the
     * same hazard on every map change and answers it the same way, so follow
     * CServer's map-reload sequence exactly. The world is torn down before
     * the map is swapped because the entities it holds were built from the
     * outgoing map's collision layer. */
    g_pGameServer->OnShutdown();
    g_pKernel->ReregisterInterface(static_cast<IGameServer *>(g_pGameServer));
    g_WorldInitialized = false;
  }

  CConfig *pConfig = g_pConfigManager->Values();
  str_copy(pConfig->m_SvMap, Config.m_pMap, sizeof(pConfig->m_SvMap));
  str_copy(pConfig->m_SvGametype, s_aModes[Config.m_Mode].m_pId,
           sizeof(pConfig->m_SvGametype));
  Tw64AudioUpdate();
  if (!LoadMap(Config.m_pMap))
    return false;
  Tw64AudioUpdate();

  /* OnInit() reads sv_gametype to pick the controller and walks the freshly
   * loaded game layer for spawns, pickups and flag stands. */
  g_pGameServer->OnInit();
  g_WorldInitialized = true;
  Tw64AudioUpdate();

  /* Bind the renderer to the new map: this builds the graphic tile-layer table
   * and loads that map's tileset sheets. A missing sheet is fatal on purpose;
   * a half-textured level is harder to diagnose than a refused match. */
  if (!Tw64RenderSetMap(g_pKernel, g_pEngineMap, Config.m_pMap)) {
    debugf("TW64 GAME_FAIL reason=gfx map=%s\n", Config.m_pMap);
    return false;
  }
  Tw64AudioUpdate();
  TrackHeap();

  g_Match = Config;
  ++g_MatchCount;
  const uint64_t Seed = 0x5457363400000000ULL + (uint64_t)g_MatchCount;

  for (int i = 0; i < Config.m_NumPlayers; ++i) {
    if (i < Config.m_NumHumans)
      str_format(g_aaPlayerNames[i], sizeof(g_aaPlayerNames[i]), "P%d", i + 1);
    else
      str_format(g_aaPlayerNames[i], sizeof(g_aaPlayerNames[i]), "CPU%d",
                 i - Config.m_NumHumans + 1);
    g_apPlayerNames[i] = g_aaPlayerNames[i];
    g_pServer->SetClientName(i, g_aaPlayerNames[i]);
    g_pServer->SetIngame(i, true);
  }
  for (int i = Config.m_NumPlayers; i < TW64_MAX_LOCAL; ++i) {
    g_apPlayerNames[i] = "";
    g_pServer->SetIngame(i, false);
  }

  /* Humans first, so client IDs 0..NumHumans-1 map onto controller ports.
   * The connect order is also what assigns teams: IGameController's
   * GetStartTeam() hands each new client the smaller team, so humans end up
   * split red/blue/red/blue and the bots that fill the remaining slots keep
   * the teams balanced. */
  for (int i = 0; i < Config.m_NumHumans; ++i) {
    Tw64InputReset(&g_aHumanInput[i]);
    g_pGameServer->OnClientConnected(i, true, false);
    if (!g_pGameServer->m_apPlayers[i]) {
      debugf("TW64 GAME_FAIL reason=addhuman slot=%d\n", i);
      return false;
    }
    g_pGameServer->m_apPlayers[i]->Respawn();
    if (Config.m_pDriverPolicy) {
      g_apAutoDrivers[i] = CreateBot(Config.m_pDriverPolicy);
      if (!g_apAutoDrivers[i]) {
        debugf("TW64 GAME_FAIL reason=autodriver policy=%s\n",
               Config.m_pDriverPolicy);
        return false;
      }
      g_apAutoDrivers[i]->Reset(Seed ^ 0x9e3779b97f4a7c15ULL, i);
    }
  }

  for (int i = Config.m_NumHumans; i < Config.m_NumPlayers; ++i) {
    if (!g_pGameServer->AddBot(i, Config.m_pBotPolicy, Seed)) {
      debugf("TW64 GAME_FAIL reason=addbot slot=%d policy=%s\n", i,
             Config.m_pBotPolicy);
      return false;
    }
  }

  Tw64AudioResetMatch(g_pGameServer);

  g_MatchTick = 0;
  g_MatchOver = false;
  g_SimWindowCycles = 0;
  g_SimWindowMaxCycles = 0;
  g_SimWindowTicks = 0;
  g_RenderWindowCycles = 0;
  g_RenderWindowMaxCycles = 0;
  g_RenderWindowFrames = 0;
  g_WindowDropped = 0;
  g_TotalDropped = 0;
  g_MatchMaxSimCycles = 0;
  TrackHeap();
  return true;
}

/* ------------------------------------------------------------------ */
/* Viewports                                                          */
/* ------------------------------------------------------------------ */

void BuildViewports(int NumHumans) {
  g_NumViewports = NumHumans < 1 ? 1 : NumHumans;
  if (g_NumViewports > TW64_MAX_LOCAL)
    g_NumViewports = TW64_MAX_LOCAL;

  if (g_NumViewports == 1) {
    g_aViewports[0].m_X0 = 0;
    g_aViewports[0].m_Y0 = 0;
    g_aViewports[0].m_X1 = TW64_SCREEN_W;
    g_aViewports[0].m_Y1 = TW64_SCREEN_H;
  } else if (g_NumViewports == 2) {
    for (int i = 0; i < 2; ++i) {
      g_aViewports[i].m_X0 = 0;
      g_aViewports[i].m_X1 = TW64_SCREEN_W;
      g_aViewports[i].m_Y0 = i * (TW64_SCREEN_H / 2);
      g_aViewports[i].m_Y1 = (i + 1) * (TW64_SCREEN_H / 2);
    }
  } else {
    for (int i = 0; i < g_NumViewports; ++i) {
      const int Col = i & 1;
      const int Row = i >> 1;
      g_aViewports[i].m_X0 = Col * (TW64_SCREEN_W / 2);
      g_aViewports[i].m_X1 = (Col + 1) * (TW64_SCREEN_W / 2);
      g_aViewports[i].m_Y0 = Row * (TW64_SCREEN_H / 2);
      g_aViewports[i].m_Y1 = (Row + 1) * (TW64_SCREEN_H / 2);
    }
  }
  for (int i = 0; i < g_NumViewports; ++i)
    g_aViewports[i].m_ClientID = i;
}

/* ------------------------------------------------------------------ */
/* Simulation                                                         */
/* ------------------------------------------------------------------ */

void ApplyLocalInputs() {
  for (int i = 0; i < g_Match.m_NumHumans; ++i) {
    CPlayer *pPlayer = g_pGameServer->m_apPlayers[i];
    if (!pPlayer)
      continue;

    CNetObj_PlayerInput Input;
    if (g_apAutoDrivers[i]) {
      /* The world builder reads per-bot opt-ins out of m_apBots, so the
       * driver is published for exactly the duration of the query and then
       * withdrawn again: CGameContext::OnTick() must keep seeing an empty
       * slot, otherwise the input would bypass the human path this variant
       * exists to exercise. */
      CBotWorldState World;
      g_pGameServer->m_apBots[i] = g_apAutoDrivers[i];
      BuildBotWorldState(g_pGameServer, i, &World);
      g_pGameServer->m_apBots[i] = 0;
      g_apAutoDrivers[i]->Tick(World, &Input);
      Tw64InputAdoptCounters(&g_aHumanInput[i], &Input);
    } else {
      Tw64InputPoll(&g_aHumanInput[i], i, &Input);
    }

    pPlayer->OnPredictedInput(&Input);
    pPlayer->OnDirectInput(&Input);
  }
}

void EmitStat() {
  const uint32_t SimAvg =
      g_SimWindowTicks ? CyclesToUs(g_SimWindowCycles) / g_SimWindowTicks : 0;
  const uint32_t RenderAvg =
      g_RenderWindowFrames
          ? CyclesToUs(g_RenderWindowCycles) / g_RenderWindowFrames
          : 0;
  CTw64AudioStats Audio;
  Tw64AudioTakeWindow(&Audio);
  debugf("TW64 GAME_STAT tick=%d sim_avg_us=%lu sim_max_us=%lu "
         "render_avg_us=%lu render_max_us=%lu dropped_frames=%lu frames=%lu "
         "audio_mix_avg_us=%lu audio_mix_max_us=%lu audio_tick_avg_us=%lu "
         "audio_tick_max_us=%lu voices=%lu\n",
         g_MatchTick, (unsigned long)SimAvg,
         (unsigned long)CyclesToUs(g_SimWindowMaxCycles),
         (unsigned long)RenderAvg,
         (unsigned long)CyclesToUs(g_RenderWindowMaxCycles),
         (unsigned long)g_WindowDropped, (unsigned long)g_RenderWindowFrames,
         (unsigned long)Audio.m_MixAvgUs, (unsigned long)Audio.m_MixMaxUs,
         (unsigned long)Audio.m_TickAvgUs, (unsigned long)Audio.m_TickMaxUs,
         (unsigned long)Audio.m_Voices);
  g_SimWindowCycles = 0;
  g_SimWindowMaxCycles = 0;
  g_SimWindowTicks = 0;
  g_RenderWindowCycles = 0;
  g_RenderWindowMaxCycles = 0;
  g_RenderWindowFrames = 0;
  g_WindowDropped = 0;
  TrackHeap();
}

/* The CTF funnel, on the same cadence as GAME_STAT. A capture-the-flag match
 * can look busy and still never touch a flag, so grabs/captures/returns are
 * reported live instead of only at MATCH_END. Its own marker, so the
 * GAME_STAT line stays parseable unchanged. */
void EmitFlagStat() {
  if (!s_aModes[g_Match.m_Mode].m_Flags)
    return;
  int Grabs = 0;
  int Captures = 0;
  int Returns = 0;
  char aPer[160];
  aPer[0] = 0;
  for (int i = 0; i < g_Match.m_NumPlayers; ++i) {
    const CGameContext::CPlayerStats &Stats = g_pGameServer->m_aPlayerStats[i];
    Grabs += Stats.m_FlagGrabs;
    Captures += Stats.m_FlagCaptures;
    Returns += Stats.m_FlagReturns;
    char aEntry[48];
    str_format(aEntry, sizeof(aEntry), "%s%s:%d/%d/%d", i ? "," : "",
               g_apPlayerNames[i], Stats.m_FlagGrabs, Stats.m_FlagCaptures,
               Stats.m_FlagReturns);
    str_append(aPer, aEntry, sizeof(aPer));
  }
  debugf("TW64 GAME_FLAGS tick=%d grabs=%d captures=%d returns=%d red=%d "
         "blue=%d per_player=%s\n",
         g_MatchTick, Grabs, Captures, Returns, TeamCaptures(TEAM_RED),
         TeamCaptures(TEAM_BLUE), aPer);
}

void SimulateTick() {
  const uint32_t Begin = TICKS_READ();
  g_pServer->AdvanceTick();
  ApplyLocalInputs();
  g_pGameServer->OnTick();
  const uint32_t Elapsed = (uint32_t)TICKS_SINCE(Begin);

  /* Presentation only, and measured separately: the sound layer reads the
   * tick's events and never writes anything the simulation reads back, so it
   * is deliberately outside the sim timing window above. */
  Tw64AudioTick(g_pGameServer, g_aViewports, g_NumViewports,
                g_Match.m_NumPlayers, s_aModes[g_Match.m_Mode].m_Flags);

  g_SimWindowCycles += Elapsed;
  if (Elapsed > g_SimWindowMaxCycles)
    g_SimWindowMaxCycles = Elapsed;
  if (Elapsed > g_MatchMaxSimCycles)
    g_MatchMaxSimCycles = Elapsed;
  ++g_SimWindowTicks;
  ++g_MatchTick;

  if (g_MatchTick % TW64_STAT_INTERVAL == 0) {
    EmitStat();
    EmitFlagStat();
  }

  if (ScoreLimitReached() || g_MatchTick >= g_Match.m_TimeLimitTicks)
    g_MatchOver = true;
}

void EmitMatchEnd() {
  char aScores[160];
  aScores[0] = 0;
  for (int i = 0; i < g_Match.m_NumPlayers; ++i) {
    char aEntry[40];
    CPlayer *pPlayer = g_pGameServer->m_apPlayers[i];
    str_format(aEntry, sizeof(aEntry), "%s%s:%d", i ? "," : "",
               g_apPlayerNames[i], pPlayer ? pPlayer->m_Score : 0);
    str_append(aScores, aEntry, sizeof(aScores));
  }

  /* CTF's real funnel is grabs -> captures -> returns; the score alone hides
   * a bot that runs the flag and never scores with it. */
  char aFlags[160];
  aFlags[0] = 0;
  if (s_aModes[g_Match.m_Mode].m_Flags) {
    for (int i = 0; i < g_Match.m_NumPlayers; ++i) {
      const CGameContext::CPlayerStats &Stats = g_pGameServer->m_aPlayerStats[i];
      char aEntry[48];
      str_format(aEntry, sizeof(aEntry), "%s%s:%d/%d/%d", i ? "," : "",
                 g_apPlayerNames[i], Stats.m_FlagGrabs, Stats.m_FlagCaptures,
                 Stats.m_FlagReturns);
      str_append(aFlags, aEntry, sizeof(aFlags));
    }
  }

  TrackHeap();
  debugf("TW64 MATCH_END mode=%s map=%s scores=%s teams=%d/%d raw_teams=%d/%d "
         "flags=%s ticks=%d max_sim_us=%lu dropped_total=%lu heap_peak=%d\n",
         g_Match.m_aLabel, g_Match.m_pMap, aScores,
         DisplayTeamScore(TEAM_RED), DisplayTeamScore(TEAM_BLUE),
         TeamScore(TEAM_RED), TeamScore(TEAM_BLUE),
         aFlags[0] ? aFlags : "-", g_MatchTick,
         (unsigned long)CyclesToUs(g_MatchMaxSimCycles),
         (unsigned long)g_TotalDropped, g_HeapPeak - g_HeapBase);

  /* One line per slot, so a capture can be attributed without parsing the
   * packed fields above. */
  for (int i = 0; i < g_Match.m_NumPlayers; ++i) {
    CPlayer *pPlayer = g_pGameServer->m_apPlayers[i];
    const CGameContext::CPlayerStats &Stats = g_pGameServer->m_aPlayerStats[i];
    debugf("TW64 MATCH_PLAYER slot=%d name=%s kind=%s team=%d score=%d kills=%d "
           "deaths=%d grabs=%d captures=%d returns=%d carrier_ticks=%d\n",
           i, g_apPlayerNames[i],
           i < g_Match.m_NumHumans ? "human" : "bot",
           pPlayer ? pPlayer->GetTeam() : -1, pPlayer ? pPlayer->m_Score : 0,
           Stats.m_Kills, Stats.m_Deaths, Stats.m_FlagGrabs,
           Stats.m_FlagCaptures, Stats.m_FlagReturns, Stats.m_FlagCarrierTicks);
  }
}

/* ------------------------------------------------------------------ */
/* Frames                                                             */
/* ------------------------------------------------------------------ */

void FillRenderInfo(CTw64RenderInfo *pInfo, bool ShowScoreboard) {
  pInfo->m_pViewports = g_aViewports;
  pInfo->m_NumViewports = g_NumViewports;
  pInfo->m_NumPlayers = g_Match.m_NumPlayers;
  pInfo->m_apPlayerNames = g_apPlayerNames;
  pInfo->m_ShowScoreboard = ShowScoreboard;
  pInfo->m_ScoreQuadrant = g_NumViewports == 3;
  const int TicksLeft = g_Match.m_TimeLimitTicks - g_MatchTick;
  pInfo->m_SecondsLeft = TicksLeft > 0 ? TicksLeft / SERVER_TICK_SPEED : 0;
  pInfo->m_ScoreLimit = ScoreLimitForHud();
  pInfo->m_Teamplay = s_aModes[g_Match.m_Mode].m_Teams;
  pInfo->m_FlagMode = s_aModes[g_Match.m_Mode].m_Flags;
  pInfo->m_aTeamScore[TEAM_RED] = DisplayTeamScore(TEAM_RED);
  pInfo->m_aTeamScore[TEAM_BLUE] = DisplayTeamScore(TEAM_BLUE);
  pInfo->m_TeamScoreDivisor = 1;
}

void RenderMatchFrame(bool ShowScoreboard) {
  Tw64AudioUpdate();
  surface_t *pDisp = display_try_get();
  if (!pDisp)
    return;
  const uint32_t Begin = TICKS_READ();
  CTw64RenderInfo Info;
  FillRenderInfo(&Info, ShowScoreboard);
  Tw64RenderMatch(pDisp, g_pGameServer, &Info);
  const uint32_t Elapsed = (uint32_t)TICKS_SINCE(Begin);

  g_RenderWindowCycles += Elapsed;
  if (Elapsed > g_RenderWindowMaxCycles)
    g_RenderWindowMaxCycles = Elapsed;
  ++g_RenderWindowFrames;
}

/* ------------------------------------------------------------------ */
/* Page furniture                                                     */
/* ------------------------------------------------------------------ */

/* The menu, the loading pages and the end screen share one layout so the shell
 * looks like one product rather than four screens: the logo and the animated
 * sky backdrop on top, a heading row with a rule under it, a body of rows, and
 * a footer strip carrying the wordmark, the page's own controls and the pad
 * count. Only the body changes between pages. */
enum {
  TW64_PAGE_HEAD_Y = 70,   /* heading / breadcrumb row, cap top */
  TW64_PAGE_RULE_Y = 84,   /* the rule under the heading */
  TW64_PAGE_BODY_X = 28,   /* left edge of the body column */
  TW64_PAGE_BODY_W = 264,  /* body column width */
  TW64_PAGE_FOOT_Y = 220,  /* top edge of the footer strip */
  TW64_PAGE_FOOT_TEXT = 226
};

/* A frame counter that runs for as long as the ROM does, so the backdrop keeps
 * drifting across page changes instead of snapping back on every confirm. */
int g_PageFrame;

void DrawPageChrome(surface_t *pDisp, const char *pHeading, const char *pCrumb,
                    const char *pControls, int Pads) {
  if (pHeading)
    Tw64RenderTextF(TW64_FONT_SMALL, TW64_PAGE_BODY_X, TW64_PAGE_HEAD_Y,
                    pHeading, 130, 185, 240, TW64_ALIGN_LEFT);
  if (pCrumb)
    Tw64RenderTextF(TW64_FONT_SMALL, TW64_PAGE_BODY_X + TW64_PAGE_BODY_W,
                    TW64_PAGE_HEAD_Y, pCrumb, 145, 162, 198, TW64_ALIGN_RIGHT);
  if (pHeading || pCrumb)
    Tw64RenderShade(TW64_PAGE_BODY_X, TW64_PAGE_RULE_Y, TW64_PAGE_BODY_W, 1,
                    64, 96, 148, 255);

  Tw64RenderShade(0, TW64_PAGE_FOOT_Y, TW64_SCREEN_W,
                  TW64_SCREEN_H - TW64_PAGE_FOOT_Y, 5, 8, 16, 216);
  Tw64RenderShade(0, TW64_PAGE_FOOT_Y, TW64_SCREEN_W, 1, 52, 76, 120, 255);
  Tw64RenderTextF(TW64_FONT_SMALL, 8, TW64_PAGE_FOOT_TEXT, "TEEWORLDS 64", 255,
                  208, 80, TW64_ALIGN_LEFT);
  if (pControls)
    Tw64RenderTextF(TW64_FONT_SMALL, TW64_SCREEN_W / 2, TW64_PAGE_FOOT_TEXT,
                    pControls, 150, 168, 205, TW64_ALIGN_CENTER);
  char aBuf[32];
  if (g_Tw64AutoplayMode)
    str_copy(aBuf, "AUTOPLAY", sizeof(aBuf));
  else
    str_format(aBuf, sizeof(aBuf), "PADS %d", Pads);
  Tw64RenderTextF(TW64_FONT_SMALL, TW64_SCREEN_W - 8, TW64_PAGE_FOOT_TEXT,
                  aBuf, 130, 146, 180, TW64_ALIGN_RIGHT);
}

/* Boot, load and failure notices. Same backdrop and typography as the menu, so
 * the very first frame the player sees is already the finished look. */
void DrawSimplePage(const char *pTitle, const char *pLine1,
                    const char *pLine2) {
  Tw64AudioUpdate();
  surface_t *pDisp = display_get();
  Tw64RenderBeginMenuPage(pDisp, g_PageFrame++, true);
  char aBuf[48];
  Tw64RenderTextF(TW64_FONT_MENU, TW64_SCREEN_W / 2, 128, pTitle, 235, 240,
                  250, TW64_ALIGN_CENTER);
  if (pLine1) {
    UpperCopy(aBuf, sizeof(aBuf), pLine1);
    Tw64RenderTextF(TW64_FONT_SMALL, TW64_SCREEN_W / 2, 148, aBuf, 150, 168,
                    205, TW64_ALIGN_CENTER);
  }
  if (pLine2) {
    UpperCopy(aBuf, sizeof(aBuf), pLine2);
    Tw64RenderTextF(TW64_FONT_SMALL, TW64_SCREEN_W / 2, 162, aBuf, 150, 168,
                    205, TW64_ALIGN_CENTER);
  }
  DrawPageChrome(pDisp, 0, 0, 0, ConnectedPads());
  Tw64RenderEndPage();
}

/* ------------------------------------------------------------------ */
/* Menu                                                               */
/* ------------------------------------------------------------------ */

enum {
  TW64_PAGE_MODE = 0,
  TW64_PAGE_PLAYERS,
  TW64_PAGE_DIFFICULTY,
  TW64_PAGE_MAP,
  TW64_NUM_PAGES,
  TW64_MENU_MAX_ROWS = 8,
  /* Eight rows of fifteen pixels sit between the rule under the heading and
   * the footer strip, which is what the longest page (the eight-map list)
   * needs; the shorter pages spend the difference on the control map. */
  TW64_MENU_ROW_Y = 88,
  TW64_MENU_ROW_STEP = 15,
  TW64_MENU_LABEL_X = 42,
  TW64_MENU_NOTE_X = 286,
  /* The map page keeps the eight-row list but gives its notes column to a
   * selected-map scene. The preview is stored at this exact screen size. */
  TW64_MAP_LIST_W = 112,
  TW64_MAP_PREVIEW_X = 156,
  TW64_MAP_PREVIEW_Y = 94,
  TW64_MAP_PREVIEW_W = 128,
  TW64_MAP_PREVIEW_H = 96
};

struct CMenuRow {
  char m_aText[24];
  char m_aNote[24];
  uint8_t m_R;
  uint8_t m_G;
  uint8_t m_B;
};

CMenuRow g_aMenuRows[TW64_MENU_MAX_ROWS];
int g_NumMenuRows;

void AddMenuRow(const char *pText, const char *pNote, uint8_t R, uint8_t G,
                uint8_t B) {
  if (g_NumMenuRows >= TW64_MENU_MAX_ROWS)
    return;
  CMenuRow *pRow = &g_aMenuRows[g_NumMenuRows++];
  str_copy(pRow->m_aText, pText, sizeof(pRow->m_aText));
  str_copy(pRow->m_aNote, pNote ? pNote : "", sizeof(pRow->m_aNote));
  pRow->m_R = R;
  pRow->m_G = G;
  pRow->m_B = B;
}

/* Menu selections. Kept across pages so B returns to a page as it was left. */
int g_aMenuSelection[TW64_NUM_PAGES];

int MenuMode() { return g_aMenuSelection[TW64_PAGE_MODE]; }
bool MenuWantsFlagMaps() { return s_aModes[MenuMode()].m_Flags; }

int MenuEntryCount(int Page) {
  switch (Page) {
  case TW64_PAGE_MODE:
    return TW64_NUM_MODES;
  case TW64_PAGE_PLAYERS:
    return TW64_MAX_LOCAL;
  case TW64_PAGE_DIFFICULTY:
    return 3;
  default: {
    /* Both filtered map lists are exactly TW64_MENU_MAX_ROWS long today. The
     * clamp keeps the page honest if a future map pushes one list past what
     * a single screen of rows can show, rather than offering an entry the
     * player cannot see. */
    const int Count = MapCount(MenuWantsFlagMaps());
    return Count > TW64_MENU_MAX_ROWS ? (int)TW64_MENU_MAX_ROWS : Count;
  }
  }
}

/* The accent a selected row is drawn in. Gold is the wordmark's own colour and
 * is what the mode, player-count and map pages use; the difficulty page
 * overrides it with its own green/amber/red ladder. */
enum { TW64_ACCENT_R = 255, TW64_ACCENT_G = 214, TW64_ACCENT_B = 110 };

void BuildMenuRows(int Page) {
  g_NumMenuRows = 0;
  char aBuf[24];
  switch (Page) {
  case TW64_PAGE_MODE:
    for (int i = 0; i < TW64_NUM_MODES; ++i) {
      char aNote[20];
      UpperCopy(aNote, sizeof(aNote), s_aModes[i].m_pId);
      AddMenuRow(s_aModes[i].m_pLabel, aNote, TW64_ACCENT_R, TW64_ACCENT_G,
                 TW64_ACCENT_B);
    }
    break;
  case TW64_PAGE_PLAYERS:
    for (int i = 1; i <= TW64_MAX_LOCAL; ++i) {
      str_format(aBuf, sizeof(aBuf), "%d PLAYER%s", i, i > 1 ? "S" : "");
      char aNote[20];
      str_format(aNote, sizeof(aNote), "%d BOT%s", TW64_MAX_LOCAL - i,
                 TW64_MAX_LOCAL - i == 1 ? "" : "S");
      AddMenuRow(aBuf, aNote, TW64_ACCENT_R, TW64_ACCENT_G, TW64_ACCENT_B);
    }
    break;
  case TW64_PAGE_DIFFICULTY:
    AddMenuRow("EASY", DifficultyPolicy(MenuMode(), 0), 130, 240, 140);
    AddMenuRow("MEDIUM", DifficultyPolicy(MenuMode(), 1), 250, 220, 100);
    AddMenuRow("HARD", DifficultyPolicy(MenuMode(), 2), 255, 110, 110);
    break;
  default: {
    const bool Flags = MenuWantsFlagMaps();
    const int Count = MapCount(Flags);
    for (int i = 0; i < Count && i < TW64_MENU_MAX_ROWS; ++i) {
      /* Uppercase, so the map list reads in the same voice as the mode and
       * player-count pages. */
      UpperCopy(aBuf, sizeof(aBuf), MapAt(Flags, i));
      AddMenuRow(aBuf, MapThemeAt(Flags, i), TW64_ACCENT_R, TW64_ACCENT_G,
                 TW64_ACCENT_B);
    }
    break;
  }
  }
}

const char *PageTitle(int Page) {
  switch (Page) {
  case TW64_PAGE_MODE:
    return "SELECT MODE";
  case TW64_PAGE_PLAYERS:
    return "LOCAL PLAYERS";
  case TW64_PAGE_DIFFICULTY:
    return "DIFFICULTY";
  default:
    return "SELECT MAP";
  }
}

/* The control map, in two columns under the mode list. It is reference
 * material, not a menu entry, so it stays in the small face and a muted
 * colour: present on the first page a player sees, never competing with the
 * list itself. */
void DrawControlsBlock(int Y) {
  static const char *const s_apLeft[4] = {"STICK   AIM + MOVE",
                                          "DPAD    MOVE LEFT/RIGHT",
                                          "START   SCOREBOARD", 0};
  static const char *const s_apRight[4] = {"A   JUMP", "B   FIRE", "Z   HOOK",
                                           "R   WEAPON"};
  Tw64RenderTextF(TW64_FONT_SMALL, TW64_PAGE_BODY_X, Y, "CONTROLS", 130, 185,
                  240, TW64_ALIGN_LEFT);
  for (int i = 0; i < 4; ++i) {
    const int LineY = Y + 12 + i * 10;
    if (s_apLeft[i])
      Tw64RenderTextF(TW64_FONT_SMALL, TW64_MENU_LABEL_X, LineY, s_apLeft[i],
                      150, 168, 205, TW64_ALIGN_LEFT);
    Tw64RenderTextF(TW64_FONT_SMALL, 186, LineY, s_apRight[i], 150, 168, 205,
                    TW64_ALIGN_LEFT);
  }
}

void DrawMenuFrame(int Page, int Selection, int Pads) {
  Tw64AudioUpdate();
  BuildMenuRows(Page);
  if (Selection >= g_NumMenuRows)
    Selection = g_NumMenuRows ? g_NumMenuRows - 1 : 0;

  surface_t *pDisp = display_get();
  Tw64RenderBeginMenuPage(pDisp, g_PageFrame++, true);

  /* Breadcrumb: what the pages before this one already decided. */
  char aCrumb[64];
  char aMode[24];
  UpperCopy(aMode, sizeof(aMode), s_aModes[MenuMode()].m_pId);
  if (Page == TW64_PAGE_MODE)
    str_copy(aCrumb, "5 MODES / 16 MAPS", sizeof(aCrumb));
  else if (Page == TW64_PAGE_PLAYERS)
    str_format(aCrumb, sizeof(aCrumb), "%s / %s", aMode,
               s_aModes[MenuMode()].m_pRule);
  else if (Page == TW64_PAGE_DIFFICULTY)
    str_format(aCrumb, sizeof(aCrumb), "%s / %d PLAYER%s", aMode,
               g_aMenuSelection[TW64_PAGE_PLAYERS] + 1,
               g_aMenuSelection[TW64_PAGE_PLAYERS] ? "S" : "");
  else {
    char aDifficulty[16];
    UpperCopy(aDifficulty, sizeof(aDifficulty),
              DifficultyName(g_aMenuSelection[TW64_PAGE_DIFFICULTY]));
    str_format(aCrumb, sizeof(aCrumb), "%s / %dP / %s", aMode,
               g_aMenuSelection[TW64_PAGE_PLAYERS] + 1, aDifficulty);
  }

  const char *pControls = Page == TW64_PAGE_MODE ? "A  CONFIRM"
                          : Page == TW64_PAGE_MAP
                              ? "A  START      B  BACK"
                              : "A  CONFIRM      B  BACK";
  DrawPageChrome(pDisp, PageTitle(Page), aCrumb, pControls, Pads);

  /* The selected row gets both a highlight band and a gold marker down its
   * left edge: colour alone is easy to lose on a composite signal. */
  const int SelY = TW64_MENU_ROW_Y + Selection * TW64_MENU_ROW_STEP;
  const int SelW =
      Page == TW64_PAGE_MAP ? (int)TW64_MAP_LIST_W : (int)TW64_PAGE_BODY_W;
  Tw64RenderShade(TW64_PAGE_BODY_X, SelY - 3, SelW,
                  TW64_MENU_ROW_STEP, 34, 62, 116, 205);
  Tw64RenderShade(TW64_PAGE_BODY_X, SelY - 3, 3, TW64_MENU_ROW_STEP, 255, 208,
                  80, 255);

  for (int i = 0; i < g_NumMenuRows; ++i) {
    const int Y = TW64_MENU_ROW_Y + i * TW64_MENU_ROW_STEP;
    const CMenuRow &Row = g_aMenuRows[i];
    const bool Active = i == Selection;
    /* Every row carries its own accent colour (the difficulty page uses it
     * for the ladder); an unselected row is drawn in the neutral body colour
     * so the accent reads as "this is the choice", not as decoration. */
    if (Active)
      Tw64RenderTextF(TW64_FONT_MENU, TW64_MENU_LABEL_X, Y, Row.m_aText,
                      Row.m_R, Row.m_G, Row.m_B, TW64_ALIGN_LEFT);
    else
      Tw64RenderTextF(TW64_FONT_MENU, TW64_MENU_LABEL_X, Y, Row.m_aText, 205,
                      215, 235, TW64_ALIGN_LEFT);
    if (Page != TW64_PAGE_MAP && Row.m_aNote[0])
      Tw64RenderTextF(TW64_FONT_SMALL, TW64_MENU_NOTE_X, Y + 2, Row.m_aNote,
                      Active ? 190 : 122, Active ? 205 : 140,
                      Active ? 230 : 172, TW64_ALIGN_RIGHT);
  }

  /* Detail block: the mode page carries the control map, the two short pages
   * explain their choice, and the map page turns the old notes column into a
   * scene window. The explanatory two-line blocks remain bottom-anchored. */
  const int DetailY = 186;
  if (Page == TW64_PAGE_MODE) {
    DrawControlsBlock(TW64_MENU_ROW_Y + g_NumMenuRows * TW64_MENU_ROW_STEP + 2);
  } else if (Page == TW64_PAGE_PLAYERS) {
    Tw64RenderTextF(TW64_FONT_SMALL, TW64_PAGE_BODY_X, DetailY,
                    ViewportNote(Selection + 1), 130, 185, 240,
                    TW64_ALIGN_LEFT);
    Tw64RenderTextF(TW64_FONT_SMALL, TW64_PAGE_BODY_X, DetailY + 12,
                    "THE REMAINING SLOTS ARE FILLED WITH BOTS", 150, 168, 205,
                    TW64_ALIGN_LEFT);
  } else if (Page == TW64_PAGE_DIFFICULTY) {
    Tw64RenderTextF(TW64_FONT_SMALL, TW64_PAGE_BODY_X, DetailY,
                    DifficultyNote(Selection), 130, 185, 240, TW64_ALIGN_LEFT);
    char aPolicy[48];
    str_format(aPolicy, sizeof(aPolicy), "BOT POLICY  %s",
               DifficultyPolicy(MenuMode(), Selection));
    Tw64RenderTextF(TW64_FONT_SMALL, TW64_PAGE_BODY_X, DetailY + 12, aPolicy,
                    150, 168, 205, TW64_ALIGN_LEFT);
  } else if (Page == TW64_PAGE_MAP) {
    /* One neutral pixel contains the colourful crop without competing with
     * the gold selection marker or repeating the removed gameplay frames. */
    Tw64RenderShade(TW64_MAP_PREVIEW_X - 1, TW64_MAP_PREVIEW_Y - 1,
                    TW64_MAP_PREVIEW_W + 2, TW64_MAP_PREVIEW_H + 2, 18, 22, 30,
                    255);
    Tw64RenderMapPreview(
        MapCatalogIndex(MenuWantsFlagMaps(), Selection), TW64_MAP_PREVIEW_X,
        TW64_MAP_PREVIEW_Y);
    Tw64RenderTextF(TW64_FONT_SMALL,
                    TW64_MAP_PREVIEW_X + TW64_MAP_PREVIEW_W / 2,
                    TW64_MAP_PREVIEW_Y + TW64_MAP_PREVIEW_H + 8,
                    g_aMenuRows[Selection].m_aNote, 150, 168, 205,
                    TW64_ALIGN_CENTER);
  }
  Tw64RenderEndPage();
}

/* The entry an autoplay ROM confirms on `Page`; the unattended variants walk
 * the same four pages a player would rather than short-circuiting them. */
int AutoplaySelection(int Page, const CTw64AutoplaySpec *pSpec) {
  /* The rotation soak derives mode and map from the match counter instead of
   * the spec, so consecutive matches walk the whole staged map list. */
  const int Rotation =
      pSpec->m_RotateMaps ? g_MatchCount % TW64_NUM_MAPS : -1;
  const bool RotateFlags = Rotation >= 0 && s_aMaps[Rotation].m_Flags;
  switch (Page) {
  case TW64_PAGE_MODE:
    if (Rotation >= 0)
      return RotateFlags ? TW64_MODE_CTF : TW64_MODE_DM;
    return pSpec->m_Mode;
  case TW64_PAGE_PLAYERS:
    return pSpec->m_NumHumans - 1;
  case TW64_PAGE_DIFFICULTY:
    return pSpec->m_Difficulty;
  default:
    if (Rotation >= 0)
      return MapIndex(RotateFlags, s_aMaps[Rotation].m_pName);
    return MapIndex(s_aModes[pSpec->m_Mode].m_Flags, pSpec->m_pMap);
  }
}

/* Returns the chosen match configuration. */
void RunMenu(CMatchConfig *pConfig) {
  const CTw64AutoplaySpec *pSpec = AutoplaySpec();
  int Page = TW64_PAGE_MODE;
  /* Per-page, not per-menu: an autoplay ROM dwells on every page for the same
   * time a player would need to read it, so the unattended captures show (and
   * blip through) each page instead of confirming the last three on
   * consecutive frames. */
  int PageFrames = 0;
  bool Announced = false;

  /* Start with the full-screen single-player setup: one local player and
   * three bots. Connected controllers remain available through the player
   * count page, but do not turn the default match into split screen. */
  g_aMenuSelection[TW64_PAGE_MODE] = 0;
  g_aMenuSelection[TW64_PAGE_PLAYERS] = 0;
  g_aMenuSelection[TW64_PAGE_DIFFICULTY] = 1;
  g_aMenuSelection[TW64_PAGE_MAP] = 0;

  while (true) {
    joypad_poll();
    const int Pads = ConnectedPads();
    const joypad_buttons_t Pressed = AnyPadPressed();

    bool Confirm = Pressed.a != 0;
    bool Back = Pressed.b != 0;
    int Move = 0;
    if (Pressed.d_down)
      Move += 1;
    if (Pressed.d_up)
      Move -= 1;

    if (pSpec && PageFrames >= TW64_AUTOPLAY_MENU_FRAMES) {
      g_aMenuSelection[Page] = AutoplaySelection(Page, pSpec);
      Confirm = true;
      Move = 0;
      Back = false;
    }

    const int NumEntries = MenuEntryCount(Page);
    int Selection = g_aMenuSelection[Page];
    if (Selection >= NumEntries)
      Selection = NumEntries - 1;
    if (Selection < 0)
      Selection = 0;
    if (Move) {
      Tw64AudioMenu(TW64_MENU_SOUND_MOVE);
      Selection += Move;
      if (Selection < 0)
        Selection = NumEntries - 1;
      if (Selection >= NumEntries)
        Selection = 0;
    }
    g_aMenuSelection[Page] = Selection;

    if (Back && Page > TW64_PAGE_MODE) {
      Tw64AudioMenu(TW64_MENU_SOUND_MOVE);
      --Page;
      PageFrames = 0;
      Confirm = false;
    } else if (Confirm) {
      Tw64AudioMenu(TW64_MENU_SOUND_CONFIRM);
      if (Page == TW64_PAGE_MODE) {
        /* The map list is mode-filtered, so a mode change invalidates it. */
        g_aMenuSelection[TW64_PAGE_MAP] = 0;
        ++Page;
        PageFrames = 0;
      } else if (Page < TW64_PAGE_MAP) {
        ++Page;
        PageFrames = 0;
      } else {
        const int Mode = MenuMode();
        const int Humans = g_aMenuSelection[TW64_PAGE_PLAYERS] + 1;
        const int Difficulty = g_aMenuSelection[TW64_PAGE_DIFFICULTY];
        pConfig->m_Mode = Mode;
        pConfig->m_NumHumans = Humans;
        pConfig->m_NumPlayers = TW64_MAX_LOCAL;
        pConfig->m_Difficulty = Difficulty;
        pConfig->m_pMap = MapAt(s_aModes[Mode].m_Flags, Selection);
        pConfig->m_pBotPolicy = DifficultyPolicy(Mode, Difficulty);
        pConfig->m_pDriverPolicy =
            pSpec ? DifficultyPolicy(Mode, Difficulty) : 0;
        str_format(pConfig->m_aLabel, sizeof(pConfig->m_aLabel), "%s-%dp-%s",
                   s_aModes[Mode].m_pId, Humans, DifficultyName(Difficulty));
        pConfig->m_TimeLimitTicks =
            pSpec ? pSpec->m_TimeLimitTicks : TW64_TIME_LIMIT_TICKS;
        return;
      }
    }

    DrawMenuFrame(Page, g_aMenuSelection[Page], Pads);
    if (!Announced) {
      debugf("TW64 MENU_OK pads=%d autoplay=%d modes=%d maps=%d\n", Pads,
             g_Tw64AutoplayMode, TW64_NUM_MODES, TW64_NUM_MAPS);
      Announced = true;
    }
    ++PageFrames;
  }
}

/* ------------------------------------------------------------------ */
/* End screen                                                         */
/* ------------------------------------------------------------------ */

void EndScreenPlayerColor(int ClientID, uint8_t *pR, uint8_t *pG,
                          uint8_t *pB) {
  CPlayer *pPlayer = g_pGameServer->m_apPlayers[ClientID];
  const int Team = pPlayer ? pPlayer->GetTeam() : -1;
  if (s_aModes[g_Match.m_Mode].m_Teams && (Team == TEAM_RED || Team == TEAM_BLUE))
    Tw64TeamColor(Team, ClientID >= 2 ? 1 : 0, pR, pG, pB);
  else
    Tw64PlayerColor(ClientID, pR, pG, pB);
}

void RunEndScreen() {
  const bool Teams = s_aModes[g_Match.m_Mode].m_Teams;
  int Frames = 0;
  while (true) {
    joypad_poll();
    const joypad_buttons_t Pressed = AnyPadPressed();
    if (!g_Tw64AutoplayMode && Pressed.a)
      return;
    const CTw64AutoplaySpec *pSpec = AutoplaySpec();
    if (pSpec && pSpec->m_Loop && Frames >= TW64_AUTOPLAY_END_FRAMES)
      return;
    ++Frames;

    Tw64AudioUpdate();
    surface_t *pDisp = display_get();
    /* Same backdrop and the same chrome as the menu, without the logo: the
     * headline of this page is the result, not the wordmark. */
    Tw64RenderBeginMenuPage(pDisp, g_PageFrame++, false);

    char aBuf[48];
    char aMode[24];
    char aMap[24];
    UpperCopy(aMode, sizeof(aMode), g_Match.m_aLabel);
    UpperCopy(aMap, sizeof(aMap), g_Match.m_pMap);
    str_format(aBuf, sizeof(aBuf), "%s / %s", aMode, aMap);
    DrawPageChrome(pDisp, "FINAL SCORE", aBuf,
                   g_Tw64AutoplayMode ? "AUTOPLAY COMPLETE" : "A  BACK TO MENU",
                   ConnectedPads());
    Tw64RenderTextF(TW64_FONT_MENU, TW64_SCREEN_W / 2, 30, "MATCH OVER", 255,
                    214, 110, TW64_ALIGN_CENTER);
    UpperCopy(aBuf, sizeof(aBuf), s_aModes[g_Match.m_Mode].m_pRule);
    Tw64RenderTextF(TW64_FONT_SMALL, TW64_SCREEN_W / 2, 48, aBuf, 150, 168,
                    205, TW64_ALIGN_CENTER);

    int Row = 0;
    if (Teams) {
      /* Both team scores on one line, each in its own team colour. */
      uint8_t R, G, B;
      str_format(aBuf, sizeof(aBuf), "RED %d", DisplayTeamScore(TEAM_RED));
      Tw64TeamColor(TEAM_RED, 0, &R, &G, &B);
      Tw64RenderTextF(TW64_FONT_MENU, TW64_SCREEN_W / 2 - 16,
                      TW64_MENU_ROW_Y, aBuf, R, G, B, TW64_ALIGN_RIGHT);
      str_format(aBuf, sizeof(aBuf), "BLUE %d", DisplayTeamScore(TEAM_BLUE));
      Tw64TeamColor(TEAM_BLUE, 0, &R, &G, &B);
      Tw64RenderTextF(TW64_FONT_MENU, TW64_SCREEN_W / 2 + 16,
                      TW64_MENU_ROW_Y, aBuf, R, G, B, TW64_ALIGN_LEFT);
      Row = 1;
    }

    bool aUsed[TW64_MAX_LOCAL] = {false, false, false, false};
    for (int Rank = 0; Rank < g_Match.m_NumPlayers; ++Rank) {
      int Best = -1;
      for (int i = 0; i < g_Match.m_NumPlayers; ++i) {
        if (aUsed[i] || !g_pGameServer->m_apPlayers[i])
          continue;
        if (Best < 0 || g_pGameServer->m_apPlayers[i]->m_Score >
                            g_pGameServer->m_apPlayers[Best]->m_Score)
          Best = i;
      }
      if (Best < 0)
        break;
      aUsed[Best] = true;
      uint8_t R, G, B;
      EndScreenPlayerColor(Best, &R, &G, &B);
      const int Y = TW64_MENU_ROW_Y + Row * TW64_MENU_ROW_STEP;
      /* The UI face is proportional, so the columns are laid out by anchor
       * instead of by padding a monospaced string: rank and name from the
       * left, score against a fixed right edge, flag stats after it. */
      if (Rank == 0) {
        /* The winner keeps the menu's selected-row treatment, so the page the
         * match ends on is read the same way as the page it started from. */
        Tw64RenderShade(TW64_PAGE_BODY_X, Y - 3, TW64_PAGE_BODY_W,
                        TW64_MENU_ROW_STEP, 34, 62, 116, 205);
        Tw64RenderShade(TW64_PAGE_BODY_X, Y - 3, 3, TW64_MENU_ROW_STEP, 255,
                        208, 80, 255);
      }
      str_format(aBuf, sizeof(aBuf), "%d.", Rank + 1);
      Tw64RenderTextF(TW64_FONT_SMALL, TW64_MENU_LABEL_X, Y + 2, aBuf, 140,
                      158, 195, TW64_ALIGN_LEFT);
      Tw64RenderTextF(TW64_FONT_MENU, TW64_MENU_LABEL_X + 18, Y,
                      g_apPlayerNames[Best], R, G, B, TW64_ALIGN_LEFT);
      str_format(aBuf, sizeof(aBuf), "%d",
                 g_pGameServer->m_apPlayers[Best]->m_Score);
      /* Right edge chosen so the widest note -- the three-field CTF funnel,
       * which runs back to about x=161 from its own right edge -- cannot
       * reach into the score column. */
      Tw64RenderTextF(TW64_FONT_MENU, 150, Y, aBuf, R, G, B,
                      TW64_ALIGN_RIGHT);
      /* The score column is the mode's own score; the note column is what
       * produced it, which is the flag funnel in CTF and the kill ledger
       * everywhere else. */
      const CGameContext::CPlayerStats &Stats =
          g_pGameServer->m_aPlayerStats[Best];
      if (s_aModes[g_Match.m_Mode].m_Flags)
        str_format(aBuf, sizeof(aBuf), "GRABS %d   CAPS %d   RET %d",
                   Stats.m_FlagGrabs, Stats.m_FlagCaptures,
                   Stats.m_FlagReturns);
      else
        str_format(aBuf, sizeof(aBuf), "KILLS %d   DEATHS %d", Stats.m_Kills,
                   Stats.m_Deaths);
      Tw64RenderTextF(TW64_FONT_SMALL, TW64_MENU_NOTE_X, Y + 2, aBuf, 140, 158,
                      195, TW64_ALIGN_RIGHT);
      ++Row;
    }
    Tw64RenderEndPage();
  }
}

/* ------------------------------------------------------------------ */
/* Match loop                                                         */
/* ------------------------------------------------------------------ */

void RunMatchLoop() {
  debugf("TW64 GAME_START mode=%s map=%s humans=%d players=%d bots=%s "
         "driver=%s views=%d teams=%d flags=%d\n",
         g_Match.m_aLabel, g_Match.m_pMap, g_Match.m_NumHumans,
         g_Match.m_NumPlayers, g_Match.m_pBotPolicy,
         g_Match.m_pDriverPolicy ? g_Match.m_pDriverPolicy : "none",
         g_NumViewports, s_aModes[g_Match.m_Mode].m_Teams ? 1 : 0,
         s_aModes[g_Match.m_Mode].m_Flags ? 1 : 0);

  uint64_t Last = get_ticks_us();
  uint64_t Accumulator = 0;
  int SkipStreak = 0;

  while (!g_MatchOver) {
    const uint64_t Now = get_ticks_us();
    uint64_t Delta = Now - Last;
    Last = Now;
    /* A long stall (first frame after loading) must not be replayed. */
    if (Delta > 500000ULL)
      Delta = TW64_TICK_US;
    Accumulator += Delta;

    joypad_poll();
    const bool Scoreboard = AnyPadStartHeld();

    int Steps = 0;
    while (Accumulator >= TW64_TICK_US && Steps < TW64_MAX_CATCHUP &&
           !g_MatchOver) {
      SimulateTick();
      Accumulator -= TW64_TICK_US;
      ++Steps;
    }

    bool Behind = Accumulator >= TW64_TICK_US;
    if (Behind) {
      /* The tick rate is authoritative: discard the backlog instead of
       * compressing simulation time, and account the lost frame. */
      Accumulator = 0;
      ++g_WindowDropped;
      ++g_TotalDropped;
    }

    if (!Behind || ++SkipStreak >= TW64_MAX_SKIP_STREAK) {
      SkipStreak = 0;
      RenderMatchFrame(Scoreboard);
    }
  }

  EmitMatchEnd();
}

} // namespace

/* ------------------------------------------------------------------ */
/* Entry point                                                        */
/* ------------------------------------------------------------------ */

void Tw64RunGame(void) {
  Tw64RenderInit();
  for (int i = 0; i < TW64_MAX_LOCAL; ++i) {
    g_apAutoDrivers[i] = 0;
    g_apPlayerNames[i] = "";
    Tw64InputReset(&g_aHumanInput[i]);
  }
  g_aLoadedMap[0] = 0;

  /* Autoplay ROMs drive every human slot from a bot, so the controller
   * mapping would otherwise ship without ever executing on target. */
  joypad_poll();
  const unsigned InputFailures = Tw64InputSelfTest();
  debugf("TW64 INPUT_SELFTEST failures=0x%x pads=%d\n", InputFailures,
         ConnectedPads());

  DrawSimplePage("LOADING", "engine, assets and map", 0);
  if (!BootEngine()) {
    while (true)
      DrawSimplePage("ENGINE INIT FAILED", "the ROM cannot start a match", 0);
  }
  debugf("TW64 ENGINE_OK heap=%d heap_base=%d maps=%d modes=%d\n",
         HeapUsed() - g_HeapBase, g_HeapBase, TW64_NUM_MAPS, TW64_NUM_MODES);

  while (true) {
    CMatchConfig Config;
    mem_zero(&Config, sizeof(Config));
    RunMenu(&Config);

    DrawSimplePage("STARTING MATCH", Config.m_aLabel, Config.m_pMap);
    const uint64_t LoadBegin = get_ticks_us();
    if (!StartMatch(Config)) {
      while (true)
        DrawSimplePage("MATCH INIT FAILED", Config.m_aLabel, Config.m_pMap);
    }
    debugf("TW64 MATCH_READY mode=%s map=%s load_us=%lu heap=%d\n",
           Config.m_aLabel, Config.m_pMap,
           (unsigned long)(uint32_t)(get_ticks_us() - LoadBegin),
           HeapUsed() - g_HeapBase);

    BuildViewports(Config.m_NumHumans);
    RunMatchLoop();
    RunEndScreen();
  }
}
