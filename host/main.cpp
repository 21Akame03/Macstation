/*
 *
 */

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <functional>
#include <iostream>
#include <cstring>
#include <cstdint>
#include <mutex>
#include <netinet/in.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <ostream>
#include <random>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <thread>
#include <unistd.h>
#include "../common/protocol.hpp"
#include "capture.hpp"
#include "encoder.hpp"

// Default to the Pi's LAN address; override at runtime with:
//   pipette_host <pi-ip> [port]
// (LAN IPs are handed out by DHCP and can change — reserve one on your
//  router, or pass it on the command line.)
#define PI_IP "192.168.7.1"
#define PI_PORT "5000"

//
// 1. capture the frame using ScreenCaptureKit
// 2. Encode the Frame
// 3. Lock, overwite and unlock
// 4. nitofy socket thread to send the frame

// set false by Ctrl+C (SIGINT) / SIGTERM, or by the sender on a dropped
// connection, to wind the capture loop down cleanly.
static std::atomic<bool> g_running{true};

static void on_signal(int) { g_running = false; }

void capture_thread(jpegFrame &f) {
    Pipette::Capture capture;

    // one encoder per thread, reused for every frame
    MjpegEncoder encoder;

    capture.start([&](Pipette::Frame& frame) {
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

    // stream continuously: ScreenCaptureKit delivers frames on its own queue,
    // so just keep this thread alive until we're asked to shut down.
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    capture.stop();

    // tell the sender to stop
    {
        std::lock_guard<std::mutex> lk(f.mtx);
        f.stop = true;
    }
    f.cv.notify_one();
}

// macOS scoped-routing fix: an interface brought up manually with `ifconfig`
// (e.g. a USB/Ethernet gadget link to the Pi) has no registered network service,
// so plain connect()'s default source-address selection fails with EHOSTUNREACH.
// Find the local interface whose subnet contains the destination so we can pin
// the socket to it with IP_BOUND_IF. Returns the interface index, or 0 if none.
// On match, *src_out is filled with that interface's own address so the caller
// can pin the connection's source address too (sae_srcif alone scopes only the
// output interface; the kernel may still pick the primary interface's address).
// Override interface selection with PIPETTE_IFACE=<ifname> (e.g. en13).
static unsigned int iface_for_dest(const struct sockaddr_in *dst,
                                   struct sockaddr_in *src_out) {
    const char *forced = getenv("PIPETTE_IFACE");

    struct ifaddrs *ifs = nullptr;
    if (getifaddrs(&ifs) != 0) return 0;

    unsigned int found = 0;
    for (struct ifaddrs *ifa = ifs; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        if (!(ifa->ifa_flags & IFF_UP) || !ifa->ifa_netmask) continue;

        // pick by name if forced, otherwise by subnet match against the dest
        bool match;
        if (forced) {
            match = (strcmp(ifa->ifa_name, forced) == 0);
        } else {
            uint32_t local = reinterpret_cast<struct sockaddr_in *>(ifa->ifa_addr)->sin_addr.s_addr;
            uint32_t mask  = reinterpret_cast<struct sockaddr_in *>(ifa->ifa_netmask)->sin_addr.s_addr;
            match = ((local & mask) == (dst->sin_addr.s_addr & mask));
        }
        if (match) {
            found = if_nametoindex(ifa->ifa_name);
            *src_out = *reinterpret_cast<struct sockaddr_in *>(ifa->ifa_addr);
            break;
        }
    }
    freeifaddrs(ifs);
    return found;
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

    // 1 Hz throughput stats so we can see whether the link is the bottleneck.
    auto stat_t0 = std::chrono::steady_clock::now();
    uint64_t stat_frames = 0, stat_bytes = 0;

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
            g_running = false;   // tell capture_thread to wind down too
            break;
        }

        ++stat_frames;
        stat_bytes += jpeg.size();
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - stat_t0).count();
        if (dt >= 1.0) {
            std::cerr << "tx " << (stat_frames / dt) << " fps, "
                      << (stat_bytes / 1024.0 / stat_frames) << " KB/frame, "
                      << (stat_bytes / (1024.0 * 1024.0) / dt) << " MB/s\n";
            stat_t0 = now;
            stat_frames = 0;
            stat_bytes = 0;
        }
    }
}

