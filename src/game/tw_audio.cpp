/* Teeworlds 64: sound. See tw_audio.h. */

#include <math.h>
#include <stdint.h>
#include <string.h>

#include <audio.h>
#include <debug.h>
#include <mixer.h>
#include <n64sys.h>
#include <wav64.h>

#include <base/math.h>
#include <base/system.h>
#include <base/vmath.h>

#include <generated/protocol.h>

#include <game/server/entities/character.h>
#include <game/server/entities/flag.h>
#include <game/server/gamecontext.h>
#include <game/server/gameworld.h>
#include <game/server/player.h>

#include "tw_audio.h"
#include "tw_render.h"

namespace {

enum {
  /* The AI runs at the sample rate the waveforms were converted to
   * (scripts/convert_n64_audio.py), so the RSP mixer copies each voice 1:1
   * instead of resampling it.
   *
   * Measured against 16000 Hz on ctf5, the only capture without frame
   * headroom: 49.5 fps at 22050 versus 49.6 at 16000, i.e. inside run noise.
   * The cheaper rate buys nothing, so the port keeps the better one. */
  TW64_AUDIO_HZ = 22050,
  /* Eight simultaneous voices. Four tees firing, taking damage, hooking and
   * running over pickups do not come close in practice; the ceiling exists so
   * a pathological tick cannot starve the mixer.
   *
   * This is a measured cost knob, not a taste one: every allocated channel
   * costs CPU in mixer_poll_async() (it emits per-channel RSP commands) and
   * RSP time in the mixer ucode, whether or not it is playing. Twelve
   * channels cost ctf5 -- the only capture without frame headroom -- around
   * 8 fps; eight buys most of that back with no audible loss. */
  TW64_AUDIO_CHANNELS = 8
};

/* Queue depth. The AI carves ~20 ms buffers at this rate, so this is roughly
 * 120 ms of headroom: deep enough that a 16.7 ms frame (or a couple of late
 * ones) can never underrun, shallow enough that a gunshot is not audibly
 * behind its muzzle flash. Loads that block for longer than this do drop
 * samples; Tw64AudioUpdate() is called through the load path to keep those
 * gaps as short as the blocking calls themselves. */
#define TW64_AUDIO_LATENCY AUDIO_INIT_LATENCY_MS(120)

/* src/engine/client/sound.cpp: linear falloff, hard cut at m_MaxDistance, and
 * a constant-power horizontal pan over the same distance. CEventHandler::Snap
 * culls events past exactly this radius too, so the port's audible set matches
 * what the desktop client would have been sent. */
const float TW64_MAX_DISTANCE = 1500.0f;
/* CSounds' channel volumes: CHN_WORLD 0.9, CHN_GUI/CHN_GLOBAL 1.0. */
const float TW64_WORLD_VOLUME = 0.9f;
const float TW64_GLOBAL_VOLUME = 1.0f;
/* Master headroom. The RSP saturates its accumulator, so this is what keeps a
 * busy four-way firefight off the clipping rail rather than a limiter. */
const float TW64_MASTER_VOLUME = 0.4f;
/* Below this the voice would be inaudible; the desktop client still burns a
 * voice slot on it, the port declines to allocate a channel. */
const float TW64_MIN_AUDIBLE = 1.0f / 256.0f;

/* ------------------------------------------------------------------ */
/* Waveforms                                                          */
/* ------------------------------------------------------------------ */

/* One waveform per shipped sound set. The file name is the set name, so the
 * table below is the same mapping scripts/convert_n64_audio.py verifies
 * against the generated SOUND_* enum before it converts anything. */
struct CSoundEntry {
  int m_SoundID;
  const char *m_pName;
};

const CSoundEntry s_aSounds[] = {
    /* CGameContext::CreateSound() */
    {SOUND_GUN_FIRE, "gun_fire"},
    {SOUND_SHOTGUN_FIRE, "shotgun_fire"},
    {SOUND_GRENADE_FIRE, "grenade_fire"},
    {SOUND_HAMMER_FIRE, "hammer_fire"},
    {SOUND_NINJA_FIRE, "ninja_fire"},
    {SOUND_GRENADE_EXPLODE, "grenade_explode"},
    {SOUND_NINJA_HIT, "ninja_hit"},
    {SOUND_LASER_FIRE, "laser_fire"},
    {SOUND_LASER_BOUNCE, "laser_bounce"},
    {SOUND_WEAPON_SWITCH, "weapon_switch"},
    {SOUND_PLAYER_PAIN_SHORT, "player_pain_short"},
    {SOUND_PLAYER_PAIN_LONG, "player_pain_long"},
    {SOUND_PLAYER_DIE, "player_die"},
    {SOUND_PICKUP_HEALTH, "pickup_health"},
    {SOUND_PICKUP_ARMOR, "pickup_armor"},
    {SOUND_PICKUP_GRENADE, "pickup_grenade"},
    {SOUND_PICKUP_SHOTGUN, "pickup_shotgun"},
    {SOUND_PICKUP_NINJA, "pickup_ninja"},
    {SOUND_WEAPON_SPAWN, "weapon_spawn"},
    {SOUND_WEAPON_NOAMMO, "weapon_noammo"},
    {SOUND_HIT, "hit"},
    /* Derived by the shell from the same state the desktop client derives
     * them from: snapshot events, core events and flag state. */
    {SOUND_HAMMER_HIT, "hammer_hit"},
    {SOUND_PLAYER_SPAWN, "player_spawn"},
    {SOUND_PLAYER_JUMP, "player_jump"},
    {SOUND_PLAYER_AIRJUMP, "player_airjump"},
    {SOUND_HOOK_ATTACH_GROUND, "hook_attach_ground"},
    {SOUND_HOOK_ATTACH_PLAYER, "hook_attach_player"},
    {SOUND_HOOK_NOATTACH, "hook_noattach"},
    {SOUND_CTF_GRAB_PL, "ctf_grab_pl"},
    {SOUND_CTF_GRAB_EN, "ctf_grab_en"},
    {SOUND_CTF_CAPTURE, "ctf_capture"},
    {SOUND_CTF_RETURN, "ctf_return"},
    {SOUND_CTF_DROP, "ctf_drop"},
    /* Menu feedback */
    {SOUND_CHAT_CLIENT, "chat_client"},
    {SOUND_CHAT_HIGHLIGHT, "chat_highlight"}};

const int TW64_NUM_SOUNDS = (int)(sizeof(s_aSounds) / sizeof(s_aSounds[0]));

wav64_t *s_apWaves[TW64_NUM_SOUNDS];
/* SOUND_* -> index into s_aWaves, or -1 for a sound the port does not ship.
 * A flat table so an event lookup is one load, not a scan. */
int8_t s_aSoundSlot[NUM_SOUNDS];
bool s_Ready;

int s_NextChannel;
/* Two separate windows, because the two costs have different causes: the
 * per-frame mixer hand-off can block on the RSP, the per-tick event scan
 * cannot. Merging them would hide which one moved. */
uint64_t s_MixCycles;
uint32_t s_MixMaxCycles;
uint32_t s_MixCalls;
uint64_t s_TickCycles;
uint32_t s_TickMaxCycles;
uint32_t s_TickCalls;
uint32_t s_WindowVoices;

/* ------------------------------------------------------------------ */
/* Listeners                                                          */
/* ------------------------------------------------------------------ */

struct CListener {
  float m_X;
  float m_Y;
  int m_ClientID;
};

CListener s_aListeners[TW64_MAX_VIEWPORTS];
int s_NumListeners;

/* Same camera the renderer uses for this viewport, so what is heard and what
 * is on screen agree. */
void BuildListeners(CGameContext *pGameServer, const CTw64Viewport *pViewports,
                    int NumViewports) {
  s_NumListeners = 0;
  for (int i = 0; i < NumViewports && i < TW64_MAX_VIEWPORTS; ++i) {
    const int ClientID = pViewports[i].m_ClientID;
    const CCharacter *pCharacter = pGameServer->GetPlayerChar(ClientID);
    const CPlayer *pPlayer = pGameServer->m_apPlayers[ClientID];
    CListener &Listener = s_aListeners[s_NumListeners++];
    Listener.m_ClientID = ClientID;
    if (pCharacter) {
      Listener.m_X = pCharacter->GetPos().x;
      Listener.m_Y = pCharacter->GetPos().y;
    } else if (pPlayer) {
      Listener.m_X = pPlayer->m_ViewPos.x;
      Listener.m_Y = pPlayer->m_ViewPos.y;
    } else {
      Listener.m_X = 0.0f;
      Listener.m_Y = 0.0f;
    }
  }
}

/* ------------------------------------------------------------------ */
/* Playback                                                           */
/* ------------------------------------------------------------------ */

int PickChannel() {
  /* Prefer an idle channel, in round-robin order so a long sample is not
   * immediately reused; if every channel is busy, take the oldest allocation.
   * Fixed-size and allocation free, like every other runtime structure in the
   * port. */
  for (int i = 0; i < TW64_AUDIO_CHANNELS; ++i) {
    const int Channel = (s_NextChannel + i) % TW64_AUDIO_CHANNELS;
    if (!mixer_ch_playing(Channel)) {
      s_NextChannel = (Channel + 1) % TW64_AUDIO_CHANNELS;
      return Channel;
    }
  }
  const int Channel = s_NextChannel;
  s_NextChannel = (Channel + 1) % TW64_AUDIO_CHANNELS;
  return Channel;
}

void PlayOn(int Slot, float Left, float Right) {
  const int Channel = PickChannel();
  mixer_ch_set_vol(Channel, Left, Right);
  wav64_play(s_apWaves[Slot], Channel);
  ++s_WindowVoices;
}

int SoundSlot(int SoundID) {
  if (SoundID < 0 || SoundID >= NUM_SOUNDS)
    return -1;
  return s_aSoundSlot[SoundID];
}

/* Unattenuated, dead centre: the desktop client's CHN_GUI/CHN_GLOBAL. */
void PlayGlobal(int SoundID) {
  const int Slot = SoundSlot(SoundID);
  if (!s_Ready || Slot < 0)
    return;
  PlayOn(Slot, TW64_GLOBAL_VOLUME, TW64_GLOBAL_VOLUME);
}

/* The desktop world channel, one listener per viewport. Mixed at the loudest
 * listener's gain: a split-screen sound that is close to player two must not
 * be quiet because player one is far away, and the pan has to come from the
 * same listener or the two would disagree about which side it is on. */
void PlayAt(int SoundID, float X, float Y) {
  const int Slot = SoundSlot(SoundID);
  if (!s_Ready || Slot < 0)
    return;

  float BestLeft = 0.0f;
  float BestRight = 0.0f;
  for (int i = 0; i < s_NumListeners; ++i) {
    const float Dx = X - s_aListeners[i].m_X;
    const float Dy = Y - s_aListeners[i].m_Y;
    const float DistanceSq = Dx * Dx + Dy * Dy;
    if (DistanceSq >= TW64_MAX_DISTANCE * TW64_MAX_DISTANCE)
      continue;
    const float Falloff =
        1.0f - sqrtf(DistanceSq) * (1.0f / TW64_MAX_DISTANCE);
    const float Amplitude = TW64_WORLD_VOLUME * Falloff;
    /* Constant power over the same span, so a sound crossing the listener
     * keeps its loudness while it moves across the stereo field. */
    const float LeftPan = 0.5f - Dx * (0.5f / TW64_MAX_DISTANCE);
    const float Left = Amplitude * sqrtf(LeftPan);
    const float Right = Amplitude * sqrtf(1.0f - LeftPan);
    if (Left + Right > BestLeft + BestRight) {
      BestLeft = Left;
      BestRight = Right;
    }
  }

  if (BestLeft + BestRight < TW64_MIN_AUDIBLE)
    return;
  PlayOn(Slot, BestLeft, BestRight);
}

/* ------------------------------------------------------------------ */
/* Per-match trackers                                                 */
/* ------------------------------------------------------------------ */

/* CTF has no server sound at all: the desktop client derives the whole flag
 * funnel from the game data it is snapped. The shell derives it from the
 * authoritative flag entities and the controller's own per-player stats, which
 * is the same information one step earlier. */
struct CFlagTracker {
  int m_CarrierID;
  int m_Captures;
  int m_Returns;
};

CFlagTracker s_aFlagState[2];

/* The match's real player count. MAX_CLIENTS is 64 and CPlayerStats is a
 * couple of hundred bytes, so scanning the whole array once per tick pulled
 * ~13 KiB through the data cache for four players' worth of information. */
int s_NumPlayers;

int LocalTeam(CGameContext *pGameServer) {
  /* Which team "we" are, for grab_pl versus grab_en. With split screen the
   * viewports can be on opposite teams; viewport zero is the one the mix is
   * addressed to, exactly as the port's single-listener HUD ordering does. */
  if (!s_NumListeners)
    return TEAM_RED;
  const CPlayer *pPlayer = pGameServer->m_apPlayers[s_aListeners[0].m_ClientID];
  return pPlayer ? pPlayer->GetTeam() : TEAM_RED;
}

void ScanFlags(CGameContext *pGameServer) {
  int aCarrier[2] = {-1, -1};
  bool aPresent[2] = {false, false};

  for (CEntity *pEntity =
           pGameServer->m_World.FindFirst(CGameWorld::ENTTYPE_FLAG);
       pEntity; pEntity = pEntity->TypeNext()) {
    CFlag *pFlag = static_cast<CFlag *>(pEntity);
    const int Team = pFlag->GetTeam();
    if (Team < 0 || Team > 1)
      continue;
    aPresent[Team] = true;
    CCharacter *pCarrier = pFlag->GetCarrier();
    if (pCarrier && pCarrier->GetPlayer())
      aCarrier[Team] = pCarrier->GetPlayer()->GetCID();
  }

  /* Captures and returns are counted per player by the controller, so a
   * delta over the whole roster is the exact event count for this tick. */
  int Captures = 0;
  int Returns = 0;
  for (int i = 0; i < s_NumPlayers; ++i) {
    const CGameContext::CPlayerStats &Stats = pGameServer->m_aPlayerStats[i];
    Captures += Stats.m_FlagCaptures;
    Returns += Stats.m_FlagReturns;
  }

  const int WasCaptures = s_aFlagState[0].m_Captures;
  const int WasReturns = s_aFlagState[0].m_Returns;
  s_aFlagState[0].m_Captures = Captures;
  s_aFlagState[0].m_Returns = Returns;

  if (Captures > WasCaptures)
    PlayGlobal(SOUND_CTF_CAPTURE);
  if (Returns > WasReturns)
    PlayGlobal(SOUND_CTF_RETURN);

  const int Local = LocalTeam(pGameServer);
  for (int Team = 0; Team < 2; ++Team) {
    if (!aPresent[Team])
      continue;
    const int Was = s_aFlagState[Team].m_CarrierID;
    const int Now = aCarrier[Team];
    s_aFlagState[Team].m_CarrierID = Now;
    if (Now >= 0 && Now != Was) {
      /* A grab of the flag whose team is not ours is our team scoring. */
      const CPlayer *pGrabber = pGameServer->m_apPlayers[Now];
      const bool Friendly = pGrabber && pGrabber->GetTeam() == Local;
      PlayGlobal(Friendly ? SOUND_CTF_GRAB_PL : SOUND_CTF_GRAB_EN);
    } else if (Was >= 0 && Now < 0 && Captures == WasCaptures &&
               Returns == WasReturns) {
      /* Carrier gone without a capture or a return: the flag was dropped. */
      PlayGlobal(SOUND_CTF_DROP);
    }
  }
}

/* ------------------------------------------------------------------ */
/* Event sources                                                      */
/* ------------------------------------------------------------------ */

void ScanEvents(CGameContext *pGameServer) {
  const CEventHandler &Events = pGameServer->m_Events;
  const int Num = Events.NumEvents();
  for (int i = 0; i < Num; ++i) {
    const CNetEvent_Common *pCommon =
        (const CNetEvent_Common *)Events.EventData(i);
    const float X = (float)pCommon->m_X;
    const float Y = (float)pCommon->m_Y;
    switch (Events.EventType(i)) {
    case NETEVENTTYPE_SOUNDWORLD: {
      /* Vanilla addresses a few sounds to specific clients (the attacker's
       * hit feedback). Play those only when a local viewport is addressed. */
      const int64 Mask = Events.EventMask(i);
      if (Mask != -1) {
        bool Local = false;
        for (int v = 0; v < s_NumListeners && !Local; ++v)
          Local = CmaskIsSet(Mask, s_aListeners[v].m_ClientID);
        if (!Local)
          break;
      }
      const CNetEvent_SoundWorld *pSound =
          (const CNetEvent_SoundWorld *)pCommon;
      PlayAt(pSound->m_SoundID, X, Y);
      break;
    }
    case NETEVENTTYPE_HAMMERHIT:
      PlayAt(SOUND_HAMMER_HIT, X, Y);
      break;
    case NETEVENTTYPE_SPAWN:
      PlayAt(SOUND_PLAYER_SPAWN, X, Y);
      break;
    default:
      break;
    }
  }
}

void ScanCoreEvents(CGameContext *pGameServer) {
  for (int i = 0; i < s_NumPlayers; ++i) {
    CCharacter *pCharacter = pGameServer->GetPlayerChar(i);
    if (!pCharacter)
      continue;
    const int Triggered = pCharacter->GetCore().m_TriggeredEvents;
    if (!Triggered)
      continue;
    const vec2 Pos = pCharacter->GetPos();
    if (Triggered & COREEVENTFLAG_GROUND_JUMP)
      PlayAt(SOUND_PLAYER_JUMP, Pos.x, Pos.y);
    if (Triggered & COREEVENTFLAG_AIR_JUMP)
      PlayAt(SOUND_PLAYER_AIRJUMP, Pos.x, Pos.y);
    if (Triggered & COREEVENTFLAG_HOOK_ATTACH_GROUND)
      PlayAt(SOUND_HOOK_ATTACH_GROUND, Pos.x, Pos.y);
    if (Triggered & COREEVENTFLAG_HOOK_ATTACH_PLAYER)
      PlayAt(SOUND_HOOK_ATTACH_PLAYER, Pos.x, Pos.y);
    if (Triggered & COREEVENTFLAG_HOOK_HIT_NOHOOK)
      PlayAt(SOUND_HOOK_NOATTACH, Pos.x, Pos.y);
  }
}

} // namespace

