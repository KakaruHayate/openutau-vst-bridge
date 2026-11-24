#pragma once

/*
 * Which OpenUtau track an instance plays, and how that choice is saved with the host session.
 *
 * Up to v1.2 an instance named its track by position, so reordering tracks in OpenUtau moved
 * every DAW track onto another voice. From v1.3 OpenUtau gives each track a stable id and the
 * instance binds to that. The position stays alongside it: it is the whole binding with an
 * older OpenUtau, and it is what an id is adopted from - a pick by position in the window or
 * from host automation, or a host session saved before ids existed.
 *
 * Nothing here touches threads or the host; the session and the plugin call it.
 */

#include "messages.h"

#include <cstdint>
#include <string>
#include <vector>

namespace bridge {

struct Binding {
    /// The bound track's id; empty while none is known yet.
    std::string trackId;
    /// The bound track's last known position.
    int trackNo = 0;
};

/// The outcome of matching a binding against OpenUtau's track list.
struct Resolution {
    /// The position the track is at now, or -1 when the binding names a track the list does
    /// not have (deleted, another project): then the instance is silent rather than playing a
    /// neighbour, and keeps the id so that an undo finds the track again.
    int trackNo = 0;
    /// True when the binding just took the id of the track at its position.
    bool adopted = false;
};

/// True when OpenUtau identifies its tracks (v1.3): a non-empty list where every track has an id.
bool TracksCarryIds(const std::vector<TrackInfo> &tracks);

/// Matches `binding` against `tracks`. Without ids, the position is the binding. With ids, a
/// binding that has none adopts the id at its position, and one that has an id follows it to
/// wherever the track is now (`binding->trackNo` is updated to match).
Resolution Resolve(Binding *binding, const std::vector<TrackInfo> &tracks);

/// The plugin state. The first 12 bytes are exactly what plugin 1.1 writes and reads (a tag,
/// version 1, the position), and the id is appended after them. An older build reads its 12
/// bytes and ignores the rest, so downgrading the plugin keeps the DAW project loading.
std::vector<uint8_t> EncodeState(const Binding &binding);

/// False when `bytes` is not a state this build can read. A state without the appended id
/// decodes with an empty `trackId`, which the first track list with ids then fills in.
bool DecodeState(const std::vector<uint8_t> &bytes, Binding *binding);

}  // namespace bridge
