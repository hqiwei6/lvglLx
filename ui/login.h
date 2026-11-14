#ifndef LOGIN_H
#define LOGIN_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl/lvgl.h"

/* 创建中文登录页（1024x600） */
void login_page_create(void);

/* 获取登录页输入控件组（绑定鼠标/键盘输入设备用） */
lv_group_t *login_page_get_group(void);

#ifdef __cplusplus
}
#endif

#endif /* LOGIN_H */
