"use strict";

const $ = (id) => document.getElementById(id);
const form = $("settings");
const audio = $("audio");
const colors = ["#f5b75a", "#77a7ff", "#45dec2"];
const names = ["小固定缓冲", "大固定缓冲", "自适应缓冲"];
const actions = {rebuffer: "重新缓冲（Rebuffer）", startup: "预缓冲", normal: "正常播放", fast: "FAST · 加快消费", slow: "SLOW · 减慢消费", plc: "PLC · 补静音", drain: "有限排空"};
const descriptions = {
  rebuffer: "源区间暂不可用：输出等待静音，冻结源位置，重新蓄水后继续；超时则回退 PLC。",
  startup: "起播前先积累音频。这段静音属于预缓冲，不计入 PLC。",
  normal: "每 10ms 输出 480 个样本，正常推进源游标。",
  fast: "水位高于目标：输出 480 样本、消费 504 样本，WSOLA 逐步消除积压。",
  slow: "水位低于目标：输出 480 样本、消费 456 样本，WSOLA 逐步积累水位。",
  plc: "当前源区间缺失：用静音和短淡变补偿，保持 10ms 输出节拍。",
  drain: "有效音频结束，有限排空网络尾部；这段静音不计入 PLC。",
};
const state = {config: null, run: null, selected: 2, position: 0, busy: false, resume: false, hover: null, lastPaint: 0};

