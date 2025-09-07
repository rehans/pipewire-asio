// Copyright (c) 2025-present, rehans.

#include "asiodrvr.h"
#include "types.h"
#include "utils.h"
#include <algorithm>
#include <array>
#include <pipewire/device.h>
#include <pipewire/extensions/metadata.h>
#include <pipewire/filter.h>
#include <pipewire/pipewire.h>
#include <spa/param/latency-utils.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <string>
#include <vector>

//-----------------------------------------------------------------------------
namespace p2a {
namespace pipewire {

struct Globals
{
    // https://docs.pipewire.org/page_thread_loop.html#sec_thread_loop_overview
    pw_main_loop* main_loop        = nullptr;
    pw_thread_loop* thread_loop    = nullptr;
    pw_loop* loop                  = nullptr;
    pw_context* context            = nullptr;
    pw_core* core                  = nullptr;
    pw_registry* registry          = nullptr;
    pw_metadata* metadata_settings = nullptr;
    pw_metadata* metadata_default  = nullptr;

    Objects objects;
};

} // namespace pipewire

struct ASIO
{
    using Buffer                = std::vector<SampleType>;
    using DoubleBuffer32        = std::array<Buffer, 2>;
    using ChannelDoubleBuffer32 = std::vector<DoubleBuffer32>;

    ASIOCallbacks* callbacks = nullptr;
    long double_buf_index    = 0;
    ChannelDoubleBuffer32 in_bufs;
    ChannelDoubleBuffer32 out_bufs;
};

//-----------------------------------------------------------------------------
// Data
//-----------------------------------------------------------------------------
struct Data
{
    pipewire::Globals pipewire;
    ASIO asio;

    FilterNode filter_node;
    Settings settings;
    Object defaults;

