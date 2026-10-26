// Copyright 2018-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <vector>
#include <cryptopp/aes.h>
#include <cryptopp/modes.h>
#include "common/archives.h"
#include "common/logging/log.h"
#include "core/file_sys/archive_artic.h"
#include "core/file_sys/archive_backend.h"
#include "core/file_sys/romfs_reader.h"
#include "core/hle/service/fs/fs_user.h"
#include "core/loader/loader.h"

SERIALIZE_EXPORT_IMPL(FileSys::DirectRomFSReader)

namespace FileSys {

std::size_t DirectRomFSReader::ReadFile(std::size_t offset, std::size_t length, u8* buffer) {
    length = std::min(length, GetSize() - offset);
    if (length == 0)
        return 0; // Crypto++ does not like zero size buffer

    const auto segments = BreakupRead(offset, length);
    std::size_t read_progress = 0;

    // Skip cache if the read is too big
    if (segments.size() == 1 && segments[0].second > cache_line_size) {
        length = file->ReadAtBytes(buffer, length, offset);
        LOG_TRACE(Service_FS, "RomFS Cache SKIP: offset={}, length={}", offset, length);
        return length;
    }

    // TODO(PabloMK7): Make cache thread safe, read the comment in CacheReady function.
    // std::unique_lock<std::shared_mutex> read_guard(cache_mutex);
    for (const auto& seg : segments) {
        std::size_t read_size = cache_line_size;
        std::size_t page = OffsetToPage(seg.first);
        // Check if segment is in cache
        auto cache_entry = cache.request(page);
        if (!cache_entry.first) {
            // If not found, read from disk and cache the data
            read_size = file->ReadAtBytes(cache_entry.second.data(), read_size, page);
            LOG_TRACE(Service_FS, "RomFS Cache MISS: page={}, length={}, into={}", page, seg.second,
                      (seg.first - page));
        } else {
            LOG_TRACE(Service_FS, "RomFS Cache HIT: page={}, length={}, into={}", page, seg.second,
                      (seg.first - page));
        }
        std::size_t copy_amount =
            (read_size > (seg.first - page))
                ? std::min((seg.first - page) + seg.second, read_size) - (seg.first - page)
                : 0;
        std::memcpy(buffer + read_progress, cache_entry.second.data() + (seg.first - page),
                    copy_amount);
        read_progress += copy_amount;
    }
    return read_progress;
}

bool DirectRomFSReader::AllowsCachedReads() const {
    return true;
}

bool DirectRomFSReader::CacheReady(std::size_t file_offset, std::size_t length) {
    auto segments = BreakupRead(file_offset, length);
    if (segments.size() == 1 && segments[0].second > cache_line_size) {
        return false;
    } else {
        // TODO(PabloMK7): Since the LRU cache is not thread safe, a lock must be used.
        // However, this completely breaks the point of using a cache, because
        // smaller reads may be blocked by bigger reads. For now, always return
        // data being in cache to prevent the need of a lock, and only read data
        // asynchronously if it is too big to use the cache.
        /*
        std::shared_lock<std::shared_mutex> read_guard(cache_mutex);
        for (auto it = segments.begin(); it != segments.end(); it++) {
            if (!cache.contains(OffsetToPage(it->first)))
                return false;
        }
        */
        return true;
    }
}

std::vector<std::pair<std::size_t, std::size_t>> DirectRomFSReader::BreakupRead(
    std::size_t offset, std::size_t length) {

    std::vector<std::pair<std::size_t, std::size_t>> ret;

    // Reads bigger than the cache line size will probably never hit again
    if (length > cache_line_size) {
        ret.push_back(std::make_pair(offset, length));
        return ret;
    }

    std::size_t curr_offset = offset;
    while (length) {
        std::size_t next_page = OffsetToPage(curr_offset + cache_line_size);
        std::size_t curr_page_len = std::min(length, next_page - curr_offset);
        ret.push_back(std::make_pair(curr_offset, curr_page_len));
        curr_offset = next_page;
        length -= curr_page_len;
    }
    return ret;
}

ArticRomFSReader::ArticRomFSReader(std::shared_ptr<Network::ArticBase::Client>& cli,
                                   bool is_update_romfs,
                                   std::optional<Network::ArticBase::TitleTarget> title)
    : client(cli), cache(cli) {
    if (title) {
        if (is_update_romfs && (title->id >> 32) != 0x00040000) {
            load_status = Loader::ResultStatus::ErrorNotUsed;
            return;
        }
        remote_update = is_update_romfs;
        Network::ArticBase::TitleReader reader(cli, *title);
        if (title->id == 0x0004003000008202ULL && title->media == 0) {
            auto data = reader.ReadHomeRomFS();
            if (!data) {
                load_status = Loader::ResultStatus::Error;
                return;
            }
            home_romfs = std::move(*data);
            data_size = home_romfs.size();
            load_status = Loader::ResultStatus::Success;
            return;
        }
        std::array<u8, 0x1028> header{};
        const auto total = reader.ReadRomFSChunk(0, header, remote_update);
        const auto layout = total ? Network::ArticBase::TitleReader::InspectRomFS(header, *total) :
                                    std::nullopt;
        if (!layout || layout->second > std::numeric_limits<std::size_t>::max()) {
            load_status = Loader::ResultStatus::Error;
            return;
        }
        remote_title = title;
        remote_size = *total;
        remote_offset = static_cast<std::size_t>(layout->first);
        data_size = static_cast<std::size_t>(layout->second);
        const auto* disable_disk = std::getenv("ARTIC_DISABLE_DISK_CACHE");
        if (!disable_disk || std::string_view(disable_disk) != "1") {
            disk_cache = std::make_unique<ArticDiskCache>(remote_update ? (title->id | 0xE00000000ULL) : title->id, title->media, *total, header, data_size);
        }
        const auto* disable_prefetch = std::getenv("ARTIC_DISABLE_PREFETCH");
        if (!disable_prefetch || std::string_view(disable_prefetch) != "1") {
            prefetch_thread = std::thread([this] { PrefetchLoop(); });
        }
        load_status = Loader::ResultStatus::Success;
        return;
    }
    auto req = client->NewRequest("FSUSER_OpenFileDirectly");

    FileSys::Path archive(FileSys::LowPathType::Empty, {});
    std::vector<u8> fileVec(0xC);
    fileVec[0] = static_cast<u8>(is_update_romfs ? 5 : 0);
    FileSys::Path file(FileSys::LowPathType::Binary, fileVec);

    req.AddParameterS32(static_cast<s32>(Service::FS::ArchiveIdCode::SelfNCCH));

    auto archive_buf = ArticArchive::BuildFSPath(archive);
    req.AddParameterBuffer(archive_buf.data(), archive_buf.size());
    auto file_buf = ArticArchive::BuildFSPath(file);
    req.AddParameterBuffer(file_buf.data(), file_buf.size());

    req.AddParameterS32(1);
    req.AddParameterS32(0);

    auto resp = client->Send(req);

    if (!resp.has_value() || !resp->Succeeded()) {
        load_status = Loader::ResultStatus::Error;
        return;
    }
    if (resp->GetMethodResult() != 0) {
        load_status = Loader::ResultStatus::ErrorNotUsed;
        return;
    }

    auto handle_buf = resp->GetResponseBuffer(0);
    if (!handle_buf.has_value() || handle_buf->second != sizeof(s32)) {
        load_status = Loader::ResultStatus::Error;
        return;
    }

    romfs_handle = *reinterpret_cast<s32*>(handle_buf->first);

    req = client->NewRequest("FSFILE_GetSize");

    req.AddParameterS32(romfs_handle);

    resp = client->Send(req);

    if (!resp.has_value() || !resp->Succeeded()) {
        load_status = Loader::ResultStatus::Error;
        return;
    }
    if (resp->GetMethodResult() != 0) {
        load_status = Loader::ResultStatus::ErrorNotUsed;
        return;
    }

    auto size_buf = resp->GetResponseBuffer(0);
    if (!size_buf.has_value() || size_buf->second != sizeof(u64)) {
        load_status = Loader::ResultStatus::Error;
        return;
    }

    data_size = static_cast<size_t>(*reinterpret_cast<u64*>(size_buf->first));
    load_status = Loader::ResultStatus::Success;
}

ArticRomFSReader::~ArticRomFSReader() {
    StopPrefetch();
    if (remote_title) {
        LOG_INFO(Service_FS, "Artic RomFS {:016X}: network_reads={} RAM_hits={} SSD_hits={} prefetched={}",
            remote_title->id, network_reads.load(), ram_hits.load(), disk_hits.load(), prefetched_pages.load());
    }
    if (romfs_handle != -1) {
        auto req = client->NewRequest("FSFILE_Close");
        req.AddParameterS32(romfs_handle);
        client->Send(req);
        romfs_handle = -1;
    }
}

std::size_t ArticRomFSReader::ReadFile(std::size_t offset, std::size_t length, u8* buffer) {
    if (offset >= data_size) {
        return 0;
    }
    length = std::min(length, static_cast<std::size_t>(data_size) - offset);
    if (length == 0) {
        return 0;
    }
    if (!home_romfs.empty()) {
        std::memcpy(buffer, home_romfs.data() + offset, length);
        return length;
    }
    if (remote_title) {
        if (remote_invalidated) return 0;
        std::size_t done = 0;
        while (done < length) {
            if (remote_invalidated) return 0;
            const auto page = Common::AlignDown(offset + done, remote_page_size);
            {
                std::scoped_lock lock(remote_mutex);
                if (remote_pages.contains(page)) {
                    auto& data = remote_pages.request(page).second;
                    const auto within = offset + done - page;
                    const auto amount = std::min(length - done, data.size() - within);
                    std::memcpy(buffer + done, data.data() + within, amount);
                    done += amount;
                    ++ram_hits;
                    continue;
                }
            }
            std::vector<u8> disk_page;
            if (disk_cache && disk_cache->Read(page, disk_page)) {
                std::scoped_lock lock(remote_mutex);
                remote_pages.request(page).second = std::move(disk_page);
                ++disk_hits;
                continue;
            }
            ++foreground_waiters;
            std::unique_lock io_lock(remote_io_mutex);
            --foreground_waiters;
            if (remote_invalidated) return 0;
            {
                std::scoped_lock lock(remote_mutex);
                if (remote_pages.contains(page)) continue;
            }
            const auto remaining = length - done + (offset + done - page);
            const auto request_limit = client->GetServerRequestMaxSize();
            const auto batch_limit = request_limit > 24 ?
                std::min<std::size_t>(256 * 1024, request_limit - 24) : 0;
            auto count = std::min(remote_page_size, data_size - page);
            if (remaining >= 2 * remote_page_size && batch_limit >= 2 * remote_page_size) {
                count = std::min({Common::AlignUp(remaining, remote_page_size),
                    Common::AlignDown(batch_limit, remote_page_size), data_size - page});
            }
            if (!FetchRemotePages(page, count)) return 0;
        }
        const auto end = offset + length;
        const auto previous_end = last_read_end.exchange(end);
        if (length >= 16384 || offset == previous_end ||
            end % remote_page_size >= remote_page_size * 3 / 4) {
            QueuePrefetch(Common::AlignDown(end - 1, remote_page_size) + remote_page_size);
        }
        return done;
    }
    auto res = cache.Read(romfs_handle, offset, length, buffer);
    if (res.Failed())
        return 0;
    return res.Unwrap();
}

bool ArticRomFSReader::FetchRemotePages(std::size_t page, std::size_t count) {
    Network::ArticBase::TitleReader reader(client, *remote_title);
    std::vector<u8> batch(count);
    ++network_reads;
    const auto total = reader.ReadRomFSChunk(remote_offset + page, batch, remote_update);
    if (!total || *total != remote_size) {
        remote_invalidated = true;
        std::scoped_lock lock(remote_mutex);
        remote_pages.clear();
        return false;
    }
    for (std::size_t position = 0; position < count; position += remote_page_size) {
        const auto size = std::min(remote_page_size, count - position);
        const auto bytes = std::span<const u8>(batch).subspan(position, size);
        {
            std::scoped_lock lock(remote_mutex);
            auto& entry = remote_pages.request(page + position).second;
            entry.assign(bytes.begin(), bytes.end());
        }
        if (disk_cache) disk_cache->Write(page + position, bytes);
    }
    return true;
}

void ArticRomFSReader::QueuePrefetch(std::size_t page) {
    if (!prefetch_thread.joinable() || page >= data_size || prefetch_stopped || remote_invalidated) return;
    {
        std::scoped_lock lock(prefetch_mutex);
        prefetch_page = page;
    }
    prefetch_ready.notify_one();
}

void ArticRomFSReader::PrefetchLoop() {
    while (!prefetch_stopped) {
        std::size_t page;
        {
            std::unique_lock lock(prefetch_mutex);
            prefetch_ready.wait(lock, [this] { return prefetch_stopped || prefetch_page.has_value(); });
            if (prefetch_stopped) return;
            page = *prefetch_page;
            prefetch_page.reset();
        }
        if (foreground_waiters || remote_invalidated) continue;
        std::unique_lock io_lock(remote_io_mutex, std::try_to_lock);
        if (!io_lock.owns_lock() || foreground_waiters || prefetch_stopped) continue;
        {
            std::scoped_lock lock(remote_mutex);
            if (remote_pages.contains(page)) continue;
        }
        std::vector<u8> data;
        if (disk_cache && disk_cache->Read(page, data)) {
            std::scoped_lock lock(remote_mutex);
            remote_pages.request(page).second = std::move(data);
            ++disk_hits;
        } else {
            if (FetchRemotePages(page, std::min(remote_page_size, data_size - page))) {
                ++prefetched_pages;
            }
        }
    }
}

void ArticRomFSReader::StopPrefetch() {
    {
        std::scoped_lock lock(prefetch_mutex);
        prefetch_stopped = true;
        prefetch_page.reset();
    }
    prefetch_ready.notify_all();
    if (prefetch_thread.joinable()) prefetch_thread.join();
}

bool ArticRomFSReader::AllowsCachedReads() const {
    return true;
}

bool ArticRomFSReader::CacheReady(std::size_t file_offset, std::size_t length) {
    if (!home_romfs.empty()) {
        return file_offset <= data_size && length <= data_size - file_offset;
    }
    if (remote_title) {
        std::scoped_lock lock(remote_mutex);
        if (file_offset > data_size || length > data_size - file_offset) {
            return false;
        }
        if (length == 0) {
            return true;
        }
        const auto end = file_offset + length;
        for (auto page = Common::AlignDown(file_offset, remote_page_size); page < end;
             page += remote_page_size) {
            if (!remote_pages.contains(page)) {
                return false;
            }
        }
        return true;
    }
    return cache.CacheReady(file_offset, length);
}

void ArticRomFSReader::CloseFile() {
    if (romfs_handle != -1) {
        auto req = client->NewRequest("FSFILE_Close");
        req.AddParameterS32(romfs_handle);
        client->Send(req);
        romfs_handle = -1;
    }
}

} // namespace FileSys
