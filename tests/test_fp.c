// Bit-identity test: inlined cf_add vs cf_add_slow on random/edge operands.
#include "cray2fp.h"
#include <stdio.h>
#include <stdlib.h>
static uint64_t s = 88172645463325252ULL;
static uint64_t rnd(void) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
static uint64_t gen(void) {
  uint64_t r = rnd(), c = rnd() & CF_COEF;
  int k = r & 7, e;
  switch (k) {
    case 0: e = 040000 + (int)(rnd() % 120) - 60; break;            // near 1
    case 1: e = 040060 + (int)(rnd() % 8); c &= 0x1ff; break;       // tagged popcounts
    case 2: e = 020000 + (int)(rnd() % 4); break;                   // underflow edge
    case 3: e = 057777 - (int)(rnd() % 4); break;                   // overflow edge
    case 4: e = (int)(rnd() % 0100000); break;                      // anything
    case 5: return 0;
    case 6: e = 040000 + (int)(rnd() % 40); c |= 1ULL << 47; break;
    default: e = 040000 + (int)(rnd() % 4000) - 2000; c |= 1ULL << 47;
  }
  return ((r >> 8) & CF_SIGN) | ((uint64_t)(e & 077777) << 48) | c;
}
int main(void) {
  long bad = 0, n = 50000000;
  for (long i = 0; i < n; i++) {
    uint64_t a = gen(), b = (i % 5 == 0) ? a ^ CF_SIGN : gen();
    if (i % 7 == 0) b = (b & ~(0x7fffULL << 48)) | (a & (0x7fffULL << 48));
    if (cf_add(a, b) != cf_add_slow(a, b) && bad++ < 5)
      printf("diff %016llx %016llx: %016llx vs %016llx\n", (unsigned long long)a, (unsigned long long)b,
             (unsigned long long)cf_add(a, b), (unsigned long long)cf_add_slow(a, b));
  }
  printf("mismatches %ld of %ld\n", bad, n);
  return bad != 0;
}
