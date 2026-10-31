/*
 * This module reads command lines from the USB-Serial-JTAG port and runs them
 * with esp_console (see debug_console.h):
 *   search                       search for the camera now
 *   stop                         stop searching or end the session
 *   status                       workflow status, memory and SD card
 *   heap [blocks]                internal RAM by capability and region
 *   ls [path]                    directory listing
 *   cat <path> [offset [length]] file contents as Base64 lines
 *   rm [-r] <path>               delete a file or a directory tree
 *   sdbench [total_kb [chunk_kb]] SD card write throughput
 * Paths are below the SD card root; "/sdcard/a", "/a" and "a" are the same.
 */

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "sdkconfig.h"

#include "debug_console.h"
#include "sd_card.h"
#include "sd_log.h"

#define CONSOLE_LINE_MAX 256
#define CONSOLE_MAX_ARGS 8
#define CONSOLE_STACK_SIZE 6144
#define CONSOLE_PRIORITY 2
// The driver's buffers are in internal RAM, which the camera session needs.
// It drops output when the buffer stays full for 50 ms; "cat" readers find
// the gaps by the offsets and request them again. With 1 KB the log lines of
// the BLE stack start and of a camera found right after it (about 2 KB within
// 100 ms) were partly lost.
#define CONSOLE_TX_BUFFER 2048
#define CONSOLE_RX_BUFFER 256
// Raw bytes per "@D" line: 192 bytes are 256 Base64 characters.
#define CONSOLE_CAT_LINE_BYTES 192
#define CONSOLE_CAT_READ (CONSOLE_CAT_LINE_BYTES * 32)

static const char *TAG = "trailcamhub_console";

namespace
{

debug_console_callbacks_t s_callbacks;

SemaphoreHandle_t s_lock;
// Shared with the setters under s_lock.
char s_phase[16] = "idle";
char s_camera_ssid[33];
int64_t s_retry_deadline_us;
uint32_t s_media_done;
uint32_t s_media_stored;
uint32_t s_media_total;

// Turns a path argument into a path below the SD card root: "" for the root,
// "a/b" otherwise. Rejects ".." segments; "." and empty segments are dropped.
bool relative_path(const char *in, std::string &out)
{
    std::string path = in;
    const std::string mount = SD_CARD_MOUNT_POINT;
    if (path.compare(0, mount.size(), mount) == 0 &&
        (path.size() == mount.size() || path[mount.size()] == '/')) {
        path.erase(0, mount.size());
    }
    out.clear();
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find('/', start);
        if (end == std::string::npos) {
            end = path.size();
        }
        const std::string segment = path.substr(start, end - start);
        if (segment == "..") {
            return false;
        }
        if (!segment.empty() && segment != ".") {
            if (!out.empty()) {
                out += '/';
            }
            out += segment;
        }
        start = end + 1;
    }
    return true;
}

std::string absolute(const std::string &relative)
{
    return relative.empty() ? std::string(SD_CARD_MOUNT_POINT)
                            : std::string(SD_CARD_MOUNT_POINT) + "/" + relative;
}

bool ends_with(const std::string &text, const char *suffix)
{
    const size_t length = strlen(suffix);
    return text.size() >= length &&
           text.compare(text.size() - length, length, suffix) == 0;
}

// Files that are open for writing, which must not be removed because that
// corrupts the FAT volume: the log being written and unfinished downloads
// (".part") or catalog updates (".tmp").
bool file_in_use(const std::string &path)
{
    if (ends_with(path, ".part") || ends_with(path, ".tmp")) {
        return true;
    }
#if CONFIG_SD_LOG_ENABLE
    char log_path[64];
    sd_log_current_path(log_path, sizeof(log_path));
    if (path == log_path) {
        return true;
    }
#endif
    return false;
}

