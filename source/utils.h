// Copyright (c) 2025-present, rehans.

#include <spa/utils/defs.h>
#include <string>
#include <vector>
#include <pipewire/core.h>
#include "types.h"

struct pw_core;
struct pw_main_loop;

namespace p2a {

auto roundtrip(pw_core* core, pw_main_loop* loop) -> void;
auto parse_values_string(const char* valuesStr) -> std::vector<double>;
auto to_map(const struct spa_dict* props) -> p2a::Properties;
auto find_object_name(const struct spa_dict* props) -> const char*;
auto find_object_id(const p2a::Objects& objects,
                    const char* type,
                    const p2a::StringType& name) -> uint32_t;

} // namespace p2a