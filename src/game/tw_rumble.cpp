/* Teeworlds 64: per-player controller rumble. See tw_rumble.h. */

#include <stdint.h>
#include <string.h>

#include <debug.h>
#include <joypad.h>
#include <n64sys.h>

#include <generated/protocol.h>

#include <game/server/gamecontext.h>

#include "tw_rumble.h"

namespace {

enum ERumbleEffect {
  TW64_RUMBLE_NONE = 0,
  TW64_RUMBLE_SHOT_LIGHT,
  TW64_RUMBLE_SHOT_HEAVY,
  TW64_RUMBLE_SHOT_NINJA,
  TW64_RUMBLE_FLAG_ACTION,
  TW64_RUMBLE_FLAG_CAPTURE,
  TW64_RUMBLE_DAMAGE_LIGHT,
  TW64_RUMBLE_DAMAGE_MEDIUM,
  TW64_RUMBLE_DAMAGE_HEAVY,
  TW64_RUMBLE_DEATH,
  TW64_RUMBLE_EFFECT_COUNT
};

/* The original accessory is a binary motor with noticeable spin-up time.
 * Durations below are deliberately coarse; rapid PWM would add Joybus traffic
 * without creating a reliable intensity scale on real hardware. */
struct CEffectSpec {
  uint16_t m_OnMs;
  uint16_t m_OffMs;
  uint8_t m_Pulses;
  uint8_t m_Priority;
};

const CEffectSpec s_aEffects[TW64_RUMBLE_EFFECT_COUNT] = {
    {0, 0, 0, 0},     /* none */
    {60, 0, 1, 10},   /* gun / hammer */
    {90, 0, 1, 11},   /* shotgun / grenade / laser */
    {120, 0, 1, 12},  /* ninja */
    {100, 0, 1, 30},  /* local flag grab or return */
    {120, 80, 2, 50}, /* local flag capture */
    {80, 0, 1, 60},   /* 1-2 damage */
    {140, 0, 1, 70},  /* 3-5 damage */
    {200, 0, 1, 80},  /* 6+ damage */
    {300, 0, 1, 100}, /* local death */
};

static_assert(sizeof(s_aEffects) / sizeof(s_aEffects[0]) ==
                  TW64_RUMBLE_EFFECT_COUNT,
              "rumble effect table mismatch");

struct CPortState {
  ERumbleEffect m_Effect;
  uint8_t m_PulsesRemaining;
  bool m_PhaseOn;
  uint64_t m_DeadlineUs;
};

struct CPlayerTracker {
  int m_aWeaponShots[NUM_WEAPONS];
  int m_FlagGrabs;
  int m_FlagCaptures;
  int m_FlagReturns;
};

CPortState s_aPorts[JOYPAD_PORT_COUNT];
CPlayerTracker s_aTrackers[JOYPAD_PORT_COUNT];
int s_aClientByPort[JOYPAD_PORT_COUNT];
int s_NumHumans;
bool s_FlagMode;
bool s_AllowHardware;
unsigned s_LastSupportedMask;

uint64_t s_TickCycles;
uint32_t s_TickMaxCycles;
uint32_t s_TickCalls;
uint64_t s_UpdateCycles;
uint32_t s_UpdateMaxCycles;
uint32_t s_UpdateCalls;
uint32_t s_WindowRequests;
uint32_t s_WindowIgnored;
uint32_t s_WindowTransitions;
uint32_t s_WindowWrites;

bool PortOn(const CPortState &State) {
  return State.m_Effect != TW64_RUMBLE_NONE && State.m_PhaseOn;
}

void ClearPort(CPortState *pState) {
  pState->m_Effect = TW64_RUMBLE_NONE;
  pState->m_PulsesRemaining = 0;
  pState->m_PhaseOn = false;
  pState->m_DeadlineUs = 0;
}

/* Higher-priority feedback replaces a lower-priority pattern. An equal or
 * lower request is discarded while one is active, so rapid fire can never
 * keep the motor on indefinitely by continually extending its deadline. */
bool RequestEffect(CPortState *pState, ERumbleEffect Effect, uint64_t NowUs,
                   bool CountStats) {
  if (CountStats)
    ++s_WindowRequests;
  if (Effect <= TW64_RUMBLE_NONE || Effect >= TW64_RUMBLE_EFFECT_COUNT)
    return false;

  const CEffectSpec &Spec = s_aEffects[Effect];
  if (pState->m_Effect != TW64_RUMBLE_NONE &&
      Spec.m_Priority <= s_aEffects[pState->m_Effect].m_Priority) {
    if (CountStats)
      ++s_WindowIgnored;
    return false;
  }

  const bool WasOn = PortOn(*pState);
  pState->m_Effect = Effect;
  pState->m_PulsesRemaining = Spec.m_Pulses;
  pState->m_PhaseOn = true;
  pState->m_DeadlineUs = NowUs + (uint64_t)Spec.m_OnMs * 1000ULL;
  if (CountStats && !WasOn)
    ++s_WindowTransitions;
  return true;
}

void AdvancePort(CPortState *pState, uint64_t NowUs, bool CountStats) {
  while (pState->m_Effect != TW64_RUMBLE_NONE &&
         NowUs >= pState->m_DeadlineUs) {
    const CEffectSpec &Spec = s_aEffects[pState->m_Effect];
    const bool WasOn = PortOn(*pState);
    if (pState->m_PhaseOn) {
      pState->m_PhaseOn = false;
      if (pState->m_PulsesRemaining <= 1) {
        ClearPort(pState);
      } else {
        pState->m_DeadlineUs += (uint64_t)Spec.m_OffMs * 1000ULL;
      }
    } else {
      --pState->m_PulsesRemaining;
      pState->m_PhaseOn = true;
      pState->m_DeadlineUs += (uint64_t)Spec.m_OnMs * 1000ULL;
    }
    if (CountStats && WasOn != PortOn(*pState))
      ++s_WindowTransitions;
  }
}

ERumbleEffect ShotEffect(const CGameContext::CPlayerStats &Stats,
                         CPlayerTracker *pTracker) {
  ERumbleEffect Effect = TW64_RUMBLE_NONE;
  for (int Weapon = 0; Weapon < NUM_WEAPONS; ++Weapon) {
    if (Stats.m_aWeaponShots[Weapon] > pTracker->m_aWeaponShots[Weapon]) {
      ERumbleEffect Candidate = TW64_RUMBLE_SHOT_LIGHT;
      if (Weapon == WEAPON_NINJA)
        Candidate = TW64_RUMBLE_SHOT_NINJA;
      else if (Weapon == WEAPON_SHOTGUN || Weapon == WEAPON_GRENADE ||
               Weapon == WEAPON_LASER)
        Candidate = TW64_RUMBLE_SHOT_HEAVY;
      if (s_aEffects[Candidate].m_Priority > s_aEffects[Effect].m_Priority)
        Effect = Candidate;
    }
    pTracker->m_aWeaponShots[Weapon] = Stats.m_aWeaponShots[Weapon];
  }
  return Effect;
}

void ScanStats(CGameContext *pGameServer, uint64_t NowUs) {
  for (int Port = 0; Port < s_NumHumans; ++Port) {
    const int ClientID = s_aClientByPort[Port];
    if (ClientID < 0 || ClientID >= MAX_CLIENTS)
      continue;
    const CGameContext::CPlayerStats &Stats =
        pGameServer->m_aPlayerStats[ClientID];
    CPlayerTracker &Tracker = s_aTrackers[Port];

    const ERumbleEffect Shot = ShotEffect(Stats, &Tracker);
    if (Shot != TW64_RUMBLE_NONE)
      RequestEffect(&s_aPorts[Port], Shot, NowUs, true);

    if (s_FlagMode) {
      if (Stats.m_FlagCaptures > Tracker.m_FlagCaptures)
        RequestEffect(&s_aPorts[Port], TW64_RUMBLE_FLAG_CAPTURE, NowUs, true);
      else if (Stats.m_FlagGrabs > Tracker.m_FlagGrabs ||
               Stats.m_FlagReturns > Tracker.m_FlagReturns)
        RequestEffect(&s_aPorts[Port], TW64_RUMBLE_FLAG_ACTION, NowUs, true);
    }
    Tracker.m_FlagGrabs = Stats.m_FlagGrabs;
    Tracker.m_FlagCaptures = Stats.m_FlagCaptures;
    Tracker.m_FlagReturns = Stats.m_FlagReturns;
  }
}

void ScanDamageEvents(CGameContext *pGameServer, uint64_t NowUs) {
  int aDamage[JOYPAD_PORT_COUNT] = {0, 0, 0, 0};
  bool aDeath[JOYPAD_PORT_COUNT] = {false, false, false, false};
  const CEventHandler &Events = pGameServer->m_Events;
  for (int i = 0; i < Events.NumEvents(); ++i) {
    if (Events.EventType(i) == NETEVENTTYPE_DAMAGE) {
      const CNetEvent_Damage *pDamage =
          (const CNetEvent_Damage *)Events.EventData(i);
      for (int Port = 0; Port < s_NumHumans; ++Port)
        if (pDamage->m_ClientID == s_aClientByPort[Port])
          aDamage[Port] +=
              pDamage->m_HealthAmount + pDamage->m_ArmorAmount;
    } else if (Events.EventType(i) == NETEVENTTYPE_DEATH) {
      const CNetEvent_Death *pDeath =
          (const CNetEvent_Death *)Events.EventData(i);
      for (int Port = 0; Port < s_NumHumans; ++Port)
        if (pDeath->m_ClientID == s_aClientByPort[Port])
          aDeath[Port] = true;
    }
  }

  for (int Port = 0; Port < s_NumHumans; ++Port) {
    if (aDeath[Port]) {
      RequestEffect(&s_aPorts[Port], TW64_RUMBLE_DEATH, NowUs, true);
    } else if (aDamage[Port] > 0) {
      const ERumbleEffect Effect = aDamage[Port] >= 6 ? TW64_RUMBLE_DAMAGE_HEAVY
                                   : aDamage[Port] >= 3
                                       ? TW64_RUMBLE_DAMAGE_MEDIUM
                                       : TW64_RUMBLE_DAMAGE_LIGHT;
      RequestEffect(&s_aPorts[Port], Effect, NowUs, true);
    }
  }
}

unsigned ActiveMask(void) {
  unsigned Mask = 0;
  for (int Port = 0; Port < s_NumHumans; ++Port)
    if (PortOn(s_aPorts[Port]))
      Mask |= 1u << Port;
  return Mask;
}

uint32_t CyclesToUs(uint64_t Cycles) {
  /* TICKS_PER_SECOND is 46.875 MHz => microseconds = cycles * 8 / 375. */
  return (uint32_t)((Cycles * 8ULL) / 375ULL);
}

void ResetWindow(void) {
  s_TickCycles = 0;
  s_TickMaxCycles = 0;
  s_TickCalls = 0;
  s_UpdateCycles = 0;
  s_UpdateMaxCycles = 0;
  s_UpdateCalls = 0;
  s_WindowRequests = 0;
  s_WindowIgnored = 0;
  s_WindowTransitions = 0;
  s_WindowWrites = 0;
}

} // namespace

