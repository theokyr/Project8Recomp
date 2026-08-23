// C23 math functions that Microsoft's UCRT does not provide.
//
// Windows only, and only because of a specific interaction: SIMDe (pulled in by
// the SDK) uses __builtin_roundevenf when the compiler advertises it, clang
// does, and clang lowers that builtin to a libm call named `roundevenf` when it
// cannot fold it. glibc has that function; the UCRT does not, so the whole game
// links on Linux and fails on Windows with one undefined symbol out of 62
// translation units.
//
// Round-half-to-even, computed rather than delegated to nearbyint, so the
// result does not depend on the current floating-point rounding mode. That
// matters here: this is in the path of recompiled guest arithmetic, and a
// rounding mode set by guest code must not change what this returns.

#if defined(_WIN32)

#include <cmath>

namespace {

template <typename T>
T RoundHalfToEven(T x) {
  // Leaves NaN, infinities and already-integral values alone. std::trunc on an
  // infinity returns the infinity, so the comparison below is never reached
  // with a meaningless difference.
  if (!std::isfinite(x)) {
    return x;
  }
  const T truncated = std::trunc(x);
  const T fraction = std::fabs(x - truncated);

  if (fraction > T(0.5)) {
    return truncated + std::copysign(T(1), x);
  }
  if (fraction < T(0.5)) {
    return truncated;
  }
  // Exactly halfway: take whichever neighbour is even. `truncated` is already
  // an integer, so halving it and testing for integrality is an exact test.
  const T half = truncated / T(2);
  return (half == std::trunc(half)) ? truncated : truncated + std::copysign(T(1), x);
}

}  // namespace

extern "C" {

float roundevenf(float x) { return RoundHalfToEven(x); }
double roundeven(double x) { return RoundHalfToEven(x); }
long double roundevenl(long double x) { return RoundHalfToEven(x); }

}  // extern "C"

#endif  // _WIN32
