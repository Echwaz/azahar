// Copyright 2014-2024 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <fmt/format.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <functional>
#include "common/archives.h"
#include "common/file_util.h"
#include "common/logging/log.h"
#include "common/string_util.h"
#include "common/swap.h"
#include "core/file_sys/archive_artic.h"
#include "core/file_sys/archive_source_sd_savedata.h"
#include "core/file_sys/errors.h"
#include "core/file_sys/savedata_archive.h"
#include "core/hle/service/fs/archive.h"

SERIALIZE_EXPORT_IMPL(FileSys::ArchiveSource_SDSaveData)

namespace FileSys {

namespace {

std::string GetSaveDataContainerPath(const std::string& sdmc_directory) {
    return fmt::format("{}Nintendo 3DS/{}/{}/title/", sdmc_directory, SYSTEM_ID, SDCARD_ID);
}

std::string GetSaveDataPath(const std::string& mount_location, u64 program_id) {
    u32 high = static_cast<u32>(program_id >> 32);
    u32 low = static_cast<u32>(program_id & 0xFFFFFFFF);
    return fmt::format("{}{:08x}/{:08x}/data/00000001/", mount_location, high, low);
}

std::string GetSaveDataMetadataPath(const std::string& mount_location, u64 program_id) {
    u32 high = static_cast<u32>(program_id >> 32);
    u32 low = static_cast<u32>(program_id & 0xFFFFFFFF);
    return fmt::format("{}{:08x}/{:08x}/data/00000001.metadata", mount_location, high, low);
}

Path DockSavePath(u64 title, u8 media) {
    const std::array<u32_le, 3> words{media, static_cast<u32>(title), static_cast<u32>(title >> 32)};
    const auto* begin = reinterpret_cast<const u8*>(words.data());
    return Path(std::vector<u8>(begin, begin + sizeof(words)));
}

Path UnicodePath(const std::string& name) {
    const auto text = Common::UTF8ToUTF16(name);
    std::vector<u8> bytes((text.size() + 1) * sizeof(char16_t));
    std::memcpy(bytes.data(), text.data(), text.size() * sizeof(char16_t));
    return Path(LowPathType::Wchar, std::move(bytes));
}

bool BackupDockSave(ArchiveBackend& archive, u64 title, bool extdata = false, u64 id = 0) {
    const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
    const auto destination = extdata
        ? fmt::format("{}dock-extdata-backups/{:016X}/{:016X}/{}/", FileUtil::GetUserPath(FileUtil::UserPath::UserDir), title, id, stamp)
        : fmt::format("{}dock-save-backups/{:016X}/{}/", FileUtil::GetUserPath(FileUtil::UserPath::UserDir), title, stamp);
    if (FileUtil::Exists(destination) || !FileUtil::CreateFullPath(destination)) return false;
    std::size_t entries_seen = 0;
    u64 bytes_seen = 0;
    std::vector<u8> buffer(65536);
    std::function<bool(const std::string&, unsigned)> visit;
    visit = [&](const std::string& path, unsigned depth) {
        if (depth > 16) return false;
        auto directory = archive.OpenDirectory(UnicodePath(path));
        if (directory.Failed()) return false;
        auto* remote = dynamic_cast<ArticDirectoryBackend*>(directory->get());
        if (!remote) return false;
        std::vector<Entry> entries;
        while (true) {
            std::array<Entry, 16> batch{};
            const auto count = remote->Read(static_cast<u32>(batch.size()), batch.data());
            if (remote->ReadFailed()) return false;
            if (count == 0) break;
            entries_seen += count;
            if (entries_seen > 4096) return false;
            entries.insert(entries.end(), batch.begin(), batch.begin() + count);
        }
        if (!remote->Close()) return false;
        for (const auto& entry : entries) {
            const auto end = std::find(std::begin(entry.filename), std::end(entry.filename), u'\0');
            if (end == std::end(entry.filename)) return false;
            const auto name = Common::UTF16ToUTF8(std::u16string(entry.filename, end));
            if (name.empty() || name == "." || name == ".." ||
                name.find_first_of("/\\:*?\"<>|") != std::string::npos ||
                name.back() == '.' || name.back() == ' ') return false;
            const auto full = path + name;
            const auto local = destination + full.substr(1);
            if (FileUtil::Exists(local)) return false;
            if (entry.is_directory) {
                if (!FileUtil::CreateFullPath(local + "/") || !visit(full + "/", depth + 1)) return false;
                continue;
            }
            auto file = archive.OpenFile(UnicodePath(full), Mode::ReadOnly());
            if (file.Failed()) return false;
            const auto file_size = (*file)->GetSize();
            if ((entry.file_size != 0 && file_size != entry.file_size) ||
                file_size > 64 * 1024 * 1024ULL - bytes_seen) return false;
            bytes_seen += file_size;
            FileUtil::IOFile output(local, "wb");
            for (u64 offset = 0; offset < file_size;) {
                const auto amount = static_cast<std::size_t>(
                    std::min<u64>(buffer.size(), file_size - offset));
                const auto read = (*file)->Read(offset, amount, buffer.data());
                if (read.Failed() || *read != amount ||
                    output.WriteBytes(buffer.data(), amount) != amount) return false;
                offset += amount;
            }
            if (!output.Flush() || !(*file)->Close()) return false;
        }
        return true;
    };
    if (!visit("/", 0)) return false;
    const auto secure = extdata ? ResultVal<std::tuple<bool, bool, u64>>(ResultUnknown) : archive.GetSaveDataSecureValue(0x1000);
    std::string metadata;
    if (extdata) {
        metadata = fmt::format("title={:016X}\nextdata={:016X}\n", title, id);
    } else if (secure.Succeeded()) {
        metadata = fmt::format("title={:016X}\nsecure_value_exists={}\nis_gamecard={}\nsecure_value={:016X}\n",
            title, std::get<0>(*secure), std::get<1>(*secure), std::get<2>(*secure));
    } else {
        metadata = fmt::format("title={:016X}\nsecure_value_result={:08X}\n", title, secure.Code().raw);
    }
    if (FileUtil::WriteStringToFile(true, destination + "save-metadata.txt", metadata) != metadata.size()) return false;
    const std::string complete = "Console save backup completed before emulator writes.\n";
    if (FileUtil::WriteStringToFile(true, destination + "backup-complete.txt", complete) != complete.size()) return false;
    LOG_INFO(Service_FS, "Backed up console save for {:016X}: {} bytes at {}", title, bytes_seen, destination);
    return true;
}

} // namespace

bool BackupDockExtData(ArchiveBackend& archive, u64 title, u64 id) {
    return BackupDockSave(archive, title, true, id);
}

ArchiveSource_SDSaveData::ArchiveSource_SDSaveData(const std::string& sdmc_directory)
    : mount_point(GetSaveDataContainerPath(sdmc_directory)) {
    LOG_DEBUG(Service_FS, "Directory {} set as SaveData.", mount_point);
}

ResultVal<std::unique_ptr<ArchiveBackend>> ArchiveSource_SDSaveData::Open(
    Service::FS::ArchiveIdCode archive_id, const Path& path, u64 program_id) {
    if (dock_mode && dock_titles.contains(program_id)) {
        EnsureCacheCreated();
        auto archive = ArticArchive::Open(artic_client,
            static_cast<Service::FS::ArchiveIdCode>(0x567890B2),
            DockSavePath(program_id, dock_titles.at(program_id)),
            Core::PerfStats::PerfArticEventBits::ARTIC_SAVE_DATA, *this, false);
        if (archive.Failed()) return archive.Code();
        if (!backed_up_titles.contains(program_id)) {
            if (!BackupDockSave(**archive, program_id)) {
                LOG_ERROR(Service_FS, "Console save backup failed; refusing writable save access");
                return ResultUnknown;
            }
            backed_up_titles.insert(program_id);
        }
        return std::move(*archive);
    }
    if (IsUsingArtic() && !dock_mode) {
        EnsureCacheCreated();
        return ArticArchive::Open(artic_client, archive_id, path,
                                  Core::PerfStats::PerfArticEventBits::ARTIC_SAVE_DATA, *this,
                                  archive_id != Service::FS::ArchiveIdCode::SaveData);
    } else {
        std::string concrete_mount_point = GetSaveDataPath(mount_point, program_id);
        if (!FileUtil::Exists(concrete_mount_point)) {
            // When a SaveData archive is created for the first time, it is not yet formatted and
            // the save file/directory structure expected by the game has not yet been initialized.
            // Returning the NotFormatted error code will signal the game to provision the SaveData
            // archive with the files and folders that it expects.
            return ResultNotFormatted;
        }

        return std::make_unique<SaveDataArchive>(std::move(concrete_mount_point));
    }
}

Result ArchiveSource_SDSaveData::Format(u64 program_id,
                                        const FileSys::ArchiveFormatInfo& format_info,
                                        Service::FS::ArchiveIdCode archive_id, const Path& path,
                                        u32 directory_buckets, u32 file_buckets) {
    if (dock_mode && dock_titles.contains(program_id)) return ResultUnsupportedOpenFlags;
    if (IsUsingArtic() && !dock_mode) {
        ClearAllCache();
        auto req = artic_client->NewRequest("FSUSER_FormatSaveData");

        req.AddParameterS32(static_cast<u32>(archive_id));
        auto artic_path = ArticArchive::BuildFSPath(path);
        req.AddParameterBuffer(artic_path.data(), artic_path.size());
        req.AddParameterU32(format_info.total_size / 512);
        req.AddParameterU32(format_info.number_directories);
        req.AddParameterU32(format_info.number_files);
        req.AddParameterU32(directory_buckets);
        req.AddParameterU32(file_buckets);
        req.AddParameterU8(format_info.duplicate_data);

        auto resp = artic_client->Send(req);
        return ArticArchive::RespResult(resp);
    } else {
        std::string concrete_mount_point = GetSaveDataPath(mount_point, program_id);
        FileUtil::DeleteDirRecursively(concrete_mount_point);
        FileUtil::CreateFullPath(concrete_mount_point);

        // Write the format metadata
        std::string metadata_path = GetSaveDataMetadataPath(mount_point, program_id);
        FileUtil::IOFile file(metadata_path, "wb");

        if (file.IsOpen()) {
            file.WriteBytes(&format_info, sizeof(format_info));
            return ResultSuccess;
        }
        return ResultSuccess;
    }
}

ResultVal<ArchiveFormatInfo> ArchiveSource_SDSaveData::GetFormatInfo(
    u64 program_id, Service::FS::ArchiveIdCode archive_id, const Path& path) const {
    if (dock_mode && dock_titles.contains(program_id)) {
        const auto info = DockSavePath(program_id, dock_titles.at(program_id));
        auto req = artic_client->NewRequest("FSUSER_GetFormatInfo");
        req.AddParameterU32(0x567890B2);
        const auto encoded = ArticArchive::BuildFSPath(info);
        req.AddParameterBuffer(encoded.data(), encoded.size());
        const auto resp = artic_client->Send(req);
        const auto result = ArticArchive::RespResult(resp);
        if (result.IsError()) return result;
        const auto buffer = resp->GetResponseBuffer(0);
        if (!buffer || buffer->second != sizeof(ArchiveFormatInfo)) return ResultUnknown;
        ArchiveFormatInfo value;
        std::memcpy(&value, buffer->first, sizeof(value));
        return value;
    }
    if (IsUsingArtic() && !dock_mode) {
        auto req = artic_client->NewRequest("FSUSER_GetFormatInfo");

        req.AddParameterS32(static_cast<u32>(archive_id));
        auto path_artic = ArticArchive::BuildFSPath(path);
        req.AddParameterBuffer(path_artic.data(), path_artic.size());

        auto resp = artic_client->Send(req);
        Result res = ArticArchive::RespResult(resp);
        if (R_FAILED(res)) {
            return res;
        }

        auto info_buf = resp->GetResponseBuffer(0);
        if (!info_buf.has_value() || info_buf->second != sizeof(ArchiveFormatInfo)) {
            return ResultUnknown;
        }

        ArchiveFormatInfo info;
        memcpy(&info, info_buf->first, sizeof(info));
        return info;
    } else {
        std::string metadata_path = GetSaveDataMetadataPath(mount_point, program_id);
        FileUtil::IOFile file(metadata_path, "rb");

        if (!file.IsOpen()) {
            LOG_ERROR(Service_FS, "Could not open metadata information for archive");
            // TODO(Subv): Verify error code
            return ResultNotFormatted;
        }

        ArchiveFormatInfo info = {};
        file.ReadBytes(&info, sizeof(info));
        return info;
    }
}

std::string ArchiveSource_SDSaveData::GetSaveDataPathFor(const std::string& mount_point,
                                                         u64 program_id) {
    return GetSaveDataPath(GetSaveDataContainerPath(mount_point), program_id);
}

} // namespace FileSys
