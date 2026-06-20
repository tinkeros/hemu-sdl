#pragma once
//============================================================================
// cpu.h -- C++ translation of Hemu-wasm/src/cpu.HC
// x86-64 interpreter for a TempleOS-only emulator (hemu-sdl build).
//============================================================================

#include "types.h"

//---------------------------------------------------------------- per-core state
struct CCpuState {
    U64 f_reg[16], f_rip, f_rfl;
    I64 f_halted, f_irq_stall;            // 0 run,1 HLT,2 bad op,3 #DE ; IRQ0-under-IF=0 watchdog
    U64 f_g_seg[8];                       // segment selectors (stored, flat long mode)
    // f_fsc removed (C++ uses memcpy for type-punning)
    F64 f_fpr[8]; I64 f_fsp;              // x87 register stack + top
    U64 f_x87_cw, f_x87_sw, f_msr_fsbase, f_msr_gsbase;
    I64 f_hasRex, f_rexW, f_rexR, f_rexX, f_rexB, f_pfx66, f_repf;   // prefix decode
    U64 f_g_segbase;                      // FS/GS base for the current instruction (0 = flat)
    I64 f_md, f_rg, f_rm, f_isMem; U64 f_ea;   // ModRM decode
};

extern CCpuState g_cpu_st[NCORES];

// Per-core field aliases via MyCore() (SMP-safe: each thread has its own t_my_core)
#define reg        g_cpu_st[MyCore()].f_reg
#define rip        g_cpu_st[MyCore()].f_rip
#define rfl        g_cpu_st[MyCore()].f_rfl
#define halted     g_cpu_st[MyCore()].f_halted
#define irq_stall  g_cpu_st[MyCore()].f_irq_stall
#define g_seg      g_cpu_st[MyCore()].f_g_seg
#define fpr        g_cpu_st[MyCore()].f_fpr
#define fsp        g_cpu_st[MyCore()].f_fsp
#define x87_cw     g_cpu_st[MyCore()].f_x87_cw
#define x87_sw     g_cpu_st[MyCore()].f_x87_sw
#define msr_fsbase g_cpu_st[MyCore()].f_msr_fsbase
#define msr_gsbase g_cpu_st[MyCore()].f_msr_gsbase
#define hasRex     g_cpu_st[MyCore()].f_hasRex
#define rexW       g_cpu_st[MyCore()].f_rexW
#define rexR       g_cpu_st[MyCore()].f_rexR
#define rexX       g_cpu_st[MyCore()].f_rexX
#define rexB       g_cpu_st[MyCore()].f_rexB
#define pfx66      g_cpu_st[MyCore()].f_pfx66
#define repf       g_cpu_st[MyCore()].f_repf
#define g_segbase  g_cpu_st[MyCore()].f_g_segbase
#define md         g_cpu_st[MyCore()].f_md
#define rg         g_cpu_st[MyCore()].f_rg
#define rm         g_cpu_st[MyCore()].f_rm
#define isMem      g_cpu_st[MyCore()].f_isMem
#define ea         g_cpu_st[MyCore()].f_ea
#define xmm_lo     g_xmm_lo[MyCore()]
#define xmm_hi     g_xmm_hi[MyCore()]
#define lockf      g_lockf[MyCore()]

//---------------------------------------------------------------- EFLAGS bits
#define F_CF 0x0001
#define F_PF 0x0004
#define F_ZF 0x0040
#define F_SF 0x0080
#define F_IF 0x0200
#define F_DF 0x0400
#define F_OF 0x0800

//---------------------------------------------------------------- extern globals (shared state)
extern U8  *mem;
extern U64 mem_size;
extern U8  g_pfx[256];

extern I64 icount;
extern U64 g_hle_blit;
extern U8  *g_desk;
extern I64 g_desk_ready;
extern U64 g_capture_rip;
extern U64 g_skip_c4, g_skip_vga, g_skip_xlat;
extern U64 g_skip_bg;
extern U64 g_game_running;
extern U64 g_jit_on;
extern U64 g_jit_rip[65536];
extern I64 g_jit_n[65536];
extern I64 g_jit_hot[65536];
extern U8  g_hooktbl[65536];