function node(tag, text, className) {
  const element = document.createElement(tag);
  if (text !== undefined) element.textContent = text;
  if (className) element.className = className;
  return element;
}
function time(value) {
  const safe = Math.max(0, Number.isFinite(value) ? value : 0);
  return `${String(Math.floor(safe / 60)).padStart(2, "0")}:${(safe % 60).toFixed(2).padStart(5, "0")}`;
}
function value(value, suffix = "ms") {
  return Number.isFinite(value) ? `${value.toFixed(1)} ${suffix}` : "—";
}
function notifyError(message) {
  $("error").textContent = message;
  $("error").hidden = false;
}
function status(text, busy = false) {
  $("run-status").classList.toggle("busy", busy);
  $("run-status").querySelector("span").textContent = text;
}
async function request(path, options = {}) {
  const response = await fetch(path, {credentials: "same-origin", signal: AbortSignal.timeout(15000), ...options});
  const data = await response.json();
  if (!response.ok) throw new Error(typeof data.detail === "string" ? data.detail : "请求失败，请检查参数后重试。");
  return data;
}
function validateRun(run) {
  if (!run || !/^[a-f0-9]{24}$/.test(run.id) || !Array.isArray(run.tracks) || run.tracks.length !== 3) throw new Error("实验结果格式不正确");
  for (const track of run.tracks) {
    if (!/^(fixed-\d{1,3}|adaptive)$/.test(track.id) || !Array.isArray(track.ticks) || !track.ticks.length || track.ticks.length > 31000 || !Array.isArray(track.events) || track.events.length > 5000) throw new Error("实验数据超出预期范围");
    for (const tick of track.ticks) {
      if (!Array.isArray(tick) || tick.length !== 13 || !Object.hasOwn(actions, tick[6]) || tick.some((n, i) => i !== 6 && n !== null && !Number.isFinite(n))) throw new Error("逐帧数据格式不正确");
    }
    for (const key of ["wait_mean_ms", "plc_ms", "startup_ms", "late"]) {
      if (track.summary[key] !== null && !Number.isFinite(track.summary[key])) throw new Error("汇总数据格式不正确");
    }
  }
  return run;
}
function fillDefaults() {
  for (const control of form.elements) {
    if (control.name && Object.hasOwn(state.config.defaults, control.name)) control.value = state.config.defaults[control.name];
  }
  updateBudgetPreview();
}
function updateBudgetPreview() {
  for (const [name, id] of [["small_ms", "small-target"], ["large_ms", "large-target"]]) {
    const input = form.elements.namedItem(name);
    const budget = Number(input.value);
    $(id).textContent = input.value !== "" && input.validity.valid && Number.isFinite(budget)
      ? `目标水位 ${budget + 40}ms（Target level）` : "目标水位 —（Target level）";
  }
}
function readParameters() {
  const config = {};
  for (const key of Object.keys(state.config.defaults)) {
    const control = form.elements.namedItem(key);
    config[key] = ["scenario", "underrun_policy"].includes(key) ? control.value : Number(control.value);
    if (!["scenario", "underrun_policy"].includes(key) && !Number.isSafeInteger(config[key])) throw new Error("请使用有效的整数参数");
  }
  if (config.small_ms >= config.large_ms) throw new Error("小固定缓冲必须小于大固定缓冲");
  if (config.min_ms > config.initial_ms || config.initial_ms > config.max_ms) throw new Error("自适应预算须满足：下限 ≤ 初始值 ≤ 上限");
  return config;
}
async function runExperiment(event) {
  if (event) event.preventDefault();
  if (state.busy || !state.config || !form.reportValidity()) return;
  let config;
  try { config = readParameters(); } catch (error) { notifyError(error.message); return; }
  state.busy = true;
  $("error").hidden = true;
  $("run").disabled = true;
  $("run").textContent = "正在生成试听与曲线…";
  status("正在生成实验", true);
  try {
    const job = await request("/api/runs", {method: "POST", headers: {"Content-Type": "application/json", "X-AJB-Request": "1"}, body: JSON.stringify(config)});
    if (!/^[a-f0-9]{24}$/.test(job.id)) throw new Error("实验编号格式不正确");
    let result;
    for (let attempt = 0; attempt < 240; attempt += 1) {
      await new Promise((resolve) => setTimeout(resolve, 600));
      result = await request(`/api/runs/${job.id}`);
      if (result.status === "error") throw new Error(result.message);
      if (result.status === "ready") break;
    }
    if (result.status !== "ready") throw new Error("实验生成超时，请稍后重试");
    state.run = validateRun(result);
    state.position = 0;
    state.hover = null;
    renderCards();
    for (const control of document.querySelectorAll("[data-track], #play, #back, #forward, #seek")) control.disabled = false;
    selectTrack(2, false);
    $("csv-download").href = fileURL("summary.csv");
    $("csv-download").hidden = false;
    $("phase-labels").hidden = config.scenario !== "phased";
    status(`实验就绪 · ${config.underrun_policy === "rebuffer" ? "Rebuffer" : "PLC"} · seed ${config.seed}`);
  } catch (error) {
    notifyError(error.message || "实验生成失败");
    status(state.run ? "生成失败 · 保留上一轮" : "请重试");
  } finally {
    state.busy = false;
    $("run").disabled = false;
    $("run").textContent = "▶ 重新运行实验";
  }
}
function fileURL(filename) { return `/api/runs/${state.run.id}/files/${encodeURIComponent(filename)}`; }
function currentTrack() { return state.run && state.selected !== "reference" ? state.run.tracks[state.selected] : null; }
function duration() {
  const track = currentTrack();
  return track ? track.ticks.at(-1)[0] + .01 : state.config?.source.duration || 45;
}
function tickAt(track, position) {
  let low = 0, high = track.ticks.length;
  while (low < high) {
    const middle = (low + high) >>> 1;
    if (track.ticks[middle][0] <= position) low = middle + 1;
    else high = middle;
  }
  return track.ticks[Math.max(0, low - 1)];
}
function renderCards() {
  const container = $("comparison");
  container.replaceChildren();
  state.run.tracks.forEach((track, index) => {
    const card = node("button", undefined, "comparison-card");
    card.type = "button";
    card.dataset.track = String(index);
    card.style.setProperty("--accent", colors[index]);
    const top = node("div", undefined, "card-top");
    top.append(node("span", names[index], "card-title"), node("span", "切换试听", "card-state"));
    const metrics = node("div", undefined, "card-metrics");
    const waiting = node("div");
    const waitingValue = node("strong", track.summary.wait_mean_ms === null ? "—" : track.summary.wait_mean_ms.toFixed(1), "metric-value");
    waitingValue.append(node("small", "ms"));
    const waitingCaption = node("span", "平均缓冲等待（Mean sample wait: arrival → first output）", "metric-caption");
    waitingCaption.title = "有效源样本首次输出时刻减去所属包到达时刻，再取平均；不含网络传输或设备出声延迟。";
    waiting.append(waitingValue, waitingCaption);
    const plc = node("div");
    const plcValue = node("strong", (track.summary.plc_ms / 1000).toFixed(2), "metric-value");
    plcValue.append(node("small", "s"));
    plc.append(plcValue, node("span", "累计补静音 · PLC", "metric-caption"));
    const rebuffer = node("div");
    const rebufferValue = node("strong", (track.summary.rebuffer_ms / 1000).toFixed(2), "metric-value");
    rebufferValue.append(node("small", "s"));
    rebuffer.append(rebufferValue,
      node("span", `重新缓冲（Rebuffer）· ${track.summary.rebuffer_events} 次 · 超时 ${track.summary.rebuffer_timeouts} 次`, "metric-caption"));
    metrics.append(waiting, plc, rebuffer);
    const budget = index === 0 ? state.run.config.small_ms : state.run.config.large_ms;
    const lookahead = track.summary.lookahead_ms;
    const equation = index === 2
      ? `动态预算 + ${lookahead}ms 前视 = 目标水位`
      : `${budget}ms 额外预算 + ${lookahead}ms 前视 = ${budget + lookahead}ms 目标水位`;
    const note = index === 2 ? "动态调整水位，在连续性与等待之间取舍" : "平均等待按样本实际到达与首次输出时刻计算。";
    card.append(top, node("p", equation, "budget-equation"), metrics, node("p", note, "card-note"));
    card.addEventListener("click", () => selectTrack(index));
    container.append(card);
  });
}
function selectTrack(selected, resume = !audio.paused) {
  audio.pause();
  state.selected = selected;
  state.resume = resume;
  state.position = Math.min(state.position, duration() - .01);
  for (const button of document.querySelectorAll("[data-track]")) {
    const active = button.dataset.track === String(selected);
    button.classList.toggle("active", active);
    button.setAttribute("aria-pressed", String(active));
    const indicator = button.querySelector(".card-state");
    if (indicator) indicator.textContent = active ? "当前试听" : "切换试听";
  }
  const track = currentTrack();
  audio.src = track ? fileURL(`${track.id}.wav`) : "/api/source.wav";
  audio.load();
  $("seek").max = String(duration());
  $("duration").textContent = time(duration());
  $("wav-download").href = audio.src;
  $("wav-download").hidden = false;
  paint();
}
function setTime(position) {
  if (!state.run) return;
  state.position = Math.max(0, Math.min(position, duration() - .01));
  if (audio.readyState >= 1) audio.currentTime = state.position;
  paint();
}
async function play() {
  if (!state.run) return;
  try {
    if (audio.paused) {
      if (audio.ended) setTime(0);
      await audio.play();
    } else audio.pause();
  } catch { notifyError("浏览器未能播放音频，请再次点击播放；也可以下载 WAV 试听。"); }
}
function canvasContext(id) {
  const canvas = $(id);
  const rect = canvas.getBoundingClientRect();
  const ratio = window.devicePixelRatio || 1;
  const width = Math.max(1, rect.width), height = Math.max(1, rect.height);
  if (canvas.width !== Math.round(width * ratio) || canvas.height !== Math.round(height * ratio)) {
    canvas.width = Math.round(width * ratio);
    canvas.height = Math.round(height * ratio);
  }
  const context = canvas.getContext("2d");
  context.setTransform(ratio, 0, 0, ratio, 0, 0);
  context.clearRect(0, 0, width, height);
  return {context, width, height};
}
function drawWaveform() {
  const {context: ctx, width, height} = canvasContext("waveform");
  const peaks = state.config?.source.waveform || [];
  const progress = state.position / duration();
  ctx.fillStyle = "#45dec20b";
  ctx.fillRect(0, 0, width * progress, height);
  const count = Math.min(peaks.length, Math.floor(width / 3));
  for (let i = 0; i < count; i += 1) {
    const amplitude = peaks[Math.floor(i * peaks.length / count)];
    const size = Math.max(2, amplitude * 125);
    ctx.fillStyle = i / count <= progress ? "#56d7c0" : "#3d5f68";
    ctx.fillRect(i * width / count, (height - size) / 2, 1.7, size);
  }
  ctx.strokeStyle = "#a4f5e4";
  ctx.beginPath(); ctx.moveTo(width * progress, 20); ctx.lineTo(width * progress, height - 17); ctx.stroke();
  if (state.run?.config.scenario === "phased") {
    const labels = $("phase-labels").children;
    [0, 5, 10, 15].forEach((begin, index) => {
      const end = index === 3 ? duration() : Math.min(begin + 5, duration());
      labels[index].style.width = `${Math.max(0, end - begin) / duration() * 100}%`;
      labels[index].hidden = begin >= duration();
    });
    for (const phase of [5, 10, 15]) {
      const x = width * phase / duration();
      ctx.strokeStyle = "#60728a55"; ctx.setLineDash([2, 4]);
      ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, height); ctx.stroke(); ctx.setLineDash([]);
    }
  }
}
function drawChart(id, columns, lineColors) {
  const {context: ctx, width, height} = canvasContext(id);
  const track = currentTrack();
  const padding = {left: 35, right: 9, top: 12, bottom: 22};
  const w = width - padding.left - padding.right, h = height - padding.top - padding.bottom;
  const maxTime = duration();
  let maximum = 200;
  if (track) for (const row of track.ticks) for (const col of columns) if (Number.isFinite(row[col])) maximum = Math.max(maximum, row[col]);
  maximum = Math.ceil(maximum / 100) * 100;
  const x = (n) => padding.left + n / maxTime * w;
  const y = (n) => padding.top + h - n / maximum * h;
  ctx.font = "9px -apple-system, sans-serif";
  ctx.textAlign = "right";
  for (let i = 0; i <= 4; i += 1) {
    const n = maximum * i / 4;
    ctx.fillStyle = "#657991"; ctx.fillText(String(Math.round(n)), padding.left - 8, y(n) + 3);
    ctx.strokeStyle = "#263242"; ctx.lineWidth = .7;
    ctx.beginPath(); ctx.moveTo(padding.left, y(n)); ctx.lineTo(width - padding.right, y(n)); ctx.stroke();
  }
  ctx.textAlign = "center";
  for (let i = 0; i <= 5; i += 1) ctx.fillText(`${Math.round(maxTime * i / 5)}s`, x(maxTime * i / 5), height - 4);
  if (state.run?.config.scenario === "phased") {
    ctx.fillStyle = "#f5b75a07"; ctx.fillRect(x(5), padding.top, x(10) - x(5), h);
    ctx.fillStyle = "#f5876309"; ctx.fillRect(x(10), padding.top, x(15) - x(10), h);
  }
  if (!track) {
    ctx.fillStyle = "#8192a9"; ctx.textAlign = "center";
    ctx.fillText(state.run ? "原始语音绕过网络与缓冲，无对应指标" : "正在准备实验数据…", width / 2, height / 2);
    $(id === "buffer-chart" ? "buffer-readout" : "delay-readout").textContent = state.run
      ? "原始语音未经过网络或缓冲，无对应曲线指标。" : "正在准备实验数据…";
    return;
  }
  const step = Math.max(1, Math.floor(track.ticks.length / (w * 2)));
  columns.forEach((column, index) => {
    ctx.strokeStyle = lineColors[index]; ctx.lineWidth = index === 0 && id === "buffer-chart" ? 1.3 : 1.25;
    ctx.setLineDash(id === "buffer-chart" && index === 0 ? [4, 4] : []);
    ctx.beginPath(); let started = false;
    for (let i = 0; i < track.ticks.length; i += step) {
      const row = track.ticks[i];
      if (!Number.isFinite(row[column])) { started = false; continue; }
      if (started) ctx.lineTo(x(row[0]), y(row[column])); else ctx.moveTo(x(row[0]), y(row[column]));
      started = true;
    }
    ctx.stroke(); ctx.setLineDash([]);
  });
  const position = state.hover === null ? state.position : state.hover;
  ctx.strokeStyle = "#d9e8fa77"; ctx.lineWidth = 1; ctx.setLineDash([3, 3]);
  ctx.beginPath(); ctx.moveTo(x(position), padding.top); ctx.lineTo(x(position), height - padding.bottom); ctx.stroke(); ctx.setLineDash([]);
  const row = tickAt(track, position);
  if (id === "buffer-chart") $("buffer-readout").textContent = `${position.toFixed(2)}s · 目标 ${value(row[1])} · 连续水位 ${value(row[2])}`;
  else $("delay-readout").textContent = `${position.toFixed(2)}s · 网络 ${value(row[7])} + 等待 ${value(row[8])} = 总计 ${value(row[9])}`;
}
function renderInstant() {
  const track = currentTrack();
  if (!track) {
    for (const id of ["instant-level", "instant-target", "instant-source", "instant-plc", "instant-rebuffer"]) $(id).textContent = "—";
    $("action").textContent = state.run ? "原始语音" : "等待运行";
    $("action-explanation").textContent = state.run ? "原始语音作为听感参考，不经过网络、缓冲或 PLC。" : "运行实验后，可以探索水位变化与播放决策。";
    $("events").replaceChildren(node("li", "原始语音没有网络或缓冲事件。", "empty-events"));
    return;
  }
  const row = tickAt(track, state.position);
  $("instant-level").textContent = value(row[2]);
  $("instant-target").textContent = value(row[1]);
  $("instant-source").textContent = value(row[10] / 48000, "s");
  $("instant-plc").textContent = value(row[11]);
  $("instant-rebuffer").textContent = value(row[12]);
  $("action").textContent = actions[row[6]];
  $("action-explanation").textContent = descriptions[row[6]];
  const events = track.events.filter((event) => event.time <= state.position).slice(-4).reverse();
  const list = $("events"); list.replaceChildren();
  for (const event of events) {
    const item = node("li");
    const label = event.kind === "target" ? `目标预算调整至 ${event.target}ms` : ({rebuffer_start: "开始重新蓄水", rebuffer_end: event.reason === "level_recovered" ? "蓄水完成，继续播放" : "等待超时，回退 PLC", late: "包迟到，源区间已播放", duplicate: "重复包，忽略", overflow: "容量保护，丢弃"}[event.kind] || event.kind);
    item.append(node("time", `${event.time.toFixed(2)}s`), node("i", undefined, "event-dot"), node("span", `${label}${event.packet >= 0 ? ` · #${event.packet}` : ""}`));
    list.append(item);
  }
  if (!events.length) list.append(node("li", "当前游标前没有目标调整或丢弃事件。", "empty-events"));
  if (track.events_truncated) list.append(node("li", "关键事件预览限前 5000 条，完整记录见 events.csv。", "empty-events"));
}
function paint() {
  $("current-time").textContent = time(state.position);
  $("seek").value = String(state.position);
  drawWaveform();
  drawChart("buffer-chart", [1, 2], ["#45dec2", "#f5b75a"]);
  drawChart("delay-chart", [7, 8, 9], ["#7f91aa", "#77a7ff", "#d7a2f8"]);
  renderInstant();
}
function frame(now) {
  if (!audio.paused && now - state.lastPaint > 90) {
    state.position = audio.currentTime;
    paint(); state.lastPaint = now;
  }
  requestAnimationFrame(frame);
}

form.addEventListener("submit", runExperiment);
form.addEventListener("input", updateBudgetPreview);
$("reset").addEventListener("click", () => { if (state.config) fillDefaults(); });
$("play").addEventListener("click", play);
$("back").addEventListener("click", () => setTime(state.position - 5));
$("forward").addEventListener("click", () => setTime(state.position + 5));
$("seek").addEventListener("input", (event) => setTime(Number(event.target.value)));
$("volume").addEventListener("input", (event) => { audio.volume = Number(event.target.value) / 100; });
$("loop").addEventListener("change", (event) => { audio.loop = event.target.checked; });
for (const button of document.querySelectorAll(".track-button")) button.addEventListener("click", () => selectTrack(button.dataset.track === "reference" ? "reference" : Number(button.dataset.track)));
audio.addEventListener("loadedmetadata", () => {
  audio.currentTime = Math.min(state.position, Math.max(0, audio.duration - .01));
  if (state.resume) { state.resume = false; audio.play().catch(() => notifyError("请点击播放按钮继续试听。")); }
});
audio.addEventListener("play", () => { $("play").textContent = "Ⅱ"; $("play").setAttribute("aria-label", "暂停"); });
audio.addEventListener("pause", () => { $("play").textContent = "▶"; $("play").setAttribute("aria-label", "播放"); });
audio.addEventListener("seeked", () => { state.position = audio.currentTime; paint(); });
audio.addEventListener("ended", () => { state.position = audio.currentTime; paint(); });
audio.addEventListener("error", () => { if (state.run) notifyError("音频载入失败，实验可能已过期，请重新运行。"); });
for (const id of ["buffer-chart", "delay-chart", "waveform"]) {
  const canvas = $(id);
  function pointerTime(event) {
    const rect = canvas.getBoundingClientRect();
    const inset = id === "waveform" ? 0 : 35;
    return Math.max(0, Math.min(1, (event.clientX - rect.left - inset) / (rect.width - inset - (id === "waveform" ? 0 : 9)))) * duration();
  }
  canvas.addEventListener("click", (event) => setTime(pointerTime(event)));
  if (id !== "waveform") {
    canvas.addEventListener("pointermove", (event) => { state.hover = pointerTime(event); paint(); });
    canvas.addEventListener("pointerleave", () => { state.hover = null; paint(); });
  }
}
window.addEventListener("resize", paint);
document.addEventListener("keydown", (event) => {
  if (event.code === "Space" && event.target === document.body) { event.preventDefault(); play(); }
});
async function boot() {
  $("run").disabled = true;
  try {
    state.config = await request("/api/config");
    if (!state.config.defaults || !state.config.source || !Array.isArray(state.config.source.waveform)) throw new Error("无法读取实验配置");
    fillDefaults();
    $("source-name").textContent = state.config.source.name;
    $("source-meta").textContent = `${state.config.source.duration.toFixed(0)} 秒 · ${state.config.source.sample_rate / 1000}kHz · 本地 WAV`;
    audio.volume = .75;
    paint();
    await runExperiment();
  } catch (error) { notifyError(error.message); status("连接失败"); }
  finally { $("run").disabled = false; }
}
paint();
requestAnimationFrame(frame);
boot();
