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
 *   - keep-alive 长连接池(t0~t7 每域一条): 每瓦片一建一断的连接风暴
 *     是板子整机网络挂死(RK3568 GMAC 驱动层)的最大嫌疑, 长连接把
 *     建连频率降一个数量级
 *   - 增量收包定界: Content-Length / chunked(0 结束块判定) / EOF 三种
 *   - 非阻塞 connect 4s 上限(SYN 黑洞时阻塞 connect 会卡约 2 分钟,
 *     SO_SNDTIMEO 管不到它)
 *   - 复用连接失效自动换新重试一次; 连续失败退避 3s
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
#include <poll.h>
#include <fcntl.h>
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
/* keep-alive 连接池: t0~t7 每个子域一条长连接 */
static int conn_fds[8] = {-1,-1,-1,-1,-1,-1,-1,-1};

/* 头部解析缓冲(响应体是二进制, 把头复制出来 NUL 结尾再用 strstr 类接口) */
static char hdr_buf[2048];

/* 二进制安全查找(响应体是 PNG, 含 \0, 不能用 strstr) */
static const char *bin_find(const char *hay, size_t hlen, const char *needle)
{
    size_t nlen = strlen(needle);
    if (nlen == 0 || hlen < nlen) return NULL;
    for (const char *p = hay; p <= hay + hlen - nlen; p++)
        if (p[0] == needle[0] && memcmp(p, needle, nlen) == 0) return p;
    return NULL;
}

/* 非阻塞 connect + 4s 上限: 阻塞 connect 遇 SYN 黑洞会卡约 2 分钟
 * (SO_SNDTIMEO 不约束 connect, tcp_syn_retries 默认 6) */
static int tcp_connect(const char *host, const char *port)
{
    struct addrinfo hints, *res = NULL, *p = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags  = AI_ADDRCONFIG;   /* 无全局 IPv6 出口时不做 AAAA 查询 */

    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0 || res == NULL) {
        fprintf(stderr, "[tile] DNS %s 失败: %s\n", host, gai_strerror(rc));
        return -1;
    }

    int fd = -1;
    for (p = res; p != NULL; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;

        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        if (connect(fd, p->ai_addr, p->ai_addrlen) != 0) {
            if (errno != EINPROGRESS) { close(fd); fd = -1; continue; }
            struct pollfd pf = { .fd = fd, .events = POLLOUT };
            if (poll(&pf, 1, 4000) <= 0) { close(fd); fd = -1; continue; }
            int soerr = 0; socklen_t slen = sizeof(soerr);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen);
            if (soerr != 0) { close(fd); fd = -1; continue; }
        }
        fcntl(fd, F_SETFL, flags);             /* 恢复阻塞 IO */
        struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        break;
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

/* 解除 chunked 编码: in → out(动态分配), 返回长度, 失败返回 -1
 * *done=true 表示收到 0 结束块(体完整, 连接可复用) */
static long dechunk(const char *in, long in_len, char **out, bool *done)
{
    long cap = in_len + 16, olen = 0;
    char *o = malloc((size_t)cap);
    const char *p = in;
    const char *end = in + in_len;
    *done = false;

    while (p < end) {
        const char *le = bin_find(p, (size_t)(end - p), "\r\n");
        if (!le) break;                        /* 块大小行不完整 → 等更多数据 */
        long n = strtol(p, NULL, 16);
        p = le + 2;
        if (n == 0) { *done = true; break; }   /* 0 结束块 */
        if (n < 0 || p + n > end) break;       /* 数据不足 → 等更多数据 */
        if (olen + n > cap) { cap = olen + n + 1024; o = realloc(o, (size_t)cap); }
        memcpy(o + olen, p, (size_t)n);
        olen += n;
        p += n + 2;                            /* 跳过块数据后的 \r\n */
    }
    if (olen == 0 && !*done) { free(o); *out = NULL; return -1; }
    *out = o;
    return olen;
}

