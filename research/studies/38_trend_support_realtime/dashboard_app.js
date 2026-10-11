"use strict";
// 스터디 38 대시보드. 데이터 형식은 build_dashboard.py 머리말에 있다.
// 주소 뒤에 #trade=<칸 번호>:<매매 번호> 를 붙이면 그 매매 차트를 바로 연다(#stock=<종목코드>, #rounds 도 된다).
const TABS = [["trades", "전체 매매"], ["stock", "종목별 전 기간"], ["rounds", "회차 요약"]];
const MOVING_AVERAGE_TOKEN = { 5: "--ma5", 10: "--ma10", 20: "--ma20", 40: "--ma40", 60: "--ma60", 120: "--ma120", 200: "--ma200" };
const state = { meta: null, cells: {}, bars: {}, rounds: null };
const $ = (selector, root = document) => root.querySelector(selector);
const el = (tag, attributes = {}, ...children) => {
  const node = document.createElement(tag);
  for (const [key, value] of Object.entries(attributes)) {
    if (key === "class") node.className = value; else if (key.startsWith("on")) node.addEventListener(key.slice(2), value);
    else if (value !== undefined && value !== null) node.setAttribute(key, value);
  }
  for (const child of children.flat(Infinity)) if (child !== null && child !== undefined)
    node.append(child instanceof Node ? child : document.createTextNode(String(child)));
  return node;
};
const css = name => getComputedStyle(document.documentElement).getPropertyValue(name).trim();
const fmt = (value, digits = 2) => value === null || value === undefined || Number.isNaN(value) ? "–"
  : !Number.isFinite(value) ? "∞" : Number(value).toLocaleString("ko-KR", { minimumFractionDigits: digits, maximumFractionDigits: digits });
const signed = (value, digits = 2) => el("span", { class: value > 0 ? "pos" : value < 0 ? "neg" : "" }, fmt(value, digits));
const dateText = index => {
  const value = state.meta.dates[index];
  return value === undefined || index < 0 ? "–" : `${String(value).slice(0, 4)}-${String(value).slice(4, 6)}-${String(value).slice(6)}`;
};
const stockText = index => `${state.meta.stocks[index]} ${state.meta.names[index]}`;
const listOf = text => text ? text.split("|").map(Number) : [];

// 데이터 파일은 gzip 바이트를 base64 텍스트로 담았다(아티팩트는 이진 파일을 싣지 않는다).
async function gunzip(path, kind) {
  const response = await fetch(path);
  if (!response.ok) throw new Error(`${path}: ${response.status}`);
  const text = await response.text(), binary = atob(text.trim()), bytes = new Uint8Array(binary.length);
  for (let index = 0; index < binary.length; index++) bytes[index] = binary.charCodeAt(index);
  const unpacked = new Response(new Blob([bytes]).stream().pipeThrough(new DecompressionStream("gzip")));
  return kind === "json" ? unpacked.json() : unpacked.arrayBuffer();
}

function rowsOf(columns) {
  const keys = Object.keys(columns), length = columns[keys[0]].length, rows = new Array(length);
  for (let index = 0; index < length; index++) {
    const row = { index };
    for (const key of keys) row[key] = columns[key][index];
    rows[index] = row;
  }
  return rows;
}

const roundName = source => source.replace(/^round(\d+)$/, "$1회차");
const schemeText = cell => cell.scheme === "decile" ? `10분위 ${cell.turnover_bin}` : cell.scheme === "absolute" ? `절대 구간 ${cell.turnover_bin}`
  : `${cell.scheme} ${cell.turnover_bin.replaceAll("|", "·")}`;
function cellLabel(cell) {
  return `${roundName(cell.source)} · ${cell.cell_role} · ${cell.rule} · ${schemeText(cell)} · ${(cell.order_amount_krw / 1e4).toLocaleString("ko-KR")}만 원 · 상한 ${cell.ratio_cap_percent}% (${cell.trades.toLocaleString("ko-KR")}건)`;
}

async function cellRows(cellIndex) {
  if (state.cells[cellIndex]) return state.cells[cellIndex];
  $("#status").textContent = "매매 불러오는 중…";
  const rows = rowsOf(await gunzip(`data/trades/${state.meta.cells[cellIndex].file}`, "json"));
  const dictionaries = state.meta.dictionaries;
  for (const row of rows) {
    row.cell = cellIndex;
    row.reason = dictionaries.exit_reason[row.exit_reason];
    row.windows = dictionaries.moving_average_windows[row.moving_average_windows];
    row.periods = dictionaries.atr_periods[row.atr_periods];
    row.signal_k = dictionaries.signal_k_list[row.signal_k_list];
    row.year = String(state.meta.dates[row.entry]).slice(0, 4);
    row.support_gap = row.support_low_value_adjusted ? (row.entry_price_adjusted / row.support_low_value_adjusted - 1) * 100 : null;
  }
  $("#status").textContent = "";
  return (state.cells[cellIndex] = rows);
}

