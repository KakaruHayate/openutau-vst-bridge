# OpenUtau DAW Integration Protocol

**Status:** Draft v1 (redesigned — not wire-compatible with PR #2187)
**Relation to PR #2187:** Mechanism lineage only. The PR's *ideas* are kept (discovery file, hash-deduped incremental sync, heartbeat, reconnect backoff, playback flush); its *code and wire format* are discarded. See §15 for rationale.
**Intended use:** Contract between the OpenUtau main repo (API/server side) and an independently maintained DAW plugin project (client side).

---

## 1. Scope & Goals

Defines the contract for synchronizing OpenUtau projects, track configuration, and rendered audio into a DAW, plus transport feedback from the DAW.

Goals:

- **Minimal surface in the main repo.** OpenUtau exposes a small, stable local API. Everything plugin/UI-shaped lives in a separate project.
- **Two-tier framing.** JSON control plane (text-debuggable) + binary data plane for audio. **Audio never travels inside JSON** (no base64, no gzip-wrapped-in-JSON — see §15).
- **Incremental by default.** Only changed state is pushed; audio is transferred on demand by hash.
- **Localhost trust only.** No auth; "anything running as the same user may connect".

Non-goals (v1):

- Remote / cross-machine connections.
- ARA (Audio Random Access) tempo/transport extension.
- Bidirectional tempo-map sync.
- **Audio format negotiation.** 44.1 kHz stereo is a hard engine limit of OpenUtau (`WaveFormat.CreateIeeeFloatWaveFormat(44100, 2)` in `PlaybackManager`); the wire format is fixed to match, not negotiated.

---

## 2. Roles & Topology

| Role | Process | Responsibility |
|---|---|---|
| **Server** | DAW plugin (VST3/AU/AAX) | Listens on `127.0.0.1:<port>`, publishes a discovery file, answers requests, renders audio to the DAW track |
| **Client** | OpenUtau main app | Scans for discovery files, connects, pushes project/track/audio state |

Connection direction: **plugin = TCP server, OpenUtau = TCP client**. One active connection at a time.

---

## 3. Transport

- **Protocol:** TCP, loopback only (`127.0.0.1`).
- **Port:** dynamic, chosen by the plugin at startup.
- **Framing:** two planes on the same socket:
  - **Control plane:** newline-delimited `UTF-8 <header> <json>\n` lines.
  - **Data plane:** length-prefixed binary frames for audio (`audio <hash> <length>\n` followed by exactly `length` raw bytes).
- **Ordering:** TCP ordering + a client-side write mutex. No sequence numbers in v1.

### Timeouts & heartbeats

| Item | Value | Owner |
|---|---|---|
| `init` handshake timeout | 5 s | OpenUtau |
| Request timeout (control-plane) | 10 s | OpenUtau |
| Heartbeat send interval | 5 s | Plugin |
| Heartbeat liveness check | every 2 s | OpenUtau |
| Heartbeat dead threshold | 15 s without any message | OpenUtau |
| Reconnect backoff | 500 ms, 1 s, 2 s (then give up) | OpenUtau |

---

## 4. Service Discovery & Version Negotiation

### Discovery file

One JSON file per plugin instance:

- **Path:** `%TEMP%/OpenUtau/PluginServers/<name>.json` (per-user temp on all OSes).
- **Schema (v1):**

```json
{
  "port": 52341,
  "name": "OpenUtau Bridge (Track 1)",
  "apiVersion": "1.1"
}
```

- Plugin writes/re-writes the file whenever it (re)binds the port; deletes it on shutdown.
- OpenUtau scans `*.json`, probes each port (attempt `bind(127.0.0.1:<port>)`; if the bind succeeds the server is gone), deletes stale files.

### Version negotiation

- `apiVersion` in the discovery file; echoed in `InitResponse`.
- **Major mismatch** → refuse connection ("plugin protocol incompatible, please update").
- **Minor skew** → connect; the newer side restricts itself to messages/fields present in the older minor (§10).

---

## 5. Message Framing

### 5.1 Control plane (line-based)

| Header | Direction | Meaning |
|---|---|---|
| `request:<uuid>:<kind>` | OpenUtau → Plugin | Request; plugin MUST reply with `response:<uuid>` |
| `response:<uuid>` | Plugin → OpenUtau | Reply; JSON envelope `DawResult` |
| `notification:<kind>` | either | Fire-and-forget; no reply |
| `close` | OpenUtau → Plugin | Raw string, no payload; clean teardown |

Response envelope:

```json
{ "success": true, "data": { }, "error": null }
```

Unknown `request:` kinds / malformed JSON → failed envelope, never a dropped connection. Unknown `notification:` kinds → log and ignore.

### 5.2 Data plane (binary audio frames)

```
audio <hash> <length>\n
<length bytes of raw audio>
```

- `hash`: decimal XXH64 of the payload bytes, **serialized as a decimal string** (e.g. `"13507256038857166760"`). Never as a JSON number — 64-bit values exceed the 2^53 safe-integer limit of JS/`double` parsers. In the data-plane frame header the hash is the same decimal string (unquoted, as it is outside JSON).
- `length`: decimal byte count. The receiver MUST read exactly `length` bytes after the header line (the frame does not end at `\n`). A length above **268435456** (256 MiB, `DawAudio.MaxFrameBytes` — about 12.7 minutes of 44.1 kHz stereo float32) is a malformed frame: the receiver MUST refuse it as a protocol error rather than allocate, because the length is peer-controlled. Senders MUST NOT emit frames above this bound.
- Distinguish the planes on receive: a line starting with `audio ` is a data frame header; anything else is a control line.

---

## 6. Message Set

### 6.1 OpenUtau → Plugin

#### `init` (request)

- `request:<uuid>:init`, payload `{ "ustx": "<full USTX project document>" }`
- Response `data`: `{ "apiVersion": "1.1" }`
- Sent once at connect; the full project is the baseline. OpenUtau is the sole owner of the project, so the baseline only ever travels outward and is never echoed back.

#### `updateUstx` (notification)

- `{ "ustx": "<full USTX project document>" }` — debounced (§7). Whole USTX resent per change in v1.

> **The `ustx` field is OpenUtau's native USTX document, which is YAML** — byte-identical to what `Ustx.Save` writes to a `.ustx` file, so a plugin can persist it or re-parse it with any YAML reader. It is *not* a JSON projection of `UProject`: 64 members are `[YamlIgnore]`, so a JSON serialization would be a different, lossy document.

#### `updatePartLayout` (request)

```json
{
  "parts": [
    { "trackNo": 0, "startMs": 1200.0, "endMs": 8400.0, "audioHash": "13507256038857166760" }
  ]
}
```

- `audioHash` = XXH64 of that part's rendered audio. Plugin dedupes against audio it already holds (in memory or in its cache). 64-bit is deliberate: collision risk is negligible, and the receiver additionally SHOULD verify the frame `length` as a cheap integrity cross-check.
- Response `data`: `{ "missingAudios": ["13507256038857166760", ...] }` (decimal strings).

#### `updateTracks` (notification)

```json
{ "tracks": [ { "name": "Singer 1", "volume": 0.0, "pan": 0.0, "muted": false } ] }
```

- `volume`/`pan` are passed through in OpenUtau's internal scale, unconverted: `UTrack.Volume` is **decibels** (`0` = unity) and `UTrack.Pan` is **-100..+100** (`0` = centre).
- `muted` is the **effective** mute: `UTrack.Muted`, which already has solo resolved against the rest of the project.
- These fields remain on the wire for compatibility and for peers that need the OpenUtau mixer state. The bridge's default output is **pre-fader**: it does not apply `volume`, `pan`, or `muted`. The DAW owns gain, pan, mute and solo so the dry signal entering its effects chain stays stable while the OpenUtau performance is edited.
- Pre-fader output is scaled by a **constant output trim of √0.5 (≈ 0.7071, −3.01 dB) per channel**. OpenUtau pans constant-power, so its own playback of a centred track puts cos(π/4) on each channel; a bridge that bypassed panning without this trim would sit a systematic 3 dB above the level the performance was tuned against. The trim is not mixer state: it never follows `volume`, `pan` or `muted`.
- A bridge may therefore receive a muted track and still request/render its part audio. `muted` is not a request to omit audio from the data plane.

#### `updateProjectInfo` (notification) — v1.1

```json
{ "name": "my song", "saved": true }
```

- What a plugin's info window shows about the project. `name` is the project file's stem; an unsaved project reports `saved` false and an empty `name`.
- Part of the fast debounced stream (§7): pushed once per full sync and whenever the project is saved or renamed. A plugin may ignore it — it carries no state the mixer or renderer needs.

### 6.2 Plugin → OpenUtau

#### `getAudio` (request) — audio pull

- `request:<uuid>:getAudio`, payload `{ "hash": "13507256038857166760" }` (decimal string, see §5.2)
- **Response is a data-plane frame**, not a JSON envelope:

```
audio 13507256038857166760 3528000\n<3528000 raw bytes>
```

- Payload encoding (fixed, engine-bound): **raw `float32` PCM, 44.1 kHz, stereo, interleaved, little-endian**, no compression, no base64. Mono mixes are upmixed to stereo.
- Ordering: plugin SHOULD pull missing hashes sequentially; one outstanding `getAudio` per connection in v1.
- The plugin is the requester (§14.2): pulling rather than being pushed to is what gives it backpressure.

#### `ping` (notification)

`{}` every 5 s.

#### `playbackStarted` (notification)

`{}` on DAW transport play rising edge; OpenUtau flushes pending debounced updates (§7).

#### `playhead` (notification) — v1.1

```json
{ "positionMs": 12500.0, "playing": false }
```

- The DAW's transport position, **one-way towards OpenUtau**: the received position simply overwrites OpenUtau's playhead, converted to ticks on the project's own time axis. The reverse direction does not exist — OpenUtau never reports its own position, and there is no reply or acknowledgement.
- `positionMs` is absolute milliseconds on the shared timeline — the same coordinate `updatePartLayout`'s `startMs` uses. Moves smaller than **5 ticks** at the destination are ignored as jitter.
- Throttling is the sender's choice. The reference bridge reports **state changes immediately**, **every 100 ms while playing**, and **only when a parked playhead moves more than 50 ms**. Receivers must tolerate any pacing.

#### `bpm` (notification) — v1.1

```json
{ "bpm": 137.5 }
```

- The DAW project's tempo, sent when it changes. Without ARA there is no tempo-map sync (§1): OpenUtau uses this only as a guard — warning the user once per distinct mismatch, with a **±0.5 BPM** tolerance, that bars will misalign — never to retempo-map the project.

### 6.3 MIDI input (reserved — not in v1)

Reserved for a future message family (e.g. `notification:midiNotes`, `request:recordMidi`). The PR #2187 plugin received `MidiEvent*` in `run()` but never relayed it; v1 does not define this direction.

---

## 7. Sync Semantics & Debouncing

- OpenUtau subscribes to `DocManager` command stream; reacts to render/volume/pan changes while connected.
- Debounce: `updateUstx` + `updateTracks` + `updateProjectInfo` (v1.1) = **1 s**; `updatePartLayout`/audio = **5 s**.
- **Playback flush:** on `playbackStarted`, both debounce queues flush before playback begins.
- **Full sync:** after every (re)connect: `updateUstx` → `updateTracks` → `updateProjectInfo` (v1.1) → `updatePartLayout`(+ audio pull), serialized by a semaphore; one update in flight per connection.

## 8. Error Handling

- Control request timeout → treat connection as dead (disconnect + reconnect).
- Malformed control line → warning log, keep connection.
- **Data-frame violation** (stream ends before `length` bytes consumed) → protocol error → disconnect.
- Non-user-initiated disconnect → reconnect with backoff `500 ms / 1 s / 2 s`, then notify user.
- User-initiated disconnect → optional final update, then raw `close`.

## 9. Connection Lifecycle

```
plugin binds :port, writes discovery file (with apiVersion)
        │
OpenUtau scans discovery dir → probe port → TCP connect
        │
request:init (5 s) → full USTX baseline + version check
        │
steady state: debounced updates; audio pulled by hash on demand
        │
DAW plays → notification:playbackStarted → flush pending
        │
disconnect (error) → backoff ×3 → re-init + full sync
disconnect (user)   → final update → "close" → teardown
```

## 10. Compatibility & Versioning Policy

- **Append-only.** New minors add messages/fields/kinds; never change the meaning of existing fields.
- **Kind namespaces.** Semantically changed kinds get suffixed, e.g. `updatePartLayoutV2`.
- **Unknown fields** ignored by receivers.
- `apiVersion` in discovery file + `init` response; major mismatch refuses connection.

## 11. Security & Trust Model

- Loopback only; dynamic port; no authentication. Any local process of the same user can read the project + audio. Matches the ACE Bridge model; accepted for v1.
- Mitigations: random high port, per-user discovery dir, no cross-machine path.
- Caveat: do not run OpenUtau in multi-user shared sessions.

## 12. Reference Implementations

| Side | Location | Notes |
|---|---|---|
| Server (plugin) | separate repo | New implementation. **Do not carry over PR #2187's `DawPlugin/` code**; reuse only the mechanisms in this doc |
| Client (API) | `OpenUtau.Core/DawIntegration/` (this repo) | `DawMessages.cs`, `DawTransport.cs`, `DawServerFinder.cs`, `DawAudio.cs`, `DawManager.cs` — new code |

## 13. Test Strategy

- **Main repo (unit):** framing (control + data plane), request/response/timeout, heartbeat, debounce flush — `OpenUtau.Test/Core/DawIntegration/`.
- **Main repo (conformance):** `DawTestPlugin` is a loopback TCP server that plays the plugin half, built on the shipping `DawTransport` so the framing is only implemented once; `DawConformanceTest` drives `init → updateTracks → updateProjectInfo → updatePartLayout → getAudio → playbackStarted` (and the v1.1 `playhead`/`bpm` notifications) against the real `DawManager` through a real discovery directory.
- **Plugin repo (conformance):** an independent test client that replays recorded transcripts (including binary frames) against the plugin and asserts responses — the only way to keep both sides honest once code is fully separated.

## 14. Open Questions (before v1 freeze)

1. **`apiVersion`** — field in discovery file + echoed in `init` (recommended) or implicit?
2. **Audio pull model** — `getAudio` request/response (recommended, gives the plugin backpressure) vs. OpenUtau pushing `audio` frames unprompted.
3. **Audio cache lifetime** — plugin caches decoded audio by hash; v1 keeps cache ownership entirely on the plugin side. OK?
4. **MIDI direction** — explicitly out of v1 (§6.3). Confirm scope.
5. **Discovery file naming** — recommend `<plugin-name>-<instance>.json` to avoid collisions.
6. **Multi-client** — one active socket per plugin instance in v1.
7. **Tempo sync** — likely v1.1 (`notification:tempoMap`); leave wire space.

Resolved decisions (recorded for the record):

- **Hash width** — RESOLVED: XXH64 (up from the PR's XXH32), serialized as decimal strings everywhere (§5.2). Receiver verifies frame `length` as a cheap integrity cross-check.
- **`apiVersion` (14.1)** — RESOLVED: carried in the discovery file *and* echoed in the `init` response, so a plugin that advertises one version and speaks another is still caught (§4, §6.1).
- **Audio pull model (14.2)** — RESOLVED: `getAudio` request/response, listed under §6.2 because the plugin is the requester.
- **`init` direction** — RESOLVED: OpenUtau pushes the USTX baseline in the `init` *request*; the response carries only `apiVersion`. OpenUtau owns the project, so the baseline is one-way (§6.1).

## 15. Why PR #2187 is discarded

The PR is valuable as a prototype but cannot be the base of the API-first split:

1. **Audio-in-JSON is not viable.** Its `updateAudio` shipped `base64(gzip(float32 PCM))` inside a JSON line: +33 % base64 overhead, poor gzip ratio on PCM, and the whole message assembled in memory (multi-MB strings, repeated copies). A 10 s part is ~3.5 MB raw → ~4.7 MB base64 before gzip even helps.
2. **Single-line framing forces full buffering.** No streaming path for large payloads; receiver must materialize the entire message.
3. **Cross-repo footprint.** 84 commits / +3409 lines spanned the main repo plus an embedded DPF plugin with its own CI matrix and submodules — exactly what the API-first split wants to avoid.
4. **Untestable seam.** With the plugin sharing no code, a text-transcript conformance harness (this doc's §13) becomes the only honest contract test; the PR had only client-side unit tests.

What is kept from the PR: discovery-file pattern with port probing, XXH64 hash dedup + `missingAudios` pull, heartbeat + reconnect backoff, `playbackStarted` flush semantics, the part-layout sync model. (The PR used XXH32; v1 upgrades to XXH64.)

---

*Draft distilled from reviewing openutau/OpenUtau PR #2187 (`add/vst-integration` @ `455d7f9c`). Line format and audio path are a fresh design; fixed constants (44.1 kHz stereo float32) reflect OpenUtau engine limits, verified in `PlaybackManager.cs`.*