// True while the camera session may be writing to the card.
bool session_writing()
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    // The backend sync reads the files.
    const bool writing = strcmp(s_phase, "connecting") == 0 ||
                         strcmp(s_phase, "connected") == 0 ||
                         strcmp(s_phase, "uploading") == 0;
    xSemaphoreGive(s_lock);
    return writing;
}

int fail(const char *reason)
{
    printf("@ERR %s\n", reason);
    return 1;
}

int cmd_search(int argc, char **argv)
{
    if (s_callbacks.on_search != NULL) {
        s_callbacks.on_search();
    }
    return 0;
}

int cmd_stop(int argc, char **argv)
{
    if (s_callbacks.on_stop != NULL) {
        s_callbacks.on_stop();
    }
    return 0;
}

int cmd_status(int argc, char **argv)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    printf("phase: %s\n", s_phase);
    printf("camera: %s\n", s_camera_ssid);
    if (strcmp(s_phase, "retry_wait") == 0) {
        const int64_t left_us = s_retry_deadline_us - esp_timer_get_time();
        printf("retry in: %lld s\n",
               (long long)(left_us > 0 ? left_us / 1000000 : 0));
    }
    printf("media: %lu of %lu done, %lu stored\n", (unsigned long)s_media_done,
           (unsigned long)s_media_total, (unsigned long)s_media_stored);
    xSemaphoreGive(s_lock);
    printf("uptime: %lld s\n", (long long)(esp_timer_get_time() / 1000000));
    printf("internal RAM: %u KB free, %u KB minimum\n",
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
           (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) /
                      1024));
    printf(
        "PSRAM: %u KB free, %u KB minimum\n",
        (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
        (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM) / 1024));
    uint64_t total = 0;
    uint64_t free_bytes = 0;
    if (esp_vfs_fat_info(SD_CARD_MOUNT_POINT, &total, &free_bytes) == ESP_OK) {
        printf("SD card: %llu MB free of %llu MB\n",
               (unsigned long long)(free_bytes >> 20),
               (unsigned long long)(total >> 20));
    } else {
        printf("SD card: not mounted\n");
    }
    return 0;
}

// Free, largest free block and minimum free size for heaps with `caps`.
void print_heap(const char *name, uint32_t caps)
{
    multi_heap_info_t info = {};
    heap_caps_get_info(&info, caps);
    printf("%-14s %7u free, %7u largest, %7u minimum, %4u blocks\n", name,
           (unsigned)info.total_free_bytes, (unsigned)info.largest_free_block,
           (unsigned)info.minimum_free_bytes, (unsigned)info.allocated_blocks);
}

// A block of a heap, copied while the heap is walked.
struct BlockCopy {
    uintptr_t address;
    uint32_t size;
    bool used;
    uint8_t head[16];
};

struct BlockList {
    BlockCopy *blocks;
    size_t capacity;
    size_t count;
};

// Copies the blocks of the small DMA-capable heaps (the pool reserved by
// CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL among them); the large main heap is
// skipped. Runs with the heap locked, so it must not print or allocate.
bool copy_block(walker_heap_into_t heap, walker_block_info_t block, void *arg)
{
    BlockList *list = static_cast<BlockList *>(arg);
    if (heap.end - heap.start > 64 * 1024 || list->count >= list->capacity) {
        return false;
    }
    BlockCopy &copy = list->blocks[list->count++];
    copy.address = reinterpret_cast<uintptr_t>(block.ptr);
    copy.size = block.size;
    copy.used = block.used;
    if (block.used) {
        memcpy(copy.head, block.ptr, block.size < 16 ? block.size : 16);
    }
    return true;
}

