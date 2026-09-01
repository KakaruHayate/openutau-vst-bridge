# openutau-vst-bridge

A thin plugin that puts OpenUtau's rendered audio on the DAW timeline. Editing stays in
the OpenUtau window; the plugin holds the connection, the audio, and the placement.

This is the plugin half of OpenUtau's DAW integration. The other half is the API under
`OpenUtau.Core/DawIntegration/` in the main repository. [`PROTOCOL.md`](PROTOCOL.md) is
the contract between them and is a verbatim copy of the file that ships there — **change
one and you must change the other.**

**Status: skeleton.** The build produces a loadable CLAP and VST3 that output silence. The
bridge itself is not implemented yet.

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

## Layout

```
src/
  bridge_entry.h    the three symbols a format module needs
  bridge_entry.cpp  the exported clap_entry, one per module
  plugin.cpp        descriptor, factory, and the CLAP implementation
libs/               dependencies, all permissively licensed
PROTOCOL.md         the wire contract, mirrored from the main repository
```

`src/` builds into a static library that knows nothing about formats;
`make_clapfirst_plugins` links it into one module per format.