// ── 정렬·쪽 나눔 표(정렬은 기준이 바뀔 때만 다시 한다) ─────────────
function dataTable({ columns, rows, pageSize = 100, sortKey, sortDescending = true, onPick, rowClass }) {
  const host = el("div");
  let sort = sortKey, descending = sortDescending, page = 0, picked = null, sorted = null;
  const order = () => {
    sorted = sort !== null && sort !== undefined ? [...rows].sort((a, b) => {
      const x = a[sort], y = b[sort];
      if (x === y) return 0; if (x === null || x === undefined) return 1; if (y === null || y === undefined) return -1;
      return (x < y ? -1 : 1) * (descending ? -1 : 1);
    }) : rows;
  };
  const render = () => {
    if (!sorted) order();
    const pages = Math.max(1, Math.ceil(sorted.length / pageSize));
    page = Math.min(page, pages - 1);
    const head = el("tr", {}, columns.map(column => el("th", {
      class: column.left ? "left" : "", title: column.title || "",
      onclick: () => { if (sort === column.key) descending = !descending; else { sort = column.key; descending = !column.left; } sorted = null; page = 0; render(); }
    }, column.label + (sort === column.key ? (descending ? " ▾" : " ▴") : ""))));
    const body = el("tbody", {}, sorted.slice(page * pageSize, page * pageSize + pageSize).map(row => {
      const tr = el("tr", { class: (row === picked ? "picked " : "") + (rowClass ? rowClass(row) : "") },
        columns.map(column => {
          const value = column.render ? column.render(row) : row[column.key];
          return el("td", { class: column.wrap ? "wrap-text" : column.left ? "left" : "" }, value instanceof Node ? value
            : typeof value === "number" ? fmt(value, column.digits ?? 2) : value ?? "–");
        }));
      if (onPick) tr.addEventListener("click", () => { picked = row; onPick(row); render(); });
      return tr;
    }));
    const pager = pages > 1 ? el("div", { class: "pager" },
      el("button", { class: "btn", onclick: () => { page = 0; render(); } }, "처음"),
      el("button", { class: "btn", onclick: () => { page = Math.max(0, page - 1); render(); } }, "이전"),
      `${(page + 1).toLocaleString("ko-KR")} / ${pages.toLocaleString("ko-KR")}쪽 · ${sorted.length.toLocaleString("ko-KR")}행`,
      el("button", { class: "btn", onclick: () => { page = Math.min(pages - 1, page + 1); render(); } }, "다음"),
      el("button", { class: "btn", onclick: () => { page = pages - 1; render(); } }, "끝"))
      : el("div", { class: "pager" }, `${sorted.length.toLocaleString("ko-KR")}행`);
    host.replaceChildren(el("div", { class: "scroll tall" }, el("table", {}, el("thead", {}, head), body)), pager);
  };
  render();
  return host;
}

// ── 캔버스 도우미 ──────────────────────────────────────────────
function canvasOf(height) {
  const canvas = el("canvas", { height: height });
  canvas.style.height = height + "px";
  return canvas;
}
function prepare(canvas) {
  const ratio = window.devicePixelRatio || 1, width = canvas.clientWidth || 600, height = parseFloat(canvas.style.height);
  canvas.width = width * ratio; canvas.height = height * ratio;
  const context = canvas.getContext("2d");
  context.setTransform(ratio, 0, 0, ratio, 0, 0);
  context.font = `11px ${css("--font-data")}`;
  context.clearRect(0, 0, width, height);
  return { context, width, height };
}
function niceTicks(low, high, count = 5) {
  const span = high - low || Math.abs(high) || 1, raw = span / count, power = 10 ** Math.floor(Math.log10(raw));
  const step = [1, 2, 2.5, 5, 10].map(multiple => multiple * power).find(size => size >= raw);
  const ticks = [];
  for (let value = Math.ceil(low / step) * step; value <= high + step * 1e-9; value += step) ticks.push(+value.toPrecision(12));
  return ticks;
}

// ── 일봉 ───────────────────────────────────────────────────────
// 이평·ATR 은 값 있는 날(그 종목이 거래된 날)만으로 계산한다 — 시험의 rolling_mean_known 과 같다.
async function barsOf(code) {
  if (state.bars[code]) return state.bars[code];
  const [bundle, offset, first, length] = state.meta.bundle_of[code];
  if (!state.bars["b" + bundle]) state.bars["b" + bundle] = new Int32Array(await gunzip(`data/bars/b${bundle}.txt`, "bin"));
  const raw = state.bars["b" + bundle], missing = -2147483648;
  const open = new Float64Array(length), high = new Float64Array(length), low = new Float64Array(length), close = new Float64Array(length);
  let previous = 0;
  for (let day = 0; day < length; day++) {
    const base = day === 0 ? 0 : previous, at = offset + day * 4;
    open[day] = raw[at] === missing ? NaN : raw[at] + base;
    high[day] = raw[at + 1] === missing ? NaN : raw[at + 1] + base;
    low[day] = raw[at + 2] === missing ? NaN : raw[at + 2] + base;
    close[day] = raw[at + 3] === missing ? NaN : raw[at + 3] + base;
    if (Number.isFinite(close[day])) previous = close[day];
  }
  const valid = [];
  for (let day = 0; day < length; day++) if (Number.isFinite(close[day])) valid.push(day);
  const cache = {};
  const movingAverage = window => {
    if (cache["ma" + window]) return cache["ma" + window];
    const result = new Float64Array(length).fill(NaN);
    let sum = 0;
    valid.forEach((day, position) => {
      sum += close[day];
      if (position >= window) sum -= close[valid[position - window]];
      if (position + 1 >= window) result[day] = sum / window;
    });
    return (cache["ma" + window] = result);
  };
  // ATR_p: 전날까지 p 일 참범위 평균. 참범위 = max(고가, 전날 종가) − min(저가, 전날 종가), 첫 행은 고가 − 저가.
  const averageTrueRange = period => {
    if (cache["atr" + period]) return cache["atr" + period];
    const range = valid.map((day, position) => position === 0 ? high[day] - low[day]
      : Math.max(high[day], close[valid[position - 1]]) - Math.min(low[day], close[valid[position - 1]]));
    const result = new Float64Array(length).fill(NaN);
    let sum = 0;
    valid.forEach((day, position) => {
      if (position >= period) result[day] = sum / period;
      sum += range[position];
      if (position >= period) sum -= range[position - period];
    });
    return (cache["atr" + period] = result);
  };
  return (state.bars[code] = { first, length, open, high, low, close, valid, movingAverage, averageTrueRange });
}

