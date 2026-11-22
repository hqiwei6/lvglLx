#
# CMakeLists.txt
#
# 双目标构建：
#   SDL2 模拟器（PC）:
#     cmake -B build -DUSE_SDL_SIM=ON   && cmake --build build
#   ARM 开发板（framebuffer + evdev）:
#     cmake -B build-arm -DUSE_SDL_SIM=OFF -DCMAKE_C_COMPILER=aarch64-linux-gcc
#     && cmake --build build-arm
#
# 原理：lv_drv_conf.h 中开关都包在 #ifndef 里，这里通过编译宏
# -DUSE_SDL=1 -DUSE_FBDEV=0 -DUSE_EVDEV=0 覆盖，lv_drv_conf.h 本身不用改。
#
cmake_minimum_required(VERSION 3.12.4)
project(lvglLx C)

find_package(Threads REQUIRED)   # 瓦片下载线程

option(USE_SDL_SIM "Build with SDL2 simulator instead of framebuffer/evdev (ARM board)" ON)

if(USE_SDL_SIM)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED sdl2)
    add_definitions(-DUSE_SDL=1 -DUSE_FBDEV=0 -DUSE_EVDEV=0)
endif()

# 瓦片目录透传: cmake -B build -DUSE_SDL_SIM=ON -DTILE_BASE_DIR="/tmp/tiles"
if(TILE_BASE_DIR)
    string(REPLACE "\"" "" TILE_BASE_DIR "${TILE_BASE_DIR}")   # 剥掉传入值里可能带的引号
    add_definitions(-DTILE_BASE_DIR="${TILE_BASE_DIR}")        # 统一按带引号字符串传给编译器
endif()

# lv_conf.h / lv_drv_conf.h 都在项目根目录
include_directories(${CMAKE_SOURCE_DIR} ${SDL2_INCLUDE_DIRS})

add_subdirectory(lvgl)
add_subdirectory(lv_drivers)

# 每个页面一个独立文件夹 (ui/<页面名>/)，自动收集（新增文件夹后重新 cmake 即可）
file(GLOB UI_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/ui/*/*.c)

add_executable(demo
    main.c
    mouse_cursor_icon.c
    ${UI_SOURCES}
)

target_link_libraries(demo PRIVATE lvgl lvgl_drivers m Threads::Threads)

if(USE_SDL_SIM)
    target_link_libraries(demo PRIVATE ${SDL2_LIBRARIES})
endif()
