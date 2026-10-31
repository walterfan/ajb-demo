#include "aca/ajb/core.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace aca::ajb {
TEST(Config, RejectsInvalidBudgets) {
  Config c;
  c.max_ms = 500;
  EXPECT_THROW(c.validate(), std::runtime_error);
  c = {};
  c.small_ms = 15;
  EXPECT_THROW(c.validate(), std::runtime_error);
  c = {};
  c.min_ms = 80;
  EXPECT_THROW(c.validate(), std::runtime_error);
}
TEST(Network, DeterministicAndReorders) {
  Config c;
  c.scenario = "reorder";
  auto a = make_network(kRate, c);
  EXPECT_EQ(a, make_network(kRate, c));
  EXPECT_GT(a[0].arrival, a[1].arrival);
}
TEST(Network, NumericDelaysControlEachPreset) {
  Config c;
  c.network_base_ms = 125;
  c.jitter_ms = 40;
  c.burst_jitter_ms = 20;
  c.burst_extra_ms = 75;
  c.reorder_extra_ms = 155;
  for (const auto* scene :
       {"constant", "jitter", "burst", "reorder", "loss", "phased"}) {
    c.scenario = scene;
    auto trace = make_network(16 * kRate, c);
    EXPECT_EQ(trace, make_network(16 * kRate, c));
    for (const auto& p : trace) {
      auto delay = milliseconds(p.arrival - p.send);
      if (c.scenario == "constant" || c.scenario == "loss" ||
          (c.scenario == "phased" && (p.seq < 500 || p.seq >= 1500))) {
        EXPECT_DOUBLE_EQ(delay, 125);
      } else if (c.scenario == "reorder") {
        EXPECT_DOUBLE_EQ(delay, p.seq % 12 == 0 ? 280 : 125);
      } else if (c.scenario == "jitter" ||
                 (c.scenario == "phased" && p.seq < 1000)) {
        EXPECT_GE(delay, 125);
        EXPECT_LE(delay, 165);
      } else {
        int base = 125 + (p.seq >= 100 && p.seq % 50 < 10 ? 75 : 0);
        EXPECT_GE(delay, base);
        EXPECT_LE(delay, base + 20);
      }
    }
  }
}
TEST(Network, ExplicitOriginalProfileRemainsReproducible) {
  Config c;
  c.network_base_ms = 30;
  c.jitter_ms = 80;
  c.burst_jitter_ms = 20;
  c.burst_extra_ms = 120;
  c.reorder_extra_ms = 70;
  auto trace = make_network(16 * kRate, c);
  for (const auto& [index, expected] :
       std::vector<std::pair<size_t, int>>{{0, 30},
                                           {499, 30},
                                           {500, 90},
                                           {501, 80},
                                           {502, 100},
                                           {509, 50},
                                           {999, 30},
                                           {1000, 170},
                                           {1001, 160},
                                           {1009, 170},
                                           {1010, 40},
                                           {1499, 50},
                                           {1500, 30}})
    EXPECT_EQ(trace[index].arrival - trace[index].send, ms(expected));
}
TEST(Network, ValidatesActiveDelayBoundsAndIntegerBase) {
  Config c;
  c.scenario = "constant";
  for (int base : {0, 35, 1000}) {
    c.network_base_ms = base;
    auto input = make_fixture(.1);
    auto trace = make_network(input.size(), c);
    EXPECT_DOUBLE_EQ(milliseconds(trace.front().arrival), base);
    EXPECT_EQ(simulate(input, trace, c, false, 20).summary.counters.plc_samples,
              0u);
  }
  for (int value : {-1, 1001}) {
    c.network_base_ms = value;
    EXPECT_THROW(c.validate(), std::runtime_error);
  }
  c = {};
  c.jitter_ms = 15;
  EXPECT_THROW(c.validate(), std::runtime_error);
  c = {};
  c.burst_jitter_ms = 25;
  EXPECT_THROW(c.validate(), std::runtime_error);
  c = {};
  c.scenario = "burst";
  c.burst_jitter_ms = 20;
  c.burst_extra_ms = 120;
  c.network_base_ms = 860;
  EXPECT_NO_THROW(c.validate());  // 860+20+120=1000.
  c.burst_extra_ms = 121;
  EXPECT_THROW(c.validate(), std::runtime_error);
  c.scenario = "constant";
  EXPECT_NO_THROW(
      c.validate());  // The unused burst parameters do not add delay.
  c.scenario = "jitter";
  c.network_base_ms = 921;
  EXPECT_THROW(c.validate(), std::runtime_error);
}
TEST(Fixture, DeterministicAndSpeechLike) {
  auto a = make_fixture(.2);
  EXPECT_EQ(a, make_fixture(.2));
  EXPECT_EQ(a.size(), 9600u);
  EXPECT_GT(*std::max_element(a.begin(), a.end()), 1000);
  EXPECT_THROW(make_fixture(std::nan("")), std::runtime_error);
}
TEST(Buffer, ConstantNetworkHasNoGaps) {
  Config c;
  c.scenario = "constant";
  auto input = make_fixture(2);
  auto trace = make_network(input.size(), c);
  for (bool adaptive : {false, true}) {
    auto r = simulate(input, trace, c, adaptive, 20);
    EXPECT_EQ(r.summary.counters.plc_samples, 0u);
    EXPECT_EQ(r.summary.counters.late, 0u);
    EXPECT_EQ(r.summary.counters.overflow, 0u);
  }
}
TEST(Buffer, BurstShowsFixedDelayTradeoff) {
  Config c;
  c.scenario = "burst";
  auto input = make_fixture(3);
  auto trace = make_network(input.size(), c);
  auto small = simulate(input, trace, c, false, 20);
  auto large = simulate(input, trace, c, false, 150);
  EXPECT_GT(small.summary.counters.plc_samples, 0u);
  EXPECT_EQ(large.summary.counters.plc_samples, 0u);
  EXPECT_LT(small.summary.startup_ms, large.summary.startup_ms);
}
TEST(Buffer, StartupReorderingPreservesFirstFrame) {
  Config c;
  Pcm input(4 * kFrame, 1200);
  auto trace = make_network(input.size(), c);
  trace[0].arrival = ms(60);
  trace[1].arrival = ms(40);
  auto r = simulate(input, trace, c, false, 20);
  EXPECT_EQ(r.summary.counters.plc_samples, 0u);
  EXPECT_EQ(r.summary.played_source_samples, input.size());
}
TEST(Buffer, SameTimeArrivalPrecedesOutput) {
  Config c;
  c.network_base_ms = 30;
  Pcm input(2 * kFrame, 1000);
  auto trace = make_network(input.size(), c);
  trace[1].arrival = ms(100);  // Startup=90, second deadline=100.
  auto r = simulate(input, trace, c, false, 20);
  EXPECT_EQ(r.summary.counters.plc_samples, 0u);
  trace[1].arrival += 1;
  r = simulate(input, trace, c, false, 20);
  EXPECT_EQ(r.summary.counters.plc_samples, kFrame);
}
TEST(Buffer, DuplicateOverflowAndAllLostAreBounded) {
  Config c;
  auto input = make_fixture(2);
  auto trace = make_network(input.size(), c);
  trace.push_back(trace[0]);
  auto r = simulate(input, trace, c, false, 20);
  EXPECT_EQ(r.summary.counters.duplicate, 1u);
  for (auto& p : trace) {
    p.lost = true;
  }
  r = simulate(input, trace, c, true, 20);
  EXPECT_EQ(r.summary.played_source_samples, 0u);
  EXPECT_LT(r.duration(), ms(6000));
  EXPECT_TRUE(std::isnan(r.summary.wait_mean_ms));
}
TEST(Buffer, CapacityDropsFutureAndPartialTailIsNotPlc) {
  Config c;
  auto input = make_fixture(2);
  auto trace = make_network(input.size(), c);
  for (auto& p : trace) {
    p.send = 0;
    p.arrival = ms(30);
  }
  auto r = simulate(input, trace, c, false, 460);
  EXPECT_GT(r.summary.counters.overflow, 0u);
  for (const auto& checkpoint : r.checkpoints)
    EXPECT_LE(checkpoint.queue.size(), 50u);
  Pcm partial(1001, 1000);
  r = simulate(partial, make_network(partial.size(), c), c, false, 20);
  EXPECT_EQ(r.summary.counters.plc_samples, 0u);
  EXPECT_EQ(r.summary.played_source_samples, partial.size());
}
TEST(Buffer, PlcHasExactMissingDurationAndFiniteFade) {
  Config c;
  Pcm input(100 * kFrame, 10000);
  auto trace = make_network(input.size(), c);
  trace[20].lost = true;
  auto r = simulate(input, trace, c, false, 20);
  EXPECT_EQ(r.summary.counters.plc_samples, kFrame);
  EXPECT_EQ(r.summary.counters.plc_events, 1u);
  size_t gap = static_cast<size_t>(r.summary.startup_ms * 48) + 20 * kFrame;
  EXPECT_EQ(r.audio[gap + 240], 0);
  EXPECT_EQ(r.audio[gap + kFrame + 240], 10000);
}
TEST(Estimator, LateDelayEvidenceRaisesTargetThenDecays) {
  Config c;
  DelayEstimator e(c);
  for (int i = 0; i < 300; ++i) e.arrive(ms(i * 10 + 30), ms(i * 10));
  int before = e.target();
  EXPECT_GT(e.arrive(ms(3200), ms(3030)), before);
  for (int i = 0; i < 1000; ++i) e.arrive(ms(3400 + i * 10), ms(3370 + i * 10));
  EXPECT_EQ(e.target(), c.min_ms);
}
TEST(Adaptive, PhasedReferenceTradeoffAndBounds) {
  Config c;
  auto input = make_fixture();
  auto network = make_network(input.size(), c);
  auto small = simulate(input, network, c, false, c.small_ms);
  auto large = simulate(input, network, c, false, c.large_ms);
  auto adaptive = simulate(input, network, c, true, 0);
  EXPECT_LT(adaptive.summary.counters.plc_samples,
            small.summary.counters.plc_samples);
  EXPECT_LT(adaptive.summary.wait_mean_ms, large.summary.wait_mean_ms);
  EXPECT_GT(adaptive.summary.counters.fast, 0u);
  EXPECT_GT(adaptive.summary.counters.slow, 0u);
  for (const auto& tick : adaptive.ticks) {
    EXPECT_LE(tick.target_ms, c.max_ms);
    EXPECT_GE(tick.target_ms, c.min_ms);
    if (tick.advance) {
      EXPECT_GE(tick.advance, 456);
      EXPECT_LE(tick.advance, 504);
    }
  }
}
TEST(Wsola, PreservesPitchAndBlockSize) {
  Pcm source(3 * kRate);
  for (size_t i = 0; i < source.size(); ++i)
    source[i] = static_cast<int16_t>(
        10000 * std::sin(2 * 3.141592653589793 * 200 * i / kRate));
  for (int advance : {456, 480, 504}) {
    Wsola processor;
    Pcm output;
    for (size_t pos = 0; pos + 4 * kFrame < source.size(); pos += advance) {
      auto block = processor.process(std::span(source).subspan(pos, 4 * kFrame),
                                     true, advance);
      output.insert(output.end(), block.samples.begin(), block.samples.end());
    }
    size_t crossings = 0;
    for (size_t i = kRate + 1; i < output.size(); ++i)
      if (output[i - 1] <= 0 && output[i] > 0) ++crossings;
    double freq =
        crossings * static_cast<double>(kRate) / (output.size() - kRate);
    EXPECT_NEAR(freq, 200, 4);
  }
}
TEST(Display, EscapesTerminalControls) {
  EXPECT_EQ(display_safe("x\x1b[2Jy\n"), "x?[2Jy?");
  EXPECT_EQ(display_safe("\x9b"
                         "31m"),
            "?31m");
  EXPECT_EQ(display_safe("中文"), "中文");
  EXPECT_EQ(display_safe("中文", 3), "中...");
  EXPECT_EQ(display_safe("\xc0\x9b"), "??");
}
TEST(Metrics, HandCalculatedConstantQueueAndEndDelay) {
  Config c;
  c.scenario = "constant";
  c.network_base_ms = 30;
  Pcm input(2 * kFrame, 1200);
  auto r = simulate(input, make_network(input.size(), c), c, false, 20);
  EXPECT_DOUBLE_EQ(r.summary.startup_ms, 90);
  EXPECT_NEAR(r.summary.end_mean_ms, 90, 1e-9);
  EXPECT_NEAR(r.summary.wait_mean_ms, 60 + 479.0 / 96, 1e-9);
  EXPECT_EQ(r.summary.played_source_samples, input.size());
}
TEST(Metrics, PacketDelayUsesActualSendTimeAndMissingBlocksStayUndefined) {
  Config c;
  Pcm input(2 * kFrame, 1200);
  std::vector<Packet> trace{{0, ms(20), ms(30), false},
                            {1, ms(10), ms(40), true}};
  auto r = simulate(input, trace, c, false, 20);
  uint64_t contributed = 0;
  for (const auto& tick : r.ticks) {
    const auto& d = tick.delays;
    if (d.samples) {
      EXPECT_EQ(d.samples, kFrame);
      EXPECT_DOUBLE_EQ(d.network_ms, 10);
      EXPECT_NEAR(d.wait_ms, 60 + 479.0 / 96, 1e-9);
      EXPECT_NEAR(d.playout_ms, 70 + 479.0 / 96, 1e-9);
    } else {
      EXPECT_TRUE(std::isnan(d.wait_ms));
      EXPECT_TRUE(std::isnan(d.playout_ms));
    }
    contributed += d.samples;
  }
  EXPECT_EQ(contributed, kFrame);
  EXPECT_EQ(contributed, r.summary.played_source_samples);
  EXPECT_NEAR(r.summary.packet_to_output_mean_ms, 70 + 479.0 / 96, 1e-9);
  EXPECT_NEAR(r.summary.end_mean_ms, 90, 1e-9);
}
TEST(Metrics, AdaptiveDelayDecompositionWeightsUniqueSourceContributions) {
  Config c;
  c.scenario = "burst";
  auto input = make_fixture(3);
  auto r = simulate(input, make_network(input.size(), c), c, true, 0);
  uint64_t samples = 0;
  double waiting = 0, network = 0, playout = 0;
  for (const auto& tick : r.ticks) {
    const auto& d = tick.delays;
    if (!d.samples) continue;
    EXPECT_NEAR(d.playout_ms, d.network_ms + d.wait_ms, 1e-8);
    EXPECT_GE(d.wait_ms, 0);
    samples += d.samples;
    waiting += d.wait_ms * d.samples;
    network += d.network_ms * d.samples;
    playout += d.playout_ms * d.samples;
  }
  ASSERT_GT(samples, 0u);
  EXPECT_EQ(samples, r.summary.played_source_samples);
  EXPECT_LE(samples, input.size());
  EXPECT_NEAR(waiting / samples, r.summary.wait_mean_ms, 1e-8);
  EXPECT_NEAR(network / samples, r.summary.played_network_mean_ms, 1e-8);
  EXPECT_NEAR(playout / samples, r.summary.packet_to_output_mean_ms, 1e-8);
}
}  // namespace aca::ajb
