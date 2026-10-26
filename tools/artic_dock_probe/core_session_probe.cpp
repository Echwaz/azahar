// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <iostream>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>
#include "core/file_sys/ncch_container.h"
#include "common/logging/log.h"
#include "common/logging/backend.h"
#include "common/dynamic_library.h"
#include "common/settings.h"
#include "core/loader/smdh.h"
#include "core/core.h"
#include "core/file_sys/romfs_reader.h"
#include "core/file_sys/archive_artic.h"
#include "core/file_sys/archive_extsavedata.h"
#include "core/file_sys/archive_ncch.h"
#include "core/file_sys/archive_source_sd_savedata.h"
#include "core/loader/artic.h"

int main(int argc, char** argv) {
    if (argc != 3 && argc != 4) {
        return 2;
    }
    Common::Log::Initialize();
    if (argc == 4 && std::string_view(argv[3]) == "--card-status") {
        auto client = std::make_shared<Network::ArticBase::Client>(argv[1], static_cast<u16>(std::stoul(argv[2])));
        if (!client->Connect()) return 1;
        Network::ArticBase::TitleReader reader(client, {});
        const auto a = reader.ReadGameCardStatus();
        const auto empty = reader.ReadGameCardStatus();
        const auto error = reader.ReadGameCardStatus();
        const auto b = reader.ReadGameCardStatus();
        const auto removed = reader.ReadGameCardStatus();
        client->Stop();
        if (!a || *a != 0x0004000000197100ULL || !empty || empty->has_value() || error ||
            !b || *b != 0x0004000000055D00ULL || !removed || removed->has_value()) return 1;
        std::cout << "card_sequence_valid=1 transport_error_distinct=1\n";
        return 0;
    }
    if (argc == 4 && (std::string_view(argv[3]) == "--extdata-route" ||
                     std::string_view(argv[3]) == "--extdata-backup-only")) {
        const auto* profile = std::getenv("ARTIC_TEST_USER_DIR");
        if (!profile) return 2;
        FileUtil::SetUserPath(std::string(profile) + "/");
        const bool physical = std::string_view(argv[3]) == "--extdata-backup-only";
        const u64 title = physical ? 0x0004000000197100ULL : 0x0004000000123400ULL;
        const u64 id = physical ? 0x1971 : 0x1234;
        auto client = std::make_shared<Network::ArticBase::Client>(argv[1], static_cast<u16>(std::stoul(argv[2])));
        if (!client->Connect()) return 1;
        if (physical) {
            Network::ArticBase::TitleReader metadata(client, {title, 2});
            const auto bytes = metadata.ReadExheader();
            if (!bytes) return 1;
            ExHeader_Header header{};
            std::memcpy(&header, bytes->data(), bytes->size());
            const auto& storage = header.arm11_system_local_caps.storage_info;
            u64 declared = storage.ext_save_data_id;
            if (storage.other_attributes >> 1) {
                declared = 0;
                for (const u64 candidate : std::array<u64, 6>{storage.extdata_id0.Value(), storage.extdata_id1.Value(),
                        storage.extdata_id2.Value(), storage.extdata_id3.Value(), storage.extdata_id4.Value(), storage.extdata_id5.Value()}) {
                    if (candidate) { declared = candidate; break; }
                }
            }
            if (declared != id) return 1;
            std::cout << "declared_extdata=" << std::hex << declared << std::dec << '\n';
        }
        {
            FileSys::ArchiveFactory_ExtSaveData factory(profile, FileSys::ExtSaveDataType::Normal);
            factory.RegisterArticDock(client, title, id);
            const auto path = FileSys::ConstructExtDataBinaryPath(1, 0, static_cast<u32>(id));
            if (!physical && !factory.Open(path, 0x0004003000008202ULL).Failed()) return 1;
            auto archive = factory.Open(path, title);
            if (archive.Failed()) { client->Stop(); return 1; }
            if (!physical) {
                if (factory.FormatAsExtData(path, {}, 0, title, 0, std::nullopt).IsSuccess() ||
                    factory.DeleteExtData(Service::FS::MediaType::SDMC, 0, 0, static_cast<u32>(id)).IsSuccess()) return 1;
                auto file = (*archive)->OpenFile(FileSys::Path("/main"), FileSys::Mode{.hex = 3});
                const std::array<u8, 4> value{'O','K','A','Y'};
                if (file.Failed() || (*file)->Write(0, value.size(), true, false, value.data()).ValueOr(0) != 4 || !(*file)->Close()) return 1;
            }
        }
        client->Stop();
        std::cout << (physical ? "extdata_backup_complete=1 read_only=1\n" :
                                 "extdata_backup_complete=1 home_scope_preserved=1\n");
        return 0;
    }
    if (argc == 4 && (std::string_view(argv[3]) == "--save-route" ||
                     std::string_view(argv[3]) == "--save-backup-only")) {
        const auto* profile = std::getenv("ARTIC_TEST_USER_DIR");
        if (!profile) return 2;
        FileUtil::SetUserPath(std::string(profile) + "/");
        auto client = std::make_shared<Network::ArticBase::Client>(
            argv[1], static_cast<u16>(std::stoul(argv[2])));
        if (!client->Connect()) return 1;
        {
            FileSys::ArchiveSource_SDSaveData source(profile);
            const bool read_only = std::string_view(argv[3]) == "--save-backup-only";
            const u64 title = read_only ? 0x0004000000055D00ULL : 0x0004000000123400ULL;
            source.RegisterArticDock(client, title, 1);
            auto save = source.Open(Service::FS::ArchiveIdCode::SaveData, FileSys::Path(),
                                    title);
            if (save.Failed()) { client->Stop(); return 1; }
            if (read_only) {
                save->reset();
                client->Stop();
                std::cout << "save_backup_only=1\n";
                return 0;
            }
            auto file = (*save)->OpenFile(FileSys::Path("/main"), FileSys::Mode{.hex = 3});
            const std::array<u8, 4> value{'O', 'K', 'A', 'Y'};
            if (file.Failed() || (*file)->Write(0, value.size(), true, false, value.data()).ValueOr(0) != 4 ||
                !(*file)->Close() || (*save)->Control(0, nullptr, 0, nullptr, 0).IsError()) return 1;
        }
        client->Stop();
        std::cout << "save_routed_to_title=1 backup_before_write=1 commit=1\n";
        return 0;
    }
    if (argc == 4 && std::string_view(argv[3]) == "--home-layout") {
        Common::Log::SetColorConsoleBackendEnabled(true);
        Common::Log::Start();
        auto client = std::make_shared<Network::ArticBase::Client>(
            argv[1], static_cast<u16>(std::stoul(argv[2])));
        if (!client->Connect()) return 1;
        Network::ArticBase::TitleReader reader(client, {0x0004003000008202ULL, 0});
        const auto layout = reader.ReadHomeLayout([](std::span<const u8> image)
                -> std::optional<std::vector<u8>> {
            std::cout << "home_container_bytes=" << image.size() << '\n';
            Common::DynamicLibrary parser;
            if (!parser.Load(FileUtil::GetExeDirectory() + "/" +
                Common::DynamicLibrary::GetLibraryName("artic_home_layout"))) {
                std::cerr << "parser_load_error=" << parser.GetLoadError() << '\n';
                return std::nullopt;
            }
            using Extractor = int (*)(const u8*, std::size_t, u8*, std::size_t);
            auto extract = parser.GetSymbol<Extractor>("artic_extract_home_layout");
            std::vector<u8> output(0x2558);
            if (!extract) { std::cerr << "parser_symbol_missing\n"; return std::nullopt; }
            const auto result = extract(image.data(), image.size(), output.data(), output.size());
            if (result) { std::cerr << "parser_result=" << result << '\n'; return std::nullopt; }
            return output;
        });
        client->Stop();
        if (!layout || layout->first.size() != 0x2DA0 || layout->second.size() != 0x2558) return 1;
        std::cout << "home_sd_layout=1 home_nand_layout=1\n";
        return 0;
    }
    if (argc == 4 && std::string_view(argv[3]) == "--update-romfs") {
        auto client = std::make_shared<Network::ArticBase::Client>(argv[1], static_cast<u16>(std::stoul(argv[2])));
        if (!client->Connect()) return 1;
        for (int repeat = 0; repeat < 2; ++repeat) {
            FileSys::ArticRomFSReader base(client, false, Network::ArticBase::TitleTarget{0x0004000000123400ULL, 1});
            FileSys::ArticRomFSReader update(client, true, Network::ArticBase::TitleTarget{0x0004000000123400ULL, 1});
            std::array<u8, 64> a{}, b{};
            if (base.OpenStatus() != Loader::ResultStatus::Success || update.OpenStatus() != Loader::ResultStatus::Success ||
                base.ReadFile(0, a.size(), a.data()) != a.size() || update.ReadFile(0, b.size(), b.data()) != b.size() ||
                a.back() != 0 || b.back() != 7) return 1;
        }
        client->Stop();
        std::cout << "base_update_separate=1\n";
        return 0;
    }
    if (argc == 4 && std::string_view(argv[3]) == "--large-code") {
        auto client = std::make_shared<Network::ArticBase::Client>(argv[1], static_cast<u16>(std::stoul(argv[2])));
        if (!client->Connect()) return 1;
        Network::ArticBase::TitleReader reader(client, {0x0004000000123400ULL, 1});
        const auto code = reader.ReadExeFS(".code");
        client->Stop();
        if (!code || code->size() != 5 * 1024 * 1024 + 17) return 1;
        for (std::size_t i = 0; i < code->size(); ++i) if ((*code)[i] != i % 251) return 1;
        std::cout << "large_code_complete=1\n";
        return 0;
    }
    if (argc == 4 && (std::string_view(argv[3]) == "--rhythm-code" || std::string_view(argv[3]) == "--mhxx-code")) {
        auto client = std::make_shared<Network::ArticBase::Client>(
            argv[1], static_cast<u16>(std::stoul(argv[2])));
        if (!client->Connect()) return 1;
        Network::ArticBase::TitleReader previous(client, {0x0004000000030800ULL, 1});
        std::array<u8, 0x1028> previous_header{};
        std::cout << "previous_romfs=" << previous.ReadRomFSChunk(0, previous_header).has_value() << '\n';
        const bool mhxx = std::string_view(argv[3]) == "--mhxx-code";
        Network::ArticBase::TitleReader reader(client, {mhxx ? 0x0004000000197100ULL : 0x000400000018A500ULL, static_cast<u8>(mhxx ? 2 : 1)});
        const auto metadata = reader.ReadExheader();
        if (!metadata) return 1;
        const auto code = reader.ReadExeFS(".code");
        std::array<u8, 0x1028> header{};
        const auto total = reader.ReadRomFSChunk(0, header);
        const auto layout = total ? Network::ArticBase::TitleReader::InspectRomFS(header, *total) : std::nullopt;
        if (mhxx) {
            std::array<u8, 0x1028> update_header{};
            const auto update_size = reader.ReadRomFSChunk(0, update_header, true);
            const bool valid = update_size && Network::ArticBase::TitleReader::InspectRomFS(update_header, *update_size).has_value();
            const bool distinct = update_header != header;
            std::cout << "update_romfs_valid=" << valid << " update_differs_from_cartridge=" << distinct << '\n';
            if (!valid || !distinct) { client->Stop(); return 1; }
        }
        client->Stop();
        std::cout << "code_bytes=" << (code ? code->size() : 0) << " romfs_received=" << total.has_value() << " layout_valid=" << layout.has_value() << '\n';
        if (!layout) return 1;
        std::vector<u8> decoded;
        if (!code || !FileSys::NCCHContainer::DecompressExeFSCode(*code, decoded, 64 * 1024 * 1024)) return 1;
        std::cout << "code_valid=1 decoded_bytes=" << decoded.size() << '\n';
        return 0;
    }
    if (argc == 4 && std::string_view(argv[3]) == "--romfs-prefetch") {
        auto client = std::make_shared<Network::ArticBase::Client>(
            argv[1], static_cast<u16>(std::stoul(argv[2])));
        if (!client->Connect()) return 1;
        {
            FileSys::ArticRomFSReader reader(client, false,
                Network::ArticBase::TitleTarget{0x0004000000123400, 1});
            std::vector<u8> data(16384);
            if (reader.OpenStatus() != Loader::ResultStatus::Success ||
                reader.ReadFile(256, data.size(), data.data()) != data.size()) return 1;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!reader.CacheReady(65536, 65536) && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (!reader.CacheReady(65536, 65536) || reader.ReadFile(65536, 1, data.data()) != 1 ||
                data[0] != static_cast<u8>(65536 % 251)) return 1;
        }
        client->Stop();
        std::cout << "prefetch_ready=1 clean_shutdown=1\n";
        return 0;
    }
    if (argc == 4 && std::string_view(argv[3]) == "--romfs-batch") {
        auto client = std::make_shared<Network::ArticBase::Client>(
            argv[1], static_cast<u16>(std::stoul(argv[2])));
        if (!client->Connect()) return 1;
        {
            FileSys::ArticRomFSReader reader(client, false,
                Network::ArticBase::TitleTarget{0x0004000000123400, 1});
            std::vector<u8> data(128 * 1024 + 17);
            if (reader.OpenStatus() != Loader::ResultStatus::Success ||
                reader.ReadFile(256, data.size(), data.data()) != data.size()) return 1;
            for (std::size_t i = 0; i < data.size(); ++i) {
                if (data[i] != static_cast<u8>((i + 256) % 251)) return 1;
            }
            if (!reader.CacheReady(3 * 65536 - 1, 1) ||
                reader.ReadFile(3 * 65536 - 1, 1, data.data()) != 1 ||
                data[0] != static_cast<u8>((3 * 65536 - 1) % 251)) return 1;
        }
        client->Stop();
        std::cout << "coalesced_read=1 trailing_page_complete=1\n";
        return 0;
    }
    if (argc == 4 && std::string_view(argv[3]) == "--ncch-media") {
        auto client = std::make_shared<Network::ArticBase::Client>(
            argv[1], static_cast<u16>(std::stoul(argv[2])));
        if (!client->Connect()) return 1;
        {
            FileSys::ArchiveFactory_NCCH provider;
            const std::array<u32, 4> archive_words{0x123400, 0x40000, 0xFFFFFF01, 0};
            const std::array<u32, 5> file_words{0, 0, 2, 0x6E6F6369, 0};
            auto path = [](const auto& words) {
                const auto* bytes = reinterpret_cast<const u8*>(words.data());
                return FileSys::Path(std::vector<u8>(bytes, bytes + sizeof(words)));
            };
            FileSys::ArticArchive archive(client, -1,
                Core::PerfStats::PerfArticEventBits::NONE, provider,
                path(archive_words), false, true);
            FileSys::Mode mode;
            mode.hex = 1;
            Settings::values.apply_region_free_patch = true;
            auto file = archive.OpenFile(path(file_words), mode, 0);
            std::array<u8, 4> header{};
            if (file.Failed() || (*file)->GetSize() != 0x36C0 ||
                (*file)->Read(0, header.size(), header.data()).ValueOr(0) != header.size() ||
                header != std::array<u8, 4>{'S', 'M', 'D', 'H'}) return 1;
            u32_le region;
            if ((*file)->Read(offsetof(Loader::SMDH, region_lockout), sizeof(region),
                    reinterpret_cast<u8*>(&region)).ValueOr(0) != sizeof(region) ||
                region != 0x7FFFFFFF) return 1;
            Settings::values.apply_region_free_patch = false;
            auto original = archive.OpenFile(path(file_words), mode, 0);
            if (original.Failed() ||
                (*original)->Read(offsetof(Loader::SMDH, region_lockout), sizeof(region),
                    reinterpret_cast<u8*>(&region)).ValueOr(0) != sizeof(region) ||
                region != 0) return 1;
        }
        client->Stop();
        std::cout << "ncch_media_low_byte=1 buffered_icon=1 region_setting_respected=1\n";
        return 0;
    }
    if (argc == 4 && std::string_view(argv[3]) == "--romfs-cache") {
        auto client = std::make_shared<Network::ArticBase::Client>(
            argv[1], static_cast<u16>(std::stoul(argv[2])));
        if (!client->Connect()) {
            return 1;
        }
        {
            FileSys::ArticRomFSReader reader(client, false,
                Network::ArticBase::TitleTarget{0x0004000000123400, 1});
            std::array<u8, 100> data{};
            if (reader.OpenStatus() != Loader::ResultStatus::Success || reader.GetSize() != 64 ||
                reader.CacheReady(0, 16) || reader.ReadFile(0, 16, data.data()) != 16 ||
                !reader.CacheReady(0, 64) || reader.ReadFile(8, 12, data.data()) != 12 ||
                reader.ReadFile(64, data.size(), data.data()) != 0 ||
                reader.CacheReady(64, 1) || !reader.CacheReady(64, 0)) {
                return 1;
            }
            reader.ClearCache();
            if (reader.CacheReady(0, 16) || reader.ReadFile(63, data.size(), data.data()) != 1) {
                return 1;
            }
        }
        client->Stop();
        std::cout << "stream_cache_reused=1 eof_bounded=1\n";
        return 0;
    }
    auto& system = Core::System::GetInstance();
    auto parent = std::make_unique<Loader::Apploader_Artic>(
        system, argv[1], static_cast<u16>(std::stoul(argv[2])),
        Loader::Apploader_Artic::ArticInitMode::NONE,
        Network::ArticBase::TitleTarget{0x0004000000123400, 1});
    if (parent->LoadKernelMemoryMode().second != Loader::ResultStatus::Success) {
        return 1;
    }
    const Network::ArticBase::TitleTarget target{0x0004000000055D00, 1};
    auto child = parent->CreateTitleLoader(target);
    if (!child || child->LoadKernelMemoryMode().second != Loader::ResultStatus::Success) {
        return 1;
    }
    child.reset();
    std::vector<u8> icon;
    if (parent->ReadIcon(icon, false) != Loader::ResultStatus::Success) {
        return 1;
    }
    auto reset = parent->CreateResetLoader(target);
    parent.reset();
    u64 id = 0;
    if (!reset || reset->ReadProgramId(id) != Loader::ResultStatus::Success || id != target.id ||
        reset->ReadIcon(icon, false) != Loader::ResultStatus::Success) {
        return 1;
    }
    reset.reset();
    std::cout << "borrowed_session_alive=1 reset_session_alive=1\n";
}
