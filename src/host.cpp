//============================================================================
// host.cpp -- SDL2 host interface for hemu-sdl.
// Implements display, input, timing, audio, snapshot loading, and disk I/O.
//============================================================================
#include "host.h"
#include "qcow2.h"
#include <SDL2/SDL.h>
#include <zlib.h>
#include <ctime>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>

// ---------------------------------------------------------------------------
// TempleOS 16-color palette (ARGB32)
// ---------------------------------------------------------------------------
static const U32 PALETTE[16] = {
    0xFF000000, 0xFF0000AA, 0xFF00AA00, 0xFF00AAAA,
    0xFFAA0000, 0xFFAA00AA, 0xFFAA5500, 0xFFAAAAAA,
    0xFF555555, 0xFF5555FF, 0xFF55FF55, 0xFF55FFFF,
    0xFFFF5555, 0xFFFF55FF, 0xFFFFFF55, 0xFFFFFFFF,
};

// ---------------------------------------------------------------------------
// Display state
// ---------------------------------------------------------------------------
static SDL_Renderer* s_renderer = nullptr;
static SDL_Texture*  s_texture  = nullptr;
static U32 s_framebuf[640 * 480];  // ARGB32 expansion buffer
static bool s_frame_dirty = false;

void host_init_display(void* renderer, void* texture)
{
    s_renderer = (SDL_Renderer*)renderer;
    s_texture  = (SDL_Texture*)texture;
}

void host_present(U8* buf, int w, int h)
{
    int pixels = w * h;
    if (pixels > 640 * 480) pixels = 640 * 480;
    // Unrolled palette expansion — 4 pixels at a time
    int i = 0;
    for (; i + 3 < pixels; i += 4) {
        s_framebuf[i]   = PALETTE[buf[i]   & 0x0F];
        s_framebuf[i+1] = PALETTE[buf[i+1] & 0x0F];
        s_framebuf[i+2] = PALETTE[buf[i+2] & 0x0F];
        s_framebuf[i+3] = PALETTE[buf[i+3] & 0x0F];
    }
    for (; i < pixels; i++)
        s_framebuf[i] = PALETTE[buf[i] & 0x0F];
    SDL_UpdateTexture(s_texture, nullptr, s_framebuf, w * 4);
    s_frame_dirty = true;
}

void host_render()
{
    if (s_frame_dirty) {
        SDL_RenderClear(s_renderer);
        SDL_RenderCopy(s_renderer, s_texture, nullptr, nullptr);
        SDL_RenderPresent(s_renderer);
        s_frame_dirty = false;
    }
}

// ---------------------------------------------------------------------------
// PS/2 Set-1 scancode mapping from SDL scancodes
// ---------------------------------------------------------------------------
static int sdl_to_ps2[SDL_NUM_SCANCODES];
static bool s_scancode_map_inited = false;

