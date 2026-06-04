#pragma once
#include <cstddef>
#include <cstdint>

namespace Pipette {

// Owns an mmap'd Linux framebuffer (/dev/fb0) and draws decoded JPEG frames to
// it. The Linux-only implementation lives in framebuffer.cpp behind a
// __linux__ guard; on other platforms (e.g. the macOS host build, which also
// compiles the pi target) every method is a no-op so callers can fall back to
// writing the frame to disk instead.
class Framebuffer {
public:
    Framebuffer() = default;
    ~Framebuffer();

    Framebuffer(const Framebuffer &) = delete;
    Framebuffer &operator=(const Framebuffer &) = delete;

    // Open and mmap the framebuffer device. Returns false (and leaves the
    // object closed) if the device can't be used.
    bool open(const char *dev = "/dev/fb0");
    bool is_open() const { return mem_ != nullptr; }

    // Decode a JPEG buffer and blit it to the screen, aspect-correct and
    // centered, scaled to fit. Returns false on decode failure or if closed.
    bool show_jpeg(const uint8_t *jpeg, size_t len);

private:
    void close();
    void clear();                                   // paint whole screen black
    // Blit a decoded RGB24 image, scaled-to-fit and centered.
    void blit_rgb(const uint8_t *rgb, int img_w, int img_h);

    int      fd_  = -1;
    uint8_t *mem_ = nullptr;
    size_t   mem_len_ = 0;

    uint32_t xres_ = 0, yres_ = 0;        // visible resolution
    uint32_t bytes_pp_ = 0;               // bytes per pixel (2/3/4)
    uint32_t line_length_ = 0;            // stride in bytes

    // RGB field placement within a pixel, from the var screen info.
    uint32_t r_off_ = 0, g_off_ = 0, b_off_ = 0;
    uint32_t r_len_ = 0, g_len_ = 0, b_len_ = 0;
};

} // namespace Pipette
