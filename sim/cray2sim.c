// cray2sim: instruction-level emulator of the 1985 CRAY-2 mainframe
// (four Background Processors + 256 Mword Common Memory), with a
// clock-period timing model.  Written from HR-2000 (May 1985); see
// ../isa/cray2_isa.json for per-instruction citations and
// ../TIMING.md for the timing parameters and their sources.
//
// The host process plays the Foreground Processor: it loads memory, starts
// Background Processors, and services their exits through a line protocol
// on stdin/stdout (see README).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include "cray2fp.h"

#define NCPU 4
#define CM_WORDS (256u << 20)
#define LM_WORDS 16384
#define NBANK 128

// ---------------- timing parameters (clock periods) ----------------
typedef struct {
  int issue;          // minimum CP between instruction issues
  int extra_parcel;   // added per constant parcel beyond the first
  int branch_taken;   // added after a taken branch
  int ibuf_miss;      // instruction buffer fill (16 words)
  int a_add, a_mul, s_int, s_shift, s_log, s_fp;  // scalar result latencies
  int v_log, v_int, v_fadd, v_fmul;               // vector startup (beyond VL)
  int unit_gap;       // CP after a vector stream before the unit accepts another
  int cm_bank_busy, cm_load_lat, cm_vec_tail, cm_store_lat;
  int lm_scalar, lm_vector;  // local memory latency
  int sema;
} Timing;

static Timing T = {
  .issue = 2, .extra_parcel = 1, .branch_taken = 4, .ibuf_miss = 75,
  .a_add = 4, .a_mul = 8, .s_int = 4, .s_shift = 4, .s_log = 2, .s_fp = 19,
  .v_log = 22, .v_int = 22, .v_fadd = 22, .v_fmul = 22,
  .unit_gap = 2,
  .cm_bank_busy = 57, .cm_load_lat = 59, .cm_vec_tail = 8, .cm_store_lat = 4,
  .lm_scalar = 4, .lm_vector = 4,
  .sema = 4,
};

enum { U_AADD, U_AMUL, U_SINT, U_SSHF, U_SLOG, U_VINT, U_VLOG, U_FADD, U_FMUL, U_CMP, U_LMP, NUNIT };
static const char *unit_name[NUNIT] = {"addr_add", "addr_mul", "s_int", "s_shift", "s_log",
                                       "v_int", "v_log", "f_add", "f_mul", "cm_port", "lm_port"};

typedef struct {
  uint32_t A[8];
  uint64_t S[8];
  uint64_t V[8][64];
  uint64_t VM;
  int VL;  // 1..64
  uint32_t P;  // parcel address relative to BA
  uint32_t BA, LA;  // word addresses
  uint64_t *LM;
  int running, exit_code, id;
  int halt_range, halt_fp;
  uint64_t bps;  // status: range/fp error bits
  // timing
  uint64_t t;  // earliest next issue
  uint64_t A_rdy[8], S_rdy[8], V_wr[8], V_rd[8], VM_rdy;
  uint64_t cm_drain;  // when this processor's last Common Memory reference has been accepted
  uint64_t held;      // mt mode: issue time of a semaphore instruction deferred to a later window
  uint64_t pub;       // mt mode: lower bound on the issue time of this processor's next semaphore
                      // instruction in the current window (~0 when it has none); atomic
  uint64_t unit_free[NUNIT];
  uint64_t ibuf[8];
  int ibuf_n, ibuf_next;
  // statistics
  uint64_t n_inst, n_vec, n_vec_elems, unit_busy[NUNIT], stall_ibuf, cm_words, n_op[128];
} CPU;

static uint64_t *CM;
static uint64_t bank_free[NBANK];
static int semaphore;  // single job semaphore flag
static uint64_t sem_time;  // clock period of the last semaphore operation
static pthread_mutex_t sem_lock = PTHREAD_MUTEX_INITIALIZER;
// Fast multithreaded mode (mt=1): one host thread per Background Processor,
// synchronised every QWIN clock periods of emulated time.  Within a window a
// processor sees other processors' bank-busy times as of the window start.
static int mt = 0;
static uint64_t QWIN = 2000;
static uint64_t bank_own[NCPU][NBANK], bank_snap[NCPU][NBANK];
static CPU cpu[NCPU];
static uint32_t mem_limit = CM_WORDS;

static inline uint64_t max64(uint64_t a, uint64_t b) { return a > b ? a : b; }

static void range_error(CPU *c) { c->bps |= 1; }

static inline uint64_t cm_read(CPU *c, uint64_t addr) {
  uint64_t a = (uint64_t)c->BA + (uint32_t)addr;
  if (a >= c->LA || a >= mem_limit) { range_error(c); return 0; }
  return CM[a];
}
// Scalar references use atomic acquire/release accesses: in mt=1 mode they
// are how processors synchronise (barrier words), so they must not race.
static inline uint64_t cm_read_sync(CPU *c, uint64_t addr) {
  uint64_t a = (uint64_t)c->BA + (uint32_t)addr;
  if (a >= c->LA || a >= mem_limit) { range_error(c); return 0; }
  return __atomic_load_n(&CM[a], __ATOMIC_ACQUIRE);
}
static inline void cm_write_sync(CPU *c, uint64_t addr, uint64_t v) {
  uint64_t a = (uint64_t)c->BA + (uint32_t)addr;
  if (a >= c->LA || a >= mem_limit) { range_error(c); return; }
  __atomic_store_n(&CM[a], v, __ATOMIC_RELEASE);
}
static inline void cm_write(CPU *c, uint64_t addr, uint64_t v) {
  uint64_t a = (uint64_t)c->BA + (uint32_t)addr;
  if (a >= c->LA || a >= mem_limit) { range_error(c); return; }
  CM[a] = v;
}

// One Common Memory reference by CPU c, requested no earlier than t.
// Returns the time the bank accepts it (phase time + bank busy).
static inline uint64_t cm_access(CPU *c, uint64_t absaddr, uint64_t t) {
  int q = absaddr & 3, b = absaddr & 127;
  if (mt) {
    uint64_t a = max64(t, max64(bank_own[c->id][b], bank_snap[c->id][b]));
    a += (int)((q - (int64_t)a - c->id) & 3);
    bank_own[c->id][b] = a + T.cm_bank_busy;
    return a;
  }
  uint64_t a = max64(t, bank_free[b]);
  // processor c may reach quadrant q only when (q - t - c) % 4 == 0
  int ph = (int)((q - (int64_t)a - c->id) & 3);
  a += ph;
  bank_free[b] = a + T.cm_bank_busy;
  return a;
}

