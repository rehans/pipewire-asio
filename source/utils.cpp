// Copyright (c) 2025-present, rehans.

#include "utils.h"
#include <pipewire/extensions/metadata.h>
#include <pipewire/pipewire.h>
#include <regex>
#include <spa/param/audio/format-utils.h>
#include <string>

namespace p2a {

/* [roundtrip] */
struct roundtrip_data
{
    int pending;
    struct pw_main_loop* loop;
};

static auto on_core_done(void* data, uint32_t id, int seq) -> void
{
    struct roundtrip_data* d = static_cast<roundtrip_data*>(data);

    if (id == PW_ID_CORE && seq == d->pending)
        pw_main_loop_quit(d->loop);
}

auto roundtrip(struct pw_core* core, struct pw_main_loop* loop) -> void
{
    static const struct pw_core_events core_events = {
        PW_VERSION_CORE_EVENTS,
        .done = on_core_done,
    };

    struct roundtrip_data d = {.loop = loop};
    struct spa_hook core_listener;
    int err;

    pw_core_add_listener(core, &core_listener, &core_events, &d);

    d.pending = pw_core_sync(core, PW_ID_CORE, 0);

    if ((err = pw_main_loop_run(loop)) < 0)
        printf("main_loop_run error:%d!\n", err);

    spa_hook_remove(&core_listener);
}
/* [roundtrip] */

auto parse_values_string(const char* valuesStr) -> std::vector<double>
{
    std::string input(valuesStr);

    std::regex regex(R"(\d+)");
    std::sregex_iterator it(input.begin(), input.end(), regex);
    std::sregex_iterator end;

    std::vector<double> values;
    while (it != end)
    {
        values.push_back(std::stod(it->str()));
        ++it;
    }

    return values;
}

auto to_map(const struct spa_dict* props) -> p2a::Properties
{
    p2a::Properties umap;

    const spa_dict_item* item{};
    spa_dict_for_each(item, props)
    {
        umap.emplace(item->key, item->value);
    }

    return umap;
}

auto find_object_name(const struct spa_dict* props) -> const char*
{
    static const char* const name_keys[] = {
        PW_KEY_NODE_NAME,
        PW_KEY_PORT_NAME,
        PW_KEY_CLIENT_NAME,
        PW_KEY_DEVICE_NAME,
        PW_KEY_METADATA_NAME,
    };

    SPA_FOR_EACH_ELEMENT_VAR(name_keys, key)
    {
        const char* name = spa_dict_lookup(props, *key);
        if (name)
            return name;
    }

    return nullptr;
}

auto find_object_id(const Objects& objects,
                    const char* type,
                    const StringType& name) -> uint32_t
{
    auto iter = std::find_if(
        objects.begin(), objects.end(), [type, name](const auto& obj) {
            return obj.type == type && obj.name == name;
        });

    if (iter == objects.end())
        return PW_ID_ANY;

    return iter->id;
}

} // namespace p2a