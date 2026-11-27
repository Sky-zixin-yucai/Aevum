#!/usr/bin/env python3
"""
AEVUM serial bridge: reads TCP-framed serial from QEMU, forwards to WebSocket.
Delta frame: A5 5A 01 COUNT [row col ch attr] * COUNT CRC
Status frame: A5 5A 03 19 + 19 * i32 LE + CRC
Path frame: A5 5A 05 12 + 12 * i32 LE + CRC
Joint command to kernel: b"J" + 3 * i16 LE (0.001 rad)
Line path command: b"P" + kind + 8 * i32 LE (Q16.16)
Release manual: b"R"
"""
import asyncio
import json
import os
import struct
import sys
import websockets

# 桌面安装包会通过环境变量指定端口；仓库里直接运行时保持旧默认值。
QEMU_SERIAL_HOST = os.environ.get("QEMU_SERIAL_HOST", "127.0.0.1")
QEMU_SERIAL_PORT = int(os.environ.get("QEMU_SERIAL_PORT", "5555"))
WS_HOST = os.environ.get("BRIDGE_WS_HOST", "127.0.0.1")
WS_PORT = int(os.environ.get("BRIDGE_WS_PORT", "8765"))

SYNC0 = 0xA5
SYNC1 = 0x5A
FRAME_TYPE_DELTA = 0x01
FRAME_TYPE_STATE = 0x02
FRAME_TYPE_STATUS = 0x03
FRAME_TYPE_PATH = 0x05
FRAME_HDR = 4
MAX_CELLS = 32
MAX_STATUS_FIELDS = 34
STATE_VALS = 3
PATH_FIELDS = 12
PATH_KIND_LINE = 1
BODY_MIN_JOINTS = 2
BODY_MAX_JOINTS = 8
Q16_ONE = 65536.0



def status_fields(n):
    return (
        ["q%d" % i for i in range(n)]
        + ["dq%d" % i for i in range(n)]
        + ["qt%d" % i for i in range(n)]
        + ["tx", "ty", "err_mm", "w_manip", "sigma_min", "sigma_max",
           "null_share", "limit_margin", "mode", "null_on"]
    )


def status_joint_count(count):
    if count < 16 or count > MAX_STATUS_FIELDS:
        return 0
    if (count - 10) % 3 != 0:
        return 0
    n = (count - 10) // 3
    return n if BODY_MIN_JOINTS <= n <= BODY_MAX_JOINTS else 0


def status_is_sane(status, n):
    """Reject a CRC collision that starts inside a VGA delta payload."""
    for prefix in ("q", "qt", "dq"):
        for i in range(n):
            if abs(status[prefix + str(i)]) > 210000:
                return False
    if status["mode"] not in (0, 1):
        return False
    if status["null_on"] not in (0, 1):
        return False
    if status["null_share"] < 0 or status["null_share"] > Q16_ONE:
        return False
    if status["limit_margin"] < 0 or status["limit_margin"] > 200 * Q16_ONE:
        return False
    return True


def pack_body(body):
    """WS body -> B + u8 n + n * 4 * i32 LE (length/min/max/home)."""
    try:
        n = int(body.get("n", 0))
        if n < BODY_MIN_JOINTS or n > BODY_MAX_JOINTS:
            return None
        lengths = [float(v) for v in body.get("lengths", [])]
        q_min = [float(v) for v in body.get("qMin", [])]
        q_max = [float(v) for v in body.get("qMax", [])]
        q_home = [float(v) for v in body.get("qHome", [])]
        if len(lengths) != n or len(q_min) != n or len(q_max) != n or len(q_home) != n:
            return None
        vals = []
        for i in range(n):
            if not (0 < lengths[i] <= 2.0):
                return None
            if not (-3.2 <= q_min[i] < q_max[i] <= 3.2):
                return None
            if not (q_min[i] <= q_home[i] <= q_max[i]):
                return None
            vals.extend([
                int(round(lengths[i] * Q16_ONE)),
                int(round(q_min[i] * Q16_ONE)),
                int(round(q_max[i] * Q16_ONE)),
                int(round(q_home[i] * Q16_ONE)),
            ])
        return b"B" + bytes([n]) + struct.pack("<%di" % (4 * n), *vals)
    except (TypeError, ValueError):
        return None


def pack_joint(q):
    try:
        n = len(q)
        if n < BODY_MIN_JOINTS or n > BODY_MAX_JOINTS:
            return None
        raw_q = [int(round(float(v) * 1000.0)) for v in q]
    except (TypeError, ValueError):
        return None
    raw_q = [max(-32768, min(32767, v)) for v in raw_q]
    return b"J" + bytes([n]) + struct.pack("<%dh" % n, *raw_q)


