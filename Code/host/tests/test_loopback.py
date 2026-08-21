"""PC 回环自测：模拟固件 + 上位机全指令收发与 CRC。可直接 `python test_loopback.py` 运行。"""
import os
import sys
import time
import zlib

HOST = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, HOST)
sys.path.insert(0, os.path.join(HOST, "tests"))

from protocol import Link, NackError, encode_packet
from aimonitor import AIMonitor
import anm_builder
from sim_firmware import SimFirmware


class SimLink:
    """伪传输：把主机发包注入模拟固件，读回包时驱动固件执行。"""

    def __init__(self, sim):
        self.sim = sim

    def send(self, data):
        self.sim.inject(data)

    def readline(self, deadline):
        while True:
            line = self.sim.pop_response_line()
            if line is not None:
                return line
            self.sim.run_pending()
            if time.time() > deadline:
                return None
            time.sleep(0.0002)


def make_frame(seed):
    return bytes([seed & 0xFF] * anm_builder.FRAME_BYTES)


def new_pair():
    sim = SimFirmware()
    link = Link(SimLink(sim), timeout=2.0)
    mon = AIMonitor(link)
    return sim, link, mon


def _run(name, fn):
    try:
        fn()
        print("PASS  %s" % name)
        return True
    except Exception as e:
        print("FAIL  %s: %r" % (name, e))
        return False


# ---------------- 用例 ----------------

def test_encode_packet():
    obj = {"cmd": "show", "seq": 3}
    frame = make_frame(7)
    head, payload = encode_packet(obj, payload=frame, enc="bin").split(b"\n", 1)
    assert b'"cmd":"show"' in head and b'"size":1024' in head
    assert payload == frame

    one = encode_packet(obj, payload=frame, enc="b64")
    assert one.count(b"\n") == 1 and b'"enc":"b64"' in one and b'"data":"' in one


def test_ping():
    sim, link, mon = new_pair()
    r = mon.ping()
    assert r["resp"] == "pong" and r["proto"] == 1 and r["fw"] == "1.3.0"


def test_home_and_reset_cmds():
    sim, link, mon = new_pair()
    r = mon._req({"cmd": "home"}, expect=("state",))
    assert r["resp"] == "state" and r["state"] == "home"
    assert mon._req({"cmd": "wifi_reconnect"}).get("resp") == "ack"
    assert mon._req({"cmd": "reset"}).get("resp") == "ack"
    assert mon._req({"cmd": "factory"}).get("resp") == "ack"


def test_ap_pass_cmd():
    sim, link, mon = new_pair()
    # 留空 = 开放热点
    assert mon._req({"cmd": "ap_pass", "pass": ""}).get("resp") == "ack"
    # ≥8 位 = 设置 WPA2 密码
    assert mon._req({"cmd": "ap_pass", "pass": "12345678"}).get("resp") == "ack"
    # 1~7 位非法
    r = mon._req({"cmd": "ap_pass", "pass": "short"}, expect=("nack",))
    assert r.get("code") == 2


def test_unknown_cmd():
    sim, link, mon = new_pair()
    try:
        mon._req({"cmd": "bogus"})
        assert False, "should raise"
    except NackError as e:
        assert e.code == 1


def test_transmit_list_play_stop():
    sim, link, mon = new_pair()
    frames = [make_frame(i) for i in range(1, 61)]
    data = anm_builder.build_anm(frames, 24)
    r = mon.transmit("demo.anm", frames, fps=24)
    assert r["resp"] == "done"
    assert sim.fs["demo.anm"] == data  # 帧数据端到端一致
    # 落盘文件必须是合法 .anm（含唯一 8B 头），能被 parse_anm 解析
    fps_stored, frames_stored = anm_builder.parse_anm(bytes(sim.fs["demo.anm"]))
    assert fps_stored == 24 and frames_stored == frames

    lst = mon.list()
    assert lst["resp"] == "list" and "demo.anm" in lst["anims"]

    r = mon.play("demo.anm", loop=1)
    assert r["resp"] == "state" and r["state"] == "play"
    assert mon.status()["state"] == "play"
    assert mon.stop()["state"] == "idle"


def test_play_missing():
    sim, link, mon = new_pair()
    try:
        mon.play("nope.anm")
        assert False, "should raise"
    except NackError as e:
        assert e.code == 3


def test_show_bin_and_b64():
    sim, link, mon = new_pair()
    f1 = make_frame(10)
    f2 = make_frame(20)
    assert mon.show(f1).get("resp") == "ack"
    assert mon.show(f2, b64=True).get("resp") == "ack"
    assert sim.received_frames == [f1, f2]