static void init_scancode_map()
{
    if (s_scancode_map_inited) return;
    s_scancode_map_inited = true;
    memset(sdl_to_ps2, 0, sizeof(sdl_to_ps2));

    // Row 1: Escape, 1-0, minus, equals, backspace
    sdl_to_ps2[SDL_SCANCODE_ESCAPE]    = 0x01;
    sdl_to_ps2[SDL_SCANCODE_1]         = 0x02;
    sdl_to_ps2[SDL_SCANCODE_2]         = 0x03;
    sdl_to_ps2[SDL_SCANCODE_3]         = 0x04;
    sdl_to_ps2[SDL_SCANCODE_4]         = 0x05;
    sdl_to_ps2[SDL_SCANCODE_5]         = 0x06;
    sdl_to_ps2[SDL_SCANCODE_6]         = 0x07;
    sdl_to_ps2[SDL_SCANCODE_7]         = 0x08;
    sdl_to_ps2[SDL_SCANCODE_8]         = 0x09;
    sdl_to_ps2[SDL_SCANCODE_9]         = 0x0A;
    sdl_to_ps2[SDL_SCANCODE_0]         = 0x0B;
    sdl_to_ps2[SDL_SCANCODE_MINUS]     = 0x0C;
    sdl_to_ps2[SDL_SCANCODE_EQUALS]    = 0x0D;
    sdl_to_ps2[SDL_SCANCODE_BACKSPACE] = 0x0E;

    // Row 2: Tab, Q-P, brackets, return
    sdl_to_ps2[SDL_SCANCODE_TAB]          = 0x0F;
    sdl_to_ps2[SDL_SCANCODE_Q]            = 0x10;
    sdl_to_ps2[SDL_SCANCODE_W]            = 0x11;
    sdl_to_ps2[SDL_SCANCODE_E]            = 0x12;
    sdl_to_ps2[SDL_SCANCODE_R]            = 0x13;
    sdl_to_ps2[SDL_SCANCODE_T]            = 0x14;
    sdl_to_ps2[SDL_SCANCODE_Y]            = 0x15;
    sdl_to_ps2[SDL_SCANCODE_U]            = 0x16;
    sdl_to_ps2[SDL_SCANCODE_I]            = 0x17;
    sdl_to_ps2[SDL_SCANCODE_O]            = 0x18;
    sdl_to_ps2[SDL_SCANCODE_P]            = 0x19;
    sdl_to_ps2[SDL_SCANCODE_LEFTBRACKET]  = 0x1A;
    sdl_to_ps2[SDL_SCANCODE_RIGHTBRACKET] = 0x1B;
    sdl_to_ps2[SDL_SCANCODE_RETURN]       = 0x1C;

    // Row 3: LCtrl, A-L, semicolon, apostrophe, grave, LShift, backslash
    sdl_to_ps2[SDL_SCANCODE_LCTRL]      = 0x1D;
    sdl_to_ps2[SDL_SCANCODE_A]          = 0x1E;
    sdl_to_ps2[SDL_SCANCODE_S]          = 0x1F;
    sdl_to_ps2[SDL_SCANCODE_D]          = 0x20;
    sdl_to_ps2[SDL_SCANCODE_F]          = 0x21;
    sdl_to_ps2[SDL_SCANCODE_G]          = 0x22;
    sdl_to_ps2[SDL_SCANCODE_H]          = 0x23;
    sdl_to_ps2[SDL_SCANCODE_J]          = 0x24;
    sdl_to_ps2[SDL_SCANCODE_K]          = 0x25;
    sdl_to_ps2[SDL_SCANCODE_L]          = 0x26;
    sdl_to_ps2[SDL_SCANCODE_SEMICOLON]  = 0x27;
    sdl_to_ps2[SDL_SCANCODE_APOSTROPHE] = 0x28;
    sdl_to_ps2[SDL_SCANCODE_GRAVE]      = 0x29;
    sdl_to_ps2[SDL_SCANCODE_LSHIFT]     = 0x2A;
    sdl_to_ps2[SDL_SCANCODE_BACKSLASH]  = 0x2B;

    // Row 4: Z-M, comma, period, slash, RShift
    sdl_to_ps2[SDL_SCANCODE_Z]      = 0x2C;
    sdl_to_ps2[SDL_SCANCODE_X]      = 0x2D;
    sdl_to_ps2[SDL_SCANCODE_C]      = 0x2E;
    sdl_to_ps2[SDL_SCANCODE_V]      = 0x2F;
    sdl_to_ps2[SDL_SCANCODE_B]      = 0x30;
    sdl_to_ps2[SDL_SCANCODE_N]      = 0x31;
    sdl_to_ps2[SDL_SCANCODE_M]      = 0x32;
    sdl_to_ps2[SDL_SCANCODE_COMMA]  = 0x33;
    sdl_to_ps2[SDL_SCANCODE_PERIOD] = 0x34;
    sdl_to_ps2[SDL_SCANCODE_SLASH]  = 0x35;
    sdl_to_ps2[SDL_SCANCODE_RSHIFT] = 0x36;

    // Keypad multiply, LAlt, Space, CapsLock
    sdl_to_ps2[SDL_SCANCODE_KP_MULTIPLY] = 0x37;
    sdl_to_ps2[SDL_SCANCODE_LALT]        = 0x38;
    sdl_to_ps2[SDL_SCANCODE_SPACE]       = 0x39;
    sdl_to_ps2[SDL_SCANCODE_CAPSLOCK]    = 0x3A;

    // Function keys F1-F10
    sdl_to_ps2[SDL_SCANCODE_F1]  = 0x3B;
    sdl_to_ps2[SDL_SCANCODE_F2]  = 0x3C;
    sdl_to_ps2[SDL_SCANCODE_F3]  = 0x3D;
    sdl_to_ps2[SDL_SCANCODE_F4]  = 0x3E;
    sdl_to_ps2[SDL_SCANCODE_F5]  = 0x3F;
    sdl_to_ps2[SDL_SCANCODE_F6]  = 0x40;
    sdl_to_ps2[SDL_SCANCODE_F7]  = 0x41;
    sdl_to_ps2[SDL_SCANCODE_F8]  = 0x42;
    sdl_to_ps2[SDL_SCANCODE_F9]  = 0x43;
    sdl_to_ps2[SDL_SCANCODE_F10] = 0x44;

    // NumLock, ScrollLock
    sdl_to_ps2[SDL_SCANCODE_NUMLOCKCLEAR] = 0x45;
    sdl_to_ps2[SDL_SCANCODE_SCROLLLOCK]   = 0x46;

    // Keypad 7-9, minus, 4-6, plus, 1-3, 0, period
    sdl_to_ps2[SDL_SCANCODE_KP_7]        = 0x47;
    sdl_to_ps2[SDL_SCANCODE_KP_8]        = 0x48;
    sdl_to_ps2[SDL_SCANCODE_KP_9]        = 0x49;
    sdl_to_ps2[SDL_SCANCODE_KP_MINUS]    = 0x4A;
    sdl_to_ps2[SDL_SCANCODE_KP_4]        = 0x4B;
    sdl_to_ps2[SDL_SCANCODE_KP_5]        = 0x4C;
    sdl_to_ps2[SDL_SCANCODE_KP_6]        = 0x4D;
    sdl_to_ps2[SDL_SCANCODE_KP_PLUS]     = 0x4E;
    sdl_to_ps2[SDL_SCANCODE_KP_1]        = 0x4F;
    sdl_to_ps2[SDL_SCANCODE_KP_2]        = 0x50;
    sdl_to_ps2[SDL_SCANCODE_KP_3]        = 0x51;
    sdl_to_ps2[SDL_SCANCODE_KP_0]        = 0x52;
    sdl_to_ps2[SDL_SCANCODE_KP_PERIOD]   = 0x53;

    // F11, F12
    sdl_to_ps2[SDL_SCANCODE_F11] = 0x57;
    sdl_to_ps2[SDL_SCANCODE_F12] = 0x58;

    // KP Enter, RCtrl (extended, handled separately)
    sdl_to_ps2[SDL_SCANCODE_KP_ENTER]  = 0xE01C;
    sdl_to_ps2[SDL_SCANCODE_RCTRL]     = 0xE01D;
    sdl_to_ps2[SDL_SCANCODE_KP_DIVIDE] = 0xE035;
    sdl_to_ps2[SDL_SCANCODE_RALT]      = 0xE038;

    // Extended keys (0xE0 prefix)
    sdl_to_ps2[SDL_SCANCODE_HOME]     = 0xE047;
    sdl_to_ps2[SDL_SCANCODE_UP]       = 0xE048;
    sdl_to_ps2[SDL_SCANCODE_PAGEUP]   = 0xE049;
    sdl_to_ps2[SDL_SCANCODE_LEFT]     = 0xE04B;
    sdl_to_ps2[SDL_SCANCODE_RIGHT]    = 0xE04D;
    sdl_to_ps2[SDL_SCANCODE_END]      = 0xE04F;
    sdl_to_ps2[SDL_SCANCODE_DOWN]     = 0xE050;
    sdl_to_ps2[SDL_SCANCODE_PAGEDOWN] = 0xE051;
    sdl_to_ps2[SDL_SCANCODE_INSERT]   = 0xE052;
    sdl_to_ps2[SDL_SCANCODE_DELETE]   = 0xE053;

    // Windows/Super keys
    sdl_to_ps2[SDL_SCANCODE_LGUI] = 0xE05B;
    sdl_to_ps2[SDL_SCANCODE_RGUI] = 0xE05C;
    sdl_to_ps2[SDL_SCANCODE_APPLICATION] = 0xE05D;
}

