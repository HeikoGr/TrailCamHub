#include <atomic>
#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <time.h>
#include <unistd.h>
#include <vector>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "soc/soc_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/aes.h"
#include "lwip/sockets.h"

#include "artemis_crypto.hpp"
#include "artemis_login.hpp"
#include "camera_clock.hpp"
#include "artemis_message.hpp"
#include "artemis_reassembler.hpp"
#include "camera_prelogin.hpp"
#include "time_sync.h"
#include "camera_protocol.h"
#include "camera_udp_transport.h"
#include "bulk_reassembler.hpp"
#include "bulk_stream.hpp"
#include "device_info.hpp"
#include "media_catalog.hpp"
#include "media_store.hpp"
#include "camera_udp_packets.hpp"
#include "mbedtls/base64.h"
#include "media_list.hpp"
#include "media_transfer.hpp"
#include "pppp_session.hpp"
#include "sd_writer.hpp"
#include "playback_download.hpp"
#include "sdkconfig.h"

#define CAMERA_DISCOVERY_LOCAL_PORT 35281
#define CAMERA_DISCOVERY_TIMEOUT_MS 1500
#define CAMERA_DISCOVERY_RECEIVE_TIMEOUT_MS 500
#define CAMERA_PRELOGIN_RECEIVE_TIMEOUT_MS 1000
#define CAMERA_RUDP_RECEIVE_TIMEOUT_MS 500
#define CAMERA_RUDP_HEARTBEAT_INTERVAL_MS 1500
// Frames buffered ahead of a missing one while streaming a download (1 KB
// each); later frames are not acknowledged, so the camera resends them.
// The window is one block of up to 1472 bytes per frame, in PSRAM if there is
// one; without, internal RAM is too short for more. During a video playback
// the camera sends about 120 frames ahead of the first unacknowledged one, so
// the window must be larger than that: with 32 frames, most of them were
// dropped and sent again, which made the download about 20 times slower than
// playback (256 frames take about 377 KB of PSRAM per stream).
#if CONFIG_SPIRAM
#define CAMERA_DOWNLOAD_REORDER_FRAMES 256
#else
#define CAMERA_DOWNLOAD_REORDER_FRAMES 8
#endif
// Bulk frames (thumbnails, photos, videos) are acknowledged as the app does:
// each frame once, up to CONFIG_CAMERA_BULK_ACK_FRAMES in one ACK, sent when
// it is full or once no frame came for the delay. Acknowledging the last 16
// frames with every ACK (each frame about four times) made the camera send
// many frames again.
#ifdef CONFIG_CAMERA_BULK_ACK_FRAMES
#define CAMERA_BULK_ACK_MAX CONFIG_CAMERA_BULK_ACK_FRAMES
#else
#define CAMERA_BULK_ACK_MAX 17
#endif
#define CAMERA_BULK_ACK_DELAY_US (1 * 1000LL)
// A thumbnail with no new frame for this long is requested again.
#define CAMERA_THUMBNAIL_STALL_US (3 * 1000 * 1000LL)
// Log a download's progress every this many bytes.
#define CAMERA_DOWNLOAD_LOG_INTERVAL_BYTES (256u * 1024u)

#ifdef CONFIG_CAMERA_DOWNLOAD_PHOTOS
static const bool kDownloadPhotos = true;
#else
static const bool kDownloadPhotos = false;
#endif
// Videos are not offered by the file download (cmdId 1285): the camera
// acknowledges the request but sends nothing. They are fetched by playing
// them back (cmdId 769), which streams their H.264 and AAC frames.
#ifdef CONFIG_CAMERA_DOWNLOAD_VIDEOS
static const bool kDownloadVideos = true;
#else
static const bool kDownloadVideos = false;
#endif
// Throughput test: receive and parse videos without writing them to the SD
// card (nothing is stored or marked as downloaded).
#ifdef CONFIG_CAMERA_VIDEO_DISCARD_DATA
static const bool kDiscardVideoData = true;
#else
static const bool kDiscardVideoData = false;
#endif
#ifdef CONFIG_CAMERA_THUMBNAIL_SERIAL_PREVIEW
#define CAMERA_THUMBNAIL_SERIAL_PREVIEW 1
#else
#define CAMERA_THUMBNAIL_SERIAL_PREVIEW 0
#endif
#ifndef CONFIG_CAMERA_VIDEO_HEARTBEAT_INTERVAL_MS
#define CONFIG_CAMERA_VIDEO_HEARTBEAT_INTERVAL_MS                              \
    CAMERA_RUDP_HEARTBEAT_INTERVAL_MS
#endif
#ifndef CONFIG_CAMERA_DOWNLOAD_STALL_TIMEOUT_S
#define CONFIG_CAMERA_DOWNLOAD_STALL_TIMEOUT_S 20
#endif
#ifndef CONFIG_CAMERA_SESSION_IDLE_TIMEOUT_S
#define CONFIG_CAMERA_SESSION_IDLE_TIMEOUT_S 60
#endif

static const char *TAG = "trailcamhub_protocol";
static const uint16_t s_camera_discovery_ports[] = {40611, 3333};
// clang-format off
static const uint8_t s_prelogin_static_header[] = {
    0x0c, 0xcb, 0x9a, 0x2b, 0x5f, 0x95, 0x1e, 0xb6,
    0x69, 0xdf, 0xaa, 0x37, 0x5a, 0x6b, 0xbe, 0x3e,
    0x76, 0x20, 0x2e, 0x13, 0xc9, 0xd1, 0xaa, 0x36,
    0x31, 0xbe, 0x74, 0xe5,
};
// clang-format on
static const uint8_t s_prelogin_aes_key[16] = {
    'a', '0', '1', 'b', 'c', '2', '3', 'e',
    'd', '4', '5', 'f', 'F', '5', '6', 'A',
};
static esp_ip4_addr_t s_camera_gateway;
static bool s_camera_gateway_valid;
// Set by camera_protocol_request_stop(); checked at least every receive
// timeout (at most CAMERA_RUDP_RECEIVE_TIMEOUT_MS) by the session loops.
static std::atomic<bool> s_stop_requested;

static bool stop_requested(void)
{
    if (!s_stop_requested.load()) {
        return false;
    }
    ESP_LOGI(TAG, "Camera session stop requested");
    return true;
}

static esp_err_t build_prelogin_request(camera::PreLoginRequest *request)
{
    static const char *TAG_CRYPTO = "trailcamhub_wifi";
    if (request == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t plaintext[96];
    uint8_t nonce[camera::kPreLoginNonceSize];
    uint8_t ciphertext[96];
    size_t json_len = 0;
    time_t utc_time = time(NULL);

    esp_fill_random(nonce, sizeof(nonce));
    size_t ciphertext_len = camera::build_prelogin_plaintext(
        utc_time, nonce, plaintext, sizeof(plaintext), &json_len);
    if (ciphertext_len == 0) {
        ESP_LOGE(TAG_CRYPTO, "Could not format PRE_LOGIN JSON payload");
        return ESP_ERR_INVALID_SIZE;
    }
    if (utc_time < 1700000000) {
        ESP_LOGW(TAG_CRYPTO,
                 "System UTC time appears unset (%lld); "
                 "PRE_LOGIN timestamp may be rejected",
                 (long long)utc_time);
    }
    if (ciphertext_len > sizeof(ciphertext) ||
        sizeof(s_prelogin_static_header) + ciphertext_len > UINT16_MAX) {
        ESP_LOGE(TAG_CRYPTO, "PRE_LOGIN payload exceeds packet capacity");
        memset(plaintext, 0, sizeof(plaintext));
        return ESP_ERR_INVALID_SIZE;
    }

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    int mbedtls_error = mbedtls_aes_setkey_enc(&aes, s_prelogin_aes_key, 128);
    if (mbedtls_error == 0) {
        for (size_t offset = 0; offset < ciphertext_len; offset += 16) {
            mbedtls_error =
                mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT,
                                      plaintext + offset, ciphertext + offset);
            if (mbedtls_error != 0) {
                break;
            }
        }
    }
    mbedtls_aes_free(&aes);
    memset(plaintext, 0, sizeof(plaintext));
    if (mbedtls_error != 0) {
        ESP_LOGE(TAG_CRYPTO, "PRE_LOGIN AES encryption failed: %d",
                 mbedtls_error);
        memset(ciphertext, 0, sizeof(ciphertext));
        return ESP_FAIL;
    }

    size_t body_len = sizeof(s_prelogin_static_header) + ciphertext_len;
    uint8_t body[sizeof(s_prelogin_static_header) + sizeof(ciphertext)];
    memcpy(body, s_prelogin_static_header, sizeof(s_prelogin_static_header));
    memcpy(body + sizeof(s_prelogin_static_header), ciphertext, ciphertext_len);
    bool built = request->build(body, body_len);
    memset(body, 0, body_len);
    memset(ciphertext, 0, sizeof(ciphertext));
    if (!built) {
        ESP_LOGE(TAG_CRYPTO, "PRE_LOGIN packet exceeds supported size");
        return ESP_ERR_INVALID_SIZE;
    }
    ESP_LOGD(TAG_CRYPTO,
             "Built PRE_LOGIN with %u-byte JSON, %u-byte "
             "ciphertext, UTC timestamp %lld",
             (unsigned)json_len, (unsigned)ciphertext_len, (long long)utc_time);
    return ESP_OK;
}