def test_stream_total():
    sim, link, mon = new_pair()
    frames = [make_frame(30 + i) for i in range(12)]
    count = [0]
    for ack in mon.stream(frames, fps=24, total=len(frames)):
        assert ack["resp"] == "ack"
        count[0] += 1
    assert count[0] == len(frames)
    assert sim.received_frames == frames


def test_stream_end_explicit():
    sim, link, mon = new_pair()
    r = mon.stream_start(fps=24)
    assert r["resp"] == "state" and r["state"] == "stream"
    assert mon.frame(make_frame(5)).get("resp") == "ack"
    assert mon.stream_end().get("resp") == "done"
    assert sim.stream_state is None


def test_upload_chunked_and_crc():
    sim, link, mon = new_pair()
    frames = [make_frame(100 + i) for i in range(20)]
    data = anm_builder.build_anm(frames, 30)
    r = mon.upload("up.anm", data, chunk=3000)
    assert r["resp"] == "done"
    assert bytes(sim.fs["up.anm"]) == data
    fps_stored, frames_stored = anm_builder.parse_anm(bytes(sim.fs["up.anm"]))
    assert frames_stored == frames


def test_upload_crc_mismatch():
    sim, link, mon = new_pair()
    frames = [make_frame(1), make_frame(2)]
    data = anm_builder.build_anm(frames, 24)
    mon._req({"cmd": "up_begin", "name": "bad.anm", "size": len(data)}, expect=("ready",))
    for off in range(0, len(data), 2000):
        mon._req({"cmd": "up_chunk", "off": off}, payload=data[off:off + 2000])
    try:
        mon._req({"cmd": "up_end", "crc": mon.upload_crc(data) + 1}, expect=("done",))
        assert False, "should raise"
    except NackError as e:
        assert e.code == 4


def test_upload_resume():
    sim, link, mon = new_pair()
    frames = [make_frame(200 + i) for i in range(10)]
    data = anm_builder.build_anm(frames, 24)
    chunk = 1500

    # 第一段：up_begin + 首个 chunk（模拟中途断线，未 up_end）
    r = mon._req({"cmd": "up_begin", "name": "res.anm", "size": len(data)}, expect=("ready",))
    assert r["off"] == 0
    mon._req({"cmd": "up_chunk", "off": 0}, payload=data[:chunk])
    assert sim.upload_progress["res.anm"] == chunk

    # 第二段：重新 up_begin，应回报已写字节数，随后从 off 续传并 up_end
    r = mon._req({"cmd": "up_begin", "name": "res.anm", "size": len(data)}, expect=("ready",))
    assert r["off"] == chunk
    off = r["off"]
    while off < len(data):
        piece = data[off:off + chunk]
        mon._req({"cmd": "up_chunk", "off": off}, payload=piece)
        off += len(piece)
    crc = zlib.crc32(data) & 0xFFFFFFFF
    assert mon._req({"cmd": "up_end", "crc": crc}, expect=("done",))["resp"] == "done"
    assert bytes(sim.fs["res.anm"]) == data


def test_delete_missing():
    sim, link, mon = new_pair()
    try:
        mon.delete("ghost.anm")
        assert False, "should raise"
    except NackError as e:
        assert e.code == 3


def test_ignore_interleaved_progress():
    sim, link, mon = new_pair()
    sim.progress_before = True
    frames = [make_frame(9)]
    mon.transmit("p.anm", frames, fps=24)
    r = mon.play("p.anm", loop=1)
    assert r["resp"] == "state"
    # play 后立刻 show：期间 sim 会先回 progress 再回 ack，request 应自动忽略 progress
    r = mon.show(make_frame(42))
    assert r["resp"] == "ack"


def main():
    tests = [
        ("encode_packet", test_encode_packet),
        ("ping", test_ping),
        ("home/reset/factory/wifi_reconnect", test_home_and_reset_cmds),
        ("ap_pass set/clear/invalid", test_ap_pass_cmd),
        ("unknown_cmd", test_unknown_cmd),
        ("transmit+list+play+stop", test_transmit_list_play_stop),
        ("play_missing", test_play_missing),
        ("show bin/b64", test_show_bin_and_b64),
        ("stream total", test_stream_total),
        ("stream_end explicit", test_stream_end_explicit),
        ("upload chunked + CRC", test_upload_chunked_and_crc),
        ("upload CRC mismatch -> nack4", test_upload_crc_mismatch),
        ("upload resume", test_upload_resume),
        ("delete missing -> nack3", test_delete_missing),
        ("ignore interleaved progress", test_ignore_interleaved_progress),
    ]
    passed = sum(1 for n, fn in tests if _run(n, fn))
    print("\n%d/%d passed" % (passed, len(tests)))
    return 0 if passed == len(tests) else 1


if __name__ == "__main__":
    sys.exit(main())
