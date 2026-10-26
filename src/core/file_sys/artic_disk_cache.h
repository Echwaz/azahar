// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version

#pragma once

#include <span>
#include <string>
#include <vector>
#include "common/common_types.h"

namespace FileSys {

class ArticDiskCache {
public:
    ArticDiskCache(u64 title, u8 media, u64 image_size, std::span<const u8> header,
                    std::size_t data_size);
    bool Read(std::size_t page, std::vector<u8>& output) const;
    void Write(std::size_t page, std::span<const u8> data) const;

private:
    std::string root;
    std::string directory;
    std::size_t data_size;
    static constexpr std::size_t page_size = 65536;
};

} // namespace FileSys
