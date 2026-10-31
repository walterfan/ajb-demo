#pragma once
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>

namespace aca::ajb::test {
struct Directory {
  std::filesystem::path path;
  Directory() {
    static std::atomic<unsigned> counter{0};
    path = std::filesystem::canonical(std::filesystem::temp_directory_path()) /
           ("ajb-test-" +
            std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()) +
            "-" + std::to_string(counter++));
    std::filesystem::create_directory(path);
  }
  ~Directory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};
inline void wave(const std::filesystem::path& path, uint32_t rate,
                 uint16_t channels, uint32_t frames) {
  std::ofstream file(path, std::ios::binary);
  auto u16 = [&](uint16_t value) {
    file.put(static_cast<char>(value & 255));
    file.put(static_cast<char>(value >> 8));
  };
  auto u32 = [&](uint32_t value) {
    for (int i = 0; i < 4; ++i)
      file.put(static_cast<char>((value >> (8 * i)) & 255));
  };
  uint32_t bytes = frames * channels * 2;
  file.write("RIFF", 4);
  u32(bytes + 36);
  file.write("WAVEfmt ", 8);
  u32(16);
  u16(1);
  u16(channels);
  u32(rate);
  u32(rate * channels * 2);
  u16(channels * 2);
  u16(16);
  file.write("data", 4);
  u32(bytes);
  if (bytes) {
    file.seekp(44 + bytes - 1);
    file.put(0);
  }
}
}  // namespace aca::ajb::test
