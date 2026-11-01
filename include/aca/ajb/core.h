#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace aca::ajb {
using Time = int64_t;  // Nanoseconds on the simulated clock.
using Pcm = std::vector<int16_t>;
inline constexpr int kRate = 48000;
inline constexpr int kFrame = 480;
inline constexpr Time kTick = 10000000;
inline constexpr int kLookaheadMs = 40;
inline constexpr int kCapacityMs = 500;
inline constexpr size_t kMaxSamples = 300 * kRate;
inline constexpr size_t kMaxEvents = 1200000;
inline constexpr double kNoDelay = std::numeric_limits<double>::quiet_NaN();
inline constexpr Time ms(int64_t value) { return value * 1000000; }
inline double milliseconds(Time value) { return value / 1e6; }

struct Config {
  std::string scenario = "phased";
  uint32_t seed = 42;
  std::string underrun_policy = "plc";
  int rebuffer_resume_ms = 100, rebuffer_timeout_ms = 300;
  int small_ms = 20, large_ms = 150;
  int min_ms = 40, initial_ms = 60, max_ms = 200;
  int network_base_ms = 100, jitter_ms = 120;
  int burst_jitter_ms = 30, burst_extra_ms = 160, reorder_extra_ms = 100;
  void validate() const;
  bool operator==(const Config&) const = default;
};
struct Packet {
  size_t seq = 0;
  Time send = 0, arrival = 0;
  bool lost = false;
  bool operator==(const Packet&) const = default;
};
std::vector<Packet> make_network(size_t samples, const Config& config);
Pcm make_fixture(double seconds = 30.0);
std::string scenario_lesson(const std::string& name, bool ascii = false);
std::string display_safe(const std::string& text, size_t limit = 240);

class DelayEstimator {
 public:
  explicit DelayEstimator(const Config& config);
  int arrive(Time now, Time send);
  int update(Time now);
  int target() const { return target_; }
  double jitter_ms() const { return jitter_ / 1e6; }

 private:
  Config config_;
  std::deque<std::pair<Time, Time>> observations_;
  Time baseline_ = std::numeric_limits<Time>::max();
  Time previous_ = 0, last_decay_ = 0;
  double jitter_ = 0;
  int target_ = 0;
  bool observed_ = false;
};

// A 20 ms overlap window with a 10 ms synthesis hop. Search is read-only;
// only the caller's bounded analysis hop commits the media cursor.
class Wsola {
 public:
  struct Block {
    std::array<int16_t, kFrame> samples{};
    std::array<int, kFrame> offsets{};
    std::array<int, kFrame> secondary_offsets{};
  };
  Block process(std::span<const int16_t> window, bool search,
                int advance = 480);
  void reset();

 private:
  std::array<int16_t, kFrame> tail_{};
  std::array<int, kFrame> tail_origins_{};
  bool ready_ = false;
  int previous_advance_ = 480;
};

struct Counters {
  uint64_t late = 0, duplicate = 0, overflow = 0, network_lost = 0;
  uint64_t plc_samples = 0, plc_events = 0, plc_ticks = 0;
  uint64_t fast = 0, slow = 0, normal = 0, media_ticks = 0;
  uint64_t rebuffer_samples = 0, rebuffer_events = 0, rebuffer_timeouts = 0;
};
struct Event {
  Time time = 0;
  uint64_t order = 0;
  std::string kind, reason;
  int64_t packet = -1;
  size_t source = 0;
  int advance = 0, target_ms = 0;
  double level_ms = 0;
  uint32_t concealed = 0;
  bool network = false;
  double jitter_ms = 0;
  double network_delay_ms = kNoDelay;
};
struct DelayStats {
  uint32_t samples = 0;
  double network_ms = kNoDelay, wait_ms = kNoDelay, playout_ms = kNoDelay;
};
struct Tick {
  Time time = 0;
  size_t source = 0;
  int advance = 0, target_ms = 0;
  double level_ms = 0, span_ms = 0, jitter_ms = 0;
  std::string action;
  int concealed = 0;
  Counters cumulative;
  double wait_mean_ms = 0;
  DelayStats delays{};
};
struct Summary {
  Counters counters;
  double startup_ms = 0, wait_mean_ms = 0, wait_p95_ms = 0;
  double end_mean_ms = 0, end_p95_ms = 0;
  double target_mean_ms = 0, target_p95_ms = 0;
  double level_mean_ms = 0, level_p95_ms = 0;
  double unplayed_fraction = 1;
  uint64_t played_source_samples = 0;
  double played_network_mean_ms = kNoDelay, packet_to_output_mean_ms = kNoDelay;
};
struct Checkpoint {
  size_t event_index = 0;
  std::set<size_t> queue;
  Counters received;
};
struct Result {
  std::string name;
  bool adaptive = false;
  int budget_ms = 0;
  Pcm audio;
  std::vector<Tick> ticks;
  std::vector<Event> events;
  std::vector<Checkpoint> checkpoints;
  Summary summary;
  Time duration() const {
    return static_cast<Time>(audio.size()) * 1000000000 / kRate;
  }
};
using Cancel = std::function<bool()>;
Result simulate(const Pcm& input, const std::vector<Packet>& network,
                const Config& config, bool adaptive, int fixed_ms,
                const Cancel& cancel = {});
void build_checkpoints(Result& result);
}  // namespace aca::ajb
