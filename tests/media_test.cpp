#include "aca/ajb/media.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "helpers.h"

namespace aca::ajb {
TEST(Media, WavRoundTripAndPartialFrame) {
  test::Directory directory;
  auto path = directory.path / "with spaces.wav";
  Pcm input(1001, 1234);
  write_wav(path, input);
  EXPECT_EQ(read_wav(path), input);
  Config c;
  auto result = simulate(input, make_network(input.size(), c), c, false, 20);
  auto output = directory.path / "result.wav";
  write_wav(output, result.audio);
  EXPECT_EQ(read_wav(output), result.audio);
}
TEST(Media, NormalizesStereo44100) {
  test::Directory directory;
  auto path = directory.path / "stereo.wav";
  test::wave(path, 44100, 2, 44100);
  auto data = read_wav(path);
  EXPECT_NEAR(static_cast<double>(data.size()), kRate, 5);
  EXPECT_EQ(*std::max_element(data.begin(), data.end()), 0);
}
TEST(Media, RejectsInvalidPathsFormatsAndLimits) {
  test::Directory directory;
  EXPECT_THROW(read_wav(directory.path / "missing.wav"), std::runtime_error);
  auto path = directory.path / "bad.wav";
  {
    std::ofstream file(path);
    file << "not a wave";
  }
  EXPECT_THROW(read_wav(path), std::runtime_error);
  test::wave(path, kRate, 1, 0);
  EXPECT_THROW(read_wav(path), std::runtime_error);
  test::wave(path, kRate, 1, 10);
  auto link = directory.path / "link.wav";
  std::filesystem::create_symlink(path, link);
  EXPECT_THROW(read_wav(link), std::runtime_error);
  EXPECT_THROW(check_distinct(path, path), std::runtime_error);
  EXPECT_THROW(checked_path(directory.path / ".." / "other.wav", false),
               std::runtime_error);
  std::filesystem::resize_file(path, 129 * 1024 * 1024);
  EXPECT_THROW(read_wav(path), std::runtime_error);
}
TEST(Media, RejectsDecodedDurationAndCancellation) {
  test::Directory directory;
  auto path = directory.path / "long.wav";
  test::wave(path, kRate, 1, 301 * kRate);
  EXPECT_THROW(read_wav(path), std::runtime_error);
  test::wave(path, kRate, 1, kRate);
  EXPECT_THROW(read_wav(path, [] { return true; }), std::runtime_error);
  EXPECT_THROW(write_wav(directory.path / "cancel.wav", make_fixture(.1),
                         [] { return true; }),
               std::runtime_error);
  EXPECT_FALSE(std::filesystem::exists(directory.path / "cancel.wav.partial"));
}
TEST(Media, PlaybackPauseRebuildAndSilentSink) {
  auto data = make_fixture(.5);
  Playback player(true);
  player.start(data);
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  player.pause();
  auto position = player.position();
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  EXPECT_NEAR(static_cast<double>(player.position()), position, ms(10));
  player.start(data, ms(200));
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!player.ended() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_TRUE(player.ended());
  EXPECT_GE(player.position(), ms(490));
}
TEST(Media, LongPauseDoesNotTripBackpressureTimeout) {
  auto data = make_fixture(1);
  Playback player(true);
  player.start(data);
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  player.pause();
  auto position = player.position();
  std::this_thread::sleep_for(std::chrono::seconds(11));
  EXPECT_TRUE(player.error().empty());
  EXPECT_NEAR(static_cast<double>(player.position()), position, ms(10));
  player.resume();
}
}  // namespace aca::ajb