    spa_hook registry_listener{0};
    spa_hook md_default_listener{0};
    spa_hook md_settings_listener{0};
};

using StringView = std::string_view;
//-----------------------------------------------------------------------------
constexpr auto CLOCK_RATE          = "clock.rate";
constexpr auto CLOCK_ALLOWED_RATES = "clock.allowed-rates";
constexpr auto CLOCK_QUANTUM       = "clock.quantum";
constexpr auto CLOCK_MIN_QUANTUM   = "clock.min-quantum";
constexpr auto CLOCK_MAX_QUANTUM   = "clock.max-quantum";
constexpr auto CLOCK_FORCE_QUANTUM = "clock.force-quantum";
constexpr auto CLOCK_FORCE_RATE    = "clock.force-rate";

static auto on_metadata_settings(void* user_data,
                                 uint32_t id,
                                 const char* key,
                                 const char* type,
                                 const char* value) -> int
{
    Data* data = static_cast<Data*>(user_data);
    if (!data)
        return -1;

    auto& settings = data->settings;
    auto& asio     = data->asio;

    fprintf(stdout,
            "'settings' update: id:%u key:'%s' value:'%s' type:'%s'\n",
            id,
            key,
            value,
            type);

    if (StringView(key) == CLOCK_RATE)
    {
        const auto rate = pw_properties_parse_double(value);
        if (settings.rate != rate)
        {
            settings.rate = rate;
            if (asio.callbacks)
                asio.callbacks->sampleRateDidChange(rate);
        }
    }
    else if (StringView(key) == CLOCK_QUANTUM)
    {
        const auto quantum = pw_properties_parse_int64(value);
        if (settings.quantum != quantum)
        {
            settings.quantum = quantum;
            if (asio.callbacks)
                asio.callbacks->asioMessage(
                    kAsioBufferSizeChange, quantum, nullptr, nullptr);
        }
    }
    else if (StringView(key) == CLOCK_MIN_QUANTUM)
    {
        settings.min_quantum = pw_properties_parse_int64(value);
    }
    else if (StringView(key) == CLOCK_MAX_QUANTUM)
    {
        settings.max_quantum = pw_properties_parse_int64(value);
    }
    else if (StringView(key) == CLOCK_ALLOWED_RATES)
    {
        settings.allowed_rates = parse_values_string(value);
    }
    else if (StringView(key) == CLOCK_FORCE_RATE)
    {
        settings.force_rate = pw_properties_parse_double(value);
    }
    else if (StringView(key) == CLOCK_FORCE_QUANTUM)
    {
        settings.force_quantum = pw_properties_parse_int64(value);
    }

    return 0;
}

//-----------------------------------------------------------------------------
constexpr auto AUDIO_SINK            = "default.audio.sink";
constexpr auto AUDIO_SOURCE          = "default.audio.source";
constexpr auto CONFIGURED_AUDIO_SINK = "default.configured.audio.sink";

static auto on_metadata_default(void* user_data,
                                uint32_t id,
                                const char* key,
                                const char* type,
                                const char* value) -> int
{
    Data* data = static_cast<Data*>(user_data);
    if (!data)
        return -1;

    fprintf(stdout,
            "'default' update: id:%u key:'%s' value:'%s' type:'%s'\n",
            id,
            key,
            value,
            type);

    if (StringView(key) == AUDIO_SINK)
    {
        // TODO for autoconnect
    }
    else if (StringView(key) == AUDIO_SOURCE)
    {
        // TODO for autoconnect
    }
    else if (StringView(key) == CONFIGURED_AUDIO_SINK)
    {
    }

    return 0;
}

//-----------------------------------------------------------------------------
// pw_registry
//-----------------------------------------------------------------------------
static auto on_register_node(Data& data, const Object& object) -> void
{
    auto& filter_node = data.filter_node;
    if (!filter_node.handle)
        return;

    if (object.id != pw_filter_get_node_id(filter_node.handle))
        return;

    filter_node.obj = object;
}

//-----------------------------------------------------------------------------
static auto on_register_port(Data& data, const Object& object) -> void
{
    if (!data.filter_node.handle)
        return;

    auto iter = object.props.find(PW_KEY_NODE_ID);
    if (iter == object.props.end())
        return;

    // Only collect the ports which belong to our filter node
    const uint32_t node_id = pw_properties_parse_uint64(iter->second.c_str());
    if (node_id != data.filter_node.obj.id)
        return;

    iter = object.props.find(PW_KEY_PORT_ID);
    if (iter == object.props.end())
        return;

    const auto port_id = pw_properties_parse_uint64(iter->second.c_str());

    iter = object.props.find(PW_KEY_PORT_DIRECTION);
    if (iter == object.props.end())
        return;

    if (iter->second == "in")
    {
        auto& port   = data.filter_node.ins.at(port_id);
        port.node_id = node_id;
        port.obj     = object;
    }
    else if (iter->second == "out")
    {
        auto& port   = data.filter_node.outs.at(port_id);
        port.node_id = node_id;
        port.obj     = object;
    }
}

//-----------------------------------------------------------------------------
static auto on_register_metadata(Data& data, const Object& object) -> void
{
    if (object.name == "settings")
        data.settings.obj = object;
    else if (object.name == "default")
        data.defaults = object;
}

//-----------------------------------------------------------------------------
static auto registry_event_global(void* user_data,
                                  uint32_t id,
                                  uint32_t permissions,
                                  const char* type,
                                  uint32_t version,
                                  const struct spa_dict* props) -> void
{
    Data* data = static_cast<Data*>(user_data);
    if (!data)
        return;

    if (props == NULL)
        return;

    const auto name = find_object_name(props);
    if (name == NULL)
        return;

    fprintf(stdout,
            "global object: id:%u type:'%s' name:'%s' version: %u\n",
            id,
            type,
            name,
            version);

    const Object obj{id, to_map(props), type, version, permissions, name};

    if (obj.type == PW_TYPE_INTERFACE_Node)
    {
        on_register_node(*data, obj);
    }
    else if (obj.type == PW_TYPE_INTERFACE_Port)
    {
        on_register_port(*data, obj);
    }
    else if (obj.type == PW_TYPE_INTERFACE_Link)
    {
        // TODO Track our Links later
    }
    else if (obj.type == PW_TYPE_INTERFACE_Metadata)
    {
        on_register_metadata(*data, obj);
    }
    else if (obj.type == PW_TYPE_INTERFACE_Device)
    {
    }
    else if (obj.type == PW_TYPE_INTERFACE_Client)
    {
    }

    data->pipewire.objects.emplace_back(obj);
}

//-----------------------------------------------------------------------------
static auto registry_event_remove(void* user_data, uint32_t id) -> void
{
    Data* data = static_cast<Data*>(user_data);
    if (!data)
        return;

    auto iter = std::find_if(data->pipewire.objects.begin(),
                             data->pipewire.objects.end(),
                             [id](const auto& node) { return node.id == id; });
    if (iter == data->pipewire.objects.end())
        return;

    data->pipewire.objects.erase(iter);

    fprintf(stdout,
            "remove object: id:%u name:'%s'\n",
            iter->id,
            iter->name.c_str());
}

//-----------------------------------------------------------------------------
static const struct pw_registry_events registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global        = registry_event_global,
    .global_remove = registry_event_remove};

