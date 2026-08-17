/* 128-bit integer division helpers, for cross-compiling to the MSVC ABI.
 *
 * clang lowers `unsigned __int128` division to calls into compiler-rt
 * (__udivti3 and friends). Linux distributions ship compiler-rt built for
 * Linux only, so a Linux host cross-compiling to windows-msvc has no Windows
 * copy to link against and the whole SDK fails at link with one undefined
 * symbol - from a single 128-bit divide in the runtime's clock code.
 *
 * These are implemented with 64-bit operations only. Using `/` or `%` on a
 * 128-bit value here would compile straight back into a call to the function
 * being defined.
 *
 * Binary long division: 128 iterations, no lookup tables, no edge cases beyond
 * division by zero. The call sites divide once at startup to establish a tick
 * ratio, so this is not on any hot path and clarity beats cleverness.
 */

#if defined(_WIN32) && defined(__clang__)

typedef unsigned __int128 u128;
typedef __int128 i128;

static u128 udiv128(u128 numerator, u128 denominator, u128 *remainder_out) {
  u128 quotient = 0;
  u128 remainder = 0;

  if (denominator == 0) {
    /* Undefined behaviour at the language level. Returning zeroes keeps this
     * function total rather than trapping in a way the caller cannot see. */
    if (remainder_out) *remainder_out = 0;
    return 0;
  }

  for (int bit = 127; bit >= 0; --bit) {
    remainder = (remainder << 1) | ((numerator >> bit) & 1);
    if (remainder >= denominator) {
      remainder -= denominator;
      quotient |= ((u128)1) << bit;
    }
  }

  if (remainder_out) *remainder_out = remainder;
  return quotient;
}

u128 __udivti3(u128 a, u128 b) { return udiv128(a, b, 0); }

u128 __umodti3(u128 a, u128 b) {
  u128 remainder = 0;
  udiv128(a, b, &remainder);
  return remainder;
}

/* Signed forms, built on the unsigned ones. Truncation toward zero, which is
 * what C requires and what the unsigned magnitude approach gives once the sign
 * is reapplied. */
i128 __divti3(i128 a, i128 b) {
  const int negative = ((a < 0) != (b < 0));
  const u128 ua = (u128)(a < 0 ? -a : a);
  const u128 ub = (u128)(b < 0 ? -b : b);
  const u128 q = udiv128(ua, ub, 0);
  return negative ? -(i128)q : (i128)q;
}

i128 __modti3(i128 a, i128 b) {
  const u128 ua = (u128)(a < 0 ? -a : a);
  const u128 ub = (u128)(b < 0 ? -b : b);
  u128 r = 0;
  udiv128(ua, ub, &r);
  /* The remainder takes the sign of the dividend. */
  return (a < 0) ? -(i128)r : (i128)r;
}

#endif /* _WIN32 && __clang__ */
