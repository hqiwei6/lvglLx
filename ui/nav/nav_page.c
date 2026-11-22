/**
 * @file nav_page.c
 * @brief 导航页 —— 瓦片地图显示（第一阶段：加载本地天地图瓦片）
 *
 * 核心思路: 车不动、地图动。
 *   - 地图区放 MAP_COLS × MAP_ROWS 个 lv_img 组成瓦片对象池（固定创建、复用）
 *   - 每次刷新按车的经纬度算出瓦片编号与屏幕摆位，src 变了才 set（省解码）
 *   - 车标固定在屏幕中心，不属于瓦片池
 *
 * 数学: Web 墨卡托（slippy map）
 *   fx = (lon + 180) / 360
 *   fy = (1 - ln(tan(lat) + 1/cos(lat)) / PI) / 2
 *   瓦片编号 x = fx * 2^l, y = fy * 2^l（floor）
 *   fx/fy 与级别无关，级别只影响乘数 2^l 和全图像素坐标
 */
#include "nav_page.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* 中文字库（ui/fonts/ 下，由脚本生成） */
LV_FONT_DECLARE(lv_font_cn_16)
LV_FONT_DECLARE(lv_font_cn_20)

/* ---------------- 布局常量 ---------------- */
#define TILE_SIZE    256                 /* 天地图标准瓦片尺寸 */
#define MAP_COLS     5                   /* 1024/256=4, +1 兜底车在瓦片边缘的最坏情况 */
#define MAP_ROWS     4                   /* 552/256≈2.2, +1 同理 */
#define TOPBAR_H     48
#define MAP_W        1024
#define MAP_H        (600 - TOPBAR_H)    /* 552 */

#define LEVEL_MIN    3
#define LEVEL_MAX    18

/* ---------------- 页面数据 ---------------- */
/* 默认位置: 广州从化·粤嵌(城鳌大道东路999号8幢)
 * 坐标由天地图地理编码 API 解析, CGCS2000 ≈ WGS84, 与 vec_w 瓦片同源无偏移 */
static double car_lon = 113.52782;
static double car_lat = 23.56941;
static int    map_level = 15;

static lv_obj_t *tile_img[MAP_ROWS][MAP_COLS];      /* 瓦片对象池 */
static char     tile_src[MAP_ROWS][MAP_COLS][96];   /* 各格当前路径，防重复 set */
static lv_obj_t *level_label;
static lv_obj_t *status_label;

/* ---------------- 墨卡托数学 ---------------- */
static double frac_x(double lon)
{
    return (lon + 180.0) / 360.0;
}

static double frac_y(double lat)
{
    double rad = lat * M_PI / 180.0;
    return (1.0 - log(tan(rad) + 1.0 / cos(rad)) / M_PI) / 2.0;
}

/* ---------------- 瓦片刷新 ---------------- */
static void map_refresh(void)
{
    double grid = (double)(1 << map_level);

    /* 车的世界像素坐标（本级别全图像素） */
    double car_px = frac_x(car_lon) * grid * TILE_SIZE;
    double car_py = frac_y(car_lat) * grid * TILE_SIZE;

    /* 地图区左上角的世界像素 */
    double x0 = car_px - MAP_W / 2.0;
    double y0 = car_py - MAP_H / 2.0;

    for (int j = 0; j < MAP_ROWS; j++) {
        for (int i = 0; i < MAP_COLS; i++) {
            int tx = (int)floor(x0 / TILE_SIZE) + i;   /* 瓦片编号 */
            int ty = (int)floor(y0 / TILE_SIZE) + j;
            int px = (int)(tx * TILE_SIZE - x0);       /* 屏幕坐标，可为负 */
            int py = (int)(ty * TILE_SIZE - y0);

            lv_obj_t *img = tile_img[j][i];
            lv_obj_set_pos(img, px, py);

            char path[96];
            snprintf(path, sizeof(path), "A:%s/%d_%d_%d.png",
                     TILE_BASE_DIR, map_level, tx, ty);
            if (tile_loader_ready(path)) {
                /* 缓存命中(含刚下载完的) → 显示 */
                if (strcmp(tile_src[j][i], path) != 0) {
                    lv_img_set_src(img, path);
                    snprintf(tile_src[j][i], sizeof(tile_src[j][i]), "%s", path);
                }
            } else {
                /* 未命中 → 入队后台下载, 清空该格避免残留别的级别画面 */
                tile_loader_request(path);
                if (tile_src[j][i][0]) {
                    lv_img_set_src(img, NULL);
                    tile_src[j][i][0] = '\0';
                }
            }
        }
    }

    lv_label_set_text_fmt(level_label, "级别 %d", map_level);
}

/* ---------------- 缩放 ---------------- */
static void zoom_btn_event_cb(lv_event_t *e)
{
    int delta = (int)(intptr_t)lv_event_get_user_data(e);   /* +1 放大 / -1 缩小 */
    int new_level = map_level + delta;
    if (new_level < LEVEL_MIN || new_level > LEVEL_MAX) return;

    map_level = new_level;
    map_refresh();
}