//-----------------------------------------------------------------------------
// FilterNode
//-----------------------------------------------------------------------------
static auto convert(struct spa_io_position* position) -> ASIOTime
{
    ASIOTime asio_time{0};
    asio_time.timeInfo.samplePosition.lo = position->clock.position;
    asio_time.timeInfo.speed             = position->clock.rate_diff;
    asio_time.timeInfo.systemTime.lo     = position->clock.nsec;
    asio_time.timeInfo.sampleRate        = position->clock.rate.denom;
    asio_time.timeInfo.flags = kSystemTimeValid | kSamplePositionValid |
                               kSampleRateValid | kSpeedValid;

    return asio_time;
}

//-----------------------------------------------------------------------------
static auto toggle(long& double_buf_index) -> long
{
    double_buf_index = double_buf_index == 0 ? 1 : 0;
    return double_buf_index;
}

//-----------------------------------------------------------------------------
static auto copy_asio_bufs(Data& data, uint32_t num_samples) -> bool
{
    const auto buf_index     = data.asio.double_buf_index;
    const auto bytes_to_copy = num_samples * sizeof(SampleType);

    // PipeWire input port -> ASIO input channel
    for (auto i = 0; i < data.filter_node.ins.size(); i++)
    {
        auto in = pw_filter_get_dsp_buffer(data.filter_node.ins.at(i).handle,
                                           num_samples);

        auto& buf        = data.asio.in_bufs.at(i);
        auto& double_buf = buf.at(buf_index);
        void* byte_buf   = static_cast<void*>(double_buf.data());
        if (in != nullptr)
            memcpy(byte_buf, in, bytes_to_copy);
        else
            memset(byte_buf, 0, bytes_to_copy);
    }

    // ASIO output channel -> PipeWire output port
    for (auto i = 0; i < data.filter_node.outs.size(); i++)
    {
        auto out = pw_filter_get_dsp_buffer(data.filter_node.outs.at(i).handle,
                                            num_samples);
        if (out != nullptr)
        {
            auto& buf        = data.asio.out_bufs.at(i);
            auto& double_buf = buf.at(buf_index);
            void* byte_buf   = static_cast<void*>(double_buf.data());
            memcpy(out, byte_buf, bytes_to_copy);
        }
    }

    return true;
}

//-----------------------------------------------------------------------------
static auto switch_asio_bufs(ASIOTime asio_time, Data& data) -> void
{
    if (!data.asio.callbacks)
        return;

    data.asio.callbacks->bufferSwitchTimeInfo(
        &asio_time, data.asio.double_buf_index, ASIOFalse);
    toggle(data.asio.double_buf_index);
}

//-----------------------------------------------------------------------------
static auto on_process(void* user_data, struct spa_io_position* position)
    -> void
{
    Data* data = static_cast<Data*>(user_data);
    if (!position || !data)
        return;

    // TODO: What if 'position->clock.duration' is bigger than our
    // blocksize of 1024???
    const auto n_samples = position->clock.duration;

    bool res = copy_asio_bufs(*data, n_samples);
    if (!res)
        return;

    switch_asio_bufs(convert(position), *data);
}

//-----------------------------------------------------------------------------
static const struct pw_filter_events filter_events = {
    PW_VERSION_FILTER_EVENTS,
    .process = on_process,
};

