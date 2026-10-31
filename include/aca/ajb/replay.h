#pragma once

#include "aca/ajb/core.h"

namespace aca::ajb {
struct Cursor {
  Time time = 0;
  uint64_t order = std::numeric_limits<uint64_t>::max();
};
struct View {
  Cursor cursor;
  std::set<size_t> queue;
  Tick tick;
  size_t prefix = 0;
  size_t source_cursor = 0;
  Time output_time = -1;
  std::vector<Event> recent;
};
View view_at(const Result& result, Cursor cursor);
struct DelayPoint {
  Time time = 0;
  double value = kNoDelay;
};
struct DelayView {
  Tick tick;
  int64_t packet = -1;
  Time arrival = 0;
  double last_network_ms = kNoDelay;
  std::vector<DelayPoint> network, waiting, playout;
};
DelayView delays_at(const Result& result, Cursor cursor);
class Replay {
 public:
  void attach(const Result* result);
  void seek(Time time);
  void next_tick();
  void next_event();
  void set_speed(double speed);
  void advance(Time wall_delta, Time audio_position = -1);
  void pause() { paused = true; }
  void play() { paused = false; }
  Cursor cursor() const { return cursor_; }
  double speed() const { return speed_; }
  bool silent() const { return paused || speed_ != 1 || !audio_available; }
  bool ended() const;
  bool paused = true;
  bool audio_available = false;

 private:
  const Result* result_ = nullptr;
  Cursor cursor_;
  double speed_ = 1;
};
}  // namespace aca::ajb
