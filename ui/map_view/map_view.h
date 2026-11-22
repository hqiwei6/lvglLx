#ifndef MAP_VIEW_H
#define MAP_VIEW_H

#include "lvgl/lvgl.h"

/*
 * 单瓦片滑动浏览页（导航页的简化版交互）
 *   - 内存里只有一张瓦片, 放大 SCALE 倍显示, 可拖动滑动
 *   - 滑动结束时计算屏幕中心落在新瓦片 → 后台下载 → 换图重新锚定
 *   - 复用 nav/tile_loader 的网络下载线程
 */
void map_view_create(void);

#endif /* MAP_VIEW_H */
