/**
 * @file login.c
 * @brief LVGL 中文登录页
 *
 * 包含：标题、账号输入框、密码输入框、登录按钮、内置软键盘。
 * 演示账号：admin  密码：123456
 */
#include "login.h"

/* 自动生成的中文字库（见 gen_lvgl_font.py） */
LV_FONT_DECLARE(lv_font_cn_16)
LV_FONT_DECLARE(lv_font_cn_20)

#define FONT_CJK   &lv_font_cn_16
#define FONT_TITLE &lv_font_cn_20

/* 演示用账号密码 */
#define DEMO_USER "admin"
#define DEMO_PWD  "123456"

static lv_obj_t *kb;          /* 软键盘 */
static lv_obj_t *msg_label;   /* 登录结果提示 */
static lv_obj_t *user_ta;     /* 账号输入框 */
static lv_obj_t *pwd_ta;      /* 密码输入框 */

/* 输入框获得/失去焦点时，弹出或隐藏软键盘 */
static void ta_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *ta = lv_event_get_target(e);

    if (code == LV_EVENT_FOCUSED) {
        /* 只在真实输入设备（鼠标/键盘）触发焦点时弹软键盘；
           开机时 lv_group 自动聚焦第一个输入框也会发 FOCUSED，
           此时 lv_indev_get_act() 为 NULL，不弹键盘 */
        if (lv_indev_get_act() == NULL) return;
        lv_keyboard_set_textarea(kb, ta);
        lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(kb);
    }
    else if (code == LV_EVENT_DEFOCUSED) {
        lv_keyboard_set_textarea(kb, NULL);
        lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    }
}

/* 登录按钮回调：校验账号密码 */
static void login_btn_event_cb(lv_event_t *e)
{
    (void)e;
    const char *user = lv_textarea_get_text(user_ta);
    const char *pwd  = lv_textarea_get_text(pwd_ta);

    if (strcmp(user, DEMO_USER) == 0 && strcmp(pwd, DEMO_PWD) == 0) {
        lv_label_set_text(msg_label, "登录成功，正在进入系统...");
        lv_obj_set_style_text_color(msg_label, lv_color_hex(0x4CAF50), 0);
    } else {
        lv_label_set_text(msg_label, "账号或密码错误，请重新输入");
        lv_obj_set_style_text_color(msg_label, lv_color_hex(0xF44336), 0);
    }
    /* 收起键盘 */
    lv_keyboard_set_textarea(kb, NULL);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
}

/* 给输入框挂字体样式 */
static void ta_add_style(lv_obj_t *ta)
{
    lv_obj_set_style_text_font(ta, FONT_CJK, 0);
    lv_obj_set_style_text_font(ta, FONT_CJK, LV_PART_TEXTAREA_PLACEHOLDER); /* 占位提示文字也要中文 */
    lv_obj_set_style_text_color(ta, lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(0x4A90D9), 0);
    lv_obj_set_style_border_width(ta, 2, 0);
    lv_obj_set_style_radius(ta, 8, 0);
}

void login_page_create(void)
{
    /* ---------- 背景：深蓝渐变 ---------- */
    lv_obj_t *bg = lv_obj_create(lv_scr_act());
    lv_obj_set_size(bg, 1024, 600);
    lv_obj_set_pos(bg, 0, 0);
    lv_obj_set_style_bg_grad_dir(bg, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_color(bg, lv_color_hex(0x1E3C72), 0);
    lv_obj_set_style_bg_grad_color(bg, lv_color_hex(0x2A5298), 0);
    lv_obj_set_style_border_width(bg, 0, 0);
    lv_obj_set_style_radius(bg, 0, 0);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE);

    /* ---------- 顶部标题 ---------- */
    lv_obj_t *title = lv_label_create(bg);
    lv_label_set_text(title, "欢迎使用设备管理系统");
    lv_obj_set_style_text_font(title, FONT_TITLE, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_t *subtitle = lv_label_create(bg);
    lv_label_set_text(subtitle, "请登录您的账户");
    lv_obj_set_style_text_font(subtitle, FONT_CJK, 0);
    lv_obj_set_style_text_color(subtitle, lv_color_hex(0xB0C4DE), 0);
    lv_obj_align_to(subtitle, title, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);

    /* ---------- 中间登录卡片 ---------- */
    lv_obj_t *card = lv_obj_create(bg);
    lv_obj_set_size(card, 400, LV_SIZE_CONTENT);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 110);
    lv_obj_set_style_bg_color(card, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_shadow_width(card, 40, 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(card, 24, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    /* 纵向 flex 布局，子控件依次排列，间距统一 12px */
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 12, 0);
    lv_obj_set_style_pad_bottom(card, 20, 0);

    /* 账号标签 + 输入框 */
    lv_obj_t *user_label = lv_label_create(card);
    lv_label_set_text(user_label, "账号");
    lv_obj_set_style_text_font(user_label, FONT_CJK, 0);
    lv_obj_set_style_text_color(user_label, lv_color_hex(0x555555), 0);

    user_ta = lv_textarea_create(card);
    lv_textarea_set_one_line(user_ta, true);
    lv_textarea_set_placeholder_text(user_ta, "请输入账号");
    ta_add_style(user_ta);
    lv_obj_set_size(user_ta, lv_pct(100), 44);
    lv_obj_add_event_cb(user_ta, ta_event_cb, LV_EVENT_ALL, NULL);

    /* 密码标签 + 输入框 */
    lv_obj_t *pwd_label = lv_label_create(card);
    lv_label_set_text(pwd_label, "密码");
    lv_obj_set_style_text_font(pwd_label, FONT_CJK, 0);
    lv_obj_set_style_text_color(pwd_label, lv_color_hex(0x555555), 0);

    pwd_ta = lv_textarea_create(card);
    lv_textarea_set_one_line(pwd_ta, true);
    lv_textarea_set_password_mode(pwd_ta, true);
    lv_textarea_set_placeholder_text(pwd_ta, "请输入密码");
    ta_add_style(pwd_ta);
    lv_obj_set_size(pwd_ta, lv_pct(100), 44);
    lv_obj_add_event_cb(pwd_ta, ta_event_cb, LV_EVENT_ALL, NULL);

    /* 登录按钮 */
    lv_obj_t *btn = lv_btn_create(card);
    lv_obj_set_size(btn, lv_pct(100), 46);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2A5298), 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_add_event_cb(btn, login_btn_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "登  录");
    lv_obj_set_style_text_font(btn_label, FONT_CJK, 0);
    lv_obj_set_style_text_color(btn_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(btn_label);

    /* 登录结果提示 */
    msg_label = lv_label_create(card);
    lv_label_set_text(msg_label, "  ");  /* 占位，保持布局稳定 */
    lv_obj_set_style_text_font(msg_label, FONT_CJK, 0);
    lv_obj_set_style_text_align(msg_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(msg_label, lv_pct(100));

    /* ---------- 软键盘（默认隐藏） ---------- */
    kb = lv_keyboard_create(bg);
    lv_obj_set_size(kb, 1024, 240);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
}