// Lists the copied blocks, with the first bytes of each used block as a hint
// to its owner.
int print_blocks()
{
    BlockList list = {};
    list.capacity = 512;
    list.blocks = static_cast<BlockCopy *>(
        heap_caps_malloc(list.capacity * sizeof(BlockCopy), MALLOC_CAP_SPIRAM));
    if (list.blocks == NULL) {
        return fail("no memory");
    }
    heap_caps_walk(MALLOC_CAP_DMA, copy_block, &list);
    printf("Blocks of the DMA-capable heaps below 64 KB:\n");
    for (size_t i = 0; i < list.count; i++) {
        const BlockCopy &block = list.blocks[i];
        printf("  %08x %6u %s", (unsigned)block.address, (unsigned)block.size,
               block.used ? "used" : "free");
        if (block.used) {
            const size_t count = block.size < 16 ? block.size : 16;
            printf(" ");
            for (size_t j = 0; j < count; j++) {
                printf(" %02x", block.head[j]);
            }
        }
        printf("\n");
    }
    free(list.blocks);
    return 0;
}

// Internal RAM by capability and by region, to see which memory a component
// is short of (the BLE controller, for example, needs DMA-capable RAM).
// "heap blocks" also lists the blocks of the small DMA-capable heaps.
int cmd_heap(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "blocks") == 0) {
        return print_blocks();
    }
    print_heap("internal", MALLOC_CAP_INTERNAL);
    print_heap("internal 8bit", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    print_heap("DMA", MALLOC_CAP_DMA);
    print_heap("RTC RAM", MALLOC_CAP_RTCRAM);
    print_heap("retention", MALLOC_CAP_RETENTION);
    print_heap("PSRAM", MALLOC_CAP_SPIRAM);
    printf("Regions of internal RAM:\n");
    heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
    return 0;
}

int cmd_ls(int argc, char **argv)
{
    std::string relative;
    if (!relative_path(argc > 1 ? argv[1] : "", relative)) {
        return fail("bad path");
    }
    const std::string directory = absolute(relative);
    DIR *dir = opendir(directory.c_str());
    if (dir == NULL) {
        return fail("no directory");
    }
    while (struct dirent *entry = readdir(dir)) {
        const std::string full = directory + "/" + entry->d_name;
        struct stat info = {};
        stat(full.c_str(), &info);
        printf("%c %10lld %lld %s\n", S_ISDIR(info.st_mode) ? 'd' : 'f',
               (long long)info.st_size, (long long)info.st_mtime,
               entry->d_name);
    }
    closedir(dir);
    return 0;
}