// Sets the socket receive timeout. lwIP converts SO_RCVTIMEO to whole
// milliseconds and treats 0 as "wait forever", so the timeout is rounded up
// and never shorter than 1 ms.
// The receive timeout last set, so that an unchanged one costs no system call
// (it is set before every received frame, about 14,000 times per video).
// Reset when a socket is created, since a new socket may get the same number.
static int s_timeout_socket = -1;
static int64_t s_timeout_ms = -1;

static esp_err_t set_receive_timeout(int socket_fd, int64_t timeout_us,
                                     const char *context)
{
    int64_t timeout_ms = (timeout_us + 999) / 1000;
    if (timeout_ms < 1) {
        timeout_ms = 1;
    }
    if (socket_fd == s_timeout_socket && timeout_ms == s_timeout_ms) {
        return ESP_OK;
    }
    struct timeval timeout = {};
    timeout.tv_sec = static_cast<decltype(timeout.tv_sec)>(timeout_ms / 1000);
    timeout.tv_usec =
        static_cast<decltype(timeout.tv_usec)>((timeout_ms % 1000) * 1000);
    if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                   sizeof(timeout)) < 0) {
        ESP_LOGE(TAG, "Could not set %s receive timeout: errno %d", context,
                 errno);
        s_timeout_socket = -1;
        return ESP_FAIL;
    }
    s_timeout_socket = socket_fd;
    s_timeout_ms = timeout_ms;
    return ESP_OK;
}

// Logs a packet that is not handled in the current state, naming its type.
static void log_ignored_packet(const char *state, const uint8_t *packet,
                               size_t packet_len)
{
    if (packet_len >= 2 && packet[0] == 0xf1) {
        ESP_LOGW(TAG, "Ignoring %u-byte packet type 0x%02X while %s",
                 (unsigned)packet_len, packet[1], state);
    } else {
        ESP_LOGW(TAG, "Ignoring %u-byte packet without F1 prefix while %s",
                 (unsigned)packet_len, state);
    }
}