def pack_path(params):
    """WS path line -> P + kind + 8 * i32 LE (Q16.16 meters / m/s)."""
    try:
        kind = int(params.get("kind", PATH_KIND_LINE))
        if kind != PATH_KIND_LINE:
            return None
        vals = [
            float(params.get("x0", 0)),
            float(params.get("z0", 0)),
            float(params.get("x1", 0)),
            float(params.get("z1", 0)),
            float(params.get("speed", 0)),
            float(params.get("tol", 0)),
            float(params.get("timeout", 0)),
            0,
        ]
        raw = [int(round(v * Q16_ONE)) for v in vals]
        return b"P" + bytes([kind]) + struct.pack("<8i", *raw)
    except (TypeError, ValueError):
        return None


class FrameParser:
    def __init__(self):
        self.buf = bytearray()
        self.frames_ok = 0
        self.frames_bad = 0

    def feed(self, data: bytes):
        self.buf.extend(data)
        out = []
        while True:
            # sync
            i = 0
            while i + 1 < len(self.buf):
                if self.buf[i] == SYNC0 and self.buf[i + 1] == SYNC1:
                    break
                i += 1
            if i > 0:
                del self.buf[:i]
            if len(self.buf) < FRAME_HDR:
                break
            ftype = self.buf[2]
            count = self.buf[3]
            if count > MAX_STATUS_FIELDS:
                del self.buf[:1]
                self.frames_bad += 1
                continue
            total = FRAME_HDR + count * 4 + 1
            if len(self.buf) < total:
                break
            crc = 0
            for b in self.buf[:total - 1]:
                crc ^= b
            if crc != self.buf[total - 1]:
                del self.buf[:1]
                self.frames_bad += 1
                continue
            if ftype == FRAME_TYPE_STATE:
                if count != STATE_VALS:
                    del self.buf[:1]
                    self.frames_bad += 1
                    continue
                q = struct.unpack_from("<iii", self.buf, FRAME_HDR)
                out.append({
                    "type": "state",
                    "q": [round(v / Q16_ONE, 6) for v in q],
                })
                del self.buf[:total]
                self.frames_ok += 1
                continue
            if ftype == FRAME_TYPE_PATH:
                if count != PATH_FIELDS:
                    del self.buf[:1]
                    self.frames_bad += 1
                    continue
                vals = struct.unpack_from("<12i", self.buf, FRAME_HDR)
                out.append({
                    "type": "path",
                    "tick": vals[0],
                    "kind": vals[1],
                    "state": vals[2],
                    "fault": vals[3],
                    "s": round(vals[4] / Q16_ONE, 6),
                    "t": round(vals[5] / Q16_ONE, 6),
                    "x": round(vals[6] / Q16_ONE, 6),
                    "z": round(vals[7] / Q16_ONE, 6),
                    "dx": round(vals[8] / Q16_ONE, 6),
                    "dz": round(vals[9] / Q16_ONE, 6),
                    "speed": round(vals[10] / Q16_ONE, 6),
                    "length": round(vals[11] / Q16_ONE, 6),
                })
                del self.buf[:total]
                self.frames_ok += 1
                continue
            if ftype == FRAME_TYPE_STATUS:
                n = status_joint_count(count)
                if n == 0:
                    del self.buf[:1]
                    self.frames_bad += 1
                    continue
                fields = status_fields(n)
                vals = struct.unpack_from("<%di" % count, self.buf, FRAME_HDR)
                status = dict(zip(fields, vals))
                if not status_is_sane(status, n):
                    del self.buf[:1]
                    self.frames_bad += 1
                    continue
                out.append({
                    "type": "status",
                    "n": n,
                    "q": [round(status["q%d" % i] / Q16_ONE, 6) for i in range(n)],
                    "dq": [round(status["dq%d" % i] / Q16_ONE, 6) for i in range(n)],
                    "qTarget": [round(status["qt%d" % i] / Q16_ONE, 6) for i in range(n)],
                    "values": status,
                })
                del self.buf[:total]
                self.frames_ok += 1
                continue
            cells = []
            p = FRAME_HDR
            for _ in range(count):
                row, col, ch, attr = struct.unpack_from("BBBB", self.buf, p)
                cells.append((row, col, ch, attr))
                p += 4
            out.append({"type": ftype, "cells": cells})
            del self.buf[:total]
            self.frames_ok += 1
        return out