// E4jN 매도 다시 계산: 매수 행 + j 행부터, 종가가 이평 N 아래인 날이 j 일 이어진 첫 날 종가(common.exit_of 와 같은 식).
function recomputeExit(bars, trade, window, days) {
  const average = bars.movingAverage(window), entryLocal = trade.entry - bars.first;
  let run = 0, entryPosition = -1;
  for (let position = 0; position < bars.valid.length; position++) {
    const day = bars.valid[position];
    run = Number.isFinite(average[day]) && bars.close[day] < average[day] ? run + 1 : 0;
    if (day === entryLocal) entryPosition = position;
    if (entryPosition >= 0 && position >= entryPosition + days && run >= days) return day + bars.first;
  }
  return null;
}

function candleChart(canvas, bars, { trade, others, whole, windows, representative }) {
  const { context, width, height } = prepare(canvas);
  const narrow = width < 600, left = narrow ? 46 : 64, right = narrow ? 78 : 112, top = 14, bottom = 28;
  const lastDay = bars.first + bars.length - 1;
  const startDay = whole ? bars.first : Math.max(bars.first, Math.min(trade.entry - 60, trade.support >= 0 ? trade.support - 10 : Infinity));
  const endDay = whole ? lastDay : Math.min(lastDay, trade.exit + 25);
  const days = []; for (let day = startDay; day <= endDay; day++) days.push(day);
  const local = day => day - bars.first;
  const averages = windows.map(window => [window, bars.movingAverage(window)]);
  const prices = [];
  for (const day of days) for (const array of [bars.high, bars.low, ...averages.map(pair => pair[1])]) {
    const value = array[local(day)]; if (Number.isFinite(value)) prices.push(value);
  }
  if (trade) for (const value of [trade.support_low_value_adjusted, trade.entry_price_adjusted, trade.exit_price_adjusted]) if (Number.isFinite(value)) prices.push(value);
  let low = Math.min(...prices), high = Math.max(...prices); const pad = (high - low) * 0.06 || 1; low -= pad; high += pad;
  const step = (width - left - right) / days.length, x = day => left + (day - startDay + 0.5) * step;
  const y = value => top + (high - value) / (high - low) * (height - top - bottom);
  context.strokeStyle = css("--line"); context.fillStyle = css("--muted"); context.lineWidth = 1; context.textAlign = "right";
  for (const tick of niceTicks(low, high, 6)) {
    context.beginPath(); context.moveTo(left, y(tick)); context.lineTo(width - right, y(tick)); context.stroke();
    context.fillText(tick.toLocaleString("ko-KR"), left - 6, y(tick) + 4);
  }
  context.textAlign = "center";
  const labelEvery = Math.ceil(days.length / Math.max(3, Math.floor((width - left - right) / 80)));
  days.forEach((day, index) => { if (index % labelEvery === 0) context.fillText(dateText(day).slice(2), x(day), height - 8); });
  if (trade && !whole) {
    context.fillStyle = css("--accent-soft");
    context.fillRect(x(trade.entry) - step / 2, top, (trade.exit - trade.entry + 1) * step, height - top - bottom);
  }
  // 전 기간처럼 봉이 너무 촘촘하면 종가 선으로 그린다
  const candles = step >= 2.5;
  if (!candles) {
    context.strokeStyle = css("--ink"); context.lineWidth = 1; context.beginPath(); let started = false;
    for (const day of days) { const value = bars.close[local(day)]; if (!Number.isFinite(value)) { started = false; continue; }
      if (started) context.lineTo(x(day), y(value)); else context.moveTo(x(day), y(value)); started = true; }
    context.stroke();
  }
  for (const day of (candles ? days : [])) {
    const index = local(day), open = bars.open[index], highPrice = bars.high[index], lowPrice = bars.low[index], close = bars.close[index];
    if (!Number.isFinite(close)) continue;
    const color = close >= open ? css("--up") : css("--down");
    context.strokeStyle = color; context.fillStyle = color; context.lineWidth = 1;
    context.beginPath(); context.moveTo(x(day), y(highPrice)); context.lineTo(x(day), y(lowPrice)); context.stroke();
    const bodyTop = y(Math.max(open, close)), bodyHeight = Math.max(1, Math.abs(y(open) - y(close)));
    context.fillRect(x(day) - Math.max(1, step * 0.35), bodyTop, Math.max(2, step * 0.7), bodyHeight);
  }
  // 점선 이름표는 봉과 겹치지 않게 오른쪽 여백에 모아 두고, 마지막에 서로 겹치지 않게 벌려 그린다.
  const marginLabels = [];
  for (const [window, array] of averages) {
    const token = MOVING_AVERAGE_TOKEN[window] ?? "--muted";
    context.strokeStyle = css(token); context.lineWidth = window === representative ? 2.4 : 1.1; context.beginPath();
    let open = false, last = NaN;
    for (const day of days) { const value = array[local(day)]; if (!Number.isFinite(value)) { open = false; continue; }
      if (open) context.lineTo(x(day), y(value)); else context.moveTo(x(day), y(value)); open = true; last = value; }
    context.stroke();
    if (Number.isFinite(last)) marginLabels.push({ text: `이평 ${window}${window === representative ? " 기준" : ""}`, token, y: y(last) });
  }
  const dashed = (value, from, to, token, text) => {
    if (!Number.isFinite(value)) return;
    context.strokeStyle = css(token); context.setLineDash([5, 3]); context.lineWidth = 1.4;
    context.beginPath(); context.moveTo(x(from) - step / 2, y(value)); context.lineTo(x(to) + step / 2, y(value)); context.stroke();
    context.setLineDash([]); marginLabels.push({ text, token, y: y(value) });
  };
  const mark = (day, price, up, token, text) => {
    context.fillStyle = css(token); context.beginPath();
    const pointX = x(day), pointY = y(price), size = 6;
    if (up) { context.moveTo(pointX, pointY); context.lineTo(pointX - size, pointY + size * 1.6); context.lineTo(pointX + size, pointY + size * 1.6); }
    else { context.moveTo(pointX, pointY); context.lineTo(pointX - size, pointY - size * 1.6); context.lineTo(pointX + size, pointY - size * 1.6); }
    context.fill();
    if (!text) return;
    // 이름표는 봉을 가리지 않게 매수는 그날 저가 아래, 매도는 고가 위에 두고 그림 영역 안으로 당긴다.
    const textWidth = context.measureText(text).width + 8, boxHeight = 16;
    const lowOfDay = bars.low[local(day)], highOfDay = bars.high[local(day)];
    const anchor = up ? y(Number.isFinite(lowOfDay) ? lowOfDay : price) + size * 1.6 + 6
      : y(Number.isFinite(highOfDay) ? highOfDay : price) - size * 1.6 - 6 - boxHeight;
    const boxX = Math.max(left, Math.min(pointX - textWidth / 2, width - right - textWidth));
    const boxY = Math.max(top, Math.min(anchor, height - bottom - boxHeight));
    context.fillStyle = css("--panel"); context.strokeStyle = css(token); context.lineWidth = 1;
    context.fillRect(boxX, boxY, textWidth, boxHeight); context.strokeRect(boxX + 0.5, boxY + 0.5, textWidth - 1, boxHeight - 1);
    context.fillStyle = css(token); context.textAlign = "left"; context.fillText(text, boxX + 4, boxY + 12);
  };
  for (const other of others) if (other !== trade && other.entry <= endDay && other.exit >= startDay) {
    context.globalAlpha = whole ? 0.75 : 0.35;
    if (other.entry >= startDay) mark(other.entry, other.entry_price_adjusted, true, whole ? "--accent" : "--ink", "");
    if (other.exit <= endDay) mark(other.exit, other.exit_price_adjusted, false, whole ? "--down" : "--ink", "");
    context.globalAlpha = 1;
  }
  if (trade && !whole) {
    if (trade.support >= startDay && trade.support <= endDay) {
      dashed(trade.support_low_value_adjusted, trade.support, trade.exit, "--warn", `지지 저점 ${fmt(trade.support_low_value_adjusted, 0)}`);
      context.strokeStyle = css("--warn"); context.lineWidth = 2;
      context.beginPath(); context.arc(x(trade.support), y(trade.support_low_value_adjusted), 6, 0, Math.PI * 2); context.stroke();
      context.fillStyle = css("--warn"); context.textAlign = "center";
      context.fillText(`근거 저점 ${dateText(trade.support).slice(2)}`, x(trade.support), Math.min(height - bottom - 4, y(trade.support_low_value_adjusted) + 20));
    }
    mark(trade.entry, trade.entry_price_adjusted, true, "--accent", `매수 ${fmt(trade.entry_price_adjusted, 0)}`);
    mark(trade.exit, trade.exit_price_adjusted, false, "--ink", `매도 · ${trade.reason}`);
  }
  marginLabels.sort((a, b) => a.y - b.y);
  for (let index = 0; index < marginLabels.length; index++) {
    const label = marginLabels[index], previous = marginLabels[index - 1];
    label.y = Math.max(label.y, top + 4, previous ? previous.y + 13 : -Infinity);
  }
  for (const label of marginLabels) { context.fillStyle = css(label.token); context.textAlign = "left"; context.fillText(label.text, width - right + 6, label.y + 4); }
}