//-----------------------------------------------------------------------------
static auto create_pw_filter(Data& data,
                             const ChannelInfos& ins,
                             const ChannelInfos& outs) -> int
{
    // Modified example:
    // https://docs.pipewire.org/audio-dsp-filter_8c-example.html

    /* Create a simple filter, the simple filter manages the core and
     * remote objects for you if you don't need to deal with them.
     *
     * Pass your events and a user_data pointer as the last arguments.
     * This will inform you about the filter state. The most important
     * event you need to listen to is the process event where you need
     * to process the data.
     */
    data.filter_node.handle =
        pw_filter_new_simple(data.pipewire.loop,
                             APP_NAME,
                             pw_properties_new(PW_KEY_MEDIA_TYPE,
                                               "Audio",
                                               PW_KEY_MEDIA_CATEGORY,
                                               "Filter",
                                               PW_KEY_MEDIA_ROLE,
                                               "DSP",
                                               PW_KEY_NODE_AUTOCONNECT,
                                               "true",
                                               PW_KEY_NODE_ALWAYS_PROCESS,
                                               "true",
                                               PW_KEY_CLIENT_API,
                                               "pipewire-asio",
                                               PW_KEY_NODE_MAX_LATENCY,
                                               "2048/48000",
                                               NULL),
                             &filter_events,
                             &data);

    if (data.filter_node.handle == nullptr)
    {
        fprintf(stderr, "can't create filter: %m\n");
        return -1;
    }

    for (const auto& info : ins)
    {
        Port port;
        port.handle =
            pw_filter_add_port(data.filter_node.handle,
                               PW_DIRECTION_INPUT,
                               PW_FILTER_PORT_FLAG_MAP_BUFFERS,
                               sizeof(Handle),
                               pw_properties_new(PW_KEY_FORMAT_DSP,
                                                 "32 bit float mono audio",
                                                 PW_KEY_PORT_NAME,
                                                 info.name.c_str(), //"input",
                                                 NULL),
                               NULL,
                               0);
        data.filter_node.ins.emplace_back(port);
    }

    for (const auto& info : outs)
    {
        Port port;
        port.handle =
            pw_filter_add_port(data.filter_node.handle,
                               PW_DIRECTION_OUTPUT,
                               PW_FILTER_PORT_FLAG_MAP_BUFFERS,
                               sizeof(Handle),
                               pw_properties_new(PW_KEY_FORMAT_DSP,
                                                 "32 bit float mono audio",
                                                 PW_KEY_PORT_NAME,
                                                 info.name.c_str(), //"output",
                                                 NULL),
                               NULL,
                               0);
        data.filter_node.outs.emplace_back(port);
    }

    return 0;
}

//-----------------------------------------------------------------------------
static auto destroy_pw_filter(Data& data) -> void
{
    pw_filter_destroy(data.filter_node.handle);
    data.filter_node.handle = nullptr;
    data.filter_node        = {0};
}

//-----------------------------------------------------------------------------
static auto create_filter_params(const spa_pod* params[1]) -> void
{
    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

    const spa_process_latency_info latency_info = {/*.quantum = */ 0,
                                                   /*.rate    = */ 0,
                                                   /*.ns      = */ 10 *
                                                       SPA_NSEC_PER_MSEC};
    params[0] =
        spa_process_latency_build(&b, SPA_PARAM_ProcessLatency, &latency_info);
}

//-----------------------------------------------------------------------------
static const struct pw_metadata_events metadata_settings_events = {
    PW_VERSION_METADATA_EVENTS, .property = on_metadata_settings};

//-----------------------------------------------------------------------------
static const struct pw_metadata_events metadata_default_events = {
    PW_VERSION_METADATA_EVENTS, .property = on_metadata_default};

//-----------------------------------------------------------------------------

static auto add_all_listeners(Data& data) -> int
{
    auto obj = pw_registry_bind(data.pipewire.registry,
                                data.settings.obj.id,
                                data.settings.obj.type.c_str(),
                                data.settings.obj.version,
                                0);

    data.pipewire.metadata_settings = static_cast<pw_metadata*>(obj);
    pw_metadata_add_listener(data.pipewire.metadata_settings,
                             &data.md_settings_listener,
                             &metadata_settings_events,
                             &data);

    obj = pw_registry_bind(data.pipewire.registry,
                           data.defaults.id,
                           data.defaults.type.c_str(),
                           data.defaults.version,
                           0);

    data.pipewire.metadata_default = static_cast<pw_metadata*>(obj);
    return pw_metadata_add_listener(data.pipewire.metadata_default,
                                    &data.md_default_listener,
                                    &metadata_default_events,
                                    &data);
}

//-----------------------------------------------------------------------------
static auto init_pipewire(Data& data) -> bool
{
    auto& pipewire = data.pipewire;

    pw_init(nullptr, nullptr);
    pipewire.main_loop = pw_main_loop_new(nullptr);
    if (pipewire.main_loop == nullptr)
    {
        fprintf(stderr, "can't create mainloop: %m\n");
        return false;
    }

    pipewire.loop    = pw_main_loop_get_loop(pipewire.main_loop);
    pipewire.context = pw_context_new(pipewire.loop, NULL, 0);
    if (pipewire.context == nullptr)
    {
        fprintf(stderr, "can't create context: %m\n");
        return false;
    }

    pipewire.core = pw_context_connect(pipewire.context, nullptr, 0);
    if (pipewire.core == nullptr)
    {
        fprintf(stderr, "can't connext core: %m\n");
        return false;
    }

    pipewire.registry =
        pw_core_get_registry(pipewire.core, PW_VERSION_REGISTRY, 0);

    // When ever we attach listeners we need to roundtrip to trigger
    // them.
    pw_registry_add_listener(
        pipewire.registry, &data.registry_listener, &registry_events, &data);
    roundtrip(pipewire.core, pipewire.main_loop);

    add_all_listeners(data);
    roundtrip(pipewire.core, pipewire.main_loop);

    return true;
}

