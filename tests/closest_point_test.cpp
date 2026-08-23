#include <cmath>
#include <cstdio>

#include "src/base/vmath.h"

namespace {
bool ExpectPoint(const char *pName, vec2 Actual, vec2 Expected) {
  if (std::isfinite(Actual.x) && std::isfinite(Actual.y) && Actual == Expected)
    return true;
  std::fprintf(stderr, "FAIL: %s: expected (%g, %g), got (%g, %g), finite=%d\n",
               pName, Expected.x, Expected.y, Actual.x, Actual.y,
               std::isfinite(Actual.x) && std::isfinite(Actual.y));
  return false;
}
} // namespace

int main() {
  bool Ok = true;
  Ok &= ExpectPoint("zero-length projectile segment",
                    closest_point_on_line(vec2(772.450012207f, 901.228088379f),
                                          vec2(772.450012207f, 901.228088379f),
                                          vec2(875.549987793f, 96.7719116211f)),
                    vec2(772.450012207f, 901.228088379f));
  Ok &= ExpectPoint("signed-zero segment",
                    closest_point_on_line(vec2(0.0f, -0.0f), vec2(-0.0f, 0.0f),
                                          vec2(4.0f, 8.0f)),
                    vec2(0.0f, -0.0f));
  Ok &= ExpectPoint("ordinary segment",
                    closest_point_on_line(vec2(0.0f, 0.0f), vec2(10.0f, 0.0f),
                                          vec2(3.0f, 4.0f)),
                    vec2(3.0f, 0.0f));
  return Ok ? 0 : 1;
}
