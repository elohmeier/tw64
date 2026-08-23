/* Teeworlds 64: per-player controller rumble.
 *
 * Like tw_audio, this is a presentation-only consumer of authoritative game
 * state. It never changes an input, simulation object or bot observation.
 * Effects are scheduled per local-player port and reduced to binary motor
 * transitions because that is the interface exposed by an N64 Rumble Pak. */
#ifndef TW64_GAME_TW_RUMBLE_H
#define TW64_GAME_TW_RUMBLE_H

#include <stdbool.h>
#include <stdint.h>

class CGameContext;

/* Initializes the fixed-size scheduler and validates its pure pattern logic.
 * Must run after joypad_init(). */
void Tw64RumbleInit(void);

/* Clears pending effects, adopts the new match's cumulative stat counters and
 * selects the local client/port range. Autoplay still runs the logical
 * scheduler for performance evidence, but AllowHardware=false prevents motor
 * activation during unattended runs. */
void Tw64RumbleResetMatch(CGameContext *pGameServer, int NumHumans,
                          bool FlagMode, bool AllowHardware);

/* Consumes one completed simulation tick. Call after CGameContext::OnTick()
 * and before CGameContext::OnPostSnap(), while the tick's event ring exists. */
void Tw64RumbleTick(CGameContext *pGameServer);

/* Advances wall-clock pulse patterns and applies only changed motor states.
 * Call regularly from gameplay, menu and end-screen loops. */
void Tw64RumbleUpdate(void);

/* Immediately requests motor-off on every supported port and clears all
 * pending effects. Safe to call even when no accessory is present. */
void Tw64RumbleStopAll(void);

/* Current dynamic capability, for the menu footer and diagnostics. */
unsigned Tw64RumbleSupportedMask(void);
int Tw64RumbleSupportedCount(void);

/* Synthetic scheduler test. Does not touch Joybus or a physical motor. */
unsigned Tw64RumbleSelfTest(void);

struct CTw64RumbleStats {
  uint32_t m_TickAvgUs;
  uint32_t m_TickMaxUs;
  uint32_t m_UpdateAvgUs;
  uint32_t m_UpdateMaxUs;
  uint32_t m_Requests;
  uint32_t m_Ignored;
  uint32_t m_Transitions;
  uint32_t m_Writes;
  unsigned m_SupportedMask;
  unsigned m_ActiveMask;
};

/* Accumulated presentation cost and activity since the last call. */
void Tw64RumbleTakeWindow(CTw64RumbleStats *pStats);

#endif