// ---------------- instruction fetch ----------------
static inline uint16_t parcel(CPU *c, uint32_t p) {
  uint64_t w = cm_read(c, p >> 2);
  return (uint16_t)(w >> (16 * (3 - (p & 3))));
}

static uint64_t fetch_time(CPU *c, uint32_t p, uint64_t t, int dry) {
  uint64_t blk = ((uint64_t)c->BA * 4 + p) >> 6;  // 16 words = 64 parcels
  for (int i = 0; i < c->ibuf_n; i++)
    if (c->ibuf[i] == blk) return t;
  if (dry) return t + T.ibuf_miss;
  if (c->ibuf_n < 8) c->ibuf[c->ibuf_n++] = blk;
  else { c->ibuf[c->ibuf_next] = blk; c->ibuf_next = (c->ibuf_next + 1) & 7; }
  c->stall_ibuf += T.ibuf_miss;
  return t + T.ibuf_miss;
}

static int popc(uint64_t x) { return __builtin_popcountll(x); }
static int lzc(uint64_t x) { return x ? __builtin_clzll(x) : 64; }

// ---------------- execute one instruction ----------------
// Each case computes the issue time `ti` from resource readiness, performs
// the operation functionally, then books resources.
#define NEED(x) ti = max64(ti, (x))
// The issue logic reserves every V register an operation uses, sources
// included (HR-2000 2.2.3), so a register being read cannot start another stream.
#define VSRC(r) do { NEED(c->V_wr[r]); NEED(c->V_rd[r]); } while (0)
#define VDST(r) do { NEED(c->V_wr[r]); NEED(c->V_rd[r]); } while (0)
// End of an instruction's resource checks: a dry run returns its issue time
// here, before any architectural or timing state is changed.
#define ISSUE if (dry) return ti

static void vbook(CPU *c, int unit, int lat, int dst, int s1, int s2, uint64_t ti) {
  int n = c->VL;
  c->unit_free[unit] = ti + n + T.unit_gap;
  c->unit_busy[unit] += n;
  if (s1 >= 0) c->V_rd[s1] = max64(c->V_rd[s1], ti + n);
  if (s2 >= 0) c->V_rd[s2] = max64(c->V_rd[s2], ti + n);
  if (dst >= 0) c->V_wr[dst] = ti + lat + n;
  c->n_vec++; c->n_vec_elems += n;
}

