#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "media_list.hpp"
#include "media_transfer.hpp"

using namespace camera::artemis;

TEST_CASE("build_download_request matches the app's cmdId 1285 shape")
{
    CHECK(build_download_request(0, 102, 940, 12345678) ==
          "{\"cmdId\":1285,\"downloadReqs\":[{\"fileType\":0,\"dirNum\":102,"
          "\"mediaNum\":940}],\"token\":12345678}");
}

TEST_CASE("build_thumbnail_request matches the app's cmdId 772 shape")
{
    CHECK(build_thumbnail_request(1, 100, 36, 42) ==
          "{\"cmdId\":772,\"thumbnailReqs\":[{\"fileType\":1,\"dirNum\":100,"
          "\"mediaNum\":36}],\"token\":42}");
}

TEST_CASE("request builders take fileType/dir/num from a MediaItem")
{
    MediaItem item;
    item.file_type = 2;
    item.dir_num = 100;    // mediaDirNum maps to the request's dirNum
    item.num = 35;         // mediaNum
    item.id = 3987654321;  // mediaId is not part of the request

    CHECK(build_download_request(item, 7) ==
          "{\"cmdId\":1285,\"downloadReqs\":[{\"fileType\":2,\"dirNum\":100,"
          "\"mediaNum\":35}],\"token\":7}");
    CHECK(build_thumbnail_request(item, 7) ==
          "{\"cmdId\":772,\"thumbnailReqs\":[{\"fileType\":2,\"dirNum\":100,"
          "\"mediaNum\":35}],\"token\":7}");
}

namespace
{

// Builds a bulk record payload: 72 zero bytes with the known fields written at
// their offsets, followed by the given JPEG bytes.
std::vector<std::uint8_t> make_payload(std::uint16_t dir_num,
                                       std::uint16_t media_num,
                                       std::uint32_t media_id,
                                       const std::vector<std::uint8_t> &jpeg)
{
    std::vector<std::uint8_t> p(kMediaRecordHeaderSize, 0);
    p[0x20] = static_cast<std::uint8_t>(dir_num & 0xff);
    p[0x21] = static_cast<std::uint8_t>((dir_num >> 8) & 0xff);
    p[0x22] = static_cast<std::uint8_t>(media_num & 0xff);
    p[0x23] = static_cast<std::uint8_t>((media_num >> 8) & 0xff);
    p[0x28] = static_cast<std::uint8_t>(media_id & 0xff);
    p[0x29] = static_cast<std::uint8_t>((media_id >> 8) & 0xff);
    p[0x2a] = static_cast<std::uint8_t>((media_id >> 16) & 0xff);
    p[0x2b] = static_cast<std::uint8_t>((media_id >> 24) & 0xff);
    p.insert(p.end(), jpeg.begin(), jpeg.end());
    return p;
}

}  // namespace

TEST_CASE("parse_media_record reads the header and locates the JPEG")
{
    // JPEG SOI + a couple of bytes + EOI, as a stand-in for the real image.
    const std::vector<std::uint8_t> jpeg = {0xff, 0xd8, 0xff, 0xe0,
                                            0x00, 0x10, 0xff, 0xd9};
    const std::vector<std::uint8_t> payload =
        make_payload(102, 940, 4136037234u, jpeg);

    MediaRecordHeader header;
    std::size_t jpeg_offset = 0;
    std::size_t jpeg_size = 0;
    REQUIRE(parse_media_record(payload.data(), payload.size(), header,
                               jpeg_offset, jpeg_size));
    CHECK(header.dir_num == 102);
    CHECK(header.media_num == 940);
    CHECK(header.media_id == 4136037234);  // survives as int64, > int32
    CHECK(jpeg_offset == kMediaRecordHeaderSize);
    CHECK(jpeg_size == jpeg.size());

    const std::uint8_t *bytes = payload.data() + jpeg_offset;
    CHECK(bytes[0] == 0xff);
    CHECK(bytes[1] == 0xd8);  // JPEG SOI
    CHECK(bytes[jpeg_size - 2] == 0xff);
    CHECK(bytes[jpeg_size - 1] == 0xd9);  // JPEG EOI
}

TEST_CASE("parse_media_record accepts a header-only record")
{
    const std::vector<std::uint8_t> payload =
        make_payload(100, 1, 1343182691u, {});
    MediaRecordHeader header;
    std::size_t jpeg_offset = 0;
    std::size_t jpeg_size = 123;
    REQUIRE(parse_media_record(payload.data(), payload.size(), header,
                               jpeg_offset, jpeg_size));
    CHECK(header.dir_num == 100);
    CHECK(header.media_num == 1);
    CHECK(header.media_id == 1343182691);
    CHECK(jpeg_offset == kMediaRecordHeaderSize);
    CHECK(jpeg_size == 0);
}

TEST_CASE("parse_media_record reads a real thumbnail header (#105)")
{
    // Header of a thumbnail from a TC100 (firmware 2.2.5.9); the media list
    // named dir 100 num 3 with mediaId 2597690595 and 17361 JPEG bytes.
    std::vector<std::uint8_t> payload = {
        0x46, 0x34, 0x3a, 0x43, 0x31, 0x3a, 0x35, 0x36, 0x3a, 0x30, 0x36, 0x3a,
        0x45, 0x44, 0x3a, 0x33, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x64, 0x00, 0x03, 0x00,
        0xd1, 0x43, 0x00, 0x00, 0xe3, 0x9c, 0xd5, 0x9a, 0x00, 0x00, 0x00, 0x00};
    payload.resize(kMediaRecordHeaderSize + 17361, 0);

    MediaRecordHeader header;
    std::size_t jpeg_offset = 0;
    std::size_t jpeg_size = 0;
    REQUIRE(parse_media_record(payload.data(), payload.size(), header,
                               jpeg_offset, jpeg_size));
    CHECK(header.dir_num == 100);
    CHECK(header.media_num == 3);
    CHECK(header.media_id == 2597690595);
    CHECK(jpeg_size == 17361);
}

TEST_CASE("parse_media_record rejects a payload shorter than the header")
{
    const std::vector<std::uint8_t> payload(kMediaRecordHeaderSize - 1, 0);
    MediaRecordHeader header;
    std::size_t jpeg_offset = 0;
    std::size_t jpeg_size = 0;
    CHECK_FALSE(parse_media_record(payload.data(), payload.size(), header,
                                   jpeg_offset, jpeg_size));
    CHECK_FALSE(parse_media_record(nullptr, 0, header, jpeg_offset, jpeg_size));
}
