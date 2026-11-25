/**
 * @file amap.c
 * @brief 高德开放平台 Web API 客户端 —— POI 搜索 + 驾车路线规划
 *
 * 设计:
 *   - 纯 POSIX socket, 零第三方依赖(与 tile_loader 同一套路)
 *   - 高德 v3 接口支持 HTTP 明文, 板上无需 TLS
 *   - 请求用 HTTP/1.0 + Connection: close: 响应以 EOF 定界, 不用解析
 *     Content-Length / chunked, 收包逻辑最简(低频接口, 无需长连接)
 *   - 后台线程逐个执行任务; 结果发布后 UI 用 amap_take_result() 取走
 *   - 坐标: 高德返回 GCJ02(国测局加密), 天地图瓦片是 WGS84, 相差
 *     300~600 米; 这里做迭代法转换(2 次迭代, 误差 <1 米)
 *   - 路线 polyline 分散在每个 step 里, 全部拼接后逐点转 WGS84
 *
 * key: 直接内置(用户明确要求写进源码; 高德 key 属个人练习凭证, 泄露风险自担)
 */
#ifndef AMAP_KEY
#define AMAP_KEY "4ebb4b3b5ffe3d921aed91eb20af24bf"
#endif

#include "amap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <netinet/in.h>

#define AMAP_HOST "restapi.amap.com"

/* ---------------- 任务/结果状态 ---------------- */
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cv = PTHREAD_COND_INITIALIZER;
static int          g_task;                    /* 0=无 1=搜索 2=路线 */
static char         g_kw[80];                  /* 搜索关键字 */
static double       g_o_lon, g_o_lat, g_d_lon, g_d_lat;
static amap_state_t g_state = AMAP_IDLE;
static amap_result_t g_res;                    /* 最新完成结果 */
static bool         g_done;                    /* 有未取走的结果 */
static bool         g_inited;

/* ---------------- GCJ02 -> WGS84 ---------------- */
#define GCJ_A  6378245.0                        /* 克拉索夫斯基椭球长半轴(米) */
#define GCJ_EE 0.00669342162296594323           /* 第一偏心率平方 */

static double transform_lat(double x, double y)
{
    double ret = -100.0 + 2.0 * x + 3.0 * y + 0.2 * y * y + 0.1 * x * y
                 + 0.2 * sqrt(fabs(x));
    ret += (20.0 * sin(6.0 * x * M_PI) + 20.0 * sin(2.0 * x * M_PI)) * 2.0 / 3.0;
    ret += (20.0 * sin(y * M_PI) + 40.0 * sin(y / 3.0 * M_PI)) * 2.0 / 3.0;
    ret += (160.0 * sin(y / 12.0 * M_PI) + 320.0 * sin(y * M_PI / 30.0)) * 2.0 / 3.0;
    return ret;
}

static double transform_lon(double x, double y)
{
    double ret = 300.0 + x + 2.0 * y + 0.1 * x * x + 0.1 * x * y
                 + 0.1 * sqrt(fabs(x));
    ret += (20.0 * sin(6.0 * x * M_PI) + 20.0 * sin(2.0 * x * M_PI)) * 2.0 / 3.0;
    ret += (20.0 * sin(x * M_PI) + 40.0 * sin(x / 3.0 * M_PI)) * 2.0 / 3.0;
    ret += (150.0 * sin(x / 12.0 * M_PI) + 300.0 * sin(x / 30.0 * M_PI)) * 2.0 / 3.0;
    return ret;
}

static bool out_of_china(double lon, double lat)
{
    return lon < 72.004 || lon > 137.8347 || lat < 0.8293 || lat > 55.8271;
}

/* gcj = wgs + t(wgs); t() 返回的是椭球修正前的粗偏移, 需再除以
 * 子午线曲率换算成度; 给定 gcj 迭代反解 wgs, 3 次迭代误差 <1 米 */
static void gcj2wgs(double glon, double glat, double *wlon, double *wlat)
{
    if (out_of_china(glon, glat)) { *wlon = glon; *wlat = glat; return; }
    double lon = glon, lat = glat;
    for (int i = 0; i < 3; i++) {
        double raw_lat = transform_lat(lon - 105.0, lat - 35.0);
        double raw_lon = transform_lon(lon - 105.0, lat - 35.0);
        double radlat  = lat * M_PI / 180.0;
        double magic   = 1.0 - GCJ_EE * sin(radlat) * sin(radlat);
        double sq      = sqrt(magic);
        double dlat    = (raw_lat * 180.0) / ((GCJ_A * (1.0 - GCJ_EE)) / (magic * sq) * M_PI);
        double dlon    = (raw_lon * 180.0) / (GCJ_A / sq * M_PI * cos(radlat));
        lon = glon - dlon;
        lat = glat - dlat;
    }
    *wlon = lon;
    *wlat = lat;
}