// Execute the next instruction of processor c, or (dry != 0) only return the
// clock period at which it would issue, without side effects.
static uint64_t step(CPU *c, int dry) {
  uint32_t p0 = c->P;
  uint64_t ti = fetch_time(c, p0, c->t, dry);
  uint16_t w = parcel(c, p0);
  int f = w >> 9, i = (w >> 6) & 7, j = (w >> 3) & 7, k = w & 7, jk = w & 63;
  int np = 1;
  uint32_t m = 0;
  // determine parcel count
  switch (f) {
    case 003: case 004: case 005: case 010: case 011: case 012: case 013:
    case 014: case 015: case 016: case 017: case 042: case 043: case 050: case 051:
    case 052: case 064: case 065: case 066: case 067: np = 3; break;
    case 040: case 041: case 044: case 045: case 054: case 055: np = 2; break;
    case 053: np = 5; break;
  }
  uint64_t mf = 0;
  if (np >= 2) m = parcel(c, p0 + 1);
  if (np == 3) m = (m << 16) | parcel(c, p0 + 2);
  if (np == 5) { for (int q = 1; q <= 4; q++) mf = (mf << 16) | parcel(c, p0 + q); }
  uint32_t nextP = p0 + np;
  int taken = 0;
  int n = c->VL;
  if (!dry) c->n_op[f]++;
  if (!dry) cf_range_error = 0;
  switch (f) {
    case 000: case 001:  // exit / error exit
      ISSUE;
      c->running = 0; c->exit_code = jk; c->P = nextP; c->t = ti + T.issue; c->n_inst++;
      return ti;
    case 002: {  // register jump (with return)
      NEED(c->A_rdy[k]); NEED(c->A_rdy[i]);
      ISSUE;
      uint32_t tgt = c->A[k];
      c->A[i] = nextP; c->A_rdy[i] = ti + T.a_add;
      nextP = tgt; taken = 1; break;
    }
    case 003: ISSUE; nextP = m; taken = 1; break;
    case 004: case 005: case 006: case 007: {
      if (f == 007) {
        // HR-2000 3.2: the semaphore is released, and the following instruction
        // issues, only once the Common Memory quadrant buffers are clear, so
        // earlier stores are complete before another processor can proceed.
        NEED(c->cm_drain); NEED(c->unit_free[U_CMP]);
      }
      if (f <= 005) ti += T.sema;
      // Semaphore operations take effect in clock order across processors.  The
      // schedulers (run, mt_worker) execute them in issue-time order, so this
      // bound only guards that order; it does not normally delay anything.
      NEED(__atomic_load_n(&sem_time, __ATOMIC_ACQUIRE));
      ISSUE;
      pthread_mutex_lock(&sem_lock);
      int was = semaphore;
      semaphore = f != 007;
      if ((f == 004 && !was) || (f == 005 && was)) { nextP = m; taken = 1; }
      if (ti > sem_time) __atomic_store_n(&sem_time, ti, __ATOMIC_RELEASE);
      pthread_mutex_unlock(&sem_lock);
      break;
    }
    case 010: case 011: case 012: case 013: {
      NEED(c->A_rdy[k]);
      ISSUE;
      uint32_t a = c->A[k];
      int cond = f == 010 ? a == 0 : f == 011 ? a != 0 : f == 012 ? !(a >> 31) : (a >> 31);
      if (cond) { nextP = m; taken = 1; }
      break;
    }
    case 014: case 015: case 016: case 017: {
      NEED(c->S_rdy[j]);
      ISSUE;
      uint64_t s = c->S[j];
      int cond = f == 014 ? s == 0 : f == 015 ? s != 0 : f == 016 ? !(s >> 63) : (int)(s >> 63);
      if (cond) { nextP = m; taken = 1; }
      break;
    }
    case 020: case 021: case 022: case 023: {
      NEED(c->A_rdy[j]); NEED(c->A_rdy[k]); NEED(c->A_rdy[i]);
      int u = f < 022 ? U_AADD : U_AMUL;
      NEED(c->unit_free[u]);
      ISSUE;
      uint32_t a = c->A[j], b = c->A[k];
      c->A[i] = f == 020 ? a + b : f == 021 ? a - b : a * b;
      c->A_rdy[i] = ti + (u == U_AADD ? T.a_add : T.a_mul);
      break;
    }
    case 024: NEED(c->S_rdy[j]); NEED(c->A_rdy[i]); ISSUE; c->A[i] = (uint32_t)c->S[j]; c->A_rdy[i] = ti + T.a_add; break;
    case 025: NEED(c->A_rdy[i]); ISSUE; c->A[i] = c->VL & 63; c->A_rdy[i] = ti + T.a_add; break;
    case 026: NEED(c->A_rdy[i]); ISSUE; c->A[i] = jk; c->A_rdy[i] = ti + 1; break;
    case 027: NEED(c->A_rdy[i]); ISSUE; c->A[i] = 0xFFFFFFC0u | jk; c->A_rdy[i] = ti + 1; break;
    case 030: case 031: case 032: case 033: {
      VSRC(k); NEED(c->unit_free[U_VLOG]); NEED(c->VM_rdy);
      ISSUE;
      uint64_t vm = 0;
      for (int e = 0; e < n; e++) {
        uint64_t x = c->V[k][e];
        int b = f == 030 ? x == 0 : f == 031 ? x != 0 : f == 032 ? !(x >> 63) : (int)(x >> 63);
        if (b) vm |= 1ULL << (63 - e);
      }
      c->VM = vm;
      vbook(c, U_VLOG, T.v_log, -1, k, -1, ti);
      c->VM_rdy = ti + T.v_log + n;
      break;
    }
    case 034: NEED(c->S_rdy[j]); NEED(c->VM_rdy); ISSUE; c->VM = c->S[j]; c->VM_rdy = ti + 1; break;
    case 035:
      ISSUE;
      if (k == 0) c->halt_range = 0; else if (k == 1) c->halt_range = 1;
      else if (k == 2) c->halt_fp = 0; else if (k == 3) c->halt_fp = 1;
      break;
    case 036: case 037: NEED(c->A_rdy[k]); ISSUE; c->VL = c->A[k] & 63; if (!c->VL) c->VL = 64; break;
    case 040: NEED(c->A_rdy[i]); ISSUE; c->A[i] = m & 0xffff; c->A_rdy[i] = ti + 1; break;
    case 041: NEED(c->A_rdy[i]); ISSUE; c->A[i] = 0xFFFF0000u | (m & 0xffff); c->A_rdy[i] = ti + 1; break;
    case 042: case 043: NEED(c->A_rdy[i]); ISSUE; c->A[i] = m; c->A_rdy[i] = ti + 1; break;
    case 044: case 046: case 054: case 056: {
      uint32_t addr = m;
      if (f == 046 || f == 056) { NEED(c->A_rdy[k]); addr = c->A[k]; }
      NEED(c->unit_free[U_LMP]);
      if (f == 044 || f == 046) NEED(c->A_rdy[i]); else NEED(c->S_rdy[i]);
      ISSUE;
      uint64_t v = c->LM[addr & (LM_WORDS - 1)];
      if (addr >= LM_WORDS) range_error(c);
      if (f == 044 || f == 046) { c->A[i] = (uint32_t)v; c->A_rdy[i] = ti + T.lm_scalar; }
      else { c->S[i] = v; c->S_rdy[i] = ti + T.lm_scalar; }
      c->unit_free[U_LMP] = ti + 1; c->unit_busy[U_LMP]++;
      break;
    }
    case 045: case 047: case 055: case 057: {
      uint32_t addr = m;
      uint64_t v;
      if (f == 047 || f == 057) { NEED(c->A_rdy[k]); addr = c->A[k]; }
      if (f == 045) { NEED(c->A_rdy[k]); v = (uint64_t)(int64_t)(int32_t)c->A[k]; }
      else if (f == 047) { NEED(c->A_rdy[j]); v = (uint64_t)(int64_t)(int32_t)c->A[j]; }
      else if (f == 055) { NEED(c->S_rdy[j]); v = c->S[j]; }
      else { NEED(c->S_rdy[i]); v = c->S[i]; }
      NEED(c->unit_free[U_LMP]);
      ISSUE;
      if (addr >= LM_WORDS) range_error(c);
      c->LM[addr & (LM_WORDS - 1)] = v;
      c->unit_free[U_LMP] = ti + 1; c->unit_busy[U_LMP]++;
      break;
    }
    case 050: NEED(c->S_rdy[i]); ISSUE; c->S[i] = m; c->S_rdy[i] = ti + 1; break;
    case 051: NEED(c->S_rdy[i]); ISSUE; c->S[i] = 0xFFFFFFFF00000000ULL | m; c->S_rdy[i] = ti + 1; break;
    case 052: NEED(c->S_rdy[i]); ISSUE; c->S[i] = (uint64_t)m << 32; c->S_rdy[i] = ti + 1; break;
    case 053: NEED(c->S_rdy[i]); ISSUE; c->S[i] = mf; c->S_rdy[i] = ti + 1; break;
    case 060: case 062: case 064: case 066: case 061: case 063: case 065: case 067: {
      uint32_t addr;
      int load = !(f & 1);
      if (f == 060 || f == 061) { NEED(c->A_rdy[j]); NEED(c->A_rdy[k]); addr = c->A[j] + c->A[k]; }
      else if (f == 062 || f == 063) { NEED(c->A_rdy[k]); addr = c->A[k]; }
      else if (f == 064 || f == 065) { NEED(c->A_rdy[k]); addr = c->A[k] + m; }
      else addr = m;
      NEED(c->unit_free[U_CMP]);
      NEED(c->S_rdy[i]);
      ISSUE;
      uint64_t acc = cm_access(c, (uint64_t)c->BA + addr, ti);
      if (load) { c->S[i] = cm_read_sync(c, addr); c->S_rdy[i] = acc + T.cm_load_lat; }
      else cm_write_sync(c, addr, c->S[i]);
      c->cm_drain = max64(c->cm_drain, acc + 1);
      c->unit_free[U_CMP] = ti + 1; c->unit_busy[U_CMP]++; c->cm_words++;
      break;
    }
    case 070: case 071: case 072: case 073: {
      int load = !(f & 1), gather = f >= 072;
      int vr = i, vx = j;  // data register Vi, index register Vj (gather/scatter)
      uint32_t base; int32_t stride = 1;
      if (!gather) { NEED(c->A_rdy[j]); NEED(c->A_rdy[k]); base = c->A[j]; stride = (int32_t)c->A[k]; }
      else { NEED(c->A_rdy[k]); VSRC(vx); base = c->A[k]; }
      NEED(c->unit_free[U_CMP]);
      if (load) VDST(vr); else VSRC(vr);
      ISSUE;
      uint64_t last = ti, req = ti;
      uint64_t tmp[64];
      for (int e = 0; e < n; e++) {
        // gather: an index register that is also the destination reads as zeros (HR-2000 2.2.3)
        uint32_t off = gather ? base + (uint32_t)(vx == vr && load ? 0 : c->V[vx][e]) : base + (uint32_t)(e * stride);
        uint64_t acc = cm_access(c, (uint64_t)c->BA + off, req);
        last = max64(last, acc);
        req = max64(req + (gather ? 4 : 1), acc + 1 - (gather ? 0 : 0));
        if (load) tmp[e] = cm_read(c, off);
        else cm_write(c, off, c->V[vr][e]);   // scatter: both registers are sources
      }
      if (load) {
        memcpy(c->V[vr], tmp, n * 8);
        c->V_wr[vr] = last + T.cm_load_lat;
      } else c->V_rd[vr] = max64(c->V_rd[vr], last);
      if (gather) c->V_rd[vx] = max64(c->V_rd[vx], last);
      c->cm_drain = max64(c->cm_drain, last + 1);
      c->unit_free[U_CMP] = last + T.cm_vec_tail;
      c->unit_busy[U_CMP] += c->unit_free[U_CMP] - ti;
      c->n_vec++; c->n_vec_elems += n; c->cm_words += n;
      break;
    }
    case 074: case 075: {
      NEED(c->A_rdy[k]); NEED(c->unit_free[U_LMP]);
      if (f == 074) VDST(i); else VSRC(i);
      ISSUE;
      uint32_t a = c->A[k];
      if (a + n > LM_WORDS) range_error(c);
      if (f == 074) {
        for (int e = 0; e < n; e++) c->V[i][e] = c->LM[(a + e) & (LM_WORDS - 1)];
        c->V_wr[i] = ti + T.lm_vector + n;
      } else {
        for (int e = 0; e < n; e++) c->LM[(a + e) & (LM_WORDS - 1)] = c->V[i][e];
        c->V_rd[i] = max64(c->V_rd[i], ti + n);
      }
      c->unit_free[U_LMP] = ti + T.lm_vector + n;
      c->unit_busy[U_LMP] += n;
      c->n_vec++; c->n_vec_elems += n;
      break;
    }
    case 076: case 077: case 0134: case 0135: case 0136: case 0137: ISSUE; break;
    case 0100: case 0101: case 0102: case 0103: {
      NEED(c->S_rdy[j]); NEED(c->S_rdy[k]); NEED(c->S_rdy[i]);
      ISSUE;
      uint64_t a = c->S[j], b = c->S[k];
      c->S[i] = f == 0100 ? a & b : f == 0101 ? a & ~b : f == 0102 ? a ^ b : a | b;
      c->S_rdy[i] = ti + T.s_log; break;
    }
    case 0104: case 0105: {
      NEED(c->S_rdy[j]); NEED(c->S_rdy[k]); NEED(c->S_rdy[i]);
      ISSUE;
      c->S[i] = f == 0104 ? c->S[j] + c->S[k] : c->S[j] - c->S[k];
      c->S_rdy[i] = ti + T.s_int; break;
    }
    case 0106: case 0107: {
      NEED(c->S_rdy[j]); NEED(c->S_rdy[i]);
      ISSUE;
      uint64_t x = c->S[j];
      c->S[i] = f == 0107 ? (uint64_t)lzc(x) : k == 1 ? (uint64_t)(popc(x) & 1) : (uint64_t)popc(x);
      c->S_rdy[i] = ti + T.s_int; break;
    }
    case 0110: NEED(c->S_rdy[i]); ISSUE; { int sh = 64 - jk; c->S[i] = sh >= 64 ? 0 : c->S[i] << sh; } c->S_rdy[i] = ti + T.s_shift; break;
    case 0111: NEED(c->S_rdy[i]); ISSUE; c->S[i] = c->S[i] >> jk; c->S_rdy[i] = ti + T.s_shift; break;
    case 0112: case 0113: {
      NEED(c->S_rdy[i]); NEED(c->S_rdy[j]); NEED(c->A_rdy[k]);
      ISSUE;
      uint32_t sh = c->A[k];
      unsigned __int128 F;
      if (f == 0112) {
        F = ((unsigned __int128)c->S[i] << 64) | c->S[j];
        c->S[i] = sh >= 128 ? 0 : (uint64_t)((F << sh) >> 64);
      } else {
        F = ((unsigned __int128)c->S[j] << 64) | c->S[i];
        c->S[i] = sh >= 128 ? 0 : (uint64_t)(F >> sh);
      }
      c->S_rdy[i] = ti + T.s_shift; break;
    }
    case 0114: NEED(c->VM_rdy); NEED(c->S_rdy[i]); ISSUE; c->S[i] = c->VM; c->S_rdy[i] = ti + 1; break;
    case 0115: NEED(c->S_rdy[i]); ISSUE; c->S[i] = ti; c->S_rdy[i] = ti + 1; break;
    case 0116: NEED(c->S_rdy[i]); ISSUE; c->S[i] = jk; c->S_rdy[i] = ti + 1; break;
    case 0117: NEED(c->S_rdy[i]); ISSUE; c->S[i] = 0xFFFFFFFFFFFFFFC0ULL | jk; c->S_rdy[i] = ti + 1; break;
    case 0120: case 0121: case 0122: case 0123: case 0124: case 0125: case 0126: case 0127:
    case 0132: case 0133: {
      int unary = f == 0122 || f == 0123 || f == 0132 || f == 0133;
      int u = (f >= 0124) ? U_FMUL : U_FADD;
      int src1 = (f == 0122 || f == 0123) ? k : j;
      NEED(c->S_rdy[src1]); if (!unary) NEED(c->S_rdy[k]);
      NEED(c->S_rdy[i]); NEED(c->unit_free[u]);
      ISSUE;
      uint64_t a = c->S[src1], b = c->S[k], r;
      switch (f) {
        case 0120: r = cf_add(a, b); break;
        case 0121: r = cf_sub(a, b); break;
        case 0122: r = cf_fix(a); break;
        case 0123: r = cf_flt(a); break;
        case 0124: case 0125: r = cf_mul(a, b); break;
        case 0126: r = cf_recip_iter(a, b); break;
        case 0127: r = cf_rsqrt_iter(a, b); break;
        case 0132: r = cf_recip_approx(c->S[j]); break;
        default: r = cf_rsqrt_approx(c->S[j]); break;
      }
      c->S[i] = r; c->S_rdy[i] = ti + T.s_fp;
      if (f == 0132 || f == 0133) c->unit_free[u] = ti + 4;
      break;
    }
    case 0130: NEED(c->A_rdy[k]); NEED(c->S_rdy[i]); ISSUE; c->S[i] = c->A[k]; c->S_rdy[i] = ti + 1; break;
    case 0131: NEED(c->A_rdy[k]); NEED(c->S_rdy[i]); ISSUE; c->S[i] = (uint64_t)(int64_t)(int32_t)c->A[k]; c->S_rdy[i] = ti + 1; break;
    default: {
      // vector register operations 140..177
      if (f < 0140) {
        ISSUE;
        fprintf(stderr, "cpu%d: undefined opcode %03o at P=%o\n", c->id, f, p0);
        c->running = 0; c->exit_code = -1; return ti;
      }
      int vj_vec = 1, unit, lat, sj = 0;
      switch (f) {
        case 0140: case 0142: case 0144: case 0146: vj_vec = 0; sj = 1; unit = U_VLOG; lat = T.v_log; break;
        case 0141: case 0143: case 0145: case 0147: unit = U_VLOG; lat = T.v_log; break;
        case 0150: case 0151: case 0152: case 0153: case 0164: case 0165: unit = U_VINT; lat = T.v_int; break;
        case 0160: case 0162: vj_vec = 0; sj = 1; unit = U_VINT; lat = T.v_int; break;
        case 0161: case 0163: unit = U_VINT; lat = T.v_int; break;
        case 0176: case 0177: vj_vec = 0; sj = 1; unit = U_VINT; lat = T.v_int; break;
        case 0154: vj_vec = 0; sj = 1; unit = U_FMUL; lat = T.v_fmul; break;
        case 0155: case 0156: case 0157: unit = U_FMUL; lat = T.v_fmul; break;
        case 0166: case 0167: vj_vec = 0; unit = U_FMUL; lat = T.v_fmul; break;
        case 0170: case 0172: vj_vec = 0; sj = 1; unit = U_FADD; lat = T.v_fadd; break;
        case 0171: case 0173: unit = U_FADD; lat = T.v_fadd; break;
        case 0174: case 0175: vj_vec = 0; unit = U_FADD; lat = T.v_fadd; break;
        default: fprintf(stderr, "bad vector opcode %03o\n", f); exit(1);
      }
      // operand usage
      int useVj = vj_vec && !(f == 0164 || f == 0165) ? 1 : 0;
      int useVk = 1;
      if (f == 0164 || f == 0165 || f == 0150 || f == 0151 || f == 0152 || f == 0153) { useVj = 1; useVk = 0; }
      if (f == 0176 || f == 0177) useVk = 0;
      if (useVj) VSRC(j);
      if (useVk) VSRC(k);
      if (sj) NEED(c->S_rdy[j]);
      if (f >= 0150 && f <= 0153) NEED(c->A_rdy[k]);
      if (f == 0176 || f == 0177) NEED(c->S_rdy[k]);
      if (f == 0146 || f == 0147) NEED(c->VM_rdy);
      VDST(i);
      NEED(c->unit_free[unit]);
      ISSUE;
      static const uint64_t zero[64];
      const uint64_t *vj = useVj ? (j == i ? zero : c->V[j]) : zero;
      const uint64_t *vk = useVk ? (k == i ? zero : c->V[k]) : zero;
      uint64_t s = c->S[j], *d = c->V[i];
      uint32_t ak = c->A[k];
      switch (f) {
        case 0140: for (int e = 0; e < n; e++) d[e] = s & vk[e]; break;
        case 0141: for (int e = 0; e < n; e++) d[e] = vj[e] & vk[e]; break;
        case 0142: for (int e = 0; e < n; e++) d[e] = s ^ vk[e]; break;
        case 0143: for (int e = 0; e < n; e++) d[e] = vj[e] ^ vk[e]; break;
        case 0144: for (int e = 0; e < n; e++) d[e] = s | vk[e]; break;
        case 0145: for (int e = 0; e < n; e++) d[e] = vj[e] | vk[e]; break;
        case 0146: for (int e = 0; e < n; e++) d[e] = (c->VM >> (63 - e)) & 1 ? s : vk[e]; break;
        case 0147: for (int e = 0; e < n; e++) d[e] = (c->VM >> (63 - e)) & 1 ? vj[e] : vk[e]; break;
        case 0150: for (int e = 0; e < n; e++) d[e] = ak >= 64 ? 0 : vj[e] << ak; break;
        case 0151: for (int e = 0; e < n; e++) d[e] = ak >= 64 ? 0 : vj[e] >> ak; break;
        case 0152: {
          uint64_t t2[64];
          for (int e = 0; e < n; e++) {
            unsigned __int128 F = ((unsigned __int128)vj[e] << 64) | (e + 1 < n ? vj[e + 1] : 0);
            t2[e] = ak >= 128 ? 0 : (uint64_t)((F << ak) >> 64);
          }
          memcpy(d, t2, n * 8); break;
        }
        case 0153: {
          uint64_t t2[64];
          for (int e = 0; e < n; e++) {
            unsigned __int128 F = ((unsigned __int128)(e > 0 ? vj[e - 1] : 0) << 64) | vj[e];
            t2[e] = ak >= 128 ? 0 : (uint64_t)(F >> ak);
          }
          memcpy(d, t2, n * 8); break;
        }
        case 0154: for (int e = 0; e < n; e++) d[e] = cf_mul(s, vk[e]); break;
        case 0155: for (int e = 0; e < n; e++) d[e] = cf_mul(vj[e], vk[e]); break;
        case 0156: for (int e = 0; e < n; e++) d[e] = cf_recip_iter(vj[e], vk[e]); break;
        case 0157: for (int e = 0; e < n; e++) d[e] = cf_rsqrt_iter(vj[e], vk[e]); break;
        case 0160: for (int e = 0; e < n; e++) d[e] = s + vk[e]; break;
        case 0161: for (int e = 0; e < n; e++) d[e] = vj[e] + vk[e]; break;
        case 0162: for (int e = 0; e < n; e++) d[e] = s - vk[e]; break;
        case 0163: for (int e = 0; e < n; e++) d[e] = vj[e] - vk[e]; break;
        case 0164: for (int e = 0; e < n; e++) d[e] = k == 1 ? popc(vj[e]) & 1 : popc(vj[e]); break;
        case 0165: for (int e = 0; e < n; e++) d[e] = lzc(vj[e]); break;
        case 0166: for (int e = 0; e < n; e++) d[e] = cf_recip_approx(vk[e]); break;
        case 0167: for (int e = 0; e < n; e++) d[e] = cf_rsqrt_approx(vk[e]); break;
        case 0170: for (int e = 0; e < n; e++) d[e] = cf_add(s, vk[e]); break;
        case 0171: for (int e = 0; e < n; e++) d[e] = cf_add(vj[e], vk[e]); break;
        case 0172: for (int e = 0; e < n; e++) d[e] = cf_sub(s, vk[e]); break;
        case 0173: for (int e = 0; e < n; e++) d[e] = cf_sub(vj[e], vk[e]); break;
        case 0174: for (int e = 0; e < n; e++) d[e] = cf_fix(vk[e]); break;
        case 0175: for (int e = 0; e < n; e++) d[e] = cf_flt(vk[e]); break;
        case 0176: case 0177: {
          uint64_t mask = s; uint32_t st = (uint32_t)c->S[k]; int o = 0;
          for (int q = 0; q < 64 && o < n; q++)
            if ((mask >> (63 - q)) & 1) d[o++] = (uint64_t)q * st;
          break;
        }
      }
      vbook(c, unit, lat, i, useVj ? j : -1, useVk ? k : -1, ti);
      break;
    }
  }
  c->n_inst++;
  c->P = nextP;
  c->t = ti + T.issue + (np > 2 ? np - 2 : 0) * T.extra_parcel + (taken ? T.branch_taken : 0);
  if (cf_range_error) {
    c->bps |= 2;                                          // floating-point range error
    if (c->halt_fp) { c->running = 0; c->exit_code = -3; }
  }
  if ((c->bps & 1) && c->halt_range) { c->running = 0; c->exit_code = -2; }
  return ti;
}

