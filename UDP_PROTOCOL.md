# Giao thức UDP telemetry ETS2 → ESP32 dashboard

Tài liệu mô tả cách gửi dữ liệu từ server telemetry (PC chạy ETS2) sang ESP32-S3 dashboard qua UDP.

- **Phiên bản giao thức:** 2
- **Kích thước gói:** 193 byte (cố định)
- **Cổng mặc định:** UDP 4210
- **Thứ tự byte:** little-endian, không có byte đệm (packed)

---

## 1. Tổng quan

```
ETS2 ──► Telemetry plugin ──► TelemetryJsonService (C#, PC)
                                   │
                                   │  mỗi khi JsonData đổi (~9 lần/giây)
                                   │  + nhịp tim 4 lần/giây
                                   ▼
                          UDP unicast, 193 byte
                                   │
                                   ▼
                    ESP32-S3 :4210  (wifi_task.cpp)
                    kiểm tra → ghi vào truck/game/job/trailer/navigation → LVGL vẽ
```

Có hai bên gửi, cùng một định dạng gói:

| Bên gửi | File | Dùng khi |
|---|---|---|
| Server C# (khuyên dùng) | `UdpTelemetry.cs` + `DataService.cs` | Gửi ngay khi có dữ liệu mới, không cần chương trình phụ |
| Cầu nối Python | `ets2_udp_bridge_bin.py` | Khi không sửa được server; hỏi HTTP `localhost:25555` rồi gửi UDP |

Hai bên không nên chạy cùng lúc (ESP sẽ nhận gói từ cả hai).

---

## 2. Kênh truyền

| Thuộc tính | Giá trị |
|---|---|
| Giao thức | UDP/IPv4, unicast tới IP của ESP32 |
| Cổng đích | 4210 (`TELEMETRY_UDP_PORT` bên ESP, `udpPort` bên server) |
| Một gói | Đúng một datagram, đúng 193 byte |
| Tần suất | Theo dữ liệu mới (~9 gói/giây với server hiện tại) + nhịp tim 4 gói/giây khi chưa có dữ liệu mới. Cầu nối Python: 20-25 gói/giây |
| Lưu lượng | 193 B × 25 gói/s ≈ 4,8 KB/s (≈ 0,04 Mbit/s) |
| Độ tin cậy | Không xác nhận, không gửi lại. Rớt một gói không sao vì gói kế tiếp thay thế |
| Bảo mật | Không mã hoá, không xác thực. Chỉ dùng trong mạng LAN tin cậy |

Kích thước 193 byte nhỏ hơn nhiều so với MTU Ethernet/WiFi (~1472 byte payload UDP) nên gói không bị phân mảnh.

---

## 3. Cấu trúc gói (193 byte)

Offset tính bằng byte từ đầu gói, số hệ thập phân. Kiểu: `u8/i8` = 8 bit không dấu/có dấu, `u16/i16` = 16 bit, `i32` = 32 bit có dấu, `char[N]` = chuỗi UTF-8 đệm bằng 0.

