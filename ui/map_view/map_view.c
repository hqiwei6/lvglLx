/**
 * @file map_view.c
 * @brief 瓦片拼接地图页 —— 槽位对象池 + 手动自由拖拽(斜向) + 按需调度
 *
 * 架构:
 *   - viewport 不可滚动(LVGL8 原生滚动是轴向锁定的); 拖拽 = PRESSING 事件
 *     手动改锚点(img_ix/img_iy = 瓦片(base_tx,base_ty) 左上角的屏幕像素位置),
 *     支持任意方向; 拖到哪画面停在哪, 不再需要"换图居中"
 *   - 槽位对象池: 瓦片按 256px 原生尺寸平铺, 视野 1024×552 最多同时
 *     覆盖 5×3=15 张瓦片, 16 槽有富余
 *   - pool_sync(): 计算视野覆盖的瓦片集合 → 逐个分配槽位:
 *       · 命中缓存 → 立即显示(拖动过程中邻格无缝补图)
 *       · 未命中   → 后台下载, 到货后自动补上(黑格悄悄填满)
 *       · 不在视野的槽 → 隐藏回收
 *   - 缩放: 以屏幕中心为锚点换算新级别坐标, 全槽复位重新调度
 *   - 下载线程复用 nav/tile_loader (缓存/去重/重试)
 *
 * 坐标数学 (slippy map):
 *   fx = (lon+180)/360, fy = (1-ln(tan(lat)+1/cos(lat))/PI)/2   ← 级别无关
 *   屏幕点 s 的世界像素 = base*256 + (s - 锚点)
 */
#include "map_view.h"
#include "../nav/tile_loader.h"
#include "../nav/nav_page.h"     /* TILE_BASE_DIR */
#include "amap.h"
#include "lvgl/src/extra/others/ime/lv_ime_pinyin.h"   /* 拼音输入法(8.3 组件移植) */
extern const lv_pinyin_dict_t pinyin_dict_zh[];        /* 简体词典(gen_pinyin_dict.py 生成) */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>

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
#define POOL_N      16               /* 槽位数(视野跨边界时最多 5×3=15, 留余量) */
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

/* 覆盖物 */
static lv_obj_t *car_dot;                /* 当前位置点(蓝) */
static lv_obj_t *dest_dot;               /* 目的地标记(红) */
static lv_obj_t *route_line;             /* 路线(lv_line) */
static lv_obj_t *search_ta;              /* 搜索输入框 */
static lv_obj_t *dest_label;             /* 目的地/状态条 */

/* 目的地与路线(UI 持有) */
static bool     dest_ok;
static double   dest_lon, dest_lat;
static char     dest_name[64];
static double  *route_ll = NULL;         /* WGS84 点串(lon,lat 交替) */
static int      route_n = 0;

static void cand_show(const amap_result_t *r);   /* 前向声明(定义在下方) */
static void search_trigger(void);
static lv_point_t *line_pts = NULL;      /* 屏幕点缓存(lv_line 引用) */
static int      line_pts_cap = 0;

static int    base_tx = -1, base_ty = -1;/* 锚点瓦片编号 */
static int    img_ix, img_iy;            /* 锚点屏幕位置(权威状态, 不回读控件) */
static lv_point_t last_pt;               /* 拖拽上一次指针位置 */

typedef struct {
    lv_obj_t *img;                       /* 底图(vec_w)控件 */
    lv_obj_t *img_cva;                   /* 注记(cva_w)控件, 透明PNG叠在底图上 */
    int tx, ty;                          /* 当前分配的瓦片编号, -1=空闲 */
    char path[96];                       /* 底图已设的 src(""=未设) */
    char path_cva[96];                   /* 注记已设的 src(""=未设) */
    bool requested;                      /* 底图已发请求等待下载 */
    bool requested_cva;                  /* 注记已发请求等待下载 */
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
    int cx_tx = base_tx + (VP_W / 2 - img_ix) / TILE;
    int cx_ty = base_ty + (VP_H / 2 - img_iy) / TILE;
    if (cx_tx == base_tx && cx_ty == base_ty) return;

    img_ix += (cx_tx - base_tx) * TILE;
    img_iy += (cx_ty - base_ty) * TILE;
    base_tx = cx_tx;
    base_ty = cx_ty;
}

