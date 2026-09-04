# GC2145 二维码扫描器：USB HID Keyboard

> 当前默认已恢复 **USB HID 键盘扫码输出**：`CAMERA_UVC_QR_TEST=OFF`、`CAMERA_OUTPUT_QR_USB=ON`。上电后自动采集、识别，无需打开电脑相机。本次只切换源码配置，没有重新编译或烧录，已有 bin 不代表本次构建结果。

HID 模式功能：**摄像头扫码后，ESP32-S3 模拟 USB 键盘，将内容输入电脑当前光标位置，末尾自动按回车。**

```text
二维码 → GC2145 → DVP RGB565 320×240 → 灰度图 → quirc 解码
       → US 键盘按键映射 → TinyUSB HID Keyboard → 电脑当前输入框
```

电脑使用系统自带键盘驱动，日常使用不需要 Python、串口助手或专用接收软件。Windows 提供标准 HID 键盘驱动，依据 [微软 HID 文档](https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/hid-architecture)。

## 使用步骤

1. 烧录下面的 HID 固件，连接 USB 数据口。
2. 打开记事本，点击编辑区，让光标处于输入状态。
3. 将电脑切换到 **英文 US 键盘布局/英文输入模式**。
4. 摄像头对准 [测试二维码](tools/test_qr.png)，应输入 `USB-QR-123456`，然后换行。
5. 持续对准同一码只输入一次；移开约 1 秒后再对准，可再次输入。

也可以输入 Excel 当前单元格或软件的条码输入框。回车的作用由当前软件决定：记事本换行，表格通常移动到下一单元格。自动格式转换由电脑软件决定，例如需要保留编号前导零时，应把单元格设为文本。

## 支持的内容

- 英文字母、数字、空格和全部可打印 ASCII 符号，适合编号、网址、普通文本。
- 支持 Tab、CR/LF。CRLF 合并为一次 Enter；内容本身已经以回车/换行结尾时，不再额外追加 Enter。
- 每个字符都先发送按下报告，再发送释放报告；连续相同字符也能逐个输入。
- 根据电脑发送的 Caps Lock 状态调整字母的 Shift，不主动切换电脑 Caps Lock。
- 每个二维码最多 **1024 字节**；空内容、超限内容和不支持字符不会被截断输入。

**中文、其他非 ASCII 字符和任意二进制内容，当前版本整段拒绝输入。** HID 键盘发送的是按键，不能直接把 UTF-8 字节当成中文键入。识别库仍可解出这些内容，但键盘输出会拒绝，UART0 统计中的 `unsupported` 增加。若后续要求中文，需要明确目标操作系统及其 Unicode 输入方式，另外实现对应方案。

键盘映射按 US 布局设计。中文输入法处于中文状态时，字母可能被输入法组合；其他国家的键盘布局可能产生不同符号。

## 硬件连接

保留现有 ESP32-S3 + GC2145、24MHz XCLK、16MB Flash、无 PSRAM 配置和摄像头 GPIO。

| USB 信号 | ESP32-S3 |
| --- | --- |
| D- | GPIO19 |
| D+ | GPIO20 |
| GND | 共地 |

