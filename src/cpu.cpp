//============================================================================
// cpu.cpp — C++ translation of Hemu-wasm/src/cpu.HC
// x86-64 interpreter: fetch/decode/execute/EFLAGS, devices, HLE hooks.
//============================================================================
#include "cpu.h"
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>

thread_local int t_my_core = 0;

//---------------------------------------------------------------- global variable definitions
U8  *mem = nullptr;
U64 mem_size = 0;
U8  g_pfx[256];

I64 icount = 0;
U64 g_hle_blit = 0;
U8  *g_desk = nullptr;
I64 g_desk_ready = 0;
U64 g_capture_rip = 0;
U64 g_skip_c4 = 0, g_skip_vga = 0, g_skip_xlat = 0;
U64 g_skip_bg = 0;
U64 g_game_running = 0;
U64 g_jit_on = 0;
U64 g_jit_rip[65536] = {};
I64 g_jit_n[65536] = {};
I64 g_jit_hot[65536] = {};
U8  g_hooktbl[65536] = {};

U64 ata_words = 0, ata_seccnt = 0, ata_pos = 0, ata_r3 = 0, ata_r4 = 0, ata_r5 = 0;
U64 ata_wr = 0, ata_wlba = 0, ata_wcnt = 0;
U8  *ata_buf = nullptr;

U64 g_hpet_rate = 0, g_jiffies0 = 0;
U64 g_badop = 0;

U64 cr0 = 0, cr2 = 0, cr3 = 0, cr4 = 0, cr8 = 0, g_cs = 0, g_ss = 0;
U64 idtr_base = 0, idtr_limit = 0, gdtr_base = 0, gdtr_limit = 0, tsc = 0;
I64 g_tsc_q16 = 0, g_tsc_acc = 0;
I64 g_hpet_q16 = 0, g_hpet_acc = 0;
I64 tsc_rate = 0;

U64 msr_efer = 0, msr_star = 0, msr_lstar = 0, msr_sfmask = 0, msr_kgsbase = 0, msr_apic = 0;
U64 g_xmm_lo[NCORES][16] = {};
U64 g_xmm_hi[NCORES][16] = {};
I64 g_lockf[NCORES] = {};

I64 pic1_base = 0, pic1_mask = 0, pic1_irr = 0, pic1_isr = 0, pic1_initstep = 0;
I64 pic2_base = 0, pic2_mask = 0, pic2_irr = 0, pic2_isr = 0, pic2_initstep = 0;
I64 pit_reload = 0, pit_div = 0, pit_counter = 0, pit_wlo = 0;
I64 irq_count = 0;

I64 rtc_idx = 0;
U64 rtc_base_sec = 0;
I64 rtc_dow = 0, rtc_day = 0, rtc_mon = 0, rtc_year = 0;

I64 kbd_q[64] = {}, kbd_qh = 0, kbd_qt = 0;
I64 ms_byte0 = 0, ms_byte1 = 0, ms_byte2 = 0, ms_has = 0, ms_phase = 0;
I64 ps2q[16] = {}, ps2q_ms[16] = {}, ps2qh = 0, ps2qt = 0, ms_next = 0, cfg_next = 0;

I64 pit2_reload = 0, pit2_div = 0, pit2_wlo = 0, spk_on = 0;

U64 hpet_ctr = 0;
U64 lapic_base = 0;
U64 hpet_base = 0;

// Raster HLE state
U64 g_raster_hle[4] = {}, g_raster_cand = 0;
I64 g_raster_n = 0, g_raster_calls = 0;
U64 g_poly3_hle = 0;
I64 g_poly3_calls = 0, g_poly3_scanned = 0, g_poly3_diag = 0;
U64 g_raster_verify = 0, g_raster_ret = 0, g_rv_zbuf = 0, g_rv_dc = 0, g_rv_c1 = 0, g_rv_c2 = 0, g_rv_icount = 0, g_rv_irqc = 0;
U64 g_dbg_bad[13] = {};
I64 g_dbg_badn = 0;
U64 g_ctr_rip = 0;
I64 g_ctr_n = 0;
I64 g_rv_idx = 0, g_rv_cnt = 0, g_rv_step = 1, g_raster_diffs = 0, g_raster_checks = 0, g_rv_bodydiff = 0;
U8  g_rv_savb[1024] = {};
U32 g_rv_savz[1024] = {}, g_rv_natz[1024] = {}, g_rv_natb[1024] = {};

//---------------------------------------------------------------- InitMem
U0 InitMem(U64 sz) {
  I64 i;
  mem = (U8*)calloc(1, sz + 16); mem_size = sz;
  for (i = 0; i < 256; i++) g_pfx[i] = 0;
  g_pfx[0x66]=1; g_pfx[0x67]=1; g_pfx[0xF0]=1; g_pfx[0xF2]=1; g_pfx[0xF3]=1;
  g_pfx[0x2E]=1; g_pfx[0x36]=1; g_pfx[0x3E]=1; g_pfx[0x26]=1; g_pfx[0x64]=1; g_pfx[0x65]=1;
  g_desk = (U8*)calloc(1, 307200); g_hpet_rate = 33;
  ata_buf = (U8*)calloc(1, 131072);
}

//---------------------------------------------------------------- PS/2 + speaker
U0 Ps2Resp(I64 b, I64 isms) { ps2q[ps2qt & 15] = b; ps2q_ms[ps2qt & 15] = isms; ps2qt++; }
U0 UpdateSpk() { if (spk_on && pit2_div) host_snd(1193182.0 / pit2_div); else host_snd(0.0); }
U0 InjectKey(I64 sc) { tsc += sc >> 8; kbd_q[kbd_qt & 63] = sc & 0xFF; kbd_qt++; pic1_irr |= 2; }
U0 InjectMouse(I64 b0, I64 b1, I64 b2) { ms_byte0=b0; ms_byte1=b1; ms_byte2=b2; ms_has=3; ms_phase=0; pic2_irr |= 0x10; }

//---------------------------------------------------------------- MMIO
U64 MmioRd(U64 a, I64 sz) {
  U64 off = a & 0xFFF;
  if (off == 0x20)  return (U64)MyCore() << 24;
  if (off == 0x30)  return 0x50014;
  if (off == 0xF0)  return hpet_ctr;
  if (off == 0xF4 || off == 0xF8) return hpet_ctr >> 32;
  return 0;
}
U0 MmioWr(U64 a, I64 sz, U64 v) {
  U64 off = a & 0xFFF;
  // SMP IPI (stubbed for single-core but structurally correct)
  extern U64 g_icr_high[NCORES];
  extern U64 g_ipi_pending[NCORES];
  extern I64 g_ncore;
  if (off == 0x310) { g_icr_high[MyCore()] = v; return; }
  if (off == 0x300) {
    I64 dest = g_icr_high[MyCore()] >> 24;
    I64 dm = (v >> 8) & 7;
    if (dm == 0 && dest >= 0 && dest < g_ncore && dest != MyCore()) {
      U64 val = (v & 0xFF) | 0x100;
      __atomic_exchange_n(&g_ipi_pending[dest], val, __ATOMIC_SEQ_CST);
    }
    return;
  }
}

//---------------------------------------------------------------- guest memory
__attribute__((always_inline)) inline
U64 RdMem(U64 a, I64 sz) {
  if (a >= mem_size) { a &= 0xFFFFFFFFFFULL; if (a >= mem_size) return MmioRd(a, sz); }
  if (sz == 8) return *(U64*)(mem + a); if (sz == 4) return *(U32*)(mem + a); if (sz == 2) return *(U16*)(mem + a);
  return mem[a];
}
__attribute__((always_inline)) inline
U0 WrMem(U64 a, I64 sz, U64 v) {
  if (a >= mem_size) { a &= 0xFFFFFFFFFFULL; if (a >= mem_size) { MmioWr(a, sz, v); return; } }
  if (sz == 8) { *(U64*)(mem + a) = v; return; }
  if (sz == 4) { *(U32*)(mem + a) = (U32)v; return; }
  if (sz == 2) { *(U16*)(mem + a) = (U16)v; return; }
  mem[a] = (U8)v;
}

//---------------------------------------------------------------- fetch / imm
__attribute__((always_inline)) inline U64 Fetch8()  { U64 b = mem[rip]; rip++; return b; }
__attribute__((always_inline)) inline U64 Fetch32() { U64 v = *(U32*)(mem + rip); rip += 4; return v; }
__attribute__((always_inline)) inline U64 Fetch64() { U64 v = RdMem(rip, 8); rip += 8; return v; }
__attribute__((always_inline)) inline I64 SignExt8(U64 b)  { b &= 0xFF; if (b & 0x80) return (I64)(b - 0x100); return (I64)b; }
__attribute__((always_inline)) inline I64 SignExt32(U64 v) { v &= 0xFFFFFFFF; if (v & 0x80000000) return (I64)(v - 0x100000000ULL); return (I64)v; }
__attribute__((always_inline)) inline U64 SizeMask(I64 sz) { if (sz == 8) return 0xFFFFFFFFFFFFFFFFULL; if (sz == 4) return 0xFFFFFFFF; if (sz == 2) return 0xFFFF; return 0xFF; }
__attribute__((always_inline)) inline I64 SignExtTo64(U64 v, I64 sz) {
  U64 m, sb; if (sz == 8) return (I64)v; m = SizeMask(sz); sb = (U64)1 << (sz * 8 - 1); v &= m; if (v & sb) return (I64)(v | ~m); return (I64)v;
}
__attribute__((always_inline)) inline I64 FetchImm(I64 sz) {
  U64 v; if (sz == 1) return SignExt8(Fetch8()); if (sz == 2) { v = RdMem(rip, 2); rip += 2; return (I64)v; } return SignExt32(Fetch32());
}

//---------------------------------------------------------------- registers
__attribute__((always_inline)) inline
U64 RdReg(I64 i, I64 sz) {
  if (sz == 8) return reg[i]; if (sz == 4) return reg[i] & 0xFFFFFFFF; if (sz == 2) return reg[i] & 0xFFFF;
  if (!hasRex && i >= 4 && i < 8) return (reg[i - 4] >> 8) & 0xFF; return reg[i] & 0xFF;
}
__attribute__((always_inline)) inline
U0 WrReg(I64 i, I64 sz, U64 v) {
  if (sz == 8) { reg[i] = v; return; }
  if (sz == 4) { reg[i] = v & 0xFFFFFFFF; return; }
  if (sz == 2) { reg[i] = (reg[i] & ~(U64)0xFFFF) | (v & 0xFFFF); return; }
  if (!hasRex && i >= 4 && i < 8) { reg[i-4] = (reg[i-4] & ~(U64)0xFF00) | ((v & 0xFF) << 8); return; }
  reg[i] = (reg[i] & ~(U64)0xFF) | (v & 0xFF);
}

//---------------------------------------------------------------- flags
__attribute__((always_inline)) inline U0 SetZSP(U64 v, I64 sz) {
  U64 m = SizeMask(sz); I64 c, k; v &= m; rfl &= ~(U64)(F_ZF | F_SF | F_PF);
  if (v == 0) rfl |= F_ZF; if ((v >> (sz*8-1)) & 1) rfl |= F_SF;
  U64 p = v & 0xFF; c = 0; for (k=0; k<8; k++) c += (p >> k) & 1; if (!(c & 1)) rfl |= F_PF;
}
__attribute__((always_inline)) inline U0 SetAdd(U64 a, U64 b, U64 r, I64 sz) {
  U64 m = SizeMask(sz), sb = (U64)1 << (sz*8-1); SetZSP(r, sz); rfl &= ~(U64)(F_CF | F_OF);
  if ((r & m) < (a & m)) rfl |= F_CF; if ((a ^ r) & (b ^ r) & sb) rfl |= F_OF;
}
__attribute__((always_inline)) inline U0 SetSub(U64 a, U64 b, U64 r, I64 sz) {
  U64 m = SizeMask(sz), sb = (U64)1 << (sz*8-1); SetZSP(r, sz); rfl &= ~(U64)(F_CF | F_OF);
  if ((a & m) < (b & m)) rfl |= F_CF; if ((a ^ b) & (a ^ r) & sb) rfl |= F_OF;
}
__attribute__((always_inline)) inline U0 SetLogic(U64 r, I64 sz) { SetZSP(r, sz); rfl &= ~(U64)(F_CF | F_OF); }

