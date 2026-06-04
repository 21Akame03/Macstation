#include "framebuffer.hpp"

#if defined(__linux__)

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <linux/kd.h>

#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

#include <jpeglib.h>

namespace Pipette {

// --- libjpeg error handling -------------------------------------------------
// The default libjpeg error handler calls exit() on a malformed image, which
// would take the whole Pi process down on a single corrupt frame. Swap it for
// a longjmp so show_jpeg() can just return false instead.
namespace {
struct JpegError {
    jpeg_error_mgr mgr;
    jmp_buf        escape;
};

void on_jpeg_error(j_common_ptr cinfo) {
    auto *err = reinterpret_cast<JpegError *>(cinfo->err);
    char msg[JMSG_LENGTH_MAX];
    (*cinfo->err->format_message)(cinfo, msg);
    std::cerr << "jpeg decode error: " << msg << std::endl;
    longjmp(err->escape, 1);
}

// Convert an 8-bit channel value into a field of `len` bits (e.g. RGB565).
inline uint32_t to_field(uint8_t v, uint32_t len) {
    return len >= 8 ? static_cast<uint32_t>(v) : (v >> (8 - len));
}

// Does /sys/class/vtconsole/vtcon<i>/name say it's the framebuffer console?
bool is_fbcon(int i) {
    char path[64];
    std::snprintf(path, sizeof(path),
                  "/sys/class/vtconsole/vtcon%d/name", i);
    int fd = ::open(path, O_RDONLY);
    if (fd < 0) return false;
    char buf[128];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    ::close(fd);
    if (n <= 0) return false;
    buf[n] = '\0';
    return std::strstr(buf, "frame buffer device") != nullptr;
}

// Write a single byte to a sysfs bind file. Uses only raw syscalls so it's
// safe to call from the signal handler (no allocation, no stdio buffering).
bool write_byte(const char *path, char value) {
    int fd = ::open(path, O_WRONLY);
    if (fd < 0) return false;
    ssize_t n = write(fd, &value, 1);
    ::close(fd);
    return n == 1;
}
} // namespace

Framebuffer::~Framebuffer() { close(); }

bool Framebuffer::open(const char *dev) {
    close();

    fd_ = ::open(dev, O_RDWR);
    if (fd_ < 0) {
        std::cerr << "framebuffer: open " << dev << " failed: "
                  << std::strerror(errno) << std::endl;
        return false;
    }

    fb_var_screeninfo var {};
    fb_fix_screeninfo fix {};
    if (ioctl(fd_, FBIOGET_VSCREENINFO, &var) != 0 ||
        ioctl(fd_, FBIOGET_FSCREENINFO, &fix) != 0) {
        std::cerr << "framebuffer: ioctl failed: " << std::strerror(errno)
                  << std::endl;
        close();
        return false;
    }

    if (var.bits_per_pixel < 16) {
        std::cerr << "framebuffer: unsupported depth "
                  << var.bits_per_pixel << " bpp (need >= 16)" << std::endl;
        close();
        return false;
    }

    xres_        = var.xres;
    yres_        = var.yres;
    bytes_pp_    = var.bits_per_pixel / 8;
    line_length_ = fix.line_length;
    r_off_ = var.red.offset;    r_len_ = var.red.length;
    g_off_ = var.green.offset;  g_len_ = var.green.length;
    b_off_ = var.blue.offset;   b_len_ = var.blue.length;

    mem_len_ = static_cast<size_t>(line_length_) * yres_;
    mem_ = static_cast<uint8_t *>(
        mmap(nullptr, mem_len_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0));
    if (mem_ == MAP_FAILED) {
        std::cerr << "framebuffer: mmap failed: " << std::strerror(errno)
                  << std::endl;
        mem_ = nullptr;
        close();
        return false;
    }

    std::cout << "framebuffer: " << xres_ << "x" << yres_ << " "
              << var.bits_per_pixel << "bpp, stride " << line_length_
              << std::endl;

    // Stop the kernel framebuffer console (fbcon) from repainting text/cursor
    // on top of our pixels. Two layers, because KD_GRAPHICS alone isn't enough
    // under the KMS/vc4 driver on Raspberry Pi OS:
    //   1. unbind fbcon from the framebuffer entirely (the reliable fix), and
    //   2. put the VT into graphics mode (belt-and-suspenders / older stacks).
    // Both need root (sysfs write / VT access) and are non-fatal if they fail.
    for (int i = 0; i < 8; ++i) {
        if (!is_fbcon(i)) continue;
        char bind_path[64];
        std::snprintf(bind_path, sizeof(bind_path),
                      "/sys/class/vtconsole/vtcon%d/bind", i);
        if (write_byte(bind_path, '0')) {
            std::snprintf(fbcon_bind_path_, sizeof(fbcon_bind_path_),
                          "%s", bind_path);
            fbcon_unbound_ = true;
        } else {
            std::cerr << "framebuffer: could not unbind fbcon (vtcon" << i
                      << "): " << std::strerror(errno)
                      << " (run as root? console may bleed through)"
                      << std::endl;
        }
        break;
    }

    tty_fd_ = ::open("/dev/tty0", O_RDWR);
    if (tty_fd_ >= 0) {
        if (ioctl(tty_fd_, KDSETMODE, KD_GRAPHICS) != 0) {
            ::close(tty_fd_);
            tty_fd_ = -1;
        }
    }

    clear();
    return true;
}

void Framebuffer::restore_console() {
    if (tty_fd_ >= 0) {
        ioctl(tty_fd_, KDSETMODE, KD_TEXT);
        ::close(tty_fd_);
        tty_fd_ = -1;
    }
    if (fbcon_unbound_) {
        write_byte(fbcon_bind_path_, '1');   // rebind fbcon -> console returns
        fbcon_unbound_ = false;
    }
}

void Framebuffer::close() {
    restore_console();
    if (mem_ && mem_ != MAP_FAILED) munmap(mem_, mem_len_);
    mem_ = nullptr;
    mem_len_ = 0;
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
}

void Framebuffer::clear() {
    if (mem_) std::memset(mem_, 0, mem_len_);
}

bool Framebuffer::show_jpeg(const uint8_t *jpeg, size_t len) {
    if (!is_open()) return false;

    jpeg_decompress_struct cinfo {};
    JpegError jerr {};
    cinfo.err = jpeg_std_error(&jerr.mgr);
    jerr.mgr.error_exit = on_jpeg_error;

    std::vector<uint8_t> rgb;
    int img_w = 0, img_h = 0;

    if (setjmp(jerr.escape)) {        // libjpeg bailed out via longjmp
        jpeg_destroy_decompress(&cinfo);
        return false;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, jpeg, static_cast<unsigned long>(len));
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }

    cinfo.out_color_space = JCS_RGB;  // 3 bytes/pixel, R,G,B
    // Favour decode speed over the last bit of fidelity -- this is streaming
    // video, so the fast integer IDCT and skipping fancy upsampling/smoothing
    // are imperceptible but noticeably cheaper on the Pi's CPU.
    cinfo.dct_method         = JDCT_IFAST;
    cinfo.do_fancy_upsampling = FALSE;
    cinfo.do_block_smoothing  = FALSE;
    jpeg_start_decompress(&cinfo);

    img_w = static_cast<int>(cinfo.output_width);
    img_h = static_cast<int>(cinfo.output_height);
    const int row_stride = img_w * cinfo.output_components;  // == img_w * 3
    rgb.resize(static_cast<size_t>(row_stride) * img_h);

    while (cinfo.output_scanline < cinfo.output_height) {
        uint8_t *row = rgb.data() +
                       static_cast<size_t>(cinfo.output_scanline) * row_stride;
        jpeg_read_scanlines(&cinfo, &row, 1);
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);

    blit_rgb(rgb.data(), img_w, img_h);
    return true;
}

void Framebuffer::blit_rgb(const uint8_t *rgb, int img_w, int img_h) {
    if (img_w <= 0 || img_h <= 0) return;

    // Scale-to-fit preserving aspect ratio (integer math, nearest neighbour).
    // dst size = round(img * scale), scale = min(xres/img_w, yres/img_h).
    const uint64_t sx = static_cast<uint64_t>(xres_) * img_h;
    const uint64_t sy = static_cast<uint64_t>(yres_) * img_w;
    int dst_w, dst_h;
    if (sx < sy) {                    // width-limited
        dst_w = static_cast<int>(xres_);
        dst_h = static_cast<int>(sx / img_w);   // xres * img_h / img_w
    } else {                          // height-limited
        dst_h = static_cast<int>(yres_);
        dst_w = static_cast<int>(sy / img_h);   // yres * img_w / img_h
    }
    if (dst_w <= 0 || dst_h <= 0) return;

    const int off_x = (static_cast<int>(xres_) - dst_w) / 2;
    const int off_y = (static_cast<int>(yres_) - dst_h) / 2;

    // Precompute the source-column byte offset for each destination column once
    // per frame. The naive form does `dx * img_w / dst_w` *per pixel* -- an
    // integer divide in the inner loop, which dominates the blit on the Pi's
    // CPU. Hoisting it to a per-column table turns the inner loop into lookups.
    col_off_.resize(static_cast<size_t>(dst_w));
    for (int dx = 0; dx < dst_w; ++dx)
        col_off_[dx] = (dx * img_w / dst_w) * 3;

    for (int dy = 0; dy < dst_h; ++dy) {
        const int sy_px = dy * img_h / dst_h;
        const uint8_t *src_row = rgb + static_cast<size_t>(sy_px) * img_w * 3;
        uint8_t *dst_row = mem_ +
            static_cast<size_t>(off_y + dy) * line_length_ +
            static_cast<size_t>(off_x) * bytes_pp_;

        for (int dx = 0; dx < dst_w; ++dx) {
            const uint8_t *p = src_row + col_off_[dx];
            const uint32_t pixel =
                (to_field(p[0], r_len_) << r_off_) |
                (to_field(p[1], g_len_) << g_off_) |
                (to_field(p[2], b_len_) << b_off_);

            uint8_t *out = dst_row + static_cast<size_t>(dx) * bytes_pp_;
            switch (bytes_pp_) {
                case 4: *reinterpret_cast<uint32_t *>(out) = pixel; break;
                case 2: *reinterpret_cast<uint16_t *>(out) =
                            static_cast<uint16_t>(pixel); break;
                case 3:
                    out[0] = static_cast<uint8_t>(pixel & 0xFF);
                    out[1] = static_cast<uint8_t>((pixel >> 8) & 0xFF);
                    out[2] = static_cast<uint8_t>((pixel >> 16) & 0xFF);
                    break;
                default: break;
            }
        }
    }
}

} // namespace Pipette

#else // !__linux__ -- stub so the pi target still builds on the macOS host

namespace Pipette {
Framebuffer::~Framebuffer() = default;
bool Framebuffer::open(const char *) { return false; }
bool Framebuffer::show_jpeg(const uint8_t *, size_t) { return false; }
void Framebuffer::restore_console() {}
void Framebuffer::close() {}
void Framebuffer::clear() {}
void Framebuffer::blit_rgb(const uint8_t *, int, int) {}
} // namespace Pipette

#endif