// Sends the bytes as "@D <offset> <base64>" lines between "@FILE <size>" and
// "@END <offset> <length> <crc32>", where the CRC-32 (as zlib computes it)
// covers the bytes that were sent. The driver drops output that the host does
// not read in time, so the offsets let the reader find gaps and request them
// again.
int cmd_cat(int argc, char **argv)
{
    std::string relative;
    if (argc < 2 || !relative_path(argv[1], relative) || relative.empty()) {
        return fail("usage: cat <path> [offset [length]]");
    }
    const long long offset = argc > 2 ? strtoll(argv[2], NULL, 0) : 0;
    long long length = argc > 3 ? strtoll(argv[3], NULL, 0) : -1;
    const std::string path = absolute(relative);
    struct stat info = {};
    if (stat(path.c_str(), &info) != 0 || S_ISDIR(info.st_mode)) {
        return fail("no file");
    }
    if (offset < 0 || offset > (long long)info.st_size) {
        return fail("bad offset");
    }
    if (length < 0 || offset + length > (long long)info.st_size) {
        length = info.st_size - offset;
    }
    FILE *file = fopen(path.c_str(), "rb");
    if (file == NULL) {
        return fail("cannot open");
    }
    if (fseek(file, offset, SEEK_SET) != 0) {
        fclose(file);
        return fail("cannot seek");
    }
    uint8_t *buffer = static_cast<uint8_t *>(heap_caps_malloc(
        CONSOLE_CAT_READ, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buffer == NULL) {
        buffer = static_cast<uint8_t *>(malloc(CONSOLE_CAT_READ));
    }
    if (buffer == NULL) {
        fclose(file);
        return fail("no memory");
    }
    printf("@FILE %lld\n", (long long)info.st_size);
    unsigned char text[CONSOLE_CAT_LINE_BYTES * 4 / 3 + 4];
    uint32_t crc = 0;
    long long position = offset;
    long long left = length;
    bool read_error = false;
    while (left > 0) {
        const size_t want =
            left > CONSOLE_CAT_READ ? CONSOLE_CAT_READ : (size_t)left;
        const size_t got = fread(buffer, 1, want, file);
        if (got == 0) {
            read_error = true;
            break;
        }
        crc = esp_rom_crc32_le(crc, buffer, got);
        for (size_t done = 0; done < got; done += CONSOLE_CAT_LINE_BYTES) {
            const size_t line = got - done > CONSOLE_CAT_LINE_BYTES
                                    ? CONSOLE_CAT_LINE_BYTES
                                    : got - done;
            size_t text_length = 0;
            mbedtls_base64_encode(text, sizeof(text), &text_length,
                                  buffer + done, line);
            printf("@D %lld %.*s\n", position + (long long)done,
                   (int)text_length, text);
        }
        position += got;
        left -= got;
    }
    free(buffer);
    fclose(file);
    printf("@END %lld %lld %08lx\n", (long long)offset,
           (long long)(position - offset), (unsigned long)crc);
    return read_error ? fail("read error") : 0;
}

// Writes a test file in chunks of `chunk_kb` from internal DMA-capable RAM,
// unbuffered like the SD writer of the video download, and reports the
// throughput and the slowest chunk; the file is removed afterwards.
int cmd_sdbench(int argc, char **argv)
{
    const long total_kb = argc > 1 ? strtol(argv[1], NULL, 0) : 4096;
    const long chunk_kb = argc > 2 ? strtol(argv[2], NULL, 0) : 8;
    if (total_kb <= 0 || chunk_kb <= 0 || chunk_kb > 64) {
        return fail("usage: sdbench [total_kb [chunk_kb (1-64)]]");
    }
    if (session_writing()) {
        return fail("the camera session is running; stop it first");
    }
    const size_t chunk = (size_t)chunk_kb * 1024;
    uint8_t *buffer = static_cast<uint8_t *>(
        heap_caps_malloc(chunk, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    if (buffer == NULL) {
        return fail("no internal DMA-capable memory for the chunk");
    }
    for (size_t i = 0; i < chunk; i++) {
        buffer[i] = static_cast<uint8_t>(i * 31 + 7);
    }
    const std::string path = absolute("sdbench.tmp");
    FILE *file = fopen(path.c_str(), "wb");
    if (file == NULL) {
        free(buffer);
        return fail("cannot create the test file");
    }
    setvbuf(file, NULL, _IONBF, 0);
    const long chunks = (total_kb + chunk_kb - 1) / chunk_kb;
    int64_t slowest_us = 0;
    const int64_t started_us = esp_timer_get_time();
    bool ok = true;
    for (long i = 0; i < chunks && ok; i++) {
        const int64_t chunk_started_us = esp_timer_get_time();
        ok = fwrite(buffer, 1, chunk, file) == chunk;
        const int64_t took_us = esp_timer_get_time() - chunk_started_us;
        if (took_us > slowest_us) {
            slowest_us = took_us;
        }
    }
    const int64_t written_us = esp_timer_get_time() - started_us;
    ok = fsync(fileno(file)) == 0 && ok;
    ok = fclose(file) == 0 && ok;
    const int64_t total_us = esp_timer_get_time() - started_us;
    remove(path.c_str());
    free(buffer);
    if (!ok) {
        return fail("write failed");
    }
    const long long bytes = (long long)chunks * (long long)chunk;
    printf("sdbench: %lld KB in %ld chunks of %ld KB: %lld KB/s (writes %lld "
           "ms, with sync %lld ms), slowest chunk %lld ms\n",
           bytes / 1024, chunks, chunk_kb,
           (long long)(bytes * 1000 / 1024 / (total_us / 1000 + 1)),
           (long long)(written_us / 1000), (long long)(total_us / 1000),
           (long long)(slowest_us / 1000));
    return 0;
}

// Removes the tree below `path`, then `path` itself. Files in use are kept
// (and with them their directories) and counted in `kept`.
bool remove_tree(const std::string &path, unsigned &removed, unsigned &kept)
{
    struct stat info = {};
    if (stat(path.c_str(), &info) != 0) {
        return false;
    }
    if (S_ISDIR(info.st_mode)) {
        // Collected first, so the directory is not changed while it is read.
        std::vector<std::string> names;
        DIR *dir = opendir(path.c_str());
        if (dir == NULL) {
            return false;
        }
        while (struct dirent *entry = readdir(dir)) {
            names.push_back(entry->d_name);
        }
        closedir(dir);
        bool ok = true;
        for (const std::string &name : names) {
            ok = remove_tree(path + "/" + name, removed, kept) && ok;
        }
        if (!ok) {
            return false;
        }
        if (rmdir(path.c_str()) != 0) {
            ESP_LOGW(TAG, "Could not remove %s: errno %d", path.c_str(), errno);
            return false;
        }
        removed++;
        return true;
    }
    if (file_in_use(path)) {
        printf("kept %s (being written)\n", path.c_str());
        kept++;
        return false;
    }
    if (remove(path.c_str()) != 0) {
        ESP_LOGW(TAG, "Could not delete %s: errno %d", path.c_str(), errno);
        return false;
    }
    removed++;
    return true;
}

int cmd_rm(int argc, char **argv)
{
    bool recursive = false;
    const char *argument = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0) {
            recursive = true;
        } else if (argument == NULL) {
            argument = argv[i];
        } else {
            argument = NULL;
            break;
        }
    }
    std::string relative;
    if (argument == NULL || !relative_path(argument, relative)) {
        return fail("usage: rm [-r] <path>");
    }
    if (relative.empty()) {
        return fail("the root cannot be removed");
    }
    if (session_writing()) {
        return fail("the camera session is running; stop it first");
    }
    const std::string path = absolute(relative);
    struct stat info = {};
    if (stat(path.c_str(), &info) != 0) {
        return fail("no such file");
    }
    if (S_ISDIR(info.st_mode) && !recursive) {
        return fail("is a directory; use rm -r");
    }
    unsigned removed = 0;
    unsigned kept = 0;
    const bool ok = remove_tree(path, removed, kept);
    printf("removed %u entries\n", removed);
    ESP_LOGI(TAG, "Removed %s (%u entries)", path.c_str(), removed);
    if (kept != 0) {
        return fail("files in use were kept");
    }
    return ok ? 0 : fail("not everything could be removed");
}

void register_command(const char *name, const char *help, const char *hint,
                      esp_console_cmd_func_t func)
{
    esp_console_cmd_t command = {};
    command.command = name;
    command.help = help;
    command.hint = hint;
    command.func = func;
    ESP_ERROR_CHECK(esp_console_cmd_register(&command));
}

void register_commands()
{
    register_command("search", "Search for the camera now", NULL, cmd_search);
    register_command("stop", "Stop searching or end the camera session", NULL,
                     cmd_stop);
    register_command("status", "Show the workflow status, memory and SD card",
                     NULL, cmd_status);
    register_command(
        "heap",
        "Show internal RAM by capability and by region, and PSRAM; "
        "with \"blocks\" the blocks of the small DMA-capable heaps",
        "[blocks]", cmd_heap);
    register_command("sdbench",
                     "Write a test file to the SD card in chunks and report "
                     "the throughput",
                     "[total_kb [chunk_kb]]", cmd_sdbench);
    register_command("ls", "List a directory: type, size, mtime, name",
                     "[path]", cmd_ls);
    register_command("cat", "Send a file as Base64 lines (@FILE, @D, @END)",
                     "<path> [offset [length]]", cmd_cat);
    register_command("rm",
                     "Delete a file, or a directory tree with -r. Delete a "
                     "camera's catalog.csv to download everything again",
                     "[-r] <path>", cmd_rm);
    ESP_ERROR_CHECK(esp_console_register_help_command());
}

void run_line(char *line)
{
    printf("> %s\n", line);
    int result = 0;
    const esp_err_t error = esp_console_run(line, &result);
    if (error == ESP_ERR_NOT_FOUND) {
        fail("unknown command; try help");
    } else if (error == ESP_ERR_INVALID_ARG) {
        // Only white space.
    } else if (error != ESP_OK) {
        printf("@ERR %s\n", esp_err_to_name(error));
    } else if (result == 0) {
        printf("@OK\n");
    }
    // A command that failed has printed its "@ERR" line.
    fflush(stdout);
}

void console_task(void *)
{
    char line[CONSOLE_LINE_MAX];
    size_t length = 0;
    bool overflow = false;
    uint8_t input[64];
    for (;;) {
        const int count =
            usb_serial_jtag_read_bytes(input, sizeof(input), portMAX_DELAY);
        for (int i = 0; i < count; i++) {
            const char c = static_cast<char>(input[i]);
            if (c == '\r' || c == '\n') {
                if (overflow) {
                    fail("line too long");
                } else if (length != 0) {
                    line[length] = '\0';
                    run_line(line);
                }
                length = 0;
                overflow = false;
            } else if (c == '\b' || c == 0x7f) {
                if (length != 0) {
                    length--;
                }
            } else if (length + 1 < sizeof(line)) {
                line[length++] = c;
            } else {
                overflow = true;
            }
        }
    }
}

}  // namespace