unsigned Tw64RumbleSupportedMask(void) {
  unsigned Mask = 0;
  for (int Port = 0; Port < JOYPAD_PORT_COUNT; ++Port)
    if (joypad_get_rumble_supported((joypad_port_t)Port))
      Mask |= 1u << Port;
  return Mask;
}

int Tw64RumbleSupportedCount(void) {
  unsigned Mask = Tw64RumbleSupportedMask();
  int Count = 0;
  while (Mask) {
    Count += Mask & 1u;
    Mask >>= 1;
  }
  return Count;
}

unsigned Tw64RumbleSelfTest(void) {
  unsigned Failures = 0;
  CPortState State;
  ClearPort(&State);
  const uint64_t Start = 1000000ULL;

  if (!RequestEffect(&State, TW64_RUMBLE_SHOT_LIGHT, Start, false) ||
      !PortOn(State) || State.m_DeadlineUs != Start + 60000ULL)
    Failures |= 1u << 0;

  /* Equal priority cannot perpetually extend a rapid-fire pulse. */
  if (RequestEffect(&State, TW64_RUMBLE_SHOT_LIGHT, Start + 10000ULL, false) ||
      State.m_DeadlineUs != Start + 60000ULL)
    Failures |= 1u << 1;

  /* Damage replaces weapon recoil and owns a fresh bounded deadline. */
  if (!RequestEffect(&State, TW64_RUMBLE_DAMAGE_MEDIUM, Start + 10000ULL,
                     false) ||
      State.m_Effect != TW64_RUMBLE_DAMAGE_MEDIUM ||
      State.m_DeadlineUs != Start + 150000ULL)
    Failures |= 1u << 2;
  AdvancePort(&State, Start + 149999ULL, false);
  if (!PortOn(State))
    Failures |= 1u << 3;
  AdvancePort(&State, Start + 150000ULL, false);
  if (State.m_Effect != TW64_RUMBLE_NONE || PortOn(State))
    Failures |= 1u << 4;

  /* Capture is exactly two pulses separated by one off interval. */
  RequestEffect(&State, TW64_RUMBLE_FLAG_CAPTURE, Start, false);
  AdvancePort(&State, Start + 120000ULL, false);
  if (PortOn(State) || State.m_Effect != TW64_RUMBLE_FLAG_CAPTURE)
    Failures |= 1u << 5;
  AdvancePort(&State, Start + 200000ULL, false);
  if (!PortOn(State) || State.m_PulsesRemaining != 1)
    Failures |= 1u << 6;
  AdvancePort(&State, Start + 320000ULL, false);
  if (State.m_Effect != TW64_RUMBLE_NONE || PortOn(State))
    Failures |= 1u << 7;

  RequestEffect(&State, TW64_RUMBLE_FLAG_CAPTURE, Start, false);
  if (!RequestEffect(&State, TW64_RUMBLE_DEATH, Start + 1000ULL, false) ||
      State.m_Effect != TW64_RUMBLE_DEATH)
    Failures |= 1u << 8;
  return Failures;
}