int main(int argc, char *argv[]) {
    jpegFrame f;

    // Ctrl+C / kill -> stop capturing and exit cleanly instead of dying mid-frame
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    // Pi address: argv[1] overrides the default IP, argv[2] the port.
    const char *pi_ip   = (argc > 1) ? argv[1] : PI_IP;
    const char *pi_port = (argc > 2) ? argv[2] : PI_PORT;

    // Connect to PI server and enables streaming capability
    int status;
    struct addrinfo hints;
    struct addrinfo *servinfo;

    memset(&hints, 0, sizeof(hints)); // make sure that hints is empty
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    // client side: we connect(), so no AI_PASSIVE (that's for bind() servers)

    // generates a linked list of all addrinfo, (saved in servinfo)
    if ((status = getaddrinfo(pi_ip, pi_port, &hints, &servinfo)) != 0) {
        std::cerr << "getaddrinfo: " << gai_strerror(status) << std::endl;
        exit(1);
    }

    // Find the interface on the Pi's subnet and pin BOTH its index and its
    // source address. Without the source address the kernel picks the primary
    // interface's address for a scoped route and connect() -> EHOSTUNREACH.
    struct sockaddr_in src{};
    unsigned int bound_if =
        iface_for_dest(reinterpret_cast<struct sockaddr_in *>(servinfo->ai_addr), &src);
    if (bound_if) {
        char ifname[IF_NAMESIZE] = {0};
        if_indextoname(bound_if, ifname);
        std::cout << "Routing to Pi via " << ifname << " (source "
                  << inet_ntoa(src.sin_addr) << ")" << std::endl;
    }

    // connectx() is the same API macOS `nc` uses. Source is pinned via bind()
    // below, so endpoints only carry the destination.
    sa_endpoints_t eps;
    memset(&eps, 0, sizeof(eps));
    eps.sae_dstaddr    = servinfo->ai_addr;
    eps.sae_dstaddrlen = servinfo->ai_addrlen;

    // The USB-gadget link to the Pi goes idle, so the first attempt often hits
    // EHOSTUNREACH (no ARP entry yet) — the attempt itself kicks off ARP. Retry
    // a few times, like ping does, so a later attempt lands once the neighbor
    // resolves. ECONNREFUSED covers the Pi server not being up yet. A failed
    // connect can leave a blocking socket unusable, so recreate it each attempt.
    const int max_attempts = 20;
    int s = -1;
    bool connected = false;
    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        s = socket(servinfo->ai_family, servinfo->ai_socktype, servinfo->ai_protocol);
        if (s < 0) {
            std::cerr << "Unable to create Socket" << std::endl;
            exit(1);
        }

        // don't let a dropped Pi connection raise SIGPIPE and kill us; send() returns EPIPE instead
        int on = 1;
        setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
        if (bound_if) setsockopt(s, IPPROTO_IP, IP_BOUND_IF, &bound_if, sizeof(bound_if));

        // bind the source address explicitly (this is what `nc -s` does, and the
        // only thing that reliably reaches the Pi over the scoped en13 link).
        if (bound_if && bind(s, reinterpret_cast<struct sockaddr *>(&src), sizeof(src)) != 0) {
            std::cerr << "Warning: bind to source " << inet_ntoa(src.sin_addr)
                      << " failed: " << strerror(errno) << std::endl;
        }

        if (connectx(s, &eps, SAE_ASSOCID_ANY, 0, nullptr, 0, nullptr, nullptr) == 0) {
            connected = true;
            break;
        }
        if (errno != EHOSTUNREACH && errno != ECONNREFUSED && errno != ETIMEDOUT) {
            close(s);
            break;
        }
        std::cerr << "connect attempt " << attempt << "/" << max_attempts
                  << " to " << pi_ip << ":" << pi_port << " — " << strerror(errno)
                  << ", retrying..." << std::endl;
        close(s);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (!connected) {
        std::cerr << "Connection FAILED: address " << pi_ip << " on Port " << pi_port << ": " << strerror(errno) << std::endl;
        exit(1);
    }

    // send each frame's header+body straight out instead of letting Nagle wait
    // to coalesce them -- lower per-frame latency on the video stream.
    int nodelay = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

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
