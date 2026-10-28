#include "media_list.hpp"

#include "artemis_json.hpp"

namespace camera::artemis
{

bool parse_media_list(const std::string &json, MediaList &out)
{
    out = MediaList{};

    std::int64_t value = 0;
    if (json_find_int(json, "cnt", value))
        out.count = value;
    if (json_find_int(json, "pageNo", value))
        out.page_no = value;
    if (json_find_int(json, "result", value))
        out.result = value;

    const std::size_t key = json.find("\"mediaFiles\"");
    if (key == std::string::npos)
        return false;
    const std::size_t array_begin = json.find('[', key);
    if (array_begin == std::string::npos)
        return false;
    const std::size_t array_end = json.find(']', array_begin);
    if (array_end == std::string::npos)
        return false;

    std::size_t pos = array_begin;
    while (true) {
        const std::size_t obj_begin = json.find('{', pos);
        if (obj_begin == std::string::npos || obj_begin > array_end)
            break;
        const std::size_t obj_end = json.find('}', obj_begin);
        if (obj_end == std::string::npos || obj_end > array_end)
            break;

        // Scope each field lookup to this object's substring so a key missing
        // from one item cannot be read from the next.
        const std::string obj = json.substr(obj_begin, obj_end - obj_begin + 1);
        MediaItem item;
        json_find_int(obj, "fileType", item.file_type);
        json_find_int(obj, "mediaDirNum", item.dir_num);
        json_find_int(obj, "mediaNum", item.num);
        json_find_int(obj, "durationMs", item.duration_ms);
        json_find_int(obj, "mediaId", item.id);
        json_find_int(obj, "mediaTime", item.time);
        out.files.push_back(item);

        pos = obj_end + 1;
    }

    return true;
}

}  // namespace camera::artemis