使用支持数据传输的 USB 线。上述 USB 引脚依据 [乐鑫 ESP-IDF 文档](https://docs.espressif.com/projects/esp-idf/en/v5.4.4/esp32s3/api-guides/usb-serial-jtag-console.html)。本模式由 USB OTG 控制器运行 TinyUSB HID；UART0 用于调试日志。

旧的 `README_GC2145_UVC.md` 记录过 USB 网名、摄像头 RESETB 和供电问题，属于历史原理图检查，并不代表已确认现在的实物仍存在这些问题。若串口还在报 `Camera probe failed`，先修复摄像头采集，再测试扫码。

## 编译和烧录

现有 `build` 目录曾启用视频测试模式，CMake 会保留旧开关。第一次切回时，在工程目录执行以下命令自行构建，显式覆盖缓存：

```powershell
idf.py -B build -D CAMERA_UVC_QR_TEST=OFF -D CAMERA_OUTPUT_QR_USB=ON build
```

VS Code 的 `.vscode/settings.json` 也已加入这两个参数。使用扩展时先运行 `ESP-IDF: Run idf.py reconfigure task`，再构建、烧录；启动日志应包含 `USB HID keyboard`、`HID keyboard mounted`。若仍显示 `GC2145 TEST v1`，说明运行的仍是视频测试固件。

使用 ESP-IDF 5.4.x PowerShell 终端，本机验证环境为 5.4.4。在工程目录执行：

```powershell
cd 'C:\Users\Lenovo\Desktop\USB_camera _QRcode'
idf.py -B build_hid -D CAMERA_UVC_QR_TEST=OFF -D CAMERA_OUTPUT_QR_USB=ON build
idf.py -B build_hid -p COM实际端口 flash
```

把 `COM实际端口` 替换为设备管理器中的**下载模式端口**。HID 应用运行时，这个原生 USB 口表现为键盘，通常没有应用 COM 口。再次下载时，按住 BOOT，按一下 RESET，再松开 BOOT，进入 ROM 下载模式；烧录后复位运行。UART0 调试适配器的串口则可以独立使用。

此次验证的配置副本为 `build_hid/sdkconfig`，其路径已经保存在该构建目录的 CMake 缓存中。默认 `build` 和此前的 `build_qr` 目录不是最终 HID 固件输出目录。

| 固件文件 | 地址 |
| --- | --- |
| `build_hid/bootloader/bootloader.bin` | `0x0` |
| `build_hid/partition_table/partition-table.bin` | `0x8000` |
| `build_hid/lcd_camera.bin` | `0x10000` |
| `build_hid/gc2145_qr_hid_merged.bin`（整包） | `0x0` |

优先使用 `idf.py ... flash` 按正确地址分文件烧录。不要把单独的 `lcd_camera.bin` 写到 `0x0`。整包包含地址间的填充区，会覆盖旧 NVS；保留旧数据时使用分文件烧录。生成整包命令：

```powershell
idf.py -B build_hid merge-bin -o gc2145_qr_hid_merged.bin
```

## 扫码与异常行为

每帧选择第一个成功解码的二维码，一次展示一个码。超过 1000ms 没有成功识别到同一码后允许重扫；持续识别失败超过这段时间，也可能触发再次输出。扫描另一个码立即重新输出。

字符输出由 USB 任务逐份报告完成确认后推进。输入期间暂停下一轮扫码，不积累离线扫码队列。USB 挂起、断开或报告超过 1500ms 无进展时，中止本次输入，恢复连接后先发送全键释放报告；不会在 USB 任务内部重放已经发送过的前缀。若中途断开，电脑可能已有半段文字，需清除后移开二维码再重新扫描。

重新连接不补发旧请求。USB 已收到键盘报告只表示主机传输完成，不代表目标业务软件已经保存或处理内容。上板先用记事本验证输入，再接入业务软件。

## 实现位置

| 文件 | 功能 |
| --- | --- |
| `main/qr_scanner.c` | 采集、解码、重复码抑制、交给 HID 输出 |
| `main/qr_payload.c` / `.h` | RGB565 转灰度和重复码状态 |
| `main/qr_keyboard.c` / `.h` | 字符映射、按下/释放顺序、回车后缀 |
| `main/usb_hid/qr_hid.c` | USB PHY、TinyUSB 任务、断开和超时恢复 |
| `main/usb_hid/usb_descriptors.c` | 一个 Boot Keyboard 接口，8 字节报告 |
| `main/usb_hid/tusb_config.h` | 启用 HID，关闭 CDC 和 UVC 类 |
| `components/quirc` | 固定 quirc v1.2 源码和 ISC 许可 |

默认 USB 产品字符串为 `GC2145 QR HID Keyboard`，序列号由芯片 MAC 派生。是否在设备管理器显示这个字符串或通用的 HID Keyboard Device，由系统界面决定。

扫码模式的 TinyUSB 只使用键盘描述符。ESP-IDF 5.4 会构建所有托管依赖，原 UVC 组件还会注入描述符源码，因此根 CMake 在所有组件注册后移除这项注入；保留托管组件文件及原 UVC 模式。编译模式通过 IDF 构建属性传入依赖预扫描，确保预扫描与真正编译一致。

若不需要自动回车，把 `main/usb_hid/qr_hid.c` 的 `HID_APPEND_ENTER` 改为 `false`。去重时间在 `main/qr_payload.h` 的 `QR_REARM_MS` 中调整。

## 内存与调试

保持一个内部 RAM RGB565 帧（153600 字节），一次分配 quirc 灰度图（76800 字节）和工作区。扫码任务栈 16KiB，USB 任务栈 4KiB，HID 请求队列深度为 1。另有摄像头 DMA、解码结果静态区和系统开销。运行时不生成 JPEG，不启动 Wi-Fi。

UART0 启动成功日志：`QR scanner ready`、`HID keyboard mounted`。每 10 秒统计 `frames / decoded / typed / oversized / unsupported / aborted / usb_busy / free / stack_free`。

320×240 适合先测试内容较短、尺寸较大、对焦清楚的二维码。1024 字节是输出缓存上限，不代表同等长度的密集二维码都能在此分辨率可靠识别。实际识别速度、距离和内存/栈余量以实机测试为准。

## 恢复视频模式

```powershell
# USB UVC 摄像头
idf.py -B build_uvc_check -D CAMERA_UVC_QR_TEST=OFF -D CAMERA_OUTPUT_QR_USB=OFF -D CAMERA_OUTPUT_USB_UVC=ON build

# 原 Wi-Fi 网页视频
idf.py -B build_web -D CAMERA_UVC_QR_TEST=OFF -D CAMERA_OUTPUT_QR_USB=OFF -D CAMERA_OUTPUT_USB_UVC=OFF build
```

模式由 CMake 选项选择，无需修改头文件默认宏。

## 主机测试

Python 仅用于开发验证，日常扫码无需安装：

```powershell
python -m venv .venv-qr-test
.\.venv-qr-test\Scripts\python.exe -m pip install -r tests/requirements-qr.txt
.\.venv-qr-test\Scripts\python.exe tests/native_qr_smoke.py
.\.venv-qr-test\Scripts\python.exe tests/native_hid_smoke.py
```

第一项测试真实 RGB565 → quirc → HID 按键序列；第二项用模拟主机执行真实 USB 任务和描述符代码，验证完成回调、故障中止、按键释放及初始化清理。两项都不能替代硬件验收。具体结果见 [QR_VALIDATION.md](QR_VALIDATION.md)。
