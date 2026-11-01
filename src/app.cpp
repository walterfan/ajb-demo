#include "aca/ajb/app.h"

#include <charconv>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace aca::ajb {
namespace {
template <class T>
T number(const std::string& value) {
  T n{};
  auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), n);
  if (error != std::errc{} || end != value.data() + value.size())
    throw std::runtime_error(
        "Use an integer within the documented option range");
  return n;
}
std::ofstream output_file(const std::filesystem::path& path) {
  checked_path(path, false);
  std::ofstream file(path);
  if (!file)
    throw std::runtime_error(
        "Cannot create result file; check output permissions");
  file.exceptions(std::ios::badbit | std::ios::failbit);
  file << std::setprecision(12);
  return file;
}
std::filesystem::path new_directory(const std::filesystem::path& root) {
  auto path = checked_path(root, false);
  std::filesystem::create_directories(path);
  for (int i = 1; i <= 100000; ++i) {
    auto run = path / ("run-" + std::to_string(i));
    if (std::filesystem::create_directory(run)) {
      std::filesystem::permissions(run, std::filesystem::perms::owner_all);
      return run;
    }
  }
  throw std::runtime_error(
      "Output has too many runs; choose another directory");
}
void write_result(const Result& result, const std::filesystem::path& directory,
                  const Cancel& cancel) {
  write_wav(directory / (result.name + ".wav"), result.audio, cancel);
  auto file = output_file(directory / (result.name + ".csv"));
  file << "output_ns,source_sample,consumed_samples,target_budget_ms,target_"
          "level_ms,continuous_level_ms,span_ms,jitter_ms,action,concealed_"
          "samples,plc_total_ms,plc_events,wait_mean_ms,delay_samples,played_"
          "network_ms,buffer_wait_ms,packet_to_output_ms,rebuffer_total_ms,"
          "rebuffer_events,rebuffer_timeouts\n";
  for (const auto& t : result.ticks)
    file << t.time << ',' << t.source << ',' << t.advance << ',' << t.target_ms
         << ',' << t.target_ms + kLookaheadMs << ',' << t.level_ms << ','
         << t.span_ms << ',' << t.jitter_ms << ',' << t.action << ','
         << t.concealed << ',' << t.cumulative.plc_samples / 48.0 << ','
         << t.cumulative.plc_events << ',' << t.wait_mean_ms << ','
         << t.delays.samples << ',' << t.delays.network_ms << ','
         << t.delays.wait_ms << ',' << t.delays.playout_ms << ','
         << t.cumulative.rebuffer_samples / 48.0 << ','
         << t.cumulative.rebuffer_events << ','
         << t.cumulative.rebuffer_timeouts << '\n';
  file.close();
}
}  // namespace