static esp_err_t send_udp_request(int socket_fd,
                                  const struct sockaddr_in *destination,
                                  const char *request_name,
                                  const camera::UdpRequest &request)
{
    ssize_t sent = camera_udp_send_packet(
        socket_fd, request.data(), request.size(), destination, request_name);
    if (sent < 0 || static_cast<size_t>(sent) != request.size()) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

// Sends session packets to the camera over the bound UDP socket.
class UdpTransport final : public camera::pppp::Transport
{
  public:
    void set_destination(int socket_fd, const struct sockaddr_in &destination)
    {
        socket_fd_ = socket_fd;
        destination_ = destination;
    }

    bool send(const uint8_t *data, size_t size, const char *context) override
    {
        ssize_t sent = camera_udp_send_packet(socket_fd_, data, size,
                                              &destination_, context);
        return sent >= 0 && static_cast<size_t>(sent) == size;
    }

  private:
    int socket_fd_ = -1;
    struct sockaddr_in destination_ = {};
};

using SessionEvent = camera::pppp::Session::Event;

static UdpTransport s_transport;
// Static because it holds the DATA packets awaiting acknowledgement.
// In PSRAM where the build allows it (about 8 KB; internal RAM is short).
EXT_RAM_BSS_ATTR static camera::pppp::Session s_session(s_transport);

// ARTEMIS command-channel state, reset at the start of each session.
static camera::artemis::Aes128 s_command_cipher =
    camera::artemis::command_cipher();
static camera::artemis::Reassembler s_response;
static bool s_logged_in;
static int64_t s_token;
static bool s_devinfo_requested;
static bool s_media_requested;
static uint32_t s_next_command_seq;
static std::string s_camera_id;
static camera::artemis::MediaCatalog s_catalog;
static std::vector<camera::artemis::CatalogEntry> s_thumb_queue;
static size_t s_thumb_index;
static bool s_thumb_emitted;
static bool s_work_done;
// The session cannot do its work (e.g. the camera has no SD card): close it
// like a finished one, but do not count it as synced.
static bool s_end_session;
// Last time the camera answered a request or sent bulk data. Heartbeats do
// not count: the camera answers them even when nothing else happens.
static int64_t s_last_progress_us;
// Battery of the camera of the last session; read by other tasks.
static std::atomic<int> s_last_battery_percent{-1};
static camera::artemis::BulkReassembler s_bulk;
static uint8_t s_bulk_subtype;
// Bulk frames received but not yet acknowledged, all of one subtype.
static uint16_t s_bulk_ack_seqs[CAMERA_BULK_ACK_MAX];
static size_t s_bulk_ack_pending;
static uint8_t s_bulk_ack_subtype;
static bool s_bulk_active;
// Sequence that follows the last completed bulk record, so that a resent
// header frame of that record is not taken for the start of the next one.
static uint16_t s_bulk_next_seq;
static bool s_bulk_next_seq_valid;
// Last frame of the record being received, and how often the current
// thumbnail was requested again after it stalled.
static int64_t s_bulk_last_frame_us;
static unsigned s_thumb_retries;

// Media download state, reset at the start of each session. A download is
// active from its request (cmdId 1285) until its record is complete or it is
// aborted.
static std::vector<camera::artemis::CatalogEntry> s_download_queue;
static size_t s_download_index;
static size_t s_download_stored;
static bool s_download_active;
static int64_t s_download_progress_us;
static int64_t s_download_started_us;
static uint64_t s_download_logged_bytes;
static camera::artemis::BulkStream
    s_download_stream(camera::artemis::kMediaRecordHeaderSize,
                      CAMERA_DOWNLOAD_REORDER_FRAMES);
static camera::store::MediaFileWriter s_media_file;
static camera_protocol_progress_cb_t s_progress_callback;

// Set when writing a video failed: no further downloads.
static bool s_stop_downloads;

static bool send_artemis_command(uint32_t cmd_id, const char *extra_fields,
                                 const char *context);
static void queue_bulk_ack(int socket_fd,
                           const struct sockaddr_in *camera_address,
                           uint8_t subtype, uint16_t seq);
static void send_bulk_ack_before_waiting(size_t size);
static void finish_current_download(bool stored, bool stop_all);
static void report_download_progress();

// Socket and camera of the running session, for sending pending ACKs from
// code that has no access to them.
static int s_ack_socket = -1;
static const struct sockaddr_in *s_ack_camera_address;

// Video download by playback (cmdId 769), on top of s_download_active.
static camera::link::PlaybackDownload s_video(
    s_command_cipher, CAMERA_DOWNLOAD_REORDER_FRAMES,
    {[](uint32_t cmd_id, const char *extra, const char *context) {
         return send_artemis_command(cmd_id, extra, context);
     },
     [](uint16_t seq) {
         queue_bulk_ack(s_ack_socket, s_ack_camera_address, 0x02, seq);
     },
     [](size_t size) { send_bulk_ack_before_waiting(size); },
     [](int64_t now_us) { s_download_progress_us = now_us; },
     [](bool stored, bool stop_all) {
         finish_current_download(stored, stop_all);
     },
     [](const camera::artemis::CatalogEntry &e, bool stored, bool sd_failed) {
         if (stored) {
             s_catalog.set_flag(e.media_id,
                                camera::artemis::MediaCatalog::kFileDownloaded);
             camera::store::save_catalog(s_camera_id, s_catalog);
             ++s_download_stored;
         }
         if (sd_failed) {
             s_stop_downloads = true;
         }
         report_download_progress();
     }});

static bool from_camera(const struct sockaddr_in *source,
                        const struct sockaddr_in *camera_address)
{
    return source->sin_addr.s_addr == camera_address->sin_addr.s_addr &&
           source->sin_port == camera_address->sin_port;
}

static uint32_t read_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static void log_received_data(const SessionEvent &event)
{
    static const uint8_t kArtemisSignature[] = {'A', 'R', 'T', 'E',
                                                'M', 'I', 'S', 0};
    if (event.payload_size >= 20 && memcmp(event.payload, kArtemisSignature,
                                           sizeof(kArtemisSignature)) == 0) {
        ESP_LOGI(TAG,
                 "Received ARTEMIS DATA index %u: message type %lu, "
                 "sequence %lu, %lu payload bytes",
                 event.index, (unsigned long)read_le32(event.payload + 8),
                 (unsigned long)read_le32(event.payload + 12),
                 (unsigned long)read_le32(event.payload + 16));
    } else {
        ESP_LOGI(TAG, "Received DATA index %u (%u bytes)", event.index,
                 (unsigned)event.payload_size);
    }
}

// Logs session events shared by all phases. Returns true if the camera
// closed the session.
static bool log_session_event(const SessionEvent &event, const uint8_t *packet,
                              size_t packet_len, const char *state)
{
    switch (event.type) {
    case SessionEvent::Type::data:
        log_received_data(event);
        break;
    case SessionEvent::Type::duplicate_data:
        ESP_LOGD(TAG, "Acknowledged repeated DATA index %u", event.index);
        break;
    case SessionEvent::Type::ack:
        ESP_LOGD(TAG, "Camera acknowledged index %u", event.index);
        break;
    case SessionEvent::Type::alive:
        ESP_LOGI(TAG, "Answered camera ALIVE");
        break;
    case SessionEvent::Type::close:
        ESP_LOGW(TAG, "Camera closed the session (CLOSE)");
        return true;
    case SessionEvent::Type::discovery_reply:
        ESP_LOGD(TAG, "Ignoring repeated camera discovery reply");
        break;
    case SessionEvent::Type::unhandled:
        log_ignored_packet(state, packet, packet_len);
        break;
    }
    return false;
}

static esp_err_t
send_artemis_handshake(int socket_fd, const struct sockaddr_in *camera_address)
{
    // The hello is the login (cmdId 0). The camera sets its clock from the
    // time in it, so it carries the current wall clock when the device has a
    // time that it can vouch for (SNTP, the backend or a browser). Without
    // one the time is left out and the camera keeps its own: a fixed time
    // would set its clock back at every session, and a time that came from
    // the camera itself must not be given back to it.
    const time_t now = time(NULL);
    const bool send_time = time_sync_trusted();
    const int64_t camera_time = send_time ? camera::camera_wall_clock(now) : 0;
    if (send_time) {
        ESP_LOGI(TAG, "Login sets the camera clock to %lld (UTC %lld)",
                 (long long)camera_time, (long long)now);
    } else if (time_sync_clock_valid()) {
        ESP_LOGW(TAG, "The clock comes from the camera; login without a time");
    } else {
        ESP_LOGW(TAG, "The clock is not set; login without a time, the camera "
                      "keeps its own clock");
    }
    const std::vector<uint8_t> hello = camera::artemis::build_request(
        s_command_cipher, 1, camera::artemis::build_login_json(camera_time));
    uint16_t hello_index = 0;
    if (hello.empty() ||
        !s_session.send_data(hello.data(), hello.size(), esp_timer_get_time(),
                             "ARTEMIS Hello", &hello_index)) {
        ESP_LOGE(TAG, "Could not send ARTEMIS Hello");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "ARTEMIS Hello sent with index %u", hello_index);
    vTaskDelay(pdMS_TO_TICKS(50));

    camera::ArtemisMagicRequest magic_one;
    if (!magic_one.build(camera::ArtemisMagicRequest::Stage::first)) {
        ESP_LOGE(TAG, "Could not build ARTEMIS Magic 1 request");
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t error = send_udp_request(socket_fd, camera_address,
                                       "ARTEMIS Magic 1", magic_one);
    if (error != ESP_OK) {
        return error;
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    camera::ArtemisMagicRequest magic_two;
    if (!magic_two.build(camera::ArtemisMagicRequest::Stage::second)) {
        ESP_LOGE(TAG, "Could not build ARTEMIS Magic 2 request");
        return ESP_ERR_INVALID_SIZE;
    }
    return send_udp_request(socket_fd, camera_address, "ARTEMIS Magic 2",
                            magic_two);
}

// Sends a token-authenticated ARTEMIS command over the session. `extra_fields`
// is inserted verbatim after cmdId (e.g. ",\"pageNo\":0"); it may be empty.
static bool send_artemis_command(uint32_t cmd_id, const char *extra_fields,
                                 const char *context)
{
    const std::string json = "{\"cmdId\":" + std::to_string(cmd_id) +
                             extra_fields +
                             ",\"token\":" + std::to_string(s_token) + "}";
    const std::vector<uint8_t> packet = camera::artemis::build_request(
        s_command_cipher, s_next_command_seq++, json);
    uint16_t index = 0;
    if (packet.empty() ||
        !s_session.send_data(packet.data(), packet.size(), esp_timer_get_time(),
                             context, &index)) {
        ESP_LOGE(TAG, "Could not send %s", context);
        return false;
    }
    ESP_LOGI(TAG, "Sent %s (cmdId %lu), index %u", context,
             (unsigned long)cmd_id, index);
    return true;
}

static void start_downloads();

// Requests the thumbnail (cmdId 772) for the current queue item, if any. The
// reply streams back on the bulk channel; handle_bulk_frame() stores it and
// advances the queue. Returns false when the queue is exhausted.
static bool request_current_thumbnail()
{
    if (s_thumb_index >= s_thumb_queue.size()) {
        ESP_LOGI(TAG, "Thumbnails complete: %u stored of %u catalog items",
                 (unsigned)s_catalog.count_with_flag(
                     camera::artemis::MediaCatalog::kThumbPresent),
                 (unsigned)s_catalog.size());
        start_downloads();
        return false;
    }
    const camera::artemis::CatalogEntry &e = s_thumb_queue[s_thumb_index];
    const std::string extra =
        ",\"thumbnailReqs\":[{\"fileType\":" + std::to_string(e.file_type) +
        ",\"dirNum\":" + std::to_string(e.dir_num) +
        ",\"mediaNum\":" + std::to_string(e.media_num) + "}]";
    return send_artemis_command(772, extra.c_str(), "ARTEMIS GetThumbnail");
}

// Sends one ACK for the bulk frames received since the last one.
static void send_bulk_ack(int socket_fd,
                          const struct sockaddr_in *camera_address)
{
    if (s_bulk_ack_pending == 0) {
        return;
    }
    camera::ArtemisBulkAckRequest ack;
    const bool built =
        ack.build(s_bulk_ack_subtype, s_bulk_ack_seqs, s_bulk_ack_pending);
    s_video.note_ack_sent(s_bulk_ack_subtype, s_bulk_ack_seqs,
                          s_bulk_ack_pending);
    s_bulk_ack_pending = 0;
    if (built && send_udp_request(socket_fd, camera_address, "BULK ACK", ack) !=
                     ESP_OK) {
        s_video.note_ack_failure();
    }
}

// Sends the pending ACK now if writing `size` bytes is about to wait for the
// SD card: the camera would otherwise take the frames received meanwhile for
// lost and send them again.
static void send_bulk_ack_before_waiting(size_t size)
{
    if (s_bulk_ack_pending > 0 && s_ack_socket >= 0 &&
        !camera::store::sd_writer_has_room(size)) {
        send_bulk_ack(s_ack_socket, s_ack_camera_address);
    }
}

// Adds a frame to the next ACK; sends it once it is full.
static void queue_bulk_ack(int socket_fd,
                           const struct sockaddr_in *camera_address,
                           uint8_t subtype, uint16_t seq)
{
    if (s_bulk_ack_pending > 0 && subtype != s_bulk_ack_subtype) {
        send_bulk_ack(socket_fd, camera_address);
    }
    s_bulk_ack_subtype = subtype;
    s_bulk_ack_seqs[s_bulk_ack_pending++] = seq;
    if (s_bulk_ack_pending >= CAMERA_BULK_ACK_MAX) {
        send_bulk_ack(socket_fd, camera_address);
    }
}

// Emits a completed JPEG over serial as Base64 (line-wrapped, with THUMB
// markers the web view can capture), guarded against oversized payloads.
static void
emit_thumbnail_jpeg(const uint8_t *jpeg, size_t jpeg_size,
                    const camera::artemis::MediaRecordHeader &header)
{
    if (jpeg_size == 0 || jpeg_size > 512u * 1024u) {
        return;
    }
    const size_t b64_cap = ((jpeg_size + 2) / 3) * 4 + 1;
    uint8_t *b64 = static_cast<uint8_t *>(malloc(b64_cap));
    if (b64 == nullptr) {
        ESP_LOGW(TAG, "THUMB b64: out of memory for %u bytes",
                 (unsigned)jpeg_size);
        return;
    }
    size_t olen = 0;
    if (mbedtls_base64_encode(b64, b64_cap, &olen, jpeg, jpeg_size) == 0) {
        ESP_LOGI(TAG, "THUMB JPEG dir=%lld num=%lld bytes=%u",
                 (long long)header.dir_num, (long long)header.media_num,
                 (unsigned)jpeg_size);
        const size_t line = 120;
        for (size_t off = 0; off < olen; off += line) {
            const size_t n = (olen - off < line) ? (olen - off) : line;
            ESP_LOGI(TAG, "THUMB b64:%.*s", (int)n,
                     reinterpret_cast<const char *>(b64) + off);
        }
        ESP_LOGI(TAG, "THUMB b64:END");
    }
    free(b64);
}

static void report_download_progress()
{
    if (s_progress_callback != nullptr) {
        s_progress_callback(static_cast<uint32_t>(s_download_index),
                            static_cast<uint32_t>(s_download_stored),
                            static_cast<uint32_t>(s_download_queue.size()));
    }
}

// Requests the current download queue item: a photo as a file (cmdId 1285),
// which streams back on the bulk channel for handle_download_frame(), or a
// video by playback (PlaybackDownload::start()). When the queue is exhausted,
// the session is closed.
static void request_current_download()
{
    // Frames of a stopped video stream may still be on their way.
    if (s_video.draining(esp_timer_get_time())) {
        return;
    }
    s_video.clear_drain();
    while (s_download_index < s_download_queue.size()) {
        const camera::artemis::CatalogEntry &e =
            s_download_queue[s_download_index];
        if (e.file_type != 0) {
            if (s_video.start(e, s_camera_id, s_download_index,
                              s_download_queue.size())) {
                s_download_active = true;
                s_download_started_us = s_video.started_us();
                s_download_progress_us = s_download_started_us;
                return;
            }
            ESP_LOGW(TAG, "Cannot write to the SD card; stopping downloads");
            s_download_index = s_download_queue.size();
            break;
        }
        if (!s_media_file.open(s_camera_id, e.media_id, nullptr,
                               !camera::store::sd_writer_active())) {
            ESP_LOGW(TAG, "Cannot write to the SD card; stopping downloads");
            s_download_index = s_download_queue.size();
            break;
        }
        const std::string extra =
            ",\"downloadReqs\":[{\"fileType\":" + std::to_string(e.file_type) +
            ",\"dirNum\":" + std::to_string(e.dir_num) +
            ",\"mediaNum\":" + std::to_string(e.media_num) + "}]";
        if (send_artemis_command(1285, extra.c_str(), "ARTEMIS Download")) {
            ESP_LOGI(TAG, "Downloading %s dir %lld num %lld (%u of %u)",
                     e.file_type == 0 ? "photo" : "video", (long long)e.dir_num,
                     (long long)e.media_num, (unsigned)(s_download_index + 1),
                     (unsigned)s_download_queue.size());
            s_download_stream.reset();
            s_download_active = true;
            s_download_started_us = esp_timer_get_time();
            s_download_progress_us = s_download_started_us;
            s_download_logged_bytes = 0;
            return;
        }
        s_media_file.abort();
        ++s_download_index;
        report_download_progress();
    }
    s_video.complete_pending(true);
    ESP_LOGI(TAG, "Downloads complete: %u of %u catalog items on the SD card",
             (unsigned)s_catalog.count_with_flag(
                 camera::artemis::MediaCatalog::kFileDownloaded),
             (unsigned)s_catalog.size());
    s_work_done = true;
}

// Builds the download queue once the thumbnails are done. Without enabled
// downloads, the session is closed right away.
static void start_downloads()
{
    s_download_queue =
        s_catalog.missing_downloads(kDownloadPhotos, kDownloadVideos);
    s_download_index = 0;
    s_download_stored = 0;
    s_stop_downloads = false;
    if (kDownloadPhotos || kDownloadVideos) {
        ESP_LOGI(TAG, "%u files to download",
                 (unsigned)s_download_queue.size());
    }
    report_download_progress();
    request_current_download();
}

// Ends the current download: on success the file gets its final name and the
// catalog records it; otherwise the partial file is removed and the item is
// tried again in a later session. Then the next item is requested, unless
// `stop_all` (the SD card failed).
static void finish_current_download(bool stored, bool stop_all)
{
    using camera::artemis::MediaCatalog;
    const camera::artemis::CatalogEntry &e = s_download_queue[s_download_index];
    const bool was_video = s_video.active();
    stop_all = stop_all || s_stop_downloads;
    // A video that ended normally is closed once the SD writer has written
    // it, while the next one is downloaded
    // (PlaybackDownload::complete_pending()).
    const bool deferred = s_video.can_defer(stored);
    // Otherwise wait for the queued writes (or drop them if the file is not
    // kept).
    if (!deferred && !camera::store::sd_writer_flush(!stored) && stored) {
        stored = false;
        stop_all = true;
    }
    uint64_t video_bytes = 0;
    if (was_video) {
        stored = s_video.finish(e, stored, deferred, &video_bytes);
    }
    camera::store::MediaFileWriter &file =
        was_video ? s_video.video_file() : s_media_file;
    if (deferred) {
        // Closed by PlaybackDownload::complete_pending().
    } else if (stored && file.finish(e.media_time)) {
        const int64_t elapsed_ms =
            (esp_timer_get_time() - s_download_started_us) / 1000;
        const uint64_t bytes = was_video
                                   ? video_bytes
                                   : s_download_stream.delivered_bytes() -
                                         s_download_stream.prefix().size();
        ESP_LOGI(TAG, "Stored %s (%llu bytes in %lld ms, %llu KB/s)",
                 file.path().c_str(), (unsigned long long)bytes,
                 (long long)elapsed_ms,
                 (unsigned long long)(elapsed_ms > 0 ? bytes / elapsed_ms : 0));
        s_catalog.set_flag(e.media_id, MediaCatalog::kFileDownloaded);
        camera::store::save_catalog(s_camera_id, s_catalog);
        ++s_download_stored;
    } else {
        file.abort();
        if (!(was_video && kDiscardVideoData)) {
            ESP_LOGW(TAG, "Download of dir %lld num %lld failed",
                     (long long)e.dir_num, (long long)e.media_num);
        }
    }
    s_download_stream.reset();
    s_download_active = false;
    s_bulk_active = false;
    ++s_download_index;
    if (stop_all) {
        ESP_LOGW(TAG, "SD card write failed; stopping downloads");
        s_download_index = s_download_queue.size();
    }
    report_download_progress();
    request_current_download();
}

// Handles one bulk frame of a running download: passes it to the stream,
// which writes the file in order, and acknowledges the frames it took.
static void handle_download_frame(int socket_fd,
                                  const struct sockaddr_in *camera_address,
                                  uint8_t subtype, uint16_t seq,
                                  const uint8_t *payload, size_t payload_len,
                                  bool is_record_start)
{
    using camera::artemis::BulkStream;
    if (!s_download_stream.header_ready() && !is_record_start) {
        // Leftover of an earlier record: acknowledge it so the camera stops
        // resending it.
        camera::ArtemisBulkAckRequest stray_ack;
        if (stray_ack.build(subtype, &seq, 1)) {
            send_udp_request(socket_fd, camera_address, "BULK ACK (stray)",
                             stray_ack);
        }
        return;
    }
    if (!s_download_stream.header_ready()) {
        s_bulk_subtype = subtype;
        s_bulk_active = true;
    }

    bool sink_failed = false;
    const BulkStream::Sink sink = [&sink_failed](const uint8_t *data,
                                                 size_t size) {
        sink_failed = !camera::store::sd_writer_write(s_media_file, data, size);
        return !sink_failed;
    };
    const uint64_t before = s_download_stream.delivered_bytes();
    bool ack = false;
    const BulkStream::Status status =
        s_download_stream.feed(seq, payload, payload_len, sink, ack);
    if (ack) {
        queue_bulk_ack(socket_fd, camera_address, subtype, seq);
    }

    const uint64_t delivered = s_download_stream.delivered_bytes();
    if (delivered > before) {
        s_download_progress_us = esp_timer_get_time();
    }
    if (before == 0 && s_download_stream.header_ready()) {
        ESP_LOGI(TAG, "Download record start: subtype 0x%02x, %u bytes",
                 (unsigned)subtype, (unsigned)s_download_stream.record_len());
    }
    if (delivered - s_download_logged_bytes >=
        CAMERA_DOWNLOAD_LOG_INTERVAL_BYTES) {
        s_download_logged_bytes = delivered;
        ESP_LOGI(TAG, "Download: %llu of %u bytes",
                 (unsigned long long)delivered,
                 (unsigned)s_download_stream.record_len());
    }

    if (status == BulkStream::Status::invalid) {
        if (!sink_failed) {
            const size_t dump = payload_len < 24 ? payload_len : 24;
            ESP_LOGW(TAG, "Download record invalid; first bytes:");
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, payload, dump, ESP_LOG_WARN);
        }
        finish_current_download(false, sink_failed);
        return;
    }
    if (status == BulkStream::Status::complete) {
        const auto &prefix = s_download_stream.prefix();
        camera::artemis::MediaRecordHeader header;
        size_t data_offset = 0;
        size_t data_size = 0;
        const camera::artemis::CatalogEntry &e =
            s_download_queue[s_download_index];
        if (camera::artemis::parse_media_record(
                prefix.data(), prefix.size(), header, data_offset, data_size) &&
            (header.dir_num != e.dir_num || header.media_num != e.media_num)) {
            ESP_LOGW(TAG,
                     "Download record is for dir %lld num %lld, requested "
                     "dir %lld num %lld; discarding it",
                     (long long)header.dir_num, (long long)header.media_num,
                     (long long)e.dir_num, (long long)e.media_num);
            finish_current_download(false, false);
            return;
        }
        finish_current_download(true, false);
    }
}

// Aborts the current download if no data arrived for the stall timeout.
// A thumbnail record that stops arriving: the heartbeat is paused during a
// bulk transfer, so without this the camera would end the session. Request
// the thumbnail again, or skip it after a second stall.
static void check_thumbnail_stall(int64_t now_us)
{
    if (!s_bulk_active || s_bulk_subtype != 0x04 ||
        now_us - s_bulk_last_frame_us < CAMERA_THUMBNAIL_STALL_US) {
        return;
    }
    ESP_LOGW(TAG, "Thumbnail stalled at %u of %u frames; %s",
             (unsigned)s_bulk.received_frames(),
             (unsigned)s_bulk.total_frames(),
             s_thumb_retries == 0 ? "requesting it again" : "skipping it");
    s_bulk.reset();
    s_bulk_active = false;
    if (s_thumb_retries == 0) {
        s_thumb_retries = 1;
    } else {
        s_thumb_retries = 0;
        ++s_thumb_index;
    }
    request_current_thumbnail();
}

static void check_download_stall(int64_t now_us)
{
    s_video.complete_pending(false);
    if (s_video.drain_done(now_us) && !s_download_active) {
        request_current_download();
        return;
    }
    if (s_video.receiving()) {
        // The camera does not mark the end of the stream; it stops sending.
        if (s_video.stream_ended(now_us)) {
            ESP_LOGI(TAG, "Video stream ended");
            finish_current_download(true, false);
        }
        return;
    }
    if (s_download_active &&
        now_us - s_download_progress_us >
            CONFIG_CAMERA_DOWNLOAD_STALL_TIMEOUT_S * 1000000LL) {
        ESP_LOGW(TAG, "No download data for %d s; skipping the file",
                 CONFIG_CAMERA_DOWNLOAD_STALL_TIMEOUT_S);
        finish_current_download(false, false);
    }
}

// Handles one bulk-stream frame (D0/D1 subtype 0x03/0x04): feeds the
// reassembler, acknowledges the window so the camera advances to the next
// frames, and on completion extracts and emits the JPEG.
static void handle_bulk_frame(int socket_fd,
                              const struct sockaddr_in *camera_address,
                              const uint8_t *packet, size_t size)
{
    using namespace camera::artemis;
    static const uint8_t kSig[8] = {'A', 'R', 'T', 'E', 'M', 'I', 'S', 0};
    const uint8_t subtype = packet[5];
    const uint16_t seq = static_cast<uint16_t>((packet[6] << 8) | packet[7]);
    const uint8_t *payload = packet + 8;
    const size_t payload_len = size - 8;
    const bool is_record_start =
        payload_len >= 20 && memcmp(payload, kSig, sizeof(kSig)) == 0;

    // During a video download the stream is on subtype 0x02; the other
    // subtypes are ignored.
    if (s_video.active()) {
        if (subtype == 0x02) {
            s_video.handle_frame(seq, payload, payload_len);
            return;
        }
        // Leftovers of the thumbnails: acknowledge them, so that the camera
        // does not send them again and again, taking airtime from the video.
        camera::ArtemisBulkAckRequest stray_ack;
        if (stray_ack.build(subtype, &seq, 1)) {
            send_udp_request(socket_fd, camera_address, "BULK ACK (stray)",
                             stray_ack);
        }
        s_video.note_other_subtype_frame(subtype, seq, payload, payload_len);
        return;
    }
    // Leftovers of a stopped video stream.
    if (subtype == 0x02) {
        s_video.note_leftover_frame();
        camera::ArtemisBulkAckRequest stray_ack;
        if (stray_ack.build(subtype, &seq, 1)) {
            send_udp_request(socket_fd, camera_address, "BULK ACK (stray)",
                             stray_ack);
        }
        return;
    }
    // Thumbnails keep subtype 0x04; any other subtype during a download is
    // the requested file (0x03 for photos).
    if (s_download_active && subtype != 0x04) {
        handle_download_frame(socket_fd, camera_address, subtype, seq, payload,
                              payload_len, is_record_start);
        return;
    }

    if (!s_bulk_active || subtype != s_bulk_subtype) {
        // A record only begins on its header-bearing (ARTEMIS) frame. Any
        // other frame received between records is a retransmit of a finished
        // record or a reordered straggler: acknowledge it so the camera stops
        // resending and moves on, but do not start a record on it.
        if (!is_record_start) {
            camera::ArtemisBulkAckRequest stray_ack;
            if (stray_ack.build(subtype, &seq, 1)) {
                send_udp_request(socket_fd, camera_address, "BULK ACK (stray)",
                                 stray_ack);
            }
            ESP_LOGD(TAG,
                     "BULK subtype 0x%02x seq %u: stray frame ignored (%u B)",
                     (unsigned)subtype, (unsigned)seq, (unsigned)payload_len);
            return;
        }
        // The camera may resend the header frame of a record that is already
        // complete; a record starting before the end of the last one is such
        // a repeat, not the next record.
        if (s_bulk_next_seq_valid && subtype == s_bulk_subtype &&
            static_cast<int16_t>(seq - s_bulk_next_seq) < 0) {
            camera::ArtemisBulkAckRequest stray_ack;
            if (stray_ack.build(subtype, &seq, 1)) {
                send_udp_request(socket_fd, camera_address, "BULK ACK (stray)",
                                 stray_ack);
            }
            ESP_LOGI(TAG,
                     "BULK subtype 0x%02x seq %u: repeated header of a "
                     "finished record ignored (next record from seq %u)",
                     (unsigned)subtype, (unsigned)seq,
                     (unsigned)s_bulk_next_seq);
            return;
        }
        s_bulk.reset();
        s_bulk_subtype = subtype;
        s_bulk_active = true;
    }

    s_bulk_last_frame_us = esp_timer_get_time();
    const bool was_header = s_bulk.header_ready();
    const size_t before = s_bulk.received_frames();
    const BulkReassembler::Status status =
        s_bulk.feed(seq, payload, payload_len);

    // Acknowledge the frame so the camera keeps streaming.
    queue_bulk_ack(socket_fd, camera_address, subtype, seq);

    // Log the record boundary so successive records can be told apart.
    if (!was_header && s_bulk.header_ready()) {
        ESP_LOGI(TAG,
                 "BULK record start: subtype 0x%02x base seq %u, ver %u typ %u "
                 "len %u (%u frames)",
                 (unsigned)subtype, (unsigned)seq, (unsigned)s_bulk.version(),
                 (unsigned)s_bulk.type(), (unsigned)s_bulk.record_len(),
                 (unsigned)s_bulk.total_frames());
    }

    const size_t frames = s_bulk.received_frames();
    if (frames > before && (frames == 1 || frames % 8 == 0)) {
        ESP_LOGI(TAG, "BULK subtype 0x%02x seq %u (%u/%u frames, %u bytes)",
                 (unsigned)subtype, (unsigned)seq, (unsigned)frames,
                 (unsigned)s_bulk.total_frames(),
                 (unsigned)s_bulk.received_bytes());
    }

    if (status == BulkReassembler::Status::invalid) {
        // Diagnostics: say why the frame was rejected and show its first
        // bytes, so a non-header frame arriving as a record start (reordering,
        // a straggler, or an unexpected framing) is visible in the log.
        const size_t dump = payload_len < 24 ? payload_len : 24;
        ESP_LOGW(TAG,
                 "BULK subtype 0x%02x seq %u: invalid record (%s), "
                 "%u payload bytes; first bytes:",
                 (unsigned)subtype, (unsigned)seq,
                 BulkReassembler::reason_name(s_bulk.invalid_reason()),
                 (unsigned)payload_len);
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, payload, dump, ESP_LOG_WARN);
        s_bulk.reset();
        s_bulk_active = false;
        return;
    }
    if (status == BulkReassembler::Status::complete) {
        s_bulk_next_seq =
            static_cast<uint16_t>(s_bulk.base_seq() + s_bulk.total_frames());
        s_bulk_next_seq_valid = true;
        MediaRecordHeader header;
        size_t jpeg_offset = 0;
        size_t jpeg_size = 0;
        const bool parsed =
            parse_media_record(s_bulk.payload(), s_bulk.payload_size(), header,
                               jpeg_offset, jpeg_size);
        if (parsed) {
            ESP_LOGI(TAG,
                     "BULK subtype 0x%02x complete: %u record bytes, "
                     "%u JPEG bytes (dir %lld num %lld)",
                     (unsigned)subtype, (unsigned)s_bulk.received_bytes(),
                     (unsigned)jpeg_size, (long long)header.dir_num,
                     (long long)header.media_num);
        }

        if (parsed && subtype == 0x04) {
            // Thumbnail: store it on the SD card and record it in the catalog.
            // Date the file by the capture time from the media list.
            const uint8_t *jpeg = s_bulk.payload() + jpeg_offset;
            // The answer names dir/num, which is unique among the items of
            // the current media list (only those are requested).
            const CatalogEntry *entry =
                s_catalog.find_listed(header.dir_num, header.media_num);
            if (entry == nullptr) {
                ESP_LOGW(TAG,
                         "Thumbnail dir %lld num %lld is not in the media "
                         "list; not stored",
                         (long long)header.dir_num,
                         (long long)header.media_num);
            } else {
                const int64_t media_id = entry->media_id;
                if (header.media_id != media_id) {
                    ESP_LOGW(TAG,
                             "Thumbnail dir %lld num %lld: record id %lld, "
                             "list mediaId %lld",
                             (long long)header.dir_num,
                             (long long)header.media_num,
                             (long long)header.media_id, (long long)media_id);
                }
                if (camera::store::write_thumbnail(s_camera_id, media_id, jpeg,
                                                   jpeg_size,
                                                   entry->media_time)) {
                    s_catalog.set_flag(media_id, MediaCatalog::kThumbPresent);
                    camera::store::save_catalog(s_camera_id, s_catalog);
                } else {
                    ESP_LOGW(TAG, "Could not store thumbnail dir %lld num %lld",
                             (long long)header.dir_num,
                             (long long)header.media_num);
                }
            }
            // Emit the first thumbnail of the cycle over serial (for the web
            // live view); the rest just go to the SD card to avoid flooding.
            if (CAMERA_THUMBNAIL_SERIAL_PREVIEW && !s_thumb_emitted) {
                emit_thumbnail_jpeg(jpeg, jpeg_size, header);
                s_thumb_emitted = true;
            }
        }

        s_bulk.reset();
        s_bulk_active = false;

        if (subtype == 0x04) {
            ++s_thumb_index;
            s_thumb_retries = 0;
            request_current_thumbnail();
        }
    }
}

// Decrypts a received ARTEMIS DATA message (reassembling fragments). On the
// login response it records the session token; the first command is sent only
// once the session has settled (see the heartbeat-ack path), mirroring the
// app's order. Other responses are logged as decrypted JSON, and the
// device-info reply triggers the media-list request.
static void process_artemis_response(const SessionEvent &event)
{
    using namespace camera::artemis;

    const Reassembler::Status status =
        s_response.feed(event.payload, event.payload_size);
    if (status == Reassembler::Status::incomplete) {
        return;
    }
    if (status == Reassembler::Status::invalid) {
        s_response.reset();
        return;
    }

    Message message;
    const bool decoded = parse(s_command_cipher, s_response.message().data(),
                               s_response.message().size(), message);
    s_response.reset();
    if (!decoded) {
        ESP_LOGW(TAG, "Could not decode the ARTEMIS response");
        return;
    }

    int64_t cmd_id = -1;
    json_find_int(message.json, "cmdId", cmd_id);

    if (cmd_id == 0) {
        if (s_logged_in) {
            return;
        }
        int64_t result = -1;
        int64_t token = 0;
        if (json_find_int(message.json, "result", result) && result == 0 &&
            json_find_int(message.json, "token", token)) {
            s_token = token;
            s_logged_in = true;
            ESP_LOGI(TAG, "ARTEMIS login succeeded; token %lld",
                     (long long)token);
        } else {
            ESP_LOGW(TAG, "ARTEMIS login rejected: %s", message.json.c_str());
        }
        return;
    }

    ESP_LOGI(TAG, "ARTEMIS response cmdId %lld: %s", (long long)cmd_id,
             message.json.c_str());
    if (cmd_id != 525) {
        s_last_progress_us = esp_timer_get_time();
    }

    // The camera answers the playback request with the video's size, then
    // streams it. When it refuses the app's fileType, try the media list's.
    if (cmd_id == 769 && s_video.active()) {
        if (s_video.handle_playback_response(
                message.json, s_download_queue[s_download_index]) ==
            camera::link::PlaybackDownload::Response::refused) {
            finish_current_download(false, false);
        }
        return;
    }

    // The camera acknowledges a download request before streaming the file;
    // a non-zero result means it will not send it.
    if (cmd_id == 1285 && s_download_active) {
        int64_t result = 0;
        if (json_find_int(message.json, "result", result) && result != 0) {
            ESP_LOGW(TAG, "Camera refused the download (result %lld)",
                     (long long)result);
            finish_current_download(false, false);
        }
        return;
    }

    // Parse the media list into the typed model and log a short summary, so
    // the model is exercised against the real response on hardware too.
    if (cmd_id == 768) {
        camera::artemis::MediaList list;
        const bool parsed =
            camera::artemis::parse_media_list(message.json, list);
        if (!parsed || camera::artemis::media_list_failed(list)) {
            // Nothing to fetch: no thumbnail or download would be queued, so
            // the session would idle on heartbeats. Leave the catalog as is.
            ESP_LOGE(TAG,
                     "Media list failed (getMediaListRet %lld, errorMsg "
                     "'%s'%s); ending the camera session",
                     (long long)list.list_ret, list.error_msg.c_str(),
                     parsed ? "" : ", no mediaFiles array");
            s_end_session = true;
        } else {
            int photos = 0;
            int videos = 0;
            for (const auto &item : list.files) {
                if (item.file_type == 0)
                    ++photos;
                else
                    ++videos;
            }
            ESP_LOGI(TAG,
                     "Media list parsed: %u items (%d photos, %d videos), "
                     "cnt %lld, pageNo %lld, result %lld",
                     (unsigned)list.files.size(), photos, videos,
                     (long long)list.count, (long long)list.page_no,
                     (long long)list.result);
            // Merge into the on-SD catalog, persist it, then fetch the
            // thumbnail of every item we do not have one for yet.
            const size_t added = s_catalog.merge(list);
            camera::store::save_catalog(s_camera_id, s_catalog);
            s_thumb_queue = s_catalog.missing_thumbnails();
            s_thumb_index = 0;
            s_thumb_emitted = false;
            ESP_LOGI(TAG,
                     "Catalog %s: %u items (%u new), %u thumbnails to fetch",
                     s_camera_id.c_str(), (unsigned)s_catalog.size(),
                     (unsigned)added, (unsigned)s_thumb_queue.size());
            request_current_thumbnail();
        }
    }

    // Device info carries the camera identity (devId) and battery state. Use
    // the id to load that camera's catalog from the SD card, then request the
    // media list (as the app does; 768 needs paging parameters to answer).
    if (cmd_id == 512 && s_logged_in && !s_media_requested) {
        DeviceInfo info;
        if (parse_device_info(message.json, info)) {
            s_camera_id = info.dev_id;
            if (info.bat_percent >= 0 && info.bat_percent <= 100) {
                s_last_battery_percent = (int)info.bat_percent;
            }
            ESP_LOGI(TAG,
                     "Camera '%s' model '%s' fw '%s' battery %lld%% "
                     "SD free %lld MB",
                     s_camera_id.c_str(), info.model_name.c_str(),
                     info.fw_version.c_str(), (long long)info.bat_percent,
                     (long long)info.sd_free_mb);
            // A device without a clock takes the time of the camera: the
            // best it has, replaced by any better source later.
            int64_t camera_wall_clock = 0;
            if (!time_sync_clock_valid() &&
                json_find_int(message.json, "timeUTC", camera_wall_clock) &&
                camera_wall_clock > 0) {
                time_sync_set(
                    camera::camera_wall_clock_to_utc(camera_wall_clock),
                    TIME_SYNC_SOURCE_CAMERA);
            }
            if (!camera::store::save_camera_info(info, (int64_t)time(NULL))) {
                ESP_LOGW(TAG, "Could not store the camera info on the SD card");
            }
        } else {
            s_camera_id = "unknown";
            ESP_LOGW(TAG, "Could not parse device info; using camera id '%s'",
                     s_camera_id.c_str());
        }
        s_catalog.clear();
        camera::store::load_catalog(s_camera_id, s_catalog);
        ESP_LOGI(TAG, "Loaded catalog: %u known items",
                 (unsigned)s_catalog.size());

        s_media_requested = true;
        send_artemis_command(768, ",\"itemCntPerPage\":45,\"pageNo\":0",
                             "ARTEMIS GetMediaList");
    }
}

// Keeps the ARTEMIS session alive with heartbeats (one at a time, sent
// after the previous one was acknowledged) and logs everything received
// until the camera closes the session or a stop is requested.
static esp_err_t run_artemis_session(int socket_fd,
                                     const struct sockaddr_in *camera_address,
                                     int64_t discovery_started_us)
{
    camera::ArtemisHeartbeatPayload heartbeat;
    uint16_t heartbeat_index = 0;
    bool heartbeat_pending = false;
    s_ack_socket = socket_fd;
    s_ack_camera_address = camera_address;
    int64_t last_heartbeat_us = 0;
    int64_t heartbeat_interval_us = CAMERA_RUDP_HEARTBEAT_INTERVAL_MS * 1000LL;
    int64_t last_receive_us = 0;
    s_response.reset();
    s_logged_in = false;
    s_token = 0;
    s_devinfo_requested = false;
    s_media_requested = false;
    s_next_command_seq = 2;
    s_camera_id.clear();
    s_catalog.clear();
    s_thumb_queue.clear();
    s_thumb_index = 0;
    s_thumb_emitted = false;
    s_work_done = false;
    s_end_session = false;
    s_last_progress_us = esp_timer_get_time();
    s_bulk.reset();
    s_bulk_subtype = 0;
    s_bulk_ack_pending = 0;
    s_bulk_active = false;
    s_bulk_next_seq_valid = false;
    s_thumb_retries = 0;
    s_download_queue.clear();
    s_download_index = 0;
    s_download_active = false;
    s_download_stream.reset();
    s_video.begin_session();
    ESP_LOGI(TAG, "Starting ARTEMIS heartbeat loop");

    while (!stop_requested()) {
        // All requested work is done: close the camera session cleanly
        // (PPPP/CS2 CLOSE) and finish, rather than idling on heartbeats.
        if (s_work_done || s_end_session) {
            const camera::ControlRequest close(camera::PacketType::close);
            send_udp_request(socket_fd, camera_address, "CLOSE", close);
            ESP_LOGI(TAG, "%s; closed the camera session",
                     s_work_done ? "Work complete" : "Nothing more to do");
            return ESP_OK;
        }
        int64_t now_us = esp_timer_get_time();
        // A request the camera never answers (or an answer we do not
        // handle) would keep the session, and the camera's Wi-Fi, alive on
        // heartbeats until its battery is empty.
        if (now_us - s_last_progress_us >
            CONFIG_CAMERA_SESSION_IDLE_TIMEOUT_S * 1000000LL) {
            ESP_LOGE(TAG,
                     "No answer or data from the camera for %d s; closing "
                     "the camera session",
                     CONFIG_CAMERA_SESSION_IDLE_TIMEOUT_S);
            const camera::ControlRequest close(camera::PacketType::close);
            send_udp_request(socket_fd, camera_address, "CLOSE", close);
            return ESP_ERR_TIMEOUT;
        }
        check_download_stall(now_us);
        check_thumbnail_stall(now_us);
        // A video download may use its own interval: the camera streamed about
        // two video frames per heartbeat, as if it were held back by them.
        heartbeat_interval_us =
            (s_video.active() ? CONFIG_CAMERA_VIDEO_HEARTBEAT_INTERVAL_MS
                              : CAMERA_RUDP_HEARTBEAT_INTERVAL_MS) *
            1000LL;
        // Pause the session heartbeat during a bulk transfer: it would not be
        // acknowledged while the camera is streaming, and its retransmissions
        // only compete with our frame ACKs on the uplink.
        if (!s_bulk_active && !heartbeat_pending &&
            (last_heartbeat_us == 0 ||
             now_us - last_heartbeat_us >= heartbeat_interval_us)) {
            const uint8_t *body = heartbeat.next_command();
            if (s_session.send_data(body, heartbeat.size(), now_us,
                                    "ARTEMIS heartbeat", &heartbeat_index)) {
                heartbeat_pending = true;
                last_heartbeat_us = now_us;
                s_video.note_heartbeat();
                ESP_LOGI(TAG,
                         "ARTEMIS heartbeat command %u, index %u awaiting ACK",
                         heartbeat.command_count(), heartbeat_index);
            } else if (!s_session.transport_failed()) {
                if (s_session.outstanding_count() <
                    camera::pppp::Session::kMaxOutstanding) {
                    // A send error, not a full window: try again after the
                    // heartbeat interval instead of in a tight loop.
                    last_heartbeat_us = now_us;
                    ESP_LOGW(TAG, "Could not send the heartbeat; retrying");
                } else {
                    ESP_LOGW(TAG, "No free DATA slot for the heartbeat");
                }
            }
        }
        size_t retransmitted = s_bulk_active ? 0 : s_session.poll(now_us);
        if (retransmitted > 0) {
            ESP_LOGW(TAG, "Retransmitted %u unacknowledged DATA packet(s)",
                     (unsigned)retransmitted);
        }
        if (s_session.transport_failed()) {
            return ESP_FAIL;
        }

        // Wake up in time for the next retransmission or heartbeat. During a
        // bulk transfer both are paused, so just block for the full interval
        // and stay responsive to incoming frames.
        int64_t timeout_us = CAMERA_RUDP_RECEIVE_TIMEOUT_MS * 1000LL;
        if (!s_bulk_active) {
            timeout_us = s_session.time_until_retransmit_us(now_us, timeout_us);
            if (!heartbeat_pending) {
                int64_t heartbeat_due_us =
                    heartbeat_interval_us - (now_us - last_heartbeat_us);
                if (heartbeat_due_us < timeout_us) {
                    timeout_us = heartbeat_due_us;
                }
            }
        }
        // Send the pending bulk ACK once no frame follows shortly.
        if (s_bulk_ack_pending > 0 && timeout_us > CAMERA_BULK_ACK_DELAY_US) {
            timeout_us = CAMERA_BULK_ACK_DELAY_US;
        }
        esp_err_t timeout_error =
            set_receive_timeout(socket_fd, timeout_us, "ARTEMIS");
        if (timeout_error != ESP_OK) {
            return timeout_error;
        }

        // Static (not on the task stack): a fragment datagram is ~1.6 KB, and
        // only the single camera session task uses it. In internal RAM: every
        // received frame, duplicates included, is written here and read
        // again, which costs noticeably more CPU time in PSRAM.
        static uint8_t packet[camera::ArtemisRudpPacket::kMaxPacketSize];
        struct sockaddr_in source = {};
        if (s_video.active() && last_receive_us != 0) {
            s_video.note_loop_busy(esp_timer_get_time() - last_receive_us);
        }
        ssize_t received = camera_udp_receive_packet(
            socket_fd, packet, sizeof(packet), &source, "ARTEMIS");
        last_receive_us = esp_timer_get_time();
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                send_bulk_ack(socket_fd, camera_address);
                continue;
            }
            ESP_LOGE(TAG, "ARTEMIS packet receive failed: errno %d", errno);
            return ESP_FAIL;
        }
        if (!from_camera(&source, camera_address)) {
            ESP_LOGW(TAG, "Ignoring UDP packet from unexpected endpoint");
            continue;
        }
        ESP_LOGD(
            TAG, "ARTEMIS packet received +%lld ms since discovery",
            (long long)((esp_timer_get_time() - discovery_started_us) / 1000));

        // The bulk data channel uses D0/D1 frames on a non-zero subtype
        // (0x03 photo, 0x04 thumbnail); the session's RUDP parser only accepts
        // subtype 0, so handle those here: reassemble the record and ACK the
        // window so the camera advances. These frames are not session DATA, so
        // skip handle_packet() for them.
        if (received >= 8 && packet[0] == 0xf1 && packet[1] == 0xd0 &&
            packet[4] == 0xd1 && packet[5] != 0x00) {
            s_last_progress_us = last_receive_us;
            handle_bulk_frame(socket_fd, camera_address, packet,
                              static_cast<size_t>(received));
            continue;
        }

        const SessionEvent event =
            s_session.handle_packet(packet, static_cast<size_t>(received));
        if (s_session.transport_failed()) {
            return ESP_FAIL;
        }
        if (event.type == SessionEvent::Type::ack && heartbeat_pending &&
            !s_session.is_outstanding(heartbeat_index)) {
            ESP_LOGI(TAG, "Heartbeat index %u acknowledged", heartbeat_index);
            heartbeat_pending = false;
            // The session is now settled (a heartbeat round-tripped). Send the
            // first command, as the app does, starting with device info.
            if (s_logged_in && !s_devinfo_requested) {
                s_devinfo_requested = true;
                send_artemis_command(512, "", "ARTEMIS GetDevInfo");
            }
        } else {
            if (log_session_event(event, packet, static_cast<size_t>(received),
                                  "in the ARTEMIS session")) {
                return ESP_ERR_INVALID_STATE;
            }
            if (event.type == SessionEvent::Type::data) {
                process_artemis_response(event);
            }
        }
    }
    return ESP_OK;
}

