/* Teeworlds 64: build-variant selector.
 *
 * This is the only translation unit that depends on TW64_AUTOPLAY_MODE, so
 * switching between the interactive ROM and the unattended autoplay ROMs only
 * recompiles this file and relinks. See n64/rom.mk. */

#ifndef TW64_AUTOPLAY_MODE
#define TW64_AUTOPLAY_MODE 0
#endif

const int g_Tw64AutoplayMode = TW64_AUTOPLAY_MODE;
