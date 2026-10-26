// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#pragma once

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Loader {

struct ArticHomeAddress {
    std::string host;
    std::uint16_t port{5543};
    std::uint64_t title;
};

inline std::optional<ArticHomeAddress> ParseArticHomeAddress(std::string_view address) {
    constexpr std::string_view prefix = "artichome://";
    if (!address.starts_with(prefix)) {
        return std::nullopt;
    }
    address.remove_prefix(prefix.size());
    const auto slash = address.find('/');
    if (slash == std::string_view::npos || slash == 0) {
        return std::nullopt;
    }
    const auto authority = address.substr(0, slash);
    const auto title_text = address.substr(slash + 1);
    const auto colon = authority.find(':');
    const auto host = authority.substr(0, colon);
    if (host.empty() || host.size() > 253 ||
        !std::all_of(host.begin(), host.end(), [](unsigned char ch) {
            return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                   (ch >= '0' && ch <= '9') || ch == '.' || ch == '-';
        })) {
        return std::nullopt;
    }
    ArticHomeAddress result{std::string(host), 5543, 0};
    if (colon != std::string_view::npos) {
        const auto port_text = authority.substr(colon + 1);
        const auto port = std::from_chars(port_text.data(), port_text.data() + port_text.size(),
                                          result.port);
        if (port.ec != std::errc{} || port.ptr != port_text.data() + port_text.size() ||
            result.port == 0) {
            return std::nullopt;
        }
    }
    if (title_text.size() != 16) {
        return std::nullopt;
    }
    const auto title = std::from_chars(title_text.data(), title_text.data() + title_text.size(),
                                     result.title, 16);
    if (title.ec != std::errc{} || title.ptr != title_text.data() + title_text.size() ||
        (result.title >> 32) != 0x00040030 || (result.title & 0xFFFFFFFF) == 0) {
        return std::nullopt;
    }
    return result;
}

} // namespace Loader
