"""PXA 像素动画文本格式：git 友好、可直接预览、可 diff。

格式（v1）:
    ;PXA v1
    ;W=<宽> H=<高> F=<帧数> FPS=<帧率> LOOP=<0|1>
    ;NAME=<名字>
    ;---- FRAME 0 ----
    <宽 个字符的行, #=已绘制(黑) .=空白(白), 共 <高> 行>
    ...

帧数据用 XBM 字节数组(LSB 位序)表示，与 anm_builder 互转:
    .anm  <=>  xbm frames  <=>  pxa
"""
import re

MAGIC = ";PXA v1"


def xbm_to_rows(data, width, height):
    bpr = (width + 7) // 8
    rows = []
    for y in range(height):
        row = []
        for x in range(width):
            byte = data[y * bpr + x // 8]
            row.append("#" if (byte >> (x % 8)) & 1 else ".")
        rows.append("".join(row))
    return rows


def rows_to_xbm(rows, width, height):
    bpr = (width + 7) // 8
    out = bytearray(bpr * height)
    for y, row in enumerate(rows[:height]):
        for x, ch in enumerate(row[:width]):
            if ch == "#":
                out[y * bpr + x // 8] |= 1 << (x % 8)
    return bytes(out)


def to_pxa(frames, width, height, fps=12, loop=True, name="animation"):
    """frames: XBM 字节数组列表。返回 PXA 文本。"""
    if len(frames) == 0:
        raise ValueError("至少一帧")
    lines = [MAGIC,
             ";W=%d H=%d F=%d FPS=%d LOOP=%d" % (width, height, len(frames), fps, 1 if loop else 0),
             ";NAME=%s" % name]
    for i, f in enumerate(frames):
        lines.append(";---- FRAME %d ----" % i)
        lines.extend(xbm_to_rows(f, width, height))
    return "\n".join(lines)


def parse_pxa(text):
    """解析 PXA 文本 -> dict(width, height, fps, loop, name, frames=[XBM bytes])。"""
    version = None
    meta = {}
    cur = None
    frame_rows = []
    for raw in text.splitlines():
        line = raw.rstrip()
        if not line:
            continue
        if line.startswith(";"):
            if re.match(r"^;PXA\s+v?1\b", line, re.I):
                version = 1
                continue
            if line.startswith(";---- FRAME"):
                if cur is not None:
                    frame_rows.append(cur)
                cur = []
                continue
            body = line[1:]
            for kv in body.split():
                m = re.match(r"^([A-Za-z]+)=(.+)$", kv)
                if m:
                    meta[m.group(1).lower()] = m.group(2)
            continue
        if cur is None:
            cur = []
        cur.append(line)
    if cur is not None:
        frame_rows.append(cur)
    if version != 1:
        raise ValueError("不是 PXA v1 文件")
    W = int(meta.get("w", 0))
    H = int(meta.get("h", 0))
    if W < 1 or H < 1:
        raise ValueError("缺少或无效的尺寸")
    frames = [rows_to_xbm(rows, W, H) for rows in frame_rows]
    if not frames:
        raise ValueError("没有帧数据")
    return {
        "width": W,
        "height": H,
        "fps": int(meta.get("fps") or 12),
        "loop": meta.get("loop") == "1",
        "name": meta.get("name", ""),
        "frames": frames,
    }