// ---------------------------------------------------------------------------
// Key queue
// ---------------------------------------------------------------------------
static I64 s_keyq[256];
static int s_keyq_head = 0;
static int s_keyq_tail = 0;

static void keyq_push(I64 val)
{
    int next = (s_keyq_head + 1) & 255;
    if (next == s_keyq_tail) return;  // full, drop
    s_keyq[s_keyq_head] = val;
    s_keyq_head = next;
}

I64 host_key()
{
    if (s_keyq_head == s_keyq_tail) return -1;
    I64 val = s_keyq[s_keyq_tail];
    s_keyq_tail = (s_keyq_tail + 1) & 255;
    return val;
}

// ---------------------------------------------------------------------------
// Mouse state
// ---------------------------------------------------------------------------
static I64 s_mouse_x = 320;
static I64 s_mouse_y = 240;
static I64 s_mouse_buttons = 0;
static I64 s_mouse_wheel = 0;
static bool s_mouse_captured = false;

I64 host_msx() { return s_mouse_x; }
I64 host_msy() { return s_mouse_y; }
I64 host_msb() { return s_mouse_buttons; }
I64 host_wheel() { return s_mouse_wheel; }

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------
static U64 s_last_ticks = 0;

I64 host_dt()
{
    U64 now = SDL_GetTicks64();
    if (s_last_ticks == 0) { s_last_ticks = now; return 16; }
    I64 dt = (I64)(now - s_last_ticks);
    s_last_ticks = now;
    return dt;
}

