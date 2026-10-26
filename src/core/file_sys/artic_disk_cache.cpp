// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>
#include <cryptopp/sha.h>
#include <fmt/format.h>
#include "common/file_util.h"
#include "common/swap.h"
#include "core/file_sys/artic_disk_cache.h"

namespace FileSys {
namespace {
constexpr std::array<u8, 8> magic{'A', 'R', 'T', 'P', 'G', '0', '0', '1'};
constexpr u64 budget = 1024ULL * 1024 * 1024;
std::mutex quota_mutex;
std::map<std::string, u64> quota_sizes;

std::vector<std::filesystem::directory_entry> Pages(const std::string& root) {
    std::vector<std::filesystem::directory_entry> pages;
    std::error_code error;
    auto iterator = std::filesystem::recursive_directory_iterator(root,
        std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::recursive_directory_iterator end;
    while (!error && iterator != end) {
        if (iterator->is_regular_file(error) && !iterator->is_symlink(error) &&
            iterator->path().extension() == ".page") pages.push_back(*iterator);
        iterator.increment(error);
    }
    return pages;
}

u64 FileSize(const std::string& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(std::filesystem::u8path(path), error);
    return error ? 0 : size;
}
} // namespace

ArticDiskCache::ArticDiskCache(u64 title, u8 media, u64 image_size,
                               std::span<const u8> header, std::size_t size)
    : data_size(size) {
    const auto* override_root = std::getenv("ARTIC_ROMFS_CACHE_DIR");
    root = override_root && *override_root ? std::string(override_root) + "/artic-romfs/" :
        FileUtil::GetUserPath(FileUtil::UserPath::CacheDir) + "artic-romfs/";
    std::array<u8, CryptoPP::SHA256::DIGESTSIZE> digest{};
    CryptoPP::SHA256 hash;
    hash.CalculateDigest(digest.data(), header.data(), header.size());
    std::string fingerprint;
    for (u8 byte : digest) fingerprint += fmt::format("{:02x}", byte);
    directory = fmt::format("{}{:016x}-{}-{:x}-{}/", root, title, media, image_size, fingerprint);
    if (!FileUtil::CreateFullPath(directory)) { directory.clear(); return; }
    std::scoped_lock lock(quota_mutex);
    if (!quota_sizes.contains(root)) {
        u64 total = 0;
        for (const auto& page : Pages(root)) total += FileSize(page.path().string());
        quota_sizes[root] = total;
    }
}

bool ArticDiskCache::Read(std::size_t page, std::vector<u8>& output) const {
    if (directory.empty() || page % page_size || page >= data_size) return false;
    const auto path = fmt::format("{}{:016x}.page", directory, page);
    const auto expected = std::min(page_size, data_size - page);
    if (!FileUtil::Exists(path)) return false;
    FileUtil::IOFile file(path, "rb");
    if (!file.IsOpen() || file.GetSize() != 52 + expected) return false;
    std::array<u8, 8> tag{};
    u64_le offset{};
    u32_le size{};
    std::array<u8, 32> saved_hash{};
    if (file.ReadBytes(tag.data(), tag.size()) != tag.size() ||
        file.ReadBytes(&offset, sizeof(offset)) != sizeof(offset) ||
        file.ReadBytes(&size, sizeof(size)) != sizeof(size) ||
        file.ReadBytes(saved_hash.data(), saved_hash.size()) != saved_hash.size() ||
        tag != magic || offset != page || size != expected) return false;
    std::vector<u8> data(expected);
    if (file.ReadBytes(data.data(), data.size()) != data.size()) return false;
    std::array<u8, 32> digest{};
    CryptoPP::SHA256 hash;
    hash.CalculateDigest(digest.data(), data.data(), data.size());
    if (digest != saved_hash) return false;
    output = std::move(data);
    return true;
}

void ArticDiskCache::Write(std::size_t page, std::span<const u8> data) const {
    if (directory.empty() || page % page_size || page >= data_size ||
        data.size() != std::min(page_size, data_size - page)) return;
    const auto path = fmt::format("{}{:016x}.page", directory, page);
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto temporary = fmt::format("{}.tmp-{}-{}", path, nonce,
        std::hash<std::thread::id>{}(std::this_thread::get_id()));
    std::array<u8, 32> digest{};
    CryptoPP::SHA256 hash;
    hash.CalculateDigest(digest.data(), data.data(), data.size());
    const u64_le offset = page;
    const u32_le size = static_cast<u32>(data.size());
    bool complete = false;
    {
        FileUtil::IOFile file(temporary, "wb");
        complete = file.IsOpen() && file.WriteBytes(magic.data(), magic.size()) == magic.size() &&
            file.WriteBytes(&offset, sizeof(offset)) == sizeof(offset) &&
            file.WriteBytes(&size, sizeof(size)) == sizeof(size) &&
            file.WriteBytes(digest.data(), digest.size()) == digest.size() &&
            file.WriteBytes(data.data(), data.size()) == data.size() && file.Flush();
    }
    std::scoped_lock lock(quota_mutex);
    const auto previous = FileSize(path);
    if (!complete || !FileUtil::Rename(temporary, path)) {
        std::error_code error;
        std::filesystem::remove(std::filesystem::u8path(temporary), error);
        return;
    }
    auto& total = quota_sizes[root];
    total = total >= previous ? total - previous : 0;
    total += data.size() + 52;
    if (total <= budget) return;
    auto pages = Pages(root);
    std::sort(pages.begin(), pages.end(), [](const auto& left, const auto& right) {
        std::error_code first_error, second_error;
        return left.last_write_time(first_error) < right.last_write_time(second_error);
    });
    for (const auto& entry : pages) {
        if (total <= budget * 9 / 10) break;
        const auto bytes = FileSize(entry.path().string());
        std::error_code error;
        if (std::filesystem::remove(entry.path(), error)) total = total >= bytes ? total - bytes : 0;
    }
}
} // namespace FileSys