__attribute__((always_inline)) inline U64 Arith(I64 opn, U64 a, U64 b, I64 sz) {
  U64 r, m, sb, cin, full, t, rr; m = SizeMask(sz); sb = (U64)1 << (sz*8-1);
  if      (opn == 0) { r = a + b; SetAdd(a, b, r, sz); }
  else if (opn == 1) { r = a | b; SetLogic(r, sz); }
  else if (opn == 2) { // ADC
    cin = (rfl & F_CF) ? 1 : 0; r = (a + b + cin) & m; SetZSP(r, sz); rfl &= ~(U64)(F_CF | F_OF);
    if (sz == 8) { t = a + b; rr = t + cin; if ((t < a) | (rr < t)) rfl |= F_CF; }
    else { full = (a & m) + (b & m) + cin; if ((full >> (sz*8)) & 1) rfl |= F_CF; }
    if ((a ^ r) & (b ^ r) & sb) rfl |= F_OF;
  }
  else if (opn == 3) { // SBB
    cin = (rfl & F_CF) ? 1 : 0; r = (a - b - cin) & m; SetZSP(r, sz); rfl &= ~(U64)(F_CF | F_OF);
    if (((a & m) < (b & m)) | (((a & m) == (b & m)) & (cin != 0))) rfl |= F_CF;
    if ((a ^ b) & (a ^ r) & sb) rfl |= F_OF;
  }
  else if (opn == 4) { r = a & b; SetLogic(r, sz); }
  else if (opn == 5) { r = a - b; SetSub(a, b, r, sz); }
  else if (opn == 6) { r = a ^ b; SetLogic(r, sz); }
  else               { r = a - b; SetSub(a, b, r, sz); } // CMP
  return r;
}

__attribute__((always_inline)) inline I64 Cond(I64 cc) {
  I64 o=(rfl>>11)&1, c=rfl&1, z=(rfl>>6)&1, s=(rfl>>7)&1, p=(rfl>>2)&1, t=0;
  if      (cc==0) t=o;       else if (cc==1) t=!o;      else if (cc==2) t=c;       else if (cc==3) t=!c;
  else if (cc==4) t=z;       else if (cc==5) t=!z;      else if (cc==6) t=c|z;     else if (cc==7) t=!(c|z);
  else if (cc==8) t=s;       else if (cc==9) t=!s;      else if (cc==10) t=p;      else if (cc==11) t=!p;
  else if (cc==12) t=(s!=o); else if (cc==13) t=(s==o);  else if (cc==14) t=z|(s!=o); else if (cc==15) t=!(z|(s!=o));
  return t;
}

//---------------------------------------------------------------- ModRM + SIB
__attribute__((always_inline)) inline U0 DecodeModRM(I64 immlen) {
  U64 b, sib, addr = 0; I64 base_rm, ss, idxf, idx, bs;
  b = mem[rip]; rip++;
  md = (b >> 6) & 3; rg = ((b >> 3) & 7) | (rexR << 3); base_rm = b & 7;
  if (md == 3) { isMem = 0; rm = base_rm | (rexB << 3); return; }
  isMem = 1;
  if (base_rm == 4) {
    sib = Fetch8(); ss = (sib >> 6) & 3; idxf = (sib >> 3) & 7; idx = idxf | (rexX << 3); bs = (sib & 7) | (rexB << 3);
    if (!(idxf == 4 && !rexX)) addr += reg[idx] << ss;
    if ((sib & 7) == 5 && md == 0) addr += SignExt32(Fetch32()); else addr += reg[bs];
  } else if (base_rm == 5 && md == 0) {
    addr = SignExt32(Fetch32()); addr += rip + immlen;
  } else {
    addr = reg[base_rm | (rexB << 3)];
  }
  if (md == 1) addr += SignExt8(Fetch8()); else if (md == 2) addr += SignExt32(Fetch32());
  ea = addr + g_segbase;
}
__attribute__((always_inline)) inline U64 RdRM(I64 sz) { if (isMem) return RdMem(ea, sz); return RdReg(rm, sz); }
__attribute__((always_inline)) inline U0  WrRM(I64 sz, U64 v) { if (isMem) WrMem(ea, sz, v); else WrReg(rm, sz, v); }

//---------------------------------------------------------------- shifts
U0 DoShift(I64 opn, I64 cnt, I64 sz) {
  U64 v, res = 0, m; I64 last = 0, bits; m = SizeMask(sz); bits = sz * 8;
  if (sz == 8) cnt &= 63; else cnt &= 31;
  if (opn <= 3) cnt %= bits;
  if (cnt == 0) return; v = RdRM(sz) & m;
  if (opn == 4)      { res = (v << cnt) & m; last = (v >> (bits - cnt)) & 1; }
  else if (opn == 5) { res = v >> cnt; last = (v >> (cnt - 1)) & 1; }
  else if (opn == 7) { res = (U64)((I64)SignExtTo64(v, sz) >> cnt); res &= m; last = (v >> (cnt - 1)) & 1; }
  else if (opn == 0) { res = ((v << cnt) | (v >> (bits - cnt))) & m; last = res & 1; }
  else if (opn == 1) { res = ((v >> cnt) | (v << (bits - cnt))) & m; last = (res >> (bits - 1)) & 1; }
  else return;
  WrRM(sz, res); if (last) rfl |= F_CF; else rfl &= ~(U64)F_CF;
  if (opn >= 4) SetZSP(res, sz);
}

U0 DoShld(I64 isLeft, U64 src, I64 cnt, I64 sz) {
  U64 dest, res, m; I64 bits, last; m = SizeMask(sz); bits = sz * 8;
  if (sz == 8) cnt &= 63; else cnt &= 31; if (cnt == 0) return;
  dest = RdRM(sz) & m; src &= m;
  if (isLeft) { res = ((dest << cnt) | (src >> (bits - cnt))) & m; last = (dest >> (bits - cnt)) & 1; }
  else        { res = ((dest >> cnt) | (src << (bits - cnt))) & m; last = (dest >> (cnt - 1)) & 1; }
  WrRM(sz, res); if (last) rfl |= F_CF; else rfl &= ~(U64)F_CF; SetZSP(res, sz);
}

//---------------------------------------------------------------- grp3
U0 Grp3(I64 sz) {
  I64 d, bit, sa, sl, sdd; U64 a, r, imm, hi, lo, b, q, rem, bv, al, ah, bl, bh, ll, lh, hl, hh, mid;
  d = rg & 7; a = RdRM(sz);
  if (d <= 1) { imm = FetchImm(sz); SetLogic((a & imm) & SizeMask(sz), sz); return; }
  if (d == 2) { WrRM(sz, ~a); return; }
  if (d == 3) { r = 0 - a; WrRM(sz, r); SetSub(0, a, r, sz); return; }
  if (d == 4 || d == 5) {
    b = RdReg(0, sz);
    if (sz == 8) {
      al = a & 0xFFFFFFFF; ah = a >> 32; bl = b & 0xFFFFFFFF; bh = b >> 32;
      ll = al * bl; lh = al * bh; hl = ah * bl; hh = ah * bh;
      mid = (ll >> 32) + (lh & 0xFFFFFFFF) + (hl & 0xFFFFFFFF);
      lo = (ll & 0xFFFFFFFF) | (mid << 32);
      hi = hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
      if (d == 5) { if (a >> 63) hi -= b; if (b >> 63) hi -= a; }
      reg[0] = lo; reg[2] = hi;
    } else {
      a &= SizeMask(sz); b &= SizeMask(sz);
      if (d == 5) r = (U64)((I64)SignExtTo64(a, sz) * (I64)SignExtTo64(b, sz));
      else r = a * b;
      WrReg(0, sz, r & SizeMask(sz)); WrReg(2, sz, (r >> (sz * 8)) & SizeMask(sz));
    }
    return;
  }
  if (a == 0) { halted = 3; return; }
  if (sz == 8) {
    hi = reg[2]; lo = reg[0];
    if (d == 6 && hi == 0) { reg[0] = lo / a; reg[2] = lo % a; return; }
    if (d == 7) { sl = (I64)lo; sa = (I64)a; reg[0] = (U64)(sl / sa); reg[2] = (U64)(sl % sa); return; }
    q = 0; rem = 0;
    for (bit = 127; bit >= 0; bit--) { rem <<= 1; if (bit >= 64) bv = (hi >> (bit - 64)) & 1; else bv = (lo >> bit) & 1; rem |= bv; if (rem >= a) { rem -= a; q |= (U64)1 << bit; } }
    reg[0] = q; reg[2] = rem;
  } else {
    hi = RdReg(2, sz); lo = RdReg(0, sz);
    if (d == 6) { r = (hi << (sz * 8)) | lo; WrReg(0, sz, r / a); WrReg(2, sz, r % a); }
    else { sdd = (SignExtTo64(hi, sz) << (sz * 8)) | (I64)(lo & SizeMask(sz)); sa = SignExtTo64(a, sz);
           WrReg(0, sz, (U64)(sdd / sa)); WrReg(2, sz, (U64)(sdd % sa)); }
  }
}

//---------------------------------------------------------------- string ops
U0 StrStep(I64 op, I64 sz) {
  I64 d; U64 a, b; if (rfl & F_DF) d = -sz; else d = sz;
  if      (op == 0xA4 || op == 0xA5) { WrMem(reg[7], sz, RdMem(reg[6], sz)); reg[6] += d; reg[7] += d; }
  else if (op == 0xAA || op == 0xAB) { WrMem(reg[7], sz, RdReg(0, sz)); reg[7] += d; }
  else if (op == 0xAC || op == 0xAD) { WrReg(0, sz, RdMem(reg[6], sz)); reg[6] += d; }
  else if (op == 0xA6 || op == 0xA7) { a = RdMem(reg[6], sz); b = RdMem(reg[7], sz); SetSub(a, b, a-b, sz); reg[6] += d; reg[7] += d; }
  else if (op == 0xAE || op == 0xAF) { a = RdReg(0, sz); b = RdMem(reg[7], sz); SetSub(a, b, a-b, sz); reg[7] += d; }
}
U0 DoStr(I64 op, I64 sz) {
  I64 cmpkind = (op == 0xA6 || op == 0xA7 || op == 0xAE || op == 0xAF);
  if (!repf) { StrStep(op, sz); return; }
  if (g_segbase == 0 && !(rfl & F_DF) && reg[1] > 0) {
    U64 n = reg[1], total = n * sz, i;
    if (op == 0xA4 || op == 0xA5) {
      U64 s = reg[6], dt = reg[7];
      if (s + total <= mem_size && dt + total <= mem_size) {
        if (sz == 8) for (i=0; i<n; i++) *(U64*)(mem+dt+i*8) = *(U64*)(mem+s+i*8);
        else for (i=0; i<total; i++) mem[dt+i] = mem[s+i];
        reg[6] = s + total; reg[7] = dt + total; reg[1] = 0; return;
      }
    } else if (op == 0xAA || op == 0xAB) {
      U64 dt = reg[7], val = RdReg(0, sz);
      if (dt + total <= mem_size) {
        if (sz == 8) for (i=0; i<n; i++) *(U64*)(mem+dt+i*8) = val;
        else if (sz == 1) for (i=0; i<total; i++) mem[dt+i] = (U8)val;
        else for (i=0; i<n; i++) WrMem(dt+i*sz, sz, val);
        reg[7] = dt + total; reg[1] = 0; return;
      }
    }
  }
  while (reg[1] != 0) {
    StrStep(op, sz); reg[1]--;
    if (cmpkind) { if (repf == 1 && !(rfl & F_ZF)) break; if (repf == 2 && (rfl & F_ZF)) break; }
  }
}

//---------------------------------------------------------------- system
U64 RdMsr(U64 i) {
  if (i==0xC0000080) return msr_efer; if (i==0xC0000100) return msr_fsbase; if (i==0xC0000101) return msr_gsbase;
  if (i==0xC0000102) return msr_kgsbase; if (i==0xC0000081) return msr_star; if (i==0xC0000082) return msr_lstar;
  if (i==0xC0000084) return msr_sfmask; if (i==0x1B) return msr_apic; return 0;
}
U0 WrMsr(U64 i, U64 v) {
  if      (i==0xC0000080) msr_efer=v; else if (i==0xC0000100) msr_fsbase=v; else if (i==0xC0000101) msr_gsbase=v;
  else if (i==0xC0000102) msr_kgsbase=v; else if (i==0xC0000081) msr_star=v; else if (i==0xC0000082) msr_lstar=v;
  else if (i==0xC0000084) msr_sfmask=v; else if (i==0x1B) msr_apic=v;
}
U0 Cpuid() {
  U64 a = reg[0] & 0xFFFFFFFF;
  if (a==0) { reg[0]=0x0000000D; reg[3]=0x756E6547; reg[2]=0x6C65746E; reg[1]=0x49656E69; }
  else if (a==1) { reg[0]=0x000006FB; reg[1]=0; reg[2]=0x00000201; reg[3]=0x178BFBFF; }
  else if (a==0x80000000) { reg[0]=0x80000008; reg[1]=0; reg[2]=0; reg[3]=0; }
  else if (a==0x80000001) { reg[0]=0; reg[1]=0; reg[2]=0x00000001; reg[3]=0x28100800; }
  else { reg[0]=0; reg[1]=0; reg[2]=0; reg[3]=0; }
}
U64 RtcRead(I64 idx) { if (idx==0x0A) return 0; if (idx==0x0B) return 0x06; return host_time(idx); }

