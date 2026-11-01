#include "aca/ajb/core.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <random>
#include <stdexcept>

namespace aca::ajb {
namespace {
constexpr double kPi = 3.14159265358979323846;
const std::array<std::string, 6> kScenarios = {"constant", "jitter", "burst",
                                               "reorder",  "loss",   "phased"};
int16_t pcm(double value) {
  return static_cast<int16_t>(std::clamp(value, -32768.0, 32767.0));
}
double percentile(std::vector<double> values, double p = .95) {
  if (values.empty()) return std::numeric_limits<double>::quiet_NaN();
  size_t i = static_cast<size_t>(std::ceil(p * values.size())) - 1;
  std::nth_element(values.begin(), values.begin() + i, values.end());
  return values[i];
}
double mean(const std::vector<double>& values) {
  return values.empty() ? std::numeric_limits<double>::quiet_NaN()
                        : std::accumulate(values.begin(), values.end(), 0.0) /
                              values.size();
}
}  // namespace

void Config::validate() const {
  if (underrun_policy != "plc" && underrun_policy != "rebuffer")
    throw std::runtime_error("underrun-policy must be plc or rebuffer");
  if (rebuffer_resume_ms < 40 || rebuffer_resume_ms > 500 ||
      rebuffer_resume_ms % 10 || rebuffer_timeout_ms < 10 ||
      rebuffer_timeout_ms > 2000 || rebuffer_timeout_ms % 10)
    throw std::runtime_error(
        "Rebuffer resume must be 40..500ms, timeout 10..2000ms, in 10ms steps");
  if (std::find(kScenarios.begin(), kScenarios.end(), scenario) ==
      kScenarios.end())
    throw std::runtime_error(
        "Unknown scenario; use constant/jitter/burst/reorder/loss/phased");
  for (int n : {small_ms, large_ms, min_ms, initial_ms, max_ms})
    if (n < 0 || n > kCapacityMs - kLookaheadMs || n % 10)
      throw std::runtime_error("Delay must be a multiple of 10 ms in 0..460");
  if (small_ms >= large_ms)
    throw std::runtime_error("Set small-buffer-ms below large-buffer-ms");
  if (!(min_ms <= initial_ms && initial_ms <= max_ms))
    throw std::runtime_error(
        "Set min-delay-ms <= initial-delay-ms <= max-delay-ms");
  for (int n : {network_base_ms, jitter_ms, burst_jitter_ms, burst_extra_ms,
                reorder_extra_ms})
    if (n < 0 || n > 1000)
      throw std::runtime_error("Network delay values must be in 0..1000ms");
  if (jitter_ms % 10 || burst_jitter_ms % 10)
    throw std::runtime_error("Network jitter must be a multiple of 10ms");
  int extra = scenario == "jitter"    ? jitter_ms
              : scenario == "burst"   ? burst_jitter_ms + burst_extra_ms
              : scenario == "reorder" ? reorder_extra_ms
              : scenario == "phased"
                  ? std::max(jitter_ms, burst_jitter_ms + burst_extra_ms)
                  : 0;
  if (network_base_ms + extra > 1000)
    throw std::runtime_error(
        "Network delay exceeds 1000ms for this scene; reduce base/extra");
}

std::string display_safe(const std::string& value, size_t limit) {
  std::string out;
  size_t i = 0;
  while (i < value.size()) {
    auto first = static_cast<unsigned char>(value[i]);
    size_t n = first < 128                      ? 1
               : first >= 0xc2 && first <= 0xdf ? 2
               : first >= 0xe0 && first <= 0xef ? 3
               : first >= 0xf0 && first <= 0xf4 ? 4
                                                : 0;
    uint32_t point = n == 1 ? first : n ? first & ((1u << (7 - n)) - 1) : 0;
    bool valid = n && i + n <= value.size();
    for (size_t j = 1; valid && j < n; ++j) {
      auto next = static_cast<unsigned char>(value[i + j]);
      valid = (next & 0xc0) == 0x80;
      point = (point << 6) | (next & 0x3f);
    }
    valid = valid && !(n == 2 && point < 0x80) && !(n == 3 && point < 0x800) &&
            !(n == 4 && point < 0x10000) && point <= 0x10ffff &&
            !(point >= 0xd800 && point <= 0xdfff);
    bool control = point < 32 || (point >= 127 && point <= 159) ||
                   (point >= 0x202a && point <= 0x202e) ||
                   (point >= 0x2066 && point <= 0x2069);
    size_t length = valid && !control ? n : 1;
    if (out.size() + length > limit) break;
    if (valid && !control)
      out.append(value, i, n);
    else
      out += '?';
    i += valid ? n : 1;
  }
  if (i < value.size()) out += "...";
  return out;
}

std::string scenario_lesson(const std::string& name, bool ascii) {
  if (name == "constant")
    return ascii ? "Startup waiting buys stable output"
                 : "观察预缓冲等待如何换取稳定、零欠载的输出";
  if (name == "jitter")
    return ascii ? "Small buffer: gaps; large buffer: delay"
                 : "同一抖动：小缓冲容易缺帧，大缓冲增加等待";
  if (name == "burst")
    return ascii ? "A late packet cannot replace committed PLC"
                 : "突发超过余量：先补静音，晚到真包不能重播";
  if (name == "reorder")
    return ascii ? "Arrival order differs from media order"
                 : "包按到达顺序接收，按源序号排序并补齐缺洞";
  if (name == "loss")
    return ascii ? "More buffering cannot recover lost packets"
                 : "真正丢包不能靠增大缓冲修复；PLC不同于主动拉伸";
  return ascii ? "Target rises first; occupancy follows; recovery sheds delay"
               : "目标先升，水位缓慢跟上；网络恢复后加速消除延迟";
}

Pcm make_fixture(double seconds) {
  if (!std::isfinite(seconds) || seconds <= 0 || seconds > 300)
    throw std::runtime_error("Fixture duration must be in (0,300] seconds");
  Pcm out(static_cast<size_t>(seconds * kRate));
  std::mt19937 rng(42);
  for (size_t i = 0; i < out.size(); ++i) {
    double t = static_cast<double>(i) / kRate;
    double envelope = (.5 - .5 * std::cos(2 * kPi * 3.6 * t)) *
                      (std::fmod(t, 1.4) < 1.05 ? 1.0 : .02);
    double phase = 2 * kPi * (140 * t + .9 * std::sin(2 * kPi * .6 * t));
    double noise = (static_cast<double>(rng() % 2001) / 1000 - 1) * .015;
    out[i] = pcm(12000 * envelope *
                 (std::sin(phase) + .25 * std::sin(2 * phase) +
                  .12 * std::sin(2 * kPi * 730 * t) + noise));
  }
  return out;
}

std::vector<Packet> make_network(size_t samples, const Config& config) {
  config.validate();
  if (!samples || samples > kMaxSamples)
    throw std::runtime_error(
        "Audio must contain 1..14400000 mono samples (300s)");
  std::mt19937 rng(config.seed);
  std::vector<Packet> packets;
  for (size_t seq = 0; seq < (samples + kFrame - 1) / kFrame; ++seq) {
    int delay = config.network_base_ms;
    bool lost = false;
    const auto& scene = config.scenario;
    if (scene == "jitter" || (scene == "phased" && seq >= 500 && seq < 1000))
      delay += static_cast<int>(rng() % (config.jitter_ms / 10 + 1)) * 10;
    if (scene == "burst" || (scene == "phased" && seq >= 1000 && seq < 1500)) {
      delay += static_cast<int>(rng() % (config.burst_jitter_ms / 10 + 1)) * 10;
      if (seq >= 100 && seq % 50 < 10) delay += config.burst_extra_ms;
    }
    if (scene == "reorder" && seq % 12 == 0) delay += config.reorder_extra_ms;
    if (scene == "loss") lost = (rng() % 100 < 5);
    packets.push_back({seq, static_cast<Time>(seq) * kTick,
                       static_cast<Time>(seq) * kTick + ms(delay), lost});
  }
  return packets;
}

DelayEstimator::DelayEstimator(const Config& config)
    : config_(config), target_(config.initial_ms) {
  config.validate();
}
int DelayEstimator::arrive(Time now, Time send) {
  if (now < send || now - send > ms(1000))
    throw std::runtime_error("Packet transit must be within 0..1000ms");
  Time transit = now - send;
  if (observed_)
    jitter_ +=
        (std::abs(static_cast<double>(transit - previous_)) - jitter_) / 16;
  previous_ = transit;
  observed_ = true;
  baseline_ = std::min(baseline_, transit);
  observations_.emplace_back(now, transit);
  int peak =
      static_cast<int>((transit - baseline_ + ms(10) - 1) / ms(10)) * 10 + 10;
  if (peak > target_) {
    target_ = std::clamp(peak, config_.min_ms, config_.max_ms);
    last_decay_ = now;
  }
  return update(now);
}
int DelayEstimator::update(Time now) {
  while (!observations_.empty() && observations_.front().first < now - ms(2000))
    observations_.pop_front();
  int desired = config_.min_ms;
  if (!observations_.empty()) {
    std::vector<double> relative;
    for (auto [arrival, transit] : observations_) {
      (void)arrival;
      relative.push_back(static_cast<double>(transit - baseline_));
    }
    desired =
        static_cast<int>(std::ceil(percentile(relative) / ms(10))) * 10 + 10;
    desired = std::clamp(desired, config_.min_ms, config_.max_ms);
  }
  if (desired > target_) {
    target_ = desired;
    last_decay_ = now;
  }
  if (desired < target_ && now - last_decay_ >= ms(500)) {
    target_ = std::max(desired, target_ - 10);
    last_decay_ = now;
  }
  return target_;
}

void Wsola::reset() { ready_ = false; }
Wsola::Block Wsola::process(std::span<const int16_t> window, bool search,
                            int advance) {
  if (window.size() < 3 * kFrame)
    throw std::runtime_error("WSOLA needs 30ms of continuous lookahead");
  if (advance < 456 || advance > 504)
    throw std::runtime_error(
        "WSOLA analysis hop must stay in 456..504 samples");
  int offset = 0;
  if (ready_ && search) {
    double best = -2;
    for (int candidate = 0; candidate <= kFrame; candidate += 4) {
      double dot = 0, a = 1, b = 1;
      for (int i = 0; i < kFrame; i += 4) {
        double x = tail_[i], y = window[candidate + i];
        dot += x * y;
        a += x * x;
        b += y * y;
      }
      double score = dot / std::sqrt(a * b) - candidate * 1e-8;
      if (score > best) {
        best = score;
        offset = candidate;
      }
    }
  }
  Block out;
  for (int i = 0; i < kFrame; ++i) {
    double weight = ready_ ? static_cast<double>(i) / kFrame : 1;
    out.samples[i] = pcm((1 - weight) * tail_[i] + weight * window[offset + i]);
    // Both nonzero overlap contributions retain their source provenance.
    out.offsets[i] = ready_ ? tail_origins_[i] - previous_advance_ : offset + i;
    out.secondary_offsets[i] = ready_ && i > 0 ? offset + i : -1000000;
    tail_[i] = window[offset + kFrame + i];
    tail_origins_[i] = offset + kFrame + i;
  }
  ready_ = true;
  previous_advance_ = advance;
  return out;
}

Result simulate(const Pcm& input, const std::vector<Packet>& network,
                const Config& config, bool adaptive, int fixed_ms,
                const Cancel& cancel) {
  config.validate();
  if (input.empty() || input.size() > kMaxSamples || fixed_ms < 0 ||
      fixed_ms > 460 || fixed_ms % 10)
    throw std::runtime_error("Invalid audio length or fixed budget (0..460ms)");
  if (network.size() > 100000)
    throw std::runtime_error("Network trace exceeds 100000 events");
  Result result;
  result.adaptive = adaptive;
  result.budget_ms = adaptive ? config.initial_ms : fixed_ms;
  result.name = adaptive ? "adaptive" : "fixed-" + std::to_string(fixed_ms);
  std::vector<Packet> arrivals, sends = network;
  Time anchor = ms(1000);
  for (const auto& p : network) {
    if (p.seq >= (input.size() + kFrame - 1) / kFrame || p.send < 0 ||
        p.send > ms(300000) || p.arrival < p.send ||
        p.arrival - p.send > ms(1000))
      throw std::runtime_error(
          "Invalid network packet: check sequence and transit");
    if (!p.lost) {
      arrivals.push_back(p);
      anchor = std::min(anchor, p.arrival);
    }
  }
  auto order = [](const Packet& a, const Packet& b) {
    return a.arrival == b.arrival ? a.seq < b.seq : a.arrival < b.arrival;
  };
  std::stable_sort(arrivals.begin(), arrivals.end(), order);
  std::stable_sort(sends.begin(), sends.end(),
                   [](const Packet& a, const Packet& b) {
                     return a.send == b.send ? a.seq < b.seq : a.send < b.send;
                   });
  Time start =
      ((anchor + ms(result.budget_ms + kLookaheadMs) + kTick - 1) / kTick) *
      kTick;
  result.summary.startup_ms = milliseconds(start);
  Time send_end = static_cast<Time>(input.size()) * 1000000000 / kRate;
  Time limit = send_end + ms(1000 + 500 + 40 + 2000 +
                             (config.underrun_policy == "rebuffer" ? 5000 : 0));
  std::map<size_t, Packet> queue;
  std::set<size_t> received;
  std::vector<uint8_t> played(input.size(), 0);
  std::vector<Time> arrival_times((input.size() + kFrame - 1) / kFrame, -1);
  std::vector<Time> send_times(arrival_times.size(), -1);
  std::vector<double> waits, ends, targets, levels;
  double wait_sum = 0;
  double network_sum = 0, packet_output_sum = 0;
  Counters counts;
  DelayEstimator estimator(config);
  Wsola stretch;
  size_t source = 0, ai = 0, si = 0;
  int target = result.budget_ms;
  double level = 0;
  bool was_plc = false, rebuffering = false;
  Time rebuffer_started = 0;
  int16_t previous_output = 0;
  int fade_out = 0, fade_in = 0;
  int16_t fade_start = 0;
  auto event = [&](Time time, std::string kind, int64_t seq, std::string reason,
                   int advance = 0, int concealed = 0, bool net = false) {
    if (result.events.size() >= kMaxEvents)
      throw std::runtime_error(
          "Experiment event limit reached; shorten the input");
    result.events.push_back({time, result.events.size(), std::move(kind),
                             std::move(reason), seq, source, advance, target,
                             level, static_cast<uint32_t>(concealed), net});
    result.events.back().jitter_ms = estimator.jitter_ms();
  };
  auto sample = [&](size_t position) -> std::pair<int16_t, bool> {
    if (position >= input.size()) return {0, true};  // Normal tail padding.
    return queue.contains(position / kFrame)
               ? std::pair{input[position], true}
               : std::pair<int16_t, bool>{0, false};
  };
  for (Time now = 0; now <= limit; now += kTick) {
    if (cancel && cancel()) throw std::runtime_error("Experiment cancelled");
    while (si < sends.size() && sends[si].send <= now) {
      const auto& p = sends[si++];
      event(p.send, p.lost ? "loss" : "send", p.seq,
            p.lost ? "network_drop" : "source_frame_sent", 0, 0, true);
      if (p.lost) ++counts.network_lost;
    }
    while (ai < arrivals.size() && arrivals[ai].arrival <= now) {
      const auto& p = arrivals[ai++];
      event(p.arrival, "arrival", p.seq, "network_arrival", 0, 0, true);
      result.events.back().network_delay_ms = milliseconds(p.arrival - p.send);
      if (!received.insert(p.seq).second) {
        ++counts.duplicate;
        event(p.arrival, "duplicate", p.seq, "already_received");
        continue;
      }
      int old_target = target;
      arrival_times[p.seq] = p.arrival;
      send_times[p.seq] = p.send;
      int estimate = estimator.arrive(p.arrival, p.send);
      if (adaptive) target = estimate;
      event(p.arrival, "estimate", p.seq, "unique_arrival_including_late");
      if (old_target != target)
        event(p.arrival, "target", p.seq, "arrival_delay_changed");
      if ((p.seq + 1) * kFrame <= source) {
        ++counts.late;
        event(p.arrival, "late", p.seq, "source_interval_already_committed");
        continue;
      }
      if (p.seq * kFrame > source + kCapacityMs * 48) {
        ++counts.overflow;
        event(p.arrival, "overflow", p.seq, "outside_source_capacity_span");
        continue;
      }
      queue.emplace(p.seq, p);
      event(p.arrival, "admit", p.seq, "ordered_insert");
      while (queue.size() * kFrame > kCapacityMs * 48) {
        auto last = std::prev(queue.end());
        ++counts.overflow;
        event(p.arrival, "overflow", last->first, "drop_farthest_future");
        queue.erase(last);
      }
    }
    if (now < start) {
      result.audio.insert(result.audio.end(), kFrame, 0);
      result.ticks.push_back({now, 0, 0, target, 0, 0, estimator.jitter_ms(),
                              "startup", 0, counts, 0});
      continue;
    }
    if (source >= input.size()) break;
    int old = target;
    if (adaptive) target = estimator.update(now);
    if (old != target) event(now, "target", -1, "stable_network_slow_decay");
    size_t continuous = source;
    while (continuous < input.size() &&
           continuous - source < kCapacityMs * 48 &&
           queue.contains(continuous / kFrame)) {
      continuous = std::min(input.size(), (continuous / kFrame + 1) * kFrame);
    }
    level = static_cast<double>(continuous - source) / 48;
    double span =
        queue.empty()
            ? 0
            : std::max(0.0, static_cast<double>((queue.rbegin()->first + 1) *
                                                kFrame) /
                                    48 -
                                static_cast<double>(source) / 48);
    // Waiting emits device-clock silence but does not consume source audio.
    // A finite per-gap timeout and a 5s total budget handle permanent loss.
    constexpr uint64_t max_rebuffer_samples = 5 * kRate;
    if (config.underrun_policy == "rebuffer" && !rebuffering &&
        continuous < std::min(input.size(), source + kFrame) &&
        counts.rebuffer_samples < max_rebuffer_samples) {
      rebuffering = true;
      rebuffer_started = now;
      ++counts.rebuffer_events;
      stretch.reset();
      event(now, "rebuffer_start", -1, "source_unavailable");
    }
    if (rebuffering) {
      const size_t needed =
          std::min(input.size() - source,
                   static_cast<size_t>(std::max(config.rebuffer_resume_ms,
                                                target + kLookaheadMs)) *
                       48);
      const bool ready = continuous - source >= needed;
      const bool timeout =
          now - rebuffer_started >= ms(config.rebuffer_timeout_ms) ||
          counts.rebuffer_samples >= max_rebuffer_samples;
      if (ready || timeout) {
        rebuffering = false;
        if (!ready) ++counts.rebuffer_timeouts;
        event(now, "rebuffer_end", -1,
              ready ? "level_recovered" : "timeout_fallback_plc");
        fade_in = 240;
      } else {
        for (int i = 0; i < kFrame; ++i)
          result.audio.push_back(
              i < 240 ? static_cast<int16_t>(previous_output * (239 - i) / 240)
                      : 0);
        previous_output = 0;
        was_plc = false;
        fade_out = 0;
        counts.rebuffer_samples += kFrame;
        event(now, "output", -1, "rebuffer");
        result.ticks.push_back({now, source, 0, target, level, span,
                                estimator.jitter_ms(), "rebuffer", 0, counts,
                                waits.empty() ? 0 : wait_sum / waits.size()});
        targets.push_back(target);
        levels.push_back(level);
        continue;
      }
    }
    int advance = kFrame;
    std::string action = "normal";
    bool window_ready = continuous - source >= 4 * kFrame;
    if (adaptive && window_ready) {
      if (level > target + kLookaheadMs + 10) {
        advance = 504;
        action = "fast";
      }
      if (level < target + kLookaheadMs - 10 && now < send_end) {
        advance = 456;
        action = "slow";
      }
    }
    std::array<int16_t, 4 * kFrame> window{};
    for (size_t i = 0; i < window.size(); ++i)
      window[i] = sample(source + i).first;
    Wsola::Block block;
    if (window_ready) {
      block = stretch.process(window, adaptive, advance);
    } else {
      stretch.reset();
      for (int i = 0; i < kFrame; ++i) {
        block.samples[i] = window[i];
        block.offsets[i] = i;
        block.secondary_offsets[i] = -1000000;
      }
    }
    int concealed = 0;
    DelayStats delays;
    double block_network = 0, block_wait = 0, block_playout = 0;
    bool in_gap = was_plc;
    for (int i = 0; i < kFrame; ++i) {
      size_t origin =
          static_cast<size_t>(static_cast<int64_t>(source) + block.offsets[i]);
      auto [value, available] = sample(origin);
      if (block.offsets[i] < 0) available = true;  // Retained WSOLA history.
      (void)value;
      if (!available) {
        ++concealed;
        // A short fade reaches silence; following missing samples remain zero.
        if (!in_gap) {
          ++counts.plc_events;
          in_gap = true;
          fade_out = 240;
          fade_start = previous_output;
        }
        block.samples[i] = pcm(fade_start * (fade_out / 240.0));
        if (fade_out) --fade_out;
      } else {
        if (in_gap) fade_in = 240;
        if (fade_in) {
          block.samples[i] = pcm(block.samples[i] * (1 - fade_in / 240.0));
          --fade_in;
        }
        in_gap = false;
        for (int relative : {block.offsets[i], block.secondary_offsets[i]}) {
          if (relative == -1000000) continue;
          auto contribution = static_cast<int64_t>(source) + relative;
          if (contribution < 0 ||
              contribution >= static_cast<int64_t>(input.size()))
            continue;
          size_t position = static_cast<size_t>(contribution);
          if (!played[position]) {
            played[position] = 1;
            Time arrival = arrival_times[position / kFrame];
            if (arrival < 0)
              throw std::runtime_error("WSOLA referenced media before arrival");
            double wait = milliseconds(now - arrival) + i / 48.0;
            double network =
                milliseconds(arrival - send_times[position / kFrame]);
            double playout = wait + network;
            double end = milliseconds(now) + i / 48.0 - position / 48.0;
            waits.push_back(wait);
            ends.push_back(end);
            wait_sum += wait;
            ++delays.samples;
            block_network += network;
            block_wait += wait;
            block_playout += playout;
            network_sum += network;
            packet_output_sum += playout;
          }
        }
      }
      previous_output = block.samples[i];
    }
    was_plc = in_gap;
    if (concealed) {
      action = "plc";
      counts.plc_samples += concealed;
      ++counts.plc_ticks;
    }
    ++counts.media_ticks;
    if (action == "fast")
      ++counts.fast;
    else if (action == "slow")
      ++counts.slow;
    else if (action == "normal")
      ++counts.normal;
    result.audio.insert(result.audio.end(), block.samples.begin(),
                        block.samples.end());
    event(now, "output", static_cast<int64_t>(source / kFrame), action, advance,
          concealed);
    result.ticks.push_back({now, source, advance, target, level, span,
                            estimator.jitter_ms(), action, concealed, counts,
                            waits.empty() ? 0 : wait_sum / waits.size()});
    if (delays.samples) {
      delays.network_ms = block_network / delays.samples;
      delays.wait_ms = block_wait / delays.samples;
      delays.playout_ms = block_playout / delays.samples;
    }
    result.ticks.back().delays = delays;
    targets.push_back(target);
    levels.push_back(level);
    source +=
        static_cast<size_t>(advance);  // Includes finite final-frame padding.
    event(now, "commit", -1, "actual_source_cursor", advance);
    while (!queue.empty() && (queue.begin()->first + 1) * kFrame <= source) {
      event(now, "consume", queue.begin()->first, "source_cursor_advanced");
      queue.erase(queue.begin());
    }
  }
  if (source < input.size())
    throw std::runtime_error(
        "Drain deadline exceeded; inspect experiment trace");
  // Classify late arrivals in the bounded remaining network tail without adding
  // PLC.
  while (ai < arrivals.size()) {
    const auto& p = arrivals[ai++];
    event(p.arrival, "arrival", p.seq, "drain_arrival", 0, 0, true);
    result.events.back().network_delay_ms = milliseconds(p.arrival - p.send);
    if (!received.insert(p.seq).second) {
      ++counts.duplicate;
      event(p.arrival, "duplicate", p.seq, "already_received");
    } else {
      int old_target = target;
      int estimate = estimator.arrive(p.arrival, p.send);
      if (adaptive) target = estimate;
      event(p.arrival, "estimate", p.seq, "unique_arrival_including_late");
      if (old_target != target)
        event(p.arrival, "target", p.seq, "arrival_delay_changed");
      ++counts.late;
      event(p.arrival, "late", p.seq, "after_media_end");
    }
  }
  std::stable_sort(
      result.events.begin(), result.events.end(),
      [](const Event& a, const Event& b) { return a.time < b.time; });
  for (size_t i = 0; i < result.events.size(); ++i) result.events[i].order = i;
  if (!result.events.empty()) {
    Time end = (result.events.back().time / kTick + 1) * kTick;
    const size_t recorded = result.events.size();
    size_t prefix = 0;
    Counters drain_counts = counts;
    drain_counts.late = drain_counts.duplicate = drain_counts.overflow =
        drain_counts.network_lost = 0;
    int observed_target = result.budget_ms;
    double observed_jitter = 0;
    while (result.duration() < end) {
      Time now = result.duration();
      while (prefix < recorded && result.events[prefix].time <= now) {
        const auto& e = result.events[prefix++];
        if (e.kind == "late") ++drain_counts.late;
        if (e.kind == "duplicate") ++drain_counts.duplicate;
        if (e.kind == "overflow") ++drain_counts.overflow;
        if (e.kind == "loss") ++drain_counts.network_lost;
        if (e.kind == "target") observed_target = e.target_ms;
        if (e.kind == "estimate") observed_jitter = e.jitter_ms;
      }
      result.audio.insert(result.audio.end(), kFrame, 0);
      result.ticks.push_back({now, source, 0, observed_target, 0, 0,
                              observed_jitter, "drain", 0, drain_counts,
                              waits.empty() ? 0 : wait_sum / waits.size()});
      event(now, "output", -1, "drain");
      result.events.back().target_ms = observed_target;
      result.events.back().jitter_ms = observed_jitter;
    }
  }
  std::stable_sort(
      result.events.begin(), result.events.end(),
      [](const Event& a, const Event& b) { return a.time < b.time; });
  for (size_t i = 0; i < result.events.size(); ++i) result.events[i].order = i;
  auto& summary = result.summary;
  summary.counters = counts;
  summary.played_source_samples = waits.size();
  if (!waits.empty()) {
    summary.played_network_mean_ms = network_sum / waits.size();
    summary.packet_to_output_mean_ms = packet_output_sum / waits.size();
  }
  summary.unplayed_fraction =
      1 - static_cast<double>(waits.size()) / input.size();
  summary.wait_mean_ms = mean(waits);
  summary.wait_p95_ms = percentile(waits);
  summary.end_mean_ms = mean(ends);
  summary.end_p95_ms = percentile(ends);
  summary.target_mean_ms = mean(targets);
  summary.target_p95_ms = percentile(targets);
  summary.level_mean_ms = mean(levels);
  summary.level_p95_ms = percentile(levels);
  build_checkpoints(result);
  return result;
}

void build_checkpoints(Result& result) {
  std::set<size_t> queue;
  Counters received;
  Time next = 0;
  result.checkpoints.clear();
  result.checkpoints.push_back({0, {}, {}});
  for (size_t i = 0; i < result.events.size(); ++i) {
    const auto& e = result.events[i];
    if (e.kind == "admit") queue.insert(static_cast<size_t>(e.packet));
    if (e.kind == "consume" || e.kind == "overflow")
      queue.erase(static_cast<size_t>(e.packet));
    if (e.kind == "late") ++received.late;
    if (e.kind == "duplicate") ++received.duplicate;
    if (e.kind == "overflow") ++received.overflow;
    if (e.kind == "loss") ++received.network_lost;
    if (e.time >= next) {
      result.checkpoints.push_back({i + 1, queue, received});
      next = e.time + ms(1000);
    }
  }
}
}  // namespace aca::ajb
