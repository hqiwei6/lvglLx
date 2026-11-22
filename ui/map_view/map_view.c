/**
 * @file map_view.c
 * @brief 瓦片拼接地图页 —— 6槽对象池 + 手动自由拖拽(斜向) + 按需调度
 *
 * 架构:
 *   - viewport 不可滚动(LVGL8 原生滚动是轴向锁定的); 拖拽 = PRESSING 事件
 *     手动改锚点(img_ix/img_iy = 瓦片(base_tx,base_ty) 左上角的屏幕像素位置),
 *     支持任意方向; 拖到哪画面停在哪, 不再需要"换图居中"
 *   - 6 个 lv_img 槽位组成对象池。SCALE=6 时单张瓦片(1536px)比屏幕大,
 *     视野最多同时覆盖 2×2=4 张瓦片, 6 槽有富余
 *   - pool_sync(): 计算视野覆盖的瓦片集合 → 逐个分配槽位:
 *       · 命中缓存 → 立即显示(拖动过程中邻格无缝补图)
 *       · 未命中   → 后台下载, 到货后自动补上(黑格悄悄填满)
 *       · 不在视野的槽 → 隐藏回收
 *   - 缩放: 以屏幕中心为锚点换算新级别坐标, 全槽复位重新调度
 *   - 下载线程复用 nav/tile_loader (缓存/去重/重试)
 *
 * 坐标数学 (slippy map):
 *   fx = (lon+180)/360, fy = (1-ln(tan(lat)+1/cos(lat))/PI)/2   ← 级别无关
 *   屏幕点 s 的世界像素 = base*256 + (s - 锚点)/SCALE
 */
#include "map_view.h"
#include "../nav/tile_loader.h"
#include "../nav/nav_page.h"     /* TILE_BASE_DIR */
#include <stdio.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* 中文字库 */
LV_FONT_DECLARE(lv_font_cn_16)
LV_FONT_DECLARE(lv_font_cn_20)

/* ---------------- 常量 ---------------- */
#define VP_W        1024
#define VP_H        (600 - 48)       /* 552 */
#define TILE        256
#define SCALE       2                /* 显示倍率(拼接版无需大倍率) */
#define CELL        (TILE * SCALE)   /* 单张瓦片显示尺寸 512 */
#define POOL_N      10               /* 槽位数(视野跨边界时最多 3×3=9, 留余量) */
#define TOPBAR_H    48
#define LEVEL_MIN   3
#define LEVEL_MAX   18

/* ---------------- 页面数据 ---------------- */
/* 默认位置: 广州从化·粤嵌(城鳌大道东路999号8幢) */
static double car_lon = 113.52782;
static double car_lat = 23.56941;
static int    map_level = 15;

static lv_obj_t *viewport;               /* 取景框(不可滚动, 接收拖拽) */
static lv_obj_t *level_label;
static lv_obj_t *status_label;

static int    base_tx = -1, base_ty = -1;/* 锚点瓦片编号 */
static int    img_ix, img_iy;            /* 锚点屏幕位置(权威状态, 不回读控件) */
static lv_point_t last_pt;               /* 拖拽上一次指针位置 */

typedef struct {
    lv_obj_t *img;                       /* 槽位控件 */
    int tx, ty;                          /* 当前分配的瓦片编号, -1=空闲 */
    char path[96];                       /* 当前已设置的 src(""=未设) */
    bool requested;                      /* 已发请求等待下载 */
} slot_t;
static slot_t slots[POOL_N];

/* ---------------- 墨卡托数学 ---------------- */
static double frac_x(double lon) { return (lon + 180.0) / 360.0; }

static double frac_y(double lat)
{
    double rad = lat * M_PI / 180.0;
    return (1.0 - log(tan(rad) + 1.0 / cos(rad)) / M_PI) / 2.0;
}

/* 参考瓦片自动重定位: 把锚点参考系平移到屏幕中心的瓦片,
 * 使 img_ix/img_iy 恒保持在小区间内(控件坐标是 int16, 防止拖远溢出),
 * 纯坐标平移, 槽位绝对位置不变, 画面零跳动 */
