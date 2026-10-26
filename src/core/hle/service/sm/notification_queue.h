// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
#pragma once
#include <deque>
#include <optional>
#include <set>
#include <algorithm>
#include "common/common_types.h"

namespace Service::SM {
class NotificationQueue {
public:
    void Subscribe(u32 id) { subscriptions.insert(id); }
    void Unsubscribe(u32 id) { subscriptions.erase(id); }
    bool Push(u32 id, bool once = true) {
        if (!subscriptions.contains(id) || pending.size() >= 16 ||
            (once && std::find(pending.begin(), pending.end(), id) != pending.end())) return false;
        pending.push_back(id);
        return true;
    }
    std::optional<u32> Pop() {
        if (pending.empty()) return std::nullopt;
        const auto id = pending.front(); pending.pop_front(); return id;
    }
    std::size_t Size() const { return pending.size(); }
    template <class Archive> void serialize(Archive& ar, const unsigned int) {
        ar & subscriptions; ar & pending;
    }
private:
    std::set<u32> subscriptions;
    std::deque<u32> pending;
};
} // namespace Service::SM
