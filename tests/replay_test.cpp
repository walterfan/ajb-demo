#include "aca/ajb/replay.h"

#include <gtest/gtest.h>

#include <cmath>

namespace aca::ajb {
TEST(Replay, SameTimeEventsHavePrefixes) {
  Result r;
  r.audio.resize(3 * kFrame);
  r.events = {{ms(1), 0, "arrival", "", 1},
              {ms(1), 1, "admit", "", 1},
              {ms(1), 2, "admit", "", 2},
              {ms(10), 3, "consume", "", 1}};
  build_checkpoints(r);
  Replay replay;
  replay.attach(&r);
  replay.next_event();
  EXPECT_TRUE(view_at(r, replay.cursor()).queue.empty());
  replay.next_event();
  EXPECT_EQ(view_at(r, replay.cursor()).queue, std::set<size_t>{1});
  replay.next_event();
  EXPECT_EQ(view_at(r, replay.cursor()).queue.size(), 2u);
  replay.next_tick();
  EXPECT_EQ(replay.cursor().time, ms(10));
  EXPECT_EQ(view_at(r, replay.cursor()).queue, std::set<size_t>{2});
}
TEST(Replay, AudioClockAndSilentStepping) {
  Result r;
  r.audio.resize(kRate);
  Replay p;
  p.attach(&r);
  p.audio_available = true;
  p.play();
  p.advance(ms(40), ms(10));
  EXPECT_EQ(p.cursor().time, ms(10));
  p.pause();
  p.advance(ms(40), ms(20));
  EXPECT_EQ(p.cursor().time, ms(10));
  p.next_tick();
  EXPECT_TRUE(p.silent());
  EXPECT_EQ(p.cursor().time, ms(20));
  p.set_speed(.25);
  p.play();
  p.advance(ms(40));
  EXPECT_EQ(p.cursor().time, ms(30));
  EXPECT_TRUE(p.silent());
  p.seek(ms(100000));
  EXPECT_TRUE(p.ended());
}
TEST(Replay, CheckpointMatchesEventPrefix) {
  Config c;
  auto input = make_fixture(3);
  auto r = simulate(input, make_network(input.size(), c), c, true, 0);
  auto full = view_at(r, {ms(2345)});
  r.checkpoints.clear();
  EXPECT_EQ(full.queue, view_at(r, {ms(2345)}).queue);
  EXPECT_EQ(full.prefix, view_at(r, {ms(2345)}).prefix);
}
TEST(Replay, LateTailCountersBecomeVisibleAtTheirEvent) {
  Config c;
  Pcm input(2 * kFrame, 1000);
  std::vector<Packet> network = {{0, 0, ms(30), false},
                                 {1, ms(10), ms(900), false}};
  auto r = simulate(input, network, c, false, 20);
  EXPECT_EQ(view_at(r, {ms(899)}).tick.cumulative.late, 0u);
  auto late = std::find_if(r.events.begin(), r.events.end(),
                           [](const Event& e) { return e.kind == "late"; });
  ASSERT_NE(late, r.events.end());
  EXPECT_EQ(view_at(r, {late->time, late->order - 1}).tick.cumulative.late, 0u);
  auto after = view_at(r, {late->time, late->order});
  EXPECT_EQ(after.tick.cumulative.late, 1u);
  EXPECT_EQ(after.tick.action, "drain");
  r.checkpoints.clear();
  EXPECT_EQ(view_at(r, {late->time, late->order}).tick.cumulative.late,
            after.tick.cumulative.late);
  EXPECT_EQ(view_at(r, {r.duration()}).tick.cumulative.late,
            r.summary.counters.late);
  auto adaptive = simulate(input, network, c, true, 0);
  EXPECT_LT(view_at(adaptive, {ms(899)}).tick.target_ms, c.max_ms);
  EXPECT_EQ(view_at(adaptive, {ms(900)}).tick.target_ms, c.max_ms);
  EXPECT_EQ(adaptive.ticks.back().cumulative.late, 1u);
  EXPECT_EQ(adaptive.ticks.back().target_ms, c.max_ms);
}
TEST(Replay, DelaysRespectArrivalAndOutputEventPrefixes) {
  Config c;
  Pcm input(2 * kFrame, 1200);
  auto r =
      simulate(input, {{0, ms(20), ms(30), false}, {1, ms(10), ms(40), false}},
               c, false, 20);
  auto arrival =
      std::find_if(r.events.begin(), r.events.end(),
                   [](const Event& e) { return e.kind == "arrival"; });
  ASSERT_NE(arrival, r.events.end());
  auto before = delays_at(r, {arrival->time, arrival->order - 1});
  EXPECT_TRUE(before.network.empty());
  EXPECT_TRUE(std::isnan(before.last_network_ms));
  auto seen = delays_at(r, {arrival->time, arrival->order});
  ASSERT_EQ(seen.network.size(), 1u);
  EXPECT_DOUBLE_EQ(seen.last_network_ms, 10);
  EXPECT_TRUE(seen.playout.empty());
  auto output = std::find_if(r.events.begin(), r.events.end(),
                             [](const Event& e) { return e.kind == "output"; });
  ASSERT_NE(output, r.events.end());
  auto uncommitted = delays_at(r, {output->time, output->order - 1});
  EXPECT_EQ(uncommitted.tick.delays.samples, 0u);
  EXPECT_TRUE(uncommitted.playout.empty());
  auto committed = delays_at(r, {output->time, output->order});
  EXPECT_EQ(committed.tick.delays.samples, kFrame);
  EXPECT_DOUBLE_EQ(committed.last_network_ms,
                   30);  // Newest arrival is packet 1.
  EXPECT_DOUBLE_EQ(committed.tick.delays.network_ms, 10);  // Playing packet 0.
  ASSERT_FALSE(committed.playout.empty());
  EXPECT_NEAR(committed.playout.back().value, 70 + 479.0 / 96, 1e-9);
}
TEST(Replay, DelayWindowContainsOnlyTheLastFiveSeconds) {
  Config c;
  c.scenario = "constant";
  auto input = make_fixture(6);
  auto r = simulate(input, make_network(input.size(), c), c, false, 20);
  auto d = delays_at(r, {ms(5600)});
  EXPECT_FALSE(d.network.empty());
  EXPECT_FALSE(d.playout.empty());
  for (const auto* points : {&d.network, &d.waiting, &d.playout})
    for (const auto& point : *points) {
      EXPECT_GE(point.time, ms(600));
      EXPECT_LE(point.time, ms(5600));
    }
}
TEST(Replay, RebufferKeepsCursorAndCountersConsistent) {
  Config c;
  c.scenario = "constant";
  c.underrun_policy = "rebuffer";
  Pcm input(50 * kFrame, 1000);
  auto trace = make_network(input.size(), c);
  trace[10].arrival = ms(400);
  auto result = simulate(input, trace, c, false, 20);
  for (const auto& tick : result.ticks) {
    if (tick.action != "rebuffer") continue;
    auto view = view_at(result, {tick.time});
    EXPECT_EQ(view.source_cursor, tick.source);
    EXPECT_EQ(view.tick.cumulative.rebuffer_samples,
              tick.cumulative.rebuffer_samples);
    EXPECT_TRUE(std::isnan(delays_at(result, {tick.time}).tick.delays.wait_ms));
  }
}
}  // namespace aca::ajb
