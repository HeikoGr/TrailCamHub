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
};

// Parses a media-list response. Returns false if the mediaFiles array is
// absent; missing per-item fields default to 0.
bool parse_media_list(const std::string &json, MediaList &out);

}  // namespace camera::artemis
