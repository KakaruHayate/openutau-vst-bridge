/*
 * The CLAP implementation: a stereo instrument that plays what OpenUtau has rendered, placed on
 * the host's timeline. Everything protocol-shaped lives behind Session; this file is the
 * translation between CLAP's vocabulary and that.
 *
 * Threads follow CLAP's own rules. init/destroy/activate and state are the main thread, process
 * is the audio thread, and the only things the audio thread does are Render() and NoteTransport().
 */

#include <clap/clap.h>
#include <clap/ext/gui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "bridge_entry.h"
#include "gui.h"
#include "session.h"
#include "transport.h"

namespace {

constexpr const char *kPluginId = "moe.kakaru.openutau-bridge";

/// The OpenUtau track this instance plays. A parameter rather than a GUI control: the plugin has
/// no window of its own, and every host can already show, automate and save a parameter.
constexpr clap_id kTrackParamId = 0;
constexpr int kMaxTrackNo = 63;

const char *const kFeatures[] = {
    CLAP_PLUGIN_FEATURE_INSTRUMENT,
    CLAP_PLUGIN_FEATURE_STEREO,
    nullptr,
};

const clap_plugin_descriptor_t kDescriptor = {
    CLAP_VERSION_INIT,
    kPluginId,
    "OpenUtau Bridge",
    "OpenUTAU",
    "https://github.com/KakaruHayate/openutau-vst-bridge",
    "",  // manual_url
    "",  // support_url
    "0.2.0",
    "Places audio rendered by OpenUtau onto the DAW timeline.",
    kFeatures,
};

/// One plugin instance.
struct Bridge {
    clap_plugin_t plugin{};
    const clap_host_t *host = nullptr;
    const clap_host_params_t *hostParams = nullptr;  // For the picker's flush nudge.
    double sampleRate = 0.0;
    /// The in-flight picker report: how many of gesture/value/gesture the host queue took,
    /// and the value it is reporting. See NotifyTrackRequest.
    int trackNotifyStage_ = 0;
    double trackNotifyValue_ = 0.0;
    bridge::Session session;
    bridge::InfoWindow *window = nullptr;  // Null where the platform has no gui backend.