static void rebase_anchor(void)
{
    int cx_tx = (int)floor((base_tx * TILE + (VP_W / 2.0 - img_ix) / SCALE) / TILE);
    int cx_ty = (int)floor((base_ty * TILE + (VP_H / 2.0 - img_iy) / SCALE) / TILE);
    if (cx_tx == base_tx && cx_ty == base_ty) return;

    img_ix += (int)((cx_tx - base_tx) * CELL);
    img_iy += (int)((cx_ty - base_ty) * CELL);
    base_tx = cx_tx;
    base_ty = cx_ty;
}

/* 屏幕中心的世界坐标 → (fx, fy), 需 base 有效 */
static void center_frac(double *fx, double *fy)
{
    double worldx = base_tx * TILE + (VP_W / 2.0 - img_ix) / SCALE;
    double worldy = base_ty * TILE + (VP_H / 2.0 - img_iy) / SCALE;
    double grid = (double)(1 << map_level);
    *fx = worldx / (TILE * grid);
    *fy = worldy / (TILE * grid);
}

/* ---------------- 对象池 ---------------- */
/* 把槽位摆到其瓦片对应的屏幕位置(锚点移动时全体跟随) */
static void slot_place(slot_t *s)
{
    lv_obj_set_pos(s->img,
                   img_ix + (s->tx - base_tx) * CELL,
                   img_iy + (s->ty - base_ty) * CELL);
}

/* 全部槽位复位(缩放换级别时用) */
static void pool_reset(void)
{
    for (int i = 0; i < POOL_N; i++) {
        slots[i].tx = slots[i].ty = -1;
        slots[i].requested = false;
        slots[i].path[0] = '\0';
        lv_obj_add_flag(slots[i].img, LV_OBJ_FLAG_HIDDEN);
    }
}

/* 核心: 按当前视野调度槽位 —— 分配/显示/请求/回收
 * force=true 时对"已请求但迟迟不到"的槽强制重发(失败重试) */
