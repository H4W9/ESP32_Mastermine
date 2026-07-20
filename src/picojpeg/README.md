# picojpeg (vendored)

`picojpeg.c` / `picojpeg.h` by Rich Geldreich, **public domain**, copied
unmodified from Bodmer's `JPEGDecoder` Arduino library.

It is vendored rather than used through that library for one reason: picojpeg
supports a **reduce** mode (`pjpeg_decode_init(..., reduce = 1)`) that decodes
only the DC coefficient of each 8x8 block, skipping the AC dequantisation, the
IDCT and chroma upsampling entirely. That is a 1/8-scale decode, and skin
textures from the Mastermine store are 512x512 — so it lands on exactly the
64x64 we cache, at a fraction of the time and memory of a full decode.

`JPEGDecoder` hardcodes `reduce = 0`, so it cannot be asked for this.

Do not edit these two files; if picojpeg ever needs updating, re-copy them.
