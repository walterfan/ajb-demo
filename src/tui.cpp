#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/loop.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>

#include "aca/ajb/app.h"

namespace aca::ajb {
namespace {
using namespace ftxui;
using Clock = std::chrono::steady_clock;
std::string decimal(double value, int precision = 1) {
  std::ostringstream s;
  s << std::fixed << std::setprecision(precision) << value;
  return s.str();
}
std::string delay_text(double value) {
  return std::isfinite(value) ? decimal(value) : "--";
}
bool focus_next(const Components& choices, const ftxui::Event& event) {
  if (choices.empty() ||
      (event != ftxui::Event::Tab && event != ftxui::Event::TabReverse))
    return false;
  auto current = std::find_if(choices.begin(), choices.end(),
                              [](const auto& c) { return c->Focused(); });
  int index = current == choices.end() ? 0 : current - choices.begin();
  int count = static_cast<int>(choices.size());
  choices[(index + (event == ftxui::Event::Tab ? 1 : count - 1)) % count]
      ->TakeFocus();
  return true;
}
struct Job {
  std::mutex mutex;
  std::string progress, error;
  std::shared_ptr<Experiment> result;
  bool done = false;
  std::atomic<bool> cancel{false};
};
// Apply fallbacks after widgets have rendered, including their own borders.
class DisplayFallback : public Node {
 public:
  DisplayFallback(Element child, bool ascii, bool monochrome)
      : Node({std::move(child)}), ascii_(ascii), monochrome_(monochrome) {}
  void ComputeRequirement() override {
    children_[0]->ComputeRequirement();
    requirement_ = children_[0]->requirement();
  }
  void SetBox(Box box) override {
    box_ = box;
    children_[0]->SetBox(box);
  }
  void Render(Screen& screen) override {
    children_[0]->Render(screen);
    for (int y = std::max(0, box_.y_min);
         y <= std::min(screen.dimy() - 1, box_.y_max); ++y)
      for (int x = std::max(0, box_.x_min);
           x <= std::min(screen.dimx() - 1, box_.x_max); ++x) {
        auto& cell = screen.PixelAt(x, y);
        if (monochrome_)
          cell.foreground_color = cell.background_color = Color::Default;
        if (!ascii_) continue;
        cell.automerge = false;
        if (cell.character == "─" || cell.character == "═" ||
            cell.character == "━")
          cell.character = "-";
        else if (cell.character == "│" || cell.character == "║" ||
                 cell.character == "┃")
          cell.character = "|";
        else if (cell.character == "▸" || cell.character == "▶")
          cell.character = ">";
        else if (!cell.character.empty() &&
                 static_cast<unsigned char>(cell.character[0]) >= 128)
          cell.character = "+";
      }
  }

