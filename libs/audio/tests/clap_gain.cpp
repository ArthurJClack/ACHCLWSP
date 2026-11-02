// A minimal CLAP effect for the host tests: one stereo port in and out and one
// parameter, Gain (id 7, -24..24 dB), changed by parameter events.
#include <clap/clap.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

struct Gain {
    clap_plugin_t plugin;
    double db = 0;
};

const char* const kFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_UTILITY, nullptr};
const clap_plugin_descriptor_t kDesc = {CLAP_VERSION_INIT, "org.wetwaresuite.test-gain", "Test Gain", "wetwaresuite", "", "", "", "1.0",
                                        "Turns the sound up or down", kFeatures};

Gain* self(const clap_plugin_t* p) { return static_cast<Gain*>(p->plugin_data); }

void events(Gain* g, const clap_input_events_t* in) {
    const uint32_t n = in ? in->size(in) : 0;
    for (uint32_t i = 0; i < n; ++i) {
        const clap_event_header_t* h = in->get(in, i);
        if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_PARAM_VALUE) {
            const auto* e = reinterpret_cast<const clap_event_param_value_t*>(h);
            if (e->param_id == 7) g->db = e->value;
        }
    }
}

// params
uint32_t pCount(const clap_plugin_t*) { return 1; }
bool pInfo(const clap_plugin_t*, uint32_t index, clap_param_info_t* info) {
    if (index != 0) return false;
    std::memset(info, 0, sizeof *info);
    info->id = 7;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    std::strncpy(info->name, "Gain", CLAP_NAME_SIZE - 1);
    info->min_value = -24;
    info->max_value = 24;
    info->default_value = 0;
    return true;
}
bool pValue(const clap_plugin_t* p, clap_id id, double* out) {
    if (id != 7) return false;
    *out = self(p)->db;
    return true;
}
bool pToText(const clap_plugin_t*, clap_id, double v, char* out, uint32_t size) {
    std::snprintf(out, size, "%.1f dB", v);
    return true;
}
bool pFromText(const clap_plugin_t*, clap_id, const char* text, double* out) {
    *out = std::atof(text);
    return true;
}
void pFlush(const clap_plugin_t* p, const clap_input_events_t* in, const clap_output_events_t*) { events(self(p), in); }
const clap_plugin_params_t kParams = {pCount, pInfo, pValue, pToText, pFromText, pFlush};

// audio ports
uint32_t aCount(const clap_plugin_t*, bool) { return 1; }
bool aGet(const clap_plugin_t*, uint32_t index, bool input, clap_audio_port_info_t* info) {
    if (index != 0) return false;
    std::memset(info, 0, sizeof *info);
    info->id = input ? 0 : 1;
    std::strncpy(info->name, input ? "in" : "out", CLAP_NAME_SIZE - 1);
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}
const clap_plugin_audio_ports_t kPorts = {aCount, aGet};

bool init(const clap_plugin_t*) { return true; }
void destroy(const clap_plugin_t* p) { delete self(p); }
bool activate(const clap_plugin_t*, double, uint32_t, uint32_t) { return true; }
void deactivate(const clap_plugin_t*) {}
bool startProcessing(const clap_plugin_t*) { return true; }
void stopProcessing(const clap_plugin_t*) {}
void reset(const clap_plugin_t*) {}
clap_process_status process(const clap_plugin_t* p, const clap_process_t* proc) {
    Gain* g = self(p);
    events(g, proc->in_events);
    const float k = static_cast<float>(std::pow(10.0, g->db / 20.0));
    for (uint32_t c = 0; c < 2; ++c)
        for (uint32_t i = 0; i < proc->frames_count; ++i) proc->audio_outputs[0].data32[c][i] = proc->audio_inputs[0].data32[c][i] * k;
    return CLAP_PROCESS_CONTINUE;
}
const void* extension(const clap_plugin_t*, const char* id) {
    if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &kParams;
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &kPorts;
    return nullptr;
}
void onMainThread(const clap_plugin_t*) {}

uint32_t fCount(const clap_plugin_factory_t*) { return 1; }
const clap_plugin_descriptor_t* fDesc(const clap_plugin_factory_t*, uint32_t i) { return i == 0 ? &kDesc : nullptr; }
const clap_plugin_t* fCreate(const clap_plugin_factory_t*, const clap_host_t*, const char* id) {
    if (std::strcmp(id, kDesc.id)) return nullptr;
    auto* g = new Gain;
    g->plugin = {&kDesc, g, init, destroy, activate, deactivate, startProcessing, stopProcessing, reset, process, extension, onMainThread};
    return &g->plugin;
}
const clap_plugin_factory_t kFactory = {fCount, fDesc, fCreate};

bool entryInit(const char*) { return true; }
void entryDeinit() {}
const void* entryFactory(const char* id) { return !std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &kFactory : nullptr; }

}  // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, entryInit, entryDeinit, entryFactory};