/* 下载一张瓦片: 复用/新建长连接, 增量收完整个响应, 校验后落盘 */
static bool http_get_tile(int z, int x, int y, const char *out_path)
{
    int idx = (x + y) % 8;
    char host[64];
    snprintf(host, sizeof(host), "t%d.tianditu.gov.cn", idx);
    char req[512];
    int req_len = snprintf(req, sizeof(req),
        "GET /DataServer?T=vec_w&x=%d&y=%d&l=%d&tk=%s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: PostmanRuntime/7.39.0\r\n"
        "Connection: keep-alive\r\n"
        "\r\n", x, y, z, TILE_TK, host);

    for (int attempt = 0; attempt < 2; attempt++) {
        bool reused = conn_fds[idx] >= 0;
        int fd = reused ? conn_fds[idx] : tcp_connect(host, "80");
        if (fd < 0) { fprintf(stderr, "[tile] connect %s 失败\n", host); return false; }

        if (send(fd, req, req_len, 0) != req_len) {   /* 复用的连接可能已被服务器关掉 */
            close(fd); conn_fds[idx] = -1;
            if (reused) continue;                     /* 换新连接再试一次 */
            return false;
        }

        /* 增量收包: 先定界头, 再按 CL / chunked / EOF 三种方式收体 */
        long cap = 1 << 20, len = 0, hdr_end = -1;
        long clen = -1;
        bool chunked = false, srv_close = false, eof_seen = false, complete = false;
        char *buf = malloc((size_t)cap);
        while (!complete) {
            if (hdr_end < 0) {
                const char *he = bin_find(buf, (size_t)len, "\r\n\r\n");
                if (he) {
                    hdr_end = (he - buf) + 4;
                    size_t hl = (size_t)hdr_end - 4;
                    if (hl < sizeof(hdr_buf)) {
                        memcpy(hdr_buf, buf, hl);
                        hdr_buf[hl] = '\0';
                        const char *v;
                        if ((v = header_val(hdr_buf, "content-length"))) clen = atol(v);
                        if ((v = header_val(hdr_buf, "transfer-encoding")) &&
                            strncasecmp(v, "chunked", 7) == 0) chunked = true;
                        if ((v = header_val(hdr_buf, "connection")) &&
                            strncasecmp(v, "close", 5) == 0) srv_close = true;
                    }
                }
            }
            if (hdr_end >= 0) {
                long have = len - hdr_end;
                if (chunked) {
                    char *tmp = NULL; bool done = false;
                    dechunk(buf + hdr_end, have, &tmp, &done);
                    free(tmp);
                    if (done) { complete = true; break; }
                } else if (clen >= 0 && have >= clen) {
                    complete = true; break;
                }
            }
            if (len == cap) {
                cap *= 2;
                buf = realloc(buf, (size_t)cap);
                if (!buf) break;
            }
            ssize_t n = recv(fd, buf + len, cap - len, 0);
            if (n <= 0) { eof_seen = true; break; }   /* EOF / 超时 / 错误 */
            len += n;
            if (len > (4 << 20)) break;               /* 响应异常超大 */
        }

        /* 体完整的合法形态: 定长收满 / 分块收完 / 无定界靠 EOF 关闭 */
        bool ok_proto = complete ||
                        (eof_seen && hdr_end >= 0 && clen < 0 && !chunked);
        if (!ok_proto) {                              /* 连接已废 → 丢弃 */
            close(fd); conn_fds[idx] = -1;
            free(buf);
            if (reused) continue;                     /* 大概率复用了早被关闭的连接 */
            fprintf(stderr, "[tile] %s 响应不完整\n", host);
            return false;
        }
        if (srv_close || eof_seen) { close(fd); conn_fds[idx] = -1; }
        else conn_fds[idx] = fd;                      /* 长连接保留复用 */

        /* 状态码 */
        if (strncmp(buf, "HTTP/", 5) != 0 || atoi(buf + 9) != 200) {
            fprintf(stderr, "[tile] HTTP 状态异常: %.32s\n", buf);
            free(buf); return false;
        }

        /* 提取体 */
        const char *body = buf + hdr_end;
        long body_len = len - hdr_end;
        char *alloc = NULL;
        if (chunked) {
            bool done = false;
            body_len = dechunk(body, body_len, &alloc, &done);
            body = alloc;
            if (!done || body_len < 0) {
                fprintf(stderr, "[tile] chunked 解码异常\n");
                free(buf); return false;
            }
        }

        /* 校验 PNG 魔数(不是 PNG = 天地图返回的错误 JSON) */
        bool ok = body_len > 8 && memcmp(body, "\x89PNG", 4) == 0;
        if (ok) {
            FILE *fp = fopen(out_path, "wb");
            if (fp) { fwrite(body, 1, (size_t)body_len, fp); fclose(fp); }
            else { fprintf(stderr, "[tile] 写文件失败: %s\n", out_path); ok = false; }
        } else {
            fprintf(stderr, "[tile] 服务返回错误: %.*s\n",
                    (int)(body_len > 120 ? 120 : body_len), body);
        }
        free(alloc);                                  /* 未解码时为 NULL, free 安全 */
        free(buf);
        return ok;
    }
    return false;
}

/* ---------------- 后台线程 ---------------- */
static void *worker(void *arg)
{
    (void)arg;
    int consec_fail = 0;
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
            consec_fail = 0;
            pthread_mutex_lock(&q_mu);
            g_new++;
            pthread_mutex_unlock(&q_mu);
        } else if (++consec_fail >= 3) {
            /* 网络不可达时停止疯狂重试: 既省电也避免加剧网卡驱动负担 */
            fprintf(stderr, "[tile] 连续失败 %d 次, 退避 3s\n", consec_fail);
            sleep(3);
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