async def serial_reader(parser, broadcast, link):
    while True:
        try:
            reader, writer = await asyncio.open_connection(QEMU_SERIAL_HOST, QEMU_SERIAL_PORT)
            break
        except OSError:
            # QEMU 先启动还是桥接先启动都可能；桥接先起时在这里等串口。
            await asyncio.sleep(0.4)
    link["writer"] = writer
    print(f"[bridge] connected to QEMU serial {QEMU_SERIAL_HOST}:{QEMU_SERIAL_PORT}")
    try:
        while True:
            data = await reader.read(4096)
            if not data:
                break
            for frame in parser.feed(data):
                await broadcast(frame)
    finally:
        link["writer"] = None
        writer.close()


async def main():
    parser = FrameParser()
    clients = set()
    state = {}
    last_status = {"frame": None}
    last_path = {"frame": None}
    link = {"writer": None}

    async def send_status(ws, frame):
        await ws.send(json.dumps(frame))

    async def handler(ws):
        clients.add(ws)
        try:
            # 新窗口后连接时补一帧当前画面，否则只收到打开之后的差量。
            if state:
                await ws.send(json.dumps({"type": "frame", "cells": list(state.values())}))
            # 新订阅者补最近一次完整状态，避免等下一次 8 帧扫描。
            if last_status["frame"] is not None:
                await send_status(ws, last_status["frame"])
            if last_path["frame"] is not None:
                await ws.send(json.dumps(last_path["frame"]))
            # 不在连接瞬间发 F：全量 VGA 扫描会占住串口/桥接，
            # 把紧随其后的第一条 J 手动命令拖到 5 秒保持窗口之外。
            # 内核自身每 120 次扫描会补一帧全量，静态单元仍能补齐。
            async for raw in ws:
                try:
                    msg = json.loads(raw)
                except (TypeError, ValueError):
                    continue
                if msg.get("type") in ("joint", "manual"):
                    payload = pack_joint(msg.get("q"))
                    if payload is not None and link["writer"]:
                        try:
                            link["writer"].write(payload)
                            await link["writer"].drain()
                        except Exception:
                            pass
                elif msg.get("type") == "body":
                    payload = pack_body(msg.get("body") or {})
                    if payload is not None and link["writer"]:
                        try:
                            link["writer"].write(payload)
                            await link["writer"].drain()
                        except Exception:
                            pass
                elif msg.get("type") == "path":
                    payload = pack_path(msg)
                    if payload is not None and link["writer"]:
                        try:
                            link["writer"].write(payload)
                            await link["writer"].drain()
                        except Exception:
                            pass
                elif msg.get("type") == "release":
                    if link["writer"]:
                        try:
                            link["writer"].write(b"R")
                            await link["writer"].drain()
                        except Exception:
                            pass
                elif msg.get("type") == "full_refresh":
                    if link["writer"]:
                        try:
                            link["writer"].write(b"F")
                            await link["writer"].drain()
                        except Exception:
                            pass
        finally:
            clients.discard(ws)

    async def broadcast(frame):
        if frame["type"] == "path":
            last_path["frame"] = frame
            dead = []
            for ws in list(clients):
                try:
                    await ws.send(json.dumps(frame))
                except Exception:
                    dead.append(ws)
            for ws in dead:
                clients.discard(ws)
            return

        if frame["type"] == "status":
            last_status["frame"] = frame
            dead = []
            for ws in list(clients):
                try:
                    await send_status(ws, frame)
                except Exception:
                    dead.append(ws)
            for ws in dead:
                clients.discard(ws)
            return

        if frame["type"] == "state":
            # 兼容仍只认 q 的旧客户端；正式前端使用 status。
            dead = []
            for ws in list(clients):
                try:
                    await ws.send(json.dumps(frame))
                except Exception:
                    dead.append(ws)
            for ws in dead:
                clients.discard(ws)
            return

        for cell in frame["cells"]:
            state[(cell[0], cell[1])] = cell
        if not clients:
            return
        msg = {"type": "frame", "cells": frame["cells"]}
        dead = []
        for ws in list(clients):
            try:
                await ws.send(json.dumps(msg))
            except Exception:
                dead.append(ws)
        for ws in dead:
            clients.discard(ws)

    async with websockets.serve(handler, WS_HOST, WS_PORT):
        print(f"[bridge] websocket listening on ws://{WS_HOST}:{WS_PORT}")
        await serial_reader(parser, broadcast, link)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\n[bridge] bye")
        sys.exit(0)