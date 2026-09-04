# UVC + QR 日志测试版验证记录

日期：2026-09-04。本次验证在用户提出“以后不用编译，我自己来”之前已执行；后续由用户自行编译和烧录。

## 已完成

- ESP-IDF 5.4.4 / ESP32-S3 编译通过，模式为 `CAMERA_UVC_QR_TEST=ON`。没有执行烧录。
- 应用为 UVC 视频 + quirc 解码，ELF 中包含 `video_task`、`qr_preview_task`、`quirc_decode`；没有链接 `qr_hid_init`。
- `tests/native_uvc_smoke.py --qr-preview` 通过：实际 QR 工作任务、UVC 回调、quirc 和 JPEG 编码器搭配模拟摄像头、调度器及 USB 接口。英文、UTF-8 中文、包含 NUL/换行/反斜杠的数据各连续识别 4 次，核对实际 `ESP_LOGI` 内容；JPEG 覆盖灰度缓冲后下一轮仍能识别，Pillow 能读取各输出图片。
- 同一测试覆盖任务/信号量分配失败清理、USB 初始化失败清理、QR 失败后退回纯预览、取帧超时、错误帧归还、扫描节流、空白画面、缓冲复用及启停回调。
- `tests/native_uvc_smoke.py` 纯 UVC 回归通过：实际 JPEG 编码、容量边界、RGB565 字节序与色条颜色、噪声画面、帧归还和初始化失败清理。噪声 JPEG 为 52108 字节，小于 65536 字节缓冲。
- 使用本次 Xtensa 编译参数的 `-fstack-usage` 检查：`quirc_decode` 单函数栈 9312 字节，`quirc_flip` 4000 字节，日志格式化函数 1104 字节，UVC 取帧回调 80 字节，JPEG `convert_image` 1168 字节。QR 使用独立 16KiB 任务栈；实际最大调用链与系统日志开销仍需上板观察 `stack_free`。

## 本次生成的固件

| 文件 | 字节数 | 烧录地址 |
| --- | ---: | --- |
| `build_uvc_qr_test/lcd_camera.bin` | 285936 | `0x10000` |
| `build_uvc_qr_test/gc2145_uvc_qr_test_merged.bin` | 351472 | `0x0` |

SHA-256：

```text
lcd_camera.bin
3e3e71a5165b0a7f46d97c72239cbec14bd808e08824b863461ea7f933cfa760

gc2145_uvc_qr_test_merged.bin
419e73cb9f0d49cf16dc6665cf2612a2fd02a058feab4c1a8c21aa596c2264fe
```

构建日志：`build_uvc_qr_test/validation.log`、`build_uvc_qr_test/package.log`。模拟日志：`.native-qr-test/uvc_qr_test.log`、`.native-qr-test/uvc_test.log`。

## 需要上板确认

没有连接实物验证 USB 枚举、Windows 相机出图、真实 QR 识别率、摄像头信号时序、实际帧率或剩余堆/栈空间。模拟测试中的空闲内存与耗时为桩函数数据，不能用作硬件性能结果。测试步骤见 [README_UVC_QR_TEST.md](README_UVC_QR_TEST.md)。
