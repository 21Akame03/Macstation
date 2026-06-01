#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>

namespace Pipette {

struct Frame {
    uint8_t* data;
    std::size_t size;
    uint32_t width;
    uint32_t height;
    uint64_t timestamp_us;
};

using FrameCallback = std::function<void(Frame&)>;

class Capture {
    public:
        void start(FrameCallback cb);
        void stop();
};

};