Options parse_options(const std::vector<std::string>& args) {
  Options options;
  bool ui_given = false;
  for (size_t i = 0; i < args.size(); ++i) {
    const auto& key = args[i];
    auto value = [&]() -> const std::string& {
      if (++i >= args.size())
        throw std::runtime_error("Missing value for " + key + "; see --help");
      return args[i];
    };
    auto ui = [&](std::string name) {
      if (ui_given)
        throw std::runtime_error("Use only one of --ui, --tui and --no-tui");
      ui_given = true;
      options.ui = std::move(name);
    };
    if (key == "--help" || key == "-h")
      options.help = true;
    else if (key == "--input")
      options.input = value();
    else if (key == "--output-dir")
      options.output = value();
    else if (key == "--generate-fixture")
      options.fixture = value();
    else if (key == "--mode")
      options.mode = value();
    else if (key == "--ui")
      ui(value());
    else if (key == "--tui")
      ui("tui");
    else if (key == "--no-tui")
      ui("plain");
    else if (key == "--play")
      options.play = true;
    else if (key == "--ascii")
      options.ascii = true;
    else if (key == "--no-color")
      options.no_color = true;
    else if (key == "--scenario")
      options.config.scenario = value();
    else if (key == "--underrun-policy")
      options.config.underrun_policy = value();
    else if (key == "--rebuffer-resume-ms")
      options.config.rebuffer_resume_ms = number<int>(value());
    else if (key == "--rebuffer-timeout-ms")
      options.config.rebuffer_timeout_ms = number<int>(value());
    else if (key == "--seed")
      options.config.seed = number<uint32_t>(value());
    else if (key == "--network-delay-ms")
      options.config.network_base_ms = number<int>(value());
    else if (key == "--jitter-ms")
      options.config.jitter_ms = number<int>(value());
    else if (key == "--burst-jitter-ms")
      options.config.burst_jitter_ms = number<int>(value());
    else if (key == "--burst-delay-ms")
      options.config.burst_extra_ms = number<int>(value());
    else if (key == "--reorder-delay-ms")
      options.config.reorder_extra_ms = number<int>(value());
    else if (key == "--buffer-ms")
      options.fixed_ms = number<int>(value());
    else if (key == "--small-buffer-ms")
      options.config.small_ms = number<int>(value());
    else if (key == "--large-buffer-ms")
      options.config.large_ms = number<int>(value());
    else if (key == "--min-delay-ms")
      options.config.min_ms = number<int>(value());
    else if (key == "--initial-delay-ms")
      options.config.initial_ms = number<int>(value());
    else if (key == "--max-delay-ms")
      options.config.max_ms = number<int>(value());
    else
      throw std::runtime_error("Unknown option " + display_safe(key) +
                               "; see --help");
  }
  options.config.validate();
  if (options.fixed_ms < 0 || options.fixed_ms > 460 || options.fixed_ms % 10)
    throw std::runtime_error("buffer-ms must be a multiple of 10 in 0..460");
  if (options.mode != "fixed" && options.mode != "adaptive" &&
      options.mode != "compare")
    throw std::runtime_error("mode must be fixed, adaptive or compare");
  if (options.ui != "plain" && options.ui != "tui" && options.ui != "auto")
    throw std::runtime_error("ui must be auto, tui or plain");
  if (options.play && options.ui != "plain")
    throw std::runtime_error(
        "Use --no-tui --play for text audition; the TUI controls its own "
        "audio");
  return options;
}
std::string help_text() {
  return R"(AJB Lab: deterministic jitter-buffer experiment
  --underrun-policy plc|rebuffer (default plc)
  --rebuffer-resume-ms 100 --rebuffer-timeout-ms 300
  --input FILE.wav --output-dir DIR (default input: assets/real/mandarin.wav)
  --mode compare|fixed|adaptive  --buffer-ms 20
  --scenario constant|jitter|burst|reorder|loss|phased --seed 42
  --network-delay-ms 100 --jitter-ms 120 --burst-jitter-ms 30
  --burst-delay-ms 160 --reorder-delay-ms 100
  --small-buffer-ms 20 --large-buffer-ms 150
  --min-delay-ms 40 --initial-delay-ms 60 --max-delay-ms 200
  --ui auto|tui|plain  (default plain; aliases --tui / --no-tui)
  --play             text audition, only with plain UI
  --ascii --no-color --help
  --generate-fixture FILE.wav
All budgets: multiples of 10 ms in 0..460; min <= initial <= max.
Network delays: integers in 0..1000ms; jitter limits are multiples of 10ms.
The selected scenario's base + maximum extra delay must not exceed 1000ms.
WAV input is normalized to 48kHz, mono, S16LE (max 128MiB/300s).
)";
}
bool use_tui(const Options& options, bool built, bool terminal) {
  if (options.ui == "tui" && (!built || !terminal))
    throw std::runtime_error(
        "TUI requires an interactive terminal and TUI-enabled build; use "
        "--no-tui");
  return options.ui != "plain" && built && terminal;
}
std::shared_ptr<Experiment> prepare(const Options& options,
                                    const Cancel& cancel,
                                    const Progress& progress) {
  options.config.validate();
  auto notify = [&](const std::string& message) {
    if (cancel && cancel()) throw std::runtime_error("Experiment cancelled");
    if (progress) progress(message);
  };
  notify("Read WAV / normalize 48kHz mono S16LE");
  auto experiment = std::make_shared<Experiment>();
  experiment->config = options.config;
  experiment->reference.name = "reference";
  experiment->reference.audio = read_wav(options.input, cancel);
  experiment->network =
      make_network(experiment->reference.audio.size(), options.config);
  experiment->directory = new_directory(options.output);
  experiment->id = experiment->directory.filename().string();
  auto directory = experiment->directory;
  bool success = false;
  try {
    auto network_file = output_file(directory / "network.csv");
    network_file << "sequence,send_ns,arrival_ns,network_lost\n";
    for (const auto& p : experiment->network)
      network_file << p.seq << ',' << p.send << ',' << p.arrival << ','
                   << p.lost << '\n';
    network_file.close();
    auto add = [&](bool adaptive, int budget) {
      notify(adaptive ? "Render adaptive"
                      : "Render fixed " + std::to_string(budget) + "ms");
      auto result = simulate(experiment->reference.audio, experiment->network,
                             options.config, adaptive, budget, cancel);
      write_result(result, directory, cancel);
      experiment->results.push_back(std::move(result));
    };
    if (options.mode == "fixed")
      add(false, options.fixed_ms);
    else if (options.mode == "adaptive")
      add(true, 0);
    else {
      add(false, options.config.small_ms);
      add(false, options.config.large_ms);
      add(true, 0);
    }
    auto events = output_file(directory / "events.csv");
    events << "policy,event_order,time_ns,kind,reason,packet,source_sample,"
              "advance_samples,target_budget_ms,level_ms,concealed_samples,"
              "network_event,jitter_ms,network_delay_ms\n";
    for (const auto& r : experiment->results)
      for (const auto& e : r.events)
        events << r.name << ',' << e.order << ',' << e.time << ',' << e.kind
               << ',' << e.reason << ',' << e.packet << ',' << e.source << ','
               << e.advance << ',' << e.target_ms << ',' << e.level_ms << ','
               << e.concealed << ',' << e.network << ',' << e.jitter_ms << ','
               << e.network_delay_ms << '\n';
    events.close();
    auto summary = output_file(directory / "summary.csv");
    summary << "policy,startup_ms,lookahead_ms,wait_mean_ms,wait_p95_ms,send_"
               "to_output_mean_ms,send_to_output_p95_ms,target_budget_mean_ms,"
               "target_budget_p95_ms,level_mean_ms,level_p95_ms,plc_ms,plc_"
               "events,plc_ticks,late,duplicate,overflow,network_lost,fast_"
               "fraction,slow_fraction,unplayed_source_fraction,played_network_"
               "mean_ms,packet_to_output_mean_ms,rebuffer_ms,rebuffer_events,"
               "rebuffer_timeouts\n";
    for (const auto& r : experiment->results) {
      const auto& s = r.summary;
      const auto& c = s.counters;
      double denominator = std::max<uint64_t>(1, c.media_ticks);
      summary << r.name << ',' << s.startup_ms << ',' << kLookaheadMs << ','
              << s.wait_mean_ms << ',' << s.wait_p95_ms << ',' << s.end_mean_ms
              << ',' << s.end_p95_ms << ',' << s.target_mean_ms << ','
              << s.target_p95_ms << ',' << s.level_mean_ms << ','
              << s.level_p95_ms << ',' << c.plc_samples / 48.0 << ','
              << c.plc_events << ',' << c.plc_ticks << ',' << c.late << ','
              << c.duplicate << ',' << c.overflow << ',' << c.network_lost
              << ',' << c.fast / denominator << ',' << c.slow / denominator
              << ',' << s.unplayed_fraction << ',' << s.played_network_mean_ms
              << ',' << s.packet_to_output_mean_ms << ','
              << c.rebuffer_samples / 48.0 << ',' << c.rebuffer_events << ','
              << c.rebuffer_timeouts << '\n';
    }
    summary.close();
    auto metadata = output_file(directory / "experiment.txt");
    metadata << "run=" << experiment->id << "\nmode=" << options.mode
             << "\nfixed_ms=" << options.fixed_ms
             << "\nscenario=" << options.config.scenario
             << "\nunderrun_policy=" << options.config.underrun_policy
             << "\nrebuffer_resume_ms=" << options.config.rebuffer_resume_ms
             << "\nrebuffer_timeout_ms=" << options.config.rebuffer_timeout_ms
             << "\nrebuffer_limit_ms=5000"
             << "\nseed=" << options.config.seed
             << "\nnetwork_base_ms=" << options.config.network_base_ms
             << "\njitter_ms=" << options.config.jitter_ms
             << "\nburst_jitter_ms=" << options.config.burst_jitter_ms
             << "\nburst_extra_ms=" << options.config.burst_extra_ms
             << "\nreorder_extra_ms=" << options.config.reorder_extra_ms
             << "\nformat=S16LE/48000/mono\nframe_ms=10\n"
             << "small_ms=" << options.config.small_ms
             << "\nlarge_ms=" << options.config.large_ms
             << "\nmin_ms=" << options.config.min_ms
             << "\ninitial_ms=" << options.config.initial_ms
             << "\nmax_ms=" << options.config.max_ms << "\nlookahead_ms=40\n"
             << "device_latency=not_measured\n";
    metadata.close();
    success = true;
  } catch (...) {
    if (!success) {
      std::error_code ignored;
      std::filesystem::remove_all(directory, ignored);
    }
    throw;
  }
  notify("Ready");
  return experiment;
}
std::string summary_text(const Experiment& experiment) {
  std::ostringstream out;
  out << "run=" << experiment.id << " scenario=" << experiment.config.scenario
      << " seed=" << experiment.config.seed
      << " underrun=" << experiment.config.underrun_policy << "\n"
      << "policy           startup_ms   wait_mean_ms   PLC_ms   Rebuffer_ms  "
         "late\n";
  for (const auto& r : experiment.results)
    out << std::setw(16) << std::left << r.name << std::fixed
        << std::setprecision(2) << std::setw(13) << r.summary.startup_ms
        << std::setw(15) << r.summary.wait_mean_ms << std::setw(9)
        << r.summary.counters.plc_samples / 48.0 << std::setw(13)
        << r.summary.counters.rebuffer_samples / 48.0 << r.summary.counters.late
        << '\n';
  out << "Output: " << display_safe(experiment.directory.string()) << '\n'
      << "Simulation delays include 40ms lookahead; device acoustic latency is "
         "not measured.\n";
  return out.str();
}
void save_snapshot(const Experiment& experiment, const Result& result,
                   Cursor cursor, const std::string& content) {
  auto file = output_file(
      experiment.directory /
      ("snapshot-" + result.name + "-" + std::to_string(cursor.time) + ".txt"));
  file << "run=" << experiment.id << " policy=" << result.name
       << " time_ns=" << cursor.time << " event_order=" << cursor.order << "\n"
       << "seed=" << experiment.config.seed
       << " scenario=" << experiment.config.scenario << '\n'
       << content;
  file.close();
}
void audition(const Experiment& experiment, const Cancel& cancel) {
  auto play = [&](const Result& result) {
    std::cout << "Playing: " << result.name << std::endl;
    Playback player;
    player.start(result.audio);
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::nanoseconds(result.duration()) +
                    std::chrono::seconds(10);
    while (!player.ended()) {
      if ((cancel && cancel()) || std::chrono::steady_clock::now() > deadline)
        throw std::runtime_error(
            "Playback interrupted/timed out; use make compare for offline "
            "results");
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  };
  try {
    play(experiment.reference);
    for (const auto& result : experiment.results) play(result);
  } catch (const std::exception& error) {
    throw std::runtime_error(
        std::string(error.what()) +
        "; rendered results are preserved; use make compare");
  }
}
}  // namespace aca::ajb
