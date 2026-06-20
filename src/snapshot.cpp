//============================================================================
// snapshot.cpp -- boot real TempleOS from a qemu RAM+register snapshot.
// Translated from Hemu-wasm/src/snapshot.HC to C++/SDL2.
//============================================================================
#include "snapshot.h"
#include "host.h"
#include <cstring>
#include <cmath>
#include <pthread.h>
#include <atomic>

// ---------------------------------------------------------------------------
// Per-core state arrays
// ---------------------------------------------------------------------------
CCpuState g_cpu_st[NCORES];
I64 g_ncore = 0;
U64 g_ipi_pending[NCORES] = {};
U64 g_icr_high[NCORES] = {};
U64 g_dc2 = 0x119ad3d8;

// ---------------------------------------------------------------------------
// Externs from cpu.h / cpu.cpp -- types must match cpu.h declarations exactly
// ---------------------------------------------------------------------------
// (most externs already declared via cpu.h include in snapshot.h)

// g_budget is local to snapshot (not in cpu.h)
static I64 s_budget = 0;

// ---------------------------------------------------------------------------
// Forward declarations for snapregs.cpp
// ---------------------------------------------------------------------------
extern void SetSnapRegs();
extern void SetSnapRegsSmp();

// ---------------------------------------------------------------------------
// snapshot state
// ---------------------------------------------------------------------------
static U64 g_inited = 0;
static U64 g_fcount = 0;
static U64 g_wake_jif = 0;

// SMP mode flag — set by main.cpp before first emu_main() call
bool g_smp_mode = false;

// Raster scan state
static U64 g_raster_tries = 0;
static U64 g_raster_scan_from = 0x11000000;
static U64 g_raster_armframes = 0;
static U64 g_raster_rej[8];
static I64 g_rej_n = 0;
static U64 g_raster_cold[8];
static I64 g_cold_n = 0, g_cold_i = 0;

// ---------------------------------------------------------------------------
// FindDc2 -- locate the gr.dc2 CDC (640x480, 8bpp body) for ANY boot
// ---------------------------------------------------------------------------
U64 FindDc2()
{
    U64 best = 0, bestnz = 0;
    for (U64 a = 0x11900000; a < 0x11A00000; a += 8) {
        U64 w = *(U64*)(mem + a + 20) & 0xFFFF;
        U64 h = *(U64*)(mem + a + 24) & 0xFFFF;
        if (w < 600 || w > 720 || h != 480) continue;
        U64 body = *(U64*)(mem + a + 384);
        if (!body || body >= 0x18000000) continue;
        U64 nz = 0;
        for (U64 i = 0; i < w * h; i += 64)
            if (mem[body + i]) nz++;
        if (nz > bestnz) { bestnz = nz; best = a; }
    }
    if (best) g_dc2 = best;
    return best;
}

// ---------------------------------------------------------------------------
// BodySum -- checksum the composited framebuffer (change-detect)
// ---------------------------------------------------------------------------
U64 BodySum()
{
    U64 dc = g_dc2;
    U64 body = *(U64*)(mem + dc + 384);
    U64 w = *(U64*)(mem + dc + 20) & 0xFFFF;
    U64 h = *(U64*)(mem + dc + 24) & 0xFFFF;
    if (w < 1 || w > 4096) w = 640;
    if (h < 1 || h > 4096) h = 480;
    U64 n = w * h;
    if (!body || body >= 0x18000000) return 0;
    U64 s = 0;
    for (U64 i = 0; i < n; i += 4)
        s += mem[body + i] * (i + 7);
    return s;
}

// ---------------------------------------------------------------------------
// Present -- HLE: blit the composited desktop
// ---------------------------------------------------------------------------
U0 Present()
{
    U64 dc, body, w, h;
    if (g_desk_ready) { host_present(g_desk, 640, 480); return; }
    dc = g_dc2;
    body = *(U64*)(mem + dc + 384);
    w = *(U64*)(mem + dc + 20) & 0xFFFF;
    h = *(U64*)(mem + dc + 24) & 0xFFFF;
    if (w < 1 || w > 4096) w = 640;
    if (h < 1 || h > 4096) h = 480;
    if (body && body < 0x18000000)
        host_present(mem + body, (int)w, (int)h);
}