/* ---------------- URL 编码 ---------------- */
static void url_encode(const char *in, char *out, size_t osz)
{
    size_t o = 0;
    for (; *in && o + 4 < osz; in++) {
        unsigned char c = (unsigned char)*in;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = c;
        } else {
            o += (size_t)snprintf(out + o, osz - o, "%%%02X", c);
        }
    }
    out[o] = '\0';
}

/* ---------------- HTTP GET (HTTP/1.0, EOF 定界) ---------------- */
/* 返回 malloc 的响应体(NUL 结尾)或 NULL; *out_len 为体长度 */
static char *http_get(const char *host, const char *query, long *out_len)
{
    struct addrinfo hints, *res = NULL, *p;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family  = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags   = AI_ADDRCONFIG;
    if (getaddrinfo(host, "80", &hints, &res) != 0 || !res) return NULL;

    int fd = -1;
    for (p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        if (connect(fd, p->ai_addr, p->ai_addrlen) != 0 &&
            errno != EINPROGRESS) { close(fd); fd = -1; continue; }
        if (errno == EINPROGRESS) {              /* 非阻塞 connect 4s 上限 */
            struct pollfd pf = { .fd = fd, .events = POLLOUT };
            if (poll(&pf, 1, 4000) <= 0) { close(fd); fd = -1; continue; }
            int soerr = 0; socklen_t slen = sizeof(soerr);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen);
            if (soerr != 0) { close(fd); fd = -1; continue; }
        }
        fcntl(fd, F_SETFL, flags);
        struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        break;
    }
    freeaddrinfo(res);
    if (fd < 0) return NULL;

    char req[1024];
    int req_len = snprintf(req, sizeof(req),
        "GET %s HTTP/1.0\r\n"
        "Host: %s\r\n"
        "User-Agent: SmartIVI/1.0\r\n"
        "Connection: close\r\n"
        "\r\n", query, host);
    if (send(fd, req, (size_t)req_len, 0) != req_len) { close(fd); return NULL; }

    /* 收到 EOF(HTTP/1.0 + close 必然如此), 响应即完整 */
    long cap = 64 << 10, len = 0;
    char *buf = malloc((size_t)cap);
    for (;;) {
        if (len + 4096 > cap) { cap *= 2; buf = realloc(buf, (size_t)cap); }
        ssize_t k = recv(fd, buf + len, cap - len, 0);
        if (k <= 0) break;
        len += k;
        if (len > (8 << 20)) break;              /* 响应异常超大 */
    }
    close(fd);
    if (len < 16) { free(buf); return NULL; }

    /* 定界响应头 */
    const char *he = NULL;
    for (const char *s = buf; s <= buf + len - 4; s++)
        if (memcmp(s, "\r\n\r\n", 4) == 0) { he = s + 4; break; }
    if (!he || strncmp(buf, "HTTP/", 5) != 0 || atoi(buf + 9) != 200) {
        fprintf(stderr, "[amap] HTTP 异常: %.40s\n", buf);
        free(buf); return NULL;
    }
    long blen = len - (long)(he - buf);
    memmove(buf, he, (size_t)blen);
    buf = realloc(buf, (size_t)blen + 1);
    buf[blen] = '\0';
    *out_len = blen;
    return buf;
}

/* ---------------- 结果发布 ---------------- */
static void publish(int type, amap_state_t st, double *pts, int npts,
                    const char *name, const char *err,
                    double dst_lon, double dst_lat,
                    int dist_m, int dur_s,
                    const amap_result_t *cand)
{
    pthread_mutex_lock(&mu);
    free(g_res.pts);                             /* 上次结果若未被取走, 释放防泄漏 */
    memset(&g_res, 0, sizeof(g_res));
    g_res.type      = type;
    g_res.pts       = pts;
    g_res.npts      = npts;
    g_res.dst_lon   = dst_lon;
    g_res.dst_lat   = dst_lat;
    g_res.distance_m = dist_m;
    g_res.duration_s = dur_s;
    if (name) snprintf(g_res.name, sizeof(g_res.name), "%s", name);
    if (err)  snprintf(g_res.err,  sizeof(g_res.err),  "%s", err);
    if (cand) {
        memcpy(g_res.cand_name, cand->cand_name, sizeof(g_res.cand_name));
        memcpy(g_res.cand_lon,  cand->cand_lon,  sizeof(g_res.cand_lon));
        memcpy(g_res.cand_lat,  cand->cand_lat,  sizeof(g_res.cand_lat));
        memcpy(g_res.cand_dist, cand->cand_dist, sizeof(g_res.cand_dist));
        g_res.ncand = cand->ncand;
    }
    g_state = st;
    g_done  = true;
    g_task  = 0;
    pthread_mutex_unlock(&mu);
}

