/**
 * @file tile_loader.c
 * @brief 纯网络瓦片加载器 —— 后台线程按需从天地图 DataServer 拉取瓦片
 *
 * 设计:
 *   - UI 线程只做两件事: 查缓存文件是否在(tile_loader_ready)、入队请求(request)
 *   - 后台 pthread 逐个下载: 纯 POSIX socket HTTP GET, 零第三方依赖
 *   - 下载完成后只落盘 + 置标志, 绝不触碰任何 lv_* API;
 *     UI 用 lv_timer 轮询 tile_loader_pop_new() 再刷新, 线程安全
 *   - 缓存: 命中磁盘零开销, 未命中走网络; 同一瓦片只下载一次
 *
 * HTTP 细节:
 *   - 请求带 User-Agent(天地图 WAF 会拦无 UA 的裸请求)
 *   - Connection: close, 读到 EOF 为止; 支持 Content-Length / chunked 两种响应体
 *   - 响应体校验 PNG 魔数, 是错误 JSON(如 key 配额)则不落盘并打印原因
 */
#include "tile_loader.h"
#include "nav_page.h"           /* TILE_BASE_DIR */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <netinet/in.h>

/* ---------------- 队列 ---------------- */
#define QLEN  64
#define PATH_MAX_LEN 96

static char         q_buf[QLEN][PATH_MAX_LEN];
static int          q_head, q_count;
static int          q_pending;                 /* 排队 + 在途数量 */
static pthread_mutex_t q_mu  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  q_cv  = PTHREAD_COND_INITIALIZER;
static int          g_new;                     /* 新落盘瓦片计数 */
static bool         g_inited;

/* 剥离 lv_fs 盘符前缀 "A:" → 真实 POSIX 路径 (stat/fopen 用) */
static const char *real_path(const char *p)
{
    if (p[0] >= 'A' && p[0] <= 'Z' && p[1] == ':') return p + 2;
    return p;
}

static bool file_exists(const char *path)
{
    struct stat st;
    path = real_path(path);
    return stat(path, &st) == 0 && st.st_size > 0;
}

/* ---------------- HTTP 下载 ---------------- */
static int tcp_connect(const char *host, const char *port)
{
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port, &hints, &res) != 0 || res == NULL) return -1;

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return -1; }

    struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        close(fd); freeaddrinfo(res); return -1;
    }
    freeaddrinfo(res);
    return fd;
}

/* 在响应头里找字段(大小写不敏感), 返回值指针或 NULL */
static const char *header_val(const char *hdr, const char *key)
{
    size_t klen = strlen(key);
    const char *p = hdr;
    while (p && *p) {
        const char *eol = strstr(p, "\r\n");
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        if (len > klen && strncasecmp(p, key, klen) == 0 && p[klen] == ':') {
            const char *v = p + klen + 1;
            while (*v == ' ') v++;
            return v;
        }
        p = eol ? eol + 2 : NULL;
    }
    return NULL;
}

/* 解除 chunked 编码: in → out(动态分配), 返回长度, 失败返回 -1 */
static long dechunk(const char *in, long in_len, char **out)
{
    long cap = in_len, olen = 0;
    char *o = malloc(cap);
    const char *p = in;
    const char *end = in + in_len;

    while (p < end) {
        /* 读块大小行 */
        const char *le = strstr(p, "\r\n");
        if (!le) break;
        long n = (long)strtol(p, NULL, 16);
        p = le + 2;
        if (n <= 0) break;                     /* 0 块 = 结束 */
        if (p + n > end) n = end - p;
        if (olen + n > cap) { cap = olen + n + 1024; o = realloc(o, cap); }
        memcpy(o + olen, p, n);
        olen += n;
        p += n + 2;                            /* 跳过块数据后的 \r\n */
    }
    *out = o;
    return olen;
}