// ---------------- Foreground Processor (host) protocol ----------------
// Exact mode: always execute the instruction that issues earliest across the
// four processors (lowest index on ties).  A processor's next issue time
// depends on its own state and, for a semaphore instruction, on sem_time, so
// it is recomputed after that processor executes, and every processor's is
// recomputed after a semaphore operation.
static int is_sema(CPU *c) { int f = parcel(c, c->P) >> 9; return f >= 004 && f <= 007; }
static void run(void) {
  uint64_t next[NCPU];
  for (int q = 0; q < NCPU; q++)
    if (cpu[q].running) next[q] = step(&cpu[q], 1);
  for (;;) {
    int best = -1;
    for (int q = 0; q < NCPU; q++)
      if (cpu[q].running && (best < 0 || next[q] < next[best])) best = q;
    if (best < 0) return;
    int sema = is_sema(&cpu[best]);
    step(&cpu[best], 0);
    if (!cpu[best].running) return;
    for (int q = 0; q < NCPU; q++)
      if (cpu[q].running && (q == best || sema)) next[q] = step(&cpu[q], 1);
  }
}

// Portable spinning barrier (macOS has no pthread_barrier_t).
typedef struct { volatile int count, gen; int n; } SpinBar;
static SpinBar mt_bar;
static void bar_wait(SpinBar *b) {
  int g = __atomic_load_n(&b->gen, __ATOMIC_ACQUIRE);
  if (__atomic_add_fetch(&b->count, 1, __ATOMIC_ACQ_REL) == b->n) {
    __atomic_store_n(&b->count, 0, __ATOMIC_RELAXED);
    __atomic_add_fetch(&b->gen, 1, __ATOMIC_RELEASE);
  } else {
    while (__atomic_load_n(&b->gen, __ATOMIC_ACQUIRE) == g) { }
  }
}
static volatile int mt_stop;
static uint64_t mt_window;