void Tw64RumbleInit(void) {
  memset(s_aPorts, 0, sizeof(s_aPorts));
  memset(s_aTrackers, 0, sizeof(s_aTrackers));
  for (int Port = 0; Port < JOYPAD_PORT_COUNT; ++Port)
    s_aClientByPort[Port] = -1;
  s_NumHumans = 0;
  s_FlagMode = false;
  s_AllowHardware = false;
  ResetWindow();
  s_LastSupportedMask = Tw64RumbleSupportedMask();
  const unsigned Failures = Tw64RumbleSelfTest();
  if (Failures == 0) {
    debugf("TW64 RUMBLE_OK selftest=0x%x supported=0x%x effects=%d\n", Failures,
           s_LastSupportedMask, (int)TW64_RUMBLE_EFFECT_COUNT - 1);
  } else {
    debugf("TW64 RUMBLE_FAIL selftest=0x%x supported=0x%x effects=%d\n",
           Failures, s_LastSupportedMask, (int)TW64_RUMBLE_EFFECT_COUNT - 1);
  }
}

void Tw64RumbleStopAll(void) {
  for (int Port = 0; Port < JOYPAD_PORT_COUNT; ++Port) {
    const joypad_port_t JoyPort = (joypad_port_t)Port;
    if (joypad_get_rumble_supported(JoyPort) &&
        joypad_get_rumble_active(JoyPort)) {
      joypad_set_rumble_active(JoyPort, false);
      ++s_WindowWrites;
    }
    ClearPort(&s_aPorts[Port]);
    s_aClientByPort[Port] = -1;
  }
  s_NumHumans = 0;
  s_FlagMode = false;
  s_AllowHardware = false;
}

