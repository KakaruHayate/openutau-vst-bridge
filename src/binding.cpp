#include "binding.h"

#include <algorithm>
#include <cstring>

namespace bridge {

namespace {

constexpr char kStateTag[4] = {'O', 'U', 'B', 'R'};
/// Still 1: the appended id is invisible to a 1.1 reader, which rejects anything newer.
constexpr uint32_t kStateVersion = 1;
constexpr char kIdTag[4] = {'O', 'U', 'I', 'D'};
/// Ids are 11 characters today; anything far longer is not one of ours.
constexpr uint32_t kMaxIdBytes = 256;

template <typename T>
void Append(std::vector<uint8_t> *out, const T &value) {
    const auto *bytes = reinterpret_cast<const uint8_t *>(&value);
    out->insert(out->end(), bytes, bytes + sizeof(T));
}

template <typename T>
bool Take(const std::vector<uint8_t> &bytes, size_t *offset, T *value) {
    if (bytes.size() - *offset < sizeof(T)) {
        return false;
    }
    std::memcpy(value, bytes.data() + *offset, sizeof(T));
    *offset += sizeof(T);
    return true;
}

}  // namespace

bool TracksCarryIds(const std::vector<TrackInfo> &tracks) {
    return !tracks.empty() &&
           std::all_of(tracks.begin(), tracks.end(),
                       [](const TrackInfo &track) { return !track.id.empty(); });
}

Resolution Resolve(Binding *binding, const std::vector<TrackInfo> &tracks) {
    Resolution result;
    if (tracks.empty() && !binding->trackId.empty()) {
        // No track can carry the bound id, whichever OpenUtau sent the list.
        result.trackNo = -1;
        return result;
    }
    if (!TracksCarryIds(tracks)) {
        result.trackNo = binding->trackNo;
        return result;
    }
    if (binding->trackId.empty()) {
        if (binding->trackNo >= 0 && static_cast<size_t>(binding->trackNo) < tracks.size()) {
            binding->trackId = tracks[static_cast<size_t>(binding->trackNo)].id;
            result.trackNo = binding->trackNo;
            result.adopted = true;
        } else {
            result.trackNo = -1;
        }
        return result;
    }
    for (size_t i = 0; i < tracks.size(); i++) {
        if (tracks[i].id == binding->trackId) {
            binding->trackNo = static_cast<int>(i);
            result.trackNo = binding->trackNo;
            return result;
        }
    }
    result.trackNo = -1;
    return result;
}

std::vector<uint8_t> EncodeState(const Binding &binding) {
    std::vector<uint8_t> out;
    out.insert(out.end(), kStateTag, kStateTag + sizeof(kStateTag));
    Append(&out, kStateVersion);
    Append(&out, static_cast<int32_t>(binding.trackNo));
    if (!binding.trackId.empty() && binding.trackId.size() <= kMaxIdBytes) {
        out.insert(out.end(), kIdTag, kIdTag + sizeof(kIdTag));
        Append(&out, static_cast<uint32_t>(binding.trackId.size()));
        out.insert(out.end(), binding.trackId.begin(), binding.trackId.end());
    }
    return out;
}

bool DecodeState(const std::vector<uint8_t> &bytes, Binding *binding) {
    size_t offset = 0;
    char tag[sizeof(kStateTag)] = {};
    uint32_t version = 0;
    int32_t trackNo = 0;
    if (!Take(bytes, &offset, &tag) || std::memcmp(tag, kStateTag, sizeof(tag)) != 0) {
        return false;
    }
    if (!Take(bytes, &offset, &version) || version > kStateVersion) {
        return false;
    }
    if (!Take(bytes, &offset, &trackNo)) {
        return false;
    }
    Binding decoded;
    decoded.trackNo = trackNo;
    char idTag[sizeof(kIdTag)] = {};
    uint32_t length = 0;
    if (Take(bytes, &offset, &idTag) && std::memcmp(idTag, kIdTag, sizeof(idTag)) == 0 &&
        Take(bytes, &offset, &length) && length <= kMaxIdBytes &&
        bytes.size() - offset >= length) {
        decoded.trackId.assign(reinterpret_cast<const char *>(bytes.data() + offset), length);
    }
    *binding = std::move(decoded);
    return true;
}

}  // namespace bridge