/* 下载一张瓦片到 out_path, 成功返回 true */
static bool http_get_tile(int z, int x, int y, const char *out_path)
{
    char host[64];
    snprintf(host, sizeof(host), "t%d.tianditu.gov.cn", (x + y) % 8);   /* t0~t7 分流 */
    char req[512];
    int req_len = snprintf(req, sizeof(req),
        "GET /DataServer?T=vec_w&x=%d&y=%d&l=%d&tk=%s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: PostmanRuntime/7.39.0\r\n"
        "Connection: close\r\n"
        "\r\n", x, y, z, TILE_TK, host);

    int fd = tcp_connect(host, "80");
    if (fd < 0) { fprintf(stderr, "[tile] connect %s 失败\n", host); return false; }

    if (send(fd, req, req_len, 0) != req_len) { close(fd); return false; }

    /* 收完整响应 */
    long cap = 1 << 20, len = 0;
    char *buf = malloc(cap);
    for (;;) {
        if (len == cap) { cap *= 2; buf = realloc(buf, cap); }
        ssize_t n = recv(fd, buf + len, cap - len, 0);
        if (n <= 0) break;                     /* EOF / 超时 / 错误 */
        len += n;
    }
    close(fd);

    /* 分离头和体 */
    char *body = strstr(buf, "\r\n\r\n");
    if (!body) { free(buf); return false; }
    body += 4;
    long body_len = len - (body - buf);

    /* 解析状态码 */
    if (strncmp(buf, "HTTP/", 5) != 0 || atoi(buf + 9) != 200) {
        fprintf(stderr, "[tile] HTTP 状态异常: %.32s\n", buf);
        free(buf); return false;
    }

    /* chunked 解码 */
    char *alloc = NULL;                        /* dechunk 后的独立缓冲 */
    const char *te = header_val(buf, "transfer-encoding");
    if (te && strncasecmp(te, "chunked", 7) == 0) {
        body_len = dechunk(body, body_len, &alloc);
        if (body_len < 0) { free(buf); return false; }
        body = alloc;
    }

    /* 校验 PNG 魔数(不是 PNG = 天地图返回的错误 JSON) */
    bool ok = body_len > 8 && memcmp(body, "\x89PNG", 4) == 0;
    if (ok) {
        FILE *fp = fopen(out_path, "wb");
        if (fp) { fwrite(body, 1, body_len, fp); fclose(fp); }
        else { fprintf(stderr, "[tile] 写文件失败: %s\n", out_path); ok = false; }
    } else {
        fprintf(stderr, "[tile] 服务返回错误: %.*s\n",
                (int)(body_len > 120 ? 120 : body_len), body);
    }
    free(alloc);                               /* 未解码时为 NULL, free 安全 */
    free(buf);
    return ok;
}

/* ---------------- 后台线程 ---------------- */
static void *worker(void *arg)
{
    (void)arg;
    for (;;) {
        char path[PATH_MAX_LEN];
        pthread_mutex_lock(&q_mu);
        while (q_count == 0) pthread_cond_wait(&q_cv, &q_mu);
        strcpy(path, q_buf[q_head]);
        q_head = (q_head + 1) % QLEN;
        q_count--;
        pthread_mutex_unlock(&q_mu);

        if (file_exists(path)) {               /* 可能已被上次任务完成 */
            pthread_mutex_lock(&q_mu);
            if (q_pending > 0) q_pending--;
            pthread_mutex_unlock(&q_mu);
            continue;
        }

        int z, x, y;
        const char *name = strrchr(path, '/');
        name = name ? name + 1 : path;
        if (sscanf(name, "%d_%d_%d", &z, &x, &y) == 3 &&
            http_get_tile(z, x, y, real_path(path))) {
            pthread_mutex_lock(&q_mu);
            g_new++;
            pthread_mutex_unlock(&q_mu);
        }
        pthread_mutex_lock(&q_mu);
        if (q_pending > 0) q_pending--;
        pthread_mutex_unlock(&q_mu);
    }
    return NULL;
}

void tile_loader_init(void)
{
    pthread_mutex_lock(&q_mu);
    if (g_inited) { pthread_mutex_unlock(&q_mu); return; }
    g_inited = true;
    pthread_mutex_unlock(&q_mu);

    mkdir(TILE_BASE_DIR, 0755);                /* 缓存目录不存在则创建 */

    pthread_t tid;
    pthread_create(&tid, NULL, worker, NULL);
    pthread_detach(tid);
}

bool tile_loader_ready(const char *path)
{
    return file_exists(path);
}

void tile_loader_request(const char *path)
{
    pthread_mutex_lock(&q_mu);
    /* 去重: 队列里已有则不重复入队 */
    for (int i = 0; i < q_count; i++) {
        int idx = (q_head + i) % QLEN;
        if (strcmp(q_buf[idx], path) == 0) { pthread_mutex_unlock(&q_mu); return; }
    }
    if (q_count < QLEN) {
        snprintf(q_buf[(q_head + q_count) % QLEN], PATH_MAX_LEN, "%s", path);
        q_count++;
        q_pending++;
        pthread_cond_signal(&q_cv);
    }
    pthread_mutex_unlock(&q_mu);
}

bool tile_loader_pop_new(void)
{
    pthread_mutex_lock(&q_mu);
    int n = g_new;
    g_new = 0;
    pthread_mutex_unlock(&q_mu);
    return n > 0;
}

int tile_loader_pending(void)
{
    pthread_mutex_lock(&q_mu);
    int n = q_pending;
    pthread_mutex_unlock(&q_mu);
    return n;
}
