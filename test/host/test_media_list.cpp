#include <doctest/doctest.h>

#include <string>

#include "media_list.hpp"

using namespace camera::artemis;

namespace
{

// A real GET_MEDIA_LIST (cmdId 768) response captured from the camera on
// hardware. Trimmed to a representative set of items that preserves the shape
// the parser must handle: the paging fields, both file types, and a mediaId
// that overflows int32 (4136037234 > 2^31-1).
const char *kRealResponse =
    "{\"mediaFiles\":["
    "{\"fileType\":0,\"mediaDirNum\":100,\"mediaNum\":36,\"durationMs\":0,"
    "\"mediaId\":4136037234,\"mediaTime\":1766505692},"
    "{\"fileType\":2,\"mediaDirNum\":100,\"mediaNum\":35,\"durationMs\":10000,"
    "\"mediaId\":3987654321,\"mediaTime\":1766505600},"
    "{\"fileType\":0,\"mediaDirNum\":100,\"mediaNum\":1,\"durationMs\":0,"
    "\"mediaId\":1343182691,\"mediaTime\":1766400000}"
    "],\"cnt\":36,\"pageNo\":0,\"getMediaListRet\":0,\"errorMsg\":\"Success\","
    "\"result\":0,\"cmdId\":768}";

}  // namespace

TEST_CASE("parse_media_list reads the paging envelope")
{
    MediaList list;
    REQUIRE(parse_media_list(kRealResponse, list));
    CHECK(list.count == 36);
    CHECK(list.page_no == 0);
    CHECK(list.result == 0);
}

TEST_CASE("parse_media_list reads every item in the array")
{
    MediaList list;
    REQUIRE(parse_media_list(kRealResponse, list));
    REQUIRE(list.files.size() == 3);

    const MediaItem &first = list.files.front();
    CHECK(first.file_type == 0);
    CHECK(first.dir_num == 100);
    CHECK(first.num == 36);
    CHECK(first.duration_ms == 0);
    CHECK(first.id == 4136037234);  // must survive as int64
    CHECK(first.time == 1766505692);

    const MediaItem &second = list.files[1];
    CHECK(second.file_type == 2);
    CHECK(second.num == 35);
    CHECK(second.duration_ms == 10000);
    CHECK(second.id == 3987654321);

    const MediaItem &last = list.files.back();
    CHECK(last.file_type == 0);
    CHECK(last.num == 1);
    CHECK(last.id == 1343182691);
    CHECK(last.time == 1766400000);
}

TEST_CASE("parse_media_list does not bleed a field across item boundaries")
{
    // The first item omits mediaTime; the parser must leave it at 0 rather
    // than reading the next item's mediaTime.
    const std::string json =
        "{\"mediaFiles\":["
        "{\"fileType\":0,\"mediaNum\":2},"
        "{\"fileType\":0,\"mediaNum\":1,\"mediaTime\":1766400000}"
        "],\"cnt\":2,\"result\":0}";
    MediaList list;
    REQUIRE(parse_media_list(json, list));
    REQUIRE(list.files.size() == 2);
    CHECK(list.files[0].num == 2);
    CHECK(list.files[0].time == 0);
    CHECK(list.files[1].time == 1766400000);
}

TEST_CASE("parse_media_list handles an empty list")
{
    const std::string json =
        "{\"mediaFiles\":[],\"cnt\":0,\"pageNo\":0,\"result\":0,\"cmdId\":768}";
    MediaList list;
    REQUIRE(parse_media_list(json, list));
    CHECK(list.files.empty());
    CHECK(list.count == 0);
    CHECK(list.result == 0);
}

TEST_CASE("parse_media_list rejects a response without the array")
{
    const std::string json = "{\"result\":0,\"cmdId\":768}";
    MediaList list;
    CHECK_FALSE(parse_media_list(json, list));
    CHECK(list.files.empty());
}
