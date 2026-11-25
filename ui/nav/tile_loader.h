#ifndef TILE_LOADER_H
#define TILE_LOADER_H

#include <stdbool.h>

/*
 * 天地图 Key（DataServer 用）
 * 换自己的 key 改这里，或编译期 -DTILE_TK='"..."'
 */
#ifndef TILE_TK
#define TILE_TK "f44dea7a049dfaa715fc1d9c12cfb904"
#endif

/* 瓦片缓存根目录(PC 模拟器与板子统一) */
#ifndef TILE_BASE_DIR
#define TILE_BASE_DIR "/userdata/tiles"
#endif

/* 纯网络瓦片加载器: 运行时按需从天地图 DataServer 下载, 落盘缓存 */
void tile_loader_init(void);                 /* 启动后台下载线程(幂等) */
bool tile_loader_ready(const char *path);    /* 瓦片缓存文件是否已存在 */
void tile_loader_request(const char *path);  /* 不存在则入队后台下载(去重) */
bool tile_loader_pop_new(void);              /* 自上次调用后有新瓦片落盘? */
int  tile_loader_pending(void);              /* 当前排队/在途下载数 */

#endif /* TILE_LOADER_H */