esp_err_t debug_console_start(const debug_console_callbacks_t *callbacks)
{
    s_callbacks = *callbacks;
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    usb_serial_jtag_driver_config_t driver_config =
        USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    driver_config.tx_buffer_size = CONSOLE_TX_BUFFER;
    driver_config.rx_buffer_size = CONSOLE_RX_BUFFER;
    esp_err_t error = usb_serial_jtag_driver_install(&driver_config);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "USB-Serial-JTAG driver: %s", esp_err_to_name(error));
        return error;
    }
    usb_serial_jtag_vfs_use_driver();

    esp_console_config_t console_config = ESP_CONSOLE_CONFIG_DEFAULT();
    console_config.max_cmdline_length = CONSOLE_LINE_MAX;
    console_config.max_cmdline_args = CONSOLE_MAX_ARGS;
    error = esp_console_init(&console_config);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Console: %s", esp_err_to_name(error));
        return error;
    }
    register_commands();

    // The stack goes to PSRAM where there is some, to spare internal RAM.
    BaseType_t created = pdFAIL;
#if CONFIG_SPIRAM
    created =
        xTaskCreateWithCaps(console_task, "console", CONSOLE_STACK_SIZE, NULL,
                            CONSOLE_PRIORITY, NULL, MALLOC_CAP_SPIRAM);
#endif
    if (created != pdPASS) {
        created = xTaskCreate(console_task, "console", CONSOLE_STACK_SIZE, NULL,
                              CONSOLE_PRIORITY, NULL);
    }
    if (created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Serial console ready; type help");
    return ESP_OK;
}

void debug_console_set_status(const char *phase, const char *ssid,
                              uint32_t retry_delay_ms)
{
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_phase, sizeof(s_phase), "%s", phase);
    snprintf(s_camera_ssid, sizeof(s_camera_ssid), "%s", ssid);
    s_retry_deadline_us = esp_timer_get_time() + (int64_t)retry_delay_ms * 1000;
    if (strcmp(phase, "searching") == 0) {
        s_media_done = 0;
        s_media_stored = 0;
        s_media_total = 0;
    }
    xSemaphoreGive(s_lock);
}

void debug_console_set_media_progress(uint32_t done, uint32_t stored,
                                      uint32_t total)
{
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_media_done = done;
    s_media_stored = stored;
    s_media_total = total;
    xSemaphoreGive(s_lock);
}