/* ------------------------------------------------------------------ */
/* Entry points                                                       */
/* ------------------------------------------------------------------ */

void Tw64AudioInit(void) {
  audio_init(TW64_AUDIO_HZ, TW64_AUDIO_LATENCY);
  mixer_init(TW64_AUDIO_CHANNELS);
  mixer_set_vol(TW64_MASTER_VOLUME);

  for (int i = 0; i < NUM_SOUNDS; ++i)
    s_aSoundSlot[i] = -1;

  int Frames = 0;
  for (int i = 0; i < TW64_NUM_SOUNDS; ++i) {
    char aPath[64];
    str_format(aPath, sizeof(aPath), "rom:/sfx/%s.wav64", s_aSounds[i].m_pName);
    /* Resident, not streamed. The whole set is 204 KiB of VADPCM, and
     * holding it in RDRAM removes one PI transfer and one CPU-side decode
     * callback per voice per mixer poll. Measured on ctf5, the only capture
     * without frame headroom: 47.7 fps streaming from ROM versus 49.5 fps
     * resident, for 204 KiB of heap. Nothing is allocated after this loop. */
    wav64_loadparms_t Parms;
    Parms.streaming_mode = WAV64_STREAMING_NONE;
    s_apWaves[i] = wav64_load(aPath, &Parms);
    s_aSoundSlot[s_aSounds[i].m_SoundID] = (int8_t)i;
    Frames += s_apWaves[i]->wave.len;
  }
  s_Ready = true;
  s_NextChannel = 0;

  debugf("TW64 AUDIO_OK rate=%d channels=%d sounds=%d frames=%d "
         "buffers=%d buffer_len=%d\n",
         TW64_AUDIO_HZ, (int)TW64_AUDIO_CHANNELS, TW64_NUM_SOUNDS, Frames,
         audio_get_num_buffers(), audio_get_buffer_length());
}

