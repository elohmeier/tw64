/* Teeworlds 64: on-target deterministic match harness.
 *
 * This is a faithful duplicate of the measured part of
 * src/tools/botbench.cpp RunMatch(): the same benchmark IServer stub, the same
 * kernel/interface registration order, the same config, the same map lookup
 * order, the same scramble-then-install bot sequence, the same per-tick order
 * (AdvanceTick -> OnTick -> HashGameState) and the same FNV-1a style
 * incremental state hash. botbench.cpp itself is host-frozen evidence and is
 * deliberately not modified or compiled here.
 *
 * Differences from RunMatch(), all of which are provably outside the hashed
 * simulation:
 *   - No trace writer, no JSON report, no traversal/exploration trackers.
 *     Those only read state; none of them feeds anything back into the world.
 *   - The observation-seam validation is kept, on the same one-per-simulated-
 *     second schedule, because it is part of the fairness contract.
 *   - The running hash is printed every TW64_HASH_INTERVAL ticks so the host
 *     and the target can be bisected; the value at the final tick is exactly
 *     botbench's "state_hash".
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <debug.h>
#include <n64sys.h>

#include <base/math.h>
#include <base/system.h>

#include <engine/config.h>
#include <engine/console.h>
#include <engine/map.h>
#include <engine/server.h>
#include <engine/shared/config.h>
#include <engine/shared/protocol.h>
#include <engine/storage.h>

#include <game/server/bot.h>
#include <game/server/entities/character.h>
#include <game/server/entities/pickup.h>
#include <game/server/entities/projectile.h>
#include <game/server/entity.h>
#include <game/server/gamecontext.h>
#include <game/server/gamecontroller.h>
#include <game/server/player.h>

#include "tw_match.h"

#ifndef TW64_SCENARIO
#define TW64_SCENARIO 1
#endif

#define TW64_HASH_INTERVAL 500

namespace {

/* Byte-identical copy of botbench.cpp's CBenchmarkServer. */
class CBenchmarkServer : public IServer {
  enum {
    MAX_RECYCLED_IDS = 4096,
  };