// Runs the ARTEMIS session and removes the partial file of a download that
// the end of the session interrupted.
static esp_err_t
wait_for_artemis_packets(int socket_fd,
                         const struct sockaddr_in *camera_address,
                         int64_t discovery_started_us)
{
    const esp_err_t error =
        run_artemis_session(socket_fd, camera_address, discovery_started_us);
    s_ack_socket = -1;
    if (s_video.active()) {
        s_video.log_transfer();
    }
    // A finished video is kept; only the one being received is dropped.
    s_video.complete_pending(true);
    camera::store::sd_writer_flush(true);
    if (s_media_file.is_open()) {
        ESP_LOGW(TAG, "Session ended during a download; partial file removed");
        s_media_file.abort();
    }
    s_video.end_session();
    s_download_active = false;
    return error;
}

static esp_err_t send_prelogin(int socket_fd,
                               const struct sockaddr_in *camera_address,
                               int64_t discovery_started_us)
{
    camera::PreLoginRequest request;
    esp_err_t error = build_prelogin_request(&request);
    if (error != ESP_OK) {
        return error;
    }

    s_transport.set_destination(socket_fd, *camera_address);
    s_session.reset();
    error = send_udp_request(socket_fd, camera_address,
                             "AES-encrypted PRE_LOGIN", request);
    if (error != ESP_OK) {
        return error;
    }
    int64_t sent_at_us = esp_timer_get_time();

    // Wait for the camera's first DATA packet, which confirms PRE_LOGIN.
    int64_t response_deadline_us =
        sent_at_us + CAMERA_PRELOGIN_RECEIVE_TIMEOUT_MS * 1000;
    while (!stop_requested()) {
        int64_t remaining_us = response_deadline_us - esp_timer_get_time();
        if (remaining_us <= 0) {
            ESP_LOGW(TAG,
                     "No DATA packet received within %u ms after "
                     "PRE_LOGIN; handshake is not verified",
                     CAMERA_PRELOGIN_RECEIVE_TIMEOUT_MS);
            return ESP_ERR_TIMEOUT;
        }

        error = set_receive_timeout(socket_fd, remaining_us, "PRE_LOGIN");
        if (error != ESP_OK) {
            return error;
        }

        uint8_t response[256];
        struct sockaddr_in source = {};
        ssize_t received = camera_udp_receive_packet(
            socket_fd, response, sizeof(response), &source, "PRE_LOGIN");
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            ESP_LOGE(TAG, "PRE_LOGIN response receive failed: errno %d", errno);
            return ESP_FAIL;
        }

        int64_t received_at_us = esp_timer_get_time();
        ESP_LOGI(TAG,
                 "PRE_LOGIN response received +%lld ms after send / "
                 "+%lld ms since discovery",
                 (long long)((received_at_us - sent_at_us) / 1000),
                 (long long)((received_at_us - discovery_started_us) / 1000));
        if (!from_camera(&source, camera_address)) {
            ESP_LOGW(TAG, "Ignoring UDP packet from unexpected endpoint");
            continue;
        }

        const SessionEvent event =
            s_session.handle_packet(response, static_cast<size_t>(received));
        if (s_session.transport_failed()) {
            return ESP_FAIL;
        }
        if (log_session_event(event, response, static_cast<size_t>(received),
                              "waiting for the PRE_LOGIN response")) {
            return ESP_ERR_INVALID_STATE;
        }
        if (event.type == SessionEvent::Type::data) {
            error = send_artemis_handshake(socket_fd, &source);
            if (error != ESP_OK) {
                return error;
            }
            return wait_for_artemis_packets(socket_fd, &source,
                                            discovery_started_us);
        }
    }
    return ESP_OK;
}

