#pragma once

#include <filesystem>
#include <memory>

#include "aca/ajb/core.h"

namespace aca::ajb {
std::filesystem::path checked_path(const std::filesystem::path& path,
                                   bool must_exist);
void check_distinct(const std::filesystem::path& input,
                    const std::filesystem::path& output);
void check_media_elements();
Pcm read_wav(const std::filesystem::path& path, const Cancel& cancel = {});
void write_wav(const std::filesystem::path& path, const Pcm& samples,
               const Cancel& cancel = {});
class Playback {
 public:
  explicit Playback(bool silent_sink = false);
  ~Playback();
  Playback(const Playback&) = delete;
  Playback& operator=(const Playback&) = delete;
  void start(const Pcm& samples, Time origin = 0);
  void pause();
  void resume();
  void stop();
  Time position();
  bool ended();
  std::string error();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace aca::ajb
