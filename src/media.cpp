#include "aca/ajb/media.h"

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/audio/audio.h>
#include <gst/gst.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace aca::ajb {
namespace {
using Clock = std::chrono::steady_clock;
void initialize() {
  static std::once_flag once;
  std::call_once(once, [] {
    GError* error = nullptr;
    if (!gst_init_check(nullptr, nullptr, &error)) {
      std::string text =
          error ? error->message : "unknown initialization error";
      if (error) g_error_free(error);
      throw std::runtime_error("GStreamer initialization failed: " +
                               display_safe(text));
    }
  });
}
struct Pipeline {
  GstElement* root = nullptr;
  GstBus* bus = nullptr;
  bool playback_warnings = false;
  Pipeline() {
    initialize();
    root = gst_pipeline_new(nullptr);
    if (!root) throw std::runtime_error("Cannot allocate GStreamer pipeline");
    bus = gst_element_get_bus(root);
  }
  ~Pipeline() {
    if (root) {
      gst_element_set_state(root, GST_STATE_NULL);
      gst_object_unref(root);
    }
    if (bus) gst_object_unref(bus);
  }
  GstElement* add(const char* factory) {
    auto* element = gst_element_factory_make(factory, nullptr);
    if (!element)
      throw std::runtime_error(
          std::string("Missing GStreamer element ") + factory +
          "; run make doctor and install the system plugins");
    if (!gst_bin_add(GST_BIN(root), element)) {
      gst_object_unref(element);
      throw std::runtime_error("Cannot add media element to pipeline");
    }
    return element;
  }
  bool poll(bool wait_eos = false) {
    GstMessage* message = gst_bus_timed_pop_filtered(
        bus, wait_eos ? 20 * GST_SECOND : 0,
        static_cast<GstMessageType>(
            GST_MESSAGE_ERROR | GST_MESSAGE_EOS |
            (playback_warnings ? GST_MESSAGE_WARNING : 0)));
    if (!message) {
      if (wait_eos)
        throw std::runtime_error(
            "GStreamer EOS timed out; check output device/file");
      return false;
    }
    bool eos = GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS;
    if (!eos) {
      GError* error = nullptr;
      gchar* debug = nullptr;
      if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_WARNING)
        gst_message_parse_warning(message, &error, &debug);
      else
        gst_message_parse_error(message, &error, &debug);
      std::string text = error ? error->message : "unknown media error";
      if (error) g_error_free(error);
      g_free(debug);
      gst_message_unref(message);
      throw std::runtime_error("GStreamer: " + display_safe(text));
    }
    gst_message_unref(message);
    return true;
  }
  void state(GstState target) {
    if (gst_element_set_state(root, target) == GST_STATE_CHANGE_FAILURE) {
      poll();
      throw std::runtime_error(
          "GStreamer state change failed; check plugins/device");
    }
  }
};
void link(std::initializer_list<GstElement*> elements) {
  if (elements.size() < 2) return;
  auto previous = elements.begin();
  for (auto next = std::next(previous); next != elements.end();
       ++next, ++previous)
    if (!gst_element_link(*previous, *next))
      throw std::runtime_error(
          "Cannot link GStreamer elements; check media plugins");
}
GstCaps* raw_caps() {
  return gst_caps_new_simple("audio/x-raw", "format", G_TYPE_STRING, "S16LE",
                             "layout", G_TYPE_STRING, "interleaved", "rate",
                             G_TYPE_INT, kRate, "channels", G_TYPE_INT, 1,
                             nullptr);
}
struct ReadMapping {
  GstBuffer* buffer;
  GstMapInfo info{};
  explicit ReadMapping(GstBuffer* value) : buffer(value) {
    if (!buffer || !gst_buffer_map(buffer, &info, GST_MAP_READ))
      throw std::runtime_error("Cannot map decoded WAV buffer");
  }
  ~ReadMapping() { gst_buffer_unmap(buffer, &info); }
  ReadMapping(const ReadMapping&) = delete;
  ReadMapping& operator=(const ReadMapping&) = delete;
};
void setup_source(GstElement* source) {
  auto* caps = raw_caps();
  gst_app_src_set_caps(GST_APP_SRC(source), caps);
  gst_caps_unref(caps);
  g_object_set(source, "format", GST_FORMAT_TIME, "is-live", FALSE, "block",
               FALSE, "max-bytes", static_cast<guint64>(kFrame * 2 * 8),
               nullptr);
}
void push(Pipeline& pipeline, GstElement* source,
          std::span<const int16_t> samples, size_t position,
          const Cancel& cancel, const Cancel& paused = {}) {
  auto deadline = Clock::now() + std::chrono::seconds(10);
  while (gst_app_src_get_current_level_bytes(GST_APP_SRC(source)) >=
         kFrame * 2 * 8) {
    if (cancel && cancel())
      throw std::runtime_error("Media operation cancelled");
    pipeline.poll();
    if (paused && paused()) deadline = Clock::now() + std::chrono::seconds(10);
    if (Clock::now() > deadline)
      throw std::runtime_error(
          "Media queue stalled for 10s; check device/file");
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  if (cancel && cancel()) throw std::runtime_error("Media operation cancelled");
  GstBuffer* buffer =
      gst_buffer_new_allocate(nullptr, samples.size_bytes(), nullptr);
  if (!buffer) throw std::runtime_error("Cannot allocate media frame");
  GstMapInfo map{};
  if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
    gst_buffer_unref(buffer);
    throw std::runtime_error("Cannot map output audio frame");
  }
  for (size_t i = 0; i < samples.size(); ++i)
    GST_WRITE_UINT16_LE(map.data + 2 * i, static_cast<uint16_t>(samples[i]));
  gst_buffer_unmap(buffer, &map);
  GST_BUFFER_PTS(buffer) = static_cast<guint64>(position) * GST_SECOND / kRate;
  GST_BUFFER_DURATION(buffer) =
      static_cast<guint64>(samples.size()) * GST_SECOND / kRate;
  if (gst_app_src_push_buffer(GST_APP_SRC(source), buffer) != GST_FLOW_OK) {
    pipeline.poll();
    throw std::runtime_error("Audio output stopped accepting frames");
  }
}
}  // namespace

