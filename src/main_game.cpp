// Teeworlds 64 game entry point: boot, init subsystems, report readiness over
// IS Viewer, then hand control to the playable shell.
//
// The deterministic-simulation ROMs use n64/src/main.cpp instead; both share
// the same FPU configuration so gameplay arithmetic matches the host.
#include <libdragon.h>

#include <cop1.h>

#include "game/tw_audio.h"
#include "game/tw_game.h"

namespace {

// The host reference runs on x86 SSE with every floating point exception
// masked. libdragon enables overflow/div0/invalid traps in debug builds, so
// force the IEEE default masking here. The VR4300's unimplemented-operation
// trap cannot be masked: target code must never feed NaNs or denormals into a
// computational instruction. FS handles denormalized results only.
void ConfigureFpuForIeeeDefaults() {
  uint32_t Fcr31 = C1_FCR31();
  Fcr31 &= ~C1_ENABLE_MASK;
  Fcr31 &= ~C1_CAUSE_MASK;
  Fcr31 |= C1_FCR31_FS;
  C1_WRITE_FCR31(Fcr31);
}

} // namespace

int main(void) {
  debug_init_isviewer();
  debug_init_usblog();
  // Three buffers: the simulation and the renderer run at different rates, so
  // a spare buffer keeps a late frame from stalling the fixed-rate tick.
  display_init(RESOLUTION_320x240, DEPTH_16_BPP, 3, GAMMA_NONE,
               FILTERS_RESAMPLE);
  dfs_init(DFS_DEFAULT_LOCATION);
  joypad_init();
  rdpq_init();
  ConfigureFpuForIeeeDefaults();
  /* Sound is game-ROM only: the deterministic simulation ROMs use main.cpp
   * and stay silent, so their timing and hash evidence is untouched. The AI
   * queue is primed here so the stream is already running by the time the
   * first frame is presented. */
  Tw64AudioInit();
  Tw64AudioUpdate();
  debugf("TW64 BOOT_OK memory=%d autoplay=%d\n", get_memory_size(),
         g_Tw64AutoplayMode);

  Tw64RunGame();

  while (1)
    ;
}