// Dynamic budget: scale up when frames are fast, down when slow (matches browser)
static I64 s_cur_budget = 4000000;

I64 host_budget()
{
    return s_cur_budget;
}

void host_adapt_budget(I64 frame_ms)
{
    if (frame_ms > 18 && s_cur_budget > 900000)
        s_cur_budget = (I64)(s_cur_budget * 0.90);
    else if (frame_ms < 12 && s_cur_budget < 40000000)
        s_cur_budget = (I64)(s_cur_budget * 1.10);
}

// ---------------------------------------------------------------------------
// RTC -- BCD CMOS values
// ---------------------------------------------------------------------------
static U8 to_bcd(int val)
{
    return (U8)(((val / 10) << 4) | (val % 10));
}

U64 host_time(I64 idx)
{
    time_t t = time(nullptr);
    struct tm* lt = localtime(&t);
    switch (idx) {
        case 0: return to_bcd(lt->tm_sec);
        case 2: return to_bcd(lt->tm_min);
        case 4: return to_bcd(lt->tm_hour);
        case 6: return (U64)(lt->tm_wday == 0 ? 7 : lt->tm_wday);  // 1=Mon..7=Sun
        case 7: return to_bcd(lt->tm_mday);
        case 8: return to_bcd(lt->tm_mon + 1);
        case 9: return to_bcd(lt->tm_year % 100);
        default: return 0;
    }
}

// ---------------------------------------------------------------------------
// Sound -- SDL audio: square wave
// ---------------------------------------------------------------------------
static SDL_AudioDeviceID s_audio_dev = 0;
static double s_snd_freq = 0.0;
static double s_snd_phase = 0.0;

