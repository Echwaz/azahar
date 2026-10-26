// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>
#include "common/swap.h"
#include "common/logging/log.h"
#include "network/artic_base/title_reader.h"

namespace Network::ArticBase {

RemoteTitleFile::RemoteTitleFile(std::shared_ptr<Client> client_, s32 handle_)
    : client(std::move(client_)), handle(handle_) {}

RemoteTitleFile::RemoteTitleFile(RemoteTitleFile&& other) noexcept
    : client(std::move(other.client)), handle(std::exchange(other.handle, -1)) {}

RemoteTitleFile& RemoteTitleFile::operator=(RemoteTitleFile&& other) noexcept {
    if (this != &other) {
        Close();
        client = std::move(other.client);
        handle = std::exchange(other.handle, -1);
    }
    return *this;
}

RemoteTitleFile::~RemoteTitleFile() {
    Close();
}

void RemoteTitleFile::Close() {
    if (handle == -1 || !client) {
        return;
    }
    auto request = client->NewRequest("FSFILE_Close");
    request.AddParameterS32(std::exchange(handle, -1));
    client->Send(request);
}

std::optional<u64> RemoteTitleFile::GetSize() {
    if (handle == -1) {
        return std::nullopt;
    }
    auto request = client->NewRequest("FSFILE_GetSize");
    request.AddParameterS32(handle);
    auto response = client->Send(request);
    if (!response || !response->Succeeded() || response->GetMethodResult() != 0) {
        return std::nullopt;
    }
    return response->GetResponseU64(0);
}

std::optional<std::size_t> RemoteTitleFile::Read(u64 offset, std::span<u8> output) {
    if (handle == -1 || client->GetServerRequestMaxSize() <= 8) {
        return std::nullopt;
    }
    const auto size = GetSize();
    if (!size || offset > *size || *size > static_cast<u64>(std::numeric_limits<s64>::max())) {
        return std::nullopt;
    }
    const auto amount = static_cast<std::size_t>(std::min<u64>(output.size(), *size - offset));
    std::size_t done = 0;
    while (done < amount) {
        const auto part = std::min({amount - done, client->GetServerRequestMaxSize() - 8,
                                    static_cast<std::size_t>(std::numeric_limits<s32>::max())});
        auto request = client->NewRequest("FSFILE_Read");
        request.AddParameterS32(handle);
        request.AddParameterS64(static_cast<s64>(offset + done));
        request.AddParameterS32(static_cast<s32>(part));
        auto response = client->Send(request);
        if (!response || !response->Succeeded() || response->GetMethodResult() != 0) {
            return std::nullopt;
        }
        const auto buffer = response->GetResponseBuffer(0);
        if (!buffer || buffer->second != part) {
            return std::nullopt;
        }
        std::memcpy(output.data() + done, buffer->first, part);
        done += part;
    }
    return done;
}

TitleReader::TitleReader(std::shared_ptr<Client> client_, TitleTarget target_)
    : client(std::move(client_)), target(target_) {}

std::optional<std::vector<u8>> TitleReader::ReadHomeNandImage() {
    LOG_INFO(Network, "HOME layout: opening raw NAND archive for read-only snapshot");
    const auto encode = [](const std::string& name) {
        std::vector<u8> path(8 + name.size() + 1);
        const std::array<u32_le, 2> words{3, static_cast<u32>(name.size() + 1)};
        std::memcpy(path.data(), words.data(), sizeof(words));
        std::memcpy(path.data() + 8, name.c_str(), name.size() + 1);
        return path;
    };
    const std::array<u8, 9> empty_path{1, 0, 0, 0, 1, 0, 0, 0, 0};
    auto open = client->NewRequest("FSUSER_OpenArchive");
    open.AddParameterU32(0x567890AB);
    open.AddParameterBuffer(empty_path.data(), empty_path.size());
    auto opened = client->Send(open);
    if (!opened || !opened->Succeeded() || opened->GetMethodResult() != 0) return std::nullopt;
    const auto archive = opened->GetResponseS64(0);
    if (!archive) return std::nullopt;
    struct ArchiveCloser {
        std::shared_ptr<Client> client;
        s64 handle;
        ~ArchiveCloser() {
            auto close = client->NewRequest("FSUSER_CloseArchive");
            close.AddParameterS64(handle);
            client->Send(close);
        }
    } closer{client, *archive};
    auto directory = client->NewRequest("FSUSER_OpenDirectory");
    directory.AddParameterS64(*archive);
    const auto data_path = encode("/data");
    directory.AddParameterBuffer(data_path.data(), data_path.size());
    auto response = client->Send(directory);
    LOG_INFO(Network, "HOME layout: opened NAND data directory");
    if (!response || !response->Succeeded() || response->GetMethodResult() != 0) return std::nullopt;
    const auto handle = response->GetResponseS32(0);
    if (!handle) return std::nullopt;
    struct DirectoryCloser {
        std::shared_ptr<Client> client;
        s32 handle;
        ~DirectoryCloser() {
            auto close = client->NewRequest("FSDIR_Close");
            close.AddParameterS32(handle);
            client->Send(close);
        }
    } directory_closer{client, *handle};
    std::string identity;
    for (unsigned batch = 0; batch < 16; ++batch) {
        auto read = client->NewRequest("FSDIR_Read");
        read.AddParameterS32(*handle);
        read.AddParameterU32(16);
        auto entries = client->Send(read);
        if (!entries || !entries->Succeeded() || entries->GetMethodResult() != 0) return std::nullopt;
        const auto buffer = entries->GetResponseBuffer(0);
        LOG_INFO(Network, "HOME layout: NAND directory response bytes={}", buffer ? buffer->second : 0);
        if (!buffer || buffer->second % 0x228 || buffer->second > 16 * 0x228) return std::nullopt;
        if (buffer->second == 0) break;
        for (std::size_t offset = 0; offset < buffer->second; offset += 0x228) {
            std::string name;
            for (std::size_t i = 0; i < 33; ++i) {
                u16_le character;
                std::memcpy(&character, static_cast<const u8*>(buffer->first) + offset + i * 2, 2);
                if (character == 0) break;
                if (character > 127) { name.clear(); break; }
                name += static_cast<char>(character);
            }
            LOG_INFO(Network, "HOME layout: directory name length={} hexadecimal={}", name.size(),
                name.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos);
            if (name.size() != 32 || name.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) continue;
            if (!identity.empty()) return std::nullopt;
            identity = name;
        }
        if (batch == 15) return std::nullopt;
    }
    if (identity.empty()) return std::nullopt;
    LOG_INFO(Network, "HOME layout: found one console identity; reading save container");
    const auto file_path = encode("/data/" + identity + "/sysdata/00020082/00000000");
    auto file_open = client->NewRequest("FSUSER_OpenFile");
    file_open.AddParameterS64(*archive);
    file_open.AddParameterBuffer(file_path.data(), file_path.size());
    file_open.AddParameterU32(1);
    file_open.AddParameterU32(0);
    auto result = client->Send(file_open);
    if (!result || !result->Succeeded() || result->GetMethodResult() != 0) return std::nullopt;
    const auto file_handle = result->GetResponseS32(0);
    if (!file_handle) return std::nullopt;
    RemoteTitleFile file(client, *file_handle);
    const auto size = file.GetSize();
    LOG_INFO(Network, "HOME layout: raw container size={}", size.value_or(0));
    if (!size || *size < 512 || *size > 1024 * 1024) return std::nullopt;
    std::vector<u8> image(static_cast<std::size_t>(*size));
    if (file.Read(0, image) != image.size() || std::memcmp(image.data() + 0x100, "DISA", 4) != 0) return std::nullopt;
    return image;
}

std::optional<std::pair<std::vector<u8>, std::vector<u8>>> TitleReader::ReadHomeLayout(
    HomeLayoutDecoder decoder) {
    if (!client || target.id != 0x0004003000008202ULL || target.media != 0) {
        return std::nullopt;
    }
    const auto read = [&](bool nand) -> std::optional<std::vector<u8>> {
        const std::array<u32_le, 5> ext_path{2, 12, 1, 0x82, 0};
        const std::array<u32_le, 4> nand_path{2, 8, 0, 0x20082};
        auto request = client->NewRequest("FSUSER_OpenArchive");
        request.AddParameterU32(nand ? 8 : 6);
        if (nand) request.AddParameterBuffer(nand_path.data(), sizeof(nand_path));
        else request.AddParameterBuffer(ext_path.data(), sizeof(ext_path));
        auto response = client->Send(request);
        if (!response || !response->Succeeded() || response->GetMethodResult() != 0) {
            return std::nullopt;
        }
        const auto archive = response->GetResponseS64(0);
        if (!archive) return std::nullopt;
        struct ArchiveCloser {
            std::shared_ptr<Client> client;
            s64 handle;
            ~ArchiveCloser() {
                auto close = client->NewRequest("FSUSER_CloseArchive");
                close.AddParameterS64(handle);
                client->Send(close);
            }
        } closer{client, *archive};
        const std::string name = nand ? "/Launcher.dat" : "/SaveData.dat";
        std::vector<u8> path(8 + name.size() + 1);
        const std::array<u32_le, 2> header{3, static_cast<u32>(name.size() + 1)};
        std::memcpy(path.data(), header.data(), sizeof(header));
        std::memcpy(path.data() + 8, name.c_str(), name.size() + 1);
        auto open = client->NewRequest("FSUSER_OpenFile");
        open.AddParameterS64(*archive);
        open.AddParameterBuffer(path.data(), path.size());
        open.AddParameterU32(1);
        open.AddParameterU32(0);
        auto opened = client->Send(open);
        if (!opened || !opened->Succeeded() || opened->GetMethodResult() != 0) {
            return std::nullopt;
        }
        const auto handle = opened->GetResponseS32(0);
        if (!handle) return std::nullopt;
        RemoteTitleFile file(client, *handle);
        const auto size = file.GetSize();
        if (!size || *size != (nand ? 0x2558ULL : 0x2DA0ULL)) return std::nullopt;
        std::vector<u8> data(static_cast<std::size_t>(*size));
        if (file.Read(0, data) != data.size()) return std::nullopt;
        return data;
    };
    auto sd = read(false);
    LOG_INFO(Network, "HOME layout: SD layout read {}", sd.has_value());
    if (!sd) return std::nullopt;
    auto nand = read(true);
    LOG_INFO(Network, "HOME layout: system archive read {}", nand.has_value());
    if (!nand && decoder) {
        const auto image = ReadHomeNandImage();
        if (image) nand = decoder(*image);
    }
    if (!nand || nand->size() != 0x2558) return std::nullopt;
    return std::pair{std::move(*sd), std::move(*nand)};
}

std::optional<std::array<u8, 0x400>> TitleReader::ReadExheader() {
    if (!client || target.media > 2 || target.id == 0) {
        return std::nullopt;
    }
    auto request = client->NewRequest("Title_GetExheader");
    request.AddParameterS64(static_cast<s64>(target.id));
    request.AddParameterU8(target.media);
    auto response = client->Send(request);
    if (!response || !response->Succeeded() || response->GetMethodResult() != 0) {
        return std::nullopt;
    }
    const auto buffer = response->GetResponseBuffer(0);
    if (!buffer || buffer->second != 0x400) {
        return std::nullopt;
    }
    std::array<u8, 0x400> result;
    std::memcpy(result.data(), buffer->first, result.size());
    u64_le returned_id;
    std::memcpy(&returned_id, result.data() + 0x200, sizeof(returned_id));
    if (returned_id != target.id) {
        return std::nullopt;
    }
    return result;
}

std::optional<RemoteTitleFile> TitleReader::OpenSection(u32 kind, std::string_view section) {
    last_open_error.reset();
    if (!client || target.media > 2 || target.id == 0 || section.size() > 8) {
        return std::nullopt;
    }
    struct ArchivePath {
        u32_le type{2};
        u32_le size{16};
        u64_le title;
        u32_le media;
        u32_le reserved{};
    } archive;
    struct FilePath {
        u32_le type{2};
        u32_le size{20};
        u32_le open_type{};
        u32_le content_index{};
        u32_le kind;
        std::array<char, 8> name{};
    } file;
    static_assert(sizeof(archive) == 24);
    static_assert(sizeof(file) == 28);
    archive.title = target.id;
    archive.media = target.media;
    file.kind = kind;
    std::copy(section.begin(), section.end(), file.name.begin());
    auto request = client->NewRequest("FSUSER_OpenFileDirectly");
    request.AddParameterS32(0x2345678A);
    request.AddParameterBuffer(&archive, sizeof(archive));
    request.AddParameterBuffer(&file, sizeof(file));
    request.AddParameterS32(1);
    request.AddParameterS32(0);
    auto response = client->Send(request);
    if (!response || !response->Succeeded()) {
        return std::nullopt;
    }
    if (response->GetMethodResult() != 0) {
        last_open_error = static_cast<u32>(response->GetMethodResult());
        return std::nullopt;
    }
    const auto handle = response->GetResponseS32(0);
    if (!handle || *handle == -1) {
        return std::nullopt;
    }
    return RemoteTitleFile(client, *handle);
}

std::optional<std::vector<u8>> TitleReader::ReadExeFS(std::string_view section) {
    auto file = OpenSection(2, section);
    if (!file) {
        return std::nullopt;
    }
    auto size = file->GetSize();
    if (section == ".code") {
        file->Close();
        auto request = client->NewRequest("Title_GetCodeSize");
        request.AddParameterU64(target.id);
        request.AddParameterU8(target.media);
        const auto response = client->Send(request);
        if (response && response->Succeeded() && response->GetMethodResult() == 0) {
            const auto registered_size = response->GetResponseU64(0);
            if (!registered_size || *registered_size == 0) return std::nullopt;
            size = registered_size;
        }
    }
    // The console rejects partial ExeFS reads, including otherwise aligned reads.
    if (!size || client->GetServerRequestMaxSize() <= 8 ||
        (section != ".code" && *size > client->GetServerRequestMaxSize() - 8) ||
        *size > 16 * 1024 * 1024) {
        return std::nullopt;
    }
    std::vector<u8> data(static_cast<std::size_t>(*size));
    if (section == ".code" && *size > client->GetServerRequestMaxSize() - 8) {
        file->Close();
        const auto limit = std::min<std::size_t>(1024 * 1024, client->GetServerRequestMaxSize() - 8);
        for (std::size_t offset = 0; offset < data.size();) {
            const auto amount = std::min(limit, data.size() - offset);
            auto request = client->NewRequest("Title_ReadCodeChunk");
            request.AddParameterU64(target.id);
            request.AddParameterU8(target.media);
            request.AddParameterS32(static_cast<s32>(data.size()));
            request.AddParameterS32(static_cast<s32>(offset));
            request.AddParameterS32(static_cast<s32>(amount));
            const auto response = client->Send(request);
            if (!response || !response->Succeeded() || response->GetMethodResult() != 0) return std::nullopt;
            const auto chunk = response->GetResponseBuffer(0);
            if (!chunk || chunk->second != amount) return std::nullopt;
            std::memcpy(data.data() + offset, chunk->first, amount);
            offset += amount;
        }
        return data;
    }
    if (section == ".code") {
        file->Close();
        auto request = client->NewRequest("Title_ReadCode");
        request.AddParameterU64(target.id);
        request.AddParameterU8(target.media);
        request.AddParameterU32(static_cast<u32>(std::min<std::size_t>(
            4 * 1024 * 1024, client->GetServerRequestMaxSize() - 8)));
        const auto response = client->Send(request);
        if (response && response->Succeeded() && response->GetMethodResult() == 0) {
            const auto code = response->GetResponseBuffer(0);
            if (code && code->second == data.size()) {
                std::memcpy(data.data(), code->first, data.size());
                return data;
            }
        }
    }
    if (section == ".code") {
        file = OpenSection(2, section);
        if (!file || file->GetSize() != size) return std::nullopt;
    }
    const auto read = file->Read(0, data);
    if (!read || *read != data.size()) return std::nullopt;
    return data;
}

std::optional<RemoteTitleFile> TitleReader::OpenRomFS() {
    return OpenSection(0, {});
}

std::optional<std::pair<u64, u64>> TitleReader::InspectRomFS(std::span<const u8> image,
                                                          u64 total_size) {
    if (image.size() < 0x1000 + 0x28 || std::memcmp(image.data(), "IVFC", 4) != 0) {
        return std::nullopt;
    }
    const auto value = [&image](std::size_t offset, std::size_t width) {
        u64 result = 0;
        for (std::size_t i = 0; i < width; ++i) {
            result |= static_cast<u64>(image[offset + i]) << (8 * i);
        }
        return result;
    };
    const u64 size = value(0x44, 8);
    if (value(4, 4) != 0x10000 || value(0x4C, 4) != 12 ||
        value(8, 4) > 0x1000 - 0x60 || size < 0x28 || total_size < 0x1028 ||
        size > total_size - 0x1000 ||
        value(0x1000, 4) != 0x28) {
        return std::nullopt;
    }
    for (std::size_t descriptor = 0; descriptor < 4; ++descriptor) {
        const u64 offset = value(0x1004 + descriptor * 8, 4);
        const u64 length = value(0x1008 + descriptor * 8, 4);
        if (offset < 0x28 || offset > size || length > size - offset) {
            return std::nullopt;
        }
    }
    const u64 data_offset = value(0x1024, 4);
    if (data_offset < 0x28 || data_offset > size) {
        return std::nullopt;
    }
    return std::pair<u64, u64>{0x1000, size};
}

std::optional<std::vector<u8>> TitleReader::ExtractHomeRomFS(std::span<const u8> image) {
    const auto layout = InspectRomFS(image, image.size());
    if (!layout) {
        return std::nullopt;
    }
    return std::vector<u8>(image.begin() + layout->first,
                           image.begin() + layout->first + layout->second);
}

std::optional<u64> TitleReader::ReadRomFSChunk(u64 offset, std::span<u8> output, bool update) {
    if (!client || target.media > 2 || output.empty() || output.size() > 256 * 1024 ||
        client->GetServerRequestMaxSize() <= 24 ||
        output.size() > client->GetServerRequestMaxSize() - 24 ||
        offset > static_cast<u64>(std::numeric_limits<s64>::max())) {
        return std::nullopt;
    }
    auto request = client->NewRequest(update ? "Title_ReadUpdateRomFS" : "Title_ReadRomFS");
    request.AddParameterS64(static_cast<s64>(target.id));
    request.AddParameterU8(target.media);
    request.AddParameterS64(static_cast<s64>(offset));
    request.AddParameterS32(static_cast<s32>(output.size()));
    auto response = client->Send(request);
    if (!response || !response->Succeeded() || response->GetMethodResult() != 0) {
        LOG_ERROR(Network, "RomFS read failed title={:016X} offset={} length={} result={:08X}",
            target.id, offset, output.size(), response ? static_cast<u32>(response->GetMethodResult()) : 0xFFFFFFFFu);
        return std::nullopt;
    }
    const auto data = response->GetResponseBuffer(0);
    const auto size = response->GetResponseU64(1);
    if (!data || data->second != output.size() || !size || offset > *size ||
        output.size() > *size - offset) {
        return std::nullopt;
    }
    std::memcpy(output.data(), data->first, output.size());
    return size;
}

std::optional<u64> TitleReader::ReadInsertedGameCard() {
    const auto status = ReadGameCardStatus();
    return status ? *status : std::nullopt;
}

std::optional<std::optional<u64>> TitleReader::ReadGameCardStatus() {
    if (!client) {
        return std::nullopt;
    }
    auto count_request = client->NewRequest("AM_GetTitleCount");
    count_request.AddParameterU8(2);
    auto count_response = client->Send(count_request);
    constexpr u32 gamecard_not_inserted = 0xC880448D;
    if (count_response && count_response->Succeeded() &&
        static_cast<u32>(count_response->GetMethodResult()) == gamecard_not_inserted)
        return std::optional<std::optional<u64>>(std::in_place, std::nullopt);
    if (!count_response || !count_response->Succeeded() ||
        count_response->GetMethodResult() != 0) return std::nullopt;
    const auto count = count_response->GetResponseS32(0);
    if (!count || (*count != 0 && *count != 1)) return std::nullopt;
    if (*count == 0) return std::optional<std::optional<u64>>(std::in_place, std::nullopt);
    auto request = client->NewRequest("AM_GetTitleList");
    request.AddParameterS32(1);
    request.AddParameterU8(2);
    auto response = client->Send(request);
    if (response && response->Succeeded() &&
        static_cast<u32>(response->GetMethodResult()) == gamecard_not_inserted)
        return std::optional<std::optional<u64>>(std::in_place, std::nullopt);
    if (!response || !response->Succeeded() || response->GetMethodResult() != 0) {
        return std::nullopt;
    }
    const auto title = response->GetResponseU64(0);
    if (!title || (*title >> 32) != 0x00040000) {
        return std::nullopt;
    }
    return title;
}

std::optional<std::vector<u8>> TitleReader::ReadHomeRomFS() {
    if (!client || target.id != 0x0004003000008202ULL || target.media != 0 ||
        client->GetServerRequestMaxSize() < 512 + 24) {
        return std::nullopt;
    }
    std::vector<u8> image;
    std::size_t offset = 0;
    do {
        const auto amount = image.empty() ? std::size_t{512} :
            std::min({image.size() - offset, std::size_t{256 * 1024},
                      client->GetServerRequestMaxSize() - 24});
        auto request = client->NewRequest("Title_ReadHomeRomFS");
        request.AddParameterS64(static_cast<s64>(offset));
        request.AddParameterS32(static_cast<s32>(amount));
        auto response = client->Send(request);
        if (!response || !response->Succeeded() || response->GetMethodResult() != 0) {
            return std::nullopt;
        }
        const auto data = response->GetResponseBuffer(0);
        const auto size = response->GetResponseU64(1);
        if (!data || data->second != amount || !size || *size < 0x1028 ||
            *size > 16 * 1024 * 1024 || (!image.empty() && *size != image.size())) {
            return std::nullopt;
        }
        if (image.empty()) {
            image.resize(static_cast<std::size_t>(*size));
        }
        std::memcpy(image.data() + offset, data->first, amount);
        offset += amount;
    } while (offset < image.size());
    return ExtractHomeRomFS(image);
}

} // namespace Network::ArticBase