static esp_err_t discover_camera(void)
{
    int64_t discovery_started_us = esp_timer_get_time();
    if (!s_camera_gateway_valid) {
        ESP_LOGE(TAG, "Cannot discover camera because the Wi-Fi gateway "
                      "address is unknown");
        return ESP_ERR_INVALID_STATE;
    }

    int socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    s_timeout_socket = -1;
    if (socket_fd < 0) {
        ESP_LOGE(TAG, "Could not create camera discovery socket: errno %d",
                 errno);
        return ESP_FAIL;
    }

    struct sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_port = htons(CAMERA_DISCOVERY_LOCAL_PORT);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    ESP_LOGI(TAG, "Binding camera UDP socket to local port %u",
             CAMERA_DISCOVERY_LOCAL_PORT);
    if (bind(socket_fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        ESP_LOGE(TAG,
                 "Could not bind camera UDP socket to local port %u: "
                 "errno %d",
                 CAMERA_DISCOVERY_LOCAL_PORT, errno);
        close(socket_fd);
        return ESP_FAIL;
    }

    if (set_receive_timeout(socket_fd,
                            CAMERA_DISCOVERY_RECEIVE_TIMEOUT_MS * 1000LL,
                            "camera discovery") != ESP_OK) {
        close(socket_fd);
        return ESP_FAIL;
    }

    const camera::CameraDiscoveryRequest discovery_packet;
    ESP_LOGI(TAG,
             "Camera discovery target " IPSTR ", source UDP port %u, "
             "payload %u bytes",
             IP2STR(&s_camera_gateway), CAMERA_DISCOVERY_LOCAL_PORT,
             static_cast<unsigned>(discovery_packet.size()));

    for (size_t port_index = 0;
         port_index <
         sizeof(s_camera_discovery_ports) / sizeof(s_camera_discovery_ports[0]);
         port_index++) {
        struct sockaddr_in destination = {};
        destination.sin_family = AF_INET;
        destination.sin_port = htons(s_camera_discovery_ports[port_index]);
        destination.sin_addr.s_addr = s_camera_gateway.addr;
        esp_err_t error = send_udp_request(socket_fd, &destination,
                                           "LBCS discovery", discovery_packet);
        if (error != ESP_OK) {
            close(socket_fd);
            return ESP_FAIL;
        }
    }

    while (!stop_requested() &&
           (esp_timer_get_time() - discovery_started_us) / 1000 <
               CAMERA_DISCOVERY_TIMEOUT_MS) {
        uint8_t response[256];
        struct sockaddr_in source = {};
        ssize_t received = camera_udp_receive_packet(
            socket_fd, response, sizeof(response), &source, "camera discovery");
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            ESP_LOGE(TAG, "Camera discovery receive failed: errno %d", errno);
            close(socket_fd);
            return ESP_FAIL;
        }

        ESP_LOGI(
            TAG, "Camera discovery reply received +%lld ms after start",
            (long long)((esp_timer_get_time() - discovery_started_us) / 1000));

        if (received > 4 && response[0] == 0xf1 &&
            source.sin_addr.s_addr == s_camera_gateway.addr) {
            ESP_LOGI(TAG,
                     "Camera UDP response accepted; using source port "
                     "%u for PRE_LOGIN",
                     ntohs(source.sin_port));
            esp_err_t prelogin_error =
                send_prelogin(socket_fd, &source, discovery_started_us);
            close(socket_fd);
            return prelogin_error;
        }
        ESP_LOGW(TAG,
                 "UDP packet did not match camera response "
                 "(expected F1 prefix from gateway " IPSTR ")",
                 IP2STR(&s_camera_gateway));
    }

    close(socket_fd);
    if (s_stop_requested.load()) {
        return ESP_OK;
    }
    ESP_LOGW(TAG,
             "No matching camera discovery response after %lld ms "
             "(target " IPSTR ", source UDP port %u)",
             (long long)((esp_timer_get_time() - discovery_started_us) / 1000),
             IP2STR(&s_camera_gateway), CAMERA_DISCOVERY_LOCAL_PORT);
    return ESP_ERR_TIMEOUT;
}

