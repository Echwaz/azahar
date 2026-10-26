// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <iostream>
#include "core/file_sys/ncch_container.h"
#include "network/artic_base/title_reader.h"

int main(int argc, char** argv) {
    if (argc != 3 && argc != 4) {
        std::cerr << "Usage: artic-code-probe ADDRESS TITLE_HEX [MEDIA]\n";
        return 2;
    }
    try {
        const auto title = std::stoull(argv[2], nullptr, 16);
        const auto media = argc == 4 ? std::stoul(argv[3]) : 0;
        if (media > 2) {
            return 2;
        }
        auto client = std::make_shared<Network::ArticBase::Client>(argv[1], 5543);
        if (!client->Connect()) {
            return 1;
        }
        Network::ArticBase::TitleReader reader(client, {title, static_cast<u8>(media)});
        const auto metadata = reader.ReadExheader();
        const auto code = reader.ReadExeFS(".code");
        client->Stop();
        if (!metadata || !code || (((*metadata)[13] & 1) == 0)) {
            std::cerr << "Compressed title data is unavailable\n";
            return 1;
        }
        std::vector<u8> decoded;
        const bool success =
            FileSys::NCCHContainer::DecompressExeFSCode(*code, decoded, 64 * 1024 * 1024);
        std::cout << "compressed_bytes=" << code->size()
                  << " decompressed_bytes=" << decoded.size()
                  << " success=" << success << '\n';
        return success ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
