# Media identity

How a recording on a camera is identified, in the firmware, the backend and
the app. Issue: #85.

## The problem with `dir`/`num`

The media list (`cmdId 768`) names each recording by `mediaDirNum`/`mediaNum`
(`dir`/`num`, for example `100_36`). The camera reuses these numbers: after its
SD card was cleared, it starts again at `100_1`, `100_2`, … A key built from
`dir`/`num` therefore treats a new recording as one that is already known: its
thumbnail is not fetched, it is not reported, and old pictures are shown under
new times.

## Findings on the TC100

Checked on hardware by the maintainer (November 2026):

- `mediaId` (32-bit, unsigned) was **never handed out twice**, also not after
  clearing the card in the camera, while `dir`/`num` were reused.
- The same recording **keeps its `mediaId`** across a restart of the camera and
  across a battery change.

## Rule

- **A recording is identified by `mediaId`, per camera.** The full identity is
  camera id + `mediaId`.
- A `mediaId` that is missing or `<= 0` is invalid; such an item is ignored.
- `mediaId` is an unsigned 32-bit value. Store it as a 64-bit integer (firmware)
  or a JavaScript number (backend, app), never as a signed 32-bit integer.
- A known `dir`/`num` with a new `mediaId` is a **new recording**, never an
  update of the old one.
- `dir`/`num` stay as data of a recording: for display, and because the camera
  is asked for thumbnails and files by `dir`/`num`. On the card they are unique
  at any one time, so they may only be used to address a recording that is in
  the current media list.

## Where it applies

| Layer | Key | State |
| --- | --- | --- |
| Hub catalog (`catalog.csv` v2) | `mediaId` | done |
| Hub SD files (`thumb/`, `media/`) | `dir`/`num` | to do |
| Check-in, ack, commands | `dir`/`num` | to do |
| Backend (DynamoDB, S3) | `dir`/`num` | to do |
| App (`mediaKey()`, video request) | `dir`/`num` | to do |

## Hub catalog

- `MediaCatalog::merge()` matches by `mediaId`. Old entries keep their flags,
  also when a new recording took over their `dir`/`num`.
- `merge()` marks the entries of the current media list as `listed` (not
  persisted). Only listed entries are camera work (thumbnails, downloads,
  requested videos): any other entry is not on the card, and its `dir`/`num`
  may belong to another recording now.
- The answer to a thumbnail request names `dir`/`num`; it is matched to the
  listed entry with that `dir`/`num`.
- Interfaces that do not carry the `mediaId` yet (the backend API) map
  `dir`/`num` to the listed entry, or else to the newest entry with that
  `dir`/`num`.
- `catalog.csv` has the header `# trailcamhub-catalog v2`. The columns did not
  change (`dir,num,mediaId,fileType,mediaTime,flags`); v1 is read as well and
  written back as v2. Rows without a valid `mediaId` and repeated `mediaId`s
  are dropped. Firmware before this change does not read v2 and starts with an
  empty catalog.
