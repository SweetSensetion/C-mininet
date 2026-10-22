#!/usr/bin/env python3
"""生成技术文档用的架构图（PNG）。

为什么要用图片而不是 ASCII art：
  中英文混排时，中文字符在大多数字体里是"双宽"，而代码块用的是比例/西文等宽字体，
  手工用空格对齐的方框在 Word/LibreOffice 里必然错位。图直接用绘图 API 排版，
  宽度由渲染器决定，不再依赖字符宽度假设。
"""
import os

from PIL import Image, ImageDraw, ImageFont

W, H = 1500, 880
BG = (255, 255, 255)

FONT_CN = r"C:\Windows\Fonts\msyh.ttc"
FONT_BOLD = r"C:\Windows\Fonts\msyhbd.ttc"

C_MAIN_FILL = (232, 241, 251)
C_MAIN_LINE = (47, 111, 176)
C_SUB_FILL = (244, 249, 254)
C_SUB_LINE = (74, 134, 200)
C_BOT_FILL = (255, 246, 232)
C_BOT_LINE = (201, 139, 58)
C_TEXT = (30, 30, 30)
C_ARROW = (90, 90, 90)


def font(size, bold=False):
    path = FONT_BOLD if bold else FONT_CN
    return ImageFont.truetype(path, size)


def text_size(draw, text, fnt):
    box = draw.textbbox((0, 0), text, font=fnt)
    return box[2] - box[0], box[3] - box[1]


def box(draw, x0, y0, x1, y1, lines, fill, line, title_size=34, body_size=26, radius=14):
    draw.rounded_rectangle([x0, y0, x1, y1], radius=radius, fill=fill, outline=line, width=3)
    tl = font(title_size, bold=True)
    bl = font(body_size)

    heights = []
    for idx, text in enumerate(lines):
        f = tl if idx == 0 else bl
        _, h = text_size(draw, text, f)
        heights.append(h)

    gap = 10
    total = sum(heights) + gap * (len(lines) - 1)
    cy = (y0 + y1) / 2 - total / 2

    for idx, text in enumerate(lines):
        f = tl if idx == 0 else bl
        tw, th = text_size(draw, text, f)
        draw.text(((x0 + x1) / 2 - tw / 2, cy), text, font=f, fill=C_TEXT)
        cy += th + gap


def arrow(draw, x0, y0, x1, y1, label=None, label_font=None, label_offset=(0, 0)):
    draw.line([x0, y0, x1, y1], fill=C_ARROW, width=3)
    # 箭头头部
    if abs(x1 - x0) > abs(y1 - y0):
        d = 1 if x1 > x0 else -1
        draw.polygon([(x1, y1), (x1 - 14 * d, y1 - 8), (x1 - 14 * d, y1 + 8)], fill=C_ARROW)
    else:
        d = 1 if y1 > y0 else -1
        draw.polygon([(x1, y1), (x1 - 8, y1 - 14 * d), (x1 + 8, y1 - 14 * d)], fill=C_ARROW)

    if label:
        f = label_font or font(22)
        tw, th = text_size(draw, label, f)
        lx = (x0 + x1) / 2 - tw / 2 + label_offset[0]
        ly = (y0 + y1) / 2 - th / 2 + label_offset[1]
        # 白底衬垫，避免文字压在线上
        draw.rectangle([lx - 6, ly - 4, lx + tw + 6, ly + th + 6], fill=BG)
        draw.text((lx, ly), label, font=f, fill=(80, 80, 80))


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_dir = os.path.join(root, "docs", "assets")
    os.makedirs(out_dir, exist_ok=True)

    img = Image.new("RGB", (W, H), BG)
    draw = ImageDraw.Draw(img)

    # 顶部：mainReactor
    box(draw, 470, 40, 1030, 190,
        ["mainReactor", "EventLoop + Acceptor(listen fd)", "唯一负责 accept，不处理业务"],
        C_MAIN_FILL, C_MAIN_LINE)

    # 中部：三个 subReactor
    sub_w, sub_h = 380, 190
    subs = [
        (60, 330, "subReactor 0"),
        (560, 330, "subReactor 1"),
        (1060, 330, "subReactor N"),
    ]
    for x, y, name in subs:
        box(draw, x, y, x + sub_w, y + sub_h,
            [name, "EventLoop + epoll_wait", "N × TcpConnection"],
            C_SUB_FILL, C_SUB_LINE, title_size=32, body_size=24)

    # main -> sub 的连线
    fan_y = 260
    draw.line([750, 190, 750, fan_y], fill=C_ARROW, width=3)
    xs = [60 + sub_w / 2, 560 + sub_w / 2, 1060 + sub_w / 2]
    draw.line([xs[0], fan_y, xs[2], fan_y], fill=C_ARROW, width=3)
    for x in xs:
        arrow(draw, x, fan_y, x, 330)

    f_small = font(22)
    tw, th = text_size(draw, "round-robin 分发 connfd", f_small)
    draw.rectangle([750 - tw / 2 - 8, fan_y - th - 18, 750 + tw / 2 + 8, fan_y - 6], fill=BG)
    draw.text((750 - tw / 2, fan_y - th - 14), "round-robin 分发 connfd", font=f_small, fill=(80, 80, 80))

    # 底部：线程池与定时器
    box(draw, 300, 640, 700, 800,
        ["ThreadPool", "业务计算 / 编解码", "有界队列 + 背压"],
        C_BOT_FILL, C_BOT_LINE, title_size=32, body_size=24)
    box(draw, 830, 640, 1230, 800,
        ["TimerQueue", "定时器 / 心跳清理", "最小堆 + epoll 超时"],
        C_BOT_FILL, C_BOT_LINE, title_size=32, body_size=24)

    # sub -> 底部模块（从中间 sub 底部引出，避免线条穿过其他模块）
    arrow(draw, 700, 520, 500, 640, "耗时任务投递", font(21), (-70, -6))
    arrow(draw, 800, 520, 1030, 640, "超时 / 心跳", font(21), (55, -6))

    path = os.path.join(out_dir, "architecture.png")
    img.save(path)
    print("saved:", path)

    make_frame(out_dir)


