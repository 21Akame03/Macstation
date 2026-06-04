/*
 *
 */

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <cstring>
#include <cstdint>
#include <mutex>
#include <netinet/in.h>
#include <ostream>
#include <random>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <thread>
#include <unistd.h>
#include "../common/protocol.hpp"
#include "capture.hpp"
#include "encoder.hpp"

#define PI_IP "192.168.7.1"
#define PI_PORT "5000"

//
// 1. capture the frame using ScreenCaptureKit
// 2. Encode the Frame
// 3. Lock, overwite and unlock
// 4. nitofy socket thread to send the frame


void capture_thread(jpegFrame &f) {
    Pipette::Capture capture;

    // one encoder per thread, reused for every frame
    MjpegEncoder encoder;

    capture.start([&](Pipette::Frame& frame) {
        std::cout << "Frame: " << frame.width << "x" << frame.height
                  << " size= " << frame.size << " ts = " << frame.timestamp_us << " \n";

        //
        // 1. encode the BGRA frame to JPEG (pitch 0 = tightly packed)
        // 2. Lock, overwite and unlock
        // 3. nitofy socket thread to send the frame
        uint8_t* jpeg = nullptr;
        size_t   jpegSize = 0;
        if (!encoder.encode(frame.data,
                            static_cast<int>(frame.width),
                            static_cast<int>(frame.height),
                            0,
                            jpeg, jpegSize)) {
            return;
        }

        // copy out of the encoder's reused buffer so the sender owns its own frame
        {
            std::lock_guard<std::mutex> lk(f.mtx);
            f.jpeg.assign(jpeg, jpeg + jpegSize);
            f.width = frame.width;
            f.height = frame.height;
            f.timestamp_us = frame.timestamp_us;
            f.newframe_present = true;
        }
        f.cv.notify_one();
    });

    // keep this thread alive
    std::this_thread::sleep_for(std::chrono::seconds(5));

    capture.stop();

    // tell the sender to stop
    {
        std::lock_guard<std::mutex> lk(f.mtx);
        f.stop = true;
    }
    f.cv.notify_one();
}

// host -> network byte order for 64-bit (mac has no portable htonll)
static uint64_t htonll_(uint64_t v) {
    static const uint16_t probe = 1;
    if (*reinterpret_cast<const uint8_t *>(&probe) == 1) {   // little endian host
        uint32_t hi = htonl(static_cast<uint32_t>(v >> 32));
        uint32_t lo = htonl(static_cast<uint32_t>(v & 0xFFFFFFFF));
        return (static_cast<uint64_t>(lo) << 32) | hi;
    }
    return v;   // already big endian
}

// simple 16-bit additive checksum over the JPEG bytes
static uint16_t checksum16(const uint8_t *data, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i < len; ++i) sum += data[i];
    return static_cast<uint16_t>(sum & 0xFFFF);
}

// send the whole buffer, looping over partial writes. returns false on error.
static bool send_all(int s, const void *buf, size_t len) {
    const uint8_t *p = static_cast<const uint8_t *>(buf);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(s, p + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR) continue;   // interrupted, retry
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

void sender_thread(jpegFrame &f, int s) {
    uint32_t frame_id = 0;

    while (true) {
        std::vector<uint8_t> jpeg;
        uint32_t width = 0, height = 0;
        uint64_t timestamp_us = 0;
        {
            // wait until capture_thread sends data
            std::unique_lock<class std::mutex> lk(f.mtx);
            f.cv.wait(lk, [&f]{return f.newframe_present || f.stop;});
            if (f.stop && !f.newframe_present) break;

            // grab the frame + metadata and drop the lock before sending
            jpeg.swap(f.jpeg);
            width = f.width;
            height = f.height;
            timestamp_us = f.timestamp_us;
            f.newframe_present = false;
        }

        std::cout << "Sender: got JPEG frame, " << jpeg.size() << " bytes\n";

        // per-frame info: dimensions, encoding, checksum
        Pipette::FrameHeader fhdr {};
        fhdr.frame_id = htonl(frame_id++);
        fhdr.timestamp_us = htonll_(timestamp_us);
        fhdr.width = htons(static_cast<uint16_t>(width));
        fhdr.height = htons(static_cast<uint16_t>(height));
        fhdr.encoding = Pipette::Encoding::MJPEG;
        fhdr.checksum = htons(checksum16(jpeg.data(), jpeg.size()));

        // packet header: payload is the FrameHeader followed by the JPEG bytes
        Pipette::PacketHeader hdr {};
        hdr.magic = htonl(Pipette::MAGIC);
        hdr.version = Pipette::VERSION;
        hdr.type = Pipette::PacketType::Video;
        hdr.flags = 0;
        hdr.payload_size = htonl(static_cast<uint32_t>(sizeof(fhdr) + jpeg.size()));

        // send packet header -> frame header -> JPEG body (full buffers, no short writes)
        if (!send_all(s, &hdr, sizeof(hdr)) ||
            !send_all(s, &fhdr, sizeof(fhdr)) ||
            !send_all(s, jpeg.data(), jpeg.size())) {
            std::cerr << "send failed\n";
            break;
        }
        std::cout << "Sent Successfully\n";
    }
}

int main() {
    jpegFrame f;

    // Connect to PI server and enables streaming capability
    int status;
    struct addrinfo hints;
    struct addrinfo *servinfo;

    memset(&hints, 0, sizeof(hints)); // make sure that hints is empty
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    // generates a linked list of all addrinfo, (saved in servinfo)
    if ((status = getaddrinfo(PI_IP, PI_PORT, &hints, &servinfo)) != 0) {
        std::cerr << "getaddrinfo: " << gai_strerror(status) << std::endl;
        exit(1);
    }

    // getting File descriptor for the socket
    // NOTE: Remember that in unix, everything is a file; the fd is basically the descriptor or reference to the connection port / file
    int s = socket(servinfo->ai_family, servinfo->ai_socktype, servinfo->ai_protocol);
    if (s < 0 ) {
        std::cerr << "Unable to create Socket" << std::endl;
        exit(1);
    }

    // don't let a dropped Pi connection raise SIGPIPE and kill us; send() returns EPIPE instead
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));

    // given we do not care about the local port;
    // we use connect
    if (connect(s, servinfo->ai_addr, servinfo->ai_addrlen) < 0) {
        std::cerr << "Connection FAILED: address " << PI_IP << " on Port " << PI_PORT << std::endl;
        exit(1);
    }

    // start the worker threads: capture+encode produces frames, sender ships them
    std::thread capture(capture_thread, std::ref(f));
    std::thread sender(sender_thread, std::ref(f), s);

    capture.join();
    sender.join();


    close(s);
    // free the linked list
    freeaddrinfo(servinfo);


    return 0;
}
