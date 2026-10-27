# Protocol analysis: ARTEMIS / PPPP encryption

**Updated:** 2026-10-27
**Scope:** Communication between the ESP32-S3 bridge and a trail camera (TC100) over
BLE / Wi-Fi / UDP.
**Cameras observed:** TC100 (own hardware) and KJK230 (public reference captures from
the open-source `pi_trailcam` project).

> **Policy note.** This document records only observations from *our own* device
> traffic and from public open-source captures. It contains **no** extracted vendor
> key and **no** instructions for defeating the payload encryption. The encrypted
> payload layer is deliberately left closed (see "Open points"). This is consistent
> with the project goal of interoperability with a user-owned camera, not copying or
> circumventing the proprietary app.

---

## 1. Key finding

There is **no runtime key exchange.** The protocol uses a **two-key model** with two
*fixed* AES-128-ECB keys:

| Stage | Purpose | Key | Status |
|---|---|---|---|
| **Bootstrap** | only the PRE-LOGIN handshake (`0xF9`), which carries a nonce + time | a publicly documented 16-byte constant (see `pi_trailcam`) | known |
| **Payload** | all ARTEMIS payloads (login, heartbeat, commands, responses) | a different, **fixed, family-wide** constant | **unknown** |

Both keys are static. The payload key is **not** negotiated per session and **not**
derived per device.

---

## 2. Evidence

### 2.1 The bootstrap key only decrypts the handshake
The public bootstrap constant decrypts the PRE-LOGIN payload cleanly into a small JSON
object of the form `{"utcTime":<n>,"nonce":"<hex>"}`. The same constant produces only
garbage on the actual ARTEMIS payloads (heartbeat, login response).

### 2.2 The payload key is a family-wide constant
The heartbeat plaintext `{"cmdId":525}` encrypts to the Base64 block
`MzlB36X/IVo8ZzI5rG9j1w==`. This exact block appears **byte-for-byte identically** on

- the **TC100** (our own capture), and
- the **KJK230** (`pi_trailcam/tests/debug06012026_2.log:43`).

Identical plaintext → identical ciphertext → on **two different devices**. Under
AES-ECB this is only possible if both devices use the same static key. A per-session or
per-device key would produce different ciphertext.

### 2.3 The payload key is NOT the bootstrap key
Encrypting `{"cmdId":525}` with the bootstrap constant yields a different block than
`MzlB…`. The bootstrap and payload keys are therefore distinct.

### 2.4 The PRE-LOGIN nonce is a red herring
The genuine vendor app does not send a UDP PRE-LOGIN (`0xF9`) at all; in the reference
project this step turned out to be a misinterpretation and was removed. Because the
payloads are encrypted with the fixed payload constant regardless, the nonce plays no
role in payload encryption.

### 2.5 Why a brute-force / known-plaintext search does not recover the key
The payload key is a 16-byte constant, but it is not present as a contiguous string or
block in the vendor library (a scan over every byte window and every printable string
of the `.so` and the app's `classes*.dex` files yields no match); it is assembled or
obfuscated at runtime. AES cannot be inverted from plaintext/ciphertext pairs, so a
known-plaintext approach does not reveal the key either.

---

## 3. BLE: Wi-Fi credentials in cleartext

The camera hands over the Wi-Fi credentials **in cleartext over BLE** (notification on
characteristic `0x0003`), framed with a 4-byte header (`13 57 01 00`) followed by a
4-byte little-endian length. The payload is a JSON object of the form:

```json
{"ret":0,"ssid":"<ap-ssid>","bssid":"<ap-bssid>","pwd":"<ap-password>"}
```

This removes the earlier "SSID/password unknown" obstacle: the firmware can read the
SSID and password directly from this BLE response instead of guessing them.

**BLE parameters (TC100):** service `0x8801`, write characteristic `0x0002`, notify
characteristic `0x0003`. Wake-up: write `BT_KEY_ON`
(`42 54 5F 4B 45 59 5F 4F 4E`) to the write characteristic.

---

## 4. UDP/RUDP transport (PPPP/CS2, "Tutk")

- Magic `0xF1`; types: `0xD0` DATA, `0xD1` ACK, `0xE0` ALIVE, `0xF0` CLOSE,
  `0x41/0x42` LBCS discovery, `0xF9` PRE-LOGIN (unused by the genuine client).
- ARTEMIS header (20 bytes): `"ARTEMIS\0"` + MsgType (LE; 2 = request, 3 = response)
  + AppSeq (LE) + PayloadLen (LE). It is followed by `Base64(AES-128-ECB/PKCS7(JSON))`
  and a terminating `\x00`.
- Commands: `cmdId 0` login (`usrName`/`password` = `admin`); the response carries a
  numeric `token`. `512` devinfo, `525` keepalive (~3 s), `768` file list,
  `772` thumbnails, `258/259` stream. After login, commands carry the token:
  `{"cmdId":512,"token":<number>}`.
- The real obstacle in the reference project was **not** the cryptography but the
  **handshake choreography / timing** (discovery stabilization, the Magic1/Magic2
  packets, sub-10 ms ACK timing, sequence resets).

---

## 5. Consequences for development

**Blocked (without reverse-engineering the vendor binary):** reading or writing the
encrypted JSON bodies — the single fixed payload constant is missing.

**Buildable without further app disassembly:**

1. **BLE credential retrieval** — SSID/BSSID/password from the cleartext BLE JSON
   (section 3).
2. **RUDP transport + discovery + ARTEMIS framing + login choreography**, including
   timing, with the JSON bodies left as placeholders. Host-testable.

Together these provide the complete protocol scaffold, ready to be "armed" once the
payload constant becomes available through a route that is compatible with the
project's policy and the applicable terms of use.

---

## 6. Sources / artifacts

- Our own hardware captures (TC100): PRE-LOGIN payload, heartbeat, login response.
- `pi_trailcam` (public): `Protocol_analysis.md`,
  `tests/MITM_Captures/ble_udp_2.log` (BLE credential JSON),
  `tests/debug06012026_2.log:43` (on-wire heartbeat, KJK230),
  `tests/MITM_Captures/App_Log_2025-12-23log.txt` (cleartext commands, token example).
