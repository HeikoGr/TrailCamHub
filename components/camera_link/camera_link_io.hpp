#pragma once

// UDP and session helpers shared by the handshake (camera_handshake.cpp) and
// the camera session (camera_protocol.cpp): the stop request, the socket
// receive timeout, sending a request, the session transport and the logging
// of session events. ESP-IDF only.

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "lwip/sockets.h"

#include "camera_udp_packets.hpp"
#include "pppp_session.hpp"

namespace camera::link
{

// Stop request of camera_protocol_request_stop(); checked at least every
// receive timeout by the loops of the handshake and the session.
void clear_stop();
void request_stop();
// True once a stop was requested; logs that it was seen.
bool stop_requested();
// True once a stop was requested, without logging.
bool stop_pending();

// Sets the socket receive timeout. lwIP converts SO_RCVTIMEO to whole
// milliseconds and treats 0 as "wait forever", so the timeout is rounded up
// and never shorter than 1 ms. An unchanged timeout costs no system call.
esp_err_t set_receive_timeout(int socket_fd, std::int64_t timeout_us,
                              const char *context);
// Forgets the timeout last set; call it when a socket is created, since a
// new socket may get the same number.
void forget_receive_timeout();

esp_err_t send_udp_request(int socket_fd, const struct sockaddr_in *destination,
                           const char *request_name,
                           const camera::UdpRequest &request);

// True if `source` is the camera's address and port.
bool from_camera(const struct sockaddr_in *source,
                 const struct sockaddr_in *camera_address);

// Logs a packet that is not handled in the current state, naming its type.
void log_ignored_packet(const char *state, const std::uint8_t *packet,
                        std::size_t packet_len);

// Logs session events shared by all phases. Returns true if the camera
// closed the session.
bool log_session_event(const camera::pppp::Session::Event &event,
                       const std::uint8_t *packet, std::size_t packet_len,
                       const char *state);

// Sends session packets to the camera over the bound UDP socket.
class UdpTransport final : public camera::pppp::Transport
{
  public:
    void set_destination(int socket_fd, const struct sockaddr_in &destination);
    bool send(const std::uint8_t *data, std::size_t size,
              const char *context) override;

  private:
    int socket_fd_ = -1;
    struct sockaddr_in destination_ = {};
};

}  // namespace camera::link
