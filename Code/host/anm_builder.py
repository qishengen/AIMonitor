"""anm_builder：文字 / 图片 / GIF / AI -> .anm 动画文件。

.anm 格式（与固件一致）：
    [0:4]  "ANM0" 魔数
    [4]    fps
    [5:7]  frame_count 小端 uint16
    [7]    保留
    [8:]   每帧 1024B 裸 XBM(128x64)，顺序拼接
"""
import struct

FRAME_BYTES = 1024
WIDTH = 128
HEIGHT = 64


def build_anm(frames, fps=24):
    """frames: 单个 bytes(len 为 1024 的倍数) 或 bytes 列表。返回完整 .anm 字节。"""
    if isinstance(frames, (bytes, bytearray)):
        raw = bytes(frames)
    else:
        raw = b"".join(bytes(f) for f in frames)
    n = len(raw) // FRAME_BYTES
    if len(raw) != n * FRAME_BYTES:
        raise ValueError("frame data length must be multiple of %d" % FRAME_BYTES)
    if n == 0 or n > 0xFFFF:
        raise ValueError("frame count out of range (1..65535)")
    hdr = b"ANM0" + bytes([fps & 0xFF, n & 0xFF, (n >> 8) & 0xFF, 0])
    return hdr + raw


def parse_anm(data):
    """解析 .anm -> (fps, frames列表)。"""
    if len(data) < 8 or data[:4] != b"ANM0":
        raise ValueError("bad .anm magic")
    fps = data[4]
    n = struct.unpack("<H", data[5:7])[0]
    raw = data[8:]
    if len(raw) != n * FRAME_BYTES:
        raise ValueError("bad .anm payload length")
    return fps, [raw[i * FRAME_BYTES:(i + 1) * FRAME_BYTES] for i in range(n)]


def write_anm(path, frames, fps=24):
    with open(path, "wb") as f:
        f.write(build_anm(frames, fps))


# ---------------- 帧转换（需要 Pillow） ----------------

def to_xbm(im):
    """PIL 图像(128x64) -> 1024B XBM(位1=黑色，bit0=行最左，LSB 位序，与设备显示一致)。"""
    im = im.convert("L")
    w, h = im.size
    if w != WIDTH or h != HEIGHT:
        raise ValueError("image must be %dx%d" % (WIDTH, HEIGHT))
    px = im.load()
    out = bytearray(WIDTH * HEIGHT // 8)
    for y in range(HEIGHT):
        row = y * (WIDTH // 8)
        for x in range(WIDTH):
            if px[x, y] < 128:
                out[row + x // 8] |= 1 << (x % 8)
    return bytes(out)


def _contain_resize(im):
    from PIL import Image
    w, h = im.size
    scale = min(WIDTH / w, HEIGHT / h)
    nw, nh = max(1, int(w * scale)), max(1, int(h * scale))
    im = im.resize((nw, nh), Image.LANCZOS)
    canvas = Image.new("L", (WIDTH, HEIGHT), 255)
    canvas.paste(im, ((WIDTH - nw) // 2, (HEIGHT - nh) // 2))
    return canvas


def frames_from_image(path, contain=True):
    """图片 -> 单帧列表。contain=True 等比缩放留白，否则拉伸铺满。"""
    from PIL import Image
    im = Image.open(path)
    if contain:
        im = _contain_resize(im)
    else:
        im = im.resize((WIDTH, HEIGHT), Image.LANCZOS)
    return [to_xbm(im)]


def frames_from_gif(path):
    """GIF -> 帧列表。"""
    from PIL import Image
    im = Image.open(path)
    frames = []
    try:
        while True:
            frames.append(to_xbm(im.convert("RGB")))
            im.seek(im.tell() + 1)
    except EOFError:
        pass
    return frames


def frames_from_text(text, font_path=None, font_size=24, color=0):
    """多行文字 -> 帧列表。color=0 黑字(白底)。未装 Pillow 时抛 ImportError。"""
    from PIL import Image, ImageDraw, ImageFont
    im = Image.new("L", (WIDTH, HEIGHT), 255)
    d = ImageDraw.Draw(im)
    try:
        font = ImageFont.truetype(font_path, font_size) if font_path else ImageFont.load_default()
    except Exception:
        font = ImageFont.load_default()
    lines = text.split("\n")
    pad = 6
    line_h = font_size + 4 if font_path else 12
    y = max(0, (HEIGHT - line_h * len(lines)) // 2)
    for ln in lines:
        bbox = d.textbbox((0, 0), ln, font=font)
        w = bbox[2] - bbox[0]
        d.text(((WIDTH - w) // 2, y), ln, font=font, fill=color)
        y += line_h
    return [to_xbm(im)]


def blank_frame():
    return bytes(FRAME_BYTES)
