/* Teeworlds 64: joypad -> CNetObj_PlayerInput. See tw_input.h. */

#include <math.h>
#include <string.h>

#include <joypad.h>

#include <base/system.h>

#include "tw_input.h"

namespace {

/* Below this the stick is treated as centred for aiming, so a resting stick
 * never spins the crosshair. */
const int AIM_DEADZONE = 12;
/* Movement needs a firmer push than aiming: the same stick does both, and a
 * player nudging the crosshair should not start walking. */
const int MOVE_DEADZONE = 40;
/* Teeworlds aim vectors are world offsets from the tee; ~120 units is the
 * length the desktop client produces at a comfortable mouse distance. */
const float AIM_LENGTH = 120.0f;

/* Same wrapping-counter convention the bot seam uses (see SetInputButton in
 * src/game/server/bot.cpp). */
void SetInputButton(int *pCounter, bool Pressed) {
  if (((*pCounter & 1) != 0) != Pressed)
    *pCounter = (*pCounter + 1) & INPUT_STATE_MASK;
}

} // namespace

void Tw64InputReset(CTw64HumanInput *pState) {
  memset(pState, 0, sizeof(*pState));
  pState->m_AimX = (int)AIM_LENGTH;
  pState->m_AimY = 0;
}

void Tw64InputApply(CTw64HumanInput *pState, joypad_buttons_t Buttons,
                    int StickX, int StickY, CNetObj_PlayerInput *pOut) {
  memset(pOut, 0, sizeof(*pOut));

  /* Stick y is positive upwards on the controller and downwards in world
   * space. */
  const int AimY = -StickY;

  if (StickX * StickX + AimY * AimY >= AIM_DEADZONE * AIM_DEADZONE) {
    const float Length = sqrtf((float)(StickX * StickX + AimY * AimY));
    pState->m_AimX = (int)((StickX / Length) * AIM_LENGTH);
    pState->m_AimY = (int)((AimY / Length) * AIM_LENGTH);
  }

  int Direction = 0;
  if (Buttons.d_left || StickX <= -MOVE_DEADZONE)
    Direction -= 1;
  if (Buttons.d_right || StickX >= MOVE_DEADZONE)
    Direction += 1;

  SetInputButton(&pState->m_FireCounter, Buttons.b != 0);
  SetInputButton(&pState->m_NextWeaponCounter, Buttons.r != 0);

  pOut->m_Direction = Direction;
  pOut->m_TargetX = pState->m_AimX;
  pOut->m_TargetY = pState->m_AimY;
  pOut->m_Jump = Buttons.a ? 1 : 0;
  pOut->m_Fire = pState->m_FireCounter;
  pOut->m_Hook = Buttons.z ? 1 : 0;
  pOut->m_NextWeapon = pState->m_NextWeaponCounter;
}

void Tw64InputPoll(CTw64HumanInput *pState, int Port,
                   CNetObj_PlayerInput *pOut) {
  if (Port < 0 || Port >= JOYPAD_PORT_COUNT) {
    joypad_buttons_t Neutral;
    memset(&Neutral, 0, sizeof(Neutral));
    Tw64InputApply(pState, Neutral, 0, 0, pOut);
    return;
  }

  const joypad_inputs_t Inputs = joypad_get_inputs((joypad_port_t)Port);
  Tw64InputApply(pState, Inputs.btn, Inputs.stick_x, Inputs.stick_y, pOut);
}

unsigned Tw64InputSelfTest(void) {
  CTw64HumanInput State;
  CNetObj_PlayerInput Out;
  joypad_buttons_t Buttons;
  unsigned Failures = 0;

  /* 1: a resting controller walks nowhere and keeps the default aim. */
  Tw64InputReset(&State);
  memset(&Buttons, 0, sizeof(Buttons));
  Tw64InputApply(&State, Buttons, 0, 0, &Out);
  if (Out.m_Direction != 0 || Out.m_Jump || Out.m_Hook || (Out.m_Fire & 1) ||
      Out.m_TargetX != (int)AIM_LENGTH || Out.m_TargetY != 0)
    Failures |= 1u << 0;

  /* 2: stick hard left aims left and moves left. */
  Tw64InputApply(&State, Buttons, -80, 0, &Out);
  if (Out.m_Direction != -1 || Out.m_TargetX > -100 || Out.m_TargetY != 0)
    Failures |= 1u << 1;

  /* 3: stick hard up aims up (negative world y) without moving. */
  Tw64InputApply(&State, Buttons, 0, 80, &Out);
  if (Out.m_Direction != 0 || Out.m_TargetY > -100 || Out.m_TargetX != 0)
    Failures |= 1u << 2;

  /* 4: a small stick deflection aims but does not walk. */
  Tw64InputApply(&State, Buttons, MOVE_DEADZONE - 5, 0, &Out);
  if (Out.m_Direction != 0 || Out.m_TargetX != (int)AIM_LENGTH)
    Failures |= 1u << 3;

  /* 5: the D-pad moves regardless of the stick. */
  memset(&Buttons, 0, sizeof(Buttons));
  Buttons.d_right = 1;
  Tw64InputApply(&State, Buttons, 0, 0, &Out);
  if (Out.m_Direction != 1)
    Failures |= 1u << 4;

  /* 6: A jumps, Z hooks. */
  memset(&Buttons, 0, sizeof(Buttons));
  Buttons.a = 1;
  Buttons.z = 1;
  Tw64InputApply(&State, Buttons, 0, 0, &Out);
  if (!Out.m_Jump || !Out.m_Hook)
    Failures |= 1u << 5;

  /* 7: B is an edge-counted press, not a level, and releasing advances the
   * counter to even again -- this is the convention CCharacter::FireWeapon
   * counts presses with. */
  memset(&Buttons, 0, sizeof(Buttons));
  Buttons.b = 1;
  Tw64InputApply(&State, Buttons, 0, 0, &Out);
  const int FireDown = Out.m_Fire;
  Tw64InputApply(&State, Buttons, 0, 0, &Out);
  if (!(FireDown & 1) || Out.m_Fire != FireDown)
    Failures |= 1u << 6;
  memset(&Buttons, 0, sizeof(Buttons));
  Tw64InputApply(&State, Buttons, 0, 0, &Out);
  if (Out.m_Fire & 1)
    Failures |= 1u << 7;

  /* 8: R advances the weapon counter on the press edge only. */
  memset(&Buttons, 0, sizeof(Buttons));
  Buttons.r = 1;
  Tw64InputApply(&State, Buttons, 0, 0, &Out);
  const int WeaponDown = Out.m_NextWeapon;
  Tw64InputApply(&State, Buttons, 0, 0, &Out);
  if (!(WeaponDown & 1) || Out.m_NextWeapon != WeaponDown)
    Failures |= 1u << 8;

  return Failures;
}

void Tw64InputAdoptCounters(CTw64HumanInput *pState,
                            const CNetObj_PlayerInput *pInput) {
  pState->m_FireCounter = pInput->m_Fire;
  pState->m_NextWeaponCounter = pInput->m_NextWeapon;
  if (pInput->m_TargetX || pInput->m_TargetY) {
    pState->m_AimX = pInput->m_TargetX;
    pState->m_AimY = pInput->m_TargetY;
  }
}
