# UDP protocol reference (PPPP/CS2 + ARTEMIS)

**Updated:** 2026-10-27
**Scope:** General reference for the camera's UDP wire protocol: the reliable-UDP
transport (PPPP/CS2, "Tutk"), the ARTEMIS application layer on top of it, the command
set, and the session lifecycle.

This document describes protocol *structure* only. The encryption of the JSON command
bodies is covered separately in [`protocol-analysis.md`](protocol-analysis.md); this
reference treats the encrypted body as an opaque Base64 blob.

> Facts below are marked **[verified]** when confirmed against our own TC100 captures
> and implementation, and **[tentative]** when taken from reference material and not yet
> independently confirmed.

---

## 1. Protocol stack

```
UDP datagram
└─ PPPP/CS2 reliable-UDP frame      (magic 0xF1, type, length)
   └─ ARTEMIS header                (20 bytes, carried as RUDP DATA payload)
      └─ Base64( AES-128-ECB( PKCS7( JSON command ) ) ) + 0x00
```

The camera listens on UDP (discovery is observed on port 40611). A single UDP socket
carries the whole session.

---

## 2. PPPP/CS2 frame

Every packet begins with the magic byte `0xF1`, followed by a 1-byte type and a 16-bit
big-endian length. **[verified]**

| Type byte | Name | Meaning |
|---|---|---|
| `0x41` | `DISCOVERY_REQUEST` | LBCS discovery probe |
| `0x42` | `DISCOVERY_REPLY` | discovery answer from the camera |
| `0xD0` | `DATA` | reliable data (carries the ARTEMIS payload) |
| `0xD1` | `DATA_ACK` | acknowledgement of DATA |
| `0xE0` | `ALIVE` | transport keep-alive (from camera) |
| `0xE1` | `ALIVE_ACK` | answer to ALIVE |
| `0xF0` | `CLOSE` | camera ends the session |
| `0xF9` | `PRE_LOGIN` | encryption-init handshake (see §7) |

### 2.1 Control frames
ALIVE / ALIVE_ACK / CLOSE are 4-byte frames with no payload: `F1 <type> 00 00`.
The camera's ALIVE **must** be answered with ALIVE_ACK, or the camera sends CLOSE.
**[verified]**

### 2.2 Reliable DATA/ACK framing ("DRW")
DATA and ACK use an 8-byte header: **[verified]**

```
F1 <D0|D1> <length:2 BE> D1 <channel=0> <word:2 BE> <payload...>
          └ length = bytes after the length field (header remainder + payload)
```

- For **DATA** (`0xD0`), `word` is the per-session packet **index**.
- For **ACK** (`0xD1`), `word` is the **count** of acknowledged indices, which follow
  as the payload. A single-index ACK is `F1 D1 00 06 D1 00 00 01 <index:2>`.

### 2.3 Discovery (LBCS)
A fixed 24-byte probe; the camera replies with a `0x42` frame. **[verified]**

```
F1 41 00 14 4C 42 43 53 00 00 00 00 00 00 00 00 43 43 43 4A 4A 00 00 00
            └ "LBCS"                             └ "CCCJJ"
```

---

## 3. Reliability layer

The transport is a sliding-window reliable-UDP scheme implemented in
`components/camera_link/pppp_session`. **[verified]**

- DATA packets are numbered per session; indices restart at 0 on `reset()` in both
  directions.
- Up to **4** DATA packets may be outstanding (unacknowledged) at once.
- Each unacknowledged DATA is **retransmitted every 800 ms** until its ACK arrives.
- Received DATA is acknowledged immediately; duplicates (index not ahead of the next
  expected index, using a wrap-around `< 0x8000` comparison) are re-acknowledged but
  not re-delivered.
- ALIVE is answered with ALIVE_ACK; CLOSE and repeated discovery replies are reported
  to the caller.

This layer is free of ESP-IDF dependencies and is unit-tested on the host.

---

## 4. ARTEMIS application layer

An ARTEMIS message is the payload of a DATA frame. Its 20-byte header is: **[verified]**

