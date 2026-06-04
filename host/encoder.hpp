#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include <turbojpeg.h>

// handoff between the encode side and the sender thread.
// holds the latest encoded JPEG; sender grabs it and ships it.
// jpeg owns its bytes so it lives independent of the encoder's reused buffer.
struct jpegFrame {
    std::mutex mtx;
    std::condition_variable cv;

    std::vector<uint8_t> jpeg;   // compressed JPEG output, owned here

    // metadata for the FrameHeader, copied in alongside the JPEG
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t timestamp_us = 0;

    bool newframe_present = false;
    bool stop = false;
};

// single-frame JPEG compressor. MJPEG is just a stream of these JPEGs, so
// encode() each captured frame and send the result.
//
// not thread-safe: one encoder per thread. the output buffer is owned by the
// encoder and reused across calls, so it's only valid until the next encode()
// on the same instance -- copy it out if it needs to live longer.
class MjpegEncoder {
public:
    explicit MjpegEncoder(int quality = 80, int subsamp = TJSAMP_420);
    ~MjpegEncoder();

    MjpegEncoder(const MjpegEncoder&) = delete;
    MjpegEncoder& operator=(const MjpegEncoder&) = delete;

    // compress one BGRA frame to JPEG. on success returns true and points
    // outJpeg/outSize at the internal buffer (valid until the next encode()).
    // pitch is bytes per row of bgra; pass 0 if tightly packed.
    bool encode(const uint8_t* bgra, int width, int height, int pitch,
                uint8_t*& outJpeg, size_t& outSize);

    bool valid() const { return handle_ != nullptr; }

private:
    tjhandle handle_ = nullptr;
    uint8_t* jpegBuf_ = nullptr;   // owned, freed with tj3Free
    size_t   jpegSize_ = 0;        // current allocation size for jpegBuf_
};
