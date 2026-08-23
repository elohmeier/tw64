// Teeworlds 64 entry point: boot, init subsystems, report readiness over
// IS Viewer, then run the deterministic bot-match harness on the target.
#include <libdragon.h>

#include <cop1.h>

#include "sim/tw_match.h"

namespace {

const char *g_pStatus = "booting...";
char g_aProgress[32] = "";

void DrawScreen() {
  surface_t *pDisplay = display_get();
  graphics_fill_screen(pDisplay, graphics_make_color(24, 28, 40, 255));
  graphics_set_color(graphics_make_color(255, 255, 255, 255), 0);
  graphics_draw_text(pDisplay, 108, 100, "TEEWORLDS 64");
  graphics_draw_text(pDisplay, 88, 116, g_pStatus);
  if (g_aProgress[0])
    graphics_draw_text(pDisplay, 88, 132, g_aProgress);
  display_show(pDisplay);
}

void OnProgress(int Tick, int TotalTicks) {
  snprintf(g_aProgress, sizeof(g_aProgress), "tick %d / %d", Tick, TotalTicks);
  DrawScreen();
}

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
  display_init(RESOLUTION_320x240, DEPTH_16_BPP, 2, GAMMA_NONE,
               FILTERS_RESAMPLE);
  dfs_init(DFS_DEFAULT_LOCATION);
  joypad_init();
  rdpq_init();
  ConfigureFpuForIeeeDefaults();
  debugf("TW64 BOOT_OK memory=%d\n", get_memory_size());

  // Get the video pipeline running before the simulation takes over the CPU so
  // the emulator capture always has a live signal.
  for (int Frame = 0; Frame < 3; ++Frame)
    DrawScreen();
  debugf("TW64 RENDER_OK\n");

  g_pStatus = "simulating...";
  int Result = Tw64RunScenario(OnProgress);
  g_pStatus = Result == 0 ? "sim complete" : "sim failed";
  debugf("TW64 SIM_EXIT status=%d\n", Result);

  while (1)
    DrawScreen();
}
