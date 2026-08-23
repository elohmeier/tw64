/* Teeworlds 64: sound.
 *
 * The desktop client's own samples, played back through libdragon's RSP mixer.
 * This layer is strictly presentation: it only ever *reads* authoritative
 * state that the simulation has already produced, and nothing it computes is
 * fed back into a tick, an input or a bot observation. The port's determinism
 * evidence is therefore unaffected by whether audio is enabled at all.
 *
 * The event sources mirror the desktop client one for one:
 *
 *   NETEVENTTYPE_SOUNDWORLD   every CGameContext::CreateSound(), with the
 *                             sound id and the client mask the desktop client
 *                             would have received
 *   NETEVENTTYPE_HAMMERHIT    hammer connect
 *   NETEVENTTYPE_SPAWN        respawn
 *   CCharacterCore events     jump, air jump, hook attach/miss
 *   CFlag / flag stats        CTF grab, capture, return, drop
 *
 * Spatialisation follows src/engine/client/sound.cpp: linear falloff to zero
 * at 1500 world units, constant-power horizontal pan, world channel at 0.9.
 * With split screen there is more than one listener, so a sound is mixed at
 * the loudest viewport's gain and that viewport's pan. */
#ifndef TW64_GAME_TW_AUDIO_H
#define TW64_GAME_TW_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

class CGameContext;
struct CTw64Viewport;

enum {
  /* Menu feedback. Two short desktop UI blips, not music. */
  TW64_MENU_SOUND_MOVE = 0,
  TW64_MENU_SOUND_CONFIRM
};

/* Brings up the AI and the mixer and opens every waveform in the ROM
 * filesystem. Must run after dfs_init(). Only the game-family ROMs call it;
 * the deterministic simulation ROMs have no audio at all, so their timing
 * evidence is untouched. */
void Tw64AudioInit(void);

/* Hands the mixer to the RSP and tops up the AI queue. Cheap and idempotent;
 * call it once per main-loop iteration and around anything that blocks for
 * longer than the AI queue is deep (map loads), or the AI runs dry and the
 * output stream develops a hole. */
void Tw64AudioUpdate(void);

/* Silences every voice and drops the per-match trackers. Call on match start
 * and match end so a stale carrier or stat counter cannot fire a phantom
 * sound into the next match. */
void Tw64AudioResetMatch(CGameContext *pGameServer);

/* Turns one simulated tick into sound. Call immediately after
 * CGameContext::OnTick() and before the shell closes the shared presentation
 * event ring through CGameContext::OnPostSnap(). */
void Tw64AudioTick(CGameContext *pGameServer, const CTw64Viewport *pViewports,
                   int NumViewports, int NumPlayers, bool FlagMode);

/* Menu blip. Unattenuated, like the desktop GUI channel. */
void Tw64AudioMenu(int Kind);

/* Accumulated audio cost since the last call, for GAME_STAT. The mixer
 * hand-off and the per-tick event scan are reported separately: only the
 * former can block on the RSP, so merging them would hide which one moved. */
struct CTw64AudioStats {
  uint32_t m_MixAvgUs;  /* per Tw64AudioUpdate(), i.e. per rendered frame */
  uint32_t m_MixMaxUs;
  uint32_t m_TickAvgUs; /* per Tw64AudioTick(), i.e. per simulated tick */
  uint32_t m_TickMaxUs;
  uint32_t m_Voices;    /* voices started in the window */
};

void Tw64AudioTakeWindow(CTw64AudioStats *pStats);

#endif