/* ---------------- 页面构建 ---------------- */
static void topbar_create(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, MAP_W, TOPBAR_H);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x101B2D), 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* 标题 */
    lv_obj_t *title = lv_label_create(bar);
    lv_label_set_text(title, "导航");
    lv_obj_set_style_text_font(title, &lv_font_cn_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 16, 0);

    /* 当前级别 */
    level_label = lv_label_create(bar);
    lv_obj_set_style_text_font(level_label, &lv_font_cn_16, 0);
    lv_obj_set_style_text_color(level_label, lv_color_hex(0x8FA3BD), 0);
    lv_obj_align(level_label, LV_ALIGN_LEFT_MID, 90, 0);

    /* 网络加载状态 */
    status_label = lv_label_create(bar);
    lv_obj_set_style_text_font(status_label, &lv_font_cn_16, 0);
    lv_obj_set_style_text_color(status_label, lv_color_hex(0x8FA3BD), 0);
    lv_obj_align(status_label, LV_ALIGN_LEFT_MID, 170, 0);
    lv_label_set_text(status_label, "在线模式");

    /* 缩放按钮（测试用，后续可改成捏合手势） */
    lv_obj_t *btn_out = lv_btn_create(bar);
    lv_obj_set_size(btn_out, 88, 36);
    lv_obj_align(btn_out, LV_ALIGN_RIGHT_MID, -116, 0);
    lv_obj_set_style_bg_color(btn_out, lv_color_hex(0x1D3A5F), 0);
    lv_obj_add_event_cb(btn_out, zoom_btn_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
    lv_obj_t *out_label = lv_label_create(btn_out);
    lv_label_set_text(out_label, "缩小");
    lv_obj_set_style_text_font(out_label, &lv_font_cn_16, 0);
    lv_obj_center(out_label);

    lv_obj_t *btn_in = lv_btn_create(bar);
    lv_obj_set_size(btn_in, 88, 36);
    lv_obj_align(btn_in, LV_ALIGN_RIGHT_MID, -16, 0);
    lv_obj_set_style_bg_color(btn_in, lv_color_hex(0x1D3A5F), 0);
    lv_obj_add_event_cb(btn_in, zoom_btn_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)+1);
    lv_obj_t *in_label = lv_label_create(btn_in);
    lv_label_set_text(in_label, "放大");
    lv_obj_set_style_text_font(in_label, &lv_font_cn_16, 0);
    lv_obj_center(in_label);
}

static void map_create(lv_obj_t *parent)
{
    /* 取景框: 全屏裁剪容器，不滚动 */
    lv_obj_t *map = lv_obj_create(parent);
    lv_obj_set_size(map, MAP_W, MAP_H);
    lv_obj_set_pos(map, 0, TOPBAR_H);
    lv_obj_set_style_bg_color(map, lv_color_hex(0x0D1621), 0);
    lv_obj_set_style_border_width(map, 0, 0);
    lv_obj_set_style_radius(map, 0, 0);
    lv_obj_set_style_pad_all(map, 0, 0);
    lv_obj_clear_flag(map, LV_OBJ_FLAG_SCROLLABLE);  /* 关键: 不让地图自己滚 */

    /* 瓦片对象池: 4×3 固定创建，位置随车动 */
    for (int j = 0; j < MAP_ROWS; j++) {
        for (int i = 0; i < MAP_COLS; i++) {
            tile_img[j][i] = lv_img_create(map);
            lv_obj_set_pos(tile_img[j][i], 0, 0);
            tile_src[j][i][0] = '\0';
        }
    }

    /* 车标: 固定屏幕中心，放最后保证盖在瓦片上面 */
    lv_obj_t *car = lv_obj_create(map);
    lv_obj_set_size(car, 22, 22);
    lv_obj_align(car, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(car, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(car, lv_color_hex(0x2196F3), 0);
    lv_obj_set_style_border_color(car, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(car, 2, 0);
    lv_obj_clear_flag(car, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(car, LV_OBJ_FLAG_CLICKABLE);
}

/* ---------------- 轮询下载结果 ---------------- */
static void tile_poll_cb(lv_timer_t *t)
{
    (void)t;
    if (tile_loader_pop_new()) map_refresh();   /* 有新瓦片落盘 → 重刷(命中缓存) */
    int n = tile_loader_pending();
    if (n > 0) lv_label_set_text_fmt(status_label, "加载中 %d", n);
    else       lv_label_set_text(status_label, "在线模式");
}

void nav_page_create(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0A1220), 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    topbar_create(scr);
    map_create(scr);
    map_refresh();

    tile_loader_init();                          /* 启动网络下载线程 */
    lv_timer_create(tile_poll_cb, 150, NULL);    /* 轮询下载完成情况 */
}