std::filesystem::path checked_path(const std::filesystem::path& input,
                                   bool must_exist) {
  if (input.empty())
    throw std::runtime_error("Specify a nonempty local file path");
  for (const auto& part : input)
    if (part == "..")
      throw std::runtime_error("Use an absolute path without '..' components");
  auto absolute = std::filesystem::absolute(input).lexically_normal();
  std::filesystem::path current;
  for (const auto& part : absolute) {
    current /= part;
    std::error_code error;
    auto status = std::filesystem::symlink_status(current, error);
    if (!error && std::filesystem::is_symlink(status))
      throw std::runtime_error(
          "Use a direct path; symbolic links are not accepted");
    if (error && error != std::errc::no_such_file_or_directory)
      throw std::runtime_error("Cannot inspect local path; check permissions");
  }
  if (must_exist && !std::filesystem::is_regular_file(absolute))
    throw std::runtime_error("Input must be an existing regular WAV file");
  return absolute;
}
void check_distinct(const std::filesystem::path& input,
                    const std::filesystem::path& output) {
  auto a = checked_path(input, true), b = checked_path(output, false);
  if (a == b ||
      (std::filesystem::exists(b) && std::filesystem::equivalent(a, b)))
    throw std::runtime_error("Choose an output file different from the input");
}
void check_media_elements() {
  initialize();
  for (const char* name :
       {"filesrc", "wavparse", "audioconvert", "audioresample", "appsink",
        "appsrc", "wavenc", "filesink", "autoaudiosink"}) {
    auto* factory = gst_element_factory_find(name);
    if (!factory)
      throw std::runtime_error(std::string("Missing ") + name +
                               "; run make doctor");
    gst_object_unref(factory);
  }
}
Pcm read_wav(const std::filesystem::path& input, const Cancel& cancel) {
  auto path = checked_path(input, true);
  if (std::filesystem::file_size(path) > 128 * 1024 * 1024)
    throw std::runtime_error("WAV exceeds 128 MiB; provide a smaller clip");
  std::array<char, 12> header{};
  std::ifstream file(path, std::ios::binary);
  file.read(header.data(), header.size());
  if (!file || std::string(header.data(), 4) != "RIFF" ||
      std::string(header.data() + 8, 4) != "WAVE")
    throw std::runtime_error(
        "Input must be a RIFF/WAVE file; convert it to PCM WAV");
  Pipeline pipeline;
  auto* source = pipeline.add("filesrc");
  auto* parser = pipeline.add("wavparse");
  auto* convert = pipeline.add("audioconvert");
  auto* resample = pipeline.add("audioresample");
  auto* filter = pipeline.add("capsfilter");
  auto* sink = pipeline.add("appsink");
  g_object_set(source, "location", path.c_str(), nullptr);
  auto* caps = raw_caps();
  g_object_set(filter, "caps", caps, nullptr);
  gst_caps_unref(caps);
  g_object_set(sink, "sync", FALSE, "max-buffers", 8u, "drop", FALSE,
               "enable-last-sample", FALSE, nullptr);
  link({source, parser, convert, resample, filter, sink});
  pipeline.state(GST_STATE_PLAYING);
  Pcm result;
  auto deadline = Clock::now() + std::chrono::seconds(60);
  for (;;) {
    if (cancel && cancel()) throw std::runtime_error("WAV reading cancelled");
    if (Clock::now() > deadline)
      throw std::runtime_error(
          "WAV decode exceeded 60s; provide a shorter valid clip");
    auto* sample =
        gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 200 * GST_MSECOND);
    if (!sample) {
      pipeline.poll();
      if (gst_app_sink_is_eos(GST_APP_SINK(sink))) break;
      continue;
    }
    std::unique_ptr<GstSample, decltype(&gst_sample_unref)> owned(
        sample, gst_sample_unref);
    auto* buffer = gst_sample_get_buffer(sample);
    GstAudioInfo info{};
    if (!gst_audio_info_from_caps(&info, gst_sample_get_caps(sample)) ||
        GST_AUDIO_INFO_FORMAT(&info) != GST_AUDIO_FORMAT_S16LE ||
        GST_AUDIO_INFO_RATE(&info) != kRate ||
        GST_AUDIO_INFO_CHANNELS(&info) != 1)
      throw std::runtime_error("GStreamer did not negotiate 48kHz/mono/S16LE");
    ReadMapping mapped(buffer);
    const auto& map = mapped.info;
    size_t count = map.size / 2;
    if (map.size % 2 || count > kMaxSamples - result.size()) {
      throw std::runtime_error(
          "Decoded WAV exceeds 300 seconds or contains a partial sample");
    }
    for (size_t i = 0; i < count; ++i)
      result.push_back(
          static_cast<int16_t>(GST_READ_UINT16_LE(map.data + 2 * i)));
  }
  if (result.empty())
    throw std::runtime_error("WAV has no audio; provide a nonempty clip");
  return result;
}
void write_wav(const std::filesystem::path& output, const Pcm& samples,
               const Cancel& cancel) {
  auto path = checked_path(output, false);
  if (samples.empty() || samples.size() > kMaxSamples + 5 * kRate)
    throw std::runtime_error("Output audio length is outside the 305s bound");
  // A private sibling avoids leaving a success-looking WAV after an error.
  auto temporary = path;
  temporary += ".partial";
  checked_path(temporary, false);
  if (std::filesystem::exists(temporary))
    throw std::runtime_error(
        "Partial output exists; choose a fresh output directory");
  try {
    Pipeline pipeline;
    auto* source = pipeline.add("appsrc");
    auto* encoder = pipeline.add("wavenc");
    auto* sink = pipeline.add("filesink");
    setup_source(source);
    g_object_set(sink, "location", temporary.c_str(), "sync", FALSE, nullptr);
    link({source, encoder, sink});
    pipeline.state(GST_STATE_PLAYING);
    for (size_t i = 0; i < samples.size(); i += kFrame)
      push(pipeline, source,
           std::span(samples).subspan(
               i, std::min<size_t>(kFrame, samples.size() - i)),
           i, cancel);
    if (gst_app_src_end_of_stream(GST_APP_SRC(source)) != GST_FLOW_OK)
      throw std::runtime_error("Cannot finish WAV stream");
    pipeline.poll(true);
    pipeline.state(GST_STATE_NULL);
    std::filesystem::rename(temporary, path);
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    throw;
  }
}

