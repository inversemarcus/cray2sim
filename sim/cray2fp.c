// Cray-2 floating point implementation. See cray2fp.h for sources/assumptions.
#include "cray2fp.h"
#include <math.h>

_Thread_local int cf_range_error;
typedef unsigned __int128 u128;

static uint64_t ovf(int sig) { cf_range_error = 1; return (uint64_t)sig << 48; }

// Normalize a (possibly wide) magnitude whose binary point sits so that
// value = mag * 2^-48 * 2^(e - bias).  mag may exceed 48 bits by a few bits.
static uint64_t pack_norm(int neg, long e, uint64_t mag) {
  if (!mag) return 0;
  while (mag >> 48) {  // carry out: shift right, rounding the dropped bit up
    uint64_t r = mag & 1;
    mag = (mag >> 1) + r;
    e++;
  }
  {
    int sh = __builtin_clzll(mag) - 16;          // bring bit 47 to the top
    mag <<= sh; e -= sh;
  }
  if (e < CF_EXPMIN) return 0;                // underflow -> zero
  if (e >= CF_EXPOVF) return ovf(060000) | (neg ? CF_SIGN : 0) | mag;
  return cf_make(neg, (int)e, mag);
}

uint64_t cf_add_slow(uint64_t a, uint64_t b) {
  int ea = cf_exp(a), eb = cf_exp(b);
  if (ea >= CF_EXPOVF || eb >= CF_EXPOVF) return ovf(060000);
  if (ea < CF_EXPMIN) a = 0, ea = 0;
  if (eb < CF_EXPMIN) b = 0, eb = 0;
  uint64_t ca = cf_coef(a), cb = cf_coef(b);
  if (!ca) return b ? pack_norm((b >> 63) & 1, eb, cb) : 0;
  if (!cb) return pack_norm((a >> 63) & 1, ea, ca);
  // align: shift smaller exponent right, rounding up (keep 1 guard bit)
  long e = ea;
  if (ea > eb) {
    int d = ea - eb;
    cb = d > 49 ? 0 : (((cb << 1) >> d) + 1) >> 1;
  } else if (eb > ea) {
    int d = eb - ea;
    ca = d > 49 ? 0 : (((ca << 1) >> d) + 1) >> 1;
    e = eb;
  }
  int na = (a >> 63) & 1, nb = (b >> 63) & 1;
  uint64_t mag; int neg;
  if (na == nb) { mag = ca + cb; neg = na; }
  else if (ca >= cb) { mag = ca - cb; neg = na; }
  else { mag = cb - ca; neg = nb; }
  if (!mag) return 0;
  return pack_norm(neg, e, mag);
}

// Truncated 48x48 product: partial products summed only down to 2^-56
// (bit index 40 of the 96-bit product), plus a constant pattern on bits
// 2^-48..2^-56.  Returns the 96-bit sum.
static u128 trunc_product(uint64_t ca, uint64_t cb, uint64_t round_pattern) {
  // Full product minus the partial-product bits below 2^-56 (index 40).
  u128 s = (u128)ca * cb;
  uint64_t lo = cb & ((1ULL << 40) - 1);
  while (lo) {
    int i = __builtin_ctzll(lo);
    lo &= lo - 1;
    s -= (u128)(ca & ((1ULL << (40 - i)) - 1)) << i;
  }
  // round pattern: 9 bits for 2^-48 (bit index 48) .. 2^-56 (index 40)
  s += (u128)round_pattern << 40;
  return s;
}
// Figure 2-3 pattern, bits 2^-48..2^-56: 0 0 1 1 0 1 0 0 1
#define RP_MUL 0x069  /* 0b001101001 */

uint64_t cf_mul(uint64_t a, uint64_t b) {
  int ea = cf_exp(a), eb = cf_exp(b);
  int neg = (int)(((a ^ b) >> 63) & 1);
  if (ea >= CF_EXPOVF || eb >= CF_EXPOVF) return ovf(060001);
  uint64_t ca = cf_coef(a), cb = cf_coef(b);
  if (ea == 0 && eb == 0) {  // case 4: integer mode, no normalize correction
    u128 s = trunc_product(ca, cb, RP_MUL);
    return cf_make(neg, 0, (uint64_t)(s >> 48));
  }
  if (ea < CF_EXPMIN || eb < CF_EXPMIN || !ca || !cb) return 0;
  long e = (long)ea + eb - CF_BIAS;
  if (e >= CF_EXPOVF) return ovf(060001);
  u128 s = trunc_product(ca, cb, RP_MUL);
  uint64_t mag;
  if ((s >> 95) & 1) mag = (uint64_t)(s >> 48);
  else { mag = (uint64_t)(s >> 47); e--; }
  mag &= CF_COEF;
  if (e < CF_EXPMIN) return 0;
  if (e >= CF_EXPOVF) return ovf(060001);
  return cf_make(neg, (int)e, mag);
}

