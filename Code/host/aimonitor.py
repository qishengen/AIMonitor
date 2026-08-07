"""AIMonitor 高层 API：play / show / stream / transmit / upload / list / delete / status / ping。

用法：
    import serial
    from protocol import Link, SerialTransport
    from aimonitor import AIMonitor

    mon = AIMonitor(Link(SerialTransport(serial.Serial("COM5", 921600, timeout=0))))
    print(mon.ping())
    mon.transmit("demo.anm", frames, fps=24)
    mon.play("demo.anm", loop=1)
"""
import zlib

from protocol import Link, NackError
import anm_builder


class AIMonitor:
    def __init__(self, link: Link):
        self.link = link

    def _req(self, obj, payload=None, enc="bin", expect=("ack",)):
        return self.link.request(obj, payload=payload, enc=enc, expect=expect)

    # ---------- 控制 / 管理 ----------

    def ping(self):
        return self._req({"cmd": "ping"}, expect=("pong",))

    def status(self):
        return self._req({"cmd": "status"}, expect=("state",))

    def stop(self):
        return self._req({"cmd": "stop"}, expect=("state", "ack"))

    def list(self):
        return self._req({"cmd": "list"}, expect=("list",))

    def delete(self, name):
        return self._req({"cmd": "delete", "name": name})

    def play(self, name, fps=0, loop=1):
        """fps=0 时使用 .anm 文件头存储值。"""
        return self._req(
            {"cmd": "play", "name": name, "fps": fps, "loop": loop},
            expect=("state", "ack"),
        )

    # ---------- 实时显示 ----------

    def show(self, frame, seq=0, b64=False):
        """立即直显一帧，无帧率/总数，完全独立。"""
        return self._req(
            {"cmd": "show", "seq": seq},
            payload=frame,
            enc="b64" if b64 else "bin",
        )

    def stream_start(self, fps=24, loop=0, total=None):
        obj = {"cmd": "stream_start", "fps": fps, "loop": loop}
        if total is not None:
            obj["total"] = total
        return self._req(obj, expect=("state", "ready"))

    def frame(self, frame, seq=0, b64=False):
        return self._req(
            {"cmd": "frame", "seq": seq},
            payload=frame,
            enc="b64" if b64 else "bin",
        )

    def stream_end(self):
        return self._req({"cmd": "stream_end"}, expect=("done", "ack"))

    def stream(self, frames, fps=24, loop=0, total=None, b64=False):
        """按生成器流式播放。每帧产出一个 ack 回包。"""
        self.stream_start(fps=fps, loop=loop, total=total)
        try:
            for i, f in enumerate(frames):
                resp = self.frame(f, seq=i, b64=b64)
                if resp.get("resp") == "nack":
                    raise NackError(resp.get("code"))
                yield resp
        finally:
            self.stream_end()

    # ---------- 写入保存 ----------

    def transmit(self, name, frames, fps=24, loop=1, b64=False):
        """整动画一次传入，仅保存到 LittleFS（不自动播放）。"""
        data = anm_builder.build_anm(frames, fps)
        return self._req(
            {"cmd": "transmit", "name": name, "fps": fps, "loop": loop, "size": len(data)},
            payload=data,
            enc="b64" if b64 else "bin",
            expect=("done",),
        )

    def upload(self, name, data, chunk=4096, b64=False, start_off=0):
        """分块上传 .anm（含 8 字节头），支持断点续传（start_off 指定续传起点）。"""
        size = len(data)
        resp = self._req(
            {"cmd": "up_begin", "name": name, "size": size}, expect=("ready",)
        )
        off = resp.get("off", start_off)
        while off < size:
            piece = data[off:off + chunk]
            self._req(
                {"cmd": "up_chunk", "off": off},
                payload=piece,
                enc="b64" if b64 else "bin",
            )
            off += len(piece)
        crc = zlib.crc32(data) & 0xFFFFFFFF
        return self._req({"cmd": "up_end", "crc": crc}, expect=("done",))

    def upload_crc(self, data):
        return zlib.crc32(data) & 0xFFFFFFFF
