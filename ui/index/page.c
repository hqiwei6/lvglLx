//
// Created by zhang on 2026/11/24.
//

#include "page.h"
#include "lvgl/lvgl.h"

LV_FONT_DECLARE(lv_font_cn_16);

void page_create() {
    lv_obj_t *obj_create = lv_obj_create(lv_scr_act());
    lv_obj_t *label_create = lv_label_create(obj_create);
    lv_label_set_text(label_create, "解放解放阿里啊时间的可方式道路架空历史的");
    lv_obj_set_style_text_font(obj_create, &lv_font_cn_16, 0);
}