| Offset | Kích thước | Kiểu | Trường | Mô tả |
|---:|---:|---|---|---|
| 0 | 1 | u8 | `magic` | Luôn `0xE2` |
| 1 | 1 | u8 | `version` | Phiên bản giao thức, hiện là `2` |
| 2 | 2 | u16 | `seq` | Số thứ tự gói, tăng 1 mỗi gói, vòng về 0 sau 65535 |
| 4 | 2 | u16 | `flags` | Các công tắc bật/tắt (xem mục 4) |
| 6 | 2 | i16 | `speed` | Tốc độ, km/h |
| 8 | 2 | i16 | `rpm` | Vòng tua hiện tại |
| 10 | 2 | i16 | `rpmMax` | Vòng tua tối đa của động cơ |
| 12 | 2 | i16 | `fuel` | Nhiên liệu hiện có (lít) |
| 14 | 2 | i16 | `fuelCap` | Dung tích bình nhiên liệu (lít) |
| 16 | 2 | i16 | `air` | Áp suất hơi |
| 18 | 2 | i16 | `waterTemp` | Nhiệt độ nước, °C |
| 20 | 2 | i16 | `oilTemp` | Nhiệt độ dầu, °C |
| 22 | 2 | i16 | `speedLimit` | Giới hạn tốc độ đường hiện tại (giá trị thô), km/h |
| 24 | 2 | i16 | `cruiseSpeed` | Tốc độ cài cruise control |
| 26 | 2 | i16 | `distKm` | Quãng đường còn lại tới đích, km |
| 28 | 1 | i8 | `gear` | Số hiển thị: dương = số tiến, `0` = N, âm = số lùi |
| 29 | 1 | u8 | `truckWear` | Hao mòn xe, % |
| 30 | 1 | u8 | `trailerWear` | Hao mòn rơ-moóc, % |
| 31 | 1 | u8 | `cargoWear` | Hao mòn hàng hoá, % |
| 32 | 4 | i32 | `odo` | Công-tơ-mét, km |
| 36 | 4 | i32 | `income` | Thu nhập của đơn hàng |
| 40 | 4 | i32 | `trailerMass` | Khối lượng rơ-moóc/hàng, kg |
| 44 | 16 | char[16] | `make` | Hãng xe |
| 60 | 20 | char[20] | `model` | Tên model |
| 80 | 16 | char[16] | `plate` | Biển số |
| 96 | 64 | char[64] | `trailerName` | Tên hàng/rơ-moóc |
| 160 | 12 | char[12] | `realTime` | Giờ thật, dạng `HH:MM` |
| 172 | 8 | char[8] | `gameTime` | Giờ trong game, dạng `HH:MM` |
| 180 | 12 | char[12] | `remainTime` | Thời gian còn lại của đơn hàng, ví dụ `05h42m` |
| 192 | 1 | u8 | `checksum` | XOR của 192 byte đứng trước (offset 0-191) |

Tổng: 6 (header) + 22 (11 số i16) + 4 (gear + 3 mức hao mòn) + 12 (3 số i32) + 148 (7 chuỗi) + 1 (checksum) = **193 byte**.


### Chuỗi định dạng Python (`struct`)

```python
FMT_BODY = "<BBHH" + "11h" + "b" + "BBB" + "3i" + "16s20s16s64s12s8s12s"
# struct.calcsize(FMT_BODY) + 1 (checksum) == 193
```

---

## 4. Trường `flags` (u16)

Mỗi bit là một công tắc: `1` = bật, `0` = tắt. Bit 12-15 để dành, luôn là 0.

| Bit | Mặt nạ | Tên | Nguồn trong JSON server | Ý nghĩa khi bật |
|---:|---|---|---|---|
| 0 | `0x0001` | `connected` | `game.connected` | Game đã kết nối với server telemetry |
| 1 | `0x0002` | `paused` | `game.paused` | Game đang tạm dừng (thiếu trường → coi là đang dừng) |
| 2 | `0x0004` | `fuelWarn` | `truck.fuelWarningOn` | Cảnh báo sắp hết nhiên liệu |
| 3 | `0x0008` | `airWarn` | `truck.airWarningOn` | Cảnh báo áp suất hơi thấp (server hiện không có trường này → luôn 0) |
| 4 | `0x0010` | `engineOn` | `truck.engineOn` | Động cơ đang nổ |
| 5 | `0x0020` | `parkBrake` | `truck.parkingBreak` | Phanh tay đang kéo |
| 6 | `0x0040` | `beamHigh` | `truck.lightsBeamHighOn` | Đèn pha |
| 7 | `0x0080` | `beamLow` | `truck.lightsBeamLowOn` | Đèn cos |
| 8 | `0x0100` | `blinkL` | `truck.blinkerLeftActive` | Xi-nhan trái (nhấp nháy theo đèn) |
| 9 | `0x0200` | `blinkR` | `truck.blinkerRightActive` | Xi-nhan phải |
| 10 | `0x0400` | `cruiseOn` | `truck.cruiseControlOn` | Đang bật ga tự động |
| 11 | `0x0800` | `trlAttach` | `trailer.attached` | Đã móc rơ-moóc |

