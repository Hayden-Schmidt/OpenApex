// Simulator-only: dump the rendered LVGL screen to a PNG so a design pass can be reviewed without
// watching the live SDL window. Deliberately self-contained -- LV_USE_LODEPNG is off (and is a
// decoder anyway), and pulling in a real PNG/zlib dependency for a debug affordance isn't worth it.
// The encoder below emits an uncompressed ("stored" deflate) PNG: a few hundred kB per 240x240
// frame, which is fine because --shot always overwrites one fixed file rather than accumulating.
#include "shot.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

extern "C" {
#include "lvgl.h"
}

namespace {

uint32_t crc32_of(const uint8_t *data, size_t len, uint32_t crc = 0xFFFFFFFFu) {
    static uint32_t table[256];
    static bool built = false;
    if (!built) {
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[n] = c;
        }
        built = true;
    }
    for (size_t i = 0; i < len; ++i) {
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc;
}

void push_be32(std::vector<uint8_t> &out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v >> 24));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

void push_chunk(std::vector<uint8_t> &out, const char type[4], const std::vector<uint8_t> &body) {
    push_be32(out, static_cast<uint32_t>(body.size()));
    const size_t type_at = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), body.begin(), body.end());
    const uint32_t crc = crc32_of(out.data() + type_at, 4 + body.size()) ^ 0xFFFFFFFFu;
    push_be32(out, crc);
}