  char m_aaNames[MAX_CLIENTS][MAX_NAME_ARRAY_SIZE];
  int m_aScores[MAX_CLIENTS];
  bool m_aIngame[MAX_CLIENTS];
  int m_aFreeIDs[MAX_RECYCLED_IDS];
  int m_NumFreeIDs;
  int m_NextID;

public:
  CBenchmarkServer() {
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
  virtual const char *ClientClan(int ClientID) const { return "botbench"; }
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
    str_format(pAddrStr, Size, "botbench-%d", ClientID);
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

/* Byte-identical copy of botbench.cpp's hashing. */
void HashInt(uint64_t *pHash, int Value) {
  uint32_t Bits = (uint32_t)Value;
  for (int i = 0; i < 4; ++i) {
    *pHash ^= (Bits >> (i * 8)) & 0xff;
    *pHash *= 1099511628211ULL;
  }
}

void HashGameState(uint64_t *pHash, CGameContext *pGameServer) {
  HashInt(pHash, pGameServer->Server()->Tick());
  for (int i = 0; i < MAX_CLIENTS; ++i) {
    CPlayer *pPlayer = pGameServer->m_apPlayers[i];
    if (!pPlayer)
      continue;
    HashInt(pHash, i);
    HashInt(pHash, pPlayer->m_Score);
    CCharacter *pCharacter = pPlayer->GetCharacter();
    HashInt(pHash, pCharacter != 0);
    if (pCharacter) {
      HashInt(pHash, round_to_int(pCharacter->GetPos().x * 256.0f));
      HashInt(pHash, round_to_int(pCharacter->GetPos().y * 256.0f));
      HashInt(pHash, round_to_int(pCharacter->GetVelocity().x * 256.0f));
      HashInt(pHash, round_to_int(pCharacter->GetVelocity().y * 256.0f));
      HashInt(pHash, pCharacter->GetHealth());
      HashInt(pHash, pCharacter->GetArmor());
      HashInt(pHash, pCharacter->GetActiveWeapon());
      HashInt(pHash, pCharacter->GetActiveWeaponAmmo());
    }
  }
  for (int Type = 0; Type < CGameWorld::NUM_ENTTYPES; ++Type) {
    for (CEntity *pEntity = pGameServer->m_World.FindFirst(Type); pEntity;
         pEntity = pEntity->TypeNext()) {
      HashInt(pHash, Type);
      HashInt(pHash, round_to_int(pEntity->GetPos().x * 256.0f));
      HashInt(pHash, round_to_int(pEntity->GetPos().y * 256.0f));
      if (Type == CGameWorld::ENTTYPE_PROJECTILE) {
        CProjectile *pProjectile = static_cast<CProjectile *>(pEntity);
        float Time =
            (pGameServer->Server()->Tick() - pProjectile->GetStartTick()) /
            (float)pGameServer->Server()->TickSpeed();
        vec2 Position = pProjectile->GetPos(Time);
        HashInt(pHash, pProjectile->GetOwner());
        HashInt(pHash, pProjectile->GetType());
        HashInt(pHash, round_to_int(Position.x * 256.0f));
        HashInt(pHash, round_to_int(Position.y * 256.0f));
        HashInt(pHash, round_to_int(pProjectile->GetDirection().x * 256.0f));
        HashInt(pHash, round_to_int(pProjectile->GetDirection().y * 256.0f));
      } else if (Type == CGameWorld::ENTTYPE_PICKUP) {
        CPickup *pPickup = static_cast<CPickup *>(pEntity);
        HashInt(pHash, pPickup->GetType());
        HashInt(pHash, pPickup->IsAvailable());
      }
    }
  }
}

bool ValidateObservationSeam(CGameContext *pGameServer, int NumPlayers,
                             int *pHiddenCharacterChecks) {
  for (int ClientID = 0; ClientID < NumPlayers; ++ClientID) {
    CBotWorldState World;
    BuildBotWorldState(pGameServer, ClientID, &World);
    for (int i = 0; i < World.m_NumCharacters; ++i) {
      const CBotCharacterState &Character = World.m_aCharacters[i];
      if (Character.m_Visible != Character.m_Known)
        return false;
      if (Character.m_Visible)
        continue;
      ++*pHiddenCharacterChecks;
      if (Character.m_Position.x != 0.0f || Character.m_Position.y != 0.0f ||
          Character.m_Velocity.x != 0.0f || Character.m_Velocity.y != 0.0f ||
          Character.m_Health != 0 || Character.m_Armor != 0 ||
          Character.m_ActiveWeapon != 0 || Character.m_Ammo != 0)
        return false;
    }
    for (int i = 0; i < World.m_NumProjectiles; ++i) {
      if (!World.m_aProjectiles[i].m_Visible)
        return false;
    }
    for (int i = 0; i < World.m_NumPickups; ++i) {
      const CBotPickupState &Pickup = World.m_aPickups[i];
      if (!Pickup.m_Visible && !Pickup.m_StrategicVisible && Pickup.m_Available)
        return false;
    }
  }
  return true;
}

struct CScenario {
  const char *m_pName;
  const char *m_pBotA;
  const char *m_pBotB;
  const char *m_apSparringBots[2];
  const char *m_pMap;
  const char *m_pGameType;
  const char *m_pScrambleBot;
  uint64_t m_Seed;
  int m_WarmupTicks;
  int m_Ticks;
};

#if TW64_SCENARIO == 2
/* botbench --bot-a hunter140 --bot-b hunter140 --sparring-bot-2 hunter
 *          --sparring-bot-3 hunter --map dm6 --seed 42424243
 *          --scramble-bot hunter --warmup-ticks 500 --ticks 2000 */
const CScenario g_Scenario = {"4bot-dm6", "hunter140", "hunter140",
                              {"hunter", "hunter"},
                              "dm6",     "dm",        "hunter",
                              42424243ULL, 500,       2000};
#else
/* botbench --bot-a hunter140 --bot-b hunter119 --map dm1 --gametype dm
 *          --seed 424242 --scramble-bot wander --warmup-ticks 0 --ticks 3000 */
const CScenario g_Scenario = {"2bot-dm1", "hunter140", "hunter119",
                              {0, 0},
                              "dm1",     "dm",        "wander",
                              424242ULL, 0,           3000};
#endif

/* newlib's printf cannot be relied on for 64-bit conversions in this ABI, so
 * format the hash by hand. */
void FormatHash(char *pBuffer, uint64_t Hash) {
  static const char s_aHex[] = "0123456789abcdef";
  for (int i = 0; i < 16; ++i)
    pBuffer[i] = s_aHex[(Hash >> ((15 - i) * 4)) & 0xf];
  pBuffer[16] = 0;
}

uint32_t TicksToMicroseconds(uint64_t Ticks) {
  /* TICKS_PER_SECOND is 46.875 MHz => microseconds = ticks * 8 / 375. */
  return (uint32_t)((Ticks * 8ULL) / 375ULL);
}

int HeapUsed() {
  heap_stats_t Stats;
  sys_get_heap_stats(&Stats);
  return Stats.used;
}

} // namespace

extern "C" int Tw64RunScenario(TW64_MATCH_PROGRESS pfnProgress) {
  const CScenario &Options = g_Scenario;

  debugf("TW64 SIM_START scenario=%s map=%s gametype=%s bots=%s,%s ticks=%d "
         "warmup=%d\n",
         Options.m_pName, Options.m_pMap, Options.m_pGameType, Options.m_pBotA,
         Options.m_pBotB, Options.m_Ticks, Options.m_WarmupTicks);

  const int HeapAtStart = HeapUsed();

  srand((unsigned int)(Options.m_Seed ^ (Options.m_Seed >> 32)));

  IKernel *pKernel = IKernel::Create();
  CBenchmarkServer *pServer = new CBenchmarkServer;
  IEngineMap *pEngineMap = CreateEngineMap();
  CGameContext *pGameServer = new CGameContext;
  IConsole *pConsole = CreateConsole(CFGFLAG_SERVER);
  IStorage *pStorage = CreateTestStorage();
  IConfigManager *pConfigManager = CreateConfigManager();

  bool RegisterFailed = false;
  RegisterFailed =
      RegisterFailed || !pKernel->RegisterInterface(static_cast<IServer *>(pServer));
  RegisterFailed = RegisterFailed ||
                   !pKernel->RegisterInterface(static_cast<IEngineMap *>(pEngineMap));
  RegisterFailed =
      RegisterFailed || !pKernel->RegisterInterface(static_cast<IMap *>(pEngineMap));
  RegisterFailed = RegisterFailed ||
                   !pKernel->RegisterInterface(static_cast<IGameServer *>(pGameServer));
  RegisterFailed = RegisterFailed || !pKernel->RegisterInterface(pConsole);
  RegisterFailed = RegisterFailed || !pKernel->RegisterInterface(pStorage);
  RegisterFailed = RegisterFailed || !pKernel->RegisterInterface(pConfigManager);
  if (RegisterFailed) {
    debugf("TW64 SIM_FAIL reason=register\n");
    return 2;
  }

  pConfigManager->Init(CFGFLAG_SERVER);
  pConsole->Init();
  CConfig *pConfig = pConfigManager->Values();
  str_copy(pConfig->m_SvMap, Options.m_pMap, sizeof(pConfig->m_SvMap));
  str_copy(pConfig->m_SvGametype, Options.m_pGameType,
           sizeof(pConfig->m_SvGametype));
  pConfig->m_SvWarmup = 0;
  pConfig->m_SvCountdown = 0;
  pConfig->m_SvScorelimit = 0;
  pConfig->m_SvTimelimit = 0;
  pConfig->m_SvInactiveKickTime = 0;
  pConfig->m_SvTeambalanceTime = 0;
  pConfig->m_SvPlayerSlots = MAX_PLAYERS;
  pConfig->m_SvMaxClients = MAX_PLAYERS;
  pConfig->m_Debug = 0;

  char aMapPath[IO_MAX_PATH_LENGTH];
  str_format(aMapPath, sizeof(aMapPath), "datasrc/maps/%s.map", Options.m_pMap);
  if (!pEngineMap->Load(aMapPath, pStorage)) {
    str_format(aMapPath, sizeof(aMapPath), "data/maps/%s.map", Options.m_pMap);
    if (!pEngineMap->Load(aMapPath, pStorage)) {
      str_format(aMapPath, sizeof(aMapPath), "maps/%s.map", Options.m_pMap);
      if (!pEngineMap->Load(aMapPath, pStorage)) {
        debugf("TW64 SIM_FAIL reason=map map=%s\n", Options.m_pMap);
        return 2;
      }
    }
  }

  pGameServer->OnConsoleInit();
  pGameServer->OnInit();
  const int NumPlayers = 2 + (Options.m_apSparringBots[0] ? 1 : 0) +
                         (Options.m_apSparringBots[1] ? 1 : 0);
  const char *apCompetitionBots[4] = {Options.m_pBotA, Options.m_pBotB,
                                      Options.m_apSparringBots[0],
                                      Options.m_apSparringBots[1]};
  for (int i = 0; i < NumPlayers; ++i) {
    pServer->SetClientName(i, apCompetitionBots[i]);
    pServer->SetIngame(i, true);
  }
  const uint64_t ScrambleSeed = Options.m_Seed ^ 0x6a09e667f3bcc909ULL;
  for (int i = 0; i < NumPlayers; ++i) {
    if (!pGameServer->AddBot(i, Options.m_pScrambleBot, ScrambleSeed)) {
      debugf("TW64 SIM_FAIL reason=addbot\n");
      return 2;
    }
  }

  int ObservationChecks = 0;
  int HiddenCharacterChecks = 0;
  bool ObservationOk = true;

  uint64_t WarmupTickCycles = 0;
  for (int Tick = 0; Tick < Options.m_WarmupTicks; ++Tick) {
    uint32_t Begin = TICKS_READ();
    pServer->AdvanceTick();
    pGameServer->OnTick();
    WarmupTickCycles += (uint32_t)TICKS_SINCE(Begin);
    if ((Tick + 1) % pServer->TickSpeed() == 0) {
      ++ObservationChecks;
      if (!ValidateObservationSeam(pGameServer, NumPlayers,
                                   &HiddenCharacterChecks)) {
        ObservationOk = false;
        debugf("TW64 SIM_FAIL reason=observation tick=%d\n", Tick + 1);
        return 2;
      }
    }
  }

  for (int i = 0; i < NumPlayers; ++i) {
    IBot *pCompetitionBot = CreateBotWithProfile(apCompetitionBots[i], 0);
    if (!pCompetitionBot) {
      debugf("TW64 SIM_FAIL reason=installbot slot=%d\n", i);
      return 2;
    }
    pCompetitionBot->Reset(Options.m_Seed, i);
    delete pGameServer->m_apBots[i];
    pGameServer->m_apBots[i] = pCompetitionBot;
  }

  for (int i = 0; i < NumPlayers; ++i) {
    pGameServer->m_apPlayers[i]->m_Score = 0;
    pGameServer->m_apPlayers[i]->m_ScoreStartTick = pServer->Tick();
    pServer->SetClientScore(i, 0);
  }
  mem_zero(pGameServer->m_aPlayerStats, sizeof(pGameServer->m_aPlayerStats));

  const int HeapAfterSetup = HeapUsed();
  int HeapPeak = HeapAfterSetup > HeapAtStart ? HeapAfterSetup : HeapAtStart;

  char aHash[17];
  uint64_t StateHash = 1469598103934665603ULL;
  uint64_t TotalTickCycles = 0;
  uint32_t MaxTickCycles = 0;
  const uint64_t WallBegin = get_ticks();

  for (int Tick = 0; Tick < Options.m_Ticks; ++Tick) {
    uint32_t Begin = TICKS_READ();
    pServer->AdvanceTick();
    pGameServer->OnTick();
    uint32_t Elapsed = (uint32_t)TICKS_SINCE(Begin);
    TotalTickCycles += Elapsed;
    if (Elapsed > MaxTickCycles)
      MaxTickCycles = Elapsed;

    HashGameState(&StateHash, pGameServer);

    if ((Tick + 1) % pServer->TickSpeed() == 0) {
      ++ObservationChecks;
      if (!ValidateObservationSeam(pGameServer, NumPlayers,
                                   &HiddenCharacterChecks)) {
        ObservationOk = false;
        debugf("TW64 SIM_FAIL reason=observation tick=%d\n", Tick + 1);
        return 2;
      }
    }

    if ((Tick + 1) % TW64_HASH_INTERVAL == 0) {
      FormatHash(aHash, StateHash);
      debugf("TW64 SIM_HASH tick=%d hash=%s\n", Tick + 1, aHash);
      int Used = HeapUsed();
      if (Used > HeapPeak)
        HeapPeak = Used;
      if (pfnProgress)
        pfnProgress(Tick + 1, Options.m_Ticks);
    }
  }

  const uint64_t WallEnd = get_ticks();
  const int HeapAtEnd = HeapUsed();
  if (HeapAtEnd > HeapPeak)
    HeapPeak = HeapAtEnd;

  FormatHash(aHash, StateHash);
  const uint32_t TotalUs = TicksToMicroseconds(TotalTickCycles);
  const uint32_t WallUs = TicksToMicroseconds(WallEnd - WallBegin);
  const uint32_t MaxUs = TicksToMicroseconds(MaxTickCycles);
  const uint32_t AvgUs =
      (uint32_t)(TicksToMicroseconds(TotalTickCycles) / (uint32_t)Options.m_Ticks);
  const uint32_t WarmupUs = TicksToMicroseconds(WarmupTickCycles);

  debugf("TW64 SIM_DONE scenario=%s hash=%s ticks=%d players=%d total_us=%lu "
         "max_tick_us=%lu avg_tick_us=%lu wall_us=%lu warmup_us=%lu "
         "heap_used=%d heap_peak=%d heap_base=%d observation_checks=%d "
         "hidden_checks=%d observation_ok=%d\n",
         Options.m_pName, aHash, Options.m_Ticks, NumPlayers,
         (unsigned long)TotalUs, (unsigned long)MaxUs, (unsigned long)AvgUs,
         (unsigned long)WallUs, (unsigned long)WarmupUs,
         HeapAtEnd - HeapAtStart, HeapPeak - HeapAtStart, HeapAtStart,
         ObservationChecks, HiddenCharacterChecks, ObservationOk ? 1 : 0);

  for (int i = 0; i < NumPlayers; ++i) {
    CPlayer *pPlayer = pGameServer->m_apPlayers[i];
    const CGameContext::CPlayerStats &Stats = pGameServer->m_aPlayerStats[i];
    debugf("TW64 SIM_PLAYER slot=%d bot=%s team=%d score=%d kills=%d deaths=%d "
           "world_deaths=%d\n",
           i, apCompetitionBots[i], pPlayer ? pPlayer->GetTeam() : -1,
           pPlayer ? pPlayer->m_Score : 0, Stats.m_Kills, Stats.m_Deaths,
           Stats.m_WorldDeaths);
  }

  pGameServer->OnShutdown();
  pEngineMap->Unload();
  delete pKernel;
  delete pServer;
  delete pEngineMap;
  delete pGameServer;
  delete pConsole;
  delete pStorage;
  delete pConfigManager;
  return 0;
}
