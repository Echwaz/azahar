// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include "network/artic_base/title_reader.h"
#include <iostream>
#include <memory>
#include <string>
#include <chrono>

int main(int argc, char **argv) {
  if (argc != 5 && argc != 6) {
    std::cerr
        << "Usage: artic-dock-probe ADDRESS TITLE_HEX MEDIA SECTION [PORT]\n";
    return 2;
  }
  try {
    const auto title = std::stoull(argv[2], nullptr, 16);
    const auto media = std::stoul(argv[3]);
    if (media > 2) {
      return 2;
    }
    const auto port = argc == 6 ? std::stoul(argv[5]) : 5543;
    if (port == 0 || port > 65535) {
      return 2;
    }
    auto client = std::make_shared<Network::ArticBase::Client>(
        argv[1], static_cast<u16>(port));
    if (!client->Connect()) {
      std::cerr << "Connection failed\n";
      return 1;
    }
    Network::ArticBase::TitleReader reader(client,
                                           {title, static_cast<u8>(media)});
    if (std::string_view(argv[4]) == "--layout") {
      const auto layout = reader.ReadHomeLayout();
      client->Stop();
      if (!layout) { std::cerr << "HOME layout read failed\n"; return 1; }
      std::cout << "sd_bytes=" << layout->first.size()
                << " nand_bytes=" << layout->second.size() << '\n';
      return 0;
    }
    if (std::string_view(argv[4]) == "--romfs-benchmark") {
      std::vector<u8> data(256 * 1024);
      for (int trial = 0; trial < 3; ++trial) {
        for (const std::size_t block : {65536u, 262144u}) {
          const auto start = std::chrono::steady_clock::now();
          for (std::size_t offset = 0; offset < data.size(); offset += block) {
            if (!reader.ReadRomFSChunk(offset, std::span<u8>(data).subspan(offset, block))) {
              client->Stop(); return 1;
            }
          }
          const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - start).count();
          std::cout << "trial=" << trial << " requests=" << data.size() / block
                    << " bytes=" << data.size() << " milliseconds=" << elapsed << '\n';
        }
      }
      client->Stop();
      return 0;
    }
    const auto exheader = reader.ReadExheader();
    if (std::string_view(argv[4]) == "--card") {
      const auto title = reader.ReadInsertedGameCard();
      client->Stop();
      std::cout << "card_title=" << std::hex << title.value_or(0) << '\n';
      return exheader ? 0 : 1;
    }
    if (std::string_view(argv[4]) == "--stream-romfs") {
      std::array<u8, 0x1028> header{};
      const auto size = reader.ReadRomFSChunk(0, header);
      const auto layout = size ? Network::ArticBase::TitleReader::InspectRomFS(header, *size)
                               : std::nullopt;
      std::array<u8, 40> sample{};
      const auto sample_size = layout ? reader.ReadRomFSChunk(layout->first, sample)
                                      : std::nullopt;
      client->Stop();
      std::cout << "stream_size=" << (layout ? layout->second : 0)
                << " sample_bytes=" << (sample_size ? sample.size() : 0) << '\n';
      return exheader && layout && sample_size == size ? 0 : 1;
    }
    const auto section = std::string_view(argv[4]) == "--home-romfs"
                             ? reader.ReadHomeRomFS()
                             : reader.ReadExeFS(argv[4]);
    client->Stop();
    std::cout << "exheader_bytes=" << (exheader ? exheader->size() : 0)
              << " section_bytes=" << (section ? section->size() : 0) << '\n';
    if (const auto error = reader.GetLastOpenError()) {
      std::cout << "open_error=" << std::hex << *error << '\n';
    }
    return exheader && section ? 0 : 1;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
