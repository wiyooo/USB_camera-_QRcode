"""Actual C decoder -> actual HID sequence, with synthetic camera frames.

python -m pip install -r tests/requirements-qr.txt
python tests/native_qr_smoke.py
Optional: --cc gcc (instead of the ziglang wheel's portable C compiler).
"""

import argparse
import json
from pathlib import Path
import subprocess
import sys

import qrcode
from PIL import Image, ImageOps

ROOT = Path(__file__).resolve().parents[1]


def replay_reports(reports, caps_lock=False):
    """Independent US keyboard host model, with press/release assertions."""
    normal = {0x27: "0", 0x28: "\n", 0x2b: "\t", 0x2c: " ",
              0x2d: "-", 0x2e: "=", 0x2f: "[", 0x30: "]", 0x31: "\\",
              0x33: ";", 0x34: "'", 0x35: "`", 0x36: ",", 0x37: ".", 0x38: "/"}
    shifted = dict(zip(range(0x1e, 0x28), "!@#$%^&*()"))
    shifted.update({0x2d: "_", 0x2e: "+", 0x2f: "{", 0x30: "}", 0x31: "|",
                    0x33: ":", 0x34: '"', 0x35: "~", 0x36: "<", 0x37: ">", 0x38: "?"})
    normal.update({0x1e + n: str(n + 1) for n in range(9)})
    assert reports[0] == [0, 0] and reports[-1] == [0, 0]
    pressed = False
    result = ""
    for modifier, usage in reports:
        assert modifier in (0, 2), modifier
        if usage == 0:
            assert modifier == 0
            pressed = False
            continue
        assert not pressed, "Every character requires a key release"
        pressed = True
        if 0x04 <= usage <= 0x1d:
            letter = chr(ord("a") + usage - 0x04)
            result += letter.upper() if bool(modifier) != caps_lock else letter
        else:
            result += shifted[usage] if modifier else normal[usage]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", help="Native C compiler executable; otherwise use python -m ziglang cc")
    args = parser.parse_args()
    out = ROOT / ".native-qr-test"
    out.mkdir(exist_ok=True)
    exe = out / ("qr_test.exe" if sys.platform == "win32" else "qr_test")
    compiler = [args.cc] if args.cc else [sys.executable, "-m", "ziglang", "cc"]
    lib = ROOT / "components/quirc/lib"
    sources = [ROOT / "tests/native_qr_harness.c", ROOT / "main/qr_payload.c", ROOT / "main/qr_keyboard.c"]
    sources += [lib / name for name in ("quirc.c", "decode.c", "identify.c", "version_db.c")]
    subprocess.run([*compiler, "-std=c11", "-O2", "-UNDEBUG", "-DQUIRC_FLOAT_TYPE=float", "-DQUIRC_USE_TGMATH",
                    "-I", str(lib), "-I", str(ROOT / "main"), *map(str, sources), "-lm", "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

    all_keys = "".join(map(chr, range(32, 127))) + "\n\t\n"
    for caps in (False, True):
        record = json.loads(subprocess.check_output([str(exe), "--keys", str(int(caps))]))
        assert replay_reports(record["reports"], caps) == all_keys
    print("PASS: all 95 printable ASCII characters, Caps Lock, CRLF and Tab")

    cases = [
        ("ascii", b"USB-QR-123456", None),
        ("url", b"https://example.com/scan?id=123", None),
        ("chinese", "扫码成功：你好，电脑".encode(), None),
        ("binary", b"line1\nline2\r\n\x00\xff\"\\", None),
        ("rotated", b"ROTATED-QR", "rotate"),
        ("mirrored", b"MIRROR-QR", "mirror"),
        ("repeated", b"AAaa0011!!__", None),
        ("newline", b"A\r\nB\tC\n", None),
    ]
    for name, expected, transform in cases:
        qr = qrcode.QRCode(error_correction=qrcode.constants.ERROR_CORRECT_M, box_size=5, border=4)
        qr.add_data(expected, optimize=0)
        qr.make(fit=True)
        qr_image = qr.make_image().convert("L")
        assert qr_image.width <= 230 and qr_image.height <= 230
        canvas = Image.new("L", (320, 240), 255)
        canvas.paste(qr_image, ((320 - qr_image.width) // 2, (240 - qr_image.height) // 2))
        if transform == "rotate":
            canvas = canvas.rotate(12, resample=Image.Resampling.BICUBIC, fillcolor=255)
        elif transform == "mirror":
            canvas = ImageOps.mirror(canvas)
        canvas.save(out / f"{name}.png")
        # Feed the same big-endian RGB565 bytes delivered by the camera driver.
        rgb = bytearray()
        for v in canvas.tobytes():
            packed = ((v >> 3) << 11) | ((v >> 2) << 5) | (v >> 3)
            rgb.extend((packed >> 8, packed & 255))
        raw = out / f"{name}.rgb565"
        raw.write_bytes(rgb)
        run = subprocess.run([str(exe), str(raw)], capture_output=True)
        assert run.returncode == 0, (name, run.returncode, run.stderr.decode(errors="replace"))
        record = json.loads(run.stdout)
        payload = bytes.fromhex(record["hex"])
        assert payload == expected, (name, payload)
        if name in ("chinese", "binary"):
            assert not record["hid_supported"] and record["reports"] == []
            print(f"PASS: {name}: decoded correctly, rejected before any HID input")
        else:
            text = expected.decode().replace("\r\n", "\n").replace("\r", "\n")
            if not text.endswith("\n"):
                text += "\n"
            assert replay_reports(record["reports"]) == text, name
            print(f"PASS: {name}: decoded and typed with release + Enter")

    white = out / "no_qr.rgb565"
    white.write_bytes(b"\xff\xff" * 320 * 240)
    assert subprocess.run([str(exe), str(white)], capture_output=True).returncode == 2
    print("PASS: blank image has no scan result")


if __name__ == "__main__":
    main()
