/*
 *
 */
#include <iostream>
#include "../common/protocol.hpp"
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <string.h>

#define BACKLOG 10 // No of pending connections allowed

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

  // Pipette packet
  Pipette::PacketHeader hdr {};
  int conn_status = recv(connfd, &hdr, sizeof(hdr), MSG_WAITALL);
  if (conn_status < 0) {
      std::cerr << "ERROR RECV" << std::endl;
  } else if (conn_status == 0 ) {
      std::cerr << "Connection Closed by Peer" << std::endl;
  }

  // MAGIC value check
  if (ntohl(hdr.magic) == Pipette::MAGIC) {
      std::cout << "MAGIC OK - Version " << (int) hdr.version << " type: " << (int)hdr.type << std::endl;
  } else {
      std::cerr << "BAD MAGIC: " << std::hex << ntohl(hdr.magic) << std::endl;
  }

  close(sockfd);
  close(connfd);

    return 0;
}
