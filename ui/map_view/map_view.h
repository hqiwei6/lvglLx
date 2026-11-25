#ifndef MAP_VIEW_H
#define MAP_VIEW_H

#include "lvgl/lvgl.h"

/*
 * 瓦片拼接地图浏览页
 *   - 视口内用槽位对象池平铺 256px 原生瓦片, 手动拖拽(任意方向) + 按需补图
 *   - 缩放按钮换 z 级别: 以屏幕中心为锚点换算坐标, 全槽复位重新调度
 *   - 复用 nav/tile_loader 的网络下载线程
 */
void map_view_create(void);

#endif /* MAP_VIEW_H */
