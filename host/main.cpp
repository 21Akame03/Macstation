/*
 *
 */

#include <chrono>
#include <iostream>
#include <cstring>
#include <cstdint>
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

#define PI_IP "192.168.7.1"
#define PI_PORT "5000"

int main() {
    Pipette::Capture capture;

    capture.start([](const Pipette::Frame& frame) {
        std::cout << "Frame: " << frame.width << "x" << frame.height << " size= " << frame.size << " ts = " << frame.timestamp_us << " \n";
    });

    // keep this main thread alive
    std::this_thread::sleep_for(std::chrono::seconds(5));

    capture.stop();

    return 0;
}


// int main() {
//     // Create TCP Socket
//     int sock = socket(AF_INET, SOCK_STREAM, 0);
//     if (sock < 0) {
//         std::cerr << "Failed to create socket" << std::endl;
//         return 1;
//     }

//     // Connect to PI server and enables streaming capability
//     int status;
//     struct addrinfo hints, *res;
//     struct addrinfo *servinfo;

//     memset(&hints, 0, sizeof(hints)); // make sure that hints is empty
//     hints.ai_family = AF_INET;
//     hints.ai_socktype = SOCK_STREAM;
//     hints.ai_flags = AI_PASSIVE;

//     // generates a linked list of all addrinfo, (saved in servinfo)
//     if ((status = getaddrinfo(PI_IP, PI_PORT, &hints, &servinfo)) != 0) {
//         std::cerr << "getaddrinfo: " << gai_strerror(status) << std::endl;
//         exit(1);
//     }

//     // getting File descriptor for the socket
//     // NOTE: Remember that in unix, everything is a file; the fd is basically the descriptor or reference to the connection port / file
//     int s = socket(servinfo->ai_family, servinfo->ai_socktype, servinfo->ai_protocol);
//     if (s < 0 ) {
//         std::cerr << "Unable to create Socket" << std::endl;
//         exit(1);
//     }

//     // given we do not care about the local port;
//     // we use connect
//     if (connect(s, servinfo->ai_addr, servinfo->ai_addrlen) < 0) {
//         std::cerr << "Connection FAILED: address " << PI_IP << " on Port " << PI_PORT << std::endl;
//         exit(1);
//     }

//     // Generating the packet
//     Pipette::PacketHeader hdr {};
//     hdr.magic = htonl(Pipette::MAGIC);
//     hdr.version = Pipette::VERSION;
//     hdr.type = Pipette::PacketType::Video;
//     hdr.flags = 0;
//     hdr.payload_size = 0;

//     send(s, &hdr, sizeof(hdr), 0);
//     std::cout << "Sent Successfully\n";

//     close(s);
//     // free the linked list
//     freeaddrinfo(servinfo);


//     return 0;
// }