//---------------------------------------------------------------- I/O
U64 IoIn(U64 port) {
  if (port==0x21) return pic1_mask; if (port==0xA1) return pic2_mask;
  if (port==0x40) return pit_counter & 0xFF; if (port==0x71) return RtcRead(rtc_idx);
  if (port==0x60) {
    if (ps2qh != ps2qt) { I64 b = ps2q[ps2qh & 15]; ps2qh++; return b; }
    if (ms_has) {
      I64 mb; if (ms_phase==0) mb=ms_byte0; else if (ms_phase==1) mb=ms_byte1; else mb=ms_byte2;
      ms_phase++; if (ms_phase >= 3) { ms_has=0; ms_phase=0; } else pic2_irr |= 0x10;
      return mb;
    }
    if (kbd_qh != kbd_qt) { I64 sc = kbd_q[kbd_qh & 63]; kbd_qh++; if (kbd_qh != kbd_qt) pic1_irr |= 2; return sc; }
    return 0;
  }
  if (port==0x64) {
    I64 st = 0x1C, nextms = 0;
    if (ps2qh != ps2qt) { st |= 0x01; if (ps2q_ms[ps2qh & 15]) nextms = 1; }
    else if (ms_has) { st |= 0x01; nextms = 1; }
    else if (kbd_qh != kbd_qt) st |= 0x01;
    if (nextms) st |= 0x20;
    return st;
  }
  if (port==0x1F7 || port==0x177) return ata_words > 0 ? 0x48 : 0x40;
  if (port==0x1F0 || port==0x170) {
    if (ata_words > 0) { U64 w = *(U16*)(ata_buf + ata_pos); ata_pos += 2; ata_words--; return w; }
    return 0;
  }
  return 0;
}

U0 IoOut(U64 port, U64 v) {
  I64 i;
  if (port >= 0x3B0 && port <= 0x3DF) return;
  if (port==0x1F0 || port==0x170) {
    if (ata_wr && ata_words > 0) { *(U16*)(ata_buf + ata_pos) = (U16)v; ata_pos += 2; ata_words--;
      if (ata_words == 0) { host_disk_wr(ata_wlba, ata_wcnt, ata_buf); ata_wr = 0; } }
    return;
  }
  v &= 0xFF;
  if (port==0x1F2||port==0x172) { ata_seccnt = ((ata_seccnt << 8) | v) & 0xFFFF; return; }
  if (port==0x1F3||port==0x173) { ata_r3 = ((ata_r3 << 8) | v) & 0xFFFF; return; }
  if (port==0x1F4||port==0x174) { ata_r4 = ((ata_r4 << 8) | v) & 0xFFFF; return; }
  if (port==0x1F5||port==0x175) { ata_r5 = ((ata_r5 << 8) | v) & 0xFFFF; return; }
  if (port==0x1F7||port==0x177) {
    U64 lba = (ata_r3&0xFF)|((ata_r4&0xFF)<<8)|((ata_r5&0xFF)<<16)
             |(((ata_r3>>8)&0xFF)<<24)|(((ata_r4>>8)&0xFF)<<32)|(((ata_r5>>8)&0xFF)<<40);
    U64 cnt = ata_seccnt & 0xFFFF; if (!cnt) cnt = 256; if (cnt > 256) cnt = 256;
    if (v==0x20||v==0x21||v==0x24||v==0x29||v==0xC4) { host_disk(lba, cnt, ata_buf); ata_words = cnt * 256; ata_pos = 0; ata_wr = 0; }
    else if (v==0x30||v==0x34||v==0x39||v==0xC5) { ata_words = cnt * 256; ata_pos = 0; ata_wr = 1; ata_wlba = lba; ata_wcnt = cnt; }
    else { ata_words = 0; ata_wr = 0; }
    ata_r3=0; ata_r4=0; ata_r5=0; ata_seccnt=0; return;
  }
  if (port==0x64) {
    if (v==0xD4) ms_next=1; else if (v==0x20) Ps2Resp(0x47,0); else if (v==0x60) cfg_next=1;
    else if (v==0xA9||v==0xAB) Ps2Resp(0x00,0); else if (v==0xAA) Ps2Resp(0x55,0);
    return;
  }
  if (port==0x60) {
    if (cfg_next) { cfg_next=0; return; }
    if (ms_next) { ms_next=0; Ps2Resp(0xFA,1); if (v==0xFF) { Ps2Resp(0xAA,1); Ps2Resp(0x00,1); } else if (v==0xF2) Ps2Resp(0x00,1); return; }
    Ps2Resp(0xFA,0); if (v==0xFF) Ps2Resp(0xAA,0); return;
  }
  if (port==0x70) { rtc_idx = v & 0x7F; return; }
  if (port==0x43) { if ((v&0xC0)==0x80) pit2_wlo=1; return; }
  if (port==0x42) { if (pit2_wlo) { pit2_reload=v; pit2_wlo=0; } else { pit2_reload|=v<<8; pit2_div=pit2_reload; pit2_wlo=1; UpdateSpk(); } return; }
  if (port==0x61) { spk_on = (v & 3) == 3; UpdateSpk(); return; }
  if (port==0x20) { if (v&0x10) pic1_initstep=1; else if (v==0x20) { for (i=0;i<8;i++) if (pic1_isr&(1<<i)) { pic1_isr&=~(1<<i); break; } } }
  else if (port==0x21) { if (pic1_initstep==1) { pic1_base=v; pic1_initstep=2; } else if (pic1_initstep==2) pic1_initstep=3; else if (pic1_initstep==3) pic1_initstep=0; else pic1_mask=v; }
  else if (port==0xA0) { if (v&0x10) pic2_initstep=1; else if (v==0x20) { for (i=0;i<8;i++) if (pic2_isr&(1<<i)) { pic2_isr&=~(1<<i); break; } } }
  else if (port==0xA1) { if (pic2_initstep==1) { pic2_base=v; pic2_initstep=2; } else if (pic2_initstep==2) pic2_initstep=3; else if (pic2_initstep==3) pic2_initstep=0; else pic2_mask=v; }
  else if (port==0x43) { pit_wlo=1; }
  else if (port==0x40) { if (pit_wlo) { pit_reload=v; pit_wlo=0; } else { pit_reload|=(v<<8); pit_div=pit_reload; if (!pit_div) pit_div=65536; pit_counter=pit_div; pit_wlo=1; } }
}

U0 DoIns(I64 sz) { I64 d; if (rfl & F_DF) d = -sz; else d = sz; if (!repf) { WrMem(reg[7], sz, IoIn(reg[2]&0xFFFF)); reg[7]+=d; return; } while (reg[1]!=0) { WrMem(reg[7], sz, IoIn(reg[2]&0xFFFF)); reg[7]+=d; reg[1]--; } }
U0 DoOuts(I64 sz) { I64 d; if (rfl & F_DF) d = -sz; else d = sz; if (!repf) { IoOut(reg[2]&0xFFFF, RdMem(reg[6], sz)); reg[6]+=d; return; } while (reg[1]!=0) { IoOut(reg[2]&0xFFFF, RdMem(reg[6], sz)); reg[6]+=d; reg[1]--; } }

U0 CheckIrq() {
  I64 irq, b; b = pic1_irr & ~pic1_mask;
  for (irq=0; irq<8; irq++) if (b & (1<<irq)) { if (irq==2) break; pic1_irr &= ~(1<<irq); pic1_isr |= (1<<irq); irq_count++; DoInt(pic1_base+irq, 0, 0); return; }
  b = pic2_irr & ~pic2_mask;
  for (irq=0; irq<8; irq++) if (b & (1<<irq)) { pic2_irr &= ~(1<<irq); pic2_isr |= (1<<irq); irq_count++; DoInt(pic2_base+irq, 0, 0); return; }
}
U0 Grp7() {
  I64 d = rg & 7; if (!isMem) return;
  if      (d==2) { gdtr_limit = RdMem(ea, 2); gdtr_base = RdMem(ea+2, 8); }
  else if (d==3) { idtr_limit = RdMem(ea, 2); idtr_base = RdMem(ea+2, 8); }
  else if (d==0) { WrMem(ea, 2, gdtr_limit); WrMem(ea+2, 8, gdtr_base); }
  else if (d==1) { WrMem(ea, 2, idtr_limit); WrMem(ea+2, 8, idtr_base); }
}
U0 DoInt(I64 vec, I64 hasErr, U64 err) {
  U64 gate, off, sel, oldsp; gate = idtr_base + vec * 16;
  off = RdMem(gate, 2) | (RdMem(gate+6, 2) << 16) | (RdMem(gate+8, 4) << 32);
  sel = RdMem(gate+2, 2); oldsp = reg[4];
  reg[4] -= 8; WrMem(reg[4], 8, g_ss); reg[4] -= 8; WrMem(reg[4], 8, oldsp); reg[4] -= 8; WrMem(reg[4], 8, rfl);
  reg[4] -= 8; WrMem(reg[4], 8, g_cs); reg[4] -= 8; WrMem(reg[4], 8, rip);
  if (hasErr) { reg[4] -= 8; WrMem(reg[4], 8, err); }
  rfl &= ~(U64)F_IF; g_cs = sel; rip = off;
}
U0 DoIret() {
  U64 newsp; rip = RdMem(reg[4]+0, 8); g_cs = RdMem(reg[4]+8, 8); rfl = RdMem(reg[4]+16, 8);
  newsp = RdMem(reg[4]+24, 8); g_ss = RdMem(reg[4]+32, 8); reg[4] = newsp;
}

//---------------------------------------------------------------- FPU helpers
F64 IntToF64(I64 v) { return (F64)v; }
I64 F64ToInt(F64 d) { return (I64)d; }
I64 RoundI(F64 d) { if (d >= 0) return F64ToInt(d + 0.5); return F64ToInt(d - 0.5); }
I64 RoundCW(F64 d) {
  I64 rc = (x87_cw >> 10) & 3, i;
  if (rc == 0) return RoundI(d);
  if (rc == 3) return F64ToInt(d);
  i = F64ToInt(d);
  if (rc == 1) { if (IntToF64(i) > d) i--; }
  else         { if (IntToF64(i) < d) i++; }
  return i;
}
F64 F32ToF64(U64 b) {
  U64 s, e, f, db; s = (b >> 31) & 1; e = (b >> 23) & 0xFF; f = b & 0x7FFFFF;
  if (e == 0) db = s << 63;
  else if (e == 0xFF) db = (s << 63) | ((U64)0x7FF << 52) | (f << 29);
  else db = (s << 63) | ((U64)(e - 127 + 1023) << 52) | (f << 29);
  return BitsToF64(db);
}
U64 F64ToF32b(F64 d) {
  U64 b, s, f; I64 e, ne; b = F64ToBits(d); s = (b >> 63) & 1; e = (b >> 52) & 0x7FF; f = b & 0xFFFFFFFFFFFFFULL;
  if (e == 0) return (U32)(s << 31); if (e == 0x7FF) return (U32)((s << 31) | (0xFFu << 23) | (U32)(f >> 29));
  ne = e - 1023 + 127; if (ne <= 0) return (U32)(s << 31); if (ne >= 0xFF) return (U32)((s << 31) | (0xFFu << 23));
  return (U32)((s << 31) | (ne << 23) | (U32)(f >> 29));
}
U0  FPush(F64 v)         { fsp = (fsp - 1) & 7; fpr[fsp] = v; }
F64 FPopv()              { F64 v = fpr[fsp]; fsp = (fsp + 1) & 7; return v; }
F64 FStv(I64 i)          { return fpr[(fsp + i) & 7]; }
U0  FStset(I64 i, F64 v) { fpr[(fsp + i) & 7] = v; }
U0  FCmpSW(F64 a, F64 b) { x87_sw &= ~(U64)0x4500; if (a != a || b != b) x87_sw |= 0x4500; else if (a < b) x87_sw |= 0x0100; else if (a == b) x87_sw |= 0x4000; }
U0  FCmpFL(F64 a, F64 b) { rfl &= ~(U64)(F_ZF|F_PF|F_CF); if (a != a || b != b) rfl |= (F_ZF|F_PF|F_CF); else if (a == b) rfl |= F_ZF; else if (a < b) rfl |= F_CF; }
F64 SqrtF64(F64 d) { return sqrt(d); }
U64 RdXmm64() { if (isMem) return RdMem(ea, 8); return xmm_lo[rm]; }