function legendOf(windows, representative, withSupport) {
  return el("div", { class: "legend" },
    windows.map(window => el("span", {}, el("i", { style: `background:${css(MOVING_AVERAGE_TOKEN[window] ?? "--muted")};height:${window === representative ? 4 : 2}px` }),
      `이평 ${window}${window === representative ? "(굵게, 매도 기준)" : ""}`)),
    withSupport ? el("span", {}, el("i", { style: `background:${css("--warn")}` }), "지지 저점(점선)·근거 저점(원)") : null,
    el("span", {}, withSupport ? "옅은 삼각형 = 같은 칸의 다른 매매" : "▲ 매수 · ▼ 매도"));
}

// ── 매매 한 건 상세 ────────────────────────────────────────────
async function showTrade(box, trade, cellIndex) {
  const meta = state.meta, code = meta.stocks[trade.stock], bars = await barsOf(code), all = await cellRows(cellIndex);
  const others = all.filter(row => row.stock === trade.stock);
  const windows = listOf(trade.windows), periods = listOf(trade.periods), representative = Math.min(...windows);
  const rule = meta.cells[cellIndex].rule, canvas = canvasOf(420);
  const scope = el("select", {}, el("option", { value: "trade" }, "이 매매 구간(근거 저점·매수 60일 전 – 매도 25일 뒤)"),
    el("option", { value: "whole" }, `종목 전 기간(이 칸 매매 ${others.length}건)`));
  const redraw = () => candleChart(canvas, bars, { trade, others, whole: scope.value === "whole", windows, representative });
  scope.addEventListener("change", redraw);
  const entryLocal = trade.entry - bars.first, supportLocal = trade.support - bars.first;
  const closeOnEntry = bars.close[entryLocal], lowOnSupport = trade.support >= 0 ? bars.low[supportLocal] : NaN;
  let exitCheck;
  if (/^E4j\d$/.test(rule)) {
    const days = Number(rule.slice(3)), recomputed = recomputeExit(bars, trade, representative, days);
    const shown = recomputed === null ? "끝까지 조건 안 섬(미청산)" : dateText(recomputed);
    const same = recomputed === null ? trade.reason !== "매도 규칙" : recomputed === trade.exit;
    exitCheck = el("span", {}, `페이지가 이평 ${representative}·${days}일로 다시 계산한 매도일 ${shown} `, el("span", { class: same ? "chip" : "chip warn" }, same ? "일치" : "다름"));
  } else {
    exitCheck = el("span", {}, `${rule}은 k ${trade.chosen_k ?? "–"}의 추세 판정이 하락이 된 날 종가라 페이지에서 다시 계산하지 않는다.`);
  }
  const distanceRows = windows.map(window => {
    const average = bars.movingAverage(window)[entryLocal];
    return el("tr", {}, el("td", { class: "left" }, `이평 ${window}`), el("td", {}, fmt(average, 0)),
      periods.map(period => {
        const range = bars.averageTrueRange(period)[entryLocal], distance = (bars.low[entryLocal] - average) / range;
        const inside = distance >= meta.distance_edges[0] && distance < meta.distance_edges[1];
        return el("td", {}, el("span", { class: inside ? "" : "neg" }, fmt(distance, 2)));
      }));
  });
  box.replaceChildren(
    el("h3", {}, `${stockText(trade.stock)} · ${dateText(trade.entry)} → ${dateText(trade.exit)} · ${trade.reason}`),
    legendOf(windows, representative, true),
    el("div", { class: "controls" }, el("label", {}, "범위", scope)),
    canvas,
    el("p", { class: "note" }, `매수일 종가 ${fmt(closeOnEntry, 0)} · 매수가 ${fmt(trade.entry_price_adjusted, 0)} `,
      el("span", { class: closeOnEntry === trade.entry_price_adjusted ? "chip" : "chip warn" }, closeOnEntry === trade.entry_price_adjusted ? "일치" : "다름"),
      ` · 근거 저점(k ${trade.chosen_k ?? "–"}의 마지막 확정 저점) ${dateText(trade.support)} 저가 ${fmt(lowOnSupport, 0)} · 기록 ${fmt(trade.support_low_value_adjusted, 0)} `,
      el("span", { class: lowOnSupport === trade.support_low_value_adjusted ? "chip" : "chip warn" }, lowOnSupport === trade.support_low_value_adjusted ? "일치" : "다름"),
      ` · 매수가는 그 저점보다 ${fmt(trade.support_gap)}% 위 · 신호 낸 k ${trade.signal_k.replaceAll("|", ", ")}`),
    el("p", { class: "note" }, exitCheck),
    el("h3", {}, "매수일 눌림 거리 = (그날 저가 − 이평 N) ÷ ATR p"),
    el("p", { class: "note" }, `매수 조건은 ${meta.distance_edges[0]} 이상 ${meta.distance_edges[1]} 미만. 표는 이 매매가 든 이평 N 목록 × ATR 기간 목록의 조합 전부다. `
      + `이 매매가 실제로 든 묶음은 ${trade.slices}개이고, 범위 밖(파란 값) 조합은 이 날 그 묶음에서는 사지 않은 것이다.`),
    el("div", { class: "scroll" }, el("table", {}, el("thead", {}, el("tr", {}, el("th", { class: "left" }, ""), el("th", {}, "이평 값"),
      periods.map(period => el("th", {}, `ATR ${period}`)))), el("tbody", {}, distanceRows))),
    el("div", { class: "scroll" }, el("table", {}, el("tbody", {},
      el("tr", {}, ["매수가(수정)", "매수가(원)", "매도가(수정)", "매도가(원)", "보유일", "초과수익%", "실현%", "MAE%", "MFE%", "거래대금20(억)", "묶음 수", "매도일 갈림"].map(text => el("th", {}, text))),
      el("tr", {}, [fmt(trade.entry_price_adjusted, 0), fmt(trade.entry_price_raw, 0), fmt(trade.exit_price_adjusted, 0), fmt(trade.exit_price_raw, 0),
        fmt(trade.hold_days, 1), signed(trade.excess_c0_3), signed(trade.realized_percent), fmt(trade.mae_percent), fmt(trade.mfe_percent),
        fmt(trade.turnover_10k / 1e4, 1), trade.slices, trade.distinct_exits_across_windows].map(value => el("td", {}, value)))))));
  requestAnimationFrame(redraw);
  box.scrollIntoView({ block: "nearest" });
}

