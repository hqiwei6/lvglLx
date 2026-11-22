#ifndef NAV_PAGE_H
#define NAV_PAGE_H

#include "lvgl/lvgl.h"

/*
 * 瓦片存放目录（天地图 DataServer 下载的 PNG）
 * 命名规则: {级别l}_{x}_{y}.png，例如 13_6744_3103.png
 *
 * 开发板默认 /userdata/tiles；
 * PC 模拟器可编译期覆盖: cmake -DTILE_BASE_DIR='"/tmp/tiles"' ...
 */
#ifndef TILE_BASE_DIR
#define TILE_BASE_DIR "/userdata/tiles"
#endif

/* 创建导航页（默认以北京为中心，级别 13） */
void nav_page_create(void);

#endif /* NAV_PAGE_H */