//---------------------------------------------------------------- x87 FPU
U0 OpX87(U64 op) {
  I64 sub, sti, bcd_i, bcd_d, bcd_ng; F64 a, b, r; DecodeModRM(); sub = rg & 7; sti = rm & 7;
  if (isMem) {
    if (op == 0xD9) {
      if (sub==0) FPush(F32ToF64(RdMem(ea,4)));
      else if (sub==2) WrMem(ea, 4, F64ToF32b(FStv(0)));
      else if (sub==3) { WrMem(ea, 4, F64ToF32b(FStv(0))); FPopv(); }
      else if (sub==5) x87_cw = RdMem(ea, 2);
      else if (sub==7) WrMem(ea, 2, x87_cw);
    } else if (op == 0xDB) {
      if (sub==0) FPush(IntToF64(SignExtTo64(RdMem(ea,4),4)));
      else if (sub==1) { WrMem(ea, 4, (U64)F64ToInt(FStv(0))); FPopv(); }
      else if (sub==2) WrMem(ea, 4, (U64)RoundCW(FStv(0)));
      else if (sub==3) { WrMem(ea, 4, (U64)RoundCW(FStv(0))); FPopv(); }
      else if (sub==5) FPush(BitsToF64(RdMem(ea,8)));
      else if (sub==7) { WrMem(ea, 8, F64ToBits(FStv(0))); FPopv(); }
    } else if (op == 0xDD) {
      if (sub==0) FPush(BitsToF64(RdMem(ea,8)));
      else if (sub==1) { WrMem(ea, 8, (U64)F64ToInt(FStv(0))); FPopv(); }
      else if (sub==2) WrMem(ea, 8, F64ToBits(FStv(0)));
      else if (sub==3) { WrMem(ea, 8, F64ToBits(FStv(0))); FPopv(); }
    } else if (op == 0xDF) {
      if (sub==0) FPush(IntToF64(SignExtTo64(RdMem(ea,2),2)));
      else if (sub==1) { WrMem(ea, 2, (U64)F64ToInt(FStv(0))); FPopv(); }
      else if (sub==2) WrMem(ea, 2, (U64)RoundCW(FStv(0)));
      else if (sub==3) { WrMem(ea, 2, (U64)RoundCW(FStv(0))); FPopv(); }
      else if (sub==5) FPush(IntToF64((I64)RdMem(ea,8)));
      else if (sub==4) { bcd_i=0; for (bcd_d=8; bcd_d>=0; bcd_d--) { bcd_ng=RdMem(ea+bcd_d,1); bcd_i=bcd_i*100+(bcd_ng>>4)*10+(bcd_ng&15); } if (RdMem(ea+9,1)&0x80) bcd_i=-bcd_i; FPush(IntToF64(bcd_i)); }
      else if (sub==6) { bcd_i=RoundCW(FStv(0)); bcd_ng=0; if (bcd_i<0) { bcd_ng=1; bcd_i=-bcd_i; } for (bcd_d=0; bcd_d<9; bcd_d++) { WrMem(ea+bcd_d,1,(bcd_i%10)|((bcd_i/10%10)<<4)); bcd_i/=100; } WrMem(ea+9,1,bcd_ng?0x80:0); FPopv(); }
      else if (sub==7) { WrMem(ea, 8, (U64)RoundCW(FStv(0))); FPopv(); }
    } else if (op == 0xD8 || op == 0xDC) {
      if (op==0xD8) b = F32ToF64(RdMem(ea,4)); else b = BitsToF64(RdMem(ea,8)); a = FStv(0);
      if (sub==2||sub==3) { FCmpSW(a, b); if (sub==3) FPopv(); return; }
      if (sub==0) r=a+b; else if (sub==1) r=a*b; else if (sub==4) r=a-b; else if (sub==5) r=b-a; else if (sub==6) r=a/b; else r=b/a; FStset(0, r);
    } else if (op == 0xDA || op == 0xDE) {
      if (op==0xDA) b = IntToF64(SignExtTo64(RdMem(ea,4),4)); else b = IntToF64(SignExtTo64(RdMem(ea,2),2));
      a = FStv(0); if (sub==0) r=a+b; else if (sub==1) r=a*b; else if (sub==4) r=a-b; else if (sub==5) r=b-a; else if (sub==6) r=a/b; else r=b/a; FStset(0, r);
    }
    return;
  }
  // register forms
  if (op == 0xD9) {
    if (sub==0) FPush(FStv(sti));
    else if (sub==1) { a = FStv(0); FStset(0, FStv(sti)); FStset(sti, a); }
    else if (sub==4) { if (sti==0) FStset(0, -FStv(0)); else if (sti==1) { a=FStv(0); if (a<0) FStset(0,-a); } }
    else if (sub==5) { if (sti==0) FPush(1.0); else if (sti==1) FPush(3.32192809488736235); else if (sti==2) FPush(1.44269504088896341); else if (sti==3) FPush(3.14159265358979324); else if (sti==4) FPush(0.30102999566398120); else if (sti==5) FPush(0.69314718055994531); else if (sti==6) FPush(0.0); }
    else if (sub==6) {
      if (sti==0) FStset(0, pow(2.0, FStv(0)) - 1.0);
      else if (sti==1) { r = FStv(1) * log(FStv(0)) * 1.44269504088896341; FPopv(); FStset(0, r); }
      else if (sti==2) { FStset(0, tan(FStv(0))); FPush(1.0); }
      else if (sti==3) { r = atan2(FStv(1), FStv(0)); FPopv(); FStset(0, r); }
      else if (sti==4) { a=FStv(0); if (a==0.0) { FStset(0,0.0); FPush(0.0); } else { b=a; if (b<0) b=-b; bcd_i=F64ToInt(log(b)*1.44269504088896341); if (IntToF64(bcd_i)>log(b)*1.44269504088896341) bcd_i--; FStset(0,IntToF64(bcd_i)); FPush(a/pow(2.0,IntToF64(bcd_i))); } }
      else if (sti==6) fsp = (fsp - 1) & 7;
      else if (sti==7) fsp = (fsp + 1) & 7;
    }
    else if (sub==7) {
      if (sti==0) { a=FStv(0); b=FStv(1); FStset(0, a - IntToF64(F64ToInt(a/b))*b); }
      else if (sti==1) { r = FStv(1) * log(FStv(0)+1.0) * 1.44269504088896341; FPopv(); FStset(0, r); }
      else if (sti==2) FStset(0, SqrtF64(FStv(0)));
      else if (sti==3) { a=FStv(0); FStset(0, sin(a)); FPush(cos(a)); }
      else if (sti==4) FStset(0, IntToF64(RoundCW(FStv(0))));
      else if (sti==5) FStset(0, FStv(0) * pow(2.0, IntToF64(F64ToInt(FStv(1)))));
      else if (sti==6) FStset(0, sin(FStv(0)));
      else if (sti==7) FStset(0, cos(FStv(0)));
    }
  } else if (op == 0xD8) {
    a = FStv(0); b = FStv(sti);
    if (sub==2||sub==3) { FCmpSW(a, b); if (sub==3) FPopv(); }
    else if (sub==0) FStset(0,a+b); else if (sub==1) FStset(0,a*b); else if (sub==4) FStset(0,a-b); else if (sub==5) FStset(0,b-a); else if (sub==6) FStset(0,a/b); else FStset(0,b/a);
  } else if (op == 0xDC) {
    a = FStv(sti); b = FStv(0);
    if (sub==0) FStset(sti,a+b); else if (sub==1) FStset(sti,a*b); else if (sub==4) FStset(sti,a-b); else if (sub==5) FStset(sti,b-a); else if (sub==6) FStset(sti,a/b); else if (sub==7) FStset(sti,b/a);
  } else if (op == 0xDD) {
    if (sub==2) FStset(sti, FStv(0));
    else if (sub==3) { FStset(sti, FStv(0)); FPopv(); }
    else if (sub==4||sub==5) { FCmpSW(FStv(0), FStv(sti)); if (sub==5) FPopv(); }
  } else if (op == 0xDE) {
    a = FStv(sti); b = FStv(0);
    if (sub==3) { FCmpSW(FStv(0), FStv(1)); FPopv(); FPopv(); }
    else { if (sub==0) FStset(sti,a+b); else if (sub==1) FStset(sti,a*b); else if (sub==4) FStset(sti,a-b); else if (sub==5) FStset(sti,b-a); else if (sub==6) FStset(sti,a/b); else FStset(sti,b/a); FPopv(); }
  } else if (op == 0xDB) {
    if (sub==4) { if (sti==3) { fsp=0; x87_cw=0x37F; x87_sw=0; } }
    else if (sub==6) FCmpFL(FStv(0), FStv(sti));
    else if (sub==7) FCmpFL(FStv(0), FStv(sti));
  } else if (op == 0xDF) {
    if (sub==4 && sti==0) reg[0] = (reg[0] & ~(U64)0xFFFF) | (x87_sw & 0xFFFF);
    else if (sub==6) { FCmpFL(FStv(0), FStv(sti)); FPopv(); }
    else if (sub==7) { FCmpFL(FStv(0), FStv(sti)); FPopv(); }
  }
}

//---------------------------------------------------------------- bit ops
U0 DoBit(I64 which, U64 bit, I64 sz) {
  U64 v, m, old; I64 asz; asz = sz;
  if (isMem) { ea += bit >> 3; bit &= 7; asz = 1; } else bit &= (sz * 8 - 1);
  m = (U64)1 << bit;
  if (lockf && isMem && which && ea < mem_size) {
    if (which==1)      old = AtomOr(ea, m, asz);
    else if (which==2) old = AtomAnd(ea, ~m, asz);
    else               old = AtomXor(ea, m, asz);
    if (old & m) rfl |= F_CF; else rfl &= ~(U64)F_CF; return;
  }
  v = RdRM(asz); if (v & m) rfl |= F_CF; else rfl &= ~(U64)F_CF;
  if (which==1) WrRM(asz, v | m); else if (which==2) WrRM(asz, v & ~m); else if (which==3) WrRM(asz, v ^ m);
}

//---------------------------------------------------------------- FXSAVE / FXRSTOR
U0 DoFxsave(U64 a) {
  I64 i;
  WrMem(a+0, 2, x87_cw); WrMem(a+2, 2, x87_sw); WrMem(a+24, 4, 0x1F80);
  for (i=0; i<8; i++) WrMem(a+32+i*16, 8, F64ToBits(fpr[i]));
  for (i=0; i<16; i++) { WrMem(a+160+i*16, 8, xmm_lo[i]); WrMem(a+160+i*16+8, 8, xmm_hi[i]); }
}
U0 DoFxrstor(U64 a) {
  I64 i; x87_cw = RdMem(a+0, 2); x87_sw = RdMem(a+2, 2);
  for (i=0; i<8; i++) fpr[i] = BitsToF64(RdMem(a+32+i*16, 8));
  for (i=0; i<16; i++) { xmm_lo[i] = RdMem(a+160+i*16, 8); xmm_hi[i] = RdMem(a+160+i*16+8, 8); }
}