Ví dụ: `connected` + `engineOn` + `beamHigh` + `beamLow` = 0x0001 + 0x0010 + 0x0040 + 0x0080 = `0x00D1`.

---

## 5. Quy tắc chuyển đổi từ JSON của server

| Trường gói | Lấy từ JSON | Biến đổi |
|---|---|---|
| `speed` | `truck.speed` | Giá trị tuyệt đối, cắt phần thập phân |
| `rpm` | `truck.engineRpm` | Cắt phần thập phân |
| `rpmMax` | `truck.engineRpmMax` | Cắt phần thập phân; thiếu trường → 2500 |
| `fuel`, `fuelCap` | `truck.fuel`, `truck.fuelCapacity` | Cắt phần thập phân |
| `air` | `truck.air` | |
| `waterTemp` | `truck.watterTemp` | Chú ý tên trường của server viết sai chính tả (`watter`) |
| `oilTemp` | `truck.oilTemp` | |
| `speedLimit` | `navigation.speedLimit` | ESP làm tròn xuống bội số của 5 khi hiển thị biển báo |
| `cruiseSpeed` | `truck.cruiseControlSpeed` | |
| `distKm` | `navigation.estimatedDistance` | Chia 1000 (mét → km), cắt phần thập phân |
| `gear` | `truck.displayedGear` | Kẹp vào −128…127 |
| `truckWear` | `truck.maxWear`, nếu không có thì lớn nhất của `wearEngine`, `wearTransmission`, `wearCabin`, `wearChassis`, `wearWheels` | Nhân 100, cắt thập phân, kẹp 0…255 |
| `trailerWear` | `trailer.trailerWear`, nếu không có thì `trailer.wear` | Nhân 100, kẹp 0…255 |
| `cargoWear` | `trailer.cargoWear` | Nhân 100; server hiện không có → 0 |
| `odo` | `truck.odometer` | |
| `income` | `job.income` | |
| `trailerMass` | `trailer.mass` | |
| `make`, `model`, `plate` | `truck.make`, `truck.model`, `truck.licensePlate` | |
| `trailerName` | `trailer.name`, nếu rỗng thì `trailer.trailerName` | |
| `realTime` | `game.realTime` | Giữ nguyên |
| `gameTime` | `game.time` (dạng `2026-11-01T14:35:00Z`) | Lấy `HH:MM` sau chữ `T` |
| `remainTime` | `job.timeRemain` | Giữ nguyên (ví dụ `05h42m`; bắt đầu bằng `-` nghĩa là trễ hạn) |

Mọi số nguyên vượt giới hạn kiểu của trường đều được **kẹp về biên** (không báo lỗi, không tràn).

### Quy tắc cho chuỗi

- Mã hoá **UTF-8**, đệm bằng byte `0x00` tới hết độ dài trường.
- Chuỗi dài hơn bị cắt còn tối đa **N − 1 byte** (chừa 1 byte `0x00` cuối), và **không cắt giữa một ký tự**: nếu điểm cắt rơi vào giữa ký tự nhiều byte thì bỏ cả ký tự đó.
- Giới hạn tính theo **byte**, không phải số ký tự. Chữ Việt có dấu chiếm 2-3 byte; ví dụ `Thiết bị trao đổi nhiệt công nghiệp` là 35 ký tự nhưng 47 byte.
- Bên nhận phải tự đặt byte cuối của mỗi trường chuỗi bằng 0 trước khi dùng.

---

## 6. Checksum

`checksum` = XOR của tất cả byte từ offset 0 đến 191 (192 byte).

**Python**
```python
x = 0
for c in body:          # body = 192 byte đầu
    x ^= c
packet = body + bytes([x])
```

**C / C++ (ESP32)**
```c
static inline uint8_t telemXor(const uint8_t* p, size_t n) {
    uint8_t x = 0;
    for (size_t i = 0; i < n; i++) x ^= p[i];
    return x;
}
// kiểm tra:  telemXor((uint8_t*)&p, sizeof(p) - 1) == p.checksum
```