void Tw64RumbleResetMatch(CGameContext *pGameServer,
                          const int *pClientByPort, int NumPorts,
                          bool FlagMode, bool AllowHardware) {
  Tw64RumbleStopAll();
  memset(s_aTrackers, 0, sizeof(s_aTrackers));
  s_NumHumans = NumPorts < JOYPAD_PORT_COUNT ? NumPorts : JOYPAD_PORT_COUNT;
  if (s_NumHumans < 0)
    s_NumHumans = 0;
  s_FlagMode = FlagMode;
  s_AllowHardware = AllowHardware;

  for (int Port = 0; Port < s_NumHumans; ++Port)
    s_aClientByPort[Port] = pClientByPort ? pClientByPort[Port] : -1;

  if (pGameServer) {
    for (int Port = 0; Port < s_NumHumans; ++Port) {
      const int ClientID = s_aClientByPort[Port];
      if (ClientID < 0 || ClientID >= MAX_CLIENTS)
        continue;
      const CGameContext::CPlayerStats &Stats =
          pGameServer->m_aPlayerStats[ClientID];
      memcpy(s_aTrackers[Port].m_aWeaponShots, Stats.m_aWeaponShots,
             sizeof(s_aTrackers[Port].m_aWeaponShots));
      s_aTrackers[Port].m_FlagGrabs = Stats.m_FlagGrabs;
      s_aTrackers[Port].m_FlagCaptures = Stats.m_FlagCaptures;
      s_aTrackers[Port].m_FlagReturns = Stats.m_FlagReturns;
    }
  }
  ResetWindow();
  debugf("TW64 RUMBLE_MATCH ports=%d clients=%d,%d,%d,%d hardware=%d "
         "flags=%d supported=0x%x\n",
         s_NumHumans, s_aClientByPort[0], s_aClientByPort[1],
         s_aClientByPort[2], s_aClientByPort[3],
         s_AllowHardware ? 1 : 0, s_FlagMode ? 1 : 0,
         Tw64RumbleSupportedMask());
}

