# OpenUtau Bridge Release Notes

**Version 1.0.0 · Protocol v1.2 · 2026-09-07**

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
plugin ships with a read-only **info window** and a **track-switch dropdown** (Windows /
macOS); double-click the plugin to open it.

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
- **Plugin window** (Windows / macOS): track-switch dropdown, connection state and port,
  project name and saved state, the selected track's singer and render engine, tempo, and
  transport. Fixed dark color scheme.
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

- **The plugin window is Windows / macOS only.** On Linux, double-clicking the plugin
  opens nothing for now; functionality is unaffected.
- **OpenUtau's playhead is driven by the DAW.** Scrubbing OpenUtau's playhead during
  playback is instantly overwritten — expected behavior.
- **The two sides align by seconds, not by bars.** A BPM mismatch produces a notice only;
  there is no bar-level conversion.
- **Unsaved OpenUtau projects cannot connect.** You get a "save the project first"
  message — deliberate protection (an unsaved project has no reliable audio path).
- **DAW projects do not store OpenUtau content.** After reopening a DAW project,
  reconnect OpenUtau once.
- **No MIDI input.** Notes are written in OpenUtau only.
- Logic Pro / GarageBand on macOS most likely will not scan the plugin (sandboxing).
- **The macOS plugin is ad-hoc signed** (no paid developer certificate): on Apple silicon,
  if your DAW (e.g. Cubase 15) reports "The VST signature is invalid", clear the
  quarantine flag and re-sign locally with the two commands in the User Manual §2;
  Logic/GarageBand will likely refuse even after re-signing, due to sandboxing. A fully
  transparent distribution would additionally need Developer ID signing + notarization
  (Apple Developer Program).

## Quality verification

- The plugin's own test suite passes 125/125 (protocol, concurrency, mixing, full
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
