#!/usr/bin/env python3
"""生成简体拼音词典 C 源 (ui/map_view/pinyin_dict.c)

背景: LVGL lv_ime_pinyin 的内置词典混有大量繁体/日体汉字 (產/車/広...),
而项目字库是 GB2312 一级简体, 候选词会显示豆腐块。
本脚本用 pypinyin 对字库字符集逐字标注拼音, 按拼音分组生成纯简体词典:
  - 字符集 = gen_cn_font.FONT_CHARS (UI 用字 + GB2312 一级 3755 字)
  - 每拼音给出全部候选字(组件按每页 6 字翻页显示, 截断会导致无法翻页)
  - 拼音按字典序排列 (首字母分块升序, 满足组件的 init_pinyin_dict 索引要求)
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_cn_font import FONT_CHARS            # noqa: E402
from pypinyin import lazy_pinyin              # noqa: E402

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "..", "ui", "map_view", "pinyin_dict.c")

# 高频字优先表(通用字频 + 地名/导航常用字): 组内这些字排前面, 其余按 GB2312
# 区位序(=笔画序)接后。GB2312 区位序不反映使用频率, 不重排的话 jing 音节
# 第一页会是"茎荆兢"而"京经景"要翻好几页才见得到。
HOT = (
    "的一是了我不人在他有这上们来到时大地为子中你说生国年着就那和要她出也"
    "得里后自以会家可下而过天去能对小多然于心学么之都好看起发当没成只如事"
    "把还用第样道想作种开美总从无情己面最女但现前些所同日手又行意动方期它"
    "头经长儿回位分爱老因很给名法间斯知世什两次使身者被高已亲其进此话常与"
    "活正感见明问力理尔点文几定本公特做外孩相西果走将月十实向声车全信重三"
    "机工物气每并别真打太新比才便夫再书部水像眼等体却加电主界门利海受听表"
    "德少克代员许先口由死安写性马光白或住难望教命花结乐色更拉东神记处让母"
    "父应直字场平友放立报南必关反求助战任质建判认条边至论变"
    # 地名/导航场景
    "京沪津渝北南东西城乡村镇路街桥站场广场公园馆店楼大厦中心医院学校银行"
    "超市饭店宾馆酒店加油站停车场出口入口地图导航定位终点起点行驶转弯直行"
    "掉头环岛高速国道省道县道公里米方向目标景精警静镜井境敬惊晶睛颈经"
)


def main():
    groups = {}
    for ch in dict.fromkeys(FONT_CHARS):      # 去重保序(UI 字与一级字表有重叠)
        if ord(ch) < 0x2E80:                  # 只处理 CJK 字符
            continue
        py = lazy_pinyin(ch, errors="ignore")
        if not py or not py[0].isascii() or not py[0].isalpha():
            continue                          # 无拼音的字跳过
        groups.setdefault(py[0], []).append(ch)

    cands = []
    hot_set = set(HOT)
    for py in sorted(groups):
        chars = groups[py]
        hot = [c for c in HOT if c in chars and c in hot_set]  # 高频字打头(保持 HOT 序)
        rest = [c for c in chars if c not in hot_set]          # 其余按区位序
        mb = "".join(hot + rest)
        if mb:
            cands.append((py, mb))

    lines = [
        "/*",
        " * @file pinyin_dict.c",
        " * @brief 简体拼音词典 — 自动生成(tools/gen_pinyin_dict.py), 勿手改",
        f" * 拼音 {len(cands)} 个, 字符集=项目中文字库(GB2312 一级)",
        " */",
        '#include "lvgl/src/extra/others/ime/lv_ime_pinyin.h"',
        "",
        "const lv_pinyin_dict_t pinyin_dict_zh[] = {",
    ]
    for py, mb in cands:
        lines.append(f'    {{"{py}", "{mb}"}},')
    lines += ["    {NULL, NULL},", "};", ""]

    with open(OUT, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print(f"生成 {os.path.normpath(OUT)}: 拼音 {len(cands)} 个, "
          f"候选 {sum(len(m) for _, m in cands)} 字")


if __name__ == "__main__":
    main()
