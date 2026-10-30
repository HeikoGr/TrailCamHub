# Video download (playback stream)

Status: implemented in `camera_link` and tried on one camera (TC100, firmware 2.2.5.9): several videos of about 460 frames were received completely in a row, each as an H.264 and an AAC file. Photos are downloaded as files (cmdId 1285, see `media_transfer.hpp`); for a video the camera acknowledges that request but sends no data. The TrailCam Go app gets videos by playing them back. This page summarizes the flow as documented by [`pwnb0t/trailcam-automation`](https://github.com/pwnb0t/trailcam-automation) (`docs/download-video.md`, `docs/video-download-apk-flow.md`, `docs/video-v4-decrypt-notes.md`), whose client reconstructs videos that match the camera's SD-card files frame by frame, and how the firmware implements it.

## Control plane

| Command | Body | Answer |
|---|---|---|
| 769 `START_PLAY_RECORD` | `fileType`, `dirNum`, `mediaNum`, `sessionNo` (random) | `startPbRet`, `videoWidth`, `videoHeight`, `totalFrame`, `totalTime`, `result` |
| 770 `STOP_PLAY_RECORD` | – | – |

The app sends `fileType: 1` for videos, while the media list of our camera reports videos as `fileType: 2`. The camera refuses playback with 1 (`startPbRet: -3`) and accepts 2, so the firmware sends the list's value first and, if the camera refuses (`startPbRet` or `result` not 0), repeats the request once with the app's value 1. Heartbeats continue during the stream.

## Data plane

The frames arrive on bulk subtype `0x02` with a running 16-bit sequence and are acknowledged with the same window ACK as thumbnails. In sequence order, the payloads form a byte stream of consecutive ARTEMIS records (one per video or audio frame, not aligned to the bulk frames). Records of version 4 carry a 108-byte header:

| Offset | Field |
|---|---|
| 8 | `pts_ms` (u32 LE) |
| 16 | data length of a video frame (equals record length − 108) |
| 20 | data length of an audio frame |
| 28, 32 | width, height (0 for audio) |
| 48 | `sessionNo` of the 769 request |

In every 0x1000-byte page of the data, the first 0x60 bytes are AES-128-CBC encrypted with the command key `xs38nul7cqf7m1va` and a zero IV, each page on its own; a page with fewer than 0x60 bytes left stays plain. Decrypted video data is H.264 in Annex-B form (start codes), audio data AAC with ADTS headers. The app drops a marker frame that starts with `00 00 00 01 01`. The camera does not mark the end of the stream; it stops sending, and the app stops it with 770. A preview stream on subtype `0x03` (low-resolution track) is ignored.

## Firmware implementation

- `FrameReorder` puts the bulk frames in order (shared with the photo download), `PlaybackStream` splits the byte stream into records, `playback_record` parses the header, decrypts the pages and finds the Annex-B start. All three are host-tested (`test/host/test_playback.cpp`).
- `camera_protocol.cpp` sends 769 after the photos, writes video frames to `<dir>_<num>.h264` and audio frames to `<dir>_<num>.aac` in `/sdcard/<camera>/media/` (both as `.part` until complete), ends the stream after 3 s without frames, sends 770 and waits until no more frames of the stopped stream arrive (1 s) before the next video. A record with another session number is dropped; if the camera ignores the requested number, its own is followed after 30 records.
- Each record is held in PSRAM until it is complete (up to 3 MB), so the option `CONFIG_CAMERA_DOWNLOAD_VIDEOS` needs PSRAM (on for the T-Dongle-C5).

The raw `.h264` file plays in VLC. One MP4 is made without re-encoding with

```sh
ffmpeg -framerate 25 -i 100_2.h264 -i 100_2.aac -c copy 100_2.mp4
```

(25 frames per second is the rate of the camera tried so far: 459 frames in 18.36 s, and the presentation times step by 40 ms; `totalFrame` and `totalTime` in the log give the rate of another camera). Writing the MP4 on the dongle is a later step, once the streams are verified.

## Observed speed

With the current firmware a 15-second 1080p video (459 frames, about 9.5 MB of H.264 and 124 KB of AAC) takes about 37 s on the T-Dongle-C5, at about 260 KB/s; three videos in a row were received completely, with no failed ACK send. A full sync of 45 such videos therefore takes about half an hour.

The first version took about 340 s for an 18-second video (about 29 KB/s). Two causes were found:

- **Reorder window too small.** The camera keeps sending about 120 frames ahead of the first frame it has no acknowledgement for. With a reorder window of 32 frames, everything beyond it was dropped unacknowledged and sent again later, so only about 32 frames got through per round (after 200 frames: 10,962 frames repeated by the camera, 4,475 beyond the window, but only 5 shortly ahead of the expected one). Thumbnails, at about 20 frames, fit into the window, which is why they were always fast. The window is now 256 frames with PSRAM (about 377 KB per stream); the speed rose to about 218 KB/s.
- **Wi-Fi TX buffers used up.** At that speed, one ACK per frame (a few hundred per second) with the station in modem sleep, where outgoing frames wait until the radio wakes, used up the 16 Wi-Fi TX buffers: thousands of sends failed with `ENOMEM`, including the heartbeat, and the second video stalled. The firmware now switches Wi-Fi power save off while it is connected to the camera (BLE is released then; modem sleep returns on disconnect) and acknowledges playback frames together, one ACK per 4 frames or after 10 ms without a frame; each ACK lists the last 16 sequences.

An earlier guess, that the camera paces the stream by the session heartbeat (about two video frames per heartbeat at 1.5 s), did not hold: with the heartbeat at 500 ms (`CONFIG_CAMERA_VIDEO_HEARTBEAT_INTERVAL_MS`) the speed stayed the same. SD writes take about a third of the transfer time (about 12 s of 37 s) and are the next limit.

## What the log shows

`ARTEMIS response cmdId 769: {...}` with the camera's answer, `First video frame: WxH, pts …` with the first 16 decrypted bytes (they should start with `00 00 00 01`), progress every 100 frames, and a summary per video: video and audio frames, dropped markers, records of another session and other records, and the file sizes, followed by `Video transfer:` with the counts of frames out of order, frames repeated by the camera, frames beyond the reorder window and failed ACK sends, the time spent in SD writes (in total and the longest), the internal heap (free and minimum) and frames far behind the expected one. `Video transfer:` is also logged with every progress line and when the session ends during a video. The first 40 frames beyond the reorder window, and then every 500th, are logged as `Playback frame seq …, expected … (delta …)` with their first bytes.
