// Copyright (c) 2025-present, rehans.

#pragma once

#include <functional>
#include <pipewire/core.h>
#include <string>
#include <unordered_map>
#include <vector>

struct pw_filter;

namespace p2a {

using Handle     = void*;
using DataHandle = Handle;
using StringType = std::string;
using Properties = std::unordered_map<StringType, StringType>;

struct Object
{
    uint32_t id = PW_ID_ANY;
    Properties props;
    StringType type;
    uint32_t version     = 0;
    uint32_t permissions = 0;
    StringType name;
};

using Objects = std::vector<Object>;

struct Port
{
    Object obj;

    // The object ID of the Node this Port belongs to.
    uint32_t node_id = PW_ID_ANY;
    Handle handle;
};

using Ports = std::vector<Port>;

struct FilterNode
{
    Object obj;

    pw_filter* handle = nullptr;

    Ports ins;
    Ports outs;
};

struct Settings
{
    using SampleRates = std::vector<double>;

    Object obj;

    double rate           = 0.;
    double force_rate     = 0.;
    int64_t quantum       = 0;
    int64_t min_quantum   = 0;
    int64_t max_quantum   = 0;
    int64_t force_quantum = 0;
    SampleRates allowed_rates;
};

using SampleType = float;
using StringType = std::string;

struct ChannelInfo
{
    StringType name;
};
using ChannelInfos = std::vector<ChannelInfo>;

} // namespace p2a