#ifndef TW_FP_SAFETY_H
#define TW_FP_SAFETY_H

/* Upstream runs one collision-sweep iteration for a stationary core, making
 * its interpolation factor 0/0. x86 carries that NaN through comparisons;
 * the VR4300 traps unconditionally when the next arithmetic operation reads
 * it. Skip the sweep when it has no distance to cover. */
static inline int Tw64CollisionSweepEnd(float Distance) {
  return Distance > 0.0f ? (int)(Distance + 1.0f) : 0;
}

#endif
