/*
 *
 */
#include <iostream>
#include <fstream>
#include <vector>
#include <cstdint>
#include <cerrno>
#include "../common/protocol.hpp"
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <string.h>

#define BACKLOG 10 // No of pending connections allowed

// network -> host byte order for 64-bit (symmetric with the host's htonll_)
static uint64_t ntohll_(uint64_t v) {
    static const uint16_t probe = 1;
    if (*reinterpret_cast<const uint8_t *>(&probe) == 1) {   // little endian host
        uint32_t hi = ntohl(static_cast<uint32_t>(v >> 32));
        uint32_t lo = ntohl(static_cast<uint32_t>(v & 0xFFFFFFFF));
        return (static_cast<uint64_t>(lo) << 32) | hi;
    }
    return v;
}

// same additive 16-bit checksum the host computes over the JPEG bytes
static uint16_t checksum16(const uint8_t *data, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i < len; ++i) sum += data[i];
    return static_cast<uint16_t>(sum & 0xFFFF);
}

// read exactly len bytes. returns false on EOF or error.
static bool recv_all(int fd, void *buf, size_t len) {
    uint8_t *p = static_cast<uint8_t *>(buf);
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(fd, p + got, len - got, 0);
        if (n == 0) return false;          // peer closed
        if (n < 0) {
            if (errno == EINTR) continue;  // interrupted, retry
            return false;
        }
        got += static_cast<size_t>(n);
    }
    return true;
}

int main() {
  /*
   * The PI is the server and waits for the mac to connect
   */

  struct sockaddr_storage their_addr;
  socklen_t addr_size;
  struct addrinfo hints, *res;

  int sockfd, connfd;

  memset(&hints, 0, sizeof(hints)); // empty addrinfo before use
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;

  if (getaddrinfo(NULL, Pipette::PORT, &hints, &res) != 0) {
    std::cerr << "getaddrinfo failed: " << gai_strerror(getaddrinfo(NULL, Pipette::PORT, &hints, &res)) << std::endl;
    return 1;
  }

  //  make a socket, bind to the socket and start listening on the socket
  sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (bind(sockfd, res->ai_addr, res->ai_addrlen) != 0 ) {
      std::cerr << "Unable to Bind to " << Pipette::PORT << std::endl;
  }
  listen(sockfd, 2);


  // accepts new connection
  std::cout << "Waiting for Incoming Connection" << std::endl;
  addr_size = sizeof(their_addr);
  connfd = accept(sockfd, (struct sockaddr *) &their_addr, &addr_size);

  // read frames until the peer closes the connection.
  // each frame is: PacketHeader -> FrameHeader -> JPEG bytes
  while (true) {
      // 1. packet header
      Pipette::PacketHeader hdr {};
      if (!recv_all(connfd, &hdr, sizeof(hdr))) {
          std::cout << "Connection closed by peer" << std::endl;
          break;
      }

      // MAGIC value check
      if (ntohl(hdr.magic) != Pipette::MAGIC) {
          std::cerr << "BAD MAGIC: " << std::hex << ntohl(hdr.magic) << std::dec << std::endl;
          break;
      }

      uint32_t payload_size = ntohl(hdr.payload_size);
      if (payload_size < sizeof(Pipette::FrameHeader)) {
          std::cerr << "payload too small: " << payload_size << std::endl;
          break;
      }

      // 2. frame header
      Pipette::FrameHeader fhdr {};
      if (!recv_all(connfd, &fhdr, sizeof(fhdr))) {
          std::cerr << "incomplete frame header" << std::endl;
          break;
      }
      uint32_t frame_id     = ntohl(fhdr.frame_id);
      uint64_t timestamp_us = ntohll_(fhdr.timestamp_us);
      uint16_t width        = ntohs(fhdr.width);
      uint16_t height       = ntohs(fhdr.height);
      uint16_t checksum     = ntohs(fhdr.checksum);

      // 3. JPEG body = rest of the payload after the frame header
      size_t jpeg_size = payload_size - sizeof(Pipette::FrameHeader);
      std::vector<uint8_t> jpeg(jpeg_size);
      if (!recv_all(connfd, jpeg.data(), jpeg_size)) {
          std::cerr << "incomplete JPEG body" << std::endl;
          break;
      }

      // verify the JPEG arrived intact
      uint16_t got = checksum16(jpeg.data(), jpeg_size);
      bool ok = (got == checksum);

      std::cout << "Frame " << frame_id
                << " " << width << "x" << height
                << " ts=" << timestamp_us
                << " enc=" << (int) fhdr.encoding
                << " " << jpeg_size << " bytes"
                << " checksum " << (ok ? "OK" : "MISMATCH") << std::endl;

      if (!ok) continue;   // drop corrupt frame

      // write out the latest frame so it can be viewed/decoded
      std::ofstream out("frame.jpg", std::ios::binary | std::ios::trunc);
      out.write(reinterpret_cast<const char *>(jpeg.data()), jpeg_size);
  }

  close(sockfd);
  close(connfd);

    return 0;
}
