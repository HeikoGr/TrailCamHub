/*
 * This module tees the ESP-IDF log output into a ring buffer and writes it to
 * the SD card from a low-priority task (see sd_log.h).
 */

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "sd_card.h"
#include "sd_log.h"

#define SD_LOG_DIR SD_CARD_MOUNT_POINT "/logs"
#define SD_LOG_PREFIX "log-"
#define SD_LOG_SUFFIX ".txt"
// Longer lines are cut; ESP-IDF log lines are well below this.
#define SD_LOG_LINE_MAX 256
#define SD_LOG_WRITE_CHUNK 4096
#define SD_LOG_FLUSH_INTERVAL_MS (CONFIG_SD_LOG_FLUSH_INTERVAL_S * 1000)
// The writer looks for new lines and flush requests this often.
#define SD_LOG_POLL_MS 250
#define SD_LOG_TASK_STACK 4096
#define SD_LOG_TASK_PRIORITY 2
#define SD_LOG_MAX_PATH 64

static const char *TAG = "trailcamhub_sdlog";

namespace
{

vprintf_like_t s_console;
RingbufHandle_t s_ring;
std::atomic<bool> s_enabled;
std::atomic<unsigned> s_dropped;
SemaphoreHandle_t s_path_lock;
char s_path[SD_LOG_MAX_PATH];
unsigned s_file_number;
FILE *s_file;
size_t s_file_size;
// sd_log_flush(): set by the caller, answered by the writer.
std::atomic<bool> s_flush_requested;
SemaphoreHandle_t s_flushed;

int log_vprintf(const char *format, va_list args)
{
    va_list copy;
    va_copy(copy, args);
    const int result = s_console(format, args);
    if (s_enabled.load(std::memory_order_relaxed)) {
        char line[SD_LOG_LINE_MAX];
        int length = vsnprintf(line, sizeof(line), format, copy);
        if (length > 0) {
            if (length >= (int)sizeof(line)) {
                length = sizeof(line) - 1;
                line[length - 1] = '\n';
            }
            if (xRingbufferSend(s_ring, line, length, 0) != pdTRUE) {
                s_dropped.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    va_end(copy);
    return result;
}

// Number of a log file name, or 0 if the name is not one of ours.
unsigned file_number(const char *name)
{
    const size_t prefix = strlen(SD_LOG_PREFIX);
    if (strncmp(name, SD_LOG_PREFIX, prefix) != 0) {
        return 0;
    }
    char *end = NULL;
    const unsigned long number = strtoul(name + prefix, &end, 10);
    if (end == name + prefix || strcmp(end, SD_LOG_SUFFIX) != 0) {
        return 0;
    }
    return (unsigned)number;
}

void make_path(char *path, size_t size, unsigned number)
{
    snprintf(path, size, SD_LOG_DIR "/" SD_LOG_PREFIX "%05u" SD_LOG_SUFFIX,
             number);
}

// Removes the oldest files so that at most `keep` remain; returns the
// highest file number found.
unsigned prune_files(unsigned keep)
{
    unsigned highest = 0;
    unsigned count = 0;
    unsigned lowest = 0;
    DIR *dir = opendir(SD_LOG_DIR);
    if (dir == NULL) {
        return 0;
    }
    while (struct dirent *entry = readdir(dir)) {
        const unsigned number = file_number(entry->d_name);
        if (number == 0) {
            continue;
        }
        count++;
        if (number > highest) {
            highest = number;
        }
        if (lowest == 0 || number < lowest) {
            lowest = number;
        }
    }
    closedir(dir);
    // Numbers are consecutive apart from gaps left by earlier pruning, so
    // remove from the lowest upwards.
    char path[SD_LOG_MAX_PATH];
    for (unsigned number = lowest; count > keep && number <= highest;
         number++) {
        make_path(path, sizeof(path), number);
        if (remove(path) == 0) {
            count--;
        }
    }
    return highest;
}

// Opens file `number` for appending.
bool open_file(unsigned number)
{
    char path[SD_LOG_MAX_PATH];
    make_path(path, sizeof(path), number);
    s_file = fopen(path, "a");
    if (s_file == NULL) {
        // Not logged: the message would only go into the buffer again.
        return false;
    }
    struct stat info;
    s_file_size = stat(path, &info) == 0 ? (size_t)info.st_size : 0;
    s_file_number = number;
    xSemaphoreTake(s_path_lock, portMAX_DELAY);
    memcpy(s_path, path, sizeof(s_path));
    xSemaphoreGive(s_path_lock);
    return true;
}

bool open_next_file()
{
    if (s_file != NULL) {
        fclose(s_file);
        s_file = NULL;
    }
    // Keep one slot free for the new file.
    prune_files(CONFIG_SD_LOG_MAX_FILES - 1);
    return open_file(s_file_number + 1);
}

// After a wake-up from deep sleep the log goes on in the newest file, so a
// sleeping hub does not start a file per wake-up and push out the older ones.
bool continue_last_file(unsigned highest)
{
    if (highest == 0 || esp_reset_reason() != ESP_RST_DEEPSLEEP) {
        return false;
    }
    char path[SD_LOG_MAX_PATH];
    make_path(path, sizeof(path), highest);
    struct stat info;
    if (stat(path, &info) != 0 ||
        (size_t)info.st_size >= CONFIG_SD_LOG_MAX_FILE_KB * 1024u) {
        return false;
    }
    return open_file(highest);
}

// Writes everything buffered so far and syncs it to the card.
void sync_file()
{
    if (s_file != NULL && (fflush(s_file) != 0 || fsync(fileno(s_file)) != 0)) {
        // The data did not reach the card: continue in a new file.
        fclose(s_file);
        s_file = NULL;
    }
}

void write_bytes(const void *data, size_t size)
{
    if (s_file == NULL) {
        return;
    }
    if (fwrite(data, 1, size, s_file) != size) {
        // Card removed or full: stop writing until the next file.
        fclose(s_file);
        s_file = NULL;
        return;
    }
    s_file_size += size;
}

void writer_task(void *arg)
{
    (void)arg;
    TickType_t last_flush = xTaskGetTickCount();
    bool dirty = false;
    while (true) {
        // Lines go to the file's buffer as they come; the card is synced
        // once per interval, or at once when a flush is requested (the wait
        // is short so that the request is seen quickly).
        size_t size = 0;
        void *data = xRingbufferReceiveUpTo(
            s_ring, &size, pdMS_TO_TICKS(SD_LOG_POLL_MS), SD_LOG_WRITE_CHUNK);
        if (data != NULL) {
            write_bytes(data, size);
            vRingbufferReturnItem(s_ring, data);
            dirty = true;
        }
        const bool flush_now = s_flush_requested.load();
        if (flush_now) {
            // Everything logged up to the request.
            while ((data = xRingbufferReceiveUpTo(
                        s_ring, &size, 0, SD_LOG_WRITE_CHUNK)) != NULL) {
                write_bytes(data, size);
                vRingbufferReturnItem(s_ring, data);
                dirty = true;
            }
        }
        const unsigned dropped = s_dropped.exchange(0);
        if (dropped > 0) {
            char note[64];
            const int length = snprintf(note, sizeof(note),
                                        "[sd_log] %u lines dropped\n", dropped);
            write_bytes(note, length);
            dirty = true;
        }
        const TickType_t now = xTaskGetTickCount();
        if (!flush_now &&
            now - last_flush < pdMS_TO_TICKS(SD_LOG_FLUSH_INTERVAL_MS)) {
            continue;
        }
        last_flush = now;
        if (dirty) {
            sync_file();
            dirty = false;
            if (s_file == NULL ||
                s_file_size >= CONFIG_SD_LOG_MAX_FILE_KB * 1024u) {
                open_next_file();
            }
        }
        if (flush_now) {
            s_flush_requested = false;
            xSemaphoreGive(s_flushed);
        }
    }
}

}  // namespace

esp_err_t sd_log_init(void)
{
    s_path_lock = xSemaphoreCreateMutex();
    s_flushed = xSemaphoreCreateBinary();
    if (s_path_lock == NULL || s_flushed == NULL) {
        return ESP_ERR_NO_MEM;
    }
    // Prefer PSRAM; without it a smaller buffer in internal RAM.
    s_ring = xRingbufferCreateWithCaps(CONFIG_SD_LOG_BUFFER_KB * 1024,
                                       RINGBUF_TYPE_BYTEBUF,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_ring == NULL) {
        s_ring = xRingbufferCreate(8 * 1024, RINGBUF_TYPE_BYTEBUF);
    }
    if (s_ring == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_enabled = true;
    s_console = esp_log_set_vprintf(log_vprintf);
    return ESP_OK;
}

esp_err_t sd_log_start(void)
{
    if (s_ring == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (mkdir(SD_LOG_DIR, 0777) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "Could not create %s: errno %d", SD_LOG_DIR, errno);
        sd_log_disable();
        return ESP_FAIL;
    }
    s_file_number = prune_files(CONFIG_SD_LOG_MAX_FILES);
    if (!continue_last_file(s_file_number) && !open_next_file()) {
        ESP_LOGW(TAG, "Could not open a log file in %s", SD_LOG_DIR);
        sd_log_disable();
        return ESP_FAIL;
    }
    if (xTaskCreate(writer_task, "sd_log", SD_LOG_TASK_STACK, NULL,
                    SD_LOG_TASK_PRIORITY, NULL) != pdPASS) {
        sd_log_disable();
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Writing the log to %s", s_path);
    return ESP_OK;
}

void sd_log_disable(void)
{
    s_enabled = false;
}

void sd_log_current_path(char *path, size_t size)
{
    if (size == 0) {
        return;
    }
    path[0] = '\0';
    if (s_path_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_path_lock, portMAX_DELAY);
    snprintf(path, size, "%s", s_path);
    xSemaphoreGive(s_path_lock);
}

esp_err_t sd_log_flush(uint32_t timeout_ms)
{
    if (!s_enabled.load() || s_flushed == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_flushed, 0);  // an old answer
    s_flush_requested = true;
    return xSemaphoreTake(s_flushed, pdMS_TO_TICKS(timeout_ms)) == pdTRUE
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}