static void pool_sync(bool force)
{
    if (base_tx < 0) return;

    /* 视野四角的世界像素 → 覆盖的瓦片编号范围(每轴跨边界时最多 3 张) */
    double wl = base_tx * TILE + (0.0        - img_ix) / SCALE;
    double wr = base_tx * TILE + (VP_W - img_ix) / SCALE;
    double wt = base_ty * TILE + (0.0        - img_iy) / SCALE;
    double wb = base_ty * TILE + (VP_H - img_iy) / SCALE;
    int tx0 = (int)floor(wl / TILE), tx1 = (int)floor(wr / TILE);
    int ty0 = (int)floor(wt / TILE), ty1 = (int)floor(wb / TILE);

    /* 1. 回收不在视野内的槽 */
    for (int i = 0; i < POOL_N; i++) {
        slot_t *s = &slots[i];
        if (s->tx < 0) continue;
        if (s->tx < tx0 || s->tx > tx1 || s->ty < ty0 || s->ty > ty1) {
            s->tx = s->ty = -1;
            s->requested = false;
            lv_obj_add_flag(s->img, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* 2. 逐个需要的瓦片分配槽位 */
    for (int ty = ty0; ty <= ty1; ty++) {
        for (int tx = tx0; tx <= tx1; tx++) {
            char path[96];
            snprintf(path, sizeof(path), "A:%s/%d_%d_%d.png",
                     TILE_BASE_DIR, map_level, tx, ty);

            /* 先找已显示该瓦片的槽 */
            slot_t *s = NULL;
            for (int i = 0; i < POOL_N; i++)
                if (slots[i].tx == tx && slots[i].ty == ty) { s = &slots[i]; break; }

            /* 否则找空闲槽或不被需要的槽改派 */
            if (!s) {
                for (int i = 0; i < POOL_N; i++) {
                    slot_t *c = &slots[i];
                    bool needed = c->tx >= tx0 && c->tx <= tx1 &&
                                  c->ty >= ty0 && c->ty <= ty1;
                    if (c->tx < 0 || !needed) { s = c; break; }
                }
            }
            if (!s) continue;                /* 槽位耗尽(理论不会发生) */

            if (s->tx != tx || s->ty != ty) {/* 改派: 清状态 */
                s->tx = tx; s->ty = ty;
                s->requested = false;
                s->path[0] = '\0';
                lv_obj_add_flag(s->img, LV_OBJ_FLAG_HIDDEN);
            }
            slot_place(s);

            if (tile_loader_ready(path)) {   /* 缓存命中: 显示 */
                if (strcmp(s->path, path) != 0) {
                    lv_img_set_src(s->img, path);
                    snprintf(s->path, sizeof(s->path), "%s", path);
                    fprintf(stderr, "[pool] 显示 %d_%d\n", tx, ty);
                }
                lv_obj_clear_flag(s->img, LV_OBJ_FLAG_HIDDEN);
                s->requested = false;
            } else if (!s->requested || force) {  /* 未命中: 隐藏+请求(force=失败重试) */
                lv_obj_add_flag(s->img, LV_OBJ_FLAG_HIDDEN);
                tile_loader_request(path);
                s->requested = true;
                fprintf(stderr, "[pool] 请求 %d_%d\n", tx, ty);
            }
        }
    }
}

/* 以 (fx,fy) 为屏幕中心设定锚点并重新调度(初始定位/缩放用) */
static void apply_view(double fx, double fy)
{
    double grid = (double)(1 << map_level);
    double worldx = fx * TILE * grid;
    double worldy = fy * TILE * grid;
    base_tx = (int)floor(worldx / TILE);
    base_ty = (int)floor(worldy / TILE);
    /* 让 (fx,fy) 落在屏幕中心 */
    img_ix = VP_W / 2 - (int)((worldx - base_tx * TILE) * SCALE);
    img_iy = VP_H / 2 - (int)((worldy - base_ty * TILE) * SCALE);
    pool_reset();
    pool_sync(false);
    lv_label_set_text_fmt(level_label, "级别 %d", map_level);
}

/* ---------------- 拖拽事件 ---------------- */
static void press_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_get_point(lv_indev_get_act(), &last_pt);
}

static void pressing_cb(lv_event_t *e)
{
    (void)e;
    if (base_tx < 0) return;               /* 没图时不拖 */

    lv_point_t p;
    lv_indev_get_point(lv_indev_get_act(), &p);

    /* 锚点跟随手指, 不设边界(拼接版视野覆盖由数学保证)
     * 拖远后 rebase_anchor 自动换参考瓦片, 坐标恒不溢出 */
    img_ix += (p.x - last_pt.x);
    img_iy += (p.y - last_pt.y);
    rebase_anchor();
    pool_sync(false);                      /* 槽位全体跟随 + 邻格即时补图 */

    last_pt = p;
}

static void release_cb(lv_event_t *e)
{
    (void)e;
    if (base_tx < 0) return;
    pool_sync(false);                      /* 松手: 请求视野内缺的瓦片 */
}

static void zoom_btn_event_cb(lv_event_t *e)
{
    int delta = (int)(intptr_t)lv_event_get_user_data(e);
    int nl = map_level + delta;
    if (nl < LEVEL_MIN || nl > LEVEL_MAX) return;

    double fx, fy;
    if (base_tx >= 0) center_frac(&fx, &fy);
    else { fx = frac_x(car_lon); fy = frac_y(car_lat); }

    map_level = nl;
    apply_view(fx, fy);                    /* 以屏幕中心为锚点缩放 */
}

/* 轮询: 状态栏 / 下载到货补图 / 失败约2s强制重试 */
static void poll_cb(lv_timer_t *t)
{
    (void)t;
    int n = tile_loader_pending();
    if (n > 0) lv_label_set_text_fmt(status_label, "加载中 %d", n);
    else       lv_label_set_text(status_label, "在线模式");

    /* 每次轮询都 sync: 到货立即上屏; 队列空但有缺图 → 约2s后强制重试 */
    static int retry_tick = 0;
    bool force = false;
    if (n > 0) {
        retry_tick = 0;
    } else if (++retry_tick >= 13) {
        force = true;
        retry_tick = 0;
    }
    pool_sync(force);
}

/* ---------------- 页面构建 ---------------- */
static void topbar_create(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, VP_W, TOPBAR_H);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x101B2D), 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(bar);
    lv_label_set_text(title, "导航");
    lv_obj_set_style_text_font(title, &lv_font_cn_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 16, 0);

    level_label = lv_label_create(bar);
    lv_obj_set_style_text_font(level_label, &lv_font_cn_16, 0);
    lv_obj_set_style_text_color(level_label, lv_color_hex(0x8FA3BD), 0);
    lv_obj_align(level_label, LV_ALIGN_LEFT_MID, 90, 0);

    status_label = lv_label_create(bar);
    lv_obj_set_style_text_font(status_label, &lv_font_cn_16, 0);
    lv_obj_set_style_text_color(status_label, lv_color_hex(0x8FA3BD), 0);
    lv_obj_align(status_label, LV_ALIGN_LEFT_MID, 170, 0);
    lv_label_set_text(status_label, "在线模式");

    lv_obj_t *btn_out = lv_btn_create(bar);
    lv_obj_set_size(btn_out, 88, 36);
    lv_obj_align(btn_out, LV_ALIGN_RIGHT_MID, -116, 0);
    lv_obj_set_style_bg_color(btn_out, lv_color_hex(0x1D3A5F), 0);
    lv_obj_add_event_cb(btn_out, zoom_btn_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
    lv_obj_t *lo = lv_label_create(btn_out);
    lv_label_set_text(lo, "缩小");
    lv_obj_set_style_text_font(lo, &lv_font_cn_16, 0);
    lv_obj_center(lo);

    lv_obj_t *btn_in = lv_btn_create(bar);
    lv_obj_set_size(btn_in, 88, 36);
    lv_obj_align(btn_in, LV_ALIGN_RIGHT_MID, -16, 0);
    lv_obj_set_style_bg_color(btn_in, lv_color_hex(0x1D3A5F), 0);
    lv_obj_add_event_cb(btn_in, zoom_btn_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)+1);
    lv_obj_t *li = lv_label_create(btn_in);
    lv_label_set_text(li, "放大");
    lv_obj_set_style_text_font(li, &lv_font_cn_16, 0);
    lv_obj_center(li);
}

