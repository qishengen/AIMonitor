"""固件模拟器：在 Python 内复刻固件的协议行为，用于无硬件回环验证上位机。

与 protocol.py 的 SimLink 配合：主机发包 -> sim 解析 -> 产生回包队列。
"""
import base64
import json
import zlib

FRAME_BYTES = 1024
DATA_CMDS = {"show", "frame", "up_chunk", "transmit"}


class SimFirmware:
    def __init__(self):
        self.inbuf = bytearray()
        self.resp = []  # 待主机读取的回包行（含 \\n）
        self.fs = {}  # name -> .anm bytes
        self.upload_progress = {}  # name -> 连续已写字节数
        self.upload = None
        self.play_state = None
        self.stream_state = None
        self.active_name = None
        self.seq = 0
        self.last_frame = None
        self.received_frames = []
        self.progress_before = False  # 用于测试：show 回包前先插一条 progress

        self.pending_obj = None
        self.pending_got = 0

    # ---------- 传输 ----------

    def inject(self, data):
        self.inbuf.extend(data)

    def pop_response_line(self):
        return self.resp.pop(0) if self.resp else None

    def _send(self, obj):
        self.resp.append((json.dumps(obj, separators=(",", ":")) + "\n").encode("utf-8"))

    def run_pending(self):
        """处理缓冲中的一整个包。"""
        if self.pending_obj is None:
            nl = self.inbuf.find(b"\n")
            if nl < 0:
                return
            header = bytes(self.inbuf[:nl])
            del self.inbuf[:nl + 1]
            try:
                obj = json.loads(header.decode("utf-8"))
            except Exception:
                self._send({"resp": "nack", "code": 2})
                return
            if obj.get("enc") == "b64":
                try:
                    payload = base64.b64decode(obj.get("data", ""))
                except Exception:
                    self._send({"resp": "nack", "code": 2})
                    return
                self._handle(obj, payload)
                return
            if obj.get("cmd") in DATA_CMDS and "size" in obj:
                self.pending_obj = obj
                self.pending_got = 0
            else:
                self._handle(obj, None)
        else:
            obj = self.pending_obj
            need = obj["size"] - self.pending_got
            if len(self.inbuf) >= need:
                payload = bytes(self.inbuf[:need])
                del self.inbuf[:need]
                self.pending_obj = None
                self._handle(obj, payload)

    # ---------- 命令处理 ----------

    def _handle(self, obj, payload):
        cmd = obj.get("cmd")
        if cmd == "ping":
            self._send({
                "resp": "pong", "fw": "1.0.0", "proto": 1,
                "fs_total": 2981888, "fs_free": 2800000,
            })
        elif cmd == "status":
            st = self.play_state or self.stream_state or "idle"
            self._send({
                "resp": "state", "state": st,
                "name": self.active_name or "", "seq": self.seq,
            })
        elif cmd == "stop":
            self.play_state = None
            self.stream_state = None
            self.active_name = None
            self.seq = 0
            self._send({"resp": "state", "state": "idle", "name": "", "seq": 0})
        elif cmd == "list":
            names = sorted(n for n in self.fs if n.endswith(".anm"))
            self._send({"resp": "list", "anims": names})
        elif cmd == "delete":
            name = obj.get("name")
            if name in self.fs:
                del self.fs[name]
                self._send({"resp": "ack"})
            else:
                self._send({"resp": "nack", "code": 3})
        elif cmd == "play":
            name = obj.get("name")
            if name not in self.fs:
                self._send({"resp": "nack", "code": 3})
            else:
                self.play_state = "play"
                self.active_name = name
                self.seq = 0
                if self.progress_before:
                    self._send({"resp": "progress", "seq": 0, "fps": 24})
                self._send({"resp": "state", "state": "play", "name": name, "seq": 0})
        elif cmd == "show":
            self.last_frame = payload
            self.received_frames.append(payload)
            if self.progress_before:
                self._send({"resp": "progress", "seq": 0, "fps": 24})
            self._send({"resp": "ack"})
        elif cmd == "stream_start":
            self.stream_state = "stream"
            self.seq = 0
            self._send({"resp": "state", "state": "stream", "name": "", "seq": 0})
        elif cmd == "frame":
            self.received_frames.append(payload)
            self.seq += 1
            self._send({"resp": "ack"})
        elif cmd == "stream_end":
            self.stream_state = None
            self.seq = 0
            self._send({"resp": "done"})
        elif cmd == "up_begin":
            name = obj["name"]
            size = obj["size"]
            if name in self.fs and len(self.fs[name]) != size:
                del self.fs[name]
            if name not in self.fs:
                self.fs[name] = bytearray(size)
                self.upload_progress[name] = 0
            self.upload = name
            off = self.upload_progress.get(name, 0)
            self._send({"resp": "ready", "off": off})
        elif cmd == "up_chunk":
            off = obj["off"]
            data = payload
            self.fs[self.upload][off:off + len(data)] = data
            self.upload_progress[self.upload] = max(
                self.upload_progress[self.upload], off + len(data)
            )
            self._send({"resp": "ack"})
        elif cmd == "up_end":
            name = self.upload
            got = zlib.crc32(bytes(self.fs[name])) & 0xFFFFFFFF
            if got == obj.get("crc"):
                self._send({"resp": "done"})
                self.upload_progress[name] = 0
            else:
                self._send({"resp": "nack", "code": 4})
        elif cmd == "transmit":
            name = obj["name"]
            self.fs[name] = bytes(payload)
            self._send({"resp": "done"})
        else:
            self._send({"resp": "nack", "code": 1})