// Wraps `raw` (already-filtered PNG scanlines) in a zlib stream using stored deflate blocks, so no
// compressor is needed.
std::vector<uint8_t> zlib_stored(const std::vector<uint8_t> &raw) {
    std::vector<uint8_t> z;
    z.push_back(0x78); // CM=8 (deflate), CINFO=7 (32k window)
    z.push_back(0x01); // FCHECK so (0x78<<8|0x01) % 31 == 0, no preset dict, fastest level
    constexpr size_t kMaxBlock = 65535;
    size_t off = 0;
    do {
        const size_t n = raw.size() - off < kMaxBlock ? raw.size() - off : kMaxBlock;
        const bool final_block = (off + n) >= raw.size();
        z.push_back(final_block ? 1 : 0);
        z.push_back(static_cast<uint8_t>(n & 0xFF));
        z.push_back(static_cast<uint8_t>(n >> 8));
        z.push_back(static_cast<uint8_t>(~n & 0xFF));
        z.push_back(static_cast<uint8_t>((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
        off += n;
    } while (off < raw.size());

    uint32_t a = 1;
    uint32_t b = 0;
    for (uint8_t byte : raw) {
        a = (a + byte) % 65521u;
        b = (b + a) % 65521u;
    }
    push_be32(z, (b << 16) | a);
    return z;
}

bool write_png_rgb(const char *path, const uint8_t *rgb, int32_t w, int32_t h) {
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(h) * (1 + static_cast<size_t>(w) * 3));
    for (int32_t y = 0; y < h; ++y) {
        raw.push_back(0); // filter type 0 (None)
        const uint8_t *row = rgb + static_cast<size_t>(y) * static_cast<size_t>(w) * 3;
        raw.insert(raw.end(), row, row + static_cast<size_t>(w) * 3);
    }

    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

    std::vector<uint8_t> ihdr;
    push_be32(ihdr, static_cast<uint32_t>(w));
    push_be32(ihdr, static_cast<uint32_t>(h));
    ihdr.push_back(8); // bit depth
    ihdr.push_back(2); // colour type 2 = truecolour RGB
    ihdr.push_back(0); // deflate
    ihdr.push_back(0); // filter method 0
    ihdr.push_back(0); // no interlace
    push_chunk(png, "IHDR", ihdr);
    push_chunk(png, "IDAT", zlib_stored(raw));
    push_chunk(png, "IEND", {});

    FILE *f = std::fopen(path, "wb");
    if (f == nullptr) {
        std::fprintf(stderr, "shot: cannot open %s for writing\n", path);
        return false;
    }
    const size_t written = std::fwrite(png.data(), 1, png.size(), f);
    std::fclose(f);
    if (written != png.size()) {
        std::fprintf(stderr, "shot: short write to %s\n", path);
        return false;
    }
    return true;
}

} // namespace

bool shot_write_active_screen(const char *path) {
    lv_obj_t *screen = lv_screen_active();
    if (screen == nullptr) {
        std::fprintf(stderr, "shot: no active screen\n");
        return false;
    }

    lv_obj_update_layout(screen);
    const int32_t w = lv_obj_get_width(screen);
    const int32_t h = lv_obj_get_height(screen);
    if (w <= 0 || h <= 0) {
        std::fprintf(stderr, "shot: active screen has zero size\n");
        return false;
    }

    // Back the snapshot with our own C-heap allocation rather than letting lv_snapshot_take()
    // lv_draw_buf_create() it: LV_USE_STDLIB_MALLOC is LV_STDLIB_BUILTIN with a 64kB LV_MEM_SIZE
    // pool (sized to mirror the C3), and a 240x240 ARGB8888 frame is ~230kB, so the pool
    // allocation fails. Keeping this buffer outside the pool also means screenshotting can't
    // perturb the allocation behaviour the sim exists to reproduce.
    const uint32_t stride = lv_draw_buf_width_to_stride(static_cast<uint32_t>(w),
                                                        LV_COLOR_FORMAT_ARGB8888);
    std::vector<uint8_t> storage(stride * static_cast<size_t>(h) + LV_DRAW_BUF_ALIGN);
    lv_draw_buf_t draw_buf;
    if (lv_draw_buf_init(&draw_buf, static_cast<uint32_t>(w), static_cast<uint32_t>(h),
                         LV_COLOR_FORMAT_ARGB8888, stride, storage.data(),
                         static_cast<uint32_t>(storage.size())) != LV_RESULT_OK) {
        std::fprintf(stderr, "shot: lv_draw_buf_init failed\n");
        return false;
    }

    // Snapshot rather than reading back the SDL texture: it captures the LVGL scene at full
    // resolution regardless of what the window manager did to the window, and works if the sim is
    // ever run without a visible window.
    lv_draw_buf_t *buf = &draw_buf;
    if (lv_snapshot_take_to_draw_buf(screen, LV_COLOR_FORMAT_ARGB8888, buf) != LV_RESULT_OK) {
        std::fprintf(stderr, "shot: lv_snapshot_take_to_draw_buf failed\n");
        return false;
    }
    // The top layer is drawn over every screen (page transitions such as the arrived reveal live
    // there), so it is snapshotted too and composited over the screen by its own alpha.
    std::vector<uint8_t> top_storage(storage.size());
    lv_draw_buf_t top_buf;
    const bool have_top =
        lv_draw_buf_init(&top_buf, static_cast<uint32_t>(w), static_cast<uint32_t>(h),
                         LV_COLOR_FORMAT_ARGB8888, stride, top_storage.data(),
                         static_cast<uint32_t>(top_storage.size())) == LV_RESULT_OK &&
        lv_snapshot_take_to_draw_buf(lv_layer_top(), LV_COLOR_FORMAT_ARGB8888, &top_buf) ==
            LV_RESULT_OK;

    std::vector<uint8_t> rgb(static_cast<size_t>(w) * static_cast<size_t>(h) * 3);
    for (int32_t y = 0; y < h; ++y) {
        const uint8_t *src = buf->data + static_cast<size_t>(y) * buf->header.stride;
        const uint8_t *top =
            have_top ? top_buf.data + static_cast<size_t>(y) * top_buf.header.stride : nullptr;
        uint8_t *dst = rgb.data() + static_cast<size_t>(y) * static_cast<size_t>(w) * 3;
        for (int32_t x = 0; x < w; ++x) {
            // ARGB8888 is stored B,G,R,A in memory. Alpha is dropped: the panel is opaque, and a
            // screenshot with a transparent background reviews badly against a dark design ref.
            for (int c = 0; c < 3; ++c) {
                const int v = src[x * 4 + 2 - c];
                if (top == nullptr) {
                    dst[x * 3 + c] = static_cast<uint8_t>(v);
                    continue;
                }
                const int a = top[x * 4 + 3];
                dst[x * 3 + c] = static_cast<uint8_t>((top[x * 4 + 2 - c] * a + v * (255 - a)) / 255);
            }
        }
    }
    // No lv_draw_buf_destroy() here -- `storage` owns the pixels, not LVGL.

    const bool ok = write_png_rgb(path, rgb.data(), w, h);
    if (ok) {
        std::printf("shot: wrote %s (%dx%d)\n", path, w, h);
    }
    return ok;
}
