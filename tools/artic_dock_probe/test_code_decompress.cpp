// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <cassert>
#include "core/file_sys/ncch_container.h"

int main() {
    std::vector<u8> output{7, 8, 9};
    const auto original = output;
    assert(!FileSys::NCCHContainer::DecompressExeFSCode(std::array<u8, 7>{}, output, 64));
    assert(output == original);
    std::array<u8, 8> large{};
    large[4] = large[5] = large[6] = large[7] = 0xFF;
    assert(!FileSys::NCCHContainer::DecompressExeFSCode(large, output, 64));
    assert(output == original);
    std::array<u8, 8> invalid{};
    invalid[3] = 0xFF;
    assert(!FileSys::NCCHContainer::DecompressExeFSCode(invalid, output, 64));
    assert(output == original);
}
