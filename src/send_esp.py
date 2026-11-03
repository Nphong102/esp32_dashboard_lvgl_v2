#!/usr/bin/env python3
"""
ets2_udp_bridge_bin.py
Đọc telemetry từ server ETS2 (cùng PC) -> đóng gói struct nhị phân 193 byte
-> gửi UDP sang ESP32 theo UDP_PROTOCOL.md.

    pip install requests
    python ets2_udp_bridge_bin.py
"""
import re
import socket
import struct
import threading
import time

import requests

# ───────────────── Cấu hình ─────────────────
TELEMETRY_URL = "http://127.0.0.1:25555"   # server telemetry trên cùng PC
ESP_ADDR      = "ets2-dash.local"          # hostname OTA của ESP (mDNS) hoặc IP cố định
ESP_PORT      = 4210                       # = TELEMETRY_UDP_PORT bên ESP
SEND_HZ       = 25
# ─────────────────────────────────────────────

MAGIC   = 0xE2
VERSION = 2

# Khớp UDP_PROTOCOL.md (little-endian, không padding):
#   magic ver seq flags | 11 x int16 | gear | 3 x uint8 wear | 3 x int32 | 7 chuỗi
FMT_BODY = "<BBHH" + "11h" + "b" + "BBB" + "3i" + "16s20s16s64s12s8s12s"
PACKET_SIZE = struct.calcsize(FMT_BODY) + 1          # +1 byte checksum
assert PACKET_SIZE == 193, f"sai kich thuoc struct: {PACKET_SIZE}"

# Thứ tự bit trong flags (khớp TF_* trong telemetry_packet.h)
FLAG_ORDER = ["connected", "paused", "fuelWarn", "airWarn", "engineOn", "parkBrake",
              "beamHigh", "beamLow", "blinkL", "blinkR", "cruiseOn", "trlAttach"]


# ── helper ──────────────────────────────────────────────
def g(d, *keys, default=0):
    for k in keys:
        if not isinstance(d, dict) or k not in d:
            return default
        d = d[k]
    return default if d is None else d


def num(v):
    try:
        return int(float(v))
    except (TypeError, ValueError):
        return 0