static void audio_callback(void* userdata, Uint8* stream, int len)
{
    (void)userdata;
    int16_t* out = (int16_t*)stream;
    int samples = len / 2;
    double freq = s_snd_freq;
    if (freq <= 0.0) {
        memset(stream, 0, (size_t)len);
        return;
    }
    double inc = freq / 44100.0;
    for (int i = 0; i < samples; i++) {
        out[i] = (s_snd_phase < 0.5) ? 4000 : -4000;
        s_snd_phase += inc;
        if (s_snd_phase >= 1.0) s_snd_phase -= 1.0;
    }
}

void host_snd(double freq)
{
    s_snd_freq = freq;
}

static void init_audio()
{
    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof(want));
    want.freq = 44100;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 1024;
    want.callback = audio_callback;
    s_audio_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (s_audio_dev) SDL_PauseAudioDevice(s_audio_dev, 0);
}

// ---------------------------------------------------------------------------
// Snapshot loading
// ---------------------------------------------------------------------------
static const char* s_snap_path = nullptr;

void host_init_snapshot(const char* snap_path)
{
    s_snap_path = snap_path;
}

void host_snap_load(U8* mem_ptr)
{
    if (!s_snap_path) {
        fprintf(stderr, "host_snap_load: no snapshot path set\n");
        return;
    }
    gzFile gz = gzopen(s_snap_path, "rb");
    if (!gz) {
        fprintf(stderr, "host_snap_load: cannot open %s\n", s_snap_path);
        exit(1);
    }
    // Read up to 384 MiB
    const size_t max_size = 384ULL * 1024 * 1024;
    size_t total = 0;
    while (total < max_size) {
        int chunk = 1024 * 1024;  // 1 MiB at a time
        if (total + (size_t)chunk > max_size) chunk = (int)(max_size - total);
        int n = gzread(gz, mem_ptr + total, (unsigned)chunk);
        if (n <= 0) break;
        total += (size_t)n;
    }
    gzclose(gz);
    fprintf(stderr, "host_snap_load: loaded %zu bytes from %s\n", total, s_snap_path);
}

// ---------------------------------------------------------------------------
// Disk -- qcow2 via gzip-compressed image
// ---------------------------------------------------------------------------
static Qcow2* s_disk = nullptr;
static U8* s_disk_buf = nullptr;
static size_t s_disk_buf_len = 0;

void host_init_disk(const char* disk_path)
{
    gzFile gz = gzopen(disk_path, "rb");
    if (!gz) {
        fprintf(stderr, "host_init_disk: cannot open %s\n", disk_path);
        exit(1);
    }
    // Read entire decompressed qcow2 into memory
    size_t cap = 256ULL * 1024 * 1024;  // start with 256 MiB
    s_disk_buf = (U8*)malloc(cap);
    if (!s_disk_buf) {
        fprintf(stderr, "host_init_disk: malloc failed\n");
        exit(1);
    }
    size_t total = 0;
    for (;;) {
        if (total >= cap) {
            cap *= 2;
            U8* nb = (U8*)realloc(s_disk_buf, cap);
            if (!nb) {
                fprintf(stderr, "host_init_disk: realloc failed at %zu\n", cap);
                exit(1);
            }
            s_disk_buf = nb;
        }
        int chunk = 1024 * 1024;
        if (total + (size_t)chunk > cap) chunk = (int)(cap - total);
        int n = gzread(gz, s_disk_buf + total, (unsigned)chunk);
        if (n <= 0) break;
        total += (size_t)n;
    }
    gzclose(gz);
    s_disk_buf_len = total;
    fprintf(stderr, "host_init_disk: loaded %zu bytes from %s\n", total, disk_path);

    s_disk = new Qcow2(s_disk_buf, s_disk_buf_len);
}

void host_disk(U64 lba, U64 cnt, U8* buf)
{
    if (s_disk) s_disk->readInto(lba, cnt, buf, 0);
    else memset(buf, 0, (size_t)(cnt * 512));
}

void host_disk_wr(U64 lba, U64 cnt, U8* buf)
{
    if (s_disk) s_disk->writeInto(lba, cnt, buf, 0);
}

