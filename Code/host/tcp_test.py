"""TCP 通路测试程序：连接 AIMonitor 固件 TCP server，跑协议命令验证。

用法：
    python tcp_test.py                      # 连接 aimonitor.local:8088
    python tcp_test.py 192.168.4.1          # 连接指定 IP
    python tcp_test.py 192.168.4.1 9000
    python tcp_test.py --sim                # 无硬件：本地 TCP 回环模拟器
    python tcp_test.py --upload demo.anm --frames 30   # 顺带上传 30 帧测试动画并播放

退出码 0 = 全部通过。
"""
import argparse
import os
import sys

HOST = os.path.dirname(os.path.abspath(__file__))
if HOST not in sys.path:
    sys.path.insert(0, HOST)
if os.path.join(HOST, "tests") not in sys.path:
    sys.path.insert(0, os.path.join(HOST, "tests"))

from protocol import connect_tcp, NackError, TimeoutError
from aimonitor import AIMonitor


def moving_box_frames(n=30):
    """生成 n 帧：一个 4x4 黑块在 y=24..27 从左向右移动（LSB 位序，可直接上屏验证）。"""
    W, H = 128, 64
    row_bytes = W // 8
    frames = []
    for i in range(n):
        buf = bytearray(W * H // 8)
        x = (i * 4) % (W - 4)
        for dy in range(4):
            for dx in range(4):
                col = x + dx
                buf[(24 + dy) * row_bytes + col // 8] |= 1 << (col % 8)
        frames.append(bytes(buf))
    return frames


def _assert_ok(resp, want):
    assert resp.get("resp") == want, resp


def _run(name, fn):
    try:
        fn()
        print("PASS  %s" % name)
        return True
    except (NackError, TimeoutError, ConnectionError, OSError, AssertionError) as e:
        print("FAIL  %s: %s" % (name, e))
        return False


def main():
    ap = argparse.ArgumentParser(description="AIMonitor TCP 通路测试")
    ap.add_argument("host", nargs="?", default="aimonitor.local")
    ap.add_argument("port", nargs="?", type=int, default=8088)
    ap.add_argument("--sim", action="store_true", help="对本地 TCP 回环模拟器测试（无硬件）")
    ap.add_argument("--upload", metavar="NAME", default=None, help="上传一个测试动画并播放")
    ap.add_argument("--frames", type=int, default=30)
    ap.add_argument("--fps", type=int, default=24)
    ap.add_argument("--timeout", type=float, default=3.0)
    args = ap.parse_args()

    srv = None
    if args.sim:
        from sim_firmware import TcpSimServer
        srv = TcpSimServer()
        srv.start()
        host, port = srv.addr()
        print("== 回环模拟 TCP server %s:%d" % (host, port))
    else:
        host, port = args.host, args.port

    try:
        link = connect_tcp(host, port, timeout=args.timeout)
    except OSError as e:
        print("连接失败 %s:%d: %s" % (host, port, e))
        return 1
    mon = AIMonitor(link)
    results = []

    def check(name, fn):
        results.append(_run(name, fn))

    check("ping", lambda: _assert_ok(mon.ping(), "pong"))
    check("wifi_status", lambda: _assert_ok(
        mon._req({"cmd": "wifi_status"}, expect=("wifi",)), "wifi"))
    check("list", lambda: _assert_ok(mon.list(), "list"))

    if args.upload:
        name = args.upload if args.upload.endswith(".anm") else args.upload + ".anm"

        def do_transmit():
            _assert_ok(mon.transmit(name, moving_box_frames(args.frames), fps=args.fps), "done")
        check("transmit %s" % name, do_transmit)

        def do_list_has():
            lst = mon.list()
            assert name in lst.get("anims", [])
        check("list 含 %s" % name, do_list_has)

        def do_play():
            r = mon.play(name, fps=args.fps, loop=1)
            assert r.get("resp") == "state" and r.get("state") == "play"
        check("play %s" % name, do_play)

        def do_status_play():
            st = mon.status()
            assert st.get("state") == "play" and st.get("name") == name
        check("status 为 play", do_status_play)

    def do_show():
        r = mon.show(moving_box_frames(1)[0], seq=99)
        assert r.get("resp") == "ack"
    check("show 直显一帧", do_show)

    def do_stream():
        r = mon.stream_start(fps=args.fps, total=3)
        assert r.get("resp") == "state" and r.get("state") == "stream"
        for i, f in enumerate(moving_box_frames(3)):
            assert mon.frame(f, seq=i).get("resp") == "ack"
        r = mon.stream_end()
        assert r.get("resp") == "done"
    check("stream 3 帧 + stream_end", do_stream)

    def do_stop():
        st = mon.stop()
        assert st.get("resp") == "state" and st.get("state") == "idle"
    check("stop 回 idle", do_stop)

    link._t.close()
    if srv:
        srv.stop()

    passed = sum(results)
    total = len(results)
    print("\n%d/%d passed" % (passed, total))
    return 0 if passed == total else 1


if __name__ == "__main__":
    sys.exit(main())
