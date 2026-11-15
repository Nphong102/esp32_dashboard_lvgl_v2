# ESP32 dashboard timing baseline

Captured on 2026-10-31 from the physical dashboard before adding a web server.

## Test conditions

- Board: ESP32-S3 dashboard; the user confirmed the physical board has 16 MB flash.
- Wi-Fi and telemetry were connected while dashboard data was being changed.
- Serial monitor: 115200 baud.
- Display metrics are reported per 500 display cycles; network metrics per 100 fetches.
- Averages below are weighted by the reported sample count. Since firmware reports
  rounded per-window averages, aggregates are approximate.

## Results

| Metric | Samples | Weighted average | Largest reported value |
|---|---:|---:|---:|
| HTTP GET + response body | 2,900 | 38.1 ms | 202.5 ms |
| JSON parse + update | 2,900 | 4.3 ms | 6.7 ms |
| Valid telemetry fetch | 2,900 | 43.0 ms | 206.8 ms |
| UI widget update | 47,850 | 0.194 ms | 3.9 ms |
| LVGL handler | 48,500 | 3.84 ms | 98.8 ms |
| TFT flush, per dirty frame | 16,694 | 4.65 ms | 35.4 ms |
| Telemetry update to TFT flush | 2,865 | 23.7 ms | 4,846.1 ms |

One telemetry-to-flush window contained a 4.846-second outlier. Excluding that
single window, the remaining 96 windows contain 2,835 samples with a weighted
average of 22.0 ms; the largest per-window maximum among them was 227.6 ms.
Keep both the all-sample and outlier-excluded figures when comparing later runs.

## Resource snapshots

- Free heap: 183,052–189,904 bytes; average 189,536 bytes over 61 snapshots.
- Free PSRAM: about 8,063,731 bytes in the captured snapshots.
- Display task stack high-water value: 7,020–7,228 (reported units) over 61
  snapshots. Do not interpret this as bytes without checking the ESP-IDF API's
  unit for this build.
- Battery readings were usually around 4.17 V.

## Comparison procedure

For a later web-server test, keep the same board, Wi-Fi, telemetry source, and
dashboard-active state. Compare these metrics with the web server idle and while
serving a request. Use the same sample windows and report both averages and
maximums. The current baseline has no web server running.

## EXTRA RAW LOG
1- [Lấy telemetry hợp lệ: trung bình 40–41 ms, max khoảng 68–73 ms.
Cập nhật UI: trung bình 171–180 µs.
LVGL handler: khoảng 3.1 ms trung bình.
Flush ra màn hình: khoảng 4.3 ms trung bình.
Telemetry → flush: trung bình 19–28 ms, max khoảng 39–44 ms.
Heap còn ổn định khoảng 190 KB.]
2 - [Lấy telemetry hợp lệ: trung bình 42–48 ms, max khoảng 77–185 ms.
Parse JSON và cập nhật data: trung bình 4.3 ms.
Cập nhật UI: khoảng 0.18–0.24 ms.
Telemetry đến lần flush TFT: trung bình 17–25 ms, max khoảng 38–45 ms.
Heap còn khoảng 187–189 KB, stack DisplayTask ổn định khoảng 7,020.]