# Wii U build (devkitPPC + wut). Built by tools/build-wiiu.ps1, which also regenerates ../../gen and
# shaders/flashplayer_gsh.c first. Output: flashplayeremu.rpx / flashplayeremu.wuhb.
cmake_minimum_required(VERSION 3.20)
project(flashplayeremu C CXX ASM)

set(HOST ${CMAKE_CURRENT_LIST_DIR}/..)
set(GEN ${HOST}/../gen)
file(GLOB GEN_SOURCES ${GEN}/flashplayer_*.c)

# -Os: the code must stay under the Wii U's 32 MB code area (it reached 34 MB at -O1 once Stage3D's
# shader translator was added, and the app then hung on a white screen before main).
# Not -Os: it miscompiled the generated code (AS3 regex searches ran away allocating and trapped, on
# the console only; -fno-strict-aliasing didn't help, -O1 did). -fno-inline keeps -O1 small enough: most
# of -O1's size is wasm2c's load/store helpers inlined into every memory access.
# (Since wasm memory accesses lost their bounds checks and are inlined (RW_UNCHECKED_MEMORY), .text is
# ~19 MB; plain -O1 and -O2 measured no faster in Cemu, so the console-tested -O1 -fno-inline stays.)
set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "-O1;-fno-inline;-w")

add_executable(flashplayeremu
    ${GEN_SOURCES}
    ${HOST}/wasm-rt/wasm-rt-impl.c
    ${HOST}/wasm-rt/wasm-rt-mem-impl.c
    ${HOST}/wasm-rt/wasm-rt-exceptions-impl.c
    ${HOST}/rw_core.c
    ${HOST}/perlin.c
    gfx_gx2.c
    kbd.cpp
    main_wiiu.c
    music.c
    pads.c
    video_h264.c
    net_wiiu.c
    ui.c
    stack_switch.S
    shaders/flashplayer_gsh.c)
target_include_directories(flashplayeremu PRIVATE ${HOST} ${HOST}/wasm-rt ${GEN})
target_compile_definitions(flashplayeremu PRIVATE
    WABT_BIG_ENDIAN=1
    WASM_RT_USE_MMAP=0
    WASM_RT_MEMCHECK_BOUNDS_CHECK=1
    WASM_RT_NO_THREAD_LOCAL=1
    WASM_RT_MAX_CALL_STACK_DEPTH=20000
    WASM_RT_GROW_LOG=rw_grow_log
    # Reserve as much wasm memory as the console can spare up front (main_wiiu.c: rw_mem_reserve).
    RW_MEM_RESERVE=rw_mem_reserve
    RW_MEM_RELEASE=rw_mem_release
    RW_MEM_CAN_GROW=rw_mem_can_grow
    # No wasm bounds checks, inlined memory accessors (tools/gen.ps1): the console was CPU-bound in them.
    RW_UNCHECKED_MEMORY=1)
target_compile_options(flashplayeremu PRIVATE $<$<COMPILE_LANGUAGE:C>:-O2>)
# Newer wut declares OSCreateThread's entry point with a different (but call-compatible) argv type,
# and GCC 14+ makes that an error; keep it a warning so the code builds with both old and new wut.
target_compile_options(flashplayeremu PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wno-error=incompatible-pointer-types>)
# Assemble through sa/tools/as-crfix (built by sa/tools/wiiu-game-build.ps1): a no-op `mcrf 0,0` before
# each mfcr, so Cemu's recompiler doesn't hand GCC's CR spills a stale cr0 once code gets hot (seen here
# as AS3 class names read as garbage after a few games). One cycle on the console.
target_compile_options(flashplayeremu PRIVATE $<$<COMPILE_LANGUAGE:C>:-B${CMAKE_CURRENT_LIST_DIR}/../../../sa/tools/as-crfix/bin/>)

find_package(PkgConfig REQUIRED)
pkg_check_modules(RW_SDL REQUIRED sdl2)
target_include_directories(flashplayeremu PRIVATE ${RW_SDL_STATIC_INCLUDE_DIRS})
target_link_directories(flashplayeremu PRIVATE ${RW_SDL_STATIC_LIBRARY_DIRS})
target_link_libraries(flashplayeremu PRIVATE ${RW_SDL_STATIC_LIBRARIES} m)

# CafeGLSL (Mesa-based GLSL -> GX2 compiler) for Stage3D shaders, compiled at runtime.
# Unpacked from cafeglsl-1.0.0-rc1-wiiu-ppc.tar.xz; adds a lot of code, keep an eye on the 32 MB limit.
set(CAFEGLSL ${CMAKE_CURRENT_LIST_DIR}/../../../buildtools/cafeglsl-wiiu/cafeglsl-1.0.0-rc1-wiiu-ppc)
target_include_directories(flashplayeremu PRIVATE ${CAFEGLSL}/include)
target_link_libraries(flashplayeremu PRIVATE ${CAFEGLSL}/lib/libcafeglsl.a stdc++)

# mbedTLS for game network connections (net_wiiu.c). Unpacked by ci-buildtools.ps1 from the official release.
set(MBEDTLS ${CMAKE_CURRENT_LIST_DIR}/../../../buildtools/mbedtls/mbedtls-3.6.7)
file(GLOB MBEDTLS_SOURCES ${MBEDTLS}/library/*.c)
add_library(rw_mbedtls STATIC ${MBEDTLS_SOURCES})
target_include_directories(rw_mbedtls PUBLIC ${MBEDTLS}/include ${CMAKE_CURRENT_LIST_DIR})
target_compile_definitions(rw_mbedtls PUBLIC MBEDTLS_USER_CONFIG_FILE="mbedtls_config_wiiu.h")
target_compile_options(rw_mbedtls PRIVATE -O2 -w)
target_link_libraries(flashplayeremu PRIVATE rw_mbedtls)

wut_create_rpx(flashplayeremu)
# content/ (/vol/content on the console): the menu music, game text fonts and CREDITS.txt. Each theme plays
# its own track from content/music (main_wiiu.c THEME_MUSIC); there is no custom music.
wut_create_wuhb(flashplayeremu
    CONTENT "${CMAKE_CURRENT_LIST_DIR}/content"
    NAME "Flashplayer Emu"
    SHORTNAME "Flashplayer Emu"
    AUTHOR "Charliebear31 (engine: Ruffle)"
    # home menu icon and boot screens, drawn by tools/make-art.ps1 (no boot sound)
    ICON "${CMAKE_CURRENT_LIST_DIR}/meta/icon.png"
    TVSPLASH "${CMAKE_CURRENT_LIST_DIR}/meta/tv.png"
    DRCSPLASH "${CMAKE_CURRENT_LIST_DIR}/meta/drc.png")
