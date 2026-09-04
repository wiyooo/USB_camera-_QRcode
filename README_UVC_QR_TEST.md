# GC2145 测试版：电脑相机预览 + ESP_LOGI 二维码日志

> 用户已确认二维码解码成功，当前默认已恢复 [USB HID 键盘模式](README_QR_USB.md)。本页保留视频测试方法；需要重新测试预览时，显式设置 `CAMERA_UVC_QR_TEST=ON`。通过 VS Code 构建还需同步修改 `.vscode/settings.json` 中同名参数。

这版用于检查摄像头究竟拍到了什么，以及 MCU 有没有识别出二维码。启用 `CAMERA_UVC_QR_TEST=ON` 时，USB 表现为原来的 UVC 摄像头。

```text
GC2145 → RGB565 320×240 ─┬→ MCU quirc 识别 → ESP_LOGI → UART0
                        └→ 软件 JPEG → USB UVC → Windows 相机
```

## 先烧录这一版

已编译固件位于 **`build_uvc_qr_test`**。推荐使用 ESP-IDF 终端，在工程根目录执行：

```powershell
idf.py -B build_uvc_qr_test -D SDKCONFIG=build_uvc_qr_test/sdkconfig -D CAMERA_UVC_QR_TEST=ON build
idf.py -B build_uvc_qr_test -p COM实际下载端口 flash
```

若原生 USB 当前运行 HID/UVC，没有下载 COM 口：按住 BOOT，按一下 RESET，再松开 BOOT，进入下载模式。烧录完复位运行。

使用图形烧录工具时，选择 ESP32-S3、DIO、40MHz、16MB：

| 文件 | 烧录地址 |
| --- | --- |
| `build_uvc_qr_test/gc2145_uvc_qr_test_merged.bin`（合并固件） | **`0x0`** |

合并固件包含 bootloader、分区表及应用，地址间的填充会覆盖旧 NVS。需要保留旧 NVS 时，使用上面的 `idf.py flash` 分文件烧录。单独的 `lcd_camera.bin` 应用地址是 `0x10000`，不能把它当合并固件烧到 `0x0`。

## 看画面、看日志

1. 使用 USB 数据线连接板子的原生 USB 口。USB 信号沿用原工程：GPIO19 为 D-，GPIO20 为 D+。
2. 打开 Windows **相机**，切换到 **ESP32-S3 GC2145 Camera**（也可能显示通用 USB 摄像头名称），确认能够看到实时画面。分辨率为 320×240。
3. 打开 UART0 对应的串口，设置 **115200、8N1、无流控**，查看 `ESP_LOGI`。日志编码选择 UTF-8。
4. 摄像头对准 [测试二维码](tools/test_qr.png)。在预览中确认二维码完整、清晰、四周留白；尽量让码占画面高度的一半以上，避免反光。
5. 正常识别会打印 `QR text: USB-QR-123456`。持续对准同一码，大约每 2 秒打印一次，方便确认持续运行。

**本测试版由电脑相机请求驱动取帧：必须保持预览打开，二维码扫描才会运行。** UVC 声明沿用 20 FPS；加入识别后实际帧率可能降低，不保证达到 20 FPS。

日志仍然是普通 `ESP_LOGI`，没有新增串口协议。原生 USB 此时用于 UVC；若平时通过 USB Serial/JTAG 看日志，需要改用独立的 UART0 调试口/USB 转串口。按原工程的调试接口标注：

| 板子调试接口 | 3.3V USB 转串口 |
| --- | --- |
| TP2 / U0_TX | RX（看日志需要） |
| TP3 / GND | GND |
| TP1 / U0_RX | TX（仅看日志可以不接） |

如果板子已带 UART0 转 USB 电路，直接使用它的 COM 口。只连接原生 UVC USB 口，不能通过该口的 COM 监视器看到本版应用日志。

## 日志含义

启动时应看到：

```text
gc2145_camera: GC2145 TEST v1: USB UVC preview + QR decode to UART0 (115200)
gc2145_camera: GC2145 ready: RGB565 320x240 ...
qr_preview: QR test ready: open the PC Camera app to start capture and scanning
usb_camera: GC2145 USB camera ready ...
```

识别成功示例（时间戳和计数随运行变化）：

```text
I (...) qr_preview: QR decoded: bytes=13 version=1 type=2 eci=0
I (...) qr_preview: QR text: USB-QR-123456
I (...) qr_preview: scans=8 candidates=8 decoded=8 gray=0..255 mean=180 work_ms=35 stack_free=...
I (...) usb_camera: UVC frames=30 jpeg=5200 bytes qr=on free=...
```

| 日志 | 怎么判断 |
| --- | --- |
| `UVC frames` 持续增加 | 正在取到合格 RGB565 帧并生成 JPEG；电脑收到并显示还要看预览确认 |
| `scans` 增加，`candidates=0` | 扫描在运行，尚未找到二维码网格；查看画面清晰度、大小和光照 |
| `candidates` 增加，`decoded=0` | 找到候选网格，但内容解码失败；日志会给出解码错误 |
| `decoded` 增加、出现 `QR text` | MCU 已成功解码并通过 ESP_LOGI 打印 |
| `gray` 最小值/最大值很接近 | 画面对比度低；结合预览看是否黑屏、过曝或没对准 |
| `Camera frame timeout` / `Camera init failed` | 优先排查摄像头采集和连接，保留完整启动日志 |
| `qr=OFF` / `QR init failed` | 识别内存初始化失败，已退回纯预览，需把启动日志发回来 |

日志最多显示二维码内容前 256 字节，超长会明确提示截断，解码本身不受此日志长度限制。NUL、换行等控制字节以及反斜杠显示为 `\xHH`，避免日志被内容截断；UTF-8 文本直接输出。

## 实现和复现

- `main/usb_camera.c`：沿用 UVC 回调，保留摄像头帧直到识别和 JPEG 编码都完成。
- `main/qr_preview.c`：接收这同一帧，RGB565 转灰度、quirc 解码、`ESP_LOGI` 输出。识别间隔至少 250ms。
- quirc 解码使用独立的 16KiB 栈任务；UVC 等待信号量完成，不占用原 UVC 的 USB 完成通知。
- 无 PSRAM：153600 字节 RGB565 帧，76800 字节灰度/JPEG/USB 共用缓冲，另有 quirc 工作区、任务栈及驱动开销。每轮识别完成后才把共用缓冲改写为 JPEG；USB 传输完成后才进行下一帧。

本地模拟测试使用实际识别与 JPEG 源码：

```powershell
.\.venv-qr-test\Scripts\python.exe tests/native_uvc_smoke.py --qr-preview
.\.venv-qr-test\Scripts\python.exe tests/native_uvc_smoke.py
```

测试范围和编译结果见 [UVC_QR_TEST_VALIDATION.md](UVC_QR_TEST_VALIDATION.md)。电脑模拟和编译不能替代实际板子的出图、对焦及扫码验证。

切回之前的 HID 功能：

```powershell
idf.py -B build_hid -D CAMERA_UVC_QR_TEST=OFF -D CAMERA_OUTPUT_QR_USB=ON build
```