static void publish_fail(int type, const char *err)
{
    fprintf(stderr, "[amap] 任务%d失败: %s\n", type, err);
    publish(type, AMAP_FAIL, NULL, 0, NULL, err, 0, 0, 0, 0, NULL);
}

/* ---------------- JSON 提取 ---------------- */
/* 从 from 起找首个 "key":"值"; json_str 是从串头开始的便捷封装 */
static bool json_str_at(const char *from, const char *key, char *out, size_t osz)
{
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *p = strstr(from, pat);
    if (!p) return false;
    p += strlen(pat);
    size_t o = 0;
    while (*p && *p != '"' && o + 1 < osz) out[o++] = *p++;
    out[o] = '\0';
    return true;
}

static bool json_str(const char *js, const char *key, char *out, size_t osz)
{
    return json_str_at(js, key, out, osz);
}

/* 两点球面距离(米), haversine */
static int dist_m(double lon1, double lat1, double lon2, double lat2)
{
    double dlat = (lat2 - lat1) * M_PI / 180.0;
    double dlon = (lon2 - lon1) * M_PI / 180.0;
    double a = sin(dlat / 2) * sin(dlat / 2) +
               cos(lat1 * M_PI / 180.0) * cos(lat2 * M_PI / 180.0) *
               sin(dlon / 2) * sin(dlon / 2);
    return (int)(2 * GCJ_A * asin(sqrt(a)));
}

/* ---------------- 任务: POI 搜索 ---------------- */
static void do_search(const char *kw)
{
    char enc[256], query[512];
    url_encode(kw, enc, sizeof(enc));
    snprintf(query, sizeof(query),
             "/v3/place/text?keywords=%s&key=%s&offset=%d&page=1",
             enc, AMAP_KEY, AMAP_MAX_CAND);

    long jlen = 0;
    char *js = http_get(AMAP_HOST, query, &jlen);
    if (!js) { publish_fail(1, "网络请求失败"); return; }

    char info[64] = "";
    json_str(js, "info", info, sizeof(info));
    const char *pois = strstr(js, "\"pois\":[");
    if (!strstr(js, "\"status\":\"1\"") || !pois) {
        publish_fail(1, info[0] ? info : "未找到结果");
        free(js); return;
    }

    /* 逐条解析 pois[]: 字段序 name→...→location 固定, 从 name 向后找 location
     * 即本条 POI 的坐标; 高德返回 GCJ02, 统一转 WGS84 */
    amap_result_t cand = { .ncand = 0 };
    const char *pp = pois;
    while (cand.ncand < AMAP_MAX_CAND) {
        pp = strstr(pp + 1, "\"name\":\"");
        if (!pp) break;
        const char *lp = strstr(pp, "\"location\":\"");
        if (!lp) break;
        char cn[48];
        if (!json_str_at(pp, "name", cn, sizeof(cn))) break;
        double glon, glat;
        if (sscanf(lp + strlen("\"location\":\""), "%lf,%lf", &glon, &glat) != 2) break;

        double wlon, wlat;
        gcj2wgs(glon, glat, &wlon, &wlat);
        int i = cand.ncand++;
        snprintf(cand.cand_name[i], sizeof(cand.cand_name[i]), "%s", cn);
        cand.cand_lon[i]  = wlon;
        cand.cand_lat[i]  = wlat;
        cand.cand_dist[i] = dist_m(g_o_lon, g_o_lat, glon, glat);  /* 距当前位置 */
        pp = lp;
    }
    if (cand.ncand == 0) {
        publish_fail(1, "坐标解析失败");
        free(js); return;
    }

    free(js);
    fprintf(stderr, "[amap] 搜索到 %d 个候选, 首条: %s\n",
            cand.ncand, cand.cand_name[0]);
    publish(1, AMAP_OK, NULL, 0, cand.cand_name[0], NULL,
            cand.cand_lon[0], cand.cand_lat[0], 0, 0, &cand);
}