// ---------------------------------------------------------------------------
// WakeTasks -- scan tasks and optionally force them ready
// ---------------------------------------------------------------------------
U0 WakeTasks(I64 do_wake)
{
    U64 head = *(U64*)(mem + 0x15F8C060);
    U64 t = head;
    I64 n = 0, game = 0;
    while (n < 24) {
        U64 pu = *(U64*)(mem + t + 0x450);
        if (pu) {
            if (pu < 0x18000000) {
                I64 wl = *(I64*)(mem + pu + 0x48);
                I64 wr = *(I64*)(mem + pu + 0x50);
                I64 wt = *(I64*)(mem + pu + 0x58);
                I64 wb = *(I64*)(mem + pu + 0x60);
                if (wr - wl >= 70 && wb - wt >= 50) game = 1;
            }
        } else if (do_wake) {
            *(U64*)(mem + t + 0x18) &= ~(U64)0x204;
            *(U64*)(mem + t + 0x10) = 0;
        }
        t = *(U64*)(mem + t + 0x80);
        n++;
        if (!t || t >= 0x18000000 || t == head) break;
    }
    g_game_running = game;
}

// ---------------------------------------------------------------------------
// ArmRaster -- find a candidate raster span-filler by signature
// ---------------------------------------------------------------------------
U0 ArmRaster()
{
    U64 a;
    I64 k, skip;
    for (a = g_raster_scan_from; a < 0x11C00000; a++) {
        if (mem[a] != 0x55) continue;
        skip = 0;
        for (k = 0; k < g_raster_n; k++)
            if (a == g_raster_hle[k]) skip = 1;
        for (k = 0; k < g_rej_n; k++)
            if (a == g_raster_rej[k]) skip = 1;
        if (skip) continue;
        // prologue check
        if (mem[a+1]!=0x48 || mem[a+2]!=0x8B || mem[a+3]!=0xEC ||
            mem[a+4]!=0x48 || mem[a+5]!=0x83 || mem[a+6]!=0xEC || mem[a+7]!=0x78) continue;
        // mov ebx,[rsi+0xa0]
        if (mem[a+0x26]!=0x8B || mem[a+0x27]!=0x9E || mem[a+0x28]!=0xA0 ||
            mem[a+0x29] || mem[a+0x2A] || mem[a+0x2B]) continue;
        // mov ebx,[rsi+0xa4]
        if (mem[a+0x2F]!=0x8B || mem[a+0x30]!=0x9E || mem[a+0x31]!=0xA4) continue;
        // mov rax,[rsi+0x188] (z-buffer)
        if (mem[a+0x3C]!=0x48 || mem[a+0x3D]!=0x8B || mem[a+0x3E]!=0x86 ||
            mem[a+0x3F]!=0x88 || mem[a+0x40]!=0x01) continue;
        // candidate found: arm in VERIFY mode
        g_raster_scan_from = a + 1;
        g_raster_cand = a;
        g_hooktbl[a & 0xFFFF] = 1;
        g_jit_n[(a ^ (a >> 16)) & 0xFFFF] = 0;
        g_raster_verify = 1;
        g_raster_checks = 0;
        g_raster_diffs = 0;
        g_raster_armframes = 0;
        return;
    }
    g_raster_scan_from = 0x11C00000;  // exhausted
}