**C#**
```csharp
byte x = 0;
for (int i = 0; i < p.Length - 1; i++) x ^= p[i];
p[p.Length - 1] = x;
```

Lưu ý: `seq` nằm trong phần được XOR nên checksum đổi theo mỗi gói. XOR chỉ để phát hiện gói sai phiên bản struct hoặc gói lạ; UDP đã có checksum riêng ở tầng dưới cho lỗi đường truyền.

---

## 7. Các loại gói

Cả ba loại có cùng định dạng và cùng kích thước 193 byte, chỉ khác nội dung.

| Loại | Khi nào gửi | Nội dung |
|---|---|---|
| **Dữ liệu** | Mỗi khi `JsonData` có giá trị mới và `game.connected = true` | Đầy đủ, `flags` bit 0 = 1 |
| **Nhịp tim** | Mỗi 250 ms nếu chưa gửi gì và dữ liệu cuối còn mới (< 1 giây) | Gửi lại gói dữ liệu cuối, `seq` và `checksum` mới |
| **Idle** (game chưa chạy) | Khi `game.connected = false`, hoặc quá 1 giây không có dữ liệu mới | Toàn số 0, trừ `magic`, `version`, `seq`, `checksum`; `flags = 0` |

Hành vi phía ESP32:

- Mỗi lần đọc, ESP rút cạn hàng đợi UDP và **chỉ dùng gói mới nhất**.
- Nhận gói `connected = 0` → `game.connected = false` → dashboard chuyển sang màn hình ảnh chờ (sau khoảng trễ chống nhấp nháy 2 giây).
- Không nhận được gói hợp lệ nào trong `UDP_TIMEOUT_MS` (1500 ms) → coi như mất kết nối, cũng chuyển sang màn hình ảnh chờ.
- Không có telemetry hoạt động trong `TELEMETRY_TIMEOUT_MS` (90 giây) → ESP vào deep sleep, thức dậy mỗi 30 giây để kiểm tra.
- `seq` dùng để đếm rớt gói: `lost` tăng khi `seq` nhảy cóc.

### Kiểm tra telemetry khi thức dậy

Sau khi thức dậy bằng timer, ESP kết nối Wi-Fi rồi mở UDP port 4210 trước khi khởi tạo màn hình. ESP chờ tối đa 3 giây cho gói hợp lệ (đúng 193 byte, version 2, checksum hợp lệ) có `connected = 1`. Nếu nhận được, dữ liệu gói đó được dùng ngay và dashboard khởi động; nếu không, ESP ngủ lại mà không bật màn hình. Vì vậy bridge/server cần chạy và gửi UDP trong lúc ESP kiểm tra.

---

## 8. Cách gửi

### 8.1. Từ server C# (khuyên dùng)

1. Thêm `UdpTelemetry.cs` vào project, thay `DataService.cs` (setter của `JsonData` gọi `UdpTelemetry.OnJson()`, `StartWebServer()` gọi `UdpTelemetry.Start()`).
2. Cài NuGet `Newtonsoft.Json` nếu chưa có.
3. Thêm vào `App.config`:
   ```xml
   <add key="udpHost" value="192.168.222.36" />   <!-- IP (hoặc hostname) của ESP32 -->
   <add key="udpPort" value="4210" />
   ```
   Không có `udpHost` thì UDP tự tắt. Server HTTP vẫn chạy song song.

Console của server in:
- `UDP: ESP -> ip:port` khi tìm thấy ESP.
- `UDP: đã gửi gói đầu tiên tới ip:port`.
- Mỗi 5 giây: `UDP 5s: json_moi=.. nhip_tim=.. idle=.. game.connected=.. dich=..`.


## 10. Ví dụ một gói thật (đã giải mã)

Xe Mercedes-Benz New Actros đang nổ máy, bật pha và cos, `seq = 1234`:

