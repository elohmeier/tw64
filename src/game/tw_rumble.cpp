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
  TW64_RUMBLE_DAMAGE_LIGHT,
  TW64_RUMBLE_DAMAGE_MEDIUM,
  TW64_RUMBLE_DAMAGE_HEAVY,
  TW64_RUMBLE_EFFECT_COUNT
};

/* The original accessory is a binary motor with noticeable spin-up time.
 * Durations below are deliberately coarse; rapid PWM would add Joybus traffic
 * without creating a reliable intensity scale on real hardware. */
struct CEffectSpec {
  uint16_t m_OnMs;
  uint8_t m_Priority;
};

const CEffectSpec s_aEffects[TW64_RUMBLE_EFFECT_COUNT] = {
    {0, 0},    /* none */
    {80, 60},  /* 1-2 damage */
    {140, 70}, /* 3-5 damage */
    {200, 80}, /* 6+ damage */
};

static_assert(sizeof(s_aEffects) / sizeof(s_aEffects[0]) ==
                  TW64_RUMBLE_EFFECT_COUNT,
              "rumble effect table mismatch");

struct CPortState {
  ERumbleEffect m_Effect;
  uint64_t m_DeadlineUs;
};

CPortState s_aPorts[JOYPAD_PORT_COUNT];
int s_aClientByPort[JOYPAD_PORT_COUNT];
int s_NumHumans;
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
  return State.m_Effect != TW64_RUMBLE_NONE;
}

void ClearPort(CPortState *pState) {
  pState->m_Effect = TW64_RUMBLE_NONE;
  pState->m_DeadlineUs = 0;
}

/* Higher-priority feedback replaces a lower-priority pattern. An equal or
 * lower request is discarded while one is active, so repeated events can
 * never keep the motor on indefinitely by extending its deadline. */
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
  pState->m_DeadlineUs = NowUs + (uint64_t)Spec.m_OnMs * 1000ULL;
  if (CountStats && !WasOn)
    ++s_WindowTransitions;
  return true;
}

void AdvancePort(CPortState *pState, uint64_t NowUs, bool CountStats) {
  if (pState->m_Effect != TW64_RUMBLE_NONE && NowUs >= pState->m_DeadlineUs) {
    ClearPort(pState);
    if (CountStats)
      ++s_WindowTransitions;
  }
}

void ScanDamageEvents(CGameContext *pGameServer, uint64_t NowUs) {
  int aDamage[JOYPAD_PORT_COUNT] = {0, 0, 0, 0};
  const CEventHandler &Events = pGameServer->m_Events;
  for (int i = 0; i < Events.NumEvents(); ++i) {
    if (Events.EventType(i) == NETEVENTTYPE_DAMAGE) {
      const CNetEvent_Damage *pDamage =
          (const CNetEvent_Damage *)Events.EventData(i);
      for (int Port = 0; Port < s_NumHumans; ++Port)
        if (pDamage->m_ClientID == s_aClientByPort[Port])
          aDamage[Port] += pDamage->m_HealthAmount + pDamage->m_ArmorAmount;
    }
  }

  for (int Port = 0; Port < s_NumHumans; ++Port) {
    if (aDamage[Port] > 0) {
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

  if (!RequestEffect(&State, TW64_RUMBLE_DAMAGE_LIGHT, Start, false) ||
      !PortOn(State) || State.m_DeadlineUs != Start + 80000ULL)
    Failures |= 1u << 0;

  /* Equal priority cannot perpetually extend a repeated pulse. */
  if (RequestEffect(&State, TW64_RUMBLE_DAMAGE_LIGHT, Start + 10000ULL,
                    false) ||
      State.m_DeadlineUs != Start + 80000ULL)
    Failures |= 1u << 1;

  /* Stronger damage replaces lighter damage with a fresh bounded deadline. */
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

  RequestEffect(&State, TW64_RUMBLE_DAMAGE_MEDIUM, Start, false);
  if (!RequestEffect(&State, TW64_RUMBLE_DAMAGE_HEAVY, Start + 1000ULL,
                     false) ||
      State.m_Effect != TW64_RUMBLE_DAMAGE_HEAVY ||
      State.m_DeadlineUs != Start + 201000ULL)
    Failures |= 1u << 5;
  if (RequestEffect(&State, TW64_RUMBLE_DAMAGE_LIGHT, Start + 2000ULL, false) ||
      State.m_DeadlineUs != Start + 201000ULL)
    Failures |= 1u << 6;
  return Failures;
}

void Tw64RumbleInit(void) {
  memset(s_aPorts, 0, sizeof(s_aPorts));
  for (int Port = 0; Port < JOYPAD_PORT_COUNT; ++Port)
    s_aClientByPort[Port] = -1;
  s_NumHumans = 0;
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
  s_AllowHardware = false;
}

void Tw64RumbleResetMatch(const int *pClientByPort, int NumPorts,
                          bool AllowHardware) {
  Tw64RumbleStopAll();
  s_NumHumans = NumPorts < JOYPAD_PORT_COUNT ? NumPorts : JOYPAD_PORT_COUNT;
  if (s_NumHumans < 0)
    s_NumHumans = 0;
  s_AllowHardware = AllowHardware;

  for (int Port = 0; Port < s_NumHumans; ++Port)
    s_aClientByPort[Port] = pClientByPort ? pClientByPort[Port] : -1;

  ResetWindow();
  debugf("TW64 RUMBLE_MATCH ports=%d clients=%d,%d,%d,%d hardware=%d "
         "supported=0x%x\n",
         s_NumHumans, s_aClientByPort[0], s_aClientByPort[1],
         s_aClientByPort[2], s_aClientByPort[3], s_AllowHardware ? 1 : 0,
         Tw64RumbleSupportedMask());
}

void Tw64RumbleTick(CGameContext *pGameServer) {
  if (!pGameServer || s_NumHumans <= 0)
    return;
  const uint32_t Begin = TICKS_READ();
  const uint64_t NowUs = get_ticks_us();
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