def make_frame(out_dir):
    """协议帧结构图：比手写 ASCII 位图可靠，也不会因为字体宽度而错位。"""
    W, H = 1300, 280
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)

    f_name = font(22)
    f_small = font(16)
    f_label = font(19)

    # (标题, 字节数文本, 宽度)
    fields = [
        ("magic", "4 B", 160),
        ("version", "1 B", 120),
        ("type", "1 B", 100),
        ("flags", "2 B", 120),
        ("seq", "4 B", 130),
        ("bodyLen", "4 B", 160),
        ("body", "bodyLen B", 300),
    ]
    gap = 8
    x = 30
    y0, y1 = 120, 220
    header_end = x

    for idx, (name, size, w) in enumerate(fields):
        is_body = name == "body"
        fill = C_BOT_FILL if is_body else C_MAIN_FILL
        line = C_BOT_LINE if is_body else C_MAIN_LINE
        if is_body:
            # 虚线表示变长
            d.rounded_rectangle([x, y0, x + w, y1], radius=8, fill=fill, outline=line, width=3)
            for dx in range(int(x) + 6, int(x + w) - 6, 14):
                d.line([dx, y0 + 2, dx + 6, y0 + 2], fill=line, width=2)
        else:
            d.rounded_rectangle([x, y0, x + w, y1], radius=8, fill=fill, outline=line, width=3)

        tw = d.textbbox((0, 0), name, font=f_name)[2]
        d.text((x + (w - tw) / 2, y0 + 26), name, font=f_name, fill=C_TEXT)
        tw2 = d.textbbox((0, 0), size, font=f_small)[2]
        d.text((x + (w - tw2) / 2, y0 + 62), size, font=f_small, fill=(110, 110, 110))

        if not is_body:
            header_end = x + w
        else:
            body_end = x + w
        x += w + gap

    # 顶部标注：固定头 / 变长体
    d.line([30, 88, header_end, 88], fill=(120, 120, 120), width=2)
    d.line([30, 82, 30, 94], fill=(120, 120, 120), width=2)
    d.line([header_end, 82, header_end, 94], fill=(120, 120, 120), width=2)
    label = "16 字节固定头"
    lw = d.textbbox((0, 0), label, font=f_label)[2]
    d.text(((30 + header_end) / 2 - lw / 2, 55), label, font=f_label, fill=(90, 90, 90))

    d.line([body_end - 300, 40, body_end, 40], fill=(120, 120, 120), width=2)
    label2 = "变长 body"
    lw2 = d.textbbox((0, 0), label2, font=f_label)[2]
    d.text((body_end - 300 + (300 - lw2) / 2, 12), label2, font=f_label, fill=(90, 90, 90))

    # 底部字节偏移
    offsets = [0, 4, 5, 6, 8, 12, 16]
    x = 30
    for idx, (name, size, w) in enumerate(fields[:-1]):
        txt = str(offsets[idx])
        tw3 = d.textbbox((0, 0), txt, font=f_small)[2]
        d.text((x - tw3 / 2 + 2, 236), txt, font=f_small, fill=(120, 120, 120))
        x += w + gap
    txt = "16"
    tw4 = d.textbbox((0, 0), txt, font=f_small)[2]
    d.text((header_end - tw4 / 2, 236), txt, font=f_small, fill=(120, 120, 120))

    path = os.path.join(out_dir, "protocol-frame.png")
    img.save(path)
    print("saved:", path)


if __name__ == "__main__":
    main()
