#pragma once
#include "types.h"

// Host interface -- implemented by SDL2 in host.cpp
void host_present(U8* buf, int w, int h);
void host_snap_load(U8* mem);
void host_disk(U64 lba, U64 cnt, U8* buf);
void host_disk_wr(U64 lba, U64 cnt, U8* buf);
I64  host_msx();
I64  host_msy();
I64  host_msb();
I64  host_wheel();
I64  host_key();
I64  host_dt();
I64  host_budget();
U64  host_time(I64 idx);
void host_snd(double freq);

// Called by main.cpp
void host_init_display(void* renderer, void* texture);
void host_init_disk(const char* disk_path);
void host_init_snapshot(const char* snap_path);
void host_poll_events(bool* quit);
void host_render();
void host_adapt_budget(I64 frame_ms);
