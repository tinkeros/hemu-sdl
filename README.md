# hemu-sdl

A native C++/SDL2 port of **HEMU** — a clean-room, TempleOS-only x86-64 emulator.
Boots a live TempleOS V5.03 desktop from a RAM snapshot in under a second, with
keyboard, mouse, disk I/O, and PC speaker sound. No JIT (yet) — pure interpreter
with HLE hooks for the hot compositor and rendering paths.

This is a direct translation of the [Hemu-wasm](https://github.com/ParkerrDev/Hemu-wasm)
HolyC/WASM emulator to standalone C++ with SDL2 replacing the browser host layer.

## Quick start

```bash
# Prerequisites: SDL2, zlib, cmake, a C++17 compiler
# Debian/Ubuntu: sudo apt install libsdl2-dev zlib1g-dev cmake g++
# Arch:          sudo pacman -S sdl2 zlib cmake gcc

cd hemu-sdl
cmake -B build
cmake --build build

# Run (needs the RAM snapshot and disk image from the sibling repos)
./build/hemu-sdl ../Hemu-wasm/live.bin.gz \
                 ../TempleOS-Web/vendor/images/templeos-hd.qcow2.gz
```