// ---------------------------------------------------------------------------
// Frame -- one emulated frame: input, wake, pacing, run, present
// ---------------------------------------------------------------------------
U0 Frame()
{
    I64 mx = host_msx(), my = host_msy(), mb = host_msb();
    if (mx < 0) mx = 0; else if (mx > 639) mx = 639;
    if (my < 0) my = 0; else if (my > 479) my = 479;
    *(U64*)(mem + 0xBF68) = (U64)mx;
    *(U64*)(mem + 0xBF70) = (U64)my;
    *(U64*)(mem + 0xBF78) = (U64)host_wheel();
    *(U64*)(mem + 0xBF80) = (U64)(mx / 8);
    *(U64*)(mem + 0xBF88) = (U64)(my / 8);
    mem[0xC008] = mb & 1;
    mem[0xC009] = (mb >> 1) & 1;
    I64 sc = host_key();
    while (sc >= 0) { InjectKey(sc); sc = host_key(); }
    mem[0xC2B0] = 1;

    // Wake scheduler at ~31Hz cadence
    U64 jif = *(U64*)(mem + 0xBD98);
    if (jif - g_wake_jif >= 32 || jif < g_wake_jif) { WakeTasks(1); g_wake_jif = jif; }
    else WakeTasks(0);

    // Auto-arm span-filler HLE: find GrFillPoly3 once
    if (!g_poly3_scanned) {
        g_poly3_scanned = 1;
        for (U64 pa = 0x11000000; pa < 0x11C00000; pa++) {
            if (mem[pa]==0x55 && mem[pa+1]==0x48 && mem[pa+2]==0x8B && mem[pa+3]==0xEC &&
                mem[pa+4]==0x48 && mem[pa+5]==0x83 && mem[pa+6]==0xEC && mem[pa+7]==0x58 &&
                mem[pa+0x12]==0x48 && mem[pa+0x14]==0x75 && mem[pa+0x15]==0x10 &&
                mem[pa+0x16]==0x4C && mem[pa+0x18]==0x55 && mem[pa+0x19]==0x20 &&
                mem[pa+0x1A]==0x4C && mem[pa+0x1C]==0x7D && mem[pa+0x1D]==0x18) {
                g_poly3_hle = pa;
                g_hooktbl[pa & 0xFFFF] = 1;
                g_jit_rip[(pa ^ (pa >> 16)) & 0xFFFF] = pa;
                g_jit_n[(pa ^ (pa >> 16)) & 0xFFFF] = -2;
                break;
            }
        }
    }

    // Raster HLE management
    if (g_game_running) {
        if (!g_raster_cand) {
            if (g_raster_scan_from < 0x11C00000) {
                if (g_raster_n < 4) ArmRaster();
            } else {
                g_raster_tries++;
                if ((g_raster_tries & 31) == 0 && g_raster_tries < 600 && g_raster_n < 4) {
                    g_raster_scan_from = 0x11000000;
                } else if (g_cold_n && (g_raster_tries & 63) == 0 && g_raster_n < 4) {
                    I64 ci, cq, ok;
                    U64 ca;
                    for (ci = 0; ci < g_cold_n; ci++) {
                        ca = g_raster_cold[(g_cold_i + ci) % g_cold_n];
                        ok = 1;
                        for (cq = 0; cq < g_raster_n; cq++)
                            if (ca == g_raster_hle[cq]) ok = 0;
                        if (ok) {
                            g_cold_i = (g_cold_i + ci + 1) % g_cold_n;
                            g_raster_cand = ca;
                            g_hooktbl[ca & 0xFFFF] = 1;
                            g_jit_n[(ca ^ (ca >> 16)) & 0xFFFF] = 0;
                            g_raster_verify = 1;
                            g_raster_checks = 0;
                            g_raster_diffs = 0;
                            g_raster_armframes = 0;
                            ci = 99;
                        }
                    }
                }
            }
        } else {
            g_raster_armframes++;
            if (g_raster_diffs >= 3 && g_raster_diffs * 10 >= g_raster_checks) {
                // wrong fn: reject
                U64 rc = g_raster_cand;
                if (g_rej_n < 8) { g_raster_rej[g_rej_n] = rc; g_rej_n++; }
                g_raster_cand = 0;
                g_raster_verify = 0;
                g_jit_n[(rc ^ (rc >> 16)) & 0xFFFF] = -1;
                HookClrAt(rc);
            } else if (g_raster_checks >= 100 && g_raster_diffs * 100 <= g_raster_checks) {
                // validated -> ACTIVE
                g_raster_hle[g_raster_n] = g_raster_cand;
                g_raster_n++;
                g_jit_rip[(g_raster_cand ^ (g_raster_cand >> 16)) & 0xFFFF] = g_raster_cand;
                g_jit_n[(g_raster_cand ^ (g_raster_cand >> 16)) & 0xFFFF] = -2;
                g_raster_cand = 0;
                g_raster_verify = 0;
            } else if (g_raster_armframes > 30 && g_raster_checks == 0) {
                // cold: remember for retry
                U64 cc = g_raster_cand;
                I64 cq, dup = 0;
                g_raster_cand = 0;
                g_raster_verify = 0;
                for (cq = 0; cq < g_cold_n; cq++)
                    if (g_raster_cold[cq] == cc) dup = 1;
                if (!dup && g_cold_n < 8) { g_raster_cold[g_cold_n] = cc; g_cold_n++; }
                g_jit_n[(cc ^ (cc >> 16)) & 0xFFFF] = -1;
                HookClrAt(cc);
            }
        }
    } else {
        // game exited: clear every raster hook
        if (g_raster_n || g_raster_cand || g_raster_ret) {
            U64 xa;
            xa = g_raster_cand; g_raster_cand = 0; HookClrAt(xa);
            if (g_raster_ret) RetHookClr();
            while (g_raster_n) {
                g_raster_n--;
                xa = g_raster_hle[g_raster_n];
                g_raster_hle[g_raster_n] = 0;
                HookClrAt(xa);
                g_jit_n[(xa ^ (xa >> 16)) & 0xFFFF] = -1;
            }
        }
        g_raster_verify = 0;
        g_rej_n = 0;
        g_raster_tries = 0;
        g_raster_scan_from = 0x11000000;
        g_cold_n = 0;
        g_cold_i = 0;
    }

    // Wall-clock pacing
    I64 dt = host_dt();
    if (dt < 1) dt = 1; else if (dt > 100) dt = 100;
    I64 B = host_budget();
    if (B < 50000) B = 50000;
    pit_div = B / dt;
    if (pit_div < 1) pit_div = 1;
    pit_counter = pit_div;

    // Q16 fixed-point clock rates
    g_tsc_q16 = (I64)(((int64_t)(1000000LL * dt) << 16) / B);
    if (g_tsc_q16 < 1) g_tsc_q16 = 1;
    g_hpet_q16 = (I64)(((int64_t)(100000LL * dt) << 16) / B);
    if (g_hpet_q16 < 1) g_hpet_q16 = 1;
    tsc_rate = (I64)(g_tsc_q16 >> 16);
    if (tsc_rate < 1) tsc_rate = 1;

    s_budget = (I64)icount + B;
    Run(s_budget);
    Present();
}

