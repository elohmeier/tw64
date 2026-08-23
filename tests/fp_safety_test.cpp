#include <limits>
#include <stdio.h>

#include "src/platform/tw_fp_safety.h"

namespace {
bool ExpectSweepEnd(const char *pName, float Distance, int Expected) {
  const int Actual = Tw64CollisionSweepEnd(Distance);
  if (Actual == Expected)
    return true;
  fprintf(stderr, "FAIL: %s: expected %d, got %d\n", pName, Expected, Actual);
  return false;
}
} // namespace

int main() {
  bool Ok = true;
  Ok &= ExpectSweepEnd("zero distance", 0.0f, 0);
  Ok &= ExpectSweepEnd("negative zero distance", -0.0f, 0);
  Ok &= ExpectSweepEnd("negative distance", -1.0f, 0);
  Ok &= ExpectSweepEnd("NaN distance", std::numeric_limits<float>::quiet_NaN(),
                       0);
  Ok &= ExpectSweepEnd("fractional movement", 0.25f, 1);
  Ok &= ExpectSweepEnd("one unit movement", 1.0f, 2);
  Ok &= ExpectSweepEnd("tile-sized movement", 32.0f, 33);
  return Ok ? 0 : 1;
}
