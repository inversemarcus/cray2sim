// Cray-2 64-bit floating point, written from HR-2000 (May 1985) section 2.5.2.
//
// Format: bit 63 sign, bits 62..48 biased exponent (bias 040000 octal),
// bits 47..0 coefficient as a fraction (bit 47 = 2^-1), signed magnitude.
// value = coef * 2^-48 * 2^(exp-040000).
//
// Documented behaviour implemented here:
//  * exponent < 020000 is underflow and the operand is treated as zero (add);
//  * exponent >= 060000 is overflow; results are forced to the documented
//    signature exponents (add 060000, mul 060001, recip 060002, rsqrt 060004)
//    and the range-error flag is raised;
//  * the add result is always normalized and a zero coefficient gives an
//    all-zero word;
//  * multiply sums the 48x48 partial-product matrix truncated 8 bit positions
//    below the low-order result bit (bits 2^-49..2^-56 kept), adds round bits
//    at 2^-50 and 2^-51 and truncation-compensation bits 2^-53..2^-56, then a
//    1-bit normalize shift; both exponents zero = integer mode (exp 0, no
//    normalize correction).
//
// Explicit modelling assumptions (manual not specific enough):
//  * add: the alignment-shifted coefficient is "rounded up": we keep one bit
//    below the LSB and add it back (round half up on magnitude);
//  * the reciprocal / reciprocal-square-root approximation tables are not
//    published; we model them as a 12-bit-indexed table followed by one
//    documented linear-interpolation step, giving ~24-bit "half precision"
//    results (the manual says the iteration instruction then yields 46 bits).
#pragma once
#include <stdint.h>

#define CF_SIGN (1ULL << 63)
#define CF_EXPMASK 0x7fff000000000000ULL
#define CF_COEF 0x0000ffffffffffffULL
#define CF_BIAS 040000
#define CF_EXPMIN 020000
#define CF_EXPOVF 060000

extern _Thread_local int cf_range_error;  // set by floating range errors; cleared per instruction

static inline int cf_exp(uint64_t x) { return (int)((x >> 48) & 077777); }
static inline uint64_t cf_coef(uint64_t x) { return x & CF_COEF; }
static inline uint64_t cf_make(int neg, int e, uint64_t c) {
  return (neg ? CF_SIGN : 0) | ((uint64_t)(e & 077777) << 48) | (c & CF_COEF);
}

uint64_t cf_add_slow(uint64_t a, uint64_t b);

// Inlined add, bit-identical to cf_add_slow (checked by sim/fptest).
static inline uint64_t cf_add(uint64_t a, uint64_t b) {
  int ea = cf_exp(a), eb = cf_exp(b);
  if (__builtin_expect(ea >= CF_EXPOVF || eb >= CF_EXPOVF, 0)) return cf_add_slow(a, b);
  uint64_t ca = cf_coef(a), cb = cf_coef(b);
  if (ea < CF_EXPMIN) ca = 0;
  if (eb < CF_EXPMIN) cb = 0;
  if (__builtin_expect(!ca || !cb, 0)) return cf_add_slow(a, b);
  if (ea < eb) { uint64_t t = a; a = b; b = t; t = ca; ca = cb; cb = t; int x = ea; ea = eb; eb = x; }
  int d = ea - eb;
  if (d) cb = d > 49 ? 0 : ((((cb << 1) >> d) + 1) >> 1);
  int na = (int)(a >> 63), nb = (int)(b >> 63), neg;
  uint64_t mag;
  if (na == nb) { mag = ca + cb; neg = na; }
  else if (ca >= cb) { mag = ca - cb; neg = na; }
  else { mag = cb - ca; neg = nb; }
  if (!mag) return 0;
  long e = ea;
  if (mag >> 48) { mag = (mag >> 1) + (mag & 1); e++; if (mag >> 48) { mag = (mag >> 1) + (mag & 1); e++; } }
  int sh = __builtin_clzll(mag) - 16;
  mag <<= sh; e -= sh;
  if (__builtin_expect(e < CF_EXPMIN || e >= CF_EXPOVF, 0)) return cf_add_slow(a, b);
  return ((uint64_t)neg << 63) | ((uint64_t)e << 48) | mag;
}
static inline uint64_t cf_sub(uint64_t a, uint64_t b) { return cf_add(a, b ^ CF_SIGN); }
uint64_t cf_mul(uint64_t a, uint64_t b);          // 124/154/155
uint64_t cf_recip_approx(uint64_t a);              // 132/166
uint64_t cf_recip_iter(uint64_t a, uint64_t b);    // 126/156: 2 - a*b
uint64_t cf_rsqrt_approx(uint64_t a);              // 133/167
uint64_t cf_rsqrt_iter(uint64_t a, uint64_t b);    // 127/157: (3 - a*b)/2
uint64_t cf_fix(uint64_t a);                       // 122/174 float -> integer
uint64_t cf_flt(uint64_t a);                       // 123/175 integer -> float

// Host helpers (not machine operations): conversions for loaders/tests.
uint64_t cf_from_double(double d);
double cf_to_double(uint64_t x);