function tradeColumns(withCell) {
  const meta = state.meta;
  return [
    ...(withCell ? [{ key: "cell", label: "칸", left: true, render: row => `${roundName(meta.cells[row.cell].source)} ${meta.cells[row.cell].rule} ${schemeText(meta.cells[row.cell])}` }] : []),
    { key: "stock", label: "종목", left: true, render: row => stockText(row.stock) },
    { key: "entry", label: "매수일", left: true, render: row => dateText(row.entry) },
    { key: "entry_price_adjusted", label: "매수가", digits: 0, title: "수정주가 종가" },
    { key: "support", label: "근거 저점일", left: true, render: row => dateText(row.support) },
    { key: "support_low_value_adjusted", label: "근거 저점", digits: 0 },
    { key: "support_gap", label: "저점 위%", digits: 1, title: "매수가가 근거 저점보다 몇 % 위" },
    { key: "chosen_k", label: "k", digits: 0 },
    { key: "exit", label: "매도일", left: true, render: row => dateText(row.exit) },
    { key: "exit_price_adjusted", label: "매도가", digits: 0 },
    { key: "hold_days", label: "보유일", digits: 1, title: "묶음 평균이라 소수가 나온다" },
    { key: "reason", label: "매도 이유", left: true },
    { key: "excess_c0_3", label: "초과수익%", render: row => signed(row.excess_c0_3), title: "충격 비용 계수 0.3 넣은 시장 대비 초과수익(묶음 평균)" },
    { key: "realized_percent", label: "실현%", render: row => signed(row.realized_percent) },
    { key: "mae_percent", label: "MAE%", digits: 1 }, { key: "mfe_percent", label: "MFE%", digits: 1 },
    { key: "turnover_10k", label: "거래대금20(억)", render: row => fmt(row.turnover_10k / 1e4, 1) },
    { key: "slices", label: "묶음", digits: 0, title: "이 매매가 든 (이평 N × ATR 기간) 묶음 수" },
    { key: "distinct_exits_across_windows", label: "매도일 갈림", digits: 0, title: "묶음마다 다른 매도일 수" }];
}