def flt(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return 0.0


def clamp(v, lo, hi):
    return lo if v < lo else hi if v > hi else v


def i16(v): return clamp(num(v), -32768, 32767)
def i32(v): return clamp(num(v), -2147483648, 2147483647)
def i8(v):  return clamp(num(v), -128, 127)


def pct(v):
    try:
        return clamp(int(float(v) * 100), 0, 255)
    except (TypeError, ValueError):
        return 0


def cstr(v, n):
    """Cắt về n-1 byte UTF-8 (chừa 1 byte NUL), không để dở ký tự giữa chừng."""
    b = str(v).encode("utf-8")[: n - 1]
    return b.decode("utf-8", "ignore").encode("utf-8")


_TIME_RE = re.compile(r"(?:T|\s|^)(\d{1,2}):(\d{2})")


def hhmm(s):
    m = _TIME_RE.search(str(s))
    return f"{int(m.group(1)):02d}:{int(m.group(2)):02d}" if m else "--:--"


def xor_bytes(b):
    x = 0
    for c in b:
        x ^= c
    return x


# ── đóng gói ────────────────────────────────────────────
def build_packet(j, seq):
    connected = bool(g(j, "game", "connected", default=False))

    if connected:
        def t(k, d=0):
            return g(j, "truck", k, default=d)

        bits = {
            "connected": True,
            "paused":    bool(g(j, "game", "paused", default=True)),
            "fuelWarn":  bool(t("fuelWarningOn")),
            "airWarn":   bool(t("airWarningOn")),
            "engineOn":  bool(t("engineOn")),
            "parkBrake": bool(t("parkingBreak")),
            "beamHigh":  bool(t("lightsBeamHighOn")),
            "beamLow":   bool(t("lightsBeamLowOn")),
            "blinkL":    bool(t("blinkerLeftActive")),
            "blinkR":    bool(t("blinkerRightActive")),
            "cruiseOn":  bool(t("cruiseControlOn")),
            "trlAttach": bool(g(j, "trailer", "attached", default=False)),
        }
        flags = sum(1 << i for i, name in enumerate(FLAG_ORDER) if bits[name])

        # Server không có truck.maxWear -> lấy mức hao mòn cao nhất trong 5 bộ phận
        max_wear = t("maxWear", None)
        if max_wear is None:
            max_wear = max(flt(t(k)) for k in ("wearEngine", "wearTransmission",
                                               "wearCabin", "wearChassis", "wearWheels"))
        # Trailer: server dùng trailer.wear (không có trailerWear / cargoWear)
        trailer_wear = g(j, "trailer", "trailerWear", default=None)
        if trailer_wear is None:
            trailer_wear = g(j, "trailer", "wear")
        cargo_wear   = g(j, "trailer", "cargoWear")          # không có -> 0
        trailer_name = g(j, "trailer", "name", default="") or g(j, "trailer", "trailerName", default="")

        body = struct.pack(
            FMT_BODY,
            MAGIC, VERSION, seq & 0xFFFF, flags,
            # 11 x int16
            i16(abs(num(t("speed")))), i16(t("engineRpm")), i16(t("engineRpmMax", 2500)),
            i16(t("fuel")), i16(t("fuelCapacity")), i16(t("air")),
            i16(t("watterTemp")), i16(t("oilTemp")),
            i16(g(j, "navigation", "speedLimit")), i16(t("cruiseControlSpeed")),
            i16(int(num(g(j, "navigation", "estimatedDistance")) / 1000)),
            # gear + wear %
            i8(t("displayedGear")),
            pct(max_wear), pct(trailer_wear), pct(cargo_wear),
            # 3 x int32
            i32(t("odometer")), i32(g(j, "job", "income")), i32(g(j, "trailer", "mass")),
            # chuỗi
            cstr(t("make", ""), 16), cstr(t("model", ""), 20), cstr(t("licensePlate", ""), 16),
            cstr(trailer_name, 64),
            cstr(hhmm(g(j, "game", "realTime", default="")), 12),
            cstr(hhmm(g(j, "game", "time", default="")), 8),
            cstr(g(j, "job", "timeRemain", default=""), 12),
        )
    else:
        # Game chưa chạy: gói cùng kích thước, toàn số 0, flag connected = 0
        body = struct.pack(FMT_BODY, MAGIC, VERSION, seq & 0xFFFF, 0,
                           *([0] * 11), 0, 0, 0, 0, 0, 0, 0,
                           *([b""] * 7))

    return body + bytes([xor_bytes(body)])


# ── tìm IP của ESP ở thread riêng ───────────────────────
target = {"ip": None}


def resolver():
    while True:
        try:
            ip = socket.gethostbyname(ESP_ADDR)
            if ip != target["ip"]:
                print(f"[bridge] ESP -> {ip}:{ESP_PORT}")
            target["ip"] = ip
        except OSError:
            if target["ip"] is None:
                print(f"[bridge] chưa tìm thấy '{ESP_ADDR}', thử lại... (có thể điền IP cố định vào ESP_ADDR)")
        time.sleep(30 if target["ip"] else 3)


def main():
    threading.Thread(target=resolver, daemon=True).start()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    session = requests.Session()
    period = 1.0 / SEND_HZ
    next_t = time.perf_counter()
    seq = 0
    last_state = None
    print(f"[bridge] gói nhị phân {PACKET_SIZE} byte, {SEND_HZ} Hz")

    while True:
        try:
            r = session.get(TELEMETRY_URL, timeout=0.5)
            r.raise_for_status()
            j = r.json()
            ok = bool(g(j, "game", "connected", default=False))
            state, msg = ("run", "game ĐANG CHẠY") if ok else \
                         ("idle", "server OK nhưng game.connected=false (game chưa chạy / plugin chưa kết nối)")
        except Exception as e:
            j, ok = {}, False
            state, msg = "error", f"KHÔNG gọi được {TELEMETRY_URL} -> {type(e).__name__}: {e}"

        if state != last_state:
            print(f"[bridge] {msg}")
            last_state = state

        ip = target["ip"]
        if ip:
            sock.sendto(build_packet(j, seq), (ip, ESP_PORT))
            seq = (seq + 1) & 0xFFFF

        next_t += period if ok else 0.5
        delay = next_t - time.perf_counter()
        if delay > 0:
            time.sleep(delay)
        else:
            next_t = time.perf_counter()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n[bridge] bye")