/* 屏幕中心的世界坐标 → (fx, fy), 需 base 有效 */
static void center_frac(double *fx, double *fy)
{
    double worldx = base_tx * TILE + (VP_W / 2 - img_ix);
    double worldy = base_ty * TILE + (VP_H / 2 - img_iy);
    double grid = (double)(1 << map_level);
    *fx = worldx / (TILE * grid);
    *fy = worldy / (TILE * grid);
}

/* ---------------- 覆盖物(当前位置点/目的地/路线) ---------------- */
/* WGS84 经纬度 → 屏幕坐标: 世界像素 = frac * 256 * 2^level,
 * 屏幕 = 世界像素 - 锚点瓦片左上角世界像素 + 锚点屏幕位置 */
static void lonlat_to_screen(double lon, double lat, int *sx, int *sy)
{
    double grid = (double)(1 << map_level);
    double wx = frac_x(lon) * TILE * grid;
    double wy = frac_y(lat) * TILE * grid;
    *sx = (int)(wx - base_tx * TILE + img_ix);
    *sy = (int)(wy - base_ty * TILE + img_iy);
}

/* 把点标记摆到其经纬度对应的屏幕位置(拖拽/缩放后跟随) */
static void dot_place(lv_obj_t *dot, double lon, double lat)
{
    if (base_tx < 0 || !dot) return;
    int sx, sy;
    lonlat_to_screen(lon, lat, &sx, &sy);
    lv_obj_set_pos(dot, sx - 8, sy - 8);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_HIDDEN);
}

/* 路线点串 → 屏幕点 → lv_line。
 * 两个必要处理:
 *  - 裁剪: 屏外远处点丢弃(带 4096px 缓冲带), 否则跨省路线的屏幕坐标
 *    上千万像素, 塞进 lv_point_t 的 int16 回绕成屏内随机位置 → 乱线
 *  - 抽稀: 相邻保留点屏幕距离 <2px 的丢弃, 17209 点的全量重绘拖垮帧率 */
