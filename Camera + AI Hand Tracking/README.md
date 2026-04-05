# Camera + AI Hand Tracking Demo

這個專案使用 C++ (`libcamera` + `OpenCV`) 擷取雙串流影像，並透過 Unix Domain Socket 把低解析度影像送到 Python (`MediaPipe Hands`) 做手部關鍵點推論，再把結果回傳到 C++ 疊圖顯示。

## Environment

常用相機測試工具：

- `rpicam-hello`: 啟動相機預覽。
- `rpicam-jpeg`: 開預覽並拍攝高解析度 JPEG。
- `rpicam-still`: 相容於 `raspistill` 類型的拍照流程。
- `rpicam-vid`: 錄影。
- `rpicam-raw`: 擷取感測器原始 Bayer 影像。

## File Overview

- `cam_export.cpp`
  - 相機主程式（producer + renderer）。
  - 建立兩路 stream：
    - Main stream: `1232x1232`，用於畫面顯示與疊圖。
    - AI stream: `256x256`，用於 AI 推論輸入。
  - 透過 `/tmp/cam.sock` 傳送 AI stream 的灰階影像給 Python。
  - 接收 Python 回傳的 21 個手部關鍵點（42 floats）後，在主畫面上畫骨架、關鍵點與 FPS。

- `cam_ai.py`
  - AI worker（consumer + inference）。
  - 連線到 `/tmp/cam.sock`，接收每張 `256x256` 灰階影像。
  - 使用 `MediaPipe Hands` 推論單手關鍵點。
  - 將資料打包為 `frame_idx + 42 floats` 回傳 C++。

- `run.sh`
  - 啟動與協調腳本。
  - 負責清除舊 socket、背景啟動 C++/Python、等待 socket ready。
  - 捕捉 `SIGINT/SIGTERM`，停止背景程序並做資源清理。

## Data Flow

1. `cam_export.cpp` 啟動相機與 UDS server (`/tmp/cam.sock`)。
2. `cam_ai.py` 連入 socket，等待 frame。
3. C++ 每完成一個 request：
   - 把 Main frame 暫存（等待 AI 結果對齊）。
   - 把 AI frame 傳給 Python。
4. Python 推論手部 landmarks，回傳同一個 `frame_idx` 的座標。
5. C++ 依 `frame_idx` 同步資料後疊圖並顯示。

## Build

```bash
g++ -o cam_export cam_export.cpp \
    $(pkg-config --cflags --libs libcamera opencv4) \
    -lpthread -I/usr/include/libcamera
```

## Run

```bash
./run.sh
```

## Notes

- IPC 路徑固定為 `/tmp/cam.sock`，若異常中斷，重新啟動前需清除殘留 socket（`run.sh` 已處理）。
- 目前 AI 影像大小固定為 `256x256`，`cam_export.cpp` 與 `cam_ai.py` 兩端需一致。
