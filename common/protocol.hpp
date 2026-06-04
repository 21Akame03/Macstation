#pragma once
#include <cstdint>

namespace Pipette {

static constexpr uint32_t MAGIC = 0xDEADF00D;
static constexpr uint8_t VERSION = 1;
static constexpr const char * PORT = "5000";

enum class PacketType: uint8_t {
    Video = 0,
    Audio = 1, // STEREO
    Control = 2,
};

enum class Encoding: uint8_t {
    Raw = 0,
    MJPEG = 1,
};

#pragma pack(push, 1)
struct PacketHeader {
    uint32_t magic;
    uint8_t version;
    PacketType type;
    uint16_t flags;
    uint32_t payload_size;
};

struct FrameHeader {
    uint32_t frame_id;
    uint64_t timestamp_us;
    uint16_t width;
    uint16_t height;
    Encoding encoding;
    uint16_t checksum;
};
#pragma pack(pop)
}
