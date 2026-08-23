/* Teeworlds 64: on-target deterministic match harness.
 *
 * Runs the same fixed scenario that src/tools/botbench.cpp RunMatch() runs on
 * the host, and reports the identical incremental state hash so host and
 * target can be compared checkpoint by checkpoint. */
#ifndef TW64_SIM_TW_MATCH_H
#define TW64_SIM_TW_MATCH_H

#ifdef __cplusplus
extern "C" {
#endif

/* Called back every TW64_HASH_INTERVAL ticks and once at the end so the caller
 * can keep the video output alive while the simulation runs. */
typedef void (*TW64_MATCH_PROGRESS)(int Tick, int TotalTicks);

/* Returns 0 on success, non-zero on setup failure. */
int Tw64RunScenario(TW64_MATCH_PROGRESS pfnProgress);

#ifdef __cplusplus
}
#endif

#endif