// ---------------------------------------------------------------------------
// Event polling -- keyboard, mouse, window
// ---------------------------------------------------------------------------
void host_poll_events(bool* quit)
{
    init_scancode_map();

    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_QUIT:
            *quit = true;
            break;

        case SDL_KEYDOWN: {
            // Ctrl+Alt+G releases mouse capture
            if (ev.key.keysym.scancode == SDL_SCANCODE_G &&
                (ev.key.keysym.mod & KMOD_CTRL) &&
                (ev.key.keysym.mod & KMOD_ALT)) {
                if (s_mouse_captured) {
                    SDL_SetRelativeMouseMode(SDL_FALSE);
                    s_mouse_captured = false;
                }
                break;
            }
            int sc = sdl_to_ps2[ev.key.keysym.scancode];
            if (!sc) break;
            // Mix in entropy from performance counter (bits 8+)
            U64 entropy = SDL_GetPerformanceCounter();
            if (sc >= 0xE000) {
                // Extended key: push 0xE0 prefix, then the code byte
                I64 code = sc & 0xFF;
                keyq_push((I64)(0xE0 | ((entropy & 0xFFFFFF) << 8)));
                keyq_push((I64)(code | ((entropy & 0xFFFFFF) << 8)));
            } else {
                keyq_push((I64)(sc | ((entropy & 0xFFFFFF) << 8)));
            }
            break;
        }

        case SDL_KEYUP: {
            if (ev.key.keysym.scancode == SDL_SCANCODE_G) break;  // ignore G release from capture toggle
            int sc = sdl_to_ps2[ev.key.keysym.scancode];
            if (!sc) break;
            if (sc >= 0xE000) {
                I64 code = sc & 0xFF;
                keyq_push(0xE0);
                keyq_push(code | 0x80);
            } else {
                keyq_push(sc | 0x80);
            }
            break;
        }

        case SDL_MOUSEBUTTONDOWN: {
            if (!s_mouse_captured) {
                SDL_SetRelativeMouseMode(SDL_TRUE);
                s_mouse_captured = true;
                break;
            }
            if (ev.button.button == SDL_BUTTON_LEFT)
                s_mouse_buttons |= 1;
            if (ev.button.button == SDL_BUTTON_RIGHT)
                s_mouse_buttons |= 2;
            break;
        }

        case SDL_MOUSEBUTTONUP: {
            if (ev.button.button == SDL_BUTTON_LEFT)
                s_mouse_buttons &= ~1;
            if (ev.button.button == SDL_BUTTON_RIGHT)
                s_mouse_buttons &= ~2;
            break;
        }

        case SDL_MOUSEMOTION: {
            if (s_mouse_captured) {
                // In relative mode, accumulate deltas and clamp to 640x480
                s_mouse_x += ev.motion.xrel;
                s_mouse_y += ev.motion.yrel;
                if (s_mouse_x < 0) s_mouse_x = 0;
                if (s_mouse_x > 639) s_mouse_x = 639;
                if (s_mouse_y < 0) s_mouse_y = 0;
                if (s_mouse_y > 479) s_mouse_y = 479;
            } else {
                // Scale window coords to 640x480
                // SDL_RenderSetLogicalSize handles this, but we track manually
                int ww, wh;
                SDL_GetRendererOutputSize(s_renderer, &ww, &wh);
                if (ww > 0 && wh > 0) {
                    s_mouse_x = (I64)ev.motion.x * 640 / ww;
                    s_mouse_y = (I64)ev.motion.y * 480 / wh;
                    if (s_mouse_x < 0) s_mouse_x = 0;
                    if (s_mouse_x > 639) s_mouse_x = 639;
                    if (s_mouse_y < 0) s_mouse_y = 0;
                    if (s_mouse_y > 479) s_mouse_y = 479;
                }
            }
            break;
        }

        case SDL_MOUSEWHEEL: {
            s_mouse_wheel += ev.wheel.y;
            break;
        }
        }  // switch
    }  // while PollEvent

    // Initialize audio lazily (after SDL_Init in main)
    static bool audio_inited = false;
    if (!audio_inited) { init_audio(); audio_inited = true; }
}
