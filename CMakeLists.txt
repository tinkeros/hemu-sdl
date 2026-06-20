cmake_minimum_required(VERSION 3.16)
project(hemu-sdl LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)

add_executable(hemu-sdl
    src/main.cpp
    src/cpu.cpp
    src/snapshot.cpp
    src/snapregs.cpp
    src/snapregs-smp.cpp
    src/host.cpp
    src/qcow2.cpp
)
target_include_directories(hemu-sdl PRIVATE src ${SDL2_INCLUDE_DIRS})
target_link_libraries(hemu-sdl ${SDL2_LIBRARIES} ZLIB::ZLIB Threads::Threads m)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
target_compile_options(hemu-sdl PRIVATE -O3 -march=native -Wall -Wno-unused-variable -Wno-misleading-indentation)