void Tw64AudioUpdate(void) {
  if (!s_Ready)
    return;
  const uint32_t Begin = TICKS_READ();
  mixer_try_play();
  const uint32_t Elapsed = (uint32_t)TICKS_SINCE(Begin);
  s_MixCycles += Elapsed;
  if (Elapsed > s_MixMaxCycles)
    s_MixMaxCycles = Elapsed;
  ++s_MixCalls;
}

void Tw64AudioResetMatch(CGameContext *pGameServer) {
  if (!s_Ready)
    return;
  for (int i = 0; i < TW64_AUDIO_CHANNELS; ++i)
    mixer_ch_stop(i);
  s_NextChannel = 0;
  mem_zero(s_aFlagState, sizeof(s_aFlagState));
  s_aFlagState[0].m_CarrierID = -1;
  s_aFlagState[1].m_CarrierID = -1;
  if (pGameServer) {
    /* Adopt the current counters instead of zero, so restarting into a
     * context that has already scored cannot replay a burst of captures. */
    int Captures = 0;
    int Returns = 0;
    for (int i = 0; i < MAX_CLIENTS; ++i) {
      Captures += pGameServer->m_aPlayerStats[i].m_FlagCaptures;
      Returns += pGameServer->m_aPlayerStats[i].m_FlagReturns;
    }
    /* Once per match, so the full-roster scan is fine here. */
    s_aFlagState[0].m_Captures = Captures;
    s_aFlagState[0].m_Returns = Returns;
  }
}

