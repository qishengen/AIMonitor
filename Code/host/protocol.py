"""AIMonitor 上位机协议层：JSON 编解码(原始二进制 / base64 双模式) + 传输抽象。

包格式约定（与固件一致）：
- 普通命令：`{"cmd":"...","k":v,...}` + '\\n'（UTF-8 文本）
- 带数据体(二进制)：JSON 头换行后紧跟 size 字段指定长度的原始字节
- 带数据体(base64)：`{"cmd":"...","size":N,"enc":"b64","data":"<base64>"}` + '\\n'
"""
import base64
import json
import socket
import time

try:
    import serial as _serial
    HAS_PYSERIAL = True
except Exception:  # 未安装 pyserial 也可导入本模块（仅传输抽象可用）
    HAS_PYSERIAL = False

FRAME_BYTES = 1024
ANM_HEADER = 8


class NackError(Exception):
    def __init__(self, code):
        self.code = code
        super().__init__("device replied nack code=%d" % code)


class TimeoutError(Exception):
    pass


def encode_packet(obj, payload=None, enc="bin"):
    """将命令字典（可选数据体）编码为线上字节。"""
    obj = dict(obj)
    if payload is not None:
        payload = bytes(payload)
        if enc == "b64":
            obj["enc"] = "b64"
            obj["size"] = len(payload)
            obj["data"] = base64.b64encode(payload).decode("ascii")
            return (json.dumps(obj, separators=(",", ":")) + "\n").encode("utf-8")
        obj["size"] = len(payload)
        head = (json.dumps(obj, separators=(",", ":")) + "\n").encode("utf-8")
        return head + payload
    return (json.dumps(obj, separators=(",", ":")) + "\n").encode("utf-8")


class Transport:
    def send(self, data: bytes):
        raise NotImplementedError

    def readline(self, deadline: float):  # -> bytes | None（超时返回 None）
        raise NotImplementedError


class SerialTransport(Transport):
    """基于 pySerial 的真实串口传输。"""

    def __init__(self, ser):
        self._ser = ser

    def send(self, data: bytes):
        self._ser.write(data)

    def readline(self, deadline: float):
        self._ser.timeout = 0
        buf = bytearray()
        while time.time() < deadline:
            c = self._ser.read(1)
            if c:
                buf += c
                if c == b"\n":
                    return bytes(buf)
            else:
                time.sleep(0.001)
        return None


class TcpTransport(Transport):
    """基于 socket 的 TCP 传输（连接固件 TCP server，默认端口 8088）。"""

    def __init__(self, sock):
        self._sock = sock

    def send(self, data: bytes):
        self._sock.sendall(data)

    def readline(self, deadline: float):
        self._sock.setblocking(False)
        buf = bytearray()
        while time.time() < deadline:
            try:
                c = self._sock.recv(1)
            except BlockingIOError:
                time.sleep(0.001)
                continue
            except socket.timeout:
                continue
            except OSError:
                return None
            if not c:
                return None  # 对端关闭
            buf += c
            if c == b"\n":
                return bytes(buf)
        return None

    def close(self):
        try:
            self._sock.close()
        except OSError:
            pass


def connect_tcp(host, port=8088, timeout=3.0):
    """连接固件 TCP server 并返回 Link。host 支持 aimonitor.local / IP。"""
    sock = socket.create_connection((host, port), timeout=timeout)
    try:
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    except OSError:
        pass
    return Link(TcpTransport(sock), timeout=timeout)


class Link:
    """请求/响应链路：发送命令，读取回包。可替换传输实现（含测试用模拟）。"""

    def __init__(self, transport: Transport, timeout=2.0):
        self._t = transport
        self.timeout = timeout

    def send(self, obj, payload=None, enc="bin"):
        self._t.send(encode_packet(obj, payload, enc))

    def read_response(self, deadline=None):
        if deadline is None:
            deadline = time.time() + self.timeout
        line = self._t.readline(deadline)
        if line is None:
            return None
        s = line.decode("utf-8").strip()
        if not s:
            return None
        return json.loads(s)

    def request(self, obj, payload=None, enc="bin", expect=("ack",), timeout=None):
        """发送并读取回包直到匹配 expect 之一；遇到 nack 抛 NackError（除非 expect 含 nack）。"""
        self.send(obj, payload, enc)
        deadline = time.time() + (timeout if timeout is not None else self.timeout)
        while True:
            resp = self.read_response(deadline)
            if resp is None:
                raise TimeoutError(
                    "timeout waiting for %s after %s" % (expect, json.dumps(obj))
                )
            r = resp.get("resp")
            if r == "nack":
                if "nack" in expect:
                    return resp
                raise NackError(resp.get("code"))
            if r in expect:
                return resp
            # 其余回包（progress/state 等）忽略，继续等待