| Offset | Field | Size | Encoding | Value |
|---|---|---|---|---|
| 0x00 | Signature | 8 | ASCII | `"ARTEMIS\0"` (`41 52 54 45 4D 49 53 00`) |
| 0x08 | MsgType | 4 | uint32 LE | `2` = request, `3` = response |
| 0x0C | AppSeq | 4 | uint32 LE | application sequence counter |
| 0x10 | PayloadLen | 4 | uint32 LE | length of the Base64 string that follows |

After the header comes a `\x00`-terminated **Base64** string. It decodes to an
AES-128-ECB / PKCS7 block whose plaintext is a JSON object. The AES key is **not**
included here — see [`protocol-analysis.md`](protocol-analysis.md).

---

## 5. Command set

Commands are JSON objects distinguished by `cmdId`. After login, every command carries
the numeric session `token` returned by the login response. **[verified]** (command
IDs and shapes confirmed against captured app logs)

| cmdId | Name | Direction | Notes |
|---|---|---|---|
| 0 | LOGIN | C→D | once per session |
| 258 | START_AV | C→D | start audio/video stream |
| 259 | STOP_AV | C→D | stop stream |
| 512 | GET_DEV_INFO | C→D | device/firmware/battery/SD info |
| 525 | HEARTBEAT | C→D | keep-alive, ~every 3 s, fire-and-forget |
| 768 | GET_MEDIA_LIST | C→D | list stored media |
| 772 | GET_THUMBNAIL | C→D | media thumbnails |

### 5.1 Login (cmdId 0)
Request (decrypted):
```json
{"cmdId":0,"usrName":"admin","password":"admin","needVideo":0,"needAudio":0,"utcTime":<unix>,"supportHeartBeat":true}
```
Response carries `errorCode:0` on success and a numeric `token` used by subsequent
commands, e.g. `{"cmdId":512,"token":<number>}`.

### 5.2 Heartbeat (cmdId 525)
Minimal body `{"cmdId":525}`, sent roughly every 3 seconds. The application sequence
for heartbeats starts at `0x10001`. Because the payload key is unknown, the firmware
currently replays a precomputed heartbeat frame rather than encrypting one at runtime.

### 5.3 Large responses (fragmentation)
Large results (e.g. media lists, thumbnails) arrive as multiple frames; each fragment
is acknowledged, and the concatenated payloads form one ARTEMIS message. **[tentative]**

---

## 6. Session lifecycle

Driven by the session state machine
(`components/session_controller/session_fsm`): **[verified]**

```
idle → ble_activation → wifi_join → session → (backoff → …)
```

Within the UDP session: **[verified for discovery/ALIVE/ACK; tentative for login order]**

```
Discovery (LBCS)  →  [stabilization pause]  →  Login handshake  →  Heartbeats + commands
```

---

## 7. Login handshake and timing (the hard part)

Reference work shows that the difficulty is **not** cryptographic but lies in the exact
handshake choreography and timing. **[tentative — from reference captures]**

- The genuine client does **not** send a UDP `PRE_LOGIN` (`0xF9`); it is present in our
  code but is not part of the working flow (see `protocol-analysis.md` §2.4).
- After discovery, a short stabilization pause (~1 s) is needed so the camera leaves
  discovery mode before login.
- Two "magic" ACK-shaped frames (`Magic1`, `Magic2`) are exchanged around login. Their
  exact purpose is **not yet understood**; in our code they are sent with fixed bytes.
- The second login attempt must follow the camera's ACK almost immediately
  (sub-10 ms observed); a delay causes the camera to drop the session with CLOSE.
- The PPPP sequence counter is sometimes reset/resynchronized after the magic frames.

These timing constraints are the main open engineering problem for a reliable login,
independent of the encryption question.

---

## 8. Sources

- Own implementation: `components/camera_link/` (framing, reliable-UDP session),
  `components/session_controller/` (lifecycle).
- Own TC100 captures (framing, discovery, ALIVE, heartbeat).
- Public `pi_trailcam` project (command IDs, login choreography, timing observations).