void Tw64AudioTick(CGameContext *pGameServer, const CTw64Viewport *pViewports,
                   int NumViewports, int NumPlayers, bool FlagMode) {
  if (!s_Ready || !pGameServer)
    return;
  const uint32_t Begin = TICKS_READ();

  s_NumPlayers = NumPlayers < MAX_CLIENTS ? NumPlayers : (int)MAX_CLIENTS;
  BuildListeners(pGameServer, pViewports, NumViewports);
  ScanEvents(pGameServer);
  ScanCoreEvents(pGameServer);
  if (FlagMode)
    ScanFlags(pGameServer);

  const uint32_t Elapsed = (uint32_t)TICKS_SINCE(Begin);
  s_TickCycles += Elapsed;
  if (Elapsed > s_TickMaxCycles)
    s_TickMaxCycles = Elapsed;
  ++s_TickCalls;
}

void Tw64AudioMenu(int Kind) {
  PlayGlobal(Kind == TW64_MENU_SOUND_CONFIRM ? SOUND_CHAT_HIGHLIGHT
                                             : SOUND_CHAT_CLIENT);
}

void Tw64AudioTakeWindow(CTw64AudioStats *pStats) {
  /* TICKS_PER_SECOND is 46.875 MHz => microseconds = cycles * 8 / 375. */
  const uint32_t MixAvg =
      s_MixCalls ? (uint32_t)(s_MixCycles / s_MixCalls) : 0;
  const uint32_t TickAvg =
      s_TickCalls ? (uint32_t)(s_TickCycles / s_TickCalls) : 0;
  pStats->m_MixAvgUs = (uint32_t)(((uint64_t)MixAvg * 8ULL) / 375ULL);
  pStats->m_MixMaxUs = (uint32_t)(((uint64_t)s_MixMaxCycles * 8ULL) / 375ULL);
  pStats->m_TickAvgUs = (uint32_t)(((uint64_t)TickAvg * 8ULL) / 375ULL);
  pStats->m_TickMaxUs = (uint32_t)(((uint64_t)s_TickMaxCycles * 8ULL) / 375ULL);
  pStats->m_Voices = s_WindowVoices;
  s_MixCycles = 0;
  s_MixMaxCycles = 0;
  s_MixCalls = 0;
  s_TickCycles = 0;
  s_TickMaxCycles = 0;
  s_TickCalls = 0;
  s_WindowVoices = 0;
}
