#pragma once

#include <filesystem>
#include <memory>
#include <ostream>

#include "aca/ajb/media.h"
#include "aca/ajb/replay.h"

namespace aca::ajb {
struct Options {
  Config config;
  std::filesystem::path input = "assets/real/mandarin.wav", output = "out", fixture;
  std::string mode = "compare", ui = "plain";
  int fixed_ms = 20;
  bool play = false, ascii = false, no_color = false, help = false;
};
Options parse_options(const std::vector<std::string>& args);
std::string help_text();
bool use_tui(const Options& options, bool built, bool terminal);
struct Experiment {
  std::string id;
  std::filesystem::path directory;
  Config config;
  Result reference;
  std::vector<Packet> network;
  std::vector<Result> results;
};
using Progress = std::function<void(const std::string&)>;
std::shared_ptr<Experiment> prepare(const Options& options,
                                    const Cancel& cancel = {},
                                    const Progress& progress = {});
std::string summary_text(const Experiment& experiment);
void save_snapshot(const Experiment& experiment, const Result& result,
                   Cursor cursor, const std::string& content);
void audition(const Experiment& experiment, const Cancel& cancel = {});
int run_tui(const Options& options);
}  // namespace aca::ajb