static void route_redraw(void)
{
    if (route_n < 2 || base_tx < 0 || !route_ll) {
        lv_obj_add_flag(route_line, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (line_pts_cap < route_n) {
        free(line_pts);
        line_pts = malloc(sizeof(lv_point_t) * (size_t)route_n);
        line_pts_cap = line_pts ? route_n : 0;
        if (!line_pts) return;
    }
    int kept = 0, lastx = 0, lasty = 0;
    for (int i = 0; i < route_n; i++) {
        int sx, sy;
        lonlat_to_screen(route_ll[2 * i], route_ll[2 * i + 1], &sx, &sy);
        if (sx < -4096 || sx > VP_W + 4096 ||
            sy < -4096 || sy > VP_H + 4096)
            continue;                            /* 缓冲带外: 丢 */
        if (kept && sx - lastx < 2 && sx - lastx > -2 &&
                    sy - lasty < 2 && sy - lasty > -2)
            continue;                            /* 紧邻: 抽稀 */
        /* lonlat_to_screen 输出 int(4B), lv_point_t 字段是 int16(2B),
         * 直接取 &line_pts[i].x 会每点溢出 4 字节踩坏堆 → 经 int 中转 */
        line_pts[kept].x = sx;
        line_pts[kept].y = sy;
        lastx = sx; lasty = sy;
        kept++;
    }
    if (kept < 2) {
        lv_obj_add_flag(route_line, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_line_set_points(route_line, line_pts, kept);
    lv_obj_invalidate(route_line);               /* 点数组原地更新, 强制重绘 */
    lv_obj_clear_flag(route_line, LV_OBJ_FLAG_HIDDEN);
}

/* ---------------- 对象池 ---------------- */
/* 把槽位摆到其瓦片对应的屏幕位置(锚点移动时全体跟随) */
static void slot_place(slot_t *s)
{
    int x = img_ix + (s->tx - base_tx) * TILE;
    int y = img_iy + (s->ty - base_ty) * TILE;
    lv_obj_set_pos(s->img, x, y);
    lv_obj_set_pos(s->img_cva, x, y);
}

/* 全部槽位复位(缩放换级别时用) */
static void pool_reset(void)
{
    for (int i = 0; i < POOL_N; i++) {
        slots[i].tx = slots[i].ty = -1;
        slots[i].requested = slots[i].requested_cva = false;
        slots[i].path[0] = slots[i].path_cva[0] = '\0';
        lv_obj_add_flag(slots[i].img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(slots[i].img_cva, LV_OBJ_FLAG_HIDDEN);
    }
}

/* 核心: 按当前视野调度槽位 —— 分配/显示/请求/回收
 * force=true 时对"已请求但迟迟不到"的槽强制重发(失败重试) */
static void pool_sync(bool force)
{
    if (base_tx < 0) return;

    /* 视野四角的世界像素 → 覆盖的瓦片编号范围(floor 防负方向漏格, 最多 5×3=15 张) */
    double wl = base_tx * TILE - img_ix;
    double wr = base_tx * TILE + VP_W - img_ix;
    double wt = base_ty * TILE - img_iy;
    double wb = base_ty * TILE + VP_H - img_iy;
    int tx0 = (int)floor(wl / TILE), tx1 = (int)floor(wr / TILE);
    int ty0 = (int)floor(wt / TILE), ty1 = (int)floor(wb / TILE);

    /* 1. 回收不在视野内的槽 */
    for (int i = 0; i < POOL_N; i++) {
        slot_t *s = &slots[i];
        if (s->tx < 0) continue;
        if (s->tx < tx0 || s->tx > tx1 || s->ty < ty0 || s->ty > ty1) {
            s->tx = s->ty = -1;
            s->requested = s->requested_cva = false;
            lv_obj_add_flag(s->img, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s->img_cva, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* 2. 逐个需要的瓦片分配槽位 */
    for (int ty = ty0; ty <= ty1; ty++) {
        for (int tx = tx0; tx <= tx1; tx++) {
            char path[96], path_cva[96];
            snprintf(path, sizeof(path), "A:%s/vec_%d_%d_%d.png",
                     TILE_BASE_DIR, map_level, tx, ty);
            snprintf(path_cva, sizeof(path_cva), "A:%s/cva_%d_%d_%d.png",
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
                s->requested = s->requested_cva = false;
                s->path[0] = s->path_cva[0] = '\0';
                lv_obj_add_flag(s->img, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s->img_cva, LV_OBJ_FLAG_HIDDEN);
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

            /* 注记层(cva_w): 透明PNG, 独立调度, 到货后叠在底图上 */
            if (tile_loader_ready(path_cva)) {
                if (strcmp(s->path_cva, path_cva) != 0) {
                    lv_img_set_src(s->img_cva, path_cva);
                    snprintf(s->path_cva, sizeof(s->path_cva), "%s", path_cva);
                    fprintf(stderr, "[pool] 注记 %d_%d\n", tx, ty);
                }
                lv_obj_clear_flag(s->img_cva, LV_OBJ_FLAG_HIDDEN);
                s->requested_cva = false;
            } else if (!s->requested_cva || force) {
                lv_obj_add_flag(s->img_cva, LV_OBJ_FLAG_HIDDEN);
                tile_loader_request(path_cva);
                s->requested_cva = true;
            }
        }
    }

    /* 覆盖物跟随地图 */
    dot_place(car_dot, car_lon, car_lat);
    if (dest_ok) {
        dot_place(dest_dot, dest_lon, dest_lat);
        route_redraw();
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
    img_ix = VP_W / 2 - (int)(worldx - base_tx * TILE);
    img_iy = VP_H / 2 - (int)(worldy - base_ty * TILE);
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

/* 轮询: 状态栏 / 下载到货补图 / 失败约2s强制重试 / 高德任务结果 */
static void poll_cb(lv_timer_t *t)
{
    (void)t;
    int n = tile_loader_pending();
    if (n > 0) lv_label_set_text_fmt(status_label, "加载中 %d", n);
    else       lv_label_set_text(status_label, "在线模式");

    /* 高德结果: 搜索到 → 摆标记并自动规划路线; 路线到 → 画线 */
    amap_result_t r;
    if (amap_take_result(&r)) {
        if (r.type == 1) {
            if (r.err[0]) {
                lv_label_set_text_fmt(dest_label, "搜索失败: %s", r.err);
            } else if (r.ncand > 1) {
                cand_show(&r);               /* 同名异地 → 让用户确认 */
            } else {
                dest_ok = true;
                dest_lon = r.dst_lon;
                dest_lat = r.dst_lat;
                snprintf(dest_name, sizeof(dest_name), "%s", r.name);
                dot_place(dest_dot, dest_lon, dest_lat);
                lv_label_set_text_fmt(dest_label, "目的地: %s, 规划路线中...", r.name);
                amap_route(car_lon, car_lat, dest_lon, dest_lat);
            }
        } else if (r.type == 2) {
            lv_obj_clear_flag(lv_obj_get_parent(dest_label), LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(dest_label, LV_OBJ_FLAG_HIDDEN);
            if (r.err[0]) {
                lv_label_set_text_fmt(dest_label, "路线失败: %s", r.err);
            } else {
                free(route_ll);
                route_ll = r.pts;                /* 所有权移交, UI 持有 */
                route_n  = r.npts;
                route_redraw();
                char dstr[24], tstr[24];
                if (r.distance_m >= 1000)
                    snprintf(dstr, sizeof(dstr), "%d公里", r.distance_m / 1000);
                else
                    snprintf(dstr, sizeof(dstr), "%d米", r.distance_m);
                if (r.duration_s >= 3600)
                    snprintf(tstr, sizeof(tstr), "%d小时%d分",
                             r.duration_s / 3600, (r.duration_s % 3600) / 60);
                else
                    snprintf(tstr, sizeof(tstr), "%d分钟", (r.duration_s + 59) / 60);
                lv_label_set_text_fmt(dest_label, "目的地: %s, %s, 预计%s",
                                      dest_name, dstr, tstr);
            }
        }
    }

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

/* ---------------- 搜索交互 ---------------- */
static lv_obj_t *pinyin_kb;
static lv_obj_t *pinyin_ime;

/* 板上中文输入: lv_keyboard + lv_ime_pinyin(纯软件拼音, 不依赖系统输入法框架) */
static void kb_toggle(bool show)
{
    if (!pinyin_kb) {
        lv_obj_t *parent = lv_scr_act();   /* 挂屏幕层: 尺寸按全屏算, 不受搜索浮层容器裁剪 */
        pinyin_kb = lv_keyboard_create(parent);
        lv_obj_set_size(pinyin_kb, LV_PCT(100), LV_PCT(45));
        lv_keyboard_set_textarea(pinyin_kb, search_ta);
        lv_obj_add_flag(pinyin_kb, LV_OBJ_FLAG_HIDDEN);  /* 键帽符号用默认字体 */

        pinyin_ime = lv_ime_pinyin_create(parent);
        lv_ime_pinyin_set_keyboard(pinyin_ime, pinyin_kb);
        lv_ime_pinyin_set_dict(pinyin_ime, pinyin_dict_zh);
        lv_obj_set_style_text_font(pinyin_ime, &lv_font_cn_16, 0);
        lv_obj_add_flag(pinyin_ime, LV_OBJ_FLAG_HIDDEN);
    }
    if (show) {
        lv_obj_clear_flag(pinyin_ime, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(pinyin_kb, LV_OBJ_FLAG_HIDDEN);
        /* 8.2 的 lv_group_focus_obj 对已聚焦对象仍会重发 FOCUSED(lv_group.c L252),
         * 直接调会经 ta_focused_cb 无限递归爆栈; 故只在焦点漂走时才拉回 */
        lv_group_t *g = lv_group_get_default();
        if (g && lv_group_get_focused(g) != search_ta)
            lv_group_focus_obj(search_ta);
    } else {
        lv_obj_add_flag(pinyin_ime, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(pinyin_kb, LV_OBJ_FLAG_HIDDEN);
        lv_ime_pinyin_set_cand_panel_hidden(pinyin_ime, true);  /* 候选条挂 scr_act, 不随 ime 隐藏 */
    }
}

static void ta_focused_cb(lv_event_t *e) { (void)e; kb_toggle(true);  }
static void kb_hide_cb(lv_event_t  *e) { (void)e; kb_toggle(false); }

/* ---------------- 目的地候选确认 ----------------
 * 同名异地常见("橘海"能搜出天津和广州两家), 搜索返回多候选时
 * 在搜索框正下方展开下拉面板让用户确认, 点选后才规划路线;
 * 唯一命中则直通 */
static lv_obj_t    *cand_layer;                  /* 下拉候选面板 */
static amap_result_t cand_res;                    /* 面板显示期间的候选拷贝 */

static void cand_hide(void)
{
    if (cand_layer) lv_obj_add_flag(cand_layer, LV_OBJ_FLAG_HIDDEN);
}

static void cand_cancel_cb(lv_event_t *e) { (void)e; cand_hide(); }

static void cand_pick(int i)
{
    dest_ok  = true;
    dest_lon = cand_res.cand_lon[i];
    dest_lat = cand_res.cand_lat[i];
    snprintf(dest_name, sizeof(dest_name), "%s", cand_res.cand_name[i]);
    dot_place(dest_dot, dest_lon, dest_lat);
    cand_hide();
    lv_label_set_text_fmt(dest_label, "目的地: %s, 规划路线中...", dest_name);
    lv_obj_clear_flag(lv_obj_get_parent(dest_label), LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(dest_label, LV_OBJ_FLAG_HIDDEN);
    amap_route(car_lon, car_lat, dest_lon, dest_lat);
}

static void cand_pick_cb(lv_event_t *e)
{
    cand_pick((int)(intptr_t)lv_event_get_user_data(e));
}

static void cand_show(const amap_result_t *r)
{
    cand_res = *r;
    kb_toggle(false);                            /* 键盘与候选面板互斥 */

    if (!cand_layer) {
        cand_layer = lv_obj_create(viewport);
        lv_obj_set_style_bg_color(cand_layer, lv_color_hex(0x101B2D), 0);
        lv_obj_set_style_bg_opa(cand_layer, LV_OPA_90, 0);
        lv_obj_set_style_radius(cand_layer, 8, 0);
        lv_obj_set_style_border_width(cand_layer, 0, 0);
        lv_obj_set_style_pad_all(cand_layer, 6, 0);
        lv_obj_set_style_pad_gap(cand_layer, 4, 0);
        lv_obj_clear_flag(cand_layer, LV_OBJ_FLAG_SCROLLABLE);
    }
    while (lv_obj_get_child_cnt(cand_layer))     /* 重建行 */
        lv_obj_del(lv_obj_get_child(cand_layer, 0));

    /* 搜索框正下方: 状态条(sp2)之下, 与搜索浮层同宽 */
    lv_obj_set_size(cand_layer, 340,
                    30 + cand_res.ncand * 40 + 44);
    lv_obj_set_pos(cand_layer, 8, 102);

    lv_obj_t *title = lv_label_create(cand_layer);
    lv_label_set_text(title, "请选择目的地");
    lv_obj_set_style_text_font(title, &lv_font_cn_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x9AB0C0), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 2, 2);

    for (int i = 0; i < cand_res.ncand; i++) {
        lv_obj_t *btn = lv_btn_create(cand_layer);
        lv_obj_set_size(btn, 324, 36);
        lv_obj_set_pos(btn, 2, 26 + i * 40);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x20303E), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2E5E8E),
                                  LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_radius(btn, 6, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_add_event_cb(btn, cand_pick_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);

        lv_obj_t *lb = lv_label_create(btn);
        lv_obj_set_style_text_font(lb, &lv_font_cn_16, 0);
        lv_obj_set_style_text_color(lb, lv_color_white(), 0);
        lv_label_set_long_mode(lb, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lb, 190);
        lv_label_set_text_fmt(lb, "%d. %s", i + 1, cand_res.cand_name[i]);
        lv_obj_align(lb, LV_ALIGN_LEFT_MID, 10, 0);

        lv_obj_t *dist = lv_label_create(btn);
        lv_obj_set_style_text_font(dist, &lv_font_cn_16, 0);
        lv_obj_set_style_text_color(dist, lv_color_hex(0x9AB0C0), 0);
        /* lv_label_set_text_fmt 用 LVGL 自带 printf, %f 默认不支持 → 用整数格式 */
        if (cand_res.cand_dist[i] >= 1000)
            lv_label_set_text_fmt(dist, "%d 公里",
                                  cand_res.cand_dist[i] / 1000);
        else
            lv_label_set_text_fmt(dist, "%d 米", cand_res.cand_dist[i]);
        lv_obj_align(dist, LV_ALIGN_RIGHT_MID, -10, 0);
    }

    lv_obj_t *cancel = lv_btn_create(cand_layer);
    lv_obj_set_size(cancel, 324, 36);
    lv_obj_set_pos(cancel, 2, 26 + cand_res.ncand * 40 + 2);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(0x3A3F45), 0);
    lv_obj_set_style_radius(cancel, 6, 0);
    lv_obj_set_style_border_width(cancel, 0, 0);
    lv_obj_add_event_cb(cancel, cand_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(cancel);
    lv_obj_set_style_text_font(cl, &lv_font_cn_16, 0);
    lv_obj_set_style_text_color(cl, lv_color_white(), 0);
    lv_label_set_text(cl, "取消");
    lv_obj_center(cl);

    lv_obj_clear_flag(cand_layer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_to_index(cand_layer, -1);        /* 提到最上层 */
}

#if USE_SDL
/* ---------- 模拟器自动自测(DEMO_SELFTEST=1 启用, 16s 后自动退出) ----------
 * 序列: 初始截图 → 弹键盘 → 逐键注入 g/u/a/n/g(键盘按钮链) → 截图(应出候选)
 *      → 注入"北京"(TEXTINPUT 链, 模拟 PC 输入法) → 截图(输入框应含汉字) */
extern void sdl_frame_dump(const char *path);
extern void sdl_inject_text(const char *utf8);

static void kb_press_char(const char *key)
{
    /* 8.2 无 lv_btnmatrix_get_btn_cnt, get_btn_text 越界安全返回 NULL */
    for (uint16_t i = 0; ; i++) {
        const char *txt = lv_btnmatrix_get_btn_text(pinyin_kb, i);
        if (!txt) return;
        if (strcmp(txt, key) == 0) {
            lv_btnmatrix_set_selected_btn(pinyin_kb, i);
            lv_event_send(pinyin_kb, LV_EVENT_VALUE_CHANGED, NULL);
            return;
        }
    }
}

static void selftest_cb(lv_timer_t *t)
{
    static int s = 0;
    s++;
    printf("[st] step %d\n", s);
    switch (s) {
    case 2:  sdl_frame_dump("/tmp/st_s0.png"); break;
    case 3:  kb_toggle(true);
             printf("[selftest] search_ta=%p kb=%p\n", (void *)search_ta, (void *)pinyin_kb);
             break;
    case 5:  sdl_frame_dump("/tmp/st_s1.png"); break;
    case 6:  kb_press_char("j"); break;
    case 7:  kb_press_char("i"); break;
    case 8:  kb_press_char("n"); break;
    case 9:  kb_press_char("g"); break;
    case 10: break;  /* jing 音节候选几十字, 足够翻页验证 */
    case 12: sdl_frame_dump("/tmp/st_s2.png"); break;
    case 13: {   /* 点候选条"下一页"(id=CAND_NUM+1): 验证翻页 */
        lv_obj_t *cp = NULL;
        uint32_t n = lv_obj_get_child_cnt(lv_scr_act());
        for (uint32_t i = 0; i < n; i++) {
            lv_obj_t *ch = lv_obj_get_child(lv_scr_act(), i);
            if (lv_obj_check_type(ch, &lv_btnmatrix_class)) cp = ch;  /* 最后一个=候选条 */
        }
        if (cp) {
            lv_btnmatrix_set_selected_btn(cp, LV_IME_PINYIN_CAND_TEXT_NUM + 1);
            lv_event_send(cp, LV_EVENT_VALUE_CHANGED, NULL);
        }
        break;
    }
    case 14: sdl_frame_dump("/tmp/st_s4.png"); break;
    case 15: sdl_inject_text("北京"); break;
    case 17: sdl_frame_dump("/tmp/st_s3.png"); break;
    case 18: search_trigger(); break;            /* 真实网络搜索: 应弹多候选 */
    case 21: sdl_frame_dump("/tmp/st_s5.png"); break;
    case 22: cand_pick(0); break;                /* 选第一个候选 */
    case 25: sdl_frame_dump("/tmp/st_s6.png"); break;
    case 28: sdl_frame_dump("/tmp/st_s7.png"); break;
    case 29: exit(0); break;
    }
    (void)t;
}
#endif

static void search_trigger(void)
{
    const char *kw = lv_textarea_get_text(search_ta);
    if (!kw || !kw[0]) return;
    kb_toggle(false);                          /* 触发搜索即收起键盘 */
    lv_obj_clear_flag(dest_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lv_obj_get_parent(dest_label), LV_OBJ_FLAG_HIDDEN);
    if (!amap_search(kw, car_lon, car_lat))
        lv_label_set_text(dest_label, "请等当前任务完成");
    else
        lv_label_set_text(dest_label, "搜索中...");
}

static void search_btn_cb(lv_event_t *e)  { (void)e; search_trigger(); }
static void search_ready_cb(lv_event_t *e){ (void)e; search_trigger(); }

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
    lv_obj_add_event_cb(viewport, kb_hide_cb,  LV_EVENT_PRESSED,  NULL);   /* 点地图收起键盘 */
    lv_obj_add_event_cb(viewport, release_cb,  LV_EVENT_RELEASED, NULL);

    /* 槽位图片: 底图在前, 注记在后(后创建的兄弟控件渲染在上层) */
    for (int i = 0; i < POOL_N; i++) {
        slots[i].img = lv_img_create(viewport);
        lv_img_set_size_mode(slots[i].img, LV_IMG_SIZE_MODE_REAL);
        lv_img_set_pivot(slots[i].img, 0, 0);
        lv_obj_clear_flag(slots[i].img, LV_OBJ_FLAG_CLICKABLE);

        slots[i].img_cva = lv_img_create(viewport);
        lv_img_set_size_mode(slots[i].img_cva, LV_IMG_SIZE_MODE_REAL);
        lv_img_set_pivot(slots[i].img_cva, 0, 0);
        lv_obj_clear_flag(slots[i].img_cva, LV_OBJ_FLAG_CLICKABLE);

        slots[i].tx = slots[i].ty = -1;
        slots[i].path[0] = slots[i].path_cva[0] = '\0';
        slots[i].requested = slots[i].requested_cva = false;
        lv_obj_add_flag(slots[i].img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(slots[i].img_cva, LV_OBJ_FLAG_HIDDEN);
    }

    /* 路线线(在地图上层) */
    route_line = lv_line_create(viewport);
    lv_obj_set_style_line_color(route_line, lv_color_hex(0xE5484D), 0);
    lv_obj_set_style_line_width(route_line, 4, 0);
    lv_obj_set_style_line_rounded(route_line, true, 0);
    lv_obj_add_flag(route_line, LV_OBJ_FLAG_HIDDEN);

    /* 当前位置点(蓝, 白描边) */
    car_dot = lv_obj_create(viewport);
    lv_obj_set_size(car_dot, 16, 16);
    lv_obj_set_style_radius(car_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(car_dot, lv_color_hex(0x2F81F7), 0);
    lv_obj_set_style_bg_opa(car_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(car_dot, 2, 0);
    lv_obj_set_style_border_color(car_dot, lv_color_white(), 0);
    lv_obj_set_style_pad_all(car_dot, 0, 0);
    lv_obj_clear_flag(car_dot, LV_OBJ_FLAG_CLICKABLE);

    /* 目的地标记(红) */
    dest_dot = lv_obj_create(viewport);
    lv_obj_set_size(dest_dot, 16, 16);
    lv_obj_set_style_radius(dest_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dest_dot, lv_color_hex(0xE5484D), 0);
    lv_obj_set_style_bg_opa(dest_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dest_dot, 2, 0);
    lv_obj_set_style_border_color(dest_dot, lv_color_white(), 0);
    lv_obj_set_style_pad_all(dest_dot, 0, 0);
    lv_obj_clear_flag(dest_dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(dest_dot, LV_OBJ_FLAG_HIDDEN);

    /* 搜索浮层: 地图左上角 */
    lv_obj_t *sp = lv_obj_create(viewport);
    lv_obj_set_size(sp, 320, 46);
    lv_obj_set_pos(sp, 8, 8);
    lv_obj_set_style_bg_color(sp, lv_color_hex(0x101B2D), 0);
    lv_obj_set_style_bg_opa(sp, LV_OPA_80, 0);
    lv_obj_set_style_radius(sp, 10, 0);
    lv_obj_set_style_border_width(sp, 0, 0);
    lv_obj_set_style_pad_all(sp, 6, 0);
    lv_obj_clear_flag(sp, LV_OBJ_FLAG_SCROLLABLE);

    search_ta = lv_textarea_create(sp);
    lv_obj_set_size(search_ta, 196, 34);
    lv_obj_align(search_ta, LV_ALIGN_LEFT_MID, 0, 0);
    lv_textarea_set_one_line(search_ta, true);
    lv_textarea_set_placeholder_text(search_ta, "搜索目的地");
    lv_textarea_set_cursor_click_pos(search_ta, true);
    lv_obj_set_style_text_font(search_ta, &lv_font_cn_16, 0);
    if (lv_group_get_default())
        lv_group_add_obj(lv_group_get_default(), search_ta);
    lv_obj_add_event_cb(search_ta, search_ready_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(search_ta, ta_focused_cb, LV_EVENT_FOCUSED, NULL);

    lv_obj_t *sbtn = lv_btn_create(sp);
    lv_obj_set_size(sbtn, 96, 34);
    lv_obj_align(sbtn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(sbtn, lv_color_hex(0x1D3A5F), 0);
    lv_obj_set_style_radius(sbtn, 8, 0);
    lv_obj_add_event_cb(sbtn, search_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(sbtn);
    lv_label_set_text(sl, "搜索");
    lv_obj_set_style_text_font(sl, &lv_font_cn_16, 0);
    lv_obj_center(sl);

    /* 状态条: 浮层正下方; 固定两行高, 文字长时自动换行
     * (LV_SIZE_CONTENT 与 wrap label 组合撑不开容器, label 会被裁成横线) */
    lv_obj_t *sp2 = lv_obj_create(viewport);
    lv_obj_set_size(sp2, 320, 58);
    lv_obj_set_pos(sp2, 8, 62);
    lv_obj_set_style_bg_color(sp2, lv_color_hex(0x101B2D), 0);
    lv_obj_set_style_bg_opa(sp2, LV_OPA_70, 0);
    lv_obj_set_style_radius(sp2, 8, 0);
    lv_obj_set_style_border_width(sp2, 0, 0);
    lv_obj_set_style_pad_all(sp2, 4, 0);
    lv_obj_clear_flag(sp2, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(sp2, LV_OBJ_FLAG_HIDDEN);          /* 有结果才显示 */
    dest_label = lv_label_create(sp2);
    lv_obj_set_style_text_font(dest_label, &lv_font_cn_16, 0);
    lv_obj_set_style_text_color(dest_label, lv_color_hex(0xB8CCDD), 0);
    lv_label_set_long_mode(dest_label, LV_LABEL_LONG_WRAP);  /* 宽度不够自动换行 */
    lv_obj_set_width(dest_label, 304);
    lv_obj_align(dest_label, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_add_flag(dest_label, LV_OBJ_FLAG_HIDDEN);   /* 有搜索/路线结果才显示 */

    /* 初始定位 */
    apply_view(frac_x(car_lon), frac_y(car_lat));

    tile_loader_init();
    amap_init();
    lv_timer_create(poll_cb, 150, NULL);

#if USE_SDL
    if (getenv("DEMO_SELFTEST")) {
        setvbuf(stdout, NULL, _IONBF, 0);   /* 自测日志走管道, 关全缓冲防 timeout 杀进程丢日志 */
        lv_timer_create(selftest_cb, 1000, NULL);
    }
#endif
}