void map_view_create(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0A1220), 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    topbar_create(scr);

    /* 取景框: 不可滚动(原生滚动是轴向锁定的), 拖拽全靠手动事件 */
    viewport = lv_obj_create(scr);
    lv_obj_set_size(viewport, VP_W, VP_H);
    lv_obj_set_pos(viewport, 0, TOPBAR_H);
    lv_obj_set_style_bg_color(viewport, lv_color_hex(0x0D1621), 0);
    lv_obj_set_style_border_width(viewport, 0, 0);
    lv_obj_set_style_radius(viewport, 0, 0);
    lv_obj_set_style_pad_all(viewport, 0, 0);
    lv_obj_set_scrollbar_mode(viewport, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(viewport, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(viewport, pressing_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(viewport, press_cb,    LV_EVENT_PRESSED,  NULL);
    lv_obj_add_event_cb(viewport, release_cb,  LV_EVENT_RELEASED, NULL);

    /* 6 个槽位图片 */
    for (int i = 0; i < POOL_N; i++) {
        slots[i].img = lv_img_create(viewport);
        lv_img_set_size_mode(slots[i].img, LV_IMG_SIZE_MODE_REAL);
        lv_img_set_pivot(slots[i].img, 0, 0);
        lv_img_set_zoom(slots[i].img, LV_IMG_ZOOM_NONE * SCALE);
        lv_img_set_antialias(slots[i].img, true);
        lv_obj_clear_flag(slots[i].img, LV_OBJ_FLAG_CLICKABLE);
        slots[i].tx = slots[i].ty = -1;
        slots[i].path[0] = '\0';
        slots[i].requested = false;
        lv_obj_add_flag(slots[i].img, LV_OBJ_FLAG_HIDDEN);
    }

    /* 初始定位 */
    apply_view(frac_x(car_lon), frac_y(car_lat));

    tile_loader_init();
    lv_timer_create(poll_cb, 150, NULL);
}
