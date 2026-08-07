"""AI 联动示例：生成关键帧 -> 流式播放到设备。

未配置真实 AI 时使用内置参数化动画（移动竖条）占位，把 ai_generate_frames
替换为真实的文字/图片/AI 生成即可。

用法：
    python ai_stream.py COM5 --fps 24 --loop 3 --b64
"""
import argparse
import sys

from protocol import Link, SerialTransport, HAS_PYSERIAL
from aimonitor import AIMonitor


def ai_generate_frames(prompt=None):
    """占位 AI：返回若干 128x64 XBM 帧。可替换为真实 AI 调用。"""
    frames = []
    steps = 8
    for k in range(steps):
        b = bytearray(1024)
        x0 = 8 + k * 14
        for y in range(64):
            for x in range(x0, min(x0 + 8, 128)):
                b[y * 16 + x // 8] |= 1 << (x % 8)   # LSB：bit0 = 行最左像素
        frames.append(bytes(b))
    return frames


def open_link(port, baud):
    if not HAS_PYSERIAL:
        sys.exit("未安装 pyserial，请先 pip install pyserial")
    import serial
    return Link(SerialTransport(serial.Serial(port, baud, timeout=0)))


def main():
    ap = argparse.ArgumentParser(description="AI 联动流式播放")
    ap.add_argument("port", help="串口，如 COM5")
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--fps", type=int, default=24)
    ap.add_argument("--loop", type=int, default=1, help="循环播放次数")
    ap.add_argument("--b64", action="store_true", help="用 base64 传输帧")
    ap.add_argument("--prompt", default=None, help="传给 AI 的提示词")
    args = ap.parse_args()

    mon = AIMonitor(open_link(args.port, args.baud))
    print("ping:", mon.ping())

    for r in range(args.loop):
        frames = ai_generate_frames(args.prompt)
        n = 0
        for ack in mon.stream(frames, fps=args.fps, b64=args.b64):
            n += 1
        print("loop %d done, %d frames" % (r + 1, n))


if __name__ == "__main__":
    main()