// ── 탭: 전체 매매 ───────────────────────────────────────────────
function renderTrades(openTrade) {
  const host = $("#tab-trades"), meta = state.meta;
  const cell = el("select", {}, meta.cells.map((item, index) => el("option", { value: index }, cellLabel(item))));
  const search = el("input", { type: "text", placeholder: "종목코드·이름", style: "width:140px" });
  const reason = el("select", {}, el("option", { value: "" }, "매도 이유 전체"), meta.dictionaries.exit_reason.map(text => el("option", { value: text }, text)));
  const years = [...new Set(meta.cells.flatMap(item => { const list = []; for (let year = Math.floor(item.first_entry / 1e4); year <= Math.floor(item.last_entry / 1e4); year++) list.push(String(year)); return list; }))].sort();
  const year = el("select", {}, el("option", { value: "" }, "전체 연도"), years.map(text => el("option", { value: text }, text)));
  const tiles = el("div", { class: "tiles" }), tableHost = el("div");
  const chartBox = el("div", { class: "panel" }, el("p", { class: "note" }, "표에서 매매 한 건을 누르면 그 종목 일봉 차트에 매수·매도 자리, 매도 이유, 근거 저점, 이평이 나온다."));
  const draw = async () => {
    const cellIndex = Number(cell.value), rows = await cellRows(cellIndex), query = search.value.trim();
    const filtered = rows.filter(row => {
      if (query && !(meta.stocks[row.stock].includes(query) || meta.names[row.stock].includes(query))) return false;
      if (reason.value && row.reason !== reason.value) return false;
      if (year.value && row.year !== year.value) return false;
      return true;
    });
    let sum = 0, positive = 0, hold = 0;
    for (const row of filtered) { sum += row.excess_c0_3; positive += row.excess_c0_3 > 0; hold += row.hold_days; }
    const count = filtered.length || 1;
    tiles.replaceChildren(...[["매매", filtered.length.toLocaleString("ko-KR")], ["종목", new Set(filtered.map(row => row.stock)).size.toLocaleString("ko-KR")],
      ["초과수익 평균%", fmt(sum / count, 3)], ["초과수익 > 0 비율%", fmt(positive / count * 100, 1)], ["보유일 평균", fmt(hold / count, 1)]]
      .map(([key, value]) => el("div", { class: "tile" }, el("div", { class: "k" }, key), el("div", { class: "v" }, value))));
    tableHost.replaceChildren(dataTable({ columns: tradeColumns(false), rows: filtered, sortKey: "entry", sortDescending: true,
      onPick: row => { history.replaceState(null, "", `#trade=${cellIndex}:${row.index}`); showTrade(chartBox, row, cellIndex); } }));
  };
  for (const control of [cell, reason, year]) control.addEventListener("change", draw);
  search.addEventListener("input", () => { clearTimeout(search.timer); search.timer = setTimeout(draw, 250); });
  const checks = meta.checks;
  host.replaceChildren(el("div", { class: "panel" }, el("h2", {}, "전체 매매"),
    el("p", { class: "note" }, "6·7회차 단일 규칙(k 8개 중 하나라도 상승 추세이고 종가가 그 k의 마지막 확정 저점 위, 그날 저가가 이평 N에서 ATR −2 – +1 사이, 전날까지 20일 평균 거래대금이 2억 원 이상)의 매매. "
      + "같은 종목·같은 매수일을 묶음(이평 N × ATR 기간)마다 따로 세지 않고 한 건으로 합쳤다. 숫자는 묶음 평균, 매도일은 가장 짧은 이평 묶음 값."),
    el("p", { class: "note" }, `전체 대조(${checks.trades.toLocaleString("ko-KR")}건): 매수가 = 그날 종가 ${checks.entry_price_equals_close.toLocaleString("ko-KR")}건, `
      + `근거 저점 값 = 그날 저가 ${checks.support_value_equals_low.toLocaleString("ko-KR")}건(근거 저점일 있는 ${checks.support_day_found.toLocaleString("ko-KR")}건 중), `
      + `근거 저점 < 매수가 ${checks.support_below_entry.toLocaleString("ko-KR")}건.`),
    el("div", { class: "controls" }, el("label", {}, "칸", cell), search, reason, year), tiles, tableHost), chartBox);
  if (openTrade) cell.value = String(openTrade.cell);
  draw().then(async () => {
    if (!openTrade) return;
    const row = (await cellRows(openTrade.cell))[openTrade.index];
    if (row) showTrade(chartBox, row, openTrade.cell);
  });
}