void Tw64RumbleTick(CGameContext *pGameServer) {
  if (!pGameServer || s_NumHumans <= 0)
    return;
  const uint32_t Begin = TICKS_READ();
  const uint64_t NowUs = get_ticks_us();
  ScanStats(pGameServer, NowUs);
  ScanDamageEvents(pGameServer, NowUs);
  const uint32_t Elapsed = (uint32_t)TICKS_SINCE(Begin);
  s_TickCycles += Elapsed;
  if (Elapsed > s_TickMaxCycles)
    s_TickMaxCycles = Elapsed;
  ++s_TickCalls;
}

void Tw64RumbleUpdate(void) {
  const uint32_t Begin = TICKS_READ();
  const uint64_t NowUs = get_ticks_us();
  for (int Port = 0; Port < JOYPAD_PORT_COUNT; ++Port)
    AdvancePort(&s_aPorts[Port], NowUs, true);

  const unsigned Supported = Tw64RumbleSupportedMask();
  if (Supported != s_LastSupportedMask) {
    s_LastSupportedMask = Supported;
    debugf("TW64 RUMBLE_PORTS supported=0x%x\n", Supported);
  }

  for (int Port = 0; Port < JOYPAD_PORT_COUNT; ++Port) {
    const joypad_port_t JoyPort = (joypad_port_t)Port;
    if (!(Supported & (1u << Port)))
      continue;
    const bool Desired =
        s_AllowHardware && Port < s_NumHumans && PortOn(s_aPorts[Port]);
    if (joypad_get_rumble_active(JoyPort) != Desired) {
      joypad_set_rumble_active(JoyPort, Desired);
      ++s_WindowWrites;
    }
  }

  const uint32_t Elapsed = (uint32_t)TICKS_SINCE(Begin);
  s_UpdateCycles += Elapsed;
  if (Elapsed > s_UpdateMaxCycles)
    s_UpdateMaxCycles = Elapsed;
  ++s_UpdateCalls;
}

void Tw64RumbleTakeWindow(CTw64RumbleStats *pStats) {
  const uint32_t TickAvg =
      s_TickCalls ? (uint32_t)(s_TickCycles / s_TickCalls) : 0;
  const uint32_t UpdateAvg =
      s_UpdateCalls ? (uint32_t)(s_UpdateCycles / s_UpdateCalls) : 0;
  pStats->m_TickAvgUs = CyclesToUs(TickAvg);
  pStats->m_TickMaxUs = CyclesToUs(s_TickMaxCycles);
  pStats->m_UpdateAvgUs = CyclesToUs(UpdateAvg);
  pStats->m_UpdateMaxUs = CyclesToUs(s_UpdateMaxCycles);
  pStats->m_Requests = s_WindowRequests;
  pStats->m_Ignored = s_WindowIgnored;
  pStats->m_Transitions = s_WindowTransitions;
  pStats->m_Writes = s_WindowWrites;
  pStats->m_SupportedMask = Tw64RumbleSupportedMask();
  pStats->m_ActiveMask = ActiveMask();
  ResetWindow();
}