//---------------------------------------------------------------- 0F two-byte opcodes
U0 Op0F() {
  U64 op2, a, b, r, lo, hi, se, al, ah, bl, bh, ll, lh, hl, mid, v; I64 sz, cc, d, isz; F64 fx, fy;
  op2 = Fetch8(); if (rexW) sz = 8; else if (pfx66) sz = 2; else sz = 4;
  if (op2 >= 0x80 && op2 <= 0x8F) { cc = op2 - 0x80; d = SignExt32(Fetch32()); if (Cond(cc)) rip += d; return; }
  if (op2 >= 0x90 && op2 <= 0x9F) { cc = op2 - 0x90; DecodeModRM(); if (Cond(cc)) WrRM(1, 1); else WrRM(1, 0); return; }
  if (op2 >= 0x40 && op2 <= 0x4F) { cc = op2 - 0x40; DecodeModRM(); a = RdRM(sz); if (Cond(cc)) WrReg(rg, sz, a); return; }
  switch (op2) {
    case 0x1F: DecodeModRM(); break;
    case 0xB6: DecodeModRM(); WrReg(rg, sz, RdRM(1) & 0xFF); break;
    case 0xB7: DecodeModRM(); WrReg(rg, sz, RdRM(2) & 0xFFFF); break;
    case 0xBE: DecodeModRM(); WrReg(rg, sz, (U64)SignExtTo64(RdRM(1), 1)); break;
    case 0xBF: DecodeModRM(); WrReg(rg, sz, (U64)SignExtTo64(RdRM(2), 2)); break;
    case 0xBC: DecodeModRM(); a = RdRM(sz);
      if (!a) rfl |= F_ZF; else { rfl &= ~(U64)F_ZF; b=0; while (!((a>>b)&1)) b++; WrReg(rg, sz, b); } break;
    case 0xBD: DecodeModRM(); a = RdRM(sz);
      if (!a) rfl |= F_ZF; else { rfl &= ~(U64)F_ZF; b=sz*8-1; while (!((a>>b)&1)) b--; WrReg(rg, sz, b); } break;
    case 0xAF: DecodeModRM(); a = RdReg(rg, sz); b = RdRM(sz);
      if (sz == 8) {
        al=a&0xFFFFFFFF; ah=a>>32; bl=b&0xFFFFFFFF; bh=b>>32; ll=al*bl; lh=al*bh; hl=ah*bl;
        mid=(ll>>32)+(lh&0xFFFFFFFF)+(hl&0xFFFFFFFF); lo=(ll&0xFFFFFFFF)|(mid<<32);
        hi=(ah*bh)+(lh>>32)+(hl>>32)+(mid>>32); if (a>>63) hi-=b; if (b>>63) hi-=a;
        r=lo; WrReg(rg, 8, r); rfl &= ~(U64)(F_CF|F_OF); se=0; if (r>>63) se=0xFFFFFFFFFFFFFFFFULL; if (hi!=se) rfl|=(F_CF|F_OF);
      } else {
        r = (U64)((I64)SignExtTo64(a,sz) * (I64)SignExtTo64(b,sz)); WrReg(rg, sz, r & SizeMask(sz));
        rfl &= ~(U64)(F_CF|F_OF); if (SignExtTo64(r & SizeMask(sz), sz) != (I64)r) rfl |= (F_CF|F_OF);
      } break;
    // SSE scalar F64
    case 0x10: DecodeModRM();
      if (repf==2) { xmm_lo[rg] = RdXmm64(); if (isMem) xmm_hi[rg]=0; }
      else if (repf==1) { xmm_lo[rg] = (xmm_lo[rg] & ~(U64)0xFFFFFFFF) | (RdXmm64() & 0xFFFFFFFF); }
      else if (isMem) { xmm_lo[rg]=RdMem(ea,8); xmm_hi[rg]=RdMem(ea+8,8); } else { xmm_lo[rg]=xmm_lo[rm]; xmm_hi[rg]=xmm_hi[rm]; }
      break;
    case 0x11: DecodeModRM();
      if (repf==2) { if (isMem) WrMem(ea,8,xmm_lo[rg]); else xmm_lo[rm]=xmm_lo[rg]; }
      else if (repf==1) { if (isMem) WrMem(ea,4,xmm_lo[rg]&0xFFFFFFFF); else xmm_lo[rm]=(xmm_lo[rm]&~(U64)0xFFFFFFFF)|(xmm_lo[rg]&0xFFFFFFFF); }
      else if (isMem) { WrMem(ea,8,xmm_lo[rg]); WrMem(ea+8,8,xmm_hi[rg]); } else { xmm_lo[rm]=xmm_lo[rg]; xmm_hi[rm]=xmm_hi[rg]; }
      break;
    case 0x28: case 0x6F: DecodeModRM();
      if (isMem) { xmm_lo[rg]=RdMem(ea,8); xmm_hi[rg]=RdMem(ea+8,8); } else { xmm_lo[rg]=xmm_lo[rm]; xmm_hi[rg]=xmm_hi[rm]; } break;
    case 0x29: case 0x7F: DecodeModRM();
      if (isMem) { WrMem(ea,8,xmm_lo[rg]); WrMem(ea+8,8,xmm_hi[rg]); } else { xmm_lo[rm]=xmm_lo[rg]; xmm_hi[rm]=xmm_hi[rg]; } break;
    case 0x6E: DecodeModRM(); if (rexW) xmm_lo[rg]=RdRM(8); else xmm_lo[rg]=RdRM(4)&0xFFFFFFFF; xmm_hi[rg]=0; break;
    case 0x7E: DecodeModRM();
      if (repf==1) { xmm_lo[rg]=RdXmm64(); xmm_hi[rg]=0; }
      else if (rexW) WrRM(8, xmm_lo[rg]); else WrRM(4, xmm_lo[rg] & 0xFFFFFFFF);
      break;
    case 0xD6: DecodeModRM(); if (isMem) WrMem(ea,8,xmm_lo[rg]); else { xmm_lo[rm]=xmm_lo[rg]; xmm_hi[rm]=0; } break;
    case 0x2A: DecodeModRM(); if (rexW) isz=8; else isz=4; xmm_lo[rg]=F64ToBits(IntToF64(SignExtTo64(RdRM(isz),isz))); break;
    case 0x2C: case 0x2D: DecodeModRM(); if (rexW) isz=8; else isz=4; WrReg(rg,isz,(U64)F64ToInt(BitsToF64(RdXmm64()))); break;
    case 0x51: DecodeModRM(); xmm_lo[rg]=F64ToBits(SqrtF64(BitsToF64(RdXmm64()))); break;
    case 0x58: DecodeModRM(); xmm_lo[rg]=F64ToBits(BitsToF64(xmm_lo[rg])+BitsToF64(RdXmm64())); break;
    case 0x59: DecodeModRM(); xmm_lo[rg]=F64ToBits(BitsToF64(xmm_lo[rg])*BitsToF64(RdXmm64())); break;
    case 0x5C: DecodeModRM(); xmm_lo[rg]=F64ToBits(BitsToF64(xmm_lo[rg])-BitsToF64(RdXmm64())); break;
    case 0x5E: DecodeModRM(); xmm_lo[rg]=F64ToBits(BitsToF64(xmm_lo[rg])/BitsToF64(RdXmm64())); break;
    case 0x5D: DecodeModRM(); fx=BitsToF64(xmm_lo[rg]); fy=BitsToF64(RdXmm64()); if (fy<fx) xmm_lo[rg]=F64ToBits(fy); break;
    case 0x5F: DecodeModRM(); fx=BitsToF64(xmm_lo[rg]); fy=BitsToF64(RdXmm64()); if (fy>fx) xmm_lo[rg]=F64ToBits(fy); break;
    case 0x2E: case 0x2F: DecodeModRM();
      fx=BitsToF64(xmm_lo[rg]); fy=BitsToF64(RdXmm64()); rfl &= ~(U64)(F_ZF|F_PF|F_CF|F_SF|F_OF);
      if (fx!=fx||fy!=fy) rfl|=(F_ZF|F_PF|F_CF); else if (fx==fy) rfl|=F_ZF; else if (fx<fy) rfl|=F_CF; break;
    case 0x57: case 0xEF: DecodeModRM();
      if (isMem) { xmm_lo[rg]^=RdMem(ea,8); xmm_hi[rg]^=RdMem(ea+8,8); } else { xmm_lo[rg]^=xmm_lo[rm]; xmm_hi[rg]^=xmm_hi[rm]; } break;
    case 0x20: DecodeModRM(); if (rg==0) reg[rm]=cr0; else if (rg==2) reg[rm]=cr2; else if (rg==3) reg[rm]=cr3; else if (rg==4) reg[rm]=cr4; else if (rg==8) reg[rm]=cr8; else reg[rm]=0; break;
    case 0x22: DecodeModRM(); if (rg==0) cr0=reg[rm]; else if (rg==2) cr2=reg[rm]; else if (rg==3) cr3=reg[rm]; else if (rg==4) cr4=reg[rm]; else if (rg==8) cr8=reg[rm]; break;
    case 0x21: DecodeModRM(); reg[rm]=0; break;
    case 0x23: DecodeModRM(); break;
    case 0x31: tsc+=100; reg[0]=tsc&0xFFFFFFFF; reg[2]=(tsc>>32)&0xFFFFFFFF; break;
    case 0x30: WrMsr(reg[1]&0xFFFFFFFF, ((reg[2]&0xFFFFFFFF)<<32)|(reg[0]&0xFFFFFFFF)); break;
    case 0x32: v=RdMsr(reg[1]&0xFFFFFFFF); reg[0]=v&0xFFFFFFFF; reg[2]=(v>>32)&0xFFFFFFFF; break;
    case 0xA2: Cpuid(); break;
    case 0x01: DecodeModRM(); Grp7(); break;
    case 0x05: break;
    case 0x09: break;
    case 0x0B: g_badop=0x0F0B; halted=2; break;
    case 0x18: DecodeModRM(); break;
    case 0xAE: DecodeModRM();
      if (isMem) { if ((rg&7)==0) DoFxsave(ea); else if ((rg&7)==1) DoFxrstor(ea); else if ((rg&7)==3) WrMem(ea,4,0x1F80); }
      break;
    case 0xA3: DecodeModRM(); DoBit(0, RdReg(rg,sz), sz); break;
    case 0xAB: DecodeModRM(); DoBit(1, RdReg(rg,sz), sz); break;
    case 0xB3: DecodeModRM(); DoBit(2, RdReg(rg,sz), sz); break;
    case 0xBB: DecodeModRM(); DoBit(3, RdReg(rg,sz), sz); break;
    case 0xBA: DecodeModRM(1); d=(rg&7)-4; b=Fetch8(); DoBit(d, b, sz); break;
    case 0xB0: DecodeModRM(); a=RdReg(0,1);
      if (lockf && isMem && ea < mem_size) b=AtomCas(ea, a, RdReg(rg,1), 1);
      else { b=RdRM(1); if (a==b) WrRM(1, RdReg(rg,1)); }
      r=a-b; SetSub(a,b,r,1); if (a!=b) WrReg(0,1,b); break;
    case 0xB1: DecodeModRM(); a=RdReg(0,sz);
      if (lockf && isMem && ea < mem_size && !(ea & (sz-1))) b=AtomCas(ea, a, RdReg(rg,sz), sz);
      else { b=RdRM(sz); if (a==b) WrRM(sz, RdReg(rg,sz)); }
      r=a-b; SetSub(a,b,r,sz); if (a!=b) WrReg(0,sz,b); break;
    case 0xC0: DecodeModRM(); a=RdReg(rg,1);
      if (lockf && isMem && ea < mem_size) { b=AtomAdd(ea,a,1); WrReg(rg,1,b); }
      else { b=RdRM(1); WrReg(rg,1,b); WrRM(1,a+b); }
      r=a+b; SetAdd(a,b,r,1); break;
    case 0xC1: DecodeModRM(); a=RdReg(rg,sz);
      if (lockf && isMem && ea < mem_size && !(ea & (sz-1))) { b=AtomAdd(ea,a,sz); WrReg(rg,sz,b); }
      else { b=RdRM(sz); WrReg(rg,sz,b); WrRM(sz,a+b); }
      r=a+b; SetAdd(a,b,r,sz); break;
    case 0xC3: DecodeModRM(); WrRM(sz, RdReg(rg,sz)); break;
    case 0xA4: DecodeModRM(); DoShld(1, RdReg(rg,sz), Fetch8(), sz); break;
    case 0xA5: DecodeModRM(); DoShld(1, RdReg(rg,sz), RdReg(1,1), sz); break;
    case 0xAC: DecodeModRM(); DoShld(0, RdReg(rg,sz), Fetch8(), sz); break;
    case 0xAD: DecodeModRM(); DoShld(0, RdReg(rg,sz), RdReg(1,1), sz); break;
    default: g_badop = 0x0F00 | op2; halted = 2; printf("BADOP0F op2=%llX rip=%llX\n", (unsigned long long)op2, (unsigned long long)rip); break;
  }
}

//---------------------------------------------------------------- HLE hooks
U0 HleBlit() {
  U64 ret = RdMem(reg[4], 8), dst_dc = RdMem(reg[4]+8, 8), src_dc = RdMem(reg[4]+16, 8);
  U64 sb = RdMem(src_dc+0x180, 8), db = RdMem(dst_dc+0x180, 8);
  I64 w = RdMem(src_dc+0x14, 4) & 0xFFFF, h = RdMem(src_dc+0x18, 4) & 0xFFFF, n = w*h, i;
  if (sb && db && sb < mem_size && db < mem_size && n > 0 && sb+n <= mem_size && db+n <= mem_size)
    for (i=0; i<n; i++) { U8 v = mem[sb+i]; if (v != 0xFF) mem[db+i] = v; }
  reg[0] = n; reg[4] += 24; rip = ret;
}
U0 CaptureFrom(U64 bd) { I64 k; if (bd && bd+307200 <= mem_size) { for (k=0; k<307200; k++) g_desk[k] = mem[bd+k]; g_desk_ready = 1; } }
U0 CaptureDesk() { CaptureFrom(RdMem(g_dc2+0x180, 8)); }

// Raster HLE stubs (the full implementation is in the original but is very large;
// for initial bring-up we fall through to the interpreter for these)
U16 RasterRand(U64 fsb) {
  U64 seed = *(U64*)(mem + fsb + 0x430);
  I64 state = (I64)(seed * 0x5851f42d4c957f2dULL);
  seed = ((I64)(seed & 0xffffffff0000ULL) >> 16) ^ state;
  seed += 0x14057b7ef767814fULL;
  if (!((*(U32*)(mem + fsb + 0x18) >> 0xe) & 1)) { tsc += 100; seed ^= tsc; }
  *(U64*)(mem + fsb + 0x430) = seed;
  return seed & 0xFFFF;
}
I64 RasterDone(I64 cnt) { reg[0] = cnt; rip = RdMem(reg[4], 8); reg[4] += 8 + 6*8; return 1; }

