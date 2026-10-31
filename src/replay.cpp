#include "aca/ajb/replay.h"

#include <algorithm>
#include <stdexcept>

namespace aca::ajb {
View view_at(const Result& result, Cursor cursor) {
  View view;
  view.cursor = cursor;
  auto after = std::upper_bound(
      result.events.begin(), result.events.end(), cursor,
      [](const Cursor& c, const Event& e) {
        return c.time < e.time || (c.time == e.time && c.order < e.order);
      });
  view.prefix = static_cast<size_t>(after - result.events.begin());
  size_t start = 0;
  Counters received;
  for (const auto& checkpoint : result.checkpoints) {
    if (checkpoint.event_index > view.prefix) break;
    start = checkpoint.event_index;
    view.queue = checkpoint.queue;
    received = checkpoint.received;
  }
  for (size_t i = start; i < view.prefix; ++i) {
    const auto& e = result.events[i];
    if (e.kind == "admit") view.queue.insert(static_cast<size_t>(e.packet));
    if (e.kind == "consume" || e.kind == "overflow")
      view.queue.erase(static_cast<size_t>(e.packet));
    if (e.kind == "late") ++received.late;
    if (e.kind == "duplicate") ++received.duplicate;
    if (e.kind == "overflow") ++received.overflow;
    if (e.kind == "loss") ++received.network_lost;
  }
  // Ticks become visible only after their actual output decision event.
  Time committed = -1;
  for (size_t i = view.prefix; i > 0; --i) {
    if (result.events[i - 1].kind == "output") {
      committed = result.events[i - 1].time;
      break;
    }
  }
  view.output_time = committed;
  for (size_t i = view.prefix; i > 0; --i)
    if (result.events[i - 1].kind == "commit") {
      view.source_cursor = result.events[i - 1].source;
      break;
    }
  auto tick = std::upper_bound(
      result.ticks.begin(), result.ticks.end(), committed,
      [](Time time, const Tick& item) { return time < item.time; });
  if (tick != result.ticks.begin())
    view.tick = *std::prev(tick);
  else {
    view.tick.target_ms = result.budget_ms;
    view.tick.action = "startup";
  }
  view.tick.target_ms = result.budget_ms;
  view.tick.jitter_ms = 0;
  for (size_t i = view.prefix; i > 0; --i)
    if (result.events[i - 1].kind == "estimate") {
      view.tick.jitter_ms = result.events[i - 1].jitter_ms;
      break;
    }
  for (size_t i = view.prefix; i > 0; --i)
    if (result.events[i - 1].kind == "target") {
      view.tick.target_ms = result.events[i - 1].target_ms;
      break;
    }
  for (size_t i = view.prefix > 12 ? view.prefix - 12 : 0; i < view.prefix; ++i)
    view.recent.push_back(result.events[i]);
  view.tick.cumulative.late = received.late;
  view.tick.cumulative.duplicate = received.duplicate;
  view.tick.cumulative.overflow = received.overflow;
  view.tick.cumulative.network_lost = received.network_lost;
  return view;
}
DelayView delays_at(const Result& result, Cursor cursor) {
  DelayView delays;
  const auto view = view_at(result, cursor);
  delays.tick = view.tick;
  const Time begin = std::max<Time>(0, cursor.time - ms(5000));
  for (size_t i = view.prefix; i > 0; --i) {
    const auto& e = result.events[i - 1];
    if (e.kind == "arrival") {
      if (delays.packet < 0) {
        delays.packet = e.packet;
        delays.arrival = e.time;
        delays.last_network_ms = e.network_delay_ms;
      }
      if (e.time >= begin)
        delays.network.push_back({e.time, e.network_delay_ms});
    }
    if (e.time < begin && delays.packet >= 0) break;
  }
  std::reverse(delays.network.begin(), delays.network.end());
  auto tick =
      std::lower_bound(result.ticks.begin(), result.ticks.end(), begin,
                       [](const Tick& t, Time time) { return t.time < time; });
  for (; tick != result.ticks.end() && tick->time <= view.output_time; ++tick) {
    delays.waiting.push_back({tick->time, tick->delays.wait_ms});
    delays.playout.push_back({tick->time, tick->delays.playout_ms});
  }
  return delays;
}
void Replay::attach(const Result* result) {
  result_ = result;
  cursor_ = {};
  paused = true;
}
void Replay::seek(Time time) {
  if (!result_) return;
  cursor_ = {std::clamp(time, Time{0}, result_->duration()),
             std::numeric_limits<uint64_t>::max()};
}
void Replay::next_tick() {
  paused = true;
  seek((cursor_.time / kTick + 1) * kTick);
}
void Replay::next_event() {
  paused = true;
  if (!result_) return;
  auto view = view_at(*result_, cursor_);
  if (view.prefix < result_->events.size()) {
    const auto& e = result_->events[view.prefix];
    cursor_ = {e.time, e.order};
  }
}
void Replay::set_speed(double speed) {
  if (speed != .25 && speed != .5 && speed != 1 && speed != 2)
    throw std::runtime_error("Replay speed must be .25, .5, 1 or 2");
  speed_ = speed;
}
void Replay::advance(Time wall_delta, Time audio_position) {
  if (paused || !result_) return;
  if (wall_delta < 0) throw std::runtime_error("Replay clock moved backwards");
  if (audio_available && speed_ == 1 && audio_position >= 0)
    seek(audio_position);
  else
    seek(cursor_.time + static_cast<Time>(wall_delta * speed_));
  if (ended()) paused = true;
}
bool Replay::ended() const {
  return result_ && cursor_.time >= result_->duration();
}
}  // namespace aca::ajb
