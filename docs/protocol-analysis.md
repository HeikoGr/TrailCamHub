# Protocol analysis: ARTEMIS / PPPP encryption

**Updated:** 2026-10-27
**Scope:** Communication between the ESP32-S3 bridge and a trail camera (TC100) over
BLE / Wi-Fi / UDP.
**Cameras observed:** TC100 (own hardware) and KJK230 (public reference captures from
the open-source `pi_trailcam` project).

> **Note on keys.** The cryptographic constants used by this camera family are already
> published in public reverse-engineering projects (see "Sources"). This document
> reuses those public constants for interoperability with a user-owned camera. This is
> consistent with the project goal of interoperability, not copying or distributing the
> proprietary app.

---

## 1. Key finding

There is **no runtime key exchange.** The protocol uses a **two-key model** with two
*fixed* keys, both of which are publicly documented:

| Stage | Purpose | Key | Source |
|---|---|---|---|
| **Bootstrap** | only the PRE-LOGIN handshake (`0xF9`), which carries a nonce + time | `a01bc23ed45fF56A` | `pi_trailcam` |
| **Payload** | all ARTEMIS payloads (login, heartbeat, commands, responses) | `xs38nul7cqf7m1va` | `trailcam-automation` |

Both keys are static constants shared across the camera family; neither is negotiated
per session or per device.

### Command-channel parameters (payload stage)

| Parameter | Value |
|---|---|
| Algorithm | **AES-128-CBC** |
| Key | `xs38nul7cqf7m1va` (16 ASCII bytes) |
| IV | 16 zero bytes (`00 … 00`) |
| Body | JSON, NUL-terminated, then padded to the 16-byte block size |

Single-block messages (e.g. the heartbeat) are indistinguishable from ECB, which is why
an early look suggested ECB; the multi-block login exchange shows it is CBC with a zero
IV.

---

## 2. Evidence

### 2.1 The bootstrap key only covers the handshake
The bootstrap key decrypts the PRE-LOGIN payload cleanly into a small JSON object of the
form `{"utcTime":<n>,"nonce":"<hex>"}`. It does **not** decrypt the ARTEMIS payloads.

### 2.2 The payload key is a family-wide constant
The heartbeat plaintext `{"cmdId":525}` encrypts to the Base64 block
`MzlB36X/IVo8ZzI5rG9j1w==`. This exact block appears **byte-for-byte identically** on

- the **TC100** (our own capture), and
- the **KJK230** (`pi_trailcam/tests/debug06012026_2.log:43`).

Identical plaintext → identical ciphertext → on two different devices, which is only
possible with a shared static key.

### 2.3 Verification against real captures
Decrypting the captured TC100 login exchange with the command-channel parameters above
yields clean JSON, for example:

```
login request  -> {"cmdId":0,"usrName":"admin","password":"admin","needVideo":0,
                   "needAudio":0,"utcTime":<unix>,"supportHeartBeat":true}
login response -> {"cmdId":0,"result":0,"token":<number>,"bat_percent":<n>,
                   "errorMsg":"Success"}
```

The login response carries the numeric session `token` that all later commands must
include.

---

## 3. BLE: Wi-Fi credentials in cleartext

The camera hands over the Wi-Fi credentials **in cleartext over BLE** (notification on
characteristic `0x0003`), framed with a 4-byte header (`13 57 01 00`) followed by a
4-byte little-endian length. The payload is a JSON object of the form:

```json
{"ret":0,"ssid":"<ap-ssid>","bssid":"<ap-bssid>","pwd":"<ap-password>"}
```

So the firmware can read the SSID and password directly from this BLE response instead
of guessing them.

**BLE parameters (TC100):** service `0x8801`, write characteristic `0x0002`, notify
characteristic `0x0003`. Wake-up: write `BT_KEY_ON`
(`42 54 5F 4B 45 59 5F 4F 4E`) to the write characteristic.

---

## 4. UDP/RUDP transport (PPPP/CS2, "Tutk")

- Magic `0xF1`; types: `0xD0` DATA, `0xD1` ACK, `0xE0` ALIVE, `0xE1` ALIVE_ACK,
  `0xF0` CLOSE, `0x41/0x42` LBCS discovery, `0xF9` PRE-LOGIN (unused by the genuine
  client).
- ARTEMIS header (20 bytes): `"ARTEMIS\0"` + MsgType (LE; 2 = request, 3 = response)
  + AppSeq (LE) + PayloadLen (LE). It is followed by `Base64(AES-128-CBC(JSON))` and a
  terminating `\x00`.
- Commands: `cmdId 0` login (`usrName`/`password` = `admin`); the response carries a
  numeric `token`. `512` devinfo, `525` keepalive (~3 s), `768` file list,
  `772` thumbnails, `258/259` stream. After login, commands carry the token:
  `{"cmdId":512,"token":<number>}`.
- The real obstacle in the reference project was the **handshake choreography / timing**
  (discovery stabilization, the Magic1/Magic2 packets, sub-10 ms ACK timing, sequence
  resets), not the cryptography — see [`udp-protocol.md`](udp-protocol.md).

---

## 5. Status

All layers needed to talk to the camera are now understood:

- **Transport** (RUDP/PPPP), **discovery**, and the **ARTEMIS framing** — documented.
- **Command-channel encryption** — AES-128-CBC with the public key above.
- **Wi-Fi credentials** — read from the cleartext BLE response.

Implementation can proceed: command encryption/decryption, then login → token →
file list (`768`) → download.

---

## 6. Sources

- [`trailcam-automation`](https://github.com/pwnb0t/trailcam-automation) —
  publishes the AES command-channel key (`xs38nul7cqf7m1va`) and the zero IV
  (`src/constants.py`, "AES command channel (from libArLink.so)").
- [`pi_trailcam`](https://github.com/philibertschlutzki/pi_trailcam) — protocol
  analysis, command IDs, the PRE-LOGIN bootstrap key, and the capture
  (`tests/debug06012026_2.log:43`) used for cross-device verification.
- Own TC100 captures (framing, discovery, heartbeat, login exchange).