//-----------------------------------------------------------------------------
static auto deinit_pipewire(Data& data) -> void
{
    auto& pipewire = data.pipewire;

    pw_core_disconnect(pipewire.core);
    pw_context_destroy(pipewire.context);
    pw_main_loop_destroy(pipewire.main_loop);
    pw_deinit();

    pipewire.context     = nullptr;
    pipewire.thread_loop = nullptr;
    pipewire.loop        = nullptr;
    pipewire.main_loop   = nullptr;
}

//-----------------------------------------------------------------------------
// PipeWire2AsioWrapper
//-----------------------------------------------------------------------------
class PipeWire2AsioWrapper : public AsioDriver
{
public:
    //-----------------------------------------------------------------------------
    PipeWire2AsioWrapper() = default;
    ~PipeWire2AsioWrapper() override;

    ASIOBool init(void* sysRef) override;
    void getDriverName(char* name) override;
    long getDriverVersion() override;
    ASIOError getChannels(long* numInputChannels,
                          long* numOutputChannels) override;
    ASIOError getSampleRate(ASIOSampleRate* sampleRate) override;
    ASIOError canSampleRate(ASIOSampleRate sampleRate) override;
    ASIOError getBufferSize(long* minSize,
                            long* maxSize,
                            long* preferredSize,
                            long* granularity) override;
    ASIOError createBuffers(ASIOBufferInfo* channelInfos,
                            long numChannels,
                            long bufferSize,
                            ASIOCallbacks* callbacks) override;
    ASIOError disposeBuffers() override;
    ASIOError getChannelInfo(ASIOChannelInfo* info) override;
    ASIOError getLatencies(long* inputLatency, long* outputLatency) override;

    ASIOError start() override;
    ASIOError stop() override;

    //-----------------------------------------------------------------------------
private:
    Data data;

    ChannelInfos in_infos;
    ChannelInfos out_infos;
};

//-----------------------------------------------------------------------------
// PipeWire2AsioWrapper
//-----------------------------------------------------------------------------
PipeWire2AsioWrapper::~PipeWire2AsioWrapper()
{
    destroy_pw_filter(data);
    deinit_pipewire(data);
}

//-----------------------------------------------------------------------------
ASIOBool PipeWire2AsioWrapper::init(void* sysRef)
{
    in_infos.push_back({"in_0"});
    in_infos.push_back({"in_1"});
    out_infos.push_back({"out_0"});
    out_infos.push_back({"out_1"});

    // Initialize all PipeWire globals as soon as possibble so we can
    // use them for querying information.
    auto bRes = init_pipewire(data);
    if (!bRes)
        return ASIOFalse;

    auto iRes = create_pw_filter(data, in_infos, out_infos);
    if (iRes < 0)
        return ASIOFalse;

    return data.settings.rate > 0. ? ASIOTrue : ASIOFalse;
}

//-----------------------------------------------------------------------------
long PipeWire2AsioWrapper::getDriverVersion()
{
    return 1;
}

//-----------------------------------------------------------------------------
void PipeWire2AsioWrapper::getDriverName(char* name)
{
    strcpy(name, APP_NAME);
}

//-----------------------------------------------------------------------------
ASIOError PipeWire2AsioWrapper::getChannels(long* numInputChannels,
                                            long* numOutputChannels)
{
    (*numInputChannels)  = in_infos.size();
    (*numOutputChannels) = out_infos.size();

    return ASE_OK;
}

//-----------------------------------------------------------------------------
ASIOError PipeWire2AsioWrapper::getChannelInfo(ASIOChannelInfo* info)
{
    constexpr size_t ASIO_CHANNEL_NAME_SIZE = sizeof(ASIOChannelInfo::name);

    if (!info)
        return ASE_InvalidParameter;

    info->channelGroup = -1;
    info->type         = ASIOSTFloat32LSB;
    info->isActive     = ASIOTrue;

    switch (info->isInput)
    {
        case ASIOTrue: {
            const auto& ch = in_infos.at(info->channel);
            const auto len = std::min(ASIO_CHANNEL_NAME_SIZE, ch.name.size());
            ch.name.copy(info->name, len, 0);
            break;
        }

        case ASIOFalse: {
            const auto& ch = out_infos.at(info->channel);
            const auto len = std::min(ASIO_CHANNEL_NAME_SIZE, ch.name.size());
            ch.name.copy(info->name, len, 0);
            break;
        }
    }

    return ASE_OK;
}