```
offset  byte hex
0000    e2 02 d2 04 d1 00 00 00  25 02 fc 08 9d 02 c0 03
0010    81 00 54 00 59 00 50 00  00 00 6c 00 01 0c 00 00
0020    40 e2 01 00 00 00 00 00  00 00 00 00 4d 65 72 63
0030    65 64 65 73 2d 42 65 6e  7a 00 00 00 4e 65 77 20
0040    41 63 74 72 6f 73 00 00  00 00 00 00 00 00 00 00
0050    37 37 43 2d 30 35 36 36  39 00 00 00 00 00 00 00
0060    00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00
 ...    (0x70 - 0x9F: toàn 00, vùng trailerName rỗng)
00A0    31 36 3a 31 36 00 00 00  00 00 00 00 31 34 3a 33
00B0    35 00 00 00 30 30 68 30  30 6d 00 00 00 00 00 00
00C0    56
```

Giải mã:

| Offset | Byte | Giá trị |
|---:|---|---|
| 0 | `e2` | magic |
| 1 | `02` | version 2 |
| 2-3 | `d2 04` | seq = 0x04D2 = 1234 |
| 4-5 | `d1 00` | flags = 0x00D1 → connected, engineOn, beamHigh, beamLow |
| 6-7 | `00 00` | speed = 0 |
| 8-9 | `25 02` | rpm = 0x0225 = 549 |
| 10-11 | `fc 08` | rpmMax = 0x08FC = 2300 |
| 12-13 | `9d 02` | fuel = 669 |
| 14-15 | `c0 03` | fuelCap = 960 |
| 16-17 | `81 00` | air = 129 |
| 18-19 | `54 00` | waterTemp = 84 |
| 20-21 | `59 00` | oilTemp = 89 |
| 22-23 | `50 00` | speedLimit = 80 |
| 26-27 | `6c 00` | distKm = 108 |
| 28 | `01` | gear = 1 |
| 29 | `0c` | truckWear = 12 % |
| 32-35 | `40 e2 01 00` | odo = 0x0001E240 = 123456 |
| 44-… | `4d 65 72 63 …` | make = `Mercedes-Benz` |
| 60-… | `4e 65 77 20 …` | model = `New Actros` |
| 80-… | `37 37 43 2d …` | plate = `77C-05669` |
| 160-… | `31 36 3a 31 36` | realTime = `16:16` |
| 172-… | `31 34 3a 33 35` | gameTime = `14:35` |
| 180-… | `30 30 68 30 30 6d` | remainTime = `00h00m` |
| 192 | `56` | checksum |

---

## 12. Báo cáo pin qua UDP

ESP32 gửi một gói JSON riêng tới `192.168.222.49:8888` sau khi kết nối Wi-Fi:

```json
{"voltage_v":4.12,"wake":"timer","time":"2026-10-31T14:25:30+07:00","time_valid":true}
```

- `voltage_v`: điện áp pin tính bằng volt.
- `wake`: `boot` khi khởi động thông thường, `timer` khi thức dậy từ deep sleep.
- `time`: giờ địa phương ICT (UTC+07:00) theo ISO 8601; là `null` nếu chưa có giờ hợp lệ.
- `time_valid`: cho biết `time` có hợp lệ hay không.

Trong lần khởi động thông thường, ESP32 đồng bộ NTP một lần (tối đa 8 giây) rồi gửi báo cáo. Khi thức dậy bằng timer, thiết bị khôi phục giờ đã lưu trong RTC và không đồng bộ NTP lại. Báo cáo này có thể bật/tắt bằng `ENABLE_BATTERY_UDP_REPORT`; đích IP và cổng được cấu hình trong firmware.


## 13. Giới hạn đã biết

- Server hiện không có `truck.maxWear`, `trailer.trailerWear`, `trailer.cargoWear`, `truck.airWarningOn`, nên gói dùng giá trị thay thế (xem mục 5) hoặc luôn bằng 0.
- Giá trị `speed`, `rpm`, `fuel`… là số nguyên 16 bit, đủ cho dải giá trị thông thường của ETS2.
- Một ESP32 nhận từ một nguồn gửi. Nếu muốn nhiều ESP, cần gửi tới từng IP (hoặc đổi sang broadcast/multicast).
- Đồng hồ hai bên không đồng bộ; `seq` chỉ để phát hiện rớt gói, không dùng đo độ trễ.
