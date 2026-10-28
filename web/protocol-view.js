// Live protocol view: taps the serial line stream (dispatched by
// serial-console.js as "serial-line" events) and turns the firmware's log
// lines into a structured picture of the camera session — a stage timeline,
// the parsed media list, and the raw bulk (thumbnail/photo) frames.
//
// It is read-only and best-effort: unknown lines are ignored, so it degrades
// gracefully as the firmware's logging evolves. A "Replay demo log" button
// feeds a bundled sample through the same parser, so the view can be seen
// without a board attached.
(function () {
  "use strict";

  // --- Stage timeline --------------------------------------------------------

  var STAGES = [
    { id: "discovery", label: "Discovery" },
    { id: "prelogin", label: "Pre-Login" },
    { id: "login", label: "Login" },
    { id: "session", label: "Session" },
    { id: "devinfo", label: "Device Info" },
    { id: "list", label: "Media List" },
    { id: "thumb", label: "Thumbnail" },
  ];

  var stageIndex = {};
  STAGES.forEach(function (s, i) {
    stageIndex[s.id] = i;
  });

  var timelineEl = document.getElementById("pv-timeline");
  var mediaWrap = document.getElementById("pv-media");
  var mediaBody = document.getElementById("pv-media-body");
  var mediaCaption = document.getElementById("pv-media-caption");
  var framesWrap = document.getElementById("pv-frames");
  var framesList = document.getElementById("pv-frames-list");
  var replayButton = document.getElementById("pv-replay");

  if (!timelineEl) {
    return; // The section is not on the page.
  }

  var reached = -1; // Highest stage index reached so far.
  var stageNotes = {};
  var currentFrame = null;

  function renderTimeline() {
    timelineEl.innerHTML = "";
    STAGES.forEach(function (stage, i) {
      var li = document.createElement("li");
      li.className =
        "pv-stage " +
        (i < reached ? "done" : i === reached ? "active" : "pending");
      var dot = document.createElement("span");
      dot.className = "pv-dot";
      var label = document.createElement("span");
      label.className = "pv-stage-label";
      label.textContent = stage.label;
      li.appendChild(dot);
      li.appendChild(label);
      if (stageNotes[stage.id]) {
        var note = document.createElement("span");
        note.className = "pv-stage-note";
        note.textContent = stageNotes[stage.id];
        li.appendChild(note);
      }
      timelineEl.appendChild(li);
    });
  }

  // Marks a stage (and everything before it) as reached.
  function reach(id, note) {
    var i = stageIndex[id];
    if (i === undefined) {
      return;
    }
    if (i > reached) {
      reached = i;
    }
    if (note) {
      stageNotes[id] = note;
    }
    renderTimeline();
  }

  function resetView() {
    reached = -1;
    stageNotes = {};
    currentFrame = null;
    renderTimeline();
    mediaWrap.hidden = true;
    mediaBody.innerHTML = "";
    mediaCaption.textContent = "";
    framesWrap.hidden = true;
    framesList.innerHTML = "";
  }

  // --- Media list ------------------------------------------------------------

  function fmtTime(unixSeconds) {
    if (!unixSeconds) {
      return "—";
    }
    var d = new Date(unixSeconds * 1000);
    if (isNaN(d.getTime())) {
      return String(unixSeconds);
    }
    return d.toISOString().replace("T", " ").replace(".000Z", " UTC");
  }

  function renderMediaList(json) {
    var data;
    try {
      data = JSON.parse(json);
    } catch (e) {
      return false;
    }
    var files = (data && data.mediaFiles) || [];
    mediaBody.innerHTML = "";
    files.forEach(function (f, i) {
      var tr = document.createElement("tr");
      var type = Number(f.fileType) === 0 ? "📷 Photo" : "🎬 Video";
      var cells = [
        String(i + 1),
        type,
        f.mediaDirNum != null ? String(f.mediaDirNum) : "—",
        f.mediaNum != null ? String(f.mediaNum) : "—",
        f.mediaId != null ? String(f.mediaId) : "—",
        fmtTime(f.mediaTime),
      ];
      cells.forEach(function (text) {
        var td = document.createElement("td");
        td.textContent = text;
        tr.appendChild(td);
      });
      mediaBody.appendChild(tr);
    });
    var photos = files.filter(function (f) {
      return Number(f.fileType) === 0;
    }).length;
    mediaCaption.textContent =
      files.length +
      " files · " +
      photos +
      " photos · " +
      (files.length - photos) +
      " videos" +
      (data.cnt != null ? " · camera reports cnt " + data.cnt : "");
    mediaWrap.hidden = files.length === 0;
    return true;
  }

  // --- Bulk frames -----------------------------------------------------------

  var HEX_LINE = /^(?:[0-9a-fA-F]{2}[ :]?)+$/;
  var ARTEMIS_SIG = [0x41, 0x52, 0x54, 0x45, 0x4d, 0x49, 0x53, 0x00];

  function u16le(bytes, off) {
    return bytes[off] | (bytes[off + 1] << 8);
  }
  function u32le(bytes, off) {
    return (
      (bytes[off] |
        (bytes[off + 1] << 8) |
        (bytes[off + 2] << 16) |
        (bytes[off + 3] << 24)) >>>
      0
    );
  }

  function startFrame(subtype, seq, bodyLen, payloadLen) {
    currentFrame = {
      subtype: subtype,
      seq: seq,
      bodyLen: bodyLen,
      payloadLen: payloadLen,
      bytes: [],
    };
    reach("thumb", "receiving bulk data");
  }

  function finishFrame() {
    if (!currentFrame) {
      return;
    }
    var f = currentFrame;
    currentFrame = null;

    var card = document.createElement("div");
    card.className = "pv-frame";

    var head = document.createElement("div");
    head.className = "pv-frame-head";
    var subtypeHex = f.subtype.toString(16).padStart(2, "0");
    var kind =
      f.subtype === 0x04
        ? "thumbnail"
        : f.subtype === 0x03
        ? "photo"
        : f.subtype === 0x02
        ? "video"
        : "?";
    head.textContent =
      "subtype 0x" +
      subtypeHex +
      " (" +
      kind +
      ") · seq " +
      f.seq +
      " · payload " +
      f.payloadLen +
      " B";
    card.appendChild(head);

    var b = f.bytes;
    var notes = [];
    var hasSig =
      b.length >= 8 &&
      ARTEMIS_SIG.every(function (v, i) {
        return b[i] === v;
      });
    if (hasSig) {
      var ver = u32le(b, 8);
      var typ = u32le(b, 12);
      var len = u32le(b, 16);
      notes.push("ARTEMIS record: ver " + ver + ", typ " + typ + ", len " + len);
      // Record payload = 72-byte media header + JPEG.
      var hdr = 20;
      if (b.length >= hdr + 0x34) {
        notes.push(
          "media header: dir " +
            u16le(b, hdr + 0x20) +
            ", num " +
            u16le(b, hdr + 0x22) +
            ", id " +
            u32le(b, hdr + 0x30)
        );
      }
      var jpeg = hdr + 72;
      if (b.length >= jpeg + 2) {
        notes.push(
          b[jpeg] === 0xff && b[jpeg + 1] === 0xd8
            ? "JPEG SOI present at offset " + jpeg + " ✓"
            : "no JPEG SOI at offset " + jpeg
        );
      }
    } else if (b.length) {
      notes.push("no ARTEMIS signature in first bytes");
    }

    notes.forEach(function (text) {
      var n = document.createElement("div");
      n.className = "pv-frame-note";
      n.textContent = text;
      card.appendChild(n);
    });

    if (b.length) {
      var hex = document.createElement("pre");
      hex.className = "pv-hex";
      hex.textContent = hexDump(b);
      card.appendChild(hex);
    }

    framesWrap.hidden = false;
    framesList.insertBefore(card, framesList.firstChild);
    // Keep the list bounded.
    while (framesList.children.length > 24) {
      framesList.removeChild(framesList.lastChild);
    }
  }

  function hexDump(bytes) {
    var lines = [];
    for (var i = 0; i < bytes.length; i += 16) {
      var slice = bytes.slice(i, i + 16);
      var hex = slice
        .map(function (v) {
          return v.toString(16).padStart(2, "0");
        })
        .join(" ");
      var ascii = slice
        .map(function (v) {
          return v >= 0x20 && v < 0x7f ? String.fromCharCode(v) : ".";
        })
        .join("");
      lines.push(
        i.toString(16).padStart(4, "0") + "  " + hex.padEnd(47, " ") + "  " + ascii
      );
    }
    return lines.join("\n");
  }

  // --- Line parsing ----------------------------------------------------------

  // Strips ANSI color codes and the ESP-IDF "I (12345) tag:" prefix, returning
  // the bare message.
  function messageOf(rawLine) {
    var line = rawLine.replace(/\u001b\[[0-9;]*m/g, "").trimEnd();
    var m = line.match(/^[VDIWE] \(\d+\) [^:]+:\s?(.*)$/);
    return m ? m[1] : line.trim();
  }

  var patterns = [
    [/^Camera discovery target/, function () { reach("discovery", "searching"); }],
    [/^Camera UDP response accepted/, function () { reach("prelogin"); stageNotes.discovery = "found"; }],
    [/^PRE_LOGIN response received/, function () { reach("login"); stageNotes.prelogin = "ok"; }],
    [/^ARTEMIS login succeeded; token (\d+)/, function (m) { reach("session"); stageNotes.login = "token " + m[1]; }],
    [/^Heartbeat index \d+ acknowledged/, function () { reach("devinfo"); stageNotes.session = "settled"; }],
    [/^ARTEMIS response cmdId 512/, function () { reach("list"); stageNotes.devinfo = "ok"; }],
    [/^Media list parsed: (\d+) items/, function (m) { stageNotes.list = m[1] + " items"; renderTimeline(); }],
    [/^Sent ARTEMIS GetThumbnail \(cmdId 772\)/, function () { reach("thumb", "requested"); }],
    [
      /^ARTEMIS response cmdId 768:\s*(\{.*\})\s*$/,
      function (m) {
        reach("list");
        if (renderMediaList(m[1])) {
          stageNotes.list = "loaded";
          renderTimeline();
        }
      },
    ],
    [
      /^BULK frame: subtype 0x([0-9a-fA-F]+) seq (\d+) body_len (\d+) payload (\d+) bytes/,
      function (m) {
        finishFrame(); // flush any previous frame
        startFrame(parseInt(m[1], 16), Number(m[2]), Number(m[3]), Number(m[4]));
      },
    ],
  ];

  function handleLine(rawLine) {
    var msg = messageOf(rawLine);
    if (!msg) {
      return;
    }

    // While collecting a bulk frame, consume its hex-dump lines.
    if (currentFrame && HEX_LINE.test(msg)) {
      msg
        .replace(/[: ]/g, " ")
        .trim()
        .split(/\s+/)
        .forEach(function (pair) {
          if (pair.length === 2) {
            currentFrame.bytes.push(parseInt(pair, 16));
          }
        });
      return;
    }
    // A non-hex line ends the current frame.
    if (currentFrame) {
      finishFrame();
    }

    for (var i = 0; i < patterns.length; i++) {
      var m = msg.match(patterns[i][0]);
      if (m) {
        patterns[i][1](m);
        return;
      }
    }
  }

  window.addEventListener("serial-line", function (e) {
    handleLine(String(e.detail || ""));
  });
  window.addEventListener("serial-reset", resetView);

  // --- Demo replay -----------------------------------------------------------

  var DEMO_LINES = buildDemoLog();

  function buildDemoLog() {
    var g = "\u001b[0;32m"; // ESP-IDF INFO color, to exercise ANSI stripping
    var r = "\u001b[0m";
    var T = "trailcamhub_protocol";
    function info(ts, msg) {
      return g + "I (" + ts + ") " + T + ": " + msg + r;
    }
    var mediaJson = JSON.stringify({
      mediaFiles: [
        { fileType: 0, mediaDirNum: 100, mediaNum: 36, durationMs: 0, mediaId: 4136037234, mediaTime: 1766505692 },
        { fileType: 2, mediaDirNum: 100, mediaNum: 35, durationMs: 10000, mediaId: 3987654321, mediaTime: 1766505600 },
        { fileType: 0, mediaDirNum: 100, mediaNum: 1, durationMs: 0, mediaId: 1343182691, mediaTime: 1766400000 },
      ],
      cnt: 36,
      pageNo: 0,
      result: 0,
      cmdId: 768,
    });
    // A plausible first thumbnail frame: ARTEMIS header + 72-byte media header
    // (dir 100, num 36, id 4136037234) + JPEG SOI.
    var payload = new Array(94).fill(0);
    [0x41, 0x52, 0x54, 0x45, 0x4d, 0x49, 0x53, 0x00].forEach(function (v, i) { payload[i] = v; });
    payload[8] = 0x04; // ver
    payload[12] = 0x04; // typ
    payload[16] = 0x80; payload[17] = 0x02; // len (lo bytes)
    payload[20 + 0x20] = 100 & 0xff; payload[20 + 0x21] = (100 >> 8) & 0xff;
    payload[20 + 0x22] = 36 & 0xff; payload[20 + 0x23] = (36 >> 8) & 0xff;
    var id = 4136037234;
    payload[20 + 0x30] = id & 0xff;
    payload[20 + 0x31] = (id >>> 8) & 0xff;
    payload[20 + 0x32] = (id >>> 16) & 0xff;
    payload[20 + 0x33] = (id >>> 24) & 0xff;
    payload[92] = 0xff; payload[93] = 0xd8; // JPEG SOI
    var hexLines = [];
    for (var i = 0; i < payload.length; i += 16) {
      hexLines.push(
        info(
          "41" + i,
          payload
            .slice(i, i + 16)
            .map(function (v) { return v.toString(16).padStart(2, "0"); })
            .join(" ")
        )
      );
    }
    return [
      info(1200, "Camera discovery target 192.168.43.1, source UDP port 16734, payload 24 bytes"),
      info(1420, "Camera UDP response accepted; using source port 32108 for PRE_LOGIN"),
      info(1650, "PRE_LOGIN response received +230 ms after send / handshake is not verified"),
      info(1980, "ARTEMIS login succeeded; token 2043399292"),
      info(2010, "Starting ARTEMIS heartbeat loop"),
      info(2240, "Heartbeat index 1 acknowledged"),
      info(2480, "ARTEMIS response cmdId 512: {\"model\":\"TC100\",\"result\":0,\"cmdId\":512}"),
      info(2600, "Sent ARTEMIS GetMediaList (cmdId 768), index 3"),
      info(2900, "ARTEMIS response cmdId 768: " + mediaJson),
      info(2910, "Media list parsed: 3 items (2 photos, 1 videos), cnt 36, pageNo 0, result 0"),
      info(2915, "  first item: dir 100 num 36 id 4136037234 time 1766505692"),
      info(3100, "Sent ARTEMIS GetThumbnail (cmdId 772), index 4"),
      info(3400, "BULK frame: subtype 0x04 seq 0 body_len 98 payload 94 bytes"),
    ].concat(hexLines, [info(3600, "Heartbeat index 2 acknowledged")]);
  }

  function replayDemo() {
    resetView();
    var i = 0;
    replayButton.disabled = true;
    var timer = setInterval(function () {
      if (i >= DEMO_LINES.length) {
        clearInterval(timer);
        finishFrame();
        replayButton.disabled = false;
        return;
      }
      handleLine(DEMO_LINES[i++]);
    }, 180);
  }

  if (replayButton) {
    replayButton.addEventListener("click", replayDemo);
  }

  renderTimeline();
})();