//-----------------------------------------------------------------------------
ASIOError PipeWire2AsioWrapper::getLatencies(long* inputLatency,
                                             long* outputLatency)
{
    return ASE_OK;
}

//-----------------------------------------------------------------------------
ASIOError PipeWire2AsioWrapper::getSampleRate(ASIOSampleRate* sampleRate)
{
    *sampleRate = data.settings.rate;

    return ASE_OK;
}

//-----------------------------------------------------------------------------
ASIOError PipeWire2AsioWrapper::canSampleRate(ASIOSampleRate sampleRate)
{
    const auto& allowed = data.settings.allowed_rates;
    const auto iter     = std::find(allowed.begin(), allowed.end(), sampleRate);
    return iter != allowed.end() ? ASE_OK : ASE_NoClock;
}

//-----------------------------------------------------------------------------
ASIOError PipeWire2AsioWrapper::getBufferSize(long* minSize,
                                              long* maxSize,
                                              long* preferredSize,
                                              long* granularity)
{
    *minSize       = data.settings.min_quantum;
    *maxSize       = data.settings.max_quantum;
    *preferredSize = data.settings.quantum;

    // TODO: I dont get it fully yet, cause when we attach to FireFox,
    // the blocksize becomes 2048
    *preferredSize = 2048;

    return ASE_OK;
}

//-----------------------------------------------------------------------------
ASIOError PipeWire2AsioWrapper::createBuffers(ASIOBufferInfo* channelInfos,
                                              long numChannels,
                                              long bufferSize,
                                              ASIOCallbacks* callbacks)
{
    data.asio.callbacks = callbacks;

    for (auto i = 0; i < numChannels; i++)
    {
        ASIO::DoubleBuffer32 double_buf;
        for (auto& buf : double_buf)
        {
            buf.resize(bufferSize, SampleType(0.));
        }

        auto& info = channelInfos[i];
        if (info.isInput == ASIOTrue)
        {
            data.asio.in_bufs.emplace_back(double_buf);
            info.buffers[0] = data.asio.in_bufs[info.channelNum][0].data();
            info.buffers[1] = data.asio.in_bufs[info.channelNum][1].data();
        }
        else
        {
            data.asio.out_bufs.emplace_back(double_buf);
            info.buffers[0] = data.asio.out_bufs[info.channelNum][0].data();
            info.buffers[1] = data.asio.out_bufs[info.channelNum][1].data();
        }
    }

    return ASE_OK;
}

//-----------------------------------------------------------------------------
ASIOError PipeWire2AsioWrapper::disposeBuffers()
{
    data.asio.callbacks = nullptr;
    data.asio.in_bufs.clear();
    data.asio.out_bufs.clear();

    return ASE_OK;
}

//-----------------------------------------------------------------------------
ASIOError PipeWire2AsioWrapper::start()
{
    data.pipewire.thread_loop =
        pw_thread_loop_new_full(data.pipewire.loop, "processing", nullptr);

    constexpr auto NUM_PARAMS = 1;
    const spa_pod* params[NUM_PARAMS];
    SPA_POD_BUILDER_INIT(params, sizeof(params));
    create_filter_params(params);

    pw_thread_loop_lock(data.pipewire.thread_loop);
    pw_thread_loop_start(data.pipewire.thread_loop);
    pw_filter_connect(
        data.filter_node.handle, PW_FILTER_FLAG_RT_PROCESS, params, NUM_PARAMS);
    pw_thread_loop_unlock(data.pipewire.thread_loop);

    return ASE_OK;
}

//-----------------------------------------------------------------------------
ASIOError PipeWire2AsioWrapper::stop()
{
    pw_thread_loop_lock(data.pipewire.thread_loop);
    pw_filter_disconnect(data.filter_node.handle);
    pw_thread_loop_unlock(data.pipewire.thread_loop);
    pw_thread_loop_stop(data.pipewire.thread_loop);
    pw_thread_loop_destroy(data.pipewire.thread_loop);
    data.pipewire.thread_loop = nullptr;
    return ASE_OK;
}

} // namespace p2a

//-----------------------------------------------------------------------------
AsioDriver* getDriver()
{
    return new p2a::PipeWire2AsioWrapper();
}
