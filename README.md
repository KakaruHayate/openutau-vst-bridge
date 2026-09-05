# openutau-vst-bridge

[![CI](https://github.com/KakaruHayate/openutau-vst-bridge/actions/workflows/ci.yml/badge.svg)](https://github.com/KakaruHayate/openutau-vst-bridge/actions/workflows/ci.yml)

A thin plugin that puts OpenUtau's rendered audio on the DAW timeline. Editing stays in
the OpenUtau window; the plugin holds the connection, the audio, and the placement.

This is the plugin half of OpenUtau's DAW integration. The other half is the API under
`OpenUtau.Core/DawIntegration/` in the main repository. [`PROTOCOL.md`](PROTOCOL.md) is
the contract between them and is a verbatim copy of the file that ships there — **change
one and you must change the other.**

## What it does

- **Project sync (v1.0).** Receives the USTX baseline and every debounced update, pulls
  part audio by hash over the data plane, and places each part on the DAW timeline at its
  absolute position — converted to the host's sample rate on arrival.
- **Multi-instance, multi-track (v1.0).** One plugin instance per DAW track; a stepped
  "OpenUtau Track" parameter chooses which track the instance answers for. All instances
  share one OpenUtau connection per instance; each advertises itself through its own
  discovery file.
- **Pre-fader output (v1.0).** OpenUtau's volume/pan/mute fields stay on the wire but are
  not applied: the DAW owns gain, pan, mute and solo, with a constant √0.5 trim so the
  level matches OpenUtau's own centred-track playback.
- **Bounce-friendly offline rendering (v1.0).** An offline (non-realtime) render waits
  for missing part audio instead of printing holes; real-time playback never waits.
- **Info window (v1.1).** A small window showing project name, saved state, connection
  state, tempo, transport and the track list with this instance's track marked. Windows
  for now; other platforms simply advertise no gui.
- **Playhead sync (v1.1).** The DAW's transport position is sent one-way to OpenUtau —
  immediately on state changes, every 100 ms while playing, and on scrubs of more than
  50 ms while parked. OpenUtau's own playhead follows the DAW, never the reverse.
- **Tempo guard (v1.1).** The DAW's tempo is reported when it changes; OpenUtau warns
  once per distinct mismatch that bars will misalign (there is no tempo-map sync).

## Formats

Built clap-first: one implementation, wrapped into each format by
[clap-wrapper](https://github.com/free-audio/clap-wrapper).

| Format | Reaches |
| --- | --- |
| CLAP | Bitwig, Reaper, FL Studio, Studio One, Qtractor |
| VST3 | Ableton Live, Cubase/Nuendo, and the rest |

Pro Tools is out of scope: AAX requires an SDK available only under an Avid agreement.

## Licensing

This project is MPL-2.0. MPL-2.0 is file-level copyleft, so the permissively licensed
dependencies below keep their own terms and notices while everything under `src/` stays
MPL-2.0.

| Dependency | License | Pinned as |
| --- | --- | --- |
| [clap](https://github.com/free-audio/clap) | MIT | submodule, `1.2.10` |
| [clap-wrapper](https://github.com/free-audio/clap-wrapper) | MIT | submodule, `v0.16.0` |
| [VST3 SDK](https://github.com/steinbergmedia/vst3sdk) | MIT | fetched by clap-wrapper, `v3.8.0_build_66` |
| [r8brain-free-src](https://github.com/avaneev/r8brain-free-src) | MIT | submodule, `version-6.5` |
| [xxHash](https://github.com/Cyan4973/xxHash) | BSD-2-Clause | submodule, `v0.8.3` |
| [nlohmann/json](https://github.com/nlohmann/json) | MIT | vendored header, `libs/json/VERSION.txt` |

Three things worth knowing:

- **The VST3 SDK became MIT in 3.8.0.** GPLv3 and the Steinberg proprietary agreement are
  no longer offered, so shipping a permissively licensed VST3 needs no dispensation. This
  is why no clean-room VST3 implementation is used here.
- Only xxHash's *library* is BSD-2-Clause; the `xxhsum` command-line tool is GPLv2. This
  build uses the library alone.
- "VST" is still a Steinberg **trademark**, which is separate from the MIT code. It
  constrains use of the name and logo in branding, not redistribution of the binary.

JUCE was considered and rejected: it is AGPLv3 or commercial, and neither can ship inside
an MPL-2.0 project.

## Building

Needs CMake 3.28+ and a C++20 compiler. On Windows that means MSVC; MinGW is untested.

```sh
git clone --recurse-submodules https://github.com/KakaruHayate/openutau-vst-bridge.git
cd openutau-vst-bridge
cmake -B build
cmake --build build --config Release --target openutau-vst-bridge_all
```

Artifacts land in `build/assets/`. The first configure clones the VST3 SDK, so it needs
network access.

On Windows, `bridge-install` copies both formats into the per-user plug-in folders under
`%LOCALAPPDATA%\Programs\Common`, which every host scans and which needs no administrator
rights:

```sh
cmake --build build --config Release --target bridge-install
```

The VST3 is built as a bundle folder (`OpenUtau Bridge.vst3/Contents/x86_64-win/`) rather than a
bare DLL. Both load in most hosts, but only the folder is what the current VST3 spec describes.

## Layout

```
src/
  bridge_entry.*    the three symbols a format module needs, and the exported clap_entry
  plugin.cpp        descriptor, factory, parameters, state, gui hookup, and the CLAP process callback
  gui.h gui_win32.cpp the info window: CLAP gui extension and its Win32 backend
  transport.h       where a block sits on the host's timeline
  session.*         the worker thread that owns the connection and publishes snapshots
  socket.* stream.h the listener and the byte streams over it
  reader.* frame.*  lines and data-plane frames off a stream
  messages.*        control-plane JSON, both directions
  connection.*      requests, responses, notifications, heartbeat
  discovery.*       the advertisement file OpenUtau scans for
  audio_store.*     hash to PCM, converted to the host's sample rate
  timeline.*        the snapshot the audio thread mixes, and the handover to it
  fader.h hash.*    OpenUtau's volume and pan law; XXH64 and its wire form
tools/
  bridge_host.cpp   runs a session outside a DAW, for checking against OpenUtau itself
libs/               dependencies, all permissively licensed
PROTOCOL.md         the wire contract, mirrored from the main repository
```

`src/` builds into a static library that knows nothing about formats;
`make_clapfirst_plugins` links it into one module per format.

## Testing

```sh
cmake --build build --config Release --target bridge-tests
./build/tests/Release/bridge-tests
```

The tests link that static library directly and open real loopback sockets, so the protocol is
exercised end to end — discovery file, framing, store, snapshot and mixer — without loading a
module into a host.

### Checking against OpenUtau itself

The suite here speaks to a fake OpenUtau and OpenUtau's own suite speaks to a fake plugin, which
leaves one thing neither can catch: the two disagreeing. `bridge-host` closes that by running the
real session outside a DAW.

```sh
cmake -B build -DBRIDGE_BUILD_HOST=ON
cmake --build build --config Release --target bridge-host
./build/Release/bridge-host --dir /tmp/bridge-live --rate 44100 --loop 4
```

Then, in the main repository, point the opt-in integration test at the same directory — it is
skipped when the variable is unset, so it costs an ordinary run nothing:

```sh
OPENUTAU_BRIDGE_DISCOVERY='C:\Users\you\AppData\Local\Temp\bridge-live' \
    dotnet test OpenUtau.Test/OpenUtau.Test.csproj --filter DawRealPluginTest
```

That drives the shipping `DawManager` — real discovery scan, real USTX serializer, real audio
extraction — against this plugin, and the whole flow is visible in `bridge-host`'s output:

```
info: OpenUtau connected on port 50512.
info: Project baseline received, 754 bytes.      <- init
t=  1s  peak L 0.25  R 0.25                  <- updatePartLayout, getAudio, mixed
t=  3s  peak L 0.0000  R 0.0000
info: Project baseline received, 754 bytes.      <- playbackStarted, flushed back as updateUstx
```

The level is worth checking rather than glancing at: the bridge sends the rendered OpenUtau part at
unity, and the DAW owns track gain, pan, mute and solo. A constant 0.25 part therefore reads 0.25
in each channel before the DAW mixer. This is deliberate: changing an OpenUtau editor fader must
not change the input level seen by a compressor or other DAW effect while the final mix is being
tuned. At a host rate other than 44100 expect a few percent more at the part's edges — that is the
resampler's overshoot on a rectangular window, not a gain error.

Without `--loop`, the transport never restarts, so `playbackStarted` is never sent and that half of
the flow goes unverified.

### Steinberg's validator

CI runs this on all three platforms on every push, so what follows is only needed to reproduce a
failure locally. clap-wrapper's own `vst3_validator` target shells out to a nested Ninja build, so
where Ninja is absent it is easier to configure the SDK directly:

```sh
cmake -S build/cpm/vst3sdk -B build/validator-vs -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
      -DSMTG_ADD_VSTGUI=OFF -DSMTG_ENABLE_VSTGUI_SUPPORT=OFF \
      -DSMTG_ENABLE_VST3_PLUGIN_EXAMPLES=OFF -DSMTG_ENABLE_VST3_HOSTING_EXAMPLES=OFF
cmake --build build/validator-vs --config Release --target validator
./build/validator-vs/bin/Release/validator.exe "build/assets/VST3/OpenUtau Bridge.vst3"
```

It instantiates the plugin through the factory and drives initialize, setActive, state and process
for real, including a run of `process` on another thread — which is as close to a host as anything
that is not one. Expect 47 passed, 0 failed.
