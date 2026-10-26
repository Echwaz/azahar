// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#pragma once

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>
#include "common/common_types.h"
#include "network/artic_base/artic_base_client.h"

namespace Network::ArticBase {

struct TitleTarget {
    u64 id;
    u8 media;
};

class RemoteTitleFile {
public:
    RemoteTitleFile(std::shared_ptr<Client> client, s32 handle);
    RemoteTitleFile(const RemoteTitleFile&) = delete;
    RemoteTitleFile& operator=(const RemoteTitleFile&) = delete;
    RemoteTitleFile(RemoteTitleFile&& other) noexcept;
    RemoteTitleFile& operator=(RemoteTitleFile&& other) noexcept;
    ~RemoteTitleFile();

    std::optional<u64> GetSize();
    std::optional<std::size_t> Read(u64 offset, std::span<u8> output);
    void Close();
    s32 ReleaseHandle() {
        return std::exchange(handle, -1);
    }

private:
    std::shared_ptr<Client> client;
    s32 handle{-1};
};

class TitleReader {
public:
    TitleReader(std::shared_ptr<Client> client, TitleTarget target);

    std::optional<std::array<u8, 0x400>> ReadExheader();
    std::optional<std::vector<u8>> ReadExeFS(std::string_view section);
    std::optional<RemoteTitleFile> OpenRomFS();
    std::optional<std::vector<u8>> ReadHomeRomFS();
    static std::optional<std::vector<u8>> ExtractHomeRomFS(std::span<const u8> image);
    static std::optional<std::pair<u64, u64>> InspectRomFS(std::span<const u8> header, u64 total_size);
    std::optional<u64> ReadRomFSChunk(u64 offset, std::span<u8> output, bool update = false);
    std::optional<u64> ReadInsertedGameCard();
    std::optional<std::optional<u64>> ReadGameCardStatus();
    using HomeLayoutDecoder = std::optional<std::vector<u8>> (*)(std::span<const u8>);
    std::optional<std::pair<std::vector<u8>, std::vector<u8>>> ReadHomeLayout(
        HomeLayoutDecoder decoder = nullptr);
    std::optional<u32> GetLastOpenError() const {
        return last_open_error;
    }

private:
    std::optional<RemoteTitleFile> OpenSection(u32 kind, std::string_view section);
    std::optional<std::vector<u8>> ReadHomeNandImage();

    std::shared_ptr<Client> client;
    TitleTarget target;
    std::optional<u32> last_open_error;
};

} // namespace Network::ArticBase