// ── 탭: 종목별 전 기간 ──────────────────────────────────────────
function renderStock(openCode) {
  const host = $("#tab-stock"), meta = state.meta;
  const list = el("datalist", { id: "stock-list" }, meta.stocks.map((code, index) => el("option", { value: code }, meta.names[index])));
  const input = el("input", { type: "text", list: "stock-list", placeholder: "종목코드 또는 이름", style: "width:180px" });
  const cell = el("select", {}, el("option", { value: "" }, "모든 칸"), meta.cells.map((item, index) => el("option", { value: index }, cellLabel(item))));
  const windowSelect = el("select", {}, [["20|60|120", "이평 20·60·120"], ["5|20|60", "이평 5·20·60"], ["60|200", "이평 60·200"], ["", "이평 없음"]]
    .map(([value, text]) => el("option", { value }, text)));
  const body = el("div"), chartBox = el("div", { class: "panel" }, el("p", { class: "note" }, "아래 표에서 한 건을 누르면 그 매매 구간 차트가 나온다."));
  const draw = async () => {
    const text = input.value.trim();
    let stock = meta.stocks.indexOf(text);
    if (stock < 0) stock = meta.names.indexOf(text);
    if (stock < 0) { body.replaceChildren(el("p", { class: "note" }, text ? "매매가 있는 종목이 아니다." : "종목을 고른다.")); return; }
    $("#status").textContent = "불러오는 중…";
    const cellIndices = cell.value === "" ? meta.cells.map((item, index) => index) : [Number(cell.value)];
    const trades = (await Promise.all(cellIndices.map(cellRows))).flat().filter(row => row.stock === stock);
    const bars = await barsOf(meta.stocks[stock]), canvas = canvasOf(420), windows = listOf(windowSelect.value);
    $("#status").textContent = "";
    history.replaceState(null, "", `#stock=${meta.stocks[stock]}`);
    body.replaceChildren(el("h3", {}, `${stockText(stock)} · ${dateText(bars.first)} – ${dateText(bars.first + bars.length - 1)} · 매매 ${trades.length.toLocaleString("ko-KR")}건`),
      legendOf(windows, null, false), canvas,
      dataTable({ columns: tradeColumns(true), rows: trades, sortKey: "entry", sortDescending: false, pageSize: 50,
        onPick: row => showTrade(chartBox, row, row.cell) }));
    requestAnimationFrame(() => candleChart(canvas, bars, { trade: null, others: trades, whole: true, windows, representative: null }));
  };
  input.addEventListener("change", draw);
  for (const control of [cell, windowSelect]) control.addEventListener("change", draw);
  host.replaceChildren(el("div", { class: "panel" }, el("h2", {}, "종목별 전 기간"),
    el("p", { class: "note" }, "종목 하나의 전 기간 일봉(수정주가, 이평 200까지 맞게 2010년 이전 210거래일부터)과 그 종목의 모든 매매. 봉이 촘촘하면 종가 선으로 그린다."),
    el("div", { class: "controls" }, list, el("label", {}, "종목", input), el("label", {}, "칸", cell), el("label", {}, "이평", windowSelect)), body), chartBox);
  if (openCode) input.value = openCode;
  draw();
}

