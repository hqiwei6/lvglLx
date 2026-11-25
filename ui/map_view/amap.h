/**
 * @file amap.h
 * @brief 高德开放平台 Web API 客户端(POI 搜索 + 驾车路线规划)
 *
 * 用法:
 *   amap_init();
 *   amap_search("从化粤嵌", 113.52782, 23.56941);  // 异步, 后台线程执行
 *   ...
 *   amap_result_t r;
 *   if (amap_take_result(&r)) {              // UI 轮询取走完成的结果
 *       if (r.type == 1) { 用 r.dst_lon/lat, r.name; amap_route(...); }
 *       if (r.type == 2) { 用 r.pts/npts 画线; free(r.pts) 由调用方接管 }
 *   }
 */
#ifndef AMAP_H
#define AMAP_H

#include <stdbool.h>

typedef enum {
    AMAP_IDLE = 0,           /* 空闲 */
    AMAP_WORKING,            /* 请求进行中 */
    AMAP_OK,                 /* 最近一次任务成功 */
    AMAP_FAIL,               /* 最近一次任务失败 */
} amap_state_t;

#define AMAP_MAX_CAND 6

typedef struct {
    int    type;             /* 结果类型: 1=搜索候选 2=路线 */
    int    seq;              /* 完成计数, 每完成一次任务 +1 */
    char   name[64];         /* 目的地名称(首条候选) */
    double dst_lon, dst_lat; /* 目的地坐标(WGS84, 可直接配天地图) */
    int    distance_m;       /* 路线总距离(米) */
    int    duration_s;       /* 预计耗时(秒) */
    int    npts;             /* 路线点数 */
    double *pts;             /* 路线点[WGS84, lon,lat 交替]; 所有权移交调用方, 用完 free() */
    char   err[96];          /* 失败原因(成功时为空串) */
    /* 搜索候选列表(仅 type==1): 同名异地时 UI 弹浮层让用户确认 */
    int    ncand;
    char   cand_name[AMAP_MAX_CAND][48];
    double cand_lon[AMAP_MAX_CAND], cand_lat[AMAP_MAX_CAND]; /* WGS84 */
    int    cand_dist[AMAP_MAX_CAND];  /* 距当前位置直线距离(米) */
} amap_result_t;

void         amap_init(void);
bool         amap_search(const char *keywords, double olon, double olat); /* 异步; 起点用于候选距离 */
bool         amap_route(double olon, double olat,
                        double dlon, double dlat);                  /* 异步 */
amap_state_t amap_state(void);                                      /* 当前状态(显示"搜索中…"用) */
bool         amap_take_result(amap_result_t *out);                  /* 取走完成结果(每条只给一次) */

#endif