struct Playback::Impl {
  explicit Impl(bool silent) : silent(silent) {}
  bool silent = false;
  std::unique_ptr<Pipeline> pipeline;
  std::jthread worker;
  std::atomic<bool> stopping{false}, eos{false}, paused{false};
  Time origin = 0, last = 0;
  std::mutex mutex;
  std::string failure;
};
Playback::Playback(bool silent) : impl_(std::make_unique<Impl>(silent)) {}
Playback::~Playback() { stop(); }
void Playback::stop() {
  auto& p = *impl_;
  p.stopping = true;
  if (p.pipeline) gst_element_set_state(p.pipeline->root, GST_STATE_NULL);
  if (p.worker.joinable()) p.worker.join();
  p.pipeline.reset();
}
void Playback::start(const Pcm& samples, Time origin) {
  stop();
  auto& p = *impl_;
  p.origin = std::clamp(origin, Time{0},
                        static_cast<Time>(samples.size()) * GST_SECOND / kRate);
  p.last = p.origin;
  p.eos = false;
  p.stopping = false;
  p.paused = false;
  {
    std::lock_guard lock(p.mutex);
    p.failure.clear();
  }
  p.pipeline = std::make_unique<Pipeline>();
  // autoaudiosink can fall back to fakesink with only a warning. Report that
  // failure so the UI explicitly switches to visual playback.
  p.pipeline->playback_warnings = !p.silent;
  auto* source = p.pipeline->add("appsrc");
  auto* convert = p.pipeline->add("audioconvert");
  auto* sink = p.pipeline->add(p.silent ? "fakesink" : "autoaudiosink");
  setup_source(source);
  g_object_set(sink, "sync", TRUE, nullptr);
  link({source, convert, sink});
  p.pipeline->state(GST_STATE_PLAYING);
  size_t offset = static_cast<size_t>(p.origin) * kRate / GST_SECOND;
  p.worker = std::jthread([&p, &samples, source, offset] {
    try {
      for (size_t i = offset; i < samples.size() && !p.stopping; i += kFrame)
        push(
            *p.pipeline, source,
            std::span(samples).subspan(
                i, std::min<size_t>(kFrame, samples.size() - i)),
            i - offset, [&p] { return p.stopping.load(); },
            [&p] { return p.paused.load(); });
      if (!p.stopping &&
          gst_app_src_end_of_stream(GST_APP_SRC(source)) != GST_FLOW_OK)
        throw std::runtime_error("Playback EOS failed");
    } catch (const std::exception& error) {
      if (!p.stopping) {
        std::lock_guard lock(p.mutex);
        p.failure = error.what();
      }
    }
  });
}
void Playback::pause() {
  if (!impl_->pipeline) return;
  impl_->paused = true;
  impl_->pipeline->state(GST_STATE_PAUSED);
  GstState state;
  auto change = gst_element_get_state(impl_->pipeline->root, &state, nullptr,
                                      2 * GST_SECOND);
  if (change == GST_STATE_CHANGE_FAILURE || state != GST_STATE_PAUSED)
    throw std::runtime_error("Audio pause did not complete within 2s");
  position();
}
void Playback::resume() {
  if (impl_->pipeline) {
    impl_->paused = false;
    impl_->pipeline->state(GST_STATE_PLAYING);
  }
}
Time Playback::position() {
  auto& p = *impl_;
  if (!p.pipeline) return p.last;
  if (p.pipeline->poll()) p.eos = true;
  gint64 position = 0;
  if (gst_element_query_position(p.pipeline->root, GST_FORMAT_TIME, &position))
    p.last = p.origin + std::max<Time>(0, position);
  auto failure = error();
  if (!failure.empty()) throw std::runtime_error(failure);
  return p.last;
}
bool Playback::ended() {
  if (impl_->pipeline) position();
  return impl_->eos;
}
std::string Playback::error() {
  std::lock_guard lock(impl_->mutex);
  return impl_->failure;
}
}  // namespace aca::ajb