// PolySpan — one z-buffered horizontal span (for the native poly rasterizer)
U0 PolySpan(U64 dc, I64 x1, I64 x2, I64 y, I64 z1, I64 z2) {
  U64 body = *(U64*)(mem+dc+0x180), zbuf = *(U64*)(mem+dc+0x188), win = *(U64*)(mem+dc+0x170), fsb = msr_fsbase;
  U32 flags = *(U32*)(mem+dc+0x1c), col_w = *(U32*)(mem+dc+0xa0);
  I64 stride = *(I32*)(mem+dc+0x14), dimA = *(I32*)(mem+dc+0x10), dimC = *(I32*)(mem+dc+0x18);
  I64 i, idx, zptr, zslope=0, zacc, cur_z, clip_lo=0, clip_hi=0, count, mode, color, w1, w2, cw;
  I64 dith_en=0, dith_rand=0, bg = *(U32*)(mem+dc+0xa4)&0xFF;
  if (!body||!zbuf) return;
  if ((flags>>0xe)&1) { x1+=*(I64*)(mem+win+0x100); x2+=*(I64*)(mem+win+0x100); y+=*(I64*)(mem+win+0x108); z1+=*(I64*)(mem+win+0x110); z2+=*(I64*)(mem+win+0x110); }
  if ((flags>>0x10)&1) {
    if (x1<*(I64*)(mem+dc+0x140)) *(I64*)(mem+dc+0x140)=x1; if (x1>*(I64*)(mem+dc+0x148)) *(I64*)(mem+dc+0x148)=x1;
    if (x2<*(I64*)(mem+dc+0x140)) *(I64*)(mem+dc+0x140)=x2; if (x2>*(I64*)(mem+dc+0x148)) *(I64*)(mem+dc+0x148)=x2;
    if (y<*(I64*)(mem+dc+0x150)) *(I64*)(mem+dc+0x150)=y; if (y>*(I64*)(mem+dc+0x158)) *(I64*)(mem+dc+0x158)=y; }
  if (y<0) return;
  if (x2<x1) { i=x1;x1=x2;x2=i; i=z1;z1=z2;z2=i; }
  if (x2<0) return;
  if (x1<0) { clip_lo=-x1; x1=0; }
  if ((flags>>0xe)&1) {
    x1+=*(I64*)(mem+win+0xd0); x2+=*(I64*)(mem+win+0xd0); if (x1>*(I64*)(mem+win+0xd8)) return;
    if (x2>*(I64*)(mem+win+0xd8)) { clip_hi=x2-*(I64*)(mem+win+0xd8); x2=*(I64*)(mem+win+0xd8); }
    y+=*(I64*)(mem+win+0xe8); if (y<0) return; if (y>*(I64*)(mem+win+0xf0)) return; if (x2<0) return; }
  if (x1>=dimA) return; if (y>=dimC) return;
  if (x2>=dimA) x2=dimA-1;
  count=x2-x1; idx=y*stride+x1;
  if (idx<0||(x2-x1+1)>8192||body+idx+(x2-x1+1)>=mem_size||zbuf+(idx+(x2-x1+1))*4>=mem_size) return;
  zptr=zbuf+idx*4; if (count) zslope=((z2-z1)<<32)/count; zacc=z1<<32; zacc+=clip_lo*zslope;
  color=col_w&0xFF; w1=col_w&0xFFFF; w2=(col_w>>16)&0xFFFF; cw=w1;
  if ((col_w>>24)&0xC0) { dith_en=1; w2=(w2&0xFF)|(w1&0xFF00);
    if ((col_w>>24)&0x80) { dith_rand=1; if (RasterRand(fsb)<*(U64*)(mem+dc+0xb8)) cw=w2; }
    else { if ((x1^y)&1) { i=w1;w1=w2;w2=i; } cw=w1; } }
  for (i=x1; i<=x2; i++) {
    cur_z=zacc>>32;
    I64 draw=1; if (cur_z<0) draw=0; else if (cur_z>*(I32*)(mem+zptr)) draw=0; else *(I32*)(mem+zptr)=cur_z;
    if (draw) { color=cw&0xFF; mode=(cw>>8)&0xFF;
      if (mode==1) mem[body+idx]^=color; else if (mode==2) { I64 v=mem[body+idx]; if (v!=0xFF&&v!=bg) (*(U64*)(mem+dc+0x130))++; } else mem[body+idx]=color; }
    if (dith_en) { if (dith_rand) { if (RasterRand(fsb)<*(U64*)(mem+dc+0xb8)) cw=w2; else cw=w1; } else { I64 t=w1;w1=w2;w2=t;cw=w1; } }
    idx+=1; zptr+=4; zacc+=zslope; }
}

// PolyTri — scanline-rasterize one triangle into horizontal spans
U0 PolyTri(U64 dc, I64 ax, I64 ay, I64 az, I64 bx, I64 by, I64 bz, I64 cx, I64 cy, I64 cz) {
  I64 tx,ty,tz,y,xl,xr,zl,zr; F64 tac,xac,zac,xs,zs,tt;
  if (ay>by) { tx=ax;ax=bx;bx=tx; ty=ay;ay=by;by=ty; tz=az;az=bz;bz=tz; }
  if (ay>cy) { tx=ax;ax=cx;cx=tx; ty=ay;ay=cy;cy=ty; tz=az;az=cz;cz=tz; }
  if (by>cy) { tx=bx;bx=cx;cx=tx; ty=by;by=cy;cy=ty; tz=bz;bz=cz;cz=tz; }
  if (cy==ay) return;
  for (y=ay; y<=cy; y++) {
    tac=IntToF64(y-ay)/IntToF64(cy-ay); xac=ax+tac*(cx-ax); zac=az+tac*(cz-az);
    if (y<by) { if (by==ay) { xs=ax;zs=az; } else { tt=IntToF64(y-ay)/IntToF64(by-ay); xs=ax+tt*(bx-ax); zs=az+tt*(bz-az); } }
    else { if (cy==by) { xs=bx;zs=bz; } else { tt=IntToF64(y-by)/IntToF64(cy-by); xs=bx+tt*(cx-bx); zs=bz+tt*(cz-bz); } }
    if (xac<=xs) { xl=(I64)xac;xr=(I64)xs;zl=(I64)zac;zr=(I64)zs; } else { xl=(I64)xs;xr=(I64)xac;zl=(I64)zs;zr=(I64)zac; }
    PolySpan(dc, xl, xr, y, zl, zr);
  }
}

I64 PolyDone(I64 cnt) { reg[0]=cnt; rip=RdMem(reg[4],8); reg[4]+=8+3*8; return 1; }

// GrPoly3HLE — fan-triangulate a convex polygon and raster each triangle natively
I64 GrPoly3HLE() {
  U64 dc,poly,body,zbuf; I64 n,i,S=12,ax,ay,az,bx,by,bz,cx,cy,cz;
  dc=RdMem(reg[4]+8,8); n=RdMem(reg[4]+0x10,8); poly=RdMem(reg[4]+0x18,8);
  body=*(U64*)(mem+dc+0x180); zbuf=*(U64*)(mem+dc+0x188);
  if (n<3||n>64||!body||!zbuf) return 0;
  if (poly+n*S>=mem_size) return 0;
  ax=*(I32*)(mem+poly); ay=*(I32*)(mem+poly+4); az=*(I32*)(mem+poly+8);
  if (!g_poly3_diag) for (i=1; i<n-1; i++) {
    bx=*(I32*)(mem+poly+i*S); by=*(I32*)(mem+poly+i*S+4); bz=*(I32*)(mem+poly+i*S+8);
    cx=*(I32*)(mem+poly+(i+1)*S); cy=*(I32*)(mem+poly+(i+1)*S+4); cz=*(I32*)(mem+poly+(i+1)*S+8);
    PolyTri(dc, ax,ay,az, bx,by,bz, cx,cy,cz);
  }
  g_poly3_calls++;
  return PolyDone(0);
}

// Full RasterHLE — z-buffered span filler (replaces ~35-45% of 3D game frame time)
I64 RasterHLE() {
  if (rip==g_poly3_hle && g_poly3_hle && mem[rip]==0x55 && mem[rip+7]==0x58) return GrPoly3HLE();
  if (mem[rip]!=0x55||mem[rip+1]!=0x48||mem[rip+2]!=0x8B||mem[rip+3]!=0xEC||
      mem[rip+4]!=0x48||mem[rip+5]!=0x83||mem[rip+6]!=0xEC||mem[rip+7]!=0x78) return 0;
  U64 dc = RdMem(reg[4]+8, 8);
  I64 vert=0; if (mem[rip+0x1d]==0x18) vert=1; else if (mem[rip+0x1d]!=0x28) return 0;
  I64 x1,x2,y;
  if (vert) { y=RdMem(reg[4]+0x10,8); x1=RdMem(reg[4]+0x18,8); x2=RdMem(reg[4]+0x20,8); }
  else { x1=RdMem(reg[4]+0x10,8); x2=RdMem(reg[4]+0x18,8); y=RdMem(reg[4]+0x20,8); }
  I64 z1=RdMem(reg[4]+0x28,8), z2=RdMem(reg[4]+0x30,8);
  U32 flags=*(U32*)(mem+dc+0x1c);
  U64 zbuf=*(U64*)(mem+dc+0x188), win=*(U64*)(mem+dc+0x170);
  U64 body=*(U64*)(mem+dc+0x180), fsb=msr_fsbase;
  I64 stride=*(I32*)(mem+dc+0x14), width=*(I32*)(mem+dc+0x10), height=*(I32*)(mem+dc+0x18);
  U32 col_w=*(U32*)(mem+dc+0xa0);
  I64 bg=*(U32*)(mem+dc+0xa4)&0xFF;
  I64 clip_lo=0,clip_hi=0,plotcnt=0,i,idx,zptr,zslope=0,zacc,cur_z;
  I64 dith_en=0,dith_rand=0,mode,color,w1,w2,cw;
  if (!zbuf) return 0;
  I64 dimA=width, dimC=height, step=1;
  I64 sA=0x100,sC=0x108,eA=0x140,eC=0x150,pA=0xd0,pAl=0xd8,pC=0xe8,pCl=0xf0,nA=0x120,nC=0x124;
  if (vert) { dimA=height;dimC=width;step=stride; sA=0x108;sC=0x100;eA=0x150;eC=0x140;pA=0xe8;pAl=0xf0;pC=0xd0;pCl=0xd8;nA=0x124;nC=0x120; }
  if ((flags>>0xe)&1) { x1+=*(I64*)(mem+win+sA); x2+=*(I64*)(mem+win+sA); y+=*(I64*)(mem+win+sC); z1+=*(I64*)(mem+win+0x110); z2+=*(I64*)(mem+win+0x110); }
  if ((flags>>0x10)&1) {
    if (x1<*(I64*)(mem+dc+eA)) *(I64*)(mem+dc+eA)=x1; if (x1>*(I64*)(mem+dc+eA+8)) *(I64*)(mem+dc+eA+8)=x1;
    if (x2<*(I64*)(mem+dc+eA)) *(I64*)(mem+dc+eA)=x2; if (x2>*(I64*)(mem+dc+eA+8)) *(I64*)(mem+dc+eA+8)=x2;
    if (y<*(I64*)(mem+dc+eC)) *(I64*)(mem+dc+eC)=y; if (y>*(I64*)(mem+dc+eC+8)) *(I64*)(mem+dc+eC+8)=y; }
  if (y<0) return RasterDone(0);
  if (x2<x1) { i=x1;x1=x2;x2=i; i=z1;z1=z2;z2=i; }
  if (x2<0) return RasterDone(0);
  if (x1<0) { clip_lo=-x1; x1=0; }
  if ((flags>>0xe)&1) {
    x1+=*(I64*)(mem+win+pA); x2+=*(I64*)(mem+win+pA); if (x1>*(I64*)(mem+win+pAl)) return RasterDone(0);
    if (x2>*(I64*)(mem+win+pAl)) { clip_hi=x2-*(I64*)(mem+win+pAl); x2=*(I64*)(mem+win+pAl); }
    y+=*(I64*)(mem+win+pC); if (y<0) return RasterDone(0); if (y>*(I64*)(mem+win+pCl)) return RasterDone(0); if (x2<0) return RasterDone(0); }
  if (x1>=dimA) return RasterDone(0); if (y>=dimC) return RasterDone(0);
  I64 count=(x2+clip_hi)-(x1-clip_lo);
  if (vert) idx=x1*stride+y; else idx=y*stride+x1;
  zptr=zbuf+idx*4; if (count) zslope=((z2-z1)<<32)/count; zacc=z1<<32; zacc+=clip_lo*zslope;
  if (x2>=dimA) x2=dimA-1;
  if ((flags>>0xb)&1) {
    I64 cx=*(I32*)(mem+dc+nA), cy2=*(I32*)(mem+dc+nC), r13, dx;
    if (cx>=x1&&cx<=x2) r13=0; else if (cx<x1) { dx=x1-cx; r13=dx*dx; } else { dx=cx-x2; r13=dx*dx; }
    I64 dy=y-cy2; r13+=dy*dy; if (r13<*(I64*)(mem+dc+0x138)) *(I64*)(mem+dc+0x138)=r13; }
  if ((flags>>0xc)&1) return RasterDone(0);
  color=col_w&0xFF; mode=(col_w>>8)&0xFF;
  w1=col_w&0xFFFF; w2=(col_w>>16)&0xFFFF; cw=w1;
  if ((col_w>>24)&0xC0) { dith_en=1; w2=(w2&0xFF)|(w1&0xFF00);
    if ((col_w>>24)&0x80) { dith_rand=1; if (RasterRand(fsb)<*(U64*)(mem+dc+0xb8)) cw=w2; }
    else { if ((x1^y)&1) { i=w1;w1=w2;w2=i; } cw=w1; } }
  I64 need_cov=((flags>>0xe)&1)&&!((flags>>0x11)&1)&&win&&*(U64*)(mem+win+0x80)!=*(U64*)(mem+0xc480);
  U64 wzbuf=0; I64 wzn=0,cov_row=0,covM=1,cc=1;
  if (vert) covM=80;
  if (need_cov) { wzbuf=*(U64*)(mem+0x119ee470); if (wzbuf&&wzbuf<mem_size) { wzn=*(U16*)(mem+win+0x30c); if (vert) cov_row=y>>3; else cov_row=(y>>3)*80; cc=wzn>=*(U16*)(mem+wzbuf+(cov_row+(x1>>3)*covM)*2); } else need_cov=0; }
  if (idx<0||x2<x1||(x2-x1+1)>4096||!body||body+idx+(x2-x1+1)*step>=mem_size||zbuf+(idx+(x2-x1+1)*step)*4>=mem_size) return 0;
  if (g_raster_verify&&rip==g_raster_cand&&g_raster_ret) return 0;
  I64 idx0=idx,cnt0=x2-x1+1,vmode=g_raster_verify&&rip==g_raster_cand&&cnt0>0&&cnt0<=1024,vj;
  if (vmode) { for (vj=0;vj<cnt0;vj++) { g_rv_savb[vj]=mem[body+idx0+vj*step]; g_rv_savz[vj]=*(U32*)(mem+zbuf+(idx0+vj*step)*4); } g_rv_c1=*(U64*)(mem+dc+0x130); g_rv_c2=*(U64*)(mem+dc+0x138); }
  for (i=x1; i<=x2; i++) {
    if (cc) { cur_z=zacc>>32;
      I64 draw=1; if (cur_z<0) draw=0; else if (cur_z>*(I32*)(mem+zptr)) draw=0; else *(I32*)(mem+zptr)=cur_z;
      if (draw) { color=cw&0xFF; mode=(cw>>8)&0xFF;
        if (mode==1) mem[body+idx]^=color; else if (mode==2) { I64 v=mem[body+idx]; if (v!=0xFF&&v!=bg) (*(U64*)(mem+dc+0x130))++; } else mem[body+idx]=color; plotcnt++; } }
    if (dith_en) { if (dith_rand) { if (RasterRand(fsb)<*(U64*)(mem+dc+0xb8)) cw=w2; else cw=w1; } else { I64 t=w1;w1=w2;w2=t;cw=w1; } }
    if (need_cov&&((i+1)&7)==0&&i+1<=x2) cc=wzn>=*(U16*)(mem+wzbuf+(cov_row+((i+1)>>3)*covM)*2);
    idx+=step; zptr+=step*4; zacc+=zslope; }
  if (vmode) {
    U64 nc1=*(U64*)(mem+dc+0x130), nc2=*(U64*)(mem+dc+0x138);
    for (vj=0;vj<cnt0;vj++) { g_rv_natz[vj]=*(U32*)(mem+zbuf+(idx0+vj*step)*4); g_rv_natb[vj]=mem[body+idx0+vj*step];
      mem[body+idx0+vj*step]=g_rv_savb[vj]; *(U32*)(mem+zbuf+(idx0+vj*step)*4)=g_rv_savz[vj]; }
    *(U64*)(mem+dc+0x130)=g_rv_c1; *(U64*)(mem+dc+0x138)=g_rv_c2; g_rv_c1=nc1; g_rv_c2=nc2;
    g_rv_idx=idx0; g_rv_cnt=cnt0; g_rv_step=step; g_rv_zbuf=zbuf; g_rv_dc=dc; g_rv_icount=icount; g_rv_irqc=irq_count;
    g_raster_ret=RdMem(reg[4],8); g_hooktbl[g_raster_ret&0xFFFF]=1;
    g_jit_n[(g_raster_ret^(g_raster_ret>>16))&0xFFFF]=-1; g_jit_hot[(g_raster_ret^(g_raster_ret>>16))&0xFFFF]=0;
    return 0; }
  g_raster_calls++;
  return RasterDone(plotcnt);
}

