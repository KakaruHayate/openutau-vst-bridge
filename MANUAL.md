# OpenUtau Bridge User Manual

**Version 1.0.0 · For OpenUtau builds with DAW Integration (protocol v1.2, compatible with v1.1 hosts)**

This manual covers **day-to-day use**: installation, connecting, the everyday workflow,
where each feature lives, and what to check when something goes wrong. For what changed
in this release, see the release notes (`RELEASE.md`).

---

## 1. How it works

OpenUtau Bridge is an **instrument plugin** (VST3 / CLAP) inside your DAW. It makes no
sound of its own. Instead:

1. On startup it opens a local port and **waits for OpenUtau to connect**;
2. OpenUtau finds it through Tools → DAW Integration and connects;
3. From then on, each part you finish rendering in OpenUtau is streamed into the plugin
   and placed on the DAW timeline **at its absolute OpenUtau position**;
4. Keep editing in OpenUtau, and the DAW follows.

The right mental model: **OpenUtau is the editor and render engine; the DAW is the mixing
console.** Volume, pan, mute and effects all happen on the DAW side; OpenUtau's own faders
do not touch the signal delivered to the DAW.

## 2. Installation

**Requirements**: Windows 10/11 64-bit, macOS 11+, or a mainstream Linux distribution;
a DAW that can load VST3 or CLAP **instruments** (REAPER, Cubase, Studio One, FL Studio,
Bitwig, Ableton Live, Waveform, Cakewalk, etc.). Hosts that only take effects — Audition,
Audacity and the like — **will not work**.

You need two things, and they **must come as a pair**:

1. **OpenUtau** (the distribution with DAW Integration) — unzip it anywhere; it does not
   interfere with your existing OpenUtau installation;
2. **The plugin** — copy per the table below:

| Format | Windows | macOS | Linux |
|---|---|---|---|
| VST3 | `C:\Program Files\Common Files\VST3\` (copy the **whole folder**) | `~/Library/Audio/Plug-Ins/VST3/` | `~/.vst3/` |
| CLAP | `C:\Program Files\Common Files\CLAP\` | `~/Library/Audio/Plug-Ins/CLAP/` | `~/.clap/` |

> Without administrator rights, Windows also accepts `%LOCALAPPDATA%\Programs\Common\VST3\`
> and `...\CLAP\`, with the same effect.
>
> A `.vst3` is a **folder** — copy the whole thing, not just the file inside it.

After installing, **rescan plugins** in your DAW. If Gatekeeper blocks the plugin on macOS
the first time, right-click → Open, or allow it in System Settings.

## 3. First connection

**Order matters: DAW first, OpenUtau second.** The plugin must be running before
OpenUtau can find it.

1. Open your DAW and create a new project;
2. Create an **instrument track** (not an audio track) and insert **OpenUtau Bridge**
   (vendor `OpenUTAU`) as the instrument;
3. Start the matching OpenUtau build;
4. Open the **Tools → DAW Integration...** dialog;
5. Your plugin instance should appear in the list (`Status` = `Compatible`). Select it →
   **Connect**;
6. The status changes to `Connected to ...` — you are done.

**The OpenUtau project must have been saved at least once.** Connecting an unsaved
project returns a "save the project first" message — this is deliberate: an unsaved
project has no reliable audio path, so nothing it delivers can be guaranteed.

> The dialog is currently English-only (OpenUtau's UI translation goes through Crowdin;
> this part has not landed yet). For reference:
>
> | UI text | Meaning |
> |---|---|
> | Plugin / Port / API / Status | plugin / port / protocol version / state |
> | Compatible / Incompatible | version matches, may connect / version mismatch, refused |
> | Refresh / Connect / Disconnect | rescan / connect / disconnect |
> | No DAW plugin found... | no plugin seen; load it in the DAW first, then refresh |
> | Connected to ... / Connected (n) | connected (to an instance) / n instances connected |
> | Connecting / Reconnecting / Connection lost: ... | connecting / reconnecting / dropped |

## 4. Everyday use

### Writing → hearing

1. Set up tracks, pick singers and write notes in OpenUtau as usual;
2. **Wait for the render to finish** (the waveform appears). Parts that have not finished
   rendering are silent at their DAW positions for the moment — that is normal;
3. Press play in the DAW. The sound appears **at the same time position as in OpenUtau** —
   a note written at second 10 sounds at the DAW's second 10.

### Mixing

- Use the DAW's faders, pan, mute and effects directly.
- OpenUtau's own volume/pan/mute **do not affect** the delivered signal (pre-fader by
  design): whatever you change in OpenUtau, the level entering the DAW's effect chain
  stays stable.
- For reference: the bridged track is about 3 dB louder in the DAW than older versions —
  that is the unified output reference, not a bug.

### Track switching and multiple instances

- Each instance has an **`OpenUtau Track` parameter** (shown in the DAW's automation panel
  as Track 1, Track 2, …) that selects which OpenUtau track the instance answers for
  (counting from 1).
- **Switch tracks right in the plugin window**: double-click the plugin, open the dropdown
  at the top, pick a track (each entry shows "number: track name"). It takes effect
  immediately, and the change is also recorded by the DAW's automation.
- You can keep using the `OpenUtau Track` parameter in the DAW's generic parameter panel
  instead; both sides stay in sync at all times. The parameter supports automation and is
  saved with the DAW project.
- **To use several OpenUtau tracks in the DAW**: put one instance per DAW track and give
  each a different track. Then connect them **one by one** in OpenUtau's DAW Integration
  list (one row per instance).

### The plugin window

Double-click the plugin to open it (native on Windows and macOS; Linux has no plugin
window yet — switch tracks from the DAW's generic parameter panel). It shows:

- **Track dropdown** (top): which OpenUtau track this instance plays (names only; singer
  and engine are on the info rows below);
- **Connection state and port**;
- **Project**: project name; `(unsaved)` until saved. Updates automatically when you save
  in OpenUtau;
- **Singer row**: the selected track's singer; **Engine row**: that track's render engine
  (e.g. DIFFSINGER). Both update automatically on track or singer changes;
- **Tempo**: the current DAW project tempo;
- **Transport**: playing / stopped.

The dropdown is the only control in the window; writing, tuning and rendering stay in
OpenUtau entirely. The window uses a fixed dark color scheme to match typical DAW themes.

### Playhead sync

While the DAW plays, **the playhead in the OpenUtau window follows the DAW** (converted to
OpenUtau's own timeline). Small scrubs of the DAW playhead while stopped also follow.
The direction is one-way: OpenUtau never drives the DAW. Scrubbing OpenUtau's playhead
during playback is meaningless — it gets overwritten.

### Tempo (BPM)

The two projects align **by absolute time (seconds)**. When the DAW's BPM differs from the
OpenUtau project's, OpenUtau raises a notice once per kind of mismatch; aligning bars
means making both sides use the same BPM. Playhead sync runs on time and is unaffected by
BPM.

### Export / Bounce

Just use the DAW's export. In offline rendering the plugin waits for audio that has not
been delivered yet, so the export matches real-time playback. Extremely long projects may
export a little slower than a purely local render — audio is streamed while it plays.

### Saving and restoring

The DAW project remembers each instance's `OpenUtau Track` setting but **not** the
OpenUtau project content. After reopening a DAW project: open OpenUtau, open the matching
project, and **Connect** once more in DAW Integration.

## 5. When something goes wrong

**Log location** (always attach when reporting):

- Windows: `Win + R` → `%TEMP%\OpenUtau` → `bridge-<number>.log`
- macOS / Linux: `/tmp/OpenUtau/bridge-<number>.log`

**Quick troubleshooting**:

| Symptom | Check first |
|---|---|
| The DAW Integration list is empty | Order reversed? (DAW first, OpenUtau second); did the plugin really load; did you copy only the file inside the `.vst3` instead of the folder; press Refresh |
| List stays empty | Open `%TEMP%\OpenUtau\PluginServers` (macOS / Linux: `/tmp/OpenUtau/PluginServers`); with the plugin loaded there should be a `.json` file. None? The plugin side never started — check `bridge-*.log` |
| Listed but will not connect | Firewall / antivirus blocking 127.0.0.1 loopback (whitelist it); has the OpenUtau project been saved |
| Connected but silent | Has the render finished in OpenUtau; is the DAW track muted / at zero volume; does the track pointed to by `OpenUtau Track` have notes; is the playhead where you expect |
| Sound at the wrong position | Replay once after changing the DAW project's sample rate; rule out the DAW's own "project start" settings |
| Export is silent | Make sure the plugin is 1.0.0; play through once in real time before exporting so audio gets delivered |
| Disconnects after a while | Report with the logs and the DAW's buffer settings |

Attaching the log files above makes issues much easier to diagnose.

## 6. Explicitly not supported

- MIDI input (playing or drawing MIDI in the DAW does nothing);
- Bar-level (tempo map) sync;
- Embedding the OpenUtau UI into the DAW window;
- Connections across machines (localhost 127.0.0.1 only);
- Pro Tools (AAX requires Avid's licensed SDK).

---

*Companion documents: `RELEASE.md` (release notes, Chinese `RELEASE.zh-CN.md`),
`PROTOCOL.md` (the wire protocol). Chinese manual: `MANUAL.zh-CN.md`.*