extern U64 ata_words, ata_seccnt, ata_pos, ata_r3, ata_r4, ata_r5;
extern U64 ata_wr, ata_wlba, ata_wcnt;
extern U8  *ata_buf;

extern U64 g_hpet_rate, g_jiffies0;
extern U64 g_badop;

extern U64 cr0, cr2, cr3, cr4, cr8, g_cs, g_ss;
extern U64 idtr_base, idtr_limit, gdtr_base, gdtr_limit, tsc;
extern I64 g_tsc_q16, g_tsc_acc;
extern I64 g_hpet_q16, g_hpet_acc;
extern I64 tsc_rate;

extern U64 msr_efer, msr_star, msr_lstar, msr_sfmask, msr_kgsbase, msr_apic;
extern U64 g_xmm_lo[NCORES][16], g_xmm_hi[NCORES][16];
extern I64 g_lockf[NCORES];

extern I64 pic1_base, pic1_mask, pic1_irr, pic1_isr, pic1_initstep;
extern I64 pic2_base, pic2_mask, pic2_irr, pic2_isr, pic2_initstep;
extern I64 pit_reload, pit_div, pit_counter, pit_wlo;
extern I64 irq_count;

extern I64 rtc_idx;
extern U64 rtc_base_sec;
extern I64 rtc_dow, rtc_day, rtc_mon, rtc_year;

extern I64 kbd_q[64], kbd_qh, kbd_qt;
extern I64 ms_byte0, ms_byte1, ms_byte2, ms_has, ms_phase;
extern I64 ps2q[16], ps2q_ms[16], ps2qh, ps2qt, ms_next, cfg_next;

extern I64 pit2_reload, pit2_div, pit2_wlo, spk_on;

extern U64 hpet_ctr;
extern U64 lapic_base;
extern U64 hpet_base;

extern I64 g_ncore;
extern U64 g_ipi_pending[NCORES];
extern U64 g_icr_high[NCORES];
extern U64 g_dc2;

// Raster HLE state
extern U64 g_raster_hle[4], g_raster_cand;
extern I64 g_raster_n, g_raster_calls;
extern U64 g_poly3_hle;
extern I64 g_poly3_calls, g_poly3_scanned, g_poly3_diag;
extern U64 g_raster_verify, g_raster_ret, g_rv_zbuf, g_rv_dc, g_rv_c1, g_rv_c2, g_rv_icount, g_rv_irqc;
extern U64 g_dbg_bad[13];
extern I64 g_dbg_badn;
extern U64 g_ctr_rip;
extern I64 g_ctr_n;
extern I64 g_rv_idx, g_rv_cnt, g_rv_step, g_raster_diffs, g_raster_checks, g_rv_bodydiff;
extern U8  g_rv_savb[1024];
extern U32 g_rv_savz[1024], g_rv_natz[1024], g_rv_natb[1024];

//---------------------------------------------------------------- host function stubs (implemented elsewhere)
extern void host_disk(U64 lba, U64 cnt, U8* buf);
extern void host_disk_wr(U64 lba, U64 cnt, U8* buf);
extern U64  host_time(I64 idx);
extern void host_snd(double freq);
extern void host_present(U8* buf, int w, int h);
extern void host_snap_load(U8* mem);
extern I64  host_msx();
extern I64  host_msy();
extern I64  host_msb();
extern I64  host_wheel();
extern I64  host_key();
extern I64  host_dt();
extern I64  host_budget();

//---------------------------------------------------------------- inline helpers: type-punning
inline F64 BitsToF64(U64 b) { F64 d; memcpy(&d, &b, 8); return d; }
inline U64 F64ToBits(F64 d) { U64 b; memcpy(&b, &d, 8); return b; }