U0 HookClrAt(U64 a) {
  I64 s = a & 0xFFFF, q; if (!a) return;
  if (s==(I64)(g_hle_blit&0xFFFF)||s==(I64)(g_capture_rip&0xFFFF)||s==(I64)(g_skip_c4&0xFFFF)||s==(I64)(g_skip_vga&0xFFFF)||s==(I64)(g_skip_xlat&0xFFFF)||s==(I64)(g_skip_bg&0xFFFF)) return;
  if (g_raster_cand && s==(I64)(g_raster_cand&0xFFFF)) return;
  if (g_raster_ret && s==(I64)(g_raster_ret&0xFFFF)) return;
  for (q=0; q<g_raster_n; q++) if (s==(I64)(g_raster_hle[q]&0xFFFF)) return;
  g_hooktbl[s] = 0;
}
U0 RetHookClr() { U64 r = g_raster_ret; g_raster_ret = 0; HookClrAt(r); }
U0 RasterVerifyRet() {
  I64 j, bad=0, bod=0;
  if (irq_count!=g_rv_irqc||icount-g_rv_icount>g_rv_cnt*64+20000) { RetHookClr(); return; }
  U64 body=*(U64*)(mem+g_rv_dc+0x180); I64 zbad=0, zj=-1;
  for (j=0; j<g_rv_cnt; j++) { if (*(U32*)(mem+g_rv_zbuf+(g_rv_idx+j*g_rv_step)*4)!=g_rv_natz[j]) { zbad++; if (zj<0) zj=j; } if (mem[body+g_rv_idx+j*g_rv_step]!=g_rv_natb[j]) bod++; }
  bad=zbad;
  if (*(U64*)(mem+g_rv_dc+0x130)!=g_rv_c1) bad++;
  if (*(U64*)(mem+g_rv_dc+0x138)!=g_rv_c2) bad++;
  g_raster_checks++;
  if (bad) g_raster_diffs++;
  g_rv_bodydiff+=bod;
  RetHookClr();
}

//---------------------------------------------------------------- Step — one instruction
__attribute__((hot)) U0 Step() {
  U64 op, a, b, r, cf; I64 sz, d, opn, r2, imm;

  if (g_hooktbl[rip & 0xFFFF]) {
    if (rip == g_ctr_rip && g_ctr_rip) g_ctr_n++;
    if (g_raster_n || g_raster_cand) {
      I64 q; for (q=0; q<g_raster_n; q++) if (rip==g_raster_hle[q]) { if (RasterHLE()) return; q=99; }
      if (rip==g_raster_cand && g_raster_cand) { if (RasterHLE()) return; }
    }
    if (g_poly3_hle && rip==g_poly3_hle) { if (RasterHLE()) return; }
    if (g_raster_ret && rip==g_raster_ret) RasterVerifyRet();
    if (rip == g_hle_blit) { HleBlit(); return; }
    if (rip == g_capture_rip) CaptureDesk();
    if (rip == g_skip_c4)  { rip = RdMem(reg[4],8); reg[4] += 8+32; return; }
    if (rip == g_skip_vga) { rip = RdMem(reg[4],8); reg[4] += 8; return; }
    if (rip == g_skip_xlat){ rip = RdMem(reg[4],8); reg[4] += 8+32; return; }
    if (rip == g_skip_bg && g_game_running) { rip = RdMem(reg[4],8); reg[4] += 8; return; }
  }

  pfx66=0; repf=0; hasRex=0; rexW=0; rexR=0; rexX=0; rexB=0; g_segbase=0; lockf=0;
  op = mem[rip]; rip++;
  while (g_pfx[op]) {
    if (op==0x66) pfx66=1; else if (op==0xF3) repf=1; else if (op==0xF2) repf=2;
    else if (op==0x64) g_segbase=msr_fsbase; else if (op==0x65) g_segbase=msr_gsbase;
    else if (op==0xF0) lockf=1;
    op = mem[rip]; rip++;
  }
  if (op >= 0x40 && op <= 0x4F) { hasRex=1; rexW=(op>>3)&1; rexR=(op>>2)&1; rexX=(op>>1)&1; rexB=op&1; op=mem[rip]; rip++; }
  if (rexW) sz = 8; else if (pfx66) sz = 2; else sz = 4;

  // patterned ALU (00..3F)
  if (op < 0x40 && (op&7)==0) { opn=(op>>3)&7; DecodeModRM(); a=RdRM(1); b=RdReg(rg,1); r=Arith(opn,a,b,1); if (opn!=7) WrRM(1,r); return; }
  if (op < 0x40 && (op&7)==1) { opn=(op>>3)&7; DecodeModRM(); a=RdRM(sz); b=RdReg(rg,sz); r=Arith(opn,a,b,sz); if (opn!=7) WrRM(sz,r); return; }
  if (op < 0x40 && (op&7)==2) { opn=(op>>3)&7; DecodeModRM(); a=RdReg(rg,1); b=RdRM(1); r=Arith(opn,a,b,1); if (opn!=7) WrReg(rg,1,r); return; }
  if (op < 0x40 && (op&7)==3) { opn=(op>>3)&7; DecodeModRM(); a=RdReg(rg,sz); b=RdRM(sz); r=Arith(opn,a,b,sz); if (opn!=7) WrReg(rg,sz,r); return; }
  if (op < 0x40 && (op&7)==4) { opn=(op>>3)&7; b=Fetch8(); a=RdReg(0,1); r=Arith(opn,a,b,1); if (opn!=7) WrReg(0,1,r); return; }
  if (op < 0x40 && (op&7)==5) { opn=(op>>3)&7; b=FetchImm(sz); a=RdReg(0,sz); r=Arith(opn,a,b,sz); if (opn!=7) WrReg(0,sz,r); return; }

  // hot MOV
  if (op==0x89) { DecodeModRM(); if (isMem) WrMem(ea,sz,RdReg(rg,sz)); else WrReg(rm,sz,RdReg(rg,sz)); return; }
  if (op==0x8B) { DecodeModRM(); WrReg(rg, sz, isMem ? RdMem(ea,sz) : RdReg(rm,sz)); return; }
  if (op==0x88) { DecodeModRM(); if (isMem) WrMem(ea,1,RdReg(rg,1)); else WrReg(rm,1,RdReg(rg,1)); return; }
  if (op==0x8A) { DecodeModRM(); WrReg(rg, 1, isMem ? RdMem(ea,1) : RdReg(rm,1)); return; }

  switch (op) {
    case 0x90: break;
    case 0xF4: halted=1; break;
    case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF: OpX87(op); break;
    case 0xFC: rfl &= ~(U64)F_DF; break;
    case 0xFD: rfl |= F_DF; break;
    case 0x0F: Op0F(); break;
    case 0x80: DecodeModRM(1); opn=rg&7; a=RdRM(1); b=Fetch8(); r=Arith(opn,a,b,1); if (opn!=7) WrRM(1,r); break;
    case 0x83: DecodeModRM(1); opn=rg&7; a=RdRM(sz); b=SignExt8(Fetch8()); r=Arith(opn,a,b,sz); if (opn!=7) WrRM(sz,r); break;
    case 0x81: d=4; if (pfx66) d=2; DecodeModRM(d); opn=rg&7; a=RdRM(sz); b=FetchImm(sz); r=Arith(opn,a,b,sz); if (opn!=7) WrRM(sz,r); break;
    case 0x84: DecodeModRM(); SetLogic((RdRM(1)&RdReg(rg,1))&0xFF, 1); break;
    case 0x85: DecodeModRM(); SetLogic((RdRM(sz)&RdReg(rg,sz))&SizeMask(sz), sz); break;
    case 0xA0: a=Fetch64(); WrReg(0,1,RdMem(a,1)); break;
    case 0xA1: a=Fetch64(); WrReg(0,sz,RdMem(a,sz)); break;
    case 0xA2: a=Fetch64(); WrMem(a,1,RdReg(0,1)); break;
    case 0xA3: a=Fetch64(); WrMem(a,sz,RdReg(0,sz)); break;
    case 0xA8: b=Fetch8(); SetLogic((RdReg(0,1)&b)&0xFF, 1); break;
    case 0xA9: b=FetchImm(sz); SetLogic((RdReg(0,sz)&b)&SizeMask(sz), sz); break;
    case 0x86: DecodeModRM();
      if (isMem && ea < mem_size) imm = AtomXchg(ea, RdReg(rg,1), 1);
      else { imm = RdRM(1); WrRM(1, RdReg(rg,1)); } WrReg(rg, 1, imm); break;
    case 0x87: DecodeModRM();
      if (isMem && ea < mem_size && !(ea & (sz-1))) imm = AtomXchg(ea, RdReg(rg,sz), sz);
      else { imm = RdRM(sz); WrRM(sz, RdReg(rg,sz)); } WrReg(rg, sz, imm); break;
    case 0x8E: DecodeModRM(); g_seg[rg&7]=RdRM(2); break;
    case 0x8C: DecodeModRM(); WrRM(2, g_seg[rg&7]); break;
    case 0xC6: DecodeModRM(1); WrRM(1, Fetch8()); break;
    case 0xC7: DecodeModRM(sz==2?2:4); imm=FetchImm(sz); WrRM(sz, imm); break;
    case 0x8D: DecodeModRM(); WrReg(rg, sz, ea); break;
    case 0x63: DecodeModRM(); WrReg(rg, sz, (U64)SignExtTo64(RdRM(4), 4)); break;
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
      r2=(op-0x90)|(rexB<<3); a=reg[0]; b=RdReg(r2,sz); WrReg(0,sz,b); WrReg(r2,sz,a); break;
    case 0x98:
      if (rexW) reg[0]=(U64)SignExtTo64(reg[0]&0xFFFFFFFF, 4); else if (pfx66) WrReg(0,2,(U64)SignExtTo64(reg[0]&0xFF,1)); else WrReg(0,4,(U64)SignExtTo64(reg[0]&0xFFFF,2)); break;
    case 0x9C: reg[4]-=8; WrMem(reg[4],8,rfl); break;
    case 0x9D: rfl=(RdMem(reg[4],8)&~(U64)0x10000)|2; reg[4]+=8; break;
    case 0x8F: DecodeModRM(); a=RdMem(reg[4],8); reg[4]+=8; WrRM(8,a); break;
    case 0x69: d=4; if (pfx66) d=2; DecodeModRM(d); a=RdRM(sz); imm=FetchImm(sz); WrReg(rg,sz,((U64)((I64)SignExtTo64(a,sz)*imm))&SizeMask(sz)); break;
    case 0x6B: DecodeModRM(1); a=RdRM(sz); imm=SignExt8(Fetch8()); WrReg(rg,sz,((U64)((I64)SignExtTo64(a,sz)*imm))&SizeMask(sz)); break;
    case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
      r2=(op-0xB0)|(rexB<<3); WrReg(r2,1,Fetch8()); break;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
      r2=(op-0xB8)|(rexB<<3); if (rexW) reg[r2]=Fetch64();
      else if (sz==2) { WrReg(r2,2,RdMem(rip,2)); rip+=2; }
      else reg[r2]=Fetch32(); break;
    case 0xC0: DecodeModRM(1); d=Fetch8(); DoShift(rg&7, d, 1); break;
    case 0xC1: DecodeModRM(1); d=Fetch8(); DoShift(rg&7, d, sz); break;
    case 0xD0: DecodeModRM(); DoShift(rg&7, 1, 1); break;
    case 0xD1: DecodeModRM(); DoShift(rg&7, 1, sz); break;
    case 0xD2: DecodeModRM(); d=RdReg(1,1); DoShift(rg&7, d, 1); break;
    case 0xD3: DecodeModRM(); d=RdReg(1,1); DoShift(rg&7, d, sz); break;
    case 0xD7: reg[0]=(reg[0]&0xFFFFFFFFFFFFFF00ULL)|RdMem(reg[3]+(reg[0]&0xFF)+g_segbase, 1); break;
    case 0xF6: DecodeModRM(((mem[rip]>>3)&7)<=1?1:0); Grp3(1); break;
    case 0xF7: DecodeModRM(((mem[rip]>>3)&7)<=1?(sz==2?2:4):0); Grp3(sz); break;
    case 0xFE: DecodeModRM();
      if ((rg&7)==0) { cf=rfl&F_CF; a=RdRM(1); r=a+1; WrRM(1,r); SetAdd(a,1,r,1); rfl=(rfl&~(U64)F_CF)|cf; }
      else           { cf=rfl&F_CF; a=RdRM(1); r=a-1; WrRM(1,r); SetSub(a,1,r,1); rfl=(rfl&~(U64)F_CF)|cf; }
      break;
    case 0xFF: DecodeModRM();
      if      ((rg&7)==0) { cf=rfl&F_CF; a=RdRM(sz); r=a+1; WrRM(sz,r); SetAdd(a,1,r,sz); rfl=(rfl&~(U64)F_CF)|cf; }
      else if ((rg&7)==1) { cf=rfl&F_CF; a=RdRM(sz); r=a-1; WrRM(sz,r); SetSub(a,1,r,sz); rfl=(rfl&~(U64)F_CF)|cf; }
      else if ((rg&7)==2) { reg[4]-=8; WrMem(reg[4],8,rip); rip=RdRM(8); }
      else if ((rg&7)==4) { rip=RdRM(8); }
      else if ((rg&7)==6) { reg[4]-=8; WrMem(reg[4],8,RdRM(8)); }
      break;
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
      r2=(op-0x50)|(rexB<<3); reg[4]-=8; WrMem(reg[4],8,reg[r2]); break;
    case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
      r2=(op-0x58)|(rexB<<3); reg[r2]=RdMem(reg[4],8); reg[4]+=8; break;
    case 0x68: reg[4]-=8; WrMem(reg[4],8,(U64)SignExt32(Fetch32())); break;
    case 0x6A: reg[4]-=8; WrMem(reg[4],8,(U64)SignExt8(Fetch8())); break;
    case 0xC9: reg[4]=reg[5]; reg[5]=RdMem(reg[4],8); reg[4]+=8; break;
    case 0x99: if (rexW) { if (reg[0]>>63) reg[2]=0xFFFFFFFFFFFFFFFFULL; else reg[2]=0; }
               else { if (reg[0]&0x80000000) WrReg(2,4,0xFFFFFFFF); else WrReg(2,4,0); } break;
    case 0x6C: DoIns(1); break;
    case 0x6D: DoIns(sz==2?2:4); break;
    case 0x6E: DoOuts(1); break;
    case 0x6F: DoOuts(sz==2?2:4); break;
    case 0xA4: DoStr(0xA4,1); break;
    case 0xA5: DoStr(0xA5,sz); break;
    case 0xAA: DoStr(0xAA,1); break;
    case 0xAB: DoStr(0xAB,sz); break;
    case 0xAC: DoStr(0xAC,1); break;
    case 0xAD: DoStr(0xAD,sz); break;
    case 0xA6: DoStr(0xA6,1); break;
    case 0xA7: DoStr(0xA7,sz); break;
    case 0xAE: DoStr(0xAE,1); break;
    case 0xAF: DoStr(0xAF,sz); break;
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F:
      d=SignExt8(Fetch8()); if (Cond(op-0x70)) rip+=d; break;
    case 0xEB: d=SignExt8(Fetch8()); rip+=d; break;
    case 0xE0: d=SignExt8(Fetch8()); reg[1]--; if (reg[1]!=0 && !(rfl&F_ZF)) rip+=d; break;
    case 0xE1: d=SignExt8(Fetch8()); reg[1]--; if (reg[1]!=0 && (rfl&F_ZF)) rip+=d; break;
    case 0xE2: d=SignExt8(Fetch8()); reg[1]--; if (reg[1]!=0) rip+=d; break;
    case 0xE3: d=SignExt8(Fetch8()); if (reg[1]==0) rip+=d; break;
    case 0xE9: d=SignExt32(Fetch32()); rip+=d; break;
    case 0xE8: d=SignExt32(Fetch32()); reg[4]-=8; WrMem(reg[4],8,rip); rip+=d; break;
    case 0xC2: d=RdMem(rip,2); rip=RdMem(reg[4],8); reg[4]+=8+d; break;
    case 0xC3: rip=RdMem(reg[4],8); reg[4]+=8; break;
    case 0xFA: rfl &= ~(U64)F_IF; break;
    case 0xFB: rfl |= F_IF; break;
    case 0xE4: d=Fetch8(); WrReg(0,1,IoIn(d)); break;
    case 0xE5: d=Fetch8(); WrReg(0,sz,IoIn(d)); break;
    case 0xE6: d=Fetch8(); IoOut(d,RdReg(0,1)); break;
    case 0xE7: d=Fetch8(); IoOut(d,RdReg(0,sz)); break;
    case 0xEC: WrReg(0,1,IoIn(reg[2]&0xFFFF)); break;
    case 0xED: WrReg(0,sz,IoIn(reg[2]&0xFFFF)); break;
    case 0xEE: IoOut(reg[2]&0xFFFF,RdReg(0,1)); break;
    case 0xEF: IoOut(reg[2]&0xFFFF,RdReg(0,sz)); break;
    case 0xCC: DoInt(3,0,0); break;
    case 0xCD: d=Fetch8(); DoInt(d,0,0); break;
    case 0xCF: DoIret(); break;
    default: g_badop=op; halted=2; printf("BADOP op=%llX rip=%llX\n",(unsigned long long)op,(unsigned long long)rip); break;
  }
}

