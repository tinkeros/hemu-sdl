//============================================================================
// main.cpp -- SDL2 main loop for hemu-sdl TempleOS emulator.
//============================================================================
#include <SDL2/SDL.h>
#include <cstdio>
#include <cstring>
#include "types.h"
#include "snapshot.h"
#include "host.h"

extern bool g_smp_mode;

int main(int argc, char** argv)
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s [--smp] <live.bin.gz> <disk.qcow2.gz>\n", argv[0]);
        return 1;
    }

    int argi = 1;
    if (argc > 3 && strcmp(argv[1], "--smp") == 0) {
        g_smp_mode = true;
        argi = 2;
    }

    if (argc - argi < 2) {
        fprintf(stderr, "Usage: %s [--smp] <live.bin.gz> <disk.qcow2.gz>\n", argv[0]);
        return 1;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    const char* title = g_smp_mode ? "TempleOS - HEMU (SMP)" : "TempleOS - HEMU";
    SDL_Window* win = SDL_CreateWindow(title,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        640 * 2, 480 * 2, SDL_WINDOW_RESIZABLE);
    if (!win) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer* ren = SDL_CreateRenderer(win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }
    SDL_RenderSetLogicalSize(ren, 640, 480);

    SDL_Texture* tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING, 640, 480);
    if (!tex) {
        fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        SDL_DestroyRenderer(ren);
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }

    host_init_display(ren, tex);
    host_init_snapshot(argv[argi]);
    host_init_disk(argv[argi + 1]);

    // First frame: initializes guest RAM, loads snapshot, seeds registers
    emu_main();
    host_render();

    // Launch AP threads if SMP
    if (g_smp_mode) smp_start();

    bool quit = false;
    while (!quit) {
        U64 t0 = SDL_GetTicks64();
        host_poll_events(&quit);
        emu_main();
        host_render();
        U64 t1 = SDL_GetTicks64();
        host_adapt_budget((I64)(t1 - t0));
    }

    if (g_smp_mode) smp_stop();

    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