// --- approximations (modelled; see header) ---
static __attribute__((unused)) uint64_t from_double_trunc(double d, int bits) {
  uint64_t x = cf_from_double(d);
  uint64_t keep = ~((1ULL << (48 - bits)) - 1) & CF_COEF;
  return (x & ~CF_COEF) | (x & keep);
}

// Build a Cray float from a double in [0.25, 2) scaled by 2^scale, keeping
// `bits` significant coefficient bits; the exponent is handled in integers so
// the full CRAY-2 exponent range is preserved.
static uint64_t scaled(double r, long scale, int bits, int sig) {
  int e;
  double m = frexp(r, &e);                      // r = m * 2^e, m in [0.5, 1)
  uint64_t c = (uint64_t)ldexp(m, 48);
  c &= ~((1ULL << (48 - bits)) - 1) & CF_COEF;
  long ex = (long)e + scale + CF_BIAS;
  if (ex < CF_EXPMIN) return 0;
  if (ex >= CF_EXPOVF) return ovf(sig);
  return cf_make(0, (int)ex, c);
}

uint64_t cf_recip_approx(uint64_t a) {
  int ea = cf_exp(a);
  if (ea >= CF_EXPOVF || ea < CF_EXPMIN || !cf_coef(a)) return ovf(060002);
  uint64_t c = cf_coef(a);
  int sh = 0;
  while (!(c >> 47)) { c <<= 1; sh++; }
  double B = (double)c / 281474976710656.0;            // operand = B * 2^e, B in [0.5,1)
  double A = 1.0 / (((c >> 36) + 0.5) / 4096.0);        // table entry for the high 12 bits
  A = floor(A * 65536.0) / 65536.0;
  double R = 2 * A - A * A * B;                         // documented interpolation step
  long e = (long)ea - CF_BIAS - sh;
  uint64_t r = scaled(R, -e, 30, 060002);
  return (a >> 63) ? r ^ CF_SIGN : r;
}

uint64_t cf_recip_iter(uint64_t a, uint64_t b) {
  // 2 - a*b using the multiply mechanism; modelled at full precision.
  uint64_t two = cf_make(0, CF_BIAS + 2, 1ULL << 47);
  return cf_sub(two, cf_mul(a, b));
}

uint64_t cf_rsqrt_approx(uint64_t a) {
  int ea = cf_exp(a);
  if (ea >= CF_EXPOVF || (a >> 63) || ea <= 3 || !cf_coef(a)) return ovf(060004);
  uint64_t c = cf_coef(a);
  int sh = 0;
  while (!(c >> 47)) { c <<= 1; sh++; }
  long e = (long)ea - CF_BIAS - sh;                     // operand = m * 2^e, m in [0.5,1)
  double m = (double)c / 281474976710656.0;
  if (e & 1) { m *= 2; e -= 1; }                        // make the exponent even
  return scaled(1.0 / sqrt(m), -e / 2, 30, 060004);
}

uint64_t cf_rsqrt_iter(uint64_t a, uint64_t b) {
  // (3 - a*b)/2
  uint64_t three = cf_make(0, CF_BIAS + 2, 3ULL << 46);
  uint64_t t = cf_sub(three, cf_mul(a, b));
  if (!t) return 0;
  return t - (1ULL << 48);  // halve: exponent - 1
}

uint64_t cf_fix(uint64_t a) {
  int e = cf_exp(a);
  uint64_t c = cf_coef(a);
  if (e < CF_EXPMIN || !c) return 0;
  int d = CF_BIAS + 48 - e;  // right shift amount to integer
  if (d < 0) { cf_range_error = 1; return 0; }
  uint64_t mag = d > 49 ? 0 : (((c << 1) >> d) + 1) >> 1;
  int64_t v = (a >> 63) ? -(int64_t)mag : (int64_t)mag;
  return (uint64_t)v;
}

uint64_t cf_flt(uint64_t a) {
  int64_t v = (int64_t)a;
  int neg = v < 0;
  uint64_t mag = (neg ? (uint64_t)(-v) : (uint64_t)v) & CF_COEF;
  return pack_norm(neg, CF_BIAS + 48, mag);
}

uint64_t cf_from_double(double d) {
  if (d == 0 || !isfinite(d)) return 0;
  int e;
  double m = frexp(fabs(d), &e);  // m in [0.5,1)
  uint64_t c = (uint64_t)ldexp(m, 48);
  return cf_make(d < 0, e + CF_BIAS, c);
}

double cf_to_double(uint64_t x) {
  int e = cf_exp(x);
  uint64_t c = cf_coef(x);
  if (!c || e < CF_EXPMIN) return 0.0;
  double v = ldexp((double)c, e - CF_BIAS - 48);
  return (x >> 63) ? -v : v;
}
