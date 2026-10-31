#include "aca/ajb/app.h"

#include <gtest/gtest.h>

#include <cmath>
#include <fstream>
#include <sstream>

#include "helpers.h"

namespace aca::ajb {
TEST(Options, RejectsUnknownAndUnsafeNumbers) {
  for (const auto& args :
       std::vector<std::vector<std::string>>{{"--wat"},
                                             {"--seed", "-1"},
                                             {"--buffer-ms", "nan"},
                                             {"--buffer-ms", "-10"},
                                             {"--buffer-ms", "500"},
                                             {"--small-buffer-ms", "200"},
                                             {"--max-delay-ms", "10"},
                                             {"--network-delay-ms", "-1"},
                                             {"--network-delay-ms", "1001"},
                                             {"--network-delay-ms", "1.5"},
                                             {"--network-delay-ms", "nan"},
                                             {"--network-delay-ms", "900"},
                                             {"--jitter-ms", "15"},
                                             {"--burst-jitter-ms", "25"},
                                             {"--ui", "tui", "--no-tui"},
                                             {"--ui", "auto", "--play"}})
    EXPECT_THROW(parse_options(args), std::runtime_error);
  EXPECT_FALSE(use_tui(parse_options({"--ui", "auto"}), true, false));
  EXPECT_THROW(use_tui(parse_options({"--tui"}), false, true),
               std::runtime_error);
}
TEST(Experiment, ResultFilesAndSameNetworkForAllPolicies) {
  test::Directory directory;
  auto input = directory.path / "speech.wav";
  write_wav(input, make_fixture(.5));
  Options options;
  options.input = input;
  options.output = directory.path / "out";
  auto experiment = prepare(options);
  ASSERT_EQ(experiment->results.size(), 3u);
  for (const auto& r : experiment->results) {
    EXPECT_EQ(read_wav(experiment->directory / (r.name + ".wav")), r.audio);
    EXPECT_TRUE(
        std::filesystem::exists(experiment->directory / (r.name + ".csv")));
  }
  for (const auto& file :
       {"events.csv", "network.csv", "summary.csv", "experiment.txt"})
    EXPECT_TRUE(std::filesystem::exists(experiment->directory / file));
  auto second = prepare(options);
  EXPECT_NE(experiment->directory, second->directory);
  EXPECT_EQ(experiment->network, second->network);
  EXPECT_EQ(experiment->results[2].audio, second->results[2].audio);
  save_snapshot(*experiment, experiment->results[0], {ms(100)},
                "test snapshot\n");
}
TEST(Experiment, NumericNetworkOptionsPersistAndDriveEveryPolicy) {
  test::Directory directory;
  auto options =
      parse_options({"--scenario", "constant", "--network-delay-ms", "125",
                     "--jitter-ms", "40", "--burst-jitter-ms", "10",
                     "--burst-delay-ms", "75", "--reorder-delay-ms", "155"});
  options.input = directory.path / "speech.wav";
  options.output = directory.path / "out";
  write_wav(options.input, make_fixture(.2));
  auto run = prepare(options);
  ASSERT_EQ(run->results.size(), 3u);
  for (const auto& p : run->network) EXPECT_EQ(p.arrival - p.send, ms(125));
  for (const auto& result : run->results) {
    EXPECT_EQ(result.summary.counters.plc_samples, 0u);
    EXPECT_DOUBLE_EQ(result.summary.played_network_mean_ms, 125);
    EXPECT_DOUBLE_EQ(delays_at(result, {ms(300)}).last_network_ms, 125);
  }
  std::ifstream metadata(run->directory / "experiment.txt");
  std::string contents((std::istreambuf_iterator<char>(metadata)), {});
  for (const auto* setting :
       {"network_base_ms=125\n", "jitter_ms=40\n", "burst_jitter_ms=10\n",
        "burst_extra_ms=75\n", "reorder_extra_ms=155\n"})
    EXPECT_NE(contents.find(setting), std::string::npos);
}
TEST(Experiment, CancellationKeepsPreviousSuccessfulRun) {
  test::Directory directory;
  Options options;
  options.input = directory.path / "speech.wav";
  options.output = directory.path / "out";
  write_wav(options.input, make_fixture(.1));
  auto first = prepare(options);
  EXPECT_THROW(prepare(options, [] { return true; }), std::runtime_error);
  EXPECT_TRUE(std::filesystem::exists(first->directory / "summary.csv"));
}
TEST(Experiment, IndividualPoliciesOnlyExportTheSelectedResult) {
  test::Directory directory;
  Options options;
  options.input = directory.path / "speech.wav";
  options.output = directory.path / "out";
  write_wav(options.input, make_fixture(.5));
  auto comparison = prepare(options);
  for (int budget : {20, 150, 80, -1}) {
    options.mode = budget < 0 ? "adaptive" : "fixed";
    options.fixed_ms = budget < 0 ? 20 : budget;
    auto single = prepare(options);
    ASSERT_EQ(single->results.size(), 1u);
    const auto& result = single->results.front();
    EXPECT_EQ(result.name,
              budget < 0 ? "adaptive" : "fixed-" + std::to_string(budget));
    EXPECT_EQ(single->network, comparison->network);
    if (budget != 80)
      for (const auto& original : comparison->results)
        if (original.name == result.name)
          EXPECT_EQ(original.audio, result.audio);
    size_t wavs = 0;
    for (const auto& file :
         std::filesystem::directory_iterator(single->directory))
      if (file.path().extension() == ".wav") ++wavs;
    EXPECT_EQ(wavs, 1u);
    std::ifstream csv(single->directory / "summary.csv");
    std::string line;
    ASSERT_TRUE(static_cast<bool>(std::getline(csv, line)));
    ASSERT_TRUE(static_cast<bool>(std::getline(csv, line)));
    EXPECT_EQ(line.substr(0, line.find(',')), result.name);
    EXPECT_FALSE(static_cast<bool>(std::getline(csv, line)));
    std::ifstream metadata(single->directory / "experiment.txt");
    std::string contents((std::istreambuf_iterator<char>(metadata)), {});
    EXPECT_NE(contents.find("mode=" + options.mode + "\n"), std::string::npos);
    EXPECT_NE(
        contents.find("fixed_ms=" + std::to_string(options.fixed_ms) + "\n"),
        std::string::npos);
  }
  EXPECT_TRUE(std::filesystem::exists(comparison->directory / "summary.csv"));
}
TEST(Experiment, DelayCsvMatchesActualOutputBlocks) {
  test::Directory directory;
  Options options;
  options.input = directory.path / "speech.wav";
  options.output = directory.path / "out";
  options.mode = "adaptive";
  write_wav(options.input, make_fixture(.5));
  auto experiment = prepare(options);
  const auto& result = experiment->results.front();
  std::ifstream csv(experiment->directory / "adaptive.csv");
  std::string row;
  ASSERT_TRUE(static_cast<bool>(std::getline(csv, row)));
  EXPECT_NE(
      row.find(
          "delay_samples,played_network_ms,buffer_wait_ms,packet_to_output_ms"),
      std::string::npos);
  for (const auto& tick : result.ticks) {
    ASSERT_TRUE(static_cast<bool>(std::getline(csv, row)));
    std::istringstream line(row);
    std::vector<std::string> cells;
    for (std::string cell; std::getline(line, cell, ',');)
      cells.push_back(cell);
    ASSERT_EQ(cells.size(), 17u);
    EXPECT_EQ(std::stoul(cells[13]), tick.delays.samples);
    if (tick.delays.samples) {
      EXPECT_NEAR(std::stod(cells[14]), tick.delays.network_ms, 1e-8);
      EXPECT_NEAR(std::stod(cells[15]), tick.delays.wait_ms, 1e-8);
      EXPECT_NEAR(std::stod(cells[16]), tick.delays.playout_ms, 1e-8);
    } else {
      EXPECT_TRUE(std::isnan(std::stod(cells[16])));
    }
  }
  EXPECT_FALSE(static_cast<bool>(std::getline(csv, row)));
}
}  // namespace aca::ajb
