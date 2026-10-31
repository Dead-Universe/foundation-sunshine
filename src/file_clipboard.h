#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include "src/nvhttp.h"
namespace file_clipboard {
void session_started(std::uint32_t id, const std::string &paired_uuid);
void session_stopped(std::uint32_t id);
void register_routes(web::Server<nvhttp::SunshineHTTPS> &server, std::function<bool(std::string_view)> paired);
void stop();
}
