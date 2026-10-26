// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <cassert>
#include "core/loader/artic_home_address.h"

int main() {
    const auto address = Loader::ParseArticHomeAddress(
        "artichome://192.0.2.1:5543/0004003000008202");
    assert(address && address->host == "192.0.2.1" && address->port == 5543 &&
           address->title == 0x0004003000008202);
    const auto default_port =
        Loader::ParseArticHomeAddress("artichome://console.local/0004003000008f02");
    assert(default_port && default_port->port == 5543);
    const char* invalid[] = {
        "articbase://192.0.2.1/0004003000008202",
        "artichome:///0004003000008202",
        "artichome://console.local:/0004003000008202",
        "artichome://console.local:0/0004003000008202",
        "artichome://console.local:65536/0004003000008202",
        "artichome://console.local:12x/0004003000008202",
        "artichome://console.local/0004000000055D00",
        "artichome://console.local/0004003000000000",
        "artichome://console.local/0004003000008202/extra",
        "artichome://console.local/0004003000008202?extra",
        "artichome://console.local/000400300000820g",
        "artichome://user@console.local/0004003000008202",
        "artichome://[::1]/0004003000008202",
    };
    for (const auto* value : invalid) {
        assert(!Loader::ParseArticHomeAddress(value));
    }
}