/* ---------------- 任务: 驾车路线 ---------------- */
static void do_route(void)
{
    char query[512];
    snprintf(query, sizeof(query),
             "/v3/direction/driving?origin=%.6f,%.6f&destination=%.6f,%.6f"
             "&key=%s&extensions=base",
             g_o_lon, g_o_lat, g_d_lon, g_d_lat, AMAP_KEY);

    long jlen = 0;
    char *js = http_get(AMAP_HOST, query, &jlen);
    if (!js) { publish_fail(2, "网络请求失败"); return; }

    char info[64] = "";
    json_str(js, "info", info, sizeof(info));
    if (!strstr(js, "\"status\":\"1\"")) {
        publish_fail(2, info[0] ? info : "规划失败");
        free(js); return;
    }

    /* polyline 散落在每个 step 里, 全部拼接并逐点转 WGS84 */
    int cap = 512, n = 0;
    double *pts = malloc((size_t)cap * 2 * sizeof(double));
    const char *p = js;
    while ((p = strstr(p, "\"polyline\":\"")) != NULL) {
        p += strlen("\"polyline\":\"");
        while (*p && *p != '"') {
            double lon, lat;
            if (sscanf(p, "%lf,%lf", &lon, &lat) == 2) {
                if (n == cap) {
                    cap *= 2;
                    pts = realloc(pts, (size_t)cap * 2 * sizeof(double));
                }
                gcj2wgs(lon, lat, &pts[2 * n], &pts[2 * n + 1]);
                n++;
            }
            const char *semi = strchr(p, ';');
            const char *quo  = strchr(p, '"');
            if (!semi || (quo && quo < semi)) {    /* 本段 polyline 结束 */
                if (quo) p = quo;
                break;
            }
            p = semi + 1;
        }
        if (*p == '"') p++;
    }

    if (n < 2) {
        free(pts);
        publish_fail(2, info[0] ? info : "无有效路线");
        free(js); return;
    }

    /* distance/duration 在 paths[0] 内、steps 之前, 首个出现即总览 */
    char dist[16] = "", dur[16] = "";
    json_str(js, "distance", dist, sizeof(dist));
    json_str(js, "duration", dur,  sizeof(dur));

    fprintf(stderr, "[amap] 路线 %d 点, %s 米, %s 秒\n", n, dist, dur);
    publish(2, AMAP_OK, pts, n, g_kw, NULL, g_d_lon, g_d_lat,
            atoi(dist), atoi(dur), NULL);
    free(js);
}

/* ---------------- 后台线程 ---------------- */
static void *worker(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&mu);
        while (g_task == 0) pthread_cond_wait(&cv, &mu);
        int task = g_task;
        char kw[80] = "";
        if (task == 1) snprintf(kw, sizeof(kw), "%s", g_kw);
        pthread_mutex_unlock(&mu);

        if (task == 1) do_search(kw);
        else           do_route();
    }
    return NULL;
}

/* ---------------- 对外接口 ---------------- */
void amap_init(void)
{
    pthread_mutex_lock(&mu);
    if (g_inited) { pthread_mutex_unlock(&mu); return; }
    g_inited = true;
    pthread_mutex_unlock(&mu);
    pthread_t tid;
    pthread_create(&tid, NULL, worker, NULL);
    pthread_detach(tid);
}

bool amap_search(const char *keywords, double olon, double olat)
{
    if (!keywords || !keywords[0]) return false;
    pthread_mutex_lock(&mu);
    if (g_task) { pthread_mutex_unlock(&mu); return false; }   /* 任务进行中 */
    g_task = 1;
    snprintf(g_kw, sizeof(g_kw), "%s", keywords);
    g_o_lon = olon;                       /* 起点: 候选距离以此计算 */
    g_o_lat = olat;
    g_state = AMAP_WORKING;
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&mu);
    return true;
}

bool amap_route(double olon, double olat, double dlon, double dlat)
{
    pthread_mutex_lock(&mu);
    if (g_task) { pthread_mutex_unlock(&mu); return false; }
    g_task  = 2;
    g_o_lon = olon; g_o_lat = olat;
    g_d_lon = dlon; g_d_lat = dlat;
    g_state = AMAP_WORKING;
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&mu);
    return true;
}

amap_state_t amap_state(void)
{
    pthread_mutex_lock(&mu);
    amap_state_t s = g_state;
    pthread_mutex_unlock(&mu);
    return s;
}

bool amap_take_result(amap_result_t *out)
{
    pthread_mutex_lock(&mu);
    if (!g_done) { pthread_mutex_unlock(&mu); return false; }
    *out = g_res;
    g_res.pts = NULL;                            /* pts 所有权移交调用方 */
    g_done = false;
    pthread_mutex_unlock(&mu);
    return true;
}
