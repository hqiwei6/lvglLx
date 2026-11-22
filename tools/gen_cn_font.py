#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_cn_font.py —— LVGL 中文点阵字库生成（Pillow 渲染, 4bpp 无压缩）

用法:
    python3 tools/gen_cn_font.py

输出:
    ui/fonts/lv_font_cn_16.c / ui/fonts/lv_font_cn_20.c

需要扩充界面文字时, 改下面 FONT_CHARS 再跑一遍即可。
"""
import os
from PIL import Image, ImageFont

# 项目根目录 (脚本位于 tools/ 下)
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "ui", "fonts")

# 字体源文件 (TTC 里选 SC 简体中文字面)
FONT_CANDIDATES = [
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Medium.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc",
]

# 界面用到的全部中文 (ASCII 由字体结构自动覆盖, 不用列)
FONT_CHARS = (
    "导航级别放大缩小地图路线返回主页设置娱乐媒体中心距离分钟到达米公里"
    "左右北南东西方向信号蓝牙电量天气温度风速档位车速播放暂停上一下首列表"
    "音量亮度开关确认取消保存启动关闭状态冷热座椅除雾湿度今天东南预计剩余"
    "目的地搜索定位刷新收藏循环单曲随机恭喜欢迎请输入账号密码成功正在进"
    "系统错误重新使用管理账户编辑删除添加计算器闹钟时间日期星期图片视频"
    "电话连接断开在线离线提示警告信息"
)

def pick_font():
    from PIL import ImageFont
    for path in FONT_CANDIDATES:
        if not os.path.exists(path):
            continue
        for idx in range(5):
            try:
                f = ImageFont.truetype(path, 16, index=idx)
                name = f.getname()
                if "SC" in name[0]:
                    print(f"使用字体: {path} index={idx} {name}")
                    return path, idx
            except Exception:
                break
    raise SystemExit("找不到含简体中文 (SC) 的 Noto Sans CJK 字体")

def gen_font(px, font_path, font_idx, chars, out_path, var_name):
    from PIL import ImageFont
    font = ImageFont.truetype(font_path, px, index=font_idx)
    ascent, descent = font.getmetrics()
    line_height = ascent + descent
    base_line = descent

    # ---------- ASCII (32..126) 固定占 gid 1..95 ----------
    ascii_glyphs = []
    for code in range(32, 127):
        ch = chr(code)
        g = render_glyph(font, ch, px)
        ascii_glyphs.append(g)

    # ---------- CJK 稀疏表: unicode 相对 0x2000 的偏移 ----------
    uniq = sorted(set(chars) - set(chr(c) for c in range(32, 127)))
    cjk = []
    for ch in uniq:
        cp = ord(ch)
        if cp < 0x2000 or cp > 0xFFFF:
            print(f"跳过超范围字符: {ch} U+{cp:04X}")
            continue
        g = render_glyph(font, ch, px)
        g["ofs"] = cp - 0x2000
        cjk.append(g)

    # ---------- 打包位图 ----------
    bitmap = bytearray()
    entries = [dict(bitmap_index=0, adv_w=0, box_w=0, box_h=0, ofs_x=0, ofs_y=0)]  # gid 0
    for g in ascii_glyphs + cjk:
        g["bitmap_index"] = len(bitmap)
        bitmap += g["data"]
        entries.append({k: g[k] for k in
                        ("bitmap_index", "adv_w", "box_w", "box_h", "ofs_x", "ofs_y")})

    # ---------- 生成 C ----------
    def fmt_entry(e, comment=""):
        c = (f'    {{.bitmap_index = {e["bitmap_index"]}, .adv_w = {e["adv_w"]}, '
             f'.box_w = {e["box_w"]}, .box_h = {e["box_h"]}, '
             f'.ofs_x = {e["ofs_x"]}, .ofs_y = {e["ofs_y"]}}},{comment}')
        return c

    lines = []
    lines.append("/**")
    lines.append(f" * @file {os.path.basename(out_path)}")
    lines.append(" * @brief 自动生成的 LVGL 中文字体（Noto Sans CJK SC，4bpp）")
    lines.append(" * 生成工具: tools/gen_cn_font.py（Pillow 渲染，无压缩）")
    lines.append(f" * 字符数: ASCII 95 + 中文 {len(cjk)}")
    lines.append(" */")
    lines.append("")
    lines.append('#include "lvgl/lvgl.h"')
    lines.append("")
    lines.append("#if LV_USE_FONT_COMPRESSED || 1")
    lines.append("")
    lines.append("/* 字位点阵数据 */")
    lines.append("static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {")
    for i in range(0, len(bitmap), 16):
        chunk = bitmap[i:i + 16]
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
    lines.append("};")
    lines.append("")
    lines.append("/* 字形描述: bitmap_index, adv_w(8.4), box_w, box_h, ofs_x, ofs_y */")
    lines.append("static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {")
    lines.append(fmt_entry(entries[0], " /* gid 0: 未找到 */"))
    for e in entries[1:]:
        lines.append(fmt_entry(e))
    lines.append("};")
    lines.append("")
    lines.append("/* CJK 字符的 unicode 相对偏移表（相对 0x2000） */")
    lines.append("static const uint16_t unicode_list_1[] = {")
    offs = [g["ofs"] for g in cjk]
    for i in range(0, len(offs), 8):
        chunk = offs[i:i + 8]
        tail = "," if i + 8 < len(offs) else ""
        lines.append("    " + ", ".join(f"0x{o:04x}" for o in chunk) + tail)
    lines.append("};")
    lines.append("")
    lines.append("static const lv_font_fmt_txt_cmap_t cmaps[] = {")
    lines.append("    {")
    lines.append("        .range_start = 32, .range_length = 95, .glyph_id_start = 1,")
    lines.append("        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY,")
    lines.append("    },")
    lines.append("    {")
    lines.append("        .range_start = 0x2000, .range_length = 65535 - 0x2000, .glyph_id_start = 96,")
    lines.append(f"        .unicode_list = unicode_list_1, .glyph_id_ofs_list = NULL, .list_length = {len(cjk)}, .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY,")
    lines.append("    },")
    lines.append("};")
    lines.append("")
    lines.append("static lv_font_fmt_txt_glyph_cache_t cache;")
    lines.append("static const lv_font_fmt_txt_dsc_t font_dsc = {")
    lines.append("    .glyph_bitmap = glyph_bitmap,")
    lines.append("    .glyph_dsc = glyph_dsc,")
    lines.append("    .cmaps = cmaps,")
    lines.append("    .kern_dsc = NULL,")
    lines.append("    .kern_scale = 0,")
    lines.append("    .cmap_num = 2,")
    lines.append("    .bpp = 4,")
    lines.append("    .kern_classes = 0,")
    lines.append("    .bitmap_format = 0,")
    lines.append("    .cache = &cache,")
    lines.append("};")
    lines.append("")
    lines.append(f"const lv_font_t {var_name} = {{")
    lines.append("    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,")
    lines.append("    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,")
    lines.append(f"    .line_height = {line_height},")
    lines.append(f"    .base_line = {base_line},")
    lines.append("    .subpx = LV_FONT_SUBPX_NONE,")
    lines.append(f"    .underline_position = {-max(2, descent - 2)},")
    lines.append("    .underline_thickness = 1,")
    lines.append("    .dsc = &font_dsc,")
    lines.append("};")
    lines.append("")
    lines.append("#endif")
    lines.append("")

    with open(out_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print(f"生成 {out_path}: line_height={line_height} base_line={base_line} "
          f"位图 {len(bitmap)} 字节, 中文 {len(cjk)} 字")

def render_glyph(font, ch, px):
    """渲染单字符 → 4bpp 位图 + 字形度量

    注意: LVGL 解码时位图是按位连续打包的 (见 lv_draw_sw_letter.c:
    width_bit = box_w * bpp, bit_ofs = row*width_bit + col*bpp),
    行末不做字节对齐! 奇数宽度字形必须连续打包, 否则错乱。
    """
    adv = int(round(font.getlength(ch) * 16))  # 8.4 定点
    gb = font.getbbox(ch)                      # 行框坐标 (x0,y0,x1,y1)
    mask = font.getmask(ch, mode="L")
    mb = mask.getbbox()

    if mb is None:                           # 空白字符(如空格): LVGL 不支持 0x0 字形, 给 1x1 空位图
        return dict(adv_w=adv, box_w=1, box_h=1, ofs_x=0, ofs_y=0,
                    data=b"\x00", bitmap_index=0)

    mx0, my0, mx1, my1 = mb
    w, h = mx1 - mx0, my1 - my0
    ofs_x = mx0
    # getmask 返回的位图垂直方向是紧贴墨迹的, 行框内垂直位置要从 getbbox 取
    ofs_y = gb[1] if (my1 - my0) == (gb[3] - gb[1]) else my0

    bits = bytearray()
    for row in range(h):
        for col in range(w):
            a = mask.getpixel((mx0 + col, my0 + row))
            bits.append(a >> 4)              # 0..15
    data = bytearray()
    for i in range(0, len(bits), 2):         # 连续位流: 高半字节在前, 不按行对齐
        hi = bits[i]
        lo = bits[i + 1] if i + 1 < len(bits) else 0
        data.append((hi << 4) | lo)
    return dict(adv_w=adv, box_w=w, box_h=h, ofs_x=ofs_x, ofs_y=ofs_y,
                data=bytes(data), bitmap_index=0)

if __name__ == "__main__":
    path, idx = pick_font()
    os.makedirs(OUT_DIR, exist_ok=True)
    gen_font(16, path, idx, FONT_CHARS,
             os.path.join(OUT_DIR, "lv_font_cn_16.c"), "lv_font_cn_16")
    gen_font(20, path, idx, FONT_CHARS,
             os.path.join(OUT_DIR, "lv_font_cn_20.c"), "lv_font_cn_20")