//---------------------------------------------------------------- atomics (GCC builtins)
inline U64 AtomOr(U64 a, U64 v, I64 sz) {
    if (sz == 1) return __atomic_fetch_or((U8*)(mem + a), (U8)v, __ATOMIC_SEQ_CST);
    if (sz == 2) return __atomic_fetch_or((U16*)(mem + a), (U16)v, __ATOMIC_SEQ_CST);
    if (sz == 4) return __atomic_fetch_or((U32*)(mem + a), (U32)v, __ATOMIC_SEQ_CST);
    return __atomic_fetch_or((U64*)(mem + a), v, __ATOMIC_SEQ_CST);
}
inline U64 AtomAnd(U64 a, U64 v, I64 sz) {
    if (sz == 1) return __atomic_fetch_and((U8*)(mem + a), (U8)v, __ATOMIC_SEQ_CST);
    if (sz == 2) return __atomic_fetch_and((U16*)(mem + a), (U16)v, __ATOMIC_SEQ_CST);
    if (sz == 4) return __atomic_fetch_and((U32*)(mem + a), (U32)v, __ATOMIC_SEQ_CST);
    return __atomic_fetch_and((U64*)(mem + a), v, __ATOMIC_SEQ_CST);
}
inline U64 AtomXor(U64 a, U64 v, I64 sz) {
    if (sz == 1) return __atomic_fetch_xor((U8*)(mem + a), (U8)v, __ATOMIC_SEQ_CST);
    if (sz == 2) return __atomic_fetch_xor((U16*)(mem + a), (U16)v, __ATOMIC_SEQ_CST);
    if (sz == 4) return __atomic_fetch_xor((U32*)(mem + a), (U32)v, __ATOMIC_SEQ_CST);
    return __atomic_fetch_xor((U64*)(mem + a), v, __ATOMIC_SEQ_CST);
}
inline U64 AtomAdd(U64 a, U64 v, I64 sz) {
    if (sz == 1) return __atomic_fetch_add((U8*)(mem + a), (U8)v, __ATOMIC_SEQ_CST);
    if (sz == 2) return __atomic_fetch_add((U16*)(mem + a), (U16)v, __ATOMIC_SEQ_CST);
    if (sz == 4) return __atomic_fetch_add((U32*)(mem + a), (U32)v, __ATOMIC_SEQ_CST);
    return __atomic_fetch_add((U64*)(mem + a), v, __ATOMIC_SEQ_CST);
}
inline U64 AtomXchg(U64 a, U64 v, I64 sz) {
    if (sz == 1) { U8  old, val = (U8)v;  __atomic_exchange((U8*)(mem+a),  &val, &old, __ATOMIC_SEQ_CST); return old; }
    if (sz == 2) { U16 old, val = (U16)v; __atomic_exchange((U16*)(mem+a), &val, &old, __ATOMIC_SEQ_CST); return old; }
    if (sz == 4) { U32 old, val = (U32)v; __atomic_exchange((U32*)(mem+a), &val, &old, __ATOMIC_SEQ_CST); return old; }
    { U64 old, val = v; __atomic_exchange((U64*)(mem+a), &val, &old, __ATOMIC_SEQ_CST); return old; }
}
inline U64 AtomCas(U64 a, U64 expected, U64 desired, I64 sz) {
    // Returns the OLD value (e is updated to old on failure)
    if (sz == 1) { U8  e = (U8)expected;  __atomic_compare_exchange_n((U8*)(mem+a),  &e, (U8)desired,  false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return e; }
    if (sz == 2) { U16 e = (U16)expected; __atomic_compare_exchange_n((U16*)(mem+a), &e, (U16)desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return e; }
    if (sz == 4) { U32 e = (U32)expected; __atomic_compare_exchange_n((U32*)(mem+a), &e, (U32)desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return e; }
    { U64 e = expected; __atomic_compare_exchange_n((U64*)(mem+a), &e, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return e; }
}

//---------------------------------------------------------------- function declarations
U0  InitMem(U64 sz);
U64 MmioRd(U64 a, I64 sz);
U0  MmioWr(U64 a, I64 sz, U64 v);
U64 RdMem(U64 a, I64 sz);
U0  WrMem(U64 a, I64 sz, U64 v);
U64 Fetch8();
U64 Fetch32();
U64 Fetch64();
I64 SignExt8(U64 b);
I64 SignExt32(U64 v);
U64 SizeMask(I64 sz);
I64 SignExtTo64(U64 v, I64 sz);
I64 FetchImm(I64 sz);
U64 RdReg(I64 i, I64 sz);
U0  WrReg(I64 i, I64 sz, U64 v);
U0  SetZSP(U64 v, I64 sz);
U0  SetAdd(U64 a, U64 b, U64 r, I64 sz);
U0  SetSub(U64 a, U64 b, U64 r, I64 sz);
U0  SetLogic(U64 r, I64 sz);
U64 Arith(I64 opn, U64 a, U64 b, I64 sz);
I64 Cond(I64 cc);
U0  DecodeModRM(I64 immlen = 0);
U64 RdRM(I64 sz);
U0  WrRM(I64 sz, U64 v);
U0  DoShift(I64 opn, I64 cnt, I64 sz);
U0  DoShld(I64 isLeft, U64 src, I64 cnt, I64 sz);
U0  Grp3(I64 sz);
U0  StrStep(I64 op, I64 sz);
U0  DoStr(I64 op, I64 sz);
U64 RdMsr(U64 i);
U0  WrMsr(U64 i, U64 v);
U0  Cpuid();
U64 RtcRead(I64 idx);
U64 IoIn(U64 port);
U0  IoOut(U64 port, U64 v);
U0  DoIns(I64 sz);
U0  DoOuts(I64 sz);
U0  CheckIrq();
U0  Grp7();
U0  DoInt(I64 vec, I64 hasErr, U64 err);
U0  DoIret();
F64 IntToF64(I64 v);
I64 F64ToInt(F64 d);
I64 RoundI(F64 d);
I64 RoundCW(F64 d);
F64 F32ToF64(U64 b);
U64 F64ToF32b(F64 d);
U0  FPush(F64 v);
F64 FPopv();
F64 FStv(I64 i);
U0  FStset(I64 i, F64 v);
U0  FCmpSW(F64 a, F64 b);
U0  FCmpFL(F64 a, F64 b);
U0  OpX87(U64 op);
F64 SqrtF64(F64 d);
U64 RdXmm64();
U0  DoBit(I64 which, U64 bit, I64 sz);
U0  DoFxsave(U64 a);
U0  DoFxrstor(U64 a);
U0  Op0F();
U0  Ps2Resp(I64 b, I64 isms);
U0  UpdateSpk();
U0  InjectKey(I64 sc);
U0  InjectMouse(I64 b0, I64 b1, I64 b2);
U0  HleBlit();
U0  CaptureFrom(U64 bd);
U0  CaptureDesk();
U16 RasterRand(U64 fsb);
I64 RasterDone(I64 cnt);
U0  PolySpan(U64 dc, I64 x1, I64 x2, I64 y, I64 z1, I64 z2);
U0  PolyTri(U64 dc, I64 ax, I64 ay, I64 az, I64 bx, I64 by, I64 bz, I64 cx, I64 cy, I64 cz);
I64 PolyDone(I64 cnt);
I64 GrPoly3HLE();
I64 RasterHLE();
U0  HookClrAt(U64 a);
U0  RetHookClr();
U0  RasterVerifyRet();
U0  Step();
U0  RunTo(U64 target, I64 budget);
U0  CallGuest(U64 fn);
U0  RunCore(I64 budget);
U0  Run(I64 maxi);
U0  Reset();
U0  Load(U8 *p, I64 n, U64 at);
