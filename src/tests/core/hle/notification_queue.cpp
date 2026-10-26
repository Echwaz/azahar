// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
#include <catch2/catch_test_macros.hpp>
#include "core/hle/service/sm/notification_queue.h"

TEST_CASE("Notifications preserve cartridge edges and isolate subscribers", "[core][srv]") {
    Service::SM::NotificationQueue home, application;
    home.Subscribe(0x208); home.Subscribe(0x20A); home.Subscribe(0x20B);
    application.Subscribe(0x207);
    REQUIRE(home.Push(0x20A)); REQUIRE(home.Push(0x20B)); REQUIRE(home.Push(0x208));
    REQUIRE_FALSE(home.Push(0x20B));
    REQUIRE_FALSE(application.Push(0x208));
    REQUIRE(home.Pop() == 0x20Au); REQUIRE(home.Pop() == 0x20Bu); REQUIRE(home.Pop() == 0x208u);
    REQUIRE_FALSE(home.Pop()); REQUIRE_FALSE(application.Pop());
    home.Unsubscribe(0x208); REQUIRE_FALSE(home.Push(0x208));
}

TEST_CASE("Notification queues reject overflow and accept new events after draining", "[core][srv]") {
    Service::SM::NotificationQueue queue;
    for (u32 i = 0; i < 17; ++i) queue.Subscribe(i);
    for (u32 i = 0; i < 16; ++i) REQUIRE(queue.Push(i));
    REQUIRE_FALSE(queue.Push(16)); REQUIRE(queue.Size() == 16);
    REQUIRE(queue.Pop() == 0u); REQUIRE(queue.Push(16));
    for (u32 i = 1; i < 17; ++i) REQUIRE(queue.Pop() == i);
    REQUIRE_FALSE(queue.Pop());
}
