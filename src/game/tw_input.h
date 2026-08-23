/* Teeworlds 64: joypad -> CNetObj_PlayerInput.
 *
 * Human players are ordinary clients of CGameContext: the struct produced
 * here goes through exactly the same CPlayer::OnPredictedInput() /
 * OnDirectInput() path that the bot seam uses, so no gameplay code learns
 * that a controller exists. */
#ifndef TW64_GAME_TW_INPUT_H
#define TW64_GAME_TW_INPUT_H

#include <joypad.h>

#include <generated/protocol.h>

/* Per-human persistent input state. Fixed size, no allocation. */
struct CTw64HumanInput {
  /* Aim vector in world units; persists while the stick is neutral. */
  int m_AimX;
  int m_AimY;
  /* Teeworlds transmits fire and weapon switching as wrapping counters. */
  int m_FireCounter;
  int m_NextWeaponCounter;
};

void Tw64InputReset(CTw64HumanInput *pState);

/* The control mapping proper, separated from the joypad read so it can be
 * exercised with synthetic values on target (see Tw64InputSelfTest). Stick
 * axes use the controller convention: +Y is up. */
void Tw64InputApply(CTw64HumanInput *pState, joypad_buttons_t Buttons,
                    int StickX, int StickY, CNetObj_PlayerInput *pOut);

/* Reads controller `Port` (already polled this frame) and fills `pOut`.
 * `Port` < 0 produces a neutral input. */
void Tw64InputPoll(CTw64HumanInput *pState, int Port,
                   CNetObj_PlayerInput *pOut);

/* Drives Tw64InputApply with synthetic controller states and checks the v1
 * mapping. Returns a bitmask of failed cases, 0 when the mapping is correct.
 * Autoplay ROMs never touch a real controller, so without this the whole human
 * input path would ship unexecuted. */
unsigned Tw64InputSelfTest(void);

/* Keeps the counters of an externally produced input (autoplay bot driver)
 * consistent with the human path so the character sees the same edges. */
void Tw64InputAdoptCounters(CTw64HumanInput *pState,
                            const CNetObj_PlayerInput *pInput);

#endif
