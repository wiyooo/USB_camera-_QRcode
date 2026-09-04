# USB HID 扫码模式验证记录

日期：2026-09-04。最终方案为 **MCU 解码 + USB HID Keyboard 输入**。

## 已通过

- ESP-IDF 5.4.4 / Xtensa GCC 14.2.0 编译 HID 模式成功，输出目录 `build_hid`，配置副本 `build_hid/sdkconfig`。
- 原 UVC 模式在最终 CMake 和依赖配置下重新编译成功，输出目录 `build_uvc_check`。网页模式代码保留，本次未重新编译。
- HID ELF 包含扫码入口、quirc、TinyUSB 键盘报告接口，并且设备描述符回调只有一个定义。没有链接 `uvc_device_init`、`usb_serial_jtag_write_bytes` 或 `esp_wifi_init`。
- HID 编译命令中不存在原 UVC 描述符源码。托管组件源文件未修改；兼容逻辑由根 CMake 处理。
- 修改过的已跟踪源码通过 `git diff --check`。

## 真实二维码及按键序列测试

命令：

```powershell
.\.venv-qr-test\Scripts\python.exe tests/native_qr_smoke.py
```

使用合成二维码构造 320×240 的 RGB565 帧，执行工程中的灰度转换、真实 quirc 解码和真实键盘序列代码，再用独立的 US 键盘模型重放报告并检查输出。

通过项目：

- 英文编号、URL、旋转、镜像二维码。
- 连续重复字符、大小写字母、全部 95 个可打印 ASCII 字符。
- Caps Lock 开/关、标点符号、Tab、CRLF 合并和回车后缀。
- 每个字符的按下/释放配对，未确认的报告不推进序列。
- 中文和包含 NUL/0xFF 的二进制二维码能正确解码，但在产生任何按键前整段拒绝。
- 空白画面无扫码结果，空载荷/超出 1024 字节边界被拒绝。
- RGB565 颜色转灰度、缓冲长度检查、重复码抑制、重新扫码和输出暂不可用时的重试资格。

## USB 任务及描述符测试

命令：

```powershell
.\.venv-qr-test\Scripts\python.exe tests/native_hid_smoke.py
```

直接编译实际 `qr_hid.c` 和 `usb_descriptors.c`，模拟 RTOS、USB 主机和时间，运行真实 USB 任务循环。通过项目：

- 队列分配、USB PHY、TinyUSB 初始化和任务创建失败时清理资源。
- 一个配置、一个 HID Boot Keyboard 接口、8 字节报告、10ms 端点轮询、有效字符串和芯片序列号。
- 正常完成、连续相同按键、短暂发送拒绝后重试。
- 完成回调超时、传输失败、断开、挂起/恢复后的全键释放。
- 异常时中止请求，USB 任务不重放已经发送过的前缀。
- 重新枚举后拒绝旧会话请求，Caps Lock LED 报告及 GET_REPORT。

模拟测试不等同于真实 USB 枚举、电气连接或电脑输入法测试。

## 固件

| 文件 | 大小 | 烧录地址 |
| --- | --- | --- |
| `build_hid/lcd_camera.bin` | 283136 字节 | `0x10000` |
| `build_hid/gc2145_qr_hid_merged.bin` | 348672 字节 | `0x0` |

SHA-256：

```text
lcd_camera.bin
04134E9130A11E5BF6B5E6E823564682386E4A69598F8407D13878CF99D79ECA

gc2145_qr_hid_merged.bin
3FC7E6647022DE38EF725C8B2E45B2B66CAAB038FE73B33E7CD08700670652B5
```

构建和打包日志：`build_hid/validation.log`。原 UVC 构建日志：`build_uvc_check/validation.log`。

## 栈和内存

Xtensa 编译器 `-fstack-usage` 静态结果：`quirc_decode` 自身为 9312 字节，`quirc_flip` 为 4000 字节，`qr_hid_type` 为 1088 字节。它们顺序调用；扫码任务分配 16384 字节栈，USB 任务分配 4096 字节栈。

静态报告不等同于运行时完整调用链峰值；剩余堆、内存碎片和任务栈水位还要根据实机 UART0 日志验收。

## 未完成的硬件验收

本次没有烧录、没有向真实电脑发送键盘输入。此前端口枚举仅发现蓝牙 COM3/COM4，没有开发板原生 USB 串口。

连接实物后，按 `README_QR_USB.md` 烧录 `build_hid` 的固件，在记事本中切换到英文 US 输入，扫描 `tools/test_qr.png`。应输入 `USB-QR-123456` 并换行。再测试同码去重、重新扫码、不同距离和光照、长字符串、USB 拔插，以及实际内存/栈余量。