//---------------------------------------------------------------- RunTo / CallGuest
U0 RunTo(U64 target, I64 budget) {
  I64 lim = icount + budget;
  while (icount < lim && rip != target && halted < 2) {
    if (pit_div) { pit_counter--; if (pit_counter <= 0) { pit_counter = pit_div; pic1_irr |= 1; } }
    hpet_ctr += 33; tsc += tsc_rate;
    if (halted == 1) { if ((rfl & F_IF) && ((pic1_irr & ~pic1_mask) || (pic2_irr & ~pic2_mask))) { halted = 0; CheckIrq(); } icount++; continue; }
    Step(); icount++; if ((rfl & F_IF) && ((pic1_irr & ~pic1_mask) || (pic2_irr & ~pic2_mask))) CheckIrq();
  }
}
U0 CallGuest(U64 fn) { U64 sentinel=0x7FF0; reg[4]-=8; WrMem(reg[4],8,sentinel); rip=fn; halted=0; RunTo(sentinel,80000000); }

U0 RunCore(I64 budget) {
  I64 lim = icount + budget, b;
  while (icount < lim) {
    if (halted >= 2) break;
    // deliver pending IPI (atomic load)
    U64 ip = __atomic_load_n(&g_ipi_pending[MyCore()], __ATOMIC_SEQ_CST);
    if (ip && (rfl & F_IF)) { __atomic_store_n(&g_ipi_pending[MyCore()], (U64)0, __ATOMIC_SEQ_CST); halted = 0; DoInt(ip & 0xFF, 0, 0); }
    if (halted == 1) break;  // idle, no deliverable IPI -> yield to orchestrator
    Step(); icount++;
  }
}

//---------------------------------------------------------------- Run — main emulation loop
U0 Run(I64 maxi) {
  while (icount < maxi) {
    if (halted >= 2) break;
    if (halted == 1) {
      I64 skip = pit_div ? pit_counter : 1; if (skip<1) skip=1; if (skip>maxi-icount) skip=maxi-icount; if (skip<1) skip=1;
      if (pit_div) { pit_counter -= skip; if (pit_counter <= 0) { pit_counter = pit_div; pic1_irr |= 1; } }
      g_hpet_acc += g_hpet_q16 * skip; hpet_ctr += g_hpet_acc >> 16; g_hpet_acc &= 0xFFFF;
      g_tsc_acc += g_tsc_q16 * skip; tsc += g_tsc_acc >> 16; g_tsc_acc &= 0xFFFF;
      icount += skip;
      if ((pic1_irr & ~pic1_mask) || (pic2_irr & ~pic2_mask)) {
        if (rfl & F_IF) { halted=0; CheckIrq(); }
        else { irq_stall += skip; if (irq_stall > 800000000) { irq_stall=0; halted=0; CheckIrq(); } }
      }
      continue;
    }
    I64 b = 0, batch = 256; if (batch > maxi - icount) batch = maxi - icount;
    while (b < batch) { Step(); b++; if (halted) break; }
    icount += b;
    g_hpet_acc += g_hpet_q16 * b; hpet_ctr += g_hpet_acc >> 16; g_hpet_acc &= 0xFFFF;
    g_tsc_acc += g_tsc_q16 * b; tsc += g_tsc_acc >> 16; g_tsc_acc &= 0xFFFF;
    if (pit_div) { pit_counter -= b; if (pit_counter <= 0) { pit_counter += pit_div; pic1_irr |= 1; } }
    if (halted) continue;
    if ((rfl & F_IF) && ((pic1_irr & ~pic1_mask) || (pic2_irr & ~pic2_mask))) CheckIrq();
    else if ((pic1_irr & 1) && !(pic1_mask & 1)) {
      irq_stall += b;
      if (irq_stall > 2000000) {
        irq_stall = 0; I64 g = 0;
        while (g < 3000000 && halted < 2) {
          Step(); icount++; g++;
          g_hpet_acc += g_hpet_q16; hpet_ctr += g_hpet_acc >> 16; g_hpet_acc &= 0xFFFF;
          g_tsc_acc += g_tsc_q16; tsc += g_tsc_acc >> 16; g_tsc_acc &= 0xFFFF;
          if (pit_div) { pit_counter--; if (pit_counter<=0) { pit_counter+=pit_div; pic1_irr|=1; } }
          if (halted) break;
          if ((rfl & F_IF) && ((pic1_irr & ~pic1_mask) || (pic2_irr & ~pic2_mask))) { CheckIrq(); break; }
          if (!((pic1_irr & 1) && !(pic1_mask & 1))) break;
        }
        if (g >= 3000000 && (pic1_irr & 1) && !(pic1_mask & 1)) CheckIrq();
      }
    }
    else irq_stall = 0;
  }
}

U0 Reset() {
  I64 i; for (i=0; i<16; i++) reg[i]=0; rip=0; rfl=0; halted=0; icount=0; g_badop=0; reg[4]=0x100000;
  cr0=0; cr2=0; cr3=0; cr4=0; cr8=0; g_cs=0x08; g_ss=0x10; idtr_base=0; idtr_limit=0; gdtr_base=0; gdtr_limit=0; tsc=0;
  for (i=0; i<16; i++) { xmm_lo[i]=0; xmm_hi[i]=0; }
  pic1_base=0; pic1_mask=0xFF; pic1_irr=0; pic1_isr=0; pic1_initstep=0;
  pic2_base=0; pic2_mask=0xFF; pic2_irr=0; pic2_isr=0; pic2_initstep=0;
  pit_reload=0; pit_div=0; pit_counter=0; pit_wlo=1;
}
U0 Load(U8 *p, I64 n, U64 at) { I64 i; for (i=0; i<n; i++) mem[at+i]=p[i]; rip=at; }