// ---------------------------------------------------------------------------
// emu_main -- host calls once per display frame (was Main() in HolyC)
// ---------------------------------------------------------------------------
void emu_main()
{
    if (!g_inited) {
        fprintf(stderr, "[hemu] Allocating 384 MiB guest RAM...\n");
        InitMem(402653184);                  // 384 MiB guest RAM
        fprintf(stderr, "[hemu] Loading RAM snapshot...\n");
        host_snap_load(mem);                 // load the RAM snapshot

        if (g_smp_mode) {
            // Sanity check: the SMP snapshot has its dc2 CDC at 0x115c51d8.
            // In the single-core snapshot that address holds different data.
            // Check that the CDC looks valid (width ~640 at offset +20).
            U64 w_check = *(U64*)(mem + 0x115c51d8 + 20) & 0xFFFF;
            if (w_check < 600 || w_check > 720) {
                fprintf(stderr, "[hemu] ERROR: --smp requires the SMP snapshot (live-smp.bin.gz),\n"
                                "       but the loaded image appears to be the single-core snapshot.\n"
                                "       Use: hemu-sdl --smp ../Hemu-wasm/live-smp.bin.gz <disk>\n");
                exit(1);
            }
            fprintf(stderr, "[hemu] Setting SMP snapshot registers (4 cores)...\n");
            SetSnapRegsSmp();
            // SMP snapshot HLE addresses (shifted by -0x3e8200 vs single-core)
            g_dc2 = 0x115c51d8;
            mem[0xC2B0] = 1;
            g_hle_blit    = 0x115e0228;
            g_capture_rip = 0x115d008f;
            g_skip_c4     = 0x115fa329;
            g_skip_vga    = 0x1126b4e0;
            g_skip_xlat   = 0x115fa228;
            g_skip_bg     = 0x1126d628;
        } else {
            fprintf(stderr, "[hemu] Setting snapshot registers...\n");
            SetSnapRegs();
            g_dc2 = 0x119AD3D8;
            *(U64*)(mem + 0x1140cc10) = 0;
            *(U64*)(mem + 0x1140cc18) = 0;
            *(F64*)(mem + 0x1140cc08) = 31.25;
            mem[0xC2B0] = 1;
            g_hle_blit    = 0x119c8428;
            g_capture_rip = 0x119b828f;
            g_skip_c4     = 0x119e2529;
            g_skip_vga    = 0x116536e0;
            g_skip_xlat   = 0x119e2428;
            g_skip_bg     = 0x11655828;
        }

        g_hooktbl[g_hle_blit & 0xFFFF] = 1;
        g_hooktbl[g_capture_rip & 0xFFFF] = 1;
        g_hooktbl[g_skip_c4 & 0xFFFF] = 1;
        g_hooktbl[g_skip_vga & 0xFFFF] = 1;
        g_hooktbl[g_skip_xlat & 0xFFFF] = 1;
        g_hooktbl[g_skip_bg & 0xFFFF] = 1;

        g_jiffies0 = *(U64*)(mem + 0xBD98);

        // JIT disabled in SDL build
        g_jit_on = 0;

        g_inited = 1;
        fprintf(stderr, "[hemu] Boot complete (%d cores). Running emulator.\n", (int)g_ncore);
    }
    Frame();
}