// Frees the buffers of the session's containers: clear() and reset() keep
// their capacity, and allocations below CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL
// are in internal RAM. After a session they held about 14 KB of it, so the
// BLE controller of the next search could not start its scan.
static void release_session_memory(void)
{
    s_response = camera::artemis::Reassembler();
    s_bulk = camera::artemis::BulkReassembler();
    s_catalog = camera::artemis::MediaCatalog();
    std::vector<camera::artemis::CatalogEntry>().swap(s_thumb_queue);
    std::vector<camera::artemis::CatalogEntry>().swap(s_download_queue);
    std::string().swap(s_camera_id);
}

esp_err_t camera_protocol_run(const esp_ip4_addr_t *gateway)
{
    if (gateway == NULL || gateway->addr == 0) {
        ESP_LOGE(TAG, "Cannot start camera protocol without a valid gateway");
        return ESP_ERR_INVALID_ARG;
    }
    s_camera_gateway = *gateway;
    s_camera_gateway_valid = true;
    s_stop_requested = false;
    camera::store::sd_writer_begin_session();
    const esp_err_t error = discover_camera();
    camera::store::sd_writer_end_session();
    // All requested work is done: record the sync with the camera.
    if (s_work_done && !s_camera_id.empty() &&
        !camera::store::mark_camera_synced(s_camera_id, (int64_t)time(NULL))) {
        ESP_LOGW(TAG, "Could not record the sync time for '%s'",
                 s_camera_id.c_str());
    }
    release_session_memory();
    return error;
}

void camera_protocol_request_stop(void)
{
    s_stop_requested = true;
}

bool camera_protocol_work_completed(void)
{
    return s_work_done;
}

int camera_protocol_last_battery_percent(void)
{
    return s_last_battery_percent.load();
}

void camera_protocol_init_storage(void)
{
    camera::store::sd_writer_start();
}

void camera_protocol_set_progress_callback(camera_protocol_progress_cb_t cb)
{
    s_progress_callback = cb;
}
