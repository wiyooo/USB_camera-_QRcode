# quirc

Vendored, unmodified library files from [dlbeer/quirc v1.2](https://github.com/dlbeer/quirc/tree/542848dd6b9b0eaa9587bbf25b9bc67bd8a71fca), commit `542848dd6b9b0eaa9587bbf25b9bc67bd8a71fca`.

The ISC license is in LICENSE and in each source file. Only the ESP-IDF CMake wrapper is local. Compile with `QUIRC_FLOAT_TYPE=float` and `QUIRC_USE_TGMATH`; retain the default 254 regions so the processing pixels alias the grayscale image. The decoder and its image buffer are allocated once at startup.