// ── 탭: 회차 요약 ───────────────────────────────────────────────
const SUMMARY_FIELDS = [["test", "시험"], ["target", "대상"], ["criterion", "판정 기준"], ["result", "결과"], ["versus", "이전 회차 대비"]];
function tableOfRecords(records) {
  const keys = [...new Set(records.flatMap(record => Object.keys(record.value)))];
  const columns = [{ key: "_name", label: "이름", left: true }, ...keys.map(key => ({ key, label: key, render: row => cellValue(row[key]) }))];
  return dataTable({ columns, rows: records.map(record => ({ _name: record.name, ...record.value })), sortKey: null, pageSize: 100 });
}
function cellValue(value) {
  if (typeof value === "number") return fmt(value, Number.isInteger(value) ? 0 : 3);
  if (value && typeof value === "object") return JSON.stringify(value);
  return value ?? "–";
}
function renderJson(json) {
  const parts = [];
  for (const [key, value] of Object.entries(json)) {
    const isTable = value && typeof value === "object" && !Array.isArray(value) && Object.values(value).length
      && Object.values(value).every(item => item && typeof item === "object" && !Array.isArray(item));
    if (isTable) parts.push(el("h3", {}, key), tableOfRecords(Object.entries(value).map(([name, item]) => ({ name, value: item }))));
    else parts.push(el("p", { class: "note" }, `${key}: `, typeof value === "object" ? el("code", {}, JSON.stringify(value)) : String(value)));
  }
  return parts;
}
// 회차 표: 머리 칸을 누르면 정렬, 값 종류가 40개 이하인 열은 고르기 상자, 나머지는 글자 검색으로 거른다.
function filteredTable(table, pageSize) {
  const host = el("div"), tableHost = el("div"), chosen = new Map();
  let text = "";
  const columns = table.columns.map((column, index) => ({ key: index, label: column, render: row => cellValue(row[index]) }));
  const draw = () => {
    const needle = text.trim().toLowerCase();
    const rows = table.rows.filter(row => [...chosen].every(([index, value]) => String(row[index]) === value)
      && (!needle || row.some(value => String(value ?? "").toLowerCase().includes(needle))));
    tableHost.replaceChildren(dataTable({ columns, rows, sortKey: null, pageSize }));
  };
  if (table.rows.length > pageSize) {
    const controls = el("div", { class: "controls" });
    const search = el("input", { type: "text", placeholder: "글자 검색(모든 열)" });
    search.addEventListener("input", () => { text = search.value; draw(); });
    controls.append(el("label", {}, "검색", search));
    table.columns.forEach((column, index) => {
      const values = [...new Set(table.rows.map(row => String(row[index])))];
      if (values.length < 2 || values.length > 40) return;
      values.sort((a, b) => (Number(a) - Number(b)) || a.localeCompare(b));
      const picker = el("select", {}, el("option", { value: "" }, "전체"), values.map(value => el("option", { value }, value)));
      picker.addEventListener("change", () => { if (picker.value) chosen.set(index, picker.value); else chosen.delete(index); draw(); });
      controls.append(el("label", {}, column, picker));
    });
    host.append(controls);
  }
  host.append(tableHost);
  draw();
  return host;
}
function renderRoundBody(round) {
  const parts = [el("dl", { class: "summary" }, SUMMARY_FIELDS.map(([key, label]) => [el("dt", {}, label), el("dd", {}, round.summary[key] ?? "–")]))];
  if (round.docstring) parts.push(el("details", {}, el("summary", {}, `round${round.round}_test.py 머리말(시험 설명 원문)`), el("pre", {}, round.docstring)));
  if (round.skipped && round.skipped.length) parts.push(el("p", { class: "note" }, "크기 때문에 싣지 않은 표: "
    + round.skipped.map(([name, size]) => `${name}(${(size / 1e6).toFixed(1)}MB)`).join(", ")));
  for (const [name, table] of Object.entries(round.tables)) {
    const content = table.json ? renderJson(table.json) : [filteredTable(table, 50)];
    parts.push(el("details", {}, el("summary", {}, `results/round${round.round}/${name} — ${table.json ? "json" : table.rows.length.toLocaleString("ko-KR") + "행"}`), ...content));
  }
  return parts;
}
async function renderRounds() {
  const host = $("#tab-rounds");
  host.replaceChildren(el("p", { class: "note" }, "불러오는 중…"));
  if (!state.rounds) state.rounds = await gunzip("data/rounds.txt", "json");
  const rounds = state.rounds, subtabs = el("div", { class: "subtabs", role: "tablist" }), body = el("div", { class: "panel" });
  const views = [["all", "통합 요약"], ...rounds.map(round => [String(round.round), `${round.round}회차`])];
  const select = key => {
    for (const button of subtabs.children) button.setAttribute("aria-selected", button.dataset.key === key);
    if (key === "all") {
      body.replaceChildren(el("h2", {}, "회차별 요약"),
        el("p", { class: "note" }, "각 회차의 시험·대상·판정 기준·결과·이전 회차 대비. 숫자는 각 회차 results/ 표에서 옮겼고, 회차 탭에 원 표가 있다. 결론 아님(check_backtest 대상 아님)."),
        el("div", { class: "scroll" }, el("table", {}, el("thead", {}, el("tr", {}, el("th", { class: "left" }, "회차"), SUMMARY_FIELDS.map(([, label]) => el("th", { class: "left" }, label)))),
          el("tbody", {}, rounds.map(round => el("tr", {}, el("td", { class: "left" }, `${round.round}회차`),
            SUMMARY_FIELDS.map(([key]) => el("td", { class: "wrap-text" }, round.summary[key] ?? "–"))))))));
      const withChoices = [...rounds].reverse().find(round => round.tables["choices_log.csv"]);
      if (withChoices) {
        const table = withChoices.tables["choices_log.csv"];
        body.append(el("h3", {}, `결과를 보고 고른 단계(results/round${withChoices.round}/choices_log.csv)`),
          el("p", { class: "note" }, "1회차부터 사전 정의 없이 바꾼 조건. 앞 회차 결과를 보고 고른 조건이 많을수록 뒤 회차의 통과는 확인 이상의 뜻을 갖기 어렵다."),
          dataTable({ columns: table.columns.map((column, index) => ({ key: index, label: column, left: true, render: row => cellValue(row[index]) })),
            rows: table.rows, sortKey: null, pageSize: 100 }));
      }
    } else {
      const round = rounds.find(item => String(item.round) === key);
      body.replaceChildren(el("h2", {}, `${round.round}회차`), el("p", { class: "note" }, "결론 아님(check_backtest 대상 아님)."), ...renderRoundBody(round));
    }
  };
  subtabs.append(...views.map(([key, label]) => el("button", { role: "tab", "data-key": key, onclick: () => select(key) }, label)));
  host.replaceChildren(subtabs, body);
  select("all");
}

// ── 시작 ───────────────────────────────────────────────────────
const rendered = new Set();
function show(name, argument) {
  for (const [key] of TABS) { $(`#tab-${key}`).hidden = key !== name; $(`#tabs [data-tab="${key}"]`).setAttribute("aria-selected", key === name); }
  if (!rendered.has(name)) {
    rendered.add(name);
    ({ trades: renderTrades, stock: renderStock, rounds: renderRounds })[name](argument);
  }
  try { localStorage.setItem("study38-support-tab", name); } catch (error) { /* 저장 안 되면 그냥 넘어간다 */ }
}
(async () => {
  $("#tabs").append(...TABS.map(([key, label]) => el("button", { role: "tab", "data-tab": key, onclick: () => show(key) }, label)));
  try {
    state.meta = await gunzip("data/meta.txt", "json");
    $("#status").textContent = "";
    const hash = location.hash.slice(1);
    const tradeMatch = hash.match(/^trade=(\d+):(\d+)$/), stockMatch = hash.match(/^stock=(\w+)$/);
    if (tradeMatch) { show("trades", { cell: Number(tradeMatch[1]), index: Number(tradeMatch[2]) }); return; }
    if (stockMatch) { show("stock", stockMatch[1]); return; }
    let first = hash;
    if (!TABS.some(([key]) => key === first)) { try { first = localStorage.getItem("study38-support-tab") || "trades"; } catch (error) { first = "trades"; } }
    show(TABS.some(([key]) => key === first) ? first : "trades");
  } catch (error) {
    $("#status").textContent = `데이터를 못 읽었다: ${error.message}`;
  }
})();