 private:
  bool ascii_, monochrome_;
};
class Lab {
 public:
  explicit Lab(Options initial)
      : options_(std::move(initial)), screen_(ScreenInteractive::Fullscreen()) {
    no_color_ = options_.no_color || std::getenv("NO_COLOR") != nullptr;
    if (options_.ascii) {
      auto quirks = Terminal::GetQuirks();
      quirks.SetComponentAscii(true);
      quirks.SetBlockCharacters(false);
      Terminal::SetQuirks(quirks);
    }
    if (no_color_) Terminal::SetColorSupport(Terminal::Palette1);
    pages_ = {tr("总览", "Overview"), tr("机制", "Mechanism"),
              tr("结果", "Results"), tr("延迟", "Delay")};
    scene_names_ = {"constant", "jitter", "burst", "reorder", "loss", "phased"};
    policy_names_ = {
        tr("三策略对比", "Compare all"), tr("小固定缓冲", "Fixed small"),
        tr("大固定缓冲", "Fixed large"), tr("自适应（单独）", "Adaptive only"),
        tr("自定义固定缓冲", "Fixed custom")};
    auto start = Button(tr("开始/暂停", "Start/Pause"), [this] { toggle(); });
    auto restart =
        Button(tr("重播", "Replay"), [this] { select_track(track_); });
    auto edit =
        Button(tr("策略/参数", "Test/Parameters"), [this] { edit_form(); });
    auto quit = Button(tr("退出", "Quit"), [this] { screen_.Exit(); });
    tabs_ = Toggle(&pages_, &page_);
    guide_ =
        Checkbox(tr("关键事件暂停讲解", "Pause at key events"), &teaching_);
    timeline_ =
        Slider(tr("实验时间(ms): ", "Time(ms): "), &scrub_ms_, 0, 305000, 1000);
    navigation_ = Container::Vertical(
        {tabs_, Container::Horizontal({start, restart, edit, quit}), timeline_,
         guide_});
    menu_ = Menu(&event_labels_, &selected_event_);
    main_focus_ = {tabs_, start, restart, edit, quit, timeline_, guide_};
    auto main =
        Container::Vertical({navigation_, Maybe(menu_, [this] {
                               return page_ == 1 && experiment_ != nullptr;
                             })});
    form_page_names_ = {tr("缓冲/策略", "Buffer/Policy"),
                        tr("网络", "Network")};
    form_tabs_ = Toggle(&form_page_names_, &form_page_);
    scene_select_ = Dropdown(&scene_names_, &scene_);
    Components buffer{Dropdown(&policy_names_, &policy_)};
    for (auto& field : fields_) buffer.push_back(Input(&field, ""));
    buffer_form_ = Container::Vertical(buffer);
    Components network;
    for (auto& field : network_fields_) network.push_back(Input(&field, ""));
    network_form_ = Container::Vertical(network);
    apply_ = Button(tr("应用并重新运行", "Apply and rerun"),
                    [this] { apply_form(); });
    cancel_ = Button(tr("取消", "Cancel"), [this] { modal_ = 0; });
    form_ = Container::Vertical(
        {form_tabs_, scene_select_,
         Container::Tab({buffer_form_, network_form_}, &form_page_), apply_,
         cancel_});
    auto close = Button(tr("关闭帮助 (Esc)", "Close help (Esc)"),
                        [this] { modal_ = 0; });
    active_ = Container::Tab({main, form_, close}, &modal_);
    auto rendered = Renderer(active_, [this, close] {
      return std::make_shared<DisplayFallback>(draw(close), options_.ascii,
                                               no_color_);
    });
    root_ = CatchEvent(rendered, [this](ftxui::Event e) { return key(e); });
    start->TakeFocus();
  }
  ~Lab() {
    if (job_) job_->cancel = true;
    if (worker_.joinable()) worker_.join();
    player_.stop();
  }
  int run() {
    screen_.HandlePipedInput(false);
    Loop loop(&screen_, root_);
    auto last = Clock::now();
    while (!loop.HasQuitted()) {
      auto now = Clock::now();
      tick(std::chrono::duration_cast<std::chrono::nanoseconds>(now - last)
               .count());
      last = now;
      screen_.PostEvent(ftxui::Event::Custom);
      loop.RunOnce();
      std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    return 0;
  }

 private:
  std::string tr(const std::string& chinese, const std::string& english) const {
    return options_.ascii ? english : chinese;
  }
  std::string test_label() const {
    if (options_.mode == "compare") return tr("三策略对比", "Compare all");
    return tr("单独测试: ", "Single test: ") +
           (options_.mode == "adaptive"
                ? "adaptive"
                : "fixed-" + std::to_string(options_.fixed_ms));
  }
  const Result* selected() const {
    if (!experiment_) return nullptr;
    if (track_ == 0) return &experiment_->reference;
    if (track_ > static_cast<int>(experiment_->results.size())) return nullptr;
    return &experiment_->results[track_ - 1];
  }
  void stop_audio() {
    player_.stop();
    replay_.audio_available = false;
    audio_running_ = false;
  }
  void pause() {
    if (audio_running_) {
      try {
        player_.pause();
        replay_.seek(player_.position());
      } catch (const std::exception& e) {
        status_ = display_safe(e.what());
        stop_audio();
      }
    }
    replay_.pause();
  }
  void resume() {
    if (!selected()) return;
    if (replay_.ended()) replay_.seek(0);
    if (replay_.speed() == 1 && !visual_only_) {
      try {
        if (audio_running_)
          player_.resume();
        else
          player_.start(selected()->audio, replay_.cursor().time);
        replay_.audio_available = true;
        audio_running_ = true;
      } catch (const std::exception& e) {
        status_ = tr("仅可视回放：", "Visual only: ") + display_safe(e.what());
        visual_only_ = true;
        stop_audio();
      }
    }
    replay_.play();
  }
  void toggle() {
    if (job_) {
      job_->cancel = true;
      status_ = tr("正在取消…", "Cancelling...");
      return;
    }
    if (!experiment_) {
      start_job(options_);
      return;
    }
    if (replay_.paused)
      resume();
    else
      pause();
  }
  void select_track(int track) {
    if (!experiment_ || track < 0 ||
        track > static_cast<int>(experiment_->results.size()))
      return;
    stop_audio();
    track_ = track;
    replay_.attach(selected());
    observed_.clear();
    teaching_prefix_ = 0;
    status_ = tr("轨道从实验零时刻开始：", "Track starts at t=0: ") +
              selected()->name;
    resume();
  }
  void choose_track(int shortcut) {
    if (!experiment_) return;
    int track = shortcut;
    if (options_.mode != "compare" && shortcut != 0) {
      int available =
          options_.mode == "adaptive"
              ? 3
              : (options_.fixed_ms == options_.config.large_ms ? 2 : 1);
      if (shortcut != available) {
        status_ =
            tr("本轮未运行该策略；按 p 选择策略/参数",
               "Policy not in this run; press p to choose test/parameters");
        return;
      }
      track = 1;
    }
    guided_ = false;
    select_track(track);
  }
  void move(Time time) {
    pause();
    stop_audio();
    replay_.seek(time);
    if (selected())
      teaching_prefix_ = view_at(*selected(), replay_.cursor()).prefix;
  }
  void start_job(const Options& options) {
    pause();
    stop_audio();
    if (job_) return;
    job_ = std::make_shared<Job>();
    auto job = job_;
    worker_ = std::jthread([options, job] {
      try {
        auto result = prepare(
            options, [job] { return job->cancel.load(); },
            [job](const std::string& text) {
              std::lock_guard lock(job->mutex);
              job->progress = text;
            });
        std::lock_guard lock(job->mutex);
        job->result = std::move(result);
      } catch (const std::exception& e) {
        std::lock_guard lock(job->mutex);
        job->error = display_safe(e.what());
      }
      std::lock_guard lock(job->mutex);
      job->done = true;
    });
    next_options_ = options;
    modal_ = 0;
    status_ =
        tr("准备中；再次开始按钮可取消", "Preparing; Start/Pause cancels");
  }
  void edit_form() {
    if (job_) {
      status_ = "Cancel the preparation before editing";
      return;
    }
    pause();
    fields_[0] = std::to_string(options_.config.seed);
    fields_[1] = std::to_string(options_.config.small_ms);
    fields_[2] = std::to_string(options_.config.large_ms);
    fields_[3] = std::to_string(options_.config.min_ms);
    fields_[4] = std::to_string(options_.config.initial_ms);
    fields_[5] = std::to_string(options_.config.max_ms);
    fields_[6] = options_.input.string();
    fields_[7] = std::to_string(options_.fixed_ms);
    network_fields_ = {std::to_string(options_.config.network_base_ms),
                       std::to_string(options_.config.jitter_ms),
                       std::to_string(options_.config.burst_jitter_ms),
                       std::to_string(options_.config.burst_extra_ms),
                       std::to_string(options_.config.reorder_extra_ms)};
    policy_ = options_.mode == "compare"                      ? 0
              : options_.mode == "adaptive"                   ? 3
              : options_.fixed_ms == options_.config.small_ms ? 1
              : options_.fixed_ms == options_.config.large_ms ? 2
                                                              : 4;
    auto found = std::find(scene_names_.begin(), scene_names_.end(),
                           options_.config.scenario);
    scene_ = static_cast<int>(found - scene_names_.begin());
    modal_ = 1;
    form_page_ = 0;
    form_error_.clear();
    buffer_form_->ChildAt(0)->TakeFocus();
  }
  void apply_form() {
    try {
      const std::array<std::string, 5> modes{"compare", "fixed", "fixed",
                                             "adaptive", "fixed"};
      const auto& fixed = policy_ == 1   ? fields_[1]
                          : policy_ == 2 ? fields_[2]
                                         : fields_[7];
      auto pending = parse_options({"--mode",
                                    modes.at(policy_),
                                    "--buffer-ms",
                                    fixed,
                                    "--scenario",
                                    scene_names_.at(scene_),
                                    "--seed",
                                    fields_[0],
                                    "--small-buffer-ms",
                                    fields_[1],
                                    "--large-buffer-ms",
                                    fields_[2],
                                    "--min-delay-ms",
                                    fields_[3],
                                    "--initial-delay-ms",
                                    fields_[4],
                                    "--max-delay-ms",
                                    fields_[5],
                                    "--network-delay-ms",
                                    network_fields_[0],
                                    "--jitter-ms",
                                    network_fields_[1],
                                    "--burst-jitter-ms",
                                    network_fields_[2],
                                    "--burst-delay-ms",
                                    network_fields_[3],
                                    "--reorder-delay-ms",
                                    network_fields_[4],
                                    "--input",
                                    fields_[6],
                                    "--output-dir",
                                    options_.output.string()});
      pending.ascii = options_.ascii;
      pending.no_color = options_.no_color;
      checked_path(pending.input, true);
      start_job(pending);
    } catch (const std::exception& e) {
      form_error_ = display_safe(e.what());
    }
  }
  void snapshot() {
    if (!experiment_ || !selected()) return;
    auto view = view_at(*selected(), replay_.cursor());
    std::ostringstream text;
    text << summary_text(*experiment_)
         << "CURRENT cursor_ns=" << replay_.cursor().time
         << " target_budget_ms=" << view.tick.target_ms
         << " lookahead_ms=40 level_ms=" << view.tick.level_ms
         << " source_sample=" << view.tick.source << '\n';
    for (const auto& e : view.recent)
      text << e.time << ' ' << e.order << ' ' << e.kind
           << " packet=" << e.packet << " source=" << e.source << ' '
           << e.reason << '\n';
    if (track_ > 0) {
      const auto delays = delays_at(*selected(), replay_.cursor());
      text << "DELAY last_arrival_packet=" << delays.packet
           << " last_network_ms=" << delays.last_network_ms
           << " delay_samples=" << delays.tick.delays.samples
           << " played_network_ms=" << delays.tick.delays.network_ms
           << " buffer_wait_ms=" << delays.tick.delays.wait_ms
           << " packet_to_output_ms=" << delays.tick.delays.playout_ms << '\n';
    }
    save_snapshot(*experiment_, *selected(), replay_.cursor(), text.str());
    status_ = tr("快照已保存：", "Snapshot saved: ") +
              display_safe(experiment_->directory.string());
  }
  bool key(ftxui::Event e) {
    if (e == ftxui::Event::Custom) return false;
    auto size = Terminal::Size();
    if ((size.dimx < 80 || size.dimy < 24) &&
        e == ftxui::Event::Character("q")) {
      screen_.Exit();
      return true;
    }
    if (e == ftxui::Event::Escape) {
      if (modal_) {
        modal_ = 0;
        return true;
      }
    }
    if (modal_ == 1) {
      if (e == ftxui::Event::F1 || e == ftxui::Event::F2) {
        form_page_ = e == ftxui::Event::F1 ? 0 : 1;
        (form_page_ ? network_form_ : buffer_form_)->ChildAt(0)->TakeFocus();
        return true;
      }
      Components choices{form_tabs_};
      if (!form_page_) choices.push_back(buffer_form_->ChildAt(0));
      choices.push_back(scene_select_);
      auto panel = form_page_ ? network_form_ : buffer_form_;
      for (size_t i = form_page_ ? 0 : 1; i < panel->ChildCount(); ++i)
        choices.push_back(panel->ChildAt(i));
      choices.push_back(apply_);
      choices.push_back(cancel_);
      return focus_next(choices, e);  // Other keys edit the focused component.
    }
    if (modal_) return false;
    try {
      if (e == ftxui::Event::Tab || e == ftxui::Event::TabReverse) {
        auto choices = main_focus_;
        if (page_ == 1 && experiment_) choices.push_back(menu_);
        return focus_next(choices, e);
      }
      if (e == ftxui::Event::Character("q")) {
        screen_.Exit();
        return true;
      }
      if (e == ftxui::Event::Character(" ")) {
        if (active_->OnEvent(e)) return true;
        toggle();
        return true;
      }
      if (e == ftxui::Event::Character("p") ||
          e == ftxui::Event::Character("s")) {
        edit_form();
        return true;
      }
      if (e == ftxui::Event::Character("?")) {
        pause();
        modal_ = 2;
        return true;
      }
      if (e == ftxui::Event::Character("v")) {
        page_ = (page_ + 1) % static_cast<int>(pages_.size());
        return true;
      }
      if (e == ftxui::Event::Character("d")) {
        page_ = 3;
        return true;
      }
      if (e == ftxui::Event::Character("e")) {
        pause();
        snapshot();
        return true;
      }
      if (e == ftxui::Event::Character("r")) {
        guided_ = false;
        select_track(track_);
        return true;
      }
      for (int i = 0; i <= 3; ++i)
        if (e == ftxui::Event::Character(std::to_string(i))) {
          choose_track(i);
          return true;
        }
      if (e == ftxui::Event::Character(".") ||
          e == ftxui::Event::Character("n")) {
        pause();
        stop_audio();
        if (e == ftxui::Event::Character("."))
          replay_.next_tick();
        else
          replay_.next_event();
        status_ = tr("单步观察，音频暂停", "Visual step; audio paused");
        return true;
      }
      if (e == ftxui::Event::Character("-") ||
          e == ftxui::Event::Character("+")) {
        bool playing = !replay_.paused;
        pause();
        stop_audio();
        std::array<double, 4> speeds{.25, .5, 1, 2};
        auto i = static_cast<int>(
            std::find(speeds.begin(), speeds.end(), replay_.speed()) -
            speeds.begin());
        replay_.set_speed(speeds[std::clamp(
            i + (e == ftxui::Event::Character("+") ? 1 : -1), 0, 3)]);
        if (playing) resume();
        return true;
      }
      if (timeline_->Focused() &&
          (e == ftxui::Event::ArrowLeft || e == ftxui::Event::ArrowRight)) {
        move(replay_.cursor().time +
             (e == ftxui::Event::ArrowRight ? ms(1000) : -ms(1000)));
        return true;
      }
    } catch (const std::exception& error) {
      status_ = display_safe(error.what());
    }
    return false;
  }
  void tick(Time elapsed) {
    if (page_ != previous_page_) {
      if (page_ == 2) pause();
      previous_page_ = page_;
    }
    if (timeline_->Focused() && std::abs(scrub_ms_ - last_scrub_ms_) >= 1000)
      move(ms(scrub_ms_));
    scrub_ms_ = static_cast<int>(milliseconds(replay_.cursor().time));
    last_scrub_ms_ = scrub_ms_;
    if (job_) {
      std::shared_ptr<Experiment> result;
      bool done = false;
      {
        std::lock_guard lock(job_->mutex);
        done = job_->done;
        status_ = job_->progress;
        if (done) {
          result = job_->result;
          if (!job_->error.empty()) status_ = job_->error;
        }
      }
      if (done) {
        worker_.join();
        job_.reset();
        if (result) {
          experiment_ = std::move(result);
          options_ = next_options_;
          guided_ = options_.mode == "compare";
          track_ = guided_ ? 0 : 1;
          page_ = previous_page_ = 0;
          replay_.attach(selected());
          observed_.clear();
          teaching_prefix_ = 0;
          status_ = guided_
                        ? tr("导览：参考 → 小固定 → 大固定 → 自适应",
                             "Guide: reference -> small -> large -> adaptive")
                        : test_label();
          resume();
        }
      }
    }
    auto size = Terminal::Size();
    if (size.dimx < 80 || size.dimy < 24) {
      if (!replay_.paused) pause();
      return;
    }
    if (!selected() || replay_.paused) return;
    try {
      Time position = audio_running_ ? player_.position() : -1;
      replay_.advance(elapsed, position);
      if (audio_running_ && player_.ended()) {
        replay_.seek(selected()->duration());
        replay_.pause();
      }
      if (track_ > 0) {
        auto view = view_at(*selected(), replay_.cursor());
        for (size_t i = std::min(teaching_prefix_, view.prefix);
             teaching_ && observed_.size() < 6 && i < view.prefix; ++i) {
          const auto& e = selected()->events[i];
          std::string kind = e.kind == "output" ? e.reason : e.kind;
          if ((kind == "plc" || kind == "slow" || kind == "fast" ||
               kind == "target" || kind == "late" || kind == "overflow") &&
              observed_.insert(kind).second) {
            pause();
            status_ =
                tr("教学暂停：", "Teaching pause: ") + kind + " / " + e.reason;
            break;
          }
        }
        teaching_prefix_ = view.prefix;
      }
      if (replay_.ended()) {
        stop_audio();
        if (guided_ && track_ < static_cast<int>(experiment_->results.size()))
          select_track(track_ + 1);
        else {
          replay_.pause();
          page_ = 2;
          status_ = tr("播放结束，查看整轮结果", "Finished; whole-run results");
        }
      }
    } catch (const std::exception& error) {
      status_ =
          tr("仅可视回放：", "Visual only: ") + display_safe(error.what());
      visual_only_ = true;
      stop_audio();
    }
  }
  Element card(const Result& result, bool selected_card, bool compact) {
    auto v = view_at(result, replay_.cursor());
    if (compact)
      return text((selected_card ? "> " : "  ") + result.name +
                  " B=" + std::to_string(v.tick.target_ms) +
                  " target=" + std::to_string(v.tick.target_ms + 40) +
                  " level=" + decimal(v.tick.level_ms) + " PLC=" +
                  decimal(v.tick.cumulative.plc_samples / 48.0) + "ms");
    auto content = vbox(
        {text((selected_card ? "> " : "") + result.name) | bold,
         text(tr("预算 ", "Budget ") + std::to_string(v.tick.target_ms) + "ms"),
         text(tr("目标水位 ", "Target level ") +
              std::to_string(v.tick.target_ms + 40) + "ms"),
         text(tr("连续水位 ", "Contiguous ") + decimal(v.tick.level_ms) + "ms"),
         text("PLC " + decimal(v.tick.cumulative.plc_samples / 48.0) + "ms / " +
              v.tick.action),
         text(tr("源位置 ", "Source ") + decimal(v.source_cursor / 48000.0, 3) +
              "s")});
    return content | border | flex;
  }
  Element chart(const Result& result, int width) {
    width = std::clamp(width - 18, 20, 100);
    constexpr int height = 5;
    std::array<std::string, height> rows;
    for (auto& row : rows) row.assign(width, ' ');
    Time end = replay_.cursor().time, begin = std::max<Time>(0, end - ms(5000));
    const auto current = view_at(result, replay_.cursor());
    for (int x = 0; x < width; ++x) {
      Time time = begin + (end - begin) * x / std::max(1, width - 1);
      auto item = std::upper_bound(
          result.ticks.begin(), result.ticks.end(), time,
          [](Time t, const Tick& row) { return t < row.time; });
      if (item == result.ticks.begin()) continue;
      const auto& tick = time == end ? current.tick : *std::prev(item);
      auto y = [](double value) {
        return height - 1 -
               std::clamp(static_cast<int>(value * (height - 1) / 500), 0,
                          height - 1);
      };
      rows[y(tick.target_ms + 40)][x] = '-';
      rows[y(tick.level_ms)][x] = tick.concealed ? '!' : '*';
    }
    Elements plot{text(tr("水位(ms) 0..500  *=实际 -=目标 !=PLC",
                          "Level(ms) 0..500  *=actual -=target !=PLC"))};
    for (auto& row : rows) plot.push_back(text("|" + row + "|"));
    plot.push_back(text(decimal(milliseconds(begin) / 1000, 2) + "s -> " +
                        decimal(milliseconds(end) / 1000, 2) + "s"));
    std::string phase = options_.config.scenario;
    if (phase == "phased")
      phase = end < ms(5000)    ? "stable"
              : end < ms(10000) ? "jitter"
              : end < ms(15000) ? "burst"
                                : "recovery";
    plot.push_back(text(tr("网络阶段: ", "Network phase: ") + phase));
    return vbox(std::move(plot));
  }
  Element mechanism(bool detailed, int height) {
    if (!selected() || track_ == 0)
      return paragraph(
          tr("参考音轨绕过网络和缓冲，没有缓存指标。",
             "Reference bypasses network and buffer; no queue metrics."));
    auto view = view_at(*selected(), replay_.cursor());
    std::ostringstream queue;
    size_t shown = 0;
    size_t expected = view.source_cursor / kFrame;
    for (auto seq : view.queue) {
      if (shown++ == 12) {
        queue << " ...";
        break;
      }
      if (seq > expected) queue << "[?" << expected << "]";
      queue << '[' << seq << ']';
      expected = seq + 1;
    }
    std::string reason =
        view.tick.action == "slow"
            ? tr("水位低，少消费以积累余量", "Low level: consume less source")
        : view.tick.action == "fast"
            ? tr("水位高，多消费以消除延迟", "High level: consume more source")
        : view.tick.action == "plc"
            ? tr("当前所需区间缺失，补静音并推进游标",
                 "Required interval missing: PLC and advance")
        : view.tick.action == "startup"
            ? tr("预缓冲等待；不计为欠载", "Startup waiting; not an underrun")
        : view.tick.action == "drain"
            ? tr("媒体结束，等待剩余网络事件；不计为欠载",
                 "Media ended; wait for network tail without PLC")
            : tr("水位处于死区，正常消费",
                 "Within deadband: normal consumption");
    Elements elements{
        text(tr("发送 -> 到达 -> 排序缓存 -> 每10ms输出",
                "Send -> arrival -> ordered queue -> output every 10ms")) |
            bold,
        text(display_safe(queue.str(), 180)),
        text("cursor=" + std::to_string(view.source_cursor) +
             " last_required=" + std::to_string(view.tick.source) +
             " output=480 consumed=" + std::to_string(view.tick.advance)),
        text(
            tr("下一tick ", "Next tick ") +
            decimal(milliseconds((replay_.cursor().time / kTick + 1) * kTick)) +
            "ms; " + tr("未来源窗口暂定", "future source window provisional")),
        paragraph(view.tick.action + ": " + reason)};
    if (detailed) {
      event_labels_.clear();
      shown_events_.clear();
      size_t first = view.prefix > 40 ? view.prefix - 40 : 0;
      for (size_t i = first; i < view.prefix; ++i) {
        const auto& e = selected()->events[i];
        event_labels_.push_back(decimal(milliseconds(e.time)) + "ms #" +
                                std::to_string(e.packet) + " " + e.kind +
                                " / " + e.reason);
        shown_events_.push_back(i);
      }
      selected_event_ =
          std::clamp(selected_event_, 0,
                     std::max(0, static_cast<int>(event_labels_.size()) - 1));
      if (!shown_events_.empty()) {
        const auto& e = selected()->events[shown_events_[selected_event_]];
        elements.push_back(separator());
        elements.push_back(paragraph(
            "packet=" + std::to_string(e.packet) + " source=" +
            std::to_string(e.source) + " event=" + std::to_string(e.order) +
            " actual_ns=" + std::to_string(e.time) + " / " + e.reason));
        if (e.packet >= 0 &&
            static_cast<size_t>(e.packet) < experiment_->network.size()) {
          const auto& packet =
              experiment_->network[static_cast<size_t>(e.packet)];
          bool received = false;
          for (size_t i = view.prefix; i > 0; --i) {
            const auto& prior = selected()->events[i - 1];
            if (prior.packet == e.packet && prior.kind == "arrival") {
              received = true;
              break;
            }
          }
          elements.push_back(text(
              "send=" + decimal(milliseconds(packet.send)) + "ms arrival=" +
              (received ? decimal(milliseconds(packet.arrival)) + "ms"
                        : "not observed") +
              " transit=" +
              (received
                   ? decimal(milliseconds(packet.arrival - packet.send)) + "ms"
                   : "not observed") +
              " source=" + std::to_string(packet.seq * kFrame) + ".." +
              std::to_string((packet.seq + 1) * kFrame)));
        }
      }
      elements.push_back(text("event prefix=" + std::to_string(view.prefix) +
                              "/" + std::to_string(selected()->events.size()) +
                              " (future details hidden)"));
      elements.push_back(menu_->Render() | vscroll_indicator | frame |
                         size(HEIGHT, LESS_THAN, std::max(3, height - 19)) |
                         flex);
    }
    return vbox(std::move(elements));
  }
  Element delay_page(int width, int height) {
    if (!selected() || track_ == 0)
      return paragraph(tr("参考绕过网络和缓冲；按1/2/3选择已运行策略查看延迟。",
                          "Reference bypasses network/buffer; choose a policy "
                          "with 1/2/3 for delays."));
    const auto delays = delays_at(*selected(), replay_.cursor());
    const auto& block = delays.tick.delays;
    const Time end = replay_.cursor().time;
    const Time begin = std::max<Time>(0, end - ms(5000));
    const int columns = std::clamp(width - 12, 20, 100);
    const int rows = height >= 36 ? 8 : 3;
    double scale = 50;
    for (const auto* points :
         {&delays.network, &delays.waiting, &delays.playout})
      for (const auto& p : *points)
        if (std::isfinite(p.value)) scale = std::max(scale, p.value);
    scale = std::ceil(scale / 50) * 50;
    std::vector<std::string> plot(rows, std::string(columns, ' '));
    auto draw = [&](const std::vector<DelayPoint>& points, char symbol) {
      for (const auto& point : points) {
        if (!std::isfinite(point.value)) continue;
        int x = static_cast<int>((point.time - begin) * (columns - 1) /
                                 std::max<Time>(1, end - begin));
        int y = rows - 1 - static_cast<int>(point.value * (rows - 1) / scale);
        auto& cell =
            plot[std::clamp(y, 0, rows - 1)][std::clamp(x, 0, columns - 1)];
        cell = cell == ' ' || cell == symbol ? symbol : '+';
      }
    };
    draw(delays.network, 'N');
    draw(delays.waiting, 'B');
    draw(delays.playout, 'P');
    Elements content{
        text(tr(
            "延迟变化(ms)，最近5秒；N=到达 B=等待 P=发送至输出 +=重叠",
            "Delay(ms), last 5s: N=arrival B=wait P=send/output +=overlap")) |
            bold,
        text("N " + tr("最近到达包 ", "last arrival #") +
             std::to_string(delays.packet) + " = " +
             delay_text(delays.last_network_ms) +
             "ms  J=" + decimal(delays.tick.jitter_ms) + "ms"),
        text("B=" + delay_text(block.wait_ms) +
             "ms  P=" + delay_text(block.playout_ms) + "ms  " +
             tr("当前块有效样本=", "block samples=") +
             std::to_string(block.samples)),
        text(tr("当前播放源的网络延迟=", "Played-source network=") +
             delay_text(block.network_ms) + "ms; P=played-N+B")};
    for (int y = 0; y < rows; ++y)
      content.push_back(text(decimal(scale * (rows - 1 - y) / (rows - 1), 0) +
                             " |" + plot[y] + "|"));
    content.push_back(text(decimal(milliseconds(begin) / 1000, 2) + "s -> " +
                           decimal(milliseconds(end) / 1000, 2) + "s  " +
                           tr("横轴=实验时间", "x=experiment time")));
    content.push_back(
        paragraph(tr("N到达包与B/P播放源可能不同；--和曲线空缺表示无有效样本。",
                     "N arrival and B/P source can differ; --/gaps mean no "
                     "valid samples.")));
    content.push_back(paragraph(tr(
        "单位均为ms；含40ms前视，声卡实际出声延迟未测量。",
        "All ms; includes 40ms lookahead; acoustic device delay unmeasured.")));
    return vbox(std::move(content));
  }
  Element draw(Component help_close) {
    auto dimensions = Terminal::Size();
    if (dimensions.dimx < 80 || dimensions.dimy < 24)
      return vbox({text("AJB Lab: paused"),
                   paragraph("Resize terminal to at least 80x24. State "
                             "preserved. q=quit")}) |
             border;
    if (modal_ == 1) {
      Elements rows{
          text(
              tr("待应用参数 — 当前结果保持原配置",
                 "Pending parameters - current result keeps original config")) |
              bold,
          form_tabs_->Render(),
          text(form_page_
                   ? tr("网络0..1000ms；抖动10ms步长；当前场景总延迟<=1000ms",
                        "Network 0..1000ms; jitter step 10ms; scene total "
                        "<=1000ms")
                   : tr("缓冲10ms整数倍；0..460；min <= initial <= max",
                        "Buffer multiples of 10ms; 0..460; min <= initial <= "
                        "max"))};
      Elements body;
      if (!form_page_)
        body.push_back(hbox(
            {text(tr("测试策略 (本轮缓冲策略): ", "policy (test strategy): ")),
             buffer_form_->ChildAt(0)->Render()}));
      body.push_back(
          hbox({text(tr("scene (网络场景): ", "scene (network scenario): ")),
                scene_select_->Render()}));
      if (!form_page_) {
        std::array<std::string, 8> labels{"seed",      "small ms",   "large ms",
                                          "min ms",    "initial ms", "max ms",
                                          "input WAV", "custom ms"};
        std::array<std::string, 8> meanings{
            tr("随机种子，复现实验", "random seed"),
            tr("小固定缓冲预算", "small fixed budget"),
            tr("大固定缓冲预算", "large fixed budget"),
            tr("自适应预算下限", "adaptive minimum"),
            tr("自适应启动预算", "adaptive initial"),
            tr("自适应预算上限", "adaptive maximum"),
            tr("输入音频文件路径", "audio file path"),
            tr("自定义固定预算", "custom fixed budget")};
        for (size_t i = 0; i < fields_.size(); ++i)
          body.push_back(hbox({text(labels[i] + " (" + meanings[i] + "): ") |
                                   size(WIDTH, EQUAL, 44),
                               buffer_form_->ChildAt(i + 1)->Render() | flex}));
      } else {
        std::array<std::string, 5> labels{"base delay ms", "jitter max ms",
                                          "burst jitter ms", "burst extra ms",
                                          "reorder extra ms"};
        std::array<std::string, 5> meanings{
            tr("基础单程网络延迟", "base one-way delay"),
            tr("随机附加抖动上限", "max extra jitter"),
            tr("突发背景抖动上限", "burst background jitter"),
            tr("突发窗口附加延迟", "extra burst delay"),
            tr("乱序包附加延迟", "extra reorder delay")};
        for (size_t i = 0; i < network_fields_.size(); ++i)
          body.push_back(hbox({text(labels[i] + " (" + meanings[i] + "): ") |
                                   size(WIDTH, EQUAL, 44),
                               network_form_->ChildAt(i)->Render() | flex}));
        body.push_back(paragraph(
            tr("constant/"
               "loss仅基础；jitter加随机抖动；burst加背景抖动和突发；reorder加"
               "乱序；phased分阶段。",
               "constant/loss: base; jitter: +jitter; burst: "
               "+background/extra; reorder: +reorder; phased: staged.")));
      }
      rows.push_back(
          vbox(std::move(body)) | vscroll_indicator | yframe |
          size(HEIGHT, LESS_THAN, std::max(3, dimensions.dimy - 10)) | flex);
      rows.push_back(paragraph(form_error_));
      rows.push_back(hbox({apply_->Render(), cancel_->Render()}));
      rows.push_back(text(tr(
          "F1缓冲 F2网络；Tab切换；Enter展开/应用；Esc取消",
          "F1 buffer F2 network; Tab focus; Enter select/apply; Esc cancel")));
      return vbox(std::move(rows)) | border;
    }
    if (modal_ == 2)
      return vbox({text(tr("AJB Lab 帮助", "AJB Lab help")) | bold,
                   paragraph(scenario_lesson(options_.config.scenario,
                                             options_.ascii)),
                   separator(),
                   text("Space pause/resume | . next 10ms | n next event "
                        "(muted)"),
                   text("0 reference / 1 small/custom / 2 large / 3 adaptive: "
                        "restart "
                        "at t=0"),
                   text("r replay | p/s choose test/parameters | +/- visual "
                        "speed | e "
                        "snapshot"),
                   text("d delay chart: arrival, buffer waiting, packet "
                        "send/output"),
                   text("Tab focus; tabs choose pages; timeline arrows seek "
                        "+/-1s"),
                   paragraph(tr("同一实验时刻可能对应不同音节。慢放和单步静音。"
                                "未来事件隐藏。预算+40ms前视=目标水位。",
                                "Same experiment time can mean different "
                                "source syllables. Non-1x and stepping are "
                                "muted. Budget+40ms=target level.")),
                   help_close->Render()}) |
             border;
    std::string state =
        job_ ? tr("准备中", "Preparing")
        : !experiment_
            ? (options_.mode == "compare"
                   ? tr("就绪 Enter开始导览", "Ready: Enter starts guide")
                   : tr("就绪 Enter开始单独测试", "Ready: Enter starts test"))
        : replay_.paused ? tr("暂停", "Paused")
        : replay_.silent()
            ? tr("可视回放，音频暂停", "Visual replay; audio paused")
            : tr("播放", "Playing");
    Elements body{
        text("AJB Lab  " + tr("确定性回放", "Deterministic replay") + "  " +
             options_.config.scenario +
             " seed=" + std::to_string(options_.config.seed) + " " +
             (experiment_ ? experiment_->id : "")) |
            bold,
        tabs_->Render(),
        text(state + "  " +
             (selected() ? (options_.mode == "compare"
                                ? tr("对比 / ", "Compare / ")
                                : tr("单独测试: ", "Single test: ")) +
                               selected()->name
                         : test_label()) +
             " t=" + decimal(milliseconds(replay_.cursor().time) / 1000, 3) +
             "s  " + decimal(replay_.speed(), 2) + "x"),
        navigation_->ChildAt(1)->Render(), timeline_->Render()};
    if (!experiment_) {
      body.push_back(text(tr("按 p 选择测试策略和参数，再应用运行",
                             "Press p to choose test policy and parameters")));
      body.push_back(
          paragraph(scenario_lesson(options_.config.scenario, options_.ascii)));
      body.push_back(
          text("WAV=" + display_safe(options_.input.filename().string())));
      const auto& c = options_.config;
      body.push_back(text("Fixed=" + std::to_string(c.small_ms) + "/" +
                          std::to_string(c.large_ms) +
                          "ms Adaptive=" + std::to_string(c.min_ms) + "/" +
                          std::to_string(c.initial_ms) + "/" +
                          std::to_string(c.max_ms) + "ms Lookahead=40ms"));
      body.push_back(text("Network base=" + std::to_string(c.network_base_ms) +
                          "ms jitter<=" + std::to_string(c.jitter_ms) +
                          "ms; p -> Network"));
    } else if (page_ == 2) {
      body.push_back(text(tr("整轮结果（与当前游标分开）",
                             "WHOLE RUN results (not cursor-time metrics)")) |
                     bold);
      std::istringstream lines(summary_text(*experiment_));
      std::string line;
      while (std::getline(lines, line)) body.push_back(paragraph(line));
      body.push_back(paragraph(
          tr("e保存快照；结果目录有WAV、逐tick CSV、network.csv与events.csv",
             "e saves snapshot; results include WAV, tick CSV, network.csv and "
             "events.csv")));
    } else if (page_ == 3) {
      body.push_back(delay_page(dimensions.dimx, dimensions.dimy) | flex);
    } else if (page_ == 1) {
      body.push_back(mechanism(true, dimensions.dimy) | flex);
    } else {
      bool compact = dimensions.dimx < 120 || dimensions.dimy < 36;
      Elements cards;
      for (size_t i = 0; i < experiment_->results.size(); ++i)
        cards.push_back(card(experiment_->results[i],
                             track_ == static_cast<int>(i + 1), compact));
      body.push_back(compact ? vbox(std::move(cards)) : hbox(std::move(cards)));
      body.push_back(text(tr(
          "截至游标；三策略同一实验时刻，源位置可能不同",
          "At cursor; common experiment time, source positions can differ")));
      if (track_ > 0 && dimensions.dimy >= 36)
        body.push_back(mechanism(false, dimensions.dimy));
      if (track_ > 0 && dimensions.dimy >= 30)
        body.push_back(chart(*selected(), dimensions.dimx));
      else
        body.push_back(paragraph(
            scenario_lesson(options_.config.scenario, options_.ascii)));
      if (track_ == 0)
        body.push_back(paragraph(
            tr("参考音轨绕过网络和缓冲，没有缓存指标。",
               "Reference bypasses network and buffer; no queue metrics.")));
      body.push_back(guide_->Render());
    }
    body.push_back(filler());
    body.push_back(separator());
    body.push_back(paragraph(display_safe(status_, 180)));
    body.push_back(
        text(tr("Space暂停 .单步 n事件 0-3音轨 p参数 d延迟 v分页 ?帮助 q退出",
                "Space pause . tick n event 0-3 track p test d delay v page ? "
                "help q quit")));
    auto content = vbox(std::move(body)) | border;
    if (!no_color_) content = content | color(Color::Cyan);
    return content;
  }
  Options options_, next_options_;
  ScreenInteractive screen_;
  Component root_, active_, navigation_, tabs_, timeline_, guide_, menu_, form_;
  Component form_tabs_, scene_select_, buffer_form_, network_form_, apply_,
      cancel_;
  Components main_focus_;
  std::vector<std::string> pages_, scene_names_, policy_names_, event_labels_;
  std::vector<std::string> form_page_names_;
  std::vector<size_t> shown_events_;
  std::array<std::string, 8> fields_;
  std::array<std::string, 5> network_fields_;
  std::shared_ptr<Experiment> experiment_;
  std::shared_ptr<Job> job_;
  std::jthread worker_;
  Playback player_;
  Replay replay_;
  std::set<std::string> observed_;
  size_t teaching_prefix_ = 0;
  std::string status_, form_error_;
  int page_ = 0, previous_page_ = 0, modal_ = 0, track_ = 0;
  int selected_event_ = 0, scrub_ms_ = 0, last_scrub_ms_ = 0, scene_ = 5;
  int policy_ = 0;
  int form_page_ = 0;
  bool no_color_ = false, audio_running_ = false, visual_only_ = false;
  bool teaching_ = false, guided_ = true;
};
}  // namespace
int run_tui(const Options& options) { return Lab(options).run(); }
}  // namespace aca::ajb
