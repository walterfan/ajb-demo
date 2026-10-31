#include <unistd.h>

#include <csignal>
#include <cstdlib>
#include <iostream>

#include "aca/ajb/app.h"

namespace {
volatile std::sig_atomic_t interrupted = 0;
void signal_handler(int) { interrupted = 1; }
}  // namespace
int main(int argc, char** argv) {
  try {
    std::vector<std::string> args(argv + 1, argv + argc);
    auto options = aca::ajb::parse_options(args);
    if (options.help) {
      std::cout << aca::ajb::help_text();
      return 0;
    }
    aca::ajb::check_media_elements();
    if (!options.fixture.empty()) {
      aca::ajb::write_wav(options.fixture, aca::ajb::make_fixture());
      return 0;
    }
    bool built = false;
#ifdef AJB_HAVE_TUI
    built = true;
#endif
    const char* term = std::getenv("TERM");
    bool terminal = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO) && term &&
                    std::string(term) != "dumb";
    if (aca::ajb::use_tui(options, built, terminal)) {
#ifdef AJB_HAVE_TUI
      return aca::ajb::run_tui(options);
#endif
    }
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    auto cancel = [] { return interrupted != 0; };
    auto experiment = aca::ajb::prepare(options, cancel);
    std::cout << "Input normalized: 48kHz mono S16LE, 480 samples/10ms\n"
              << aca::ajb::summary_text(*experiment);
    if (options.play) aca::ajb::audition(*experiment, cancel);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "ajb: " << aca::ajb::display_safe(error.what(), 600) << '\n';
    return 1;
  }
}