static void mt_advance(void) {
  int any_running = 0, any_exit = 0;
  uint64_t tmin = ~0ULL;
  for (int q = 0; q < NCPU; q++) {
    if (cpu[q].running) { any_running = 1; uint64_t tq = max64(cpu[q].t, cpu[q].held); tmin = tq < tmin ? tq : tmin; }
    else if (cpu[q].exit_code != 1000) any_exit = 1;
  }
  if (!any_running || any_exit) { mt_stop = 1; return; }
  mt_window = tmin + QWIN;
  for (int q = 0; q < NCPU; q++)
    __atomic_store_n(&cpu[q].pub, cpu[q].running ? max64(cpu[q].t, cpu[q].held) : ~0ULL, __ATOMIC_RELEASE);
  for (int i = 0; i < NCPU; i++)
    for (int b = 0; b < NBANK; b++) {
      uint64_t m = 0;
      for (int j = 0; j < NCPU; j++) if (j != i && bank_own[j][b] > m) m = bank_own[j][b];
      bank_snap[i][b] = m;
    }
}

static void *mt_worker(void *arg) {
  CPU *c = &cpu[(long)arg];
  for (;;) {
    while (c->running && c->t < mt_window) {
      // A semaphore instruction must not take effect before its own issue
      // time (CSM can wait thousands of CP for stores to drain), so it runs
      // only in the window that contains that time.
      if (is_sema(c)) {
        uint64_t ti = step(c, 1);
        if (ti >= mt_window) { c->held = ti; break; }
        // Within the window, semaphore operations run in issue-time order
        // (lowest processor first on ties), as in exact mode: wait until no
        // other processor can still reach one that issues earlier.
        __atomic_store_n(&c->pub, ti, __ATOMIC_RELEASE);
        for (int q = 0; q < NCPU; q++) {
          if (q == c->id) continue;
          uint64_t pq;
          while ((pq = __atomic_load_n(&cpu[q].pub, __ATOMIC_ACQUIRE)) < ti || (pq == ti && q < c->id)) { }
        }
      }
      c->held = 0;
      step(c, 0);
      __atomic_store_n(&c->pub, c->running ? c->t : ~0ULL, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&c->pub, ~0ULL, __ATOMIC_RELEASE);
    bar_wait(&mt_bar);
    if (c->id == 0) mt_advance();
    bar_wait(&mt_bar);
    if (mt_stop) return NULL;
  }
}

static void run_mt(void) {
  for (int q = 0; q < NCPU; q++)
    for (int b = 0; b < NBANK; b++) bank_own[q][b] = bank_free[b];
  mt_stop = 0;
  mt_advance();
  if (!mt_stop) {
    pthread_t th[NCPU];
    mt_bar.count = 0; mt_bar.gen = 0; mt_bar.n = NCPU;
    for (long q = 0; q < NCPU; q++) pthread_create(&th[q], NULL, mt_worker, (void *)q);
    for (int q = 0; q < NCPU; q++) pthread_join(th[q], NULL);
  }
  for (int b = 0; b < NBANK; b++) {
    uint64_t m = 0;
    for (int q = 0; q < NCPU; q++) if (bank_own[q][b] > m) m = bank_own[q][b];
    bank_free[b] = m;
  }
}

static uint64_t now(void) {
  uint64_t t = 0;
  for (int q = 0; q < NCPU; q++) t = max64(t, cpu[q].t);
  return t;
}

static void report_exit(void) {
  for (int q = 0; q < NCPU; q++) {
    CPU *c = &cpu[q];
    if (!c->running && c->exit_code != 1000) {
      printf("EXIT %d %d %llu P=%u A=", q, c->exit_code, (unsigned long long)c->t, c->P);
      for (int r = 0; r < 8; r++) printf("%s%u", r ? "," : "", c->A[r]);
      printf(" S=");
      for (int r = 0; r < 8; r++) printf("%s%llx", r ? "," : "", (unsigned long long)c->S[r]);
      printf("\n");
      c->exit_code = 1000;  // reported
    }
  }
  fflush(stdout);
}

static void stats(FILE *o) {
  for (int q = 0; q < NCPU; q++) {
    CPU *c = &cpu[q];
    fprintf(o, "STAT cpu%d t=%llu inst=%llu vec=%llu elems=%llu cm_words=%llu ibuf_stall=%llu", q,
            (unsigned long long)c->t, (unsigned long long)c->n_inst, (unsigned long long)c->n_vec,
            (unsigned long long)c->n_vec_elems, (unsigned long long)c->cm_words, (unsigned long long)c->stall_ibuf);
    for (int u = 0; u < NUNIT; u++) fprintf(o, " %s=%llu", unit_name[u], (unsigned long long)c->unit_busy[u]);
    fprintf(o, "\n");
  }
  fflush(o);
}

static int set_timing(const char *kv) {
  char key[64]; int v;
  if (sscanf(kv, "%63[^=]=%d", key, &v) != 2) return -1;
#define TK(x) if (!strcmp(key, #x)) { T.x = v; return 0; }
  TK(issue) TK(extra_parcel) TK(branch_taken) TK(ibuf_miss) TK(a_add) TK(a_mul) TK(s_int) TK(s_shift)
  TK(s_log) TK(s_fp) TK(v_log) TK(v_int) TK(v_fadd) TK(v_fmul) TK(unit_gap) TK(cm_bank_busy)
  TK(cm_load_lat) TK(cm_vec_tail) TK(cm_store_lat) TK(lm_scalar) TK(lm_vector) TK(sema)
  return -1;
}

int main(int argc, char **argv) {
  for (int a = 1; a < argc; a++) {
    if (!strncmp(argv[a], "mt=", 3)) { mt = atoi(argv[a] + 3); continue; }
    if (!strncmp(argv[a], "qwin=", 5)) { QWIN = strtoull(argv[a] + 5, 0, 10); continue; }
    if (set_timing(argv[a])) { fprintf(stderr, "bad timing arg %s\n", argv[a]); return 2; }
  }
  CM = calloc(CM_WORDS, 8);
  if (!CM) { perror("calloc"); return 1; }
  for (int q = 0; q < NCPU; q++) {
    cpu[q].id = q; cpu[q].LM = calloc(LM_WORDS, 8); cpu[q].VL = 64; cpu[q].LA = CM_WORDS;
    cpu[q].exit_code = 1000;
  }
  char *line = NULL; size_t line_cap = 0;
  printf("READY cray2sim 4cpu %u words\n", CM_WORDS); fflush(stdout);
  // getline reads each command line whole, whatever its length or content, so
  // every line gets exactly one reply
  while (getline(&line, &line_cap, stdin) > 0) {
    char cmd[32] = "";
    sscanf(line, "%31s", cmd);
    if (!strcmp(cmd, "load")) {  // load FILE WORDADDR : raw little-endian words (FILE may contain spaces)
      char *sp1 = strchr(line, ' '), *sp2 = strrchr(line, ' ');
      if (!sp1 || sp2 == sp1) { printf("ERR usage: load FILE ADDR\n"); fflush(stdout); continue; }
      char *end; unsigned long long at = strtoull(sp2 + 1, &end, 0);
      *sp2 = 0;
      const char *fn = sp1 + 1;
      FILE *fp = fopen(fn, "rb");
      if (!fp) { printf("ERR %s\n", strerror(errno)); fflush(stdout); continue; }
      fseek(fp, 0, SEEK_END); long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
      size_t nw = sz / 8;
      if (at > CM_WORDS || nw > CM_WORDS - at) { printf("ERR too big\n"); fflush(stdout); fclose(fp); continue; }
      size_t got = fread(CM + at, 8, nw, fp); fclose(fp);
      printf("OK %zu\n", got); fflush(stdout);
    } else if (!strcmp(cmd, "w")) {  // w ADDR v1 v2 ... (hex)
      // parse and check every value before writing any, so a bad line changes nothing
      char *p = line + 1, *e; errno = 0; unsigned long long at = strtoull(p, &e, 0);
      static uint64_t vals[CM_WORDS < 65536 ? CM_WORDS : 65536];
      int cnt = 0;
      const char *err = e == p || errno || (*e != ' ' && *e != '\n' && *e) ? "ERR address" : NULL;
      for (p = e; !err;) {
        while (*p == ' ') p++;
        if (*p == '\n' || !*p) break;
        errno = 0;
        uint64_t v = strtoull(p, &e, 16);
        if (e == p || errno || (*e && *e != ' ' && *e != '\n')) err = "ERR value";
        else if (cnt == (int)(sizeof vals / sizeof *vals)) err = "ERR too many values";
        else if (at + cnt >= CM_WORDS) err = "ERR address";
        else vals[cnt++] = v;
        p = e;
      }
      if (err) printf("%s\n", err);
      else { memcpy(CM + at, vals, cnt * sizeof *vals); printf("OK %d\n", cnt); }
      fflush(stdout);
    } else if (!strcmp(cmd, "r")) {  // r ADDR N (hex output)
      unsigned long long at = 0, nn = 0;
      if (sscanf(line, "%*s %llu %llu", &at, &nn) != 2 || at > CM_WORDS || nn > CM_WORDS - at) {
        printf("ERR address\n"); fflush(stdout); continue;
      }
      printf("DATA");
      for (unsigned long long q = 0; q < nn; q++) printf(" %llx", (unsigned long long)CM[at + q]);
      printf("\n"); fflush(stdout);
    } else if (!strcmp(cmd, "dump")) {  // dump ADDR N FILE
      unsigned long long at = 0, nn = 0; char fn[1024];
      if (sscanf(line, "%*s %llu %llu %1023[^\n]", &at, &nn, fn) != 3 || at > CM_WORDS || nn > CM_WORDS - at) {
        printf("ERR usage: dump ADDR N FILE\n"); fflush(stdout); continue;
      }
      FILE *fp = fopen(fn, "wb");
      if (!fp) { printf("ERR %s\n", strerror(errno)); fflush(stdout); continue; }
      fwrite(CM + at, 8, nn, fp); fclose(fp);
      printf("OK\n"); fflush(stdout);
    } else if (!strcmp(cmd, "start")) {  // start CPU PARCEL_ADDR : fresh start (registers cleared, S7=cpu)
      int q = -1; unsigned long long pa = 0;
      if (sscanf(line, "%*s %d %llu", &q, &pa) != 2 || q < 0 || q >= NCPU) { printf("ERR usage: start CPU PARCEL\n"); fflush(stdout); continue; }
      CPU *c = &cpu[q];
      memset(c->A, 0, sizeof c->A); memset(c->S, 0, sizeof c->S);
      c->S[7] = q; c->A[7] = q; c->P = (uint32_t)pa; c->running = 1; c->exit_code = 1000; c->held = 0;
      c->t = max64(c->t, now());
      printf("OK\n"); fflush(stdout);
    } else if (!strcmp(cmd, "resume")) {  // resume CPU : continue after exit
      int q = -1;
      if (sscanf(line, "%*s %d", &q) != 1 || q < 0 || q >= NCPU) { printf("ERR usage: resume CPU\n"); fflush(stdout); continue; }
      cpu[q].running = 1; cpu[q].exit_code = 1000; cpu[q].held = 0; cpu[q].t = max64(cpu[q].t, now());
      printf("OK\n"); fflush(stdout);
    } else if (!strcmp(cmd, "go")) {  // run until any CPU exits (or all idle)
      if (mt) run_mt(); else run();
      int any = 0;
      for (int q = 0; q < NCPU; q++) any |= cpu[q].running;
      report_exit();
      if (!any) printf("IDLE %llu\n", (unsigned long long)now());
      printf("END\n"); fflush(stdout);
    } else if (!strcmp(cmd, "regs")) {  // regs CPU : one line, V regs first 2 elements
      int q = -1;
      if (sscanf(line, "%*s %d", &q) != 1 || q < 0 || q >= NCPU) { printf("ERR usage: regs CPU\n"); fflush(stdout); continue; }
      CPU *c = &cpu[q];
      printf("REGS VL=%d VM=%llx", c->VL, (unsigned long long)c->VM);
      for (int r = 0; r < 8; r++) printf(" V%d=%llx,%llx", r, (unsigned long long)c->V[r][0], (unsigned long long)c->V[r][1]);
      printf("\n"); fflush(stdout);
    } else if (!strcmp(cmd, "stats")) {
      stats(stdout);
    } else if (!strcmp(cmd, "timing")) {
      char kv[64];
      if (sscanf(line, "%*s %63s", kv) == 1 && !set_timing(kv)) printf("OK\n"); else printf("ERR\n");
      fflush(stdout);
    } else if (!strcmp(cmd, "quit")) {
      break;
    } else if (cmd[0]) {
      printf("ERR unknown %s\n", cmd); fflush(stdout);
    }
  }
  return 0;
}
