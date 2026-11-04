#pragma once

// Typed model of a GET_MEDIA_LIST (cmdId 768) response. Parses the mediaFiles
// array and the paging fields into structs. Pure logic, host-testable.

#include <cstdint>
#include <string>
#include <vector>

namespace camera::artemis
{

struct MediaItem {
    std::int64_t file_type = 0;    // "fileType" (0 and 2 observed; photo/video)
    std::int64_t dir_num = 0;      // "mediaDirNum" (DCIM directory, e.g. 100)
    std::int64_t num = 0;          // "mediaNum" (file number within the dir)
    std::int64_t duration_ms = 0;  // "durationMs"
    std::int64_t id = 0;    // "mediaId" (32-bit id for download/thumbnail)
    std::int64_t time = 0;  // "mediaTime" (Unix seconds; unreliable clock)
};

struct MediaList {
    std::vector<MediaItem> files;
    std::int64_t count = 0;    // "cnt" reported by the camera
    std::int64_t page_no = 0;  // "pageNo"
    std::int64_t result = -1;  // "result" (0 = success)
    // "getMediaListRet": 0 on success. Without an SD card the camera answers
    // -1 with "errorMsg":"Memory card not exist", while "result" stays 0.
    std::int64_t list_ret = 0;
    // "errorMsg"; "Success" on success, empty when absent.
    std::string error_msg;
};

// Parses a media-list response. Returns false if the mediaFiles array is
// absent; missing per-item fields default to 0. The paging and error fields
// are filled in either case.
bool parse_media_list(const std::string &json, MediaList &out);

// True if the camera reported an error for the media list
// (getMediaListRet != 0), e.g. when it has no SD card. errorMsg is not used:
// it is also set on success ("Success").
bool media_list_failed(const MediaList &list);

// Entries per page that the hub asks for. The camera does not report a total
// ("cnt" counts the entries of the page), so a page with fewer entries is the
// last one; a page past the end is empty.
constexpr std::int64_t kMediaListPageSize = 45;

// Pages read at most: a stop for a camera that never sends a short page.
constexpr std::int64_t kMediaListMaxPages = 1000;

// The fields of a media-list request for a page, in the form that
// send_artemis_command() takes: ",\"itemCntPerPage\":45,\"pageNo\":0".
std::string media_list_request_fields(std::int64_t page_no);

// True if a page with the entries of `list` may be followed by another one.
bool media_list_has_more(const MediaList &list);

// One entry as text, for example
// "type=2 dir=100 num=56 id=1828790833 time=2026-11-03T21:11:28". The type is
// the camera's fileType. The time is the camera's wall clock as it shows it,
// so it has no zone (see docs/clock.md).
std::string format_media_item(const MediaItem &item);

}  // namespace camera::artemis