// ---------------------------------------------------------------------------
// SMP: AP (application processor) threads
// ---------------------------------------------------------------------------
static std::atomic<bool> s_smp_running{false};
static pthread_t s_ap_threads[NCORES];
static int s_ap_count = 0;

static void* ap_thread_fn(void* arg) {
    int core_id = (int)(intptr_t)arg;
    t_my_core = core_id;
    fprintf(stderr, "[hemu] AP%d thread started\n", core_id);

    const I64 AP_BUDGET = 2000000;
    int idle_spins = 0;

    while (s_smp_running.load(std::memory_order_relaxed)) {
        I64 before = icount;
        RunCore(AP_BUDGET);
        I64 did = icount - before;

        if (halted >= 2) {
            // AP faulted — clear and continue (best-effort recovery)
            fprintf(stderr, "[hemu] AP%d FAULT halted=%lld rip=0x%llx — recovering\n",
                    core_id, (long long)halted, (unsigned long long)rip);
            halted = 0;
        }

        // yield when idle (don't pin a host CPU core)
        if (did < 1000) {
            idle_spins++;
            if (idle_spins > 4) {
                struct timespec ts = {0, 1000000}; // 1ms
                nanosleep(&ts, nullptr);
                idle_spins = 0;
            }
        } else {
            idle_spins = 0;
        }
    }
    fprintf(stderr, "[hemu] AP%d thread exiting\n", core_id);
    return nullptr;
}

void smp_start() {
    if (g_ncore <= 1) return;
    s_smp_running.store(true, std::memory_order_relaxed);
    s_ap_count = 0;
    for (int i = 1; i < g_ncore && i < NCORES; i++) {
        if (pthread_create(&s_ap_threads[i], nullptr, ap_thread_fn, (void*)(intptr_t)i) == 0)
            s_ap_count++;
        else
            fprintf(stderr, "[hemu] failed to create AP%d thread\n", i);
    }
    fprintf(stderr, "[hemu] SMP: %d AP threads launched\n", s_ap_count);
}

void smp_stop() {
    s_smp_running.store(false, std::memory_order_relaxed);
    for (int i = 1; i <= s_ap_count; i++)
        pthread_join(s_ap_threads[i], nullptr);
    s_ap_count = 0;
}
