#pragma once

// Persists per-camera media on the SD card: the catalog CSV and the thumbnail
// JPEGs, laid out as
//     /sdcard/<camera-id>/catalog.csv
//     /sdcard/<camera-id>/thumb/<dir>_<num>.jpg
// Only the filesystem-id sanitiser is pure (and host-tested); the read/write
// functions use stdio against the mounted SD card.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "device_info.hpp"
#include "media_catalog.hpp"

namespace camera::store
{

// Maps a camera identity to a safe single path segment: keeps [A-Za-z0-9._-],
// turns anything else into '_', caps the length, and never yields an empty
// name or "."/".." (which would escape or collide on the filesystem).
inline std::string sanitize_camera_id(const std::string &id)
{
    std::string out;
    for (char c : id) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                        c == '.';
        out.push_back(ok ? c : '_');
    }
    if (out.size() > 48)
        out.resize(48);
    if (out.empty())
        out = "unknown";
    if (out == "." || out == "..")
        out = "_" + out;
    return out;
}

// Loads the catalog for `camera_id`. A missing file is treated as an empty
// catalog and still returns true; false means the SD card or file could not be
// read, or the file was not a valid catalog.
bool load_catalog(const std::string &camera_id,
                  camera::artemis::MediaCatalog &out);

// Writes the catalog atomically (temp file + rename). Returns false on error.
bool save_catalog(const std::string &camera_id,
                  const camera::artemis::MediaCatalog &catalog);

// Writes a thumbnail JPEG for (dir, num), creating directories as needed, and
// sets the file's access/modification times to `media_time` (Unix seconds) so
// the thumbnail's date reflects the capture. A media_time <= 0 leaves the
// default timestamp.
bool write_thumbnail(const std::string &camera_id, std::int64_t dir,
                     std::int64_t num, const std::uint8_t *jpeg,
                     std::size_t size, std::int64_t media_time);

// Path of the stored thumbnail for (dir, num); the file may not exist.
std::string thumbnail_path(const std::string &camera_id, std::int64_t dir,
                           std::int64_t num);

// Stores the camera's device info next to its catalog, at
// /sdcard/<camera-id>/camera.json, together with the time it was read
// (Unix seconds, 0 = unknown). An upload sends every camera stored this way.
bool save_camera_info(const camera::artemis::DeviceInfo &info,
                      std::int64_t read_at);
bool load_camera_info(const std::string &camera_id,
                      camera::artemis::DeviceInfo &out, std::int64_t &read_at);

// Ids (sanitized directory names) of all cameras with a camera.json.
std::vector<std::string> list_cameras();

}  // namespace camera::store
