# OpenUtau Bridge Release Notes

**Version 1.1.1 · Protocol v1.2 · 2026-11-25**

1.1.1 is a host-integration fix for the floating editor (PR #19, by tcpassos). Closing the
editor with its own close box hid the window but never told the host, so a DAW that tracks
whether the editor is open kept its toggle on. It then took two clicks to bring the window
back: the first turned the stale toggle off.

The plugin now reports `clap_host_gui::closed(host, true)`. The host acknowledges it with
`clap_plugin_gui::destroy()`, which in this bridge only hides the window and keeps it
reusable, so the next `create()` / `show()` brings the same editor back.

- **The report is only for the user's close.** The Win32 `WM_CLOSE`, the Cocoa
  `NSWindowWillCloseNotification` and the X11 `WM_DELETE_WINDOW` paths send it; a host's own
  `hide()` or `destroy()` does not.
- **The close cannot race an embed on X11.** Reparenting now takes the same lock as the close
  path, so a queued close cannot hide an editor the host has just embedded.

The wire format is unchanged (protocol v1.2), so 1.1.1 pairs with every OpenUtau build that
speaks v1.1 or later. The macOS and Linux bundles are still unsigned; the re-signing and
Flatpak notes in `README.md` apply to this release as they did to 1.1.0.

---

## 1.1.0 · 2026-11-17

1.1.0 gives Linux the plugin window Windows and macOS already had (PR #15, by linux-leo),
and the work that mattered turned out to be the crash the window brought with it.

Loading a second instance — which is every plug-in scanner, and any host keeping more than
one — used to end in `X Error of failed request: BadLength`. Each instance opened its own
X11 display during `PluginInit`, before any host had asked for a window, and closed it when
the instance died. Closing ran libXft's teardown after libXrender's, so libXrender put the
dying `Display` back into its global display list holding a freed `XExtCodes`; the next
`XOpenDisplay` to reuse that heap address sent a malformed request, and the host or scanner
died with it.

So the connection is now opened once, lazily, and shared:

- **Nothing X11 happens before a window is asked for.** Scanning a plugin, hosting it without
  an editor, or simply having it loaded opens no display, no fonts and no event thread. The
  window is built on the first `clap_plugin_gui::create()`.
- **One display and one event thread per process.** The thread stops when the last window goes
  away; the display is not reopened, so nothing re-enters libXrender's list.
- **One dropdown at a time, across instances.** Opening it in a second instance closes the
  first, and a pointer grab that cannot be taken closes the list rather than leaving it stuck
  on screen.
- **The 4 Hz repaint runs on a deadline**, not on an idle connection, so a wheel spinning over
  one instance's track list no longer holds back another instance's info rows — and the host
  notification a track pick sends (`request_flush`) now runs outside the gui's lock, so a host
  that answers it synchronously cannot deadlock the event thread.

**The Linux window is the newest and least proven backend.** It is plain Xlib with Xft text:
no toolkit, and no fallback to one. It is compiled in when X11 and Xft development files are
found at configure time, as they are in the release builds; a build or a Wayland-only session
without them still loads and still works, just without a window, and the track stays
switchable from the host's own parameter panel. Wayland sessions reach the window through
XWayland. Treat it as a feature with rough edges — reports that name the host are what will
settle them.

The wire format is unchanged (protocol v1.2), so 1.1.0 pairs with every OpenUtau build that
speaks v1.1 or later. The macOS and Linux bundles are still unsigned; the re-signing and
Flatpak notes now in `README.md` apply to this release as they did to 1.0.3.

---

## 1.0.3 · 2026-11-07

1.0.3 is a fix for one project shape that could take OpenUtau down: a DAW project carrying a
**tempo map**, especially a gradual tempo change. The host reports a new tempo every block
while the playhead walks it, the plugin relayed every one of those, and OpenUtau answers each
`bpm` it disagrees with by opening a **modal dialog** — so a single ramp produced hundreds of
error windows, gigabytes of memory and a process that had to be ended from Task Manager
(2026-11-07 field report; the code path is `Session::PumpTempo` on this side and
`DawManager.HandleBpm` on OpenUtau's).

Only *when* the tempo is reported changed. The wire format, the message and OpenUtau's side are
untouched, so 1.0.3 works with every OpenUtau build that speaks v1.1 or later:

- **Only the base tempo is reported** — the tempo the host reports within 100 ms of the start of
  its timeline, which is the only value OpenUtau's guard compares against its project's first
  tempo. The tempo under a playhead walking a tempo map is a different quantity, and it is no
  longer sent at all.
- **A value has to settle** before it goes out (one second of stillness), so dragging the tempo
  control is one notification instead of one per poll.
- **At most one notification per 5 s**, whatever the host reports. A receiver that shows each of
  these as a dialog must never be handed a stream of them.
- **The comparison tolerance is OpenUtau's own ±0.5 BPM** (it was 0.01), so a change OpenUtau
  cannot see no longer costs traffic — nor counts as "a change" on every block of a ramp.
- **The version a host shows is the release version.** The plug-in's descriptor carried a literal
  `0.2.0` that never followed the releases — 1.0.0 through 1.0.2 all announced themselves as
  0.2.0 in a host's plug-in list. It now comes from the same number the release is tagged with.

**Nothing about alignment changed.** The two sides still align by absolute time, and a DAW tempo
map that matches OpenUtau's still lines up exactly: the audio is rendered through that tempo map
before it is sent. What is gone is the false warning a *matching* tempo map used to raise on
every step, and the avalanche that came with it.

Two cases are on record as deliberate: a tempo map that does *not* match OpenUtau's is still
not reported (a single value cannot describe a map — bars drift and seconds stay aligned), and
a mismatch introduced anywhere other than the timeline start is reported only when the playhead
is back at the start.

1.0.3 was re-cut the day it shipped. The first asset set announced itself as 0.2.0 to every host;
the tag was moved and the archives replaced rather than opening a 1.0.4 for a version string.
Downloading `plugins-*-latest.zip` from this release gets the corrected build either way.

---

## 1.0.2 · 2026-10-05

One community fix (#12): the VST3 module loads in Ableton Live again. No protocol or behaviour
change.

---

## 1.0.1 · 2026-10-02

1.0.1 is 1.0.0 with the plug-in icon. The VST3 and CLAP modules now carry the same
puzzle-piece artwork OpenUTAU-FX uses, on Windows and macOS, so the two plug-ins read as
one family in a host's plug-in browser. Nothing else changed: the protocol, the audio path
and every behaviour are 1.0.0's, and its notes below still describe what ships.

---

## 1.0.0 · 2026-09-07

The first stable release of OpenUtau Bridge. Written for people using it directly; for
installation and everyday use see the User Manual (`MANUAL.md`, Chinese
`MANUAL.zh-CN.md`). Protocol-level details live in `PROTOCOL.md`.

---

## What this is

A plugin that puts OpenUtau's rendered audio straight onto the DAW timeline. You **edit
in OpenUtau** (notes, singers, rendering), and the sound appears in real time on the
**instrument track in your DAW**, at the same position as in OpenUtau — no exporting and
dragging wav files.

It is **not** the "OpenUtau inside the DAW window" kind of integration (the Synthesizer V
/ ARA approach). OpenUtau stays its own window; the plugin in the DAW only does
"connect, receive audio, place it at the right position". So there is no editor UI — the
plugin ships with a read-only **info window** and a **track-switch dropdown** (Windows,
macOS, Linux); double-click the plugin to open it.

## Package contents

Every platform ships the same set:

| Platform | Files |
|---|---|
| Windows 64-bit | `OpenUtau Bridge.vst3` (folder) + `OpenUtau Bridge.clap` (file) |
| macOS (Apple silicon / Intel) | Both formats as above |
| Linux 64-bit | Both formats as above |

> **Must be paired with an OpenUtau build that has DAW Integration** (the
> `OpenUtau-win-x64.zip` etc. distribution packages for each platform). The plugin speaks
> protocol v1.2 and stays compatible with v1.1 OpenUtau hosts; OpenUtau builds without
> DAW Integration cannot connect.

## Core features

- **Real-time sync**: edits in OpenUtau (notes, lyrics, singers, rendering) update the
  DAW automatically.
- **Multi-instance, multi-track**: one instance per DAW track; the `OpenUtau Track`
  parameter (or the dropdown in the plugin window) selects which OpenUtau track each
  instance answers for, with DAW automation support.
- **Plugin window** (Windows / macOS / Linux): track-switch dropdown, connection state and
  port, project name and saved state, the selected track's singer and render engine, tempo,
  and transport. Fixed dark color scheme.
- **Playhead sync**: the DAW's playhead drives OpenUtau one-way — transport changes sync
  immediately, playback position about every 100 ms, and stopped scrubs sync past a
  50 ms threshold.
- **BPM mismatch notice**: when the two projects' tempos differ by more than 0.5 BPM,
  OpenUtau raises one notice per kind of mismatch.
- **Pre-fader output**: OpenUtau's volume/pan/mute do not touch the bridged signal —
  mixing belongs entirely to the DAW.
- **Export / bounce support**: offline rendering waits for missing audio, so exports
  match real-time playback.
- **Sample-rate adaptation**: the wire runs at a fixed 44.1 kHz; the plugin converts to
  the DAW's rate on arrival.

## Changes since 0.2.0 Alpha

- **The plugin window now covers macOS** (native AppKit implementation with the same
  info rows and track dropdown as Windows); Linux has no plugin window yet — switch
  tracks from the DAW's generic parameter panel.
- **Window reliability**: the window survives host UI rebuilds / plugin-window teardown;
  the track dropdown no longer fights automation or user interaction.
- **Split info rows**: singer and render engine each get their own row (the protocol
  v1.2 `singer` / `engine` fields), updating automatically on track or singer changes.
- **Protocol upgraded to v1.2**: adds `singer` / `engine` metadata fields; the major
  version is unchanged, so v1.1 OpenUtau hosts degrade gracefully and unknown messages
  are safely ignored.

## Known issues and limitations

- **The Linux plugin window is the newest backend.** It needs a session with X11 available
  (XWayland counts); a Linux build without X11/Xft at configure time, or a host that offers no
  X11 parent, loads without a window and the track is still switchable from the host's own
  parameter panel.
- **OpenUtau's playhead is driven by the DAW.** Scrubbing OpenUtau's playhead during
  playback is instantly overwritten — expected behavior.
- **The two sides align by seconds, not by bars.** A BPM mismatch produces a notice only;
  there is no bar-level conversion.
- **Unsaved OpenUtau projects cannot connect.** You get a "save the project first"
  message — deliberate protection (an unsaved project has no reliable audio path).
- **DAW projects do not store OpenUtau content.** After reopening a DAW project,
  reconnect OpenUtau once.
- **No MIDI input.** Notes are written in OpenUtau only.
- **The macOS bundles are unsigned.** Apple silicon and the sandboxed hosts (Logic Pro,
  GarageBand) refuse to load them, and Cubase/Nuendo report an invalid signature; signing
  the installed copy locally works around it — see
  [#8](https://github.com/KakaruHayate/openutau-vst-bridge/issues/8).
- **A Flatpak host keeps its own private `/tmp`.** Bitwig (and any other Flathub build)
  advertises the plugin into a directory OpenUtau cannot see, so the DAW Integration list
  stays empty; grant `/tmp` or share `TMPDIR` — see
  [#16](https://github.com/KakaruHayate/openutau-vst-bridge/issues/16).

## Quality verification

- The plugin's own test suite passes 127/127 (protocol, concurrency, mixing, full
  resample chain, over real sockets).
- GitHub CI on three platforms (Windows x64 / macOS arm64 / Linux x64): build + tests +
  Steinberg VST3 validator, all green.

## Feedback

Open an issue at the repository's
[issue tracker](https://github.com/KakaruHayate/openutau-vst-bridge/issues), **always
including**: the log files (Windows: `Win + R` → `%TEMP%\OpenUtau` → `bridge-<number>.log`;
macOS / Linux: `/tmp/OpenUtau/`), DAW name and version, OS version, VST3 or CLAP, and
reproduction steps.

## License

The plugin is MPL-2.0; dependencies are all permissively licensed (see `README.md`).
The paired OpenUtau builds follow their own license.