    static Bridge *Of(const clap_plugin_t *plugin) {
        return static_cast<Bridge *>(plugin->plugin_data);
    }
};

int ClampTrack(double value) {
    return std::clamp(static_cast<int>(std::lround(value)), 0, kMaxTrackNo);
}

// ------------------------------------------------------------------ audio ports

uint32_t AudioPortsCount(const clap_plugin_t *, bool isInput) {
    return isInput ? 0u : 1u;
}

bool AudioPortsGet(const clap_plugin_t *, uint32_t index, bool isInput,
                   clap_audio_port_info_t *info) {
    if (isInput || index != 0) {
        return false;
    }
    info->id = 0;
    std::snprintf(info->name, sizeof(info->name), "Main");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

const clap_plugin_audio_ports_t kAudioPorts = {AudioPortsCount, AudioPortsGet};

// ---------------------------------------------------------------------- params

uint32_t ParamsCount(const clap_plugin_t *) {
    return 1;
}

bool ParamsGetInfo(const clap_plugin_t *, uint32_t index, clap_param_info_t *info) {
    if (index != 0) {
        return false;
    }
    std::memset(info, 0, sizeof(*info));
    info->id = kTrackParamId;
    info->flags = CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_AUTOMATABLE;
    info->min_value = 0.0;
    info->max_value = static_cast<double>(kMaxTrackNo);
    info->default_value = 0.0;
    std::snprintf(info->name, sizeof(info->name), "OpenUtau Track");
    return true;
}

bool ParamsGetValue(const clap_plugin_t *plugin, clap_id id, double *out) {
    if (id != kTrackParamId) {
        return false;
    }
    // The session holds it, so there is one copy of this value rather than two that can disagree.
    *out = static_cast<double>(Bridge::Of(plugin)->session.TrackNo());
    return true;
}

bool ParamsValueToText(const clap_plugin_t *, clap_id id, double value, char *out,
                       uint32_t capacity) {
    if (id != kTrackParamId) {
        return false;
    }
    // One-based, because that is how OpenUtau numbers tracks in its own window.
    std::snprintf(out, capacity, "Track %d", ClampTrack(value) + 1);
    return true;
}

bool ParamsTextToValue(const clap_plugin_t *, clap_id id, const char *text, double *out) {
    if (id != kTrackParamId || text == nullptr) {
        return false;
    }
    const char *digits = text;
    while (*digits != '\0' && (*digits < '0' || *digits > '9')) {
        digits++;  // Accepts "Track 3" as readily as "3".
    }
    if (*digits == '\0') {
        return false;
    }
    *out = static_cast<double>(ClampTrack(std::atof(digits) - 1.0));
    return true;
}

/// Parameter changes are applied whole-block. Sub-block accuracy would mean rendering two
/// tracks' audio into one buffer, which is not what changing this parameter means.
void ApplyEvents(Bridge *self, const clap_input_events_t *in) {
    if (in == nullptr || in->size == nullptr || in->get == nullptr) {
        return;
    }
    uint32_t count = in->size(in);
    for (uint32_t i = 0; i < count; i++) {
        const clap_event_header_t *header = in->get(in, i);
        if (header == nullptr || header->space_id != CLAP_CORE_EVENT_SPACE_ID ||
            header->type != CLAP_EVENT_PARAM_VALUE) {
            continue;
        }
        const auto *event = reinterpret_cast<const clap_event_param_value_t *>(header);
        if (event->param_id == kTrackParamId) {
            self->session.SetTrackNo(ClampTrack(event->value));
        }
    }
}

/// A track picked in the window has already changed the routing; this reports it to the
/// host as a genuine parameter movement (gesture, value, gesture), so automation records
/// it and the host's own controls follow. Queued wherever the host gives us an output
/// queue - process or flush - and a request_flush from the picker's callback wakes a
/// stopped host enough to run flush and collect it.
///
/// The output queue is allowed to reject events, so the report is staged: the consumed
/// value and how far the three-event sequence got live in the Bridge, and each call
/// resumes where the last one stopped. The pending flag is only cleared once the queue
/// took everything; a value picked mid-report simply re-reads at stage 0, so the newest
/// choice is what the host sees.
void NotifyTrackRequest(Bridge *self, const clap_output_events_t *out) {
    if (out == nullptr) {
        return;
    }
    if (self->trackNotifyStage_ == 0) {
        if (!self->session.ConsumeTrackRequest()) {
            return;
        }
        self->trackNotifyValue_ = static_cast<double>(self->session.TrackNo());
    }

    while (self->trackNotifyStage_ < 3) {
        bool accepted = false;
        if (self->trackNotifyStage_ != 1) {
            clap_event_param_gesture_t gesture{};
            gesture.header.size = sizeof(gesture);
            gesture.header.time = 0;
            gesture.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            gesture.header.type = self->trackNotifyStage_ == 0 ? CLAP_EVENT_PARAM_GESTURE_BEGIN
                                                               : CLAP_EVENT_PARAM_GESTURE_END;
            gesture.param_id = kTrackParamId;
            accepted = out->try_push(out, &gesture.header);
        } else {
            clap_event_param_value_t value{};
            value.header.size = sizeof(value);
            value.header.time = 0;
            value.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            value.header.type = CLAP_EVENT_PARAM_VALUE;
            value.param_id = kTrackParamId;
            value.value = self->trackNotifyValue_;
            accepted = out->try_push(out, &value.header);
        }
        if (!accepted) {
            return;  // Queue full or not accepting; try again on the next block or flush.
        }
        self->trackNotifyStage_++;
    }
    self->trackNotifyStage_ = 0;
}

void ParamsFlush(const clap_plugin_t *plugin, const clap_input_events_t *in,
                 const clap_output_events_t *out) {
    Bridge *self = Bridge::Of(plugin);
    ApplyEvents(self, in);
    NotifyTrackRequest(self, out);
}

const clap_plugin_params_t kParams = {
    ParamsCount, ParamsGetInfo, ParamsGetValue, ParamsValueToText, ParamsTextToValue, ParamsFlush,
};

// ----------------------------------------------------------------------- state

/// A four-byte tag and a version, so a blob written by a later build is recognised as one this
/// build cannot read rather than misread. Nothing but the track index is in it yet; the project
/// document belongs here too once reopening a host session restores OpenUtau's side of it.
constexpr char kStateTag[4] = {'O', 'U', 'B', 'R'};
constexpr uint32_t kStateVersion = 1;

bool WriteAll(const clap_ostream_t *stream, const void *data, size_t size) {
    const auto *bytes = static_cast<const char *>(data);
    size_t written = 0;
    while (written < size) {
        int64_t step = stream->write(stream, bytes + written, size - written);
        if (step <= 0) {
            return false;  // 0 is not "try again" for a CLAP stream; it is a failure.
        }
        written += static_cast<size_t>(step);
    }
    return true;
}

bool ReadAll(const clap_istream_t *stream, void *data, size_t size) {
    auto *bytes = static_cast<char *>(data);
    size_t read = 0;
    while (read < size) {
        int64_t step = stream->read(stream, bytes + read, size - read);
        if (step <= 0) {
            return false;
        }
        read += static_cast<size_t>(step);
    }
    return true;
}

bool StateSave(const clap_plugin_t *plugin, const clap_ostream_t *stream) {
    if (stream == nullptr) {
        return false;
    }
    int32_t trackNo = static_cast<int32_t>(Bridge::Of(plugin)->session.TrackNo());
    return WriteAll(stream, kStateTag, sizeof(kStateTag)) &&
           WriteAll(stream, &kStateVersion, sizeof(kStateVersion)) &&
           WriteAll(stream, &trackNo, sizeof(trackNo));
}

bool StateLoad(const clap_plugin_t *plugin, const clap_istream_t *stream) {
    if (stream == nullptr) {
        return false;
    }
    char tag[sizeof(kStateTag)] = {};
    uint32_t version = 0;
    int32_t trackNo = 0;
    if (!ReadAll(stream, tag, sizeof(tag)) || std::memcmp(tag, kStateTag, sizeof(tag)) != 0) {
        return false;
    }
    if (!ReadAll(stream, &version, sizeof(version)) || version > kStateVersion) {
        return false;
    }
    if (!ReadAll(stream, &trackNo, sizeof(trackNo))) {
        return false;
    }
    Bridge::Of(plugin)->session.SetTrackNo(ClampTrack(static_cast<double>(trackNo)));
    return true;
}

const clap_plugin_state_t kState = {StateSave, StateLoad};

// ---------------------------------------------------------------------- render

/// False: this plugin can be rendered faster than real time. Saying otherwise would tell a host
/// it must bounce in real time, which is a much heavier promise than what is actually needed —
/// only that an offline block may take as long as it takes.
bool RenderHasHardRealtimeRequirement(const clap_plugin_t *) {
    return false;
}

bool RenderSetMode(const clap_plugin_t *plugin, clap_plugin_render_mode mode) {
    // The whole point of knowing: part audio arrives over a socket while the timeline plays, and
    // a bounce runs through the project far faster than OpenUtau can send it. Offline, the
    // session waits for what is missing instead of rendering a hole where a part should be.
    Bridge::Of(plugin)->session.SetOffline(mode == CLAP_RENDER_OFFLINE);
    return true;
}

const clap_plugin_render_t kRender = {RenderHasHardRealtimeRequirement, RenderSetMode};

// -------------------------------------------------------------------- lifecycle

bool PluginInit(const clap_plugin_t *plugin) {
    // Listening from init rather than from activate: OpenUtau should be able to find and connect
    // to an instance the user has only just added, before any audio runs through it. A port that
    // cannot be bound is not fatal — the plugin still loads, and still passes audio through as
    // silence.
    Bridge *self = Bridge::Of(plugin);
    if (self->host != nullptr) {
        self->hostParams = static_cast<const clap_host_params_t *>(
            self->host->get_extension(self->host, CLAP_EXT_PARAMS));
    }
    self->session.Start();
    // The picker's flush nudge: a stopped host learns of the new value when it next runs
    // flush, which the request asks it to do soon.
    self->window = bridge::CreateInfoWindow(&self->session, [self] {
        if (self->hostParams != nullptr) {
            self->hostParams->request_flush(self->host);
        }
    });
    bridge::GuiRegister(plugin, self->window);  // Null on platforms with no gui backend.
    return true;
}

void PluginDestroy(const clap_plugin_t *plugin) {
    Bridge *self = Bridge::Of(plugin);
    bridge::GuiUnregister(plugin);
    delete self->window;
    delete self;  // Session's destructor stops the worker and unpublishes.
}

bool PluginActivate(const clap_plugin_t *plugin, double sampleRate, uint32_t, uint32_t) {
    // The wire format is fixed at 44.1 kHz (§6.1), so the store converts to whatever is asked
    // for here, and asking for something new drops and re-pulls every clip.
    Bridge *self = Bridge::Of(plugin);
    self->sampleRate = sampleRate;
    self->session.SetHostSampleRate(sampleRate);
    return true;
}

void PluginDeactivate(const clap_plugin_t *) {}

bool PluginStartProcessing(const clap_plugin_t *) {
    return true;
}

void PluginStopProcessing(const clap_plugin_t *plugin) {
    // Clears the edge without reporting one, so resuming counts as a fresh start.
    Bridge::Of(plugin)->session.NoteTransport(false, 0.0, false, false, 0.0);
}

void PluginReset(const clap_plugin_t *plugin) {
    Bridge::Of(plugin)->session.NoteTransport(false, 0.0, false, false, 0.0);
}

void ClearOutput(const clap_audio_buffer_t &out, uint32_t frames) {
    for (uint32_t channel = 0; channel < out.channel_count; channel++) {
        if (out.data32[channel] != nullptr) {
            std::memset(out.data32[channel], 0, sizeof(float) * frames);
        }
    }
}

clap_process_status PluginProcess(const clap_plugin_t *plugin, const clap_process_t *process) {
    Bridge *self = Bridge::Of(plugin);
    ApplyEvents(self, process->in_events);
    NotifyTrackRequest(self, process->out_events);

    if (process->audio_outputs_count == 0 || process->audio_outputs[0].data32 == nullptr ||
        process->audio_outputs[0].channel_count < 2) {
        // Not a stereo float port, which is the only thing this plugin declares or can fill.
        return CLAP_PROCESS_CONTINUE;
    }
    const clap_audio_buffer_t &out = process->audio_outputs[0];

    int64_t fromFrame = 0;
    bool positioned = bridge::BlockStartFrame(process->transport, self->sampleRate, &fromFrame);
    double seconds = 0.0;
    bool hasPosition = bridge::BlockSeconds(process->transport, &seconds);
    const clap_event_transport_t *transport = process->transport;
    bool hasTempo = transport != nullptr && (transport->flags & CLAP_TRANSPORT_HAS_TEMPO) != 0;
    // Reported every block, whatever the render decision: a parked transport is as much a
    // playhead state as a moving one, and the worker decides what is worth sending.
    self->session.NoteTransport(hasPosition, seconds, bridge::IsPlaying(transport), hasTempo,
                                hasTempo ? transport->tempo : 0.0);
    if (!positioned || !bridge::ShouldRender(transport, self->session.IsOffline()) ||
        out.data32[0] == nullptr || out.data32[1] == nullptr) {
        ClearOutput(out, process->frames_count);
        return CLAP_PROCESS_CONTINUE;
    }

    self->session.Render(fromFrame, process->frames_count, out.data32[0], out.data32[1]);
    // CONTINUE rather than SLEEP: audio can appear at any timeline position the moment a pull
    // completes, and a sleeping plugin may not be called again promptly.
    return CLAP_PROCESS_CONTINUE;
}

const void *PluginGetExtension(const clap_plugin_t *plugin, const char *id) {
    if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) {
        return &kAudioPorts;
    }
    if (std::strcmp(id, CLAP_EXT_PARAMS) == 0) {
        return &kParams;
    }
    if (std::strcmp(id, CLAP_EXT_STATE) == 0) {
        return &kState;
    }
    if (std::strcmp(id, CLAP_EXT_RENDER) == 0) {
        return &kRender;
    }
    if (std::strcmp(id, CLAP_EXT_GUI) == 0) {
        Bridge *self = Bridge::Of(plugin);
        return self->window != nullptr ? self->window->Extension() : nullptr;
    }
    return nullptr;
}

void PluginOnMainThread(const clap_plugin_t *) {}

// ----------------------------------------------------------------------- factory

uint32_t FactoryCount(const clap_plugin_factory_t *) {
    return 1;
}

const clap_plugin_descriptor_t *FactoryDescriptor(const clap_plugin_factory_t *,
                                                  uint32_t index) {
    return index == 0 ? &kDescriptor : nullptr;
}

const clap_plugin_t *FactoryCreate(const clap_plugin_factory_t *, const clap_host_t *host,
                                   const char *pluginId) {
    if (pluginId == nullptr || std::strcmp(pluginId, kPluginId) != 0) {
        return nullptr;
    }
    auto *bridge = new Bridge();
    bridge->host = host;
    bridge->plugin.desc = &kDescriptor;
    bridge->plugin.plugin_data = bridge;
    bridge->plugin.init = PluginInit;
    bridge->plugin.destroy = PluginDestroy;
    bridge->plugin.activate = PluginActivate;
    bridge->plugin.deactivate = PluginDeactivate;
    bridge->plugin.start_processing = PluginStartProcessing;
    bridge->plugin.stop_processing = PluginStopProcessing;
    bridge->plugin.reset = PluginReset;
    bridge->plugin.process = PluginProcess;
    bridge->plugin.get_extension = PluginGetExtension;
    bridge->plugin.on_main_thread = PluginOnMainThread;
    return &bridge->plugin;
}

const clap_plugin_factory_t kFactory = {FactoryCount, FactoryDescriptor, FactoryCreate};

}  // namespace

bool bridge_entry_init(const char *) {
    return true;
}

void bridge_entry_deinit(void) {}

const void *bridge_entry_get_factory(const char *factoryId) {
    if (factoryId != nullptr && std::strcmp(factoryId, CLAP_PLUGIN_FACTORY_ID) == 0) {
        return &kFactory;
    }
    return nullptr;
}
