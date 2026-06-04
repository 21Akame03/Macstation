#include "encoder.hpp"

#include <iostream>

namespace {
constexpr int kPixelFormat = TJPF_BGRA;   // matches kCVPixelFormatType_32BGRA
} // namespace

MjpegEncoder::MjpegEncoder(int quality, int subsamp) {
    handle_ = tj3Init(TJINIT_COMPRESS);
    if (!handle_) {
        std::cerr << "tj3Init failed: " << tjGetErrorStr() << "\n";
        return;
    }
    tj3Set(handle_, TJPARAM_QUALITY,   quality);
    tj3Set(handle_, TJPARAM_SUBSAMP,   subsamp);
    tj3Set(handle_, TJPARAM_NOREALLOC, 0);   // let TJ grow the output buffer
}

MjpegEncoder::~MjpegEncoder() {
    if (jpegBuf_) tj3Free(jpegBuf_);
    if (handle_)  tj3Destroy(handle_);
}

bool MjpegEncoder::encode(const uint8_t* bgra, int width, int height, int pitch,
                          uint8_t*& outJpeg, size_t& outSize) {
    if (!handle_ || !bgra) return false;

    // tj3Compress8 reallocs jpegBuf_ as needed (NOREALLOC=0); passing the
    // previous allocation lets it get reused/grown across frames.
    if (tj3Compress8(handle_,
                     bgra,
                     width,
                     pitch,
                     height,
                     kPixelFormat,
                     &jpegBuf_,
                     &jpegSize_) != 0) {
        std::cerr << "tj3Compress8 failed: " << tj3GetErrorStr(handle_) << "\n";
        return false;
    }

    outJpeg = jpegBuf_;
    outSize = jpegSize_;
    return true;
}
