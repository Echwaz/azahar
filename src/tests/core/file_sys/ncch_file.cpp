// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <array>
#include <limits>
#include <catch2/catch_test_macros.hpp>
#include "core/file_sys/archive_ncch.h"
#include "core/file_sys/delay_generator.h"
#include "core/file_sys/ncch_container.h"

TEST_CASE("Buffered NCCH files bound partial reads", "[core][file_sys][artic]") {
    FileSys::NCCHFile file({1, 2, 3, 4}, std::make_unique<FileSys::DefaultDelayGenerator>());
    std::array<u8, 4> output{9, 9, 9, 9};
    const auto tail = file.Read(3, output.size(), output.data());
    REQUIRE(tail.Succeeded());
    REQUIRE(tail.Unwrap() == 1);
    REQUIRE((output == std::array<u8, 4>{4, 9, 9, 9}));
    for (u64 offset : {u64{4}, u64{5}, std::numeric_limits<u64>::max()}) {
        output.fill(9);
        const auto end = file.Read(offset, output.size(), output.data());
        REQUIRE(end.Succeeded());
        REQUIRE(end.Unwrap() == 0);
        REQUIRE((output == std::array<u8, 4>{9, 9, 9, 9}));
    }
}

TEST_CASE("Remote ExeFS decompression respects its allocation bound", "[core][file_sys][artic]") {
    std::vector<u8> output{7, 8, 9};
    const std::vector<u8> original = output;
    REQUIRE_FALSE(FileSys::NCCHContainer::DecompressExeFSCode(std::array<u8, 7>{}, output, 64));
    REQUIRE(output == original);

    std::array<u8, 8> oversized{};
    oversized[4] = oversized[5] = oversized[6] = oversized[7] = 0xFF;
    REQUIRE_FALSE(FileSys::NCCHContainer::DecompressExeFSCode(oversized, output, 64));
    REQUIRE(output == original);

    std::array<u8, 8> invalid_footer{};
    invalid_footer[3] = 0xFF;
    REQUIRE_FALSE(FileSys::NCCHContainer::DecompressExeFSCode(invalid_footer, output, 64));
    REQUIRE(output == original);
}
