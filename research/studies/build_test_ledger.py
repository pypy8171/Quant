"""시험 원장 생성기 — 2026-10-06–10-07 시험 7항목의 조건·결과·재계산 대조를 한 장의 HTML로 만든다.

실행(저장소 루트): py -X utf8 research/studies/build_test_ledger.py
출력: research/studies/test_ledger.html

숫자는 손으로 옮기지 않는다. 결과 숫자는 각 시험의 산출 파일(txt·json·parquet)에서 읽고, 핵심 숫자는 parquet에서
이 파일이 따로 다시 계산해 txt 값과 대조한다. 대조는 txt의 표시 자릿수 기준(마지막 자리 0.5 단위 허용)이다.
파라미터는 각 시험 .py에서 ast·줄 검색으로 뽑아 "파일:줄"을 붙인다.
"""
from __future__ import annotations

import ast
import html
import importlib.util
import json
import math
import re
import sys
from pathlib import Path

import numpy as np
import pandas as pd

STUDIES = Path(__file__).resolve().parent
REPOSITORY = STUDIES.parents[1]
SURGE = STUDIES / "35_surge_box_breakout"
SWING = STUDIES / "37_swing_trend"
OUTPUT = STUDIES / "test_ledger.html"

HALT_SCRIPT = SURGE / "halt_trigger.py"
HALT_TEXT = SURGE / "halt_trigger.txt"
HALT_TRADES = SURGE / "halt_trigger_trades.parquet"
INDEX_SNAPSHOT = SURGE / "index_snapshot.parquet"
VALIDATE_SCRIPT = SURGE / "halt_validate.py"
VALIDATE_TEXT = SURGE / "halt_validate.txt"
VALIDATE_WIDE = SURGE / "halt_validate_wide.parquet"
VALIDATE_SWEEP = SURGE / "halt_validate_sweep.parquet"
OVERNIGHT_SCRIPT = SURGE / "overnight_us.py"
OVERNIGHT_TEXT = SURGE / "overnight_us.txt"
OVERNIGHT_TRADES = SURGE / "overnight_us_trades.parquet"
OVERNIGHT_FEATURES = SURGE / "overnight_us_features.parquet"
STANDALONE_SCRIPT = SWING / "standalone_swing.py"
STANDALONE_TEXT = SWING / "standalone_swing.txt"
STANDALONE_TRADES = SWING / "standalone_swing_trades.parquet"
STANDALONE_METRICS = SWING / "standalone_swing_metrics.json"
DEVSCALE_SCRIPT = SWING / "devscale_trend_filter.py"
DEVSCALE_TEXT = SWING / "devscale_trend_filter.txt"
DEVSCALE_TRADES = SWING / "devscale_trend_trades.parquet"
SUPPORT_SCRIPT = SWING / "ma_support_exits.py"
SUPPORT_TEXT = SWING / "ma_support_exits.txt"
SUPPORT_CELLS = SWING / "ma_support_cells.parquet"
SUPPORT_WALK = SWING / "ma_support_walk_trades.parquet"
SUPPORT_PORTFOLIO = SWING / "ma_support_portfolio.parquet"
SUPPORT_PORTFOLIO_TRADES = SWING / "ma_support_portfolio_trades.parquet"
SUPPORT_METRICS = SWING / "ma_support_exits_metrics.json"

NUMBER_PATTERN = re.compile(r"[+-]?(?:\d[\d,]*(?:\.\d+)?|nan)")
YEAR_LABEL_PATTERN = re.compile(r"(\d\d):([+-]\d+)")


# ---------------------------------------------------------------- 파일·출처


LINE_CACHE: dict[Path, list[str]] = {}


def relative(path: Path) -> str:
    return path.relative_to(REPOSITORY).as_posix()


def read_lines(path: Path) -> list[str]:
    if path not in LINE_CACHE:
        LINE_CACHE[path] = path.read_bytes().decode("utf-8").splitlines()

    return LINE_CACHE[path]


def line_of(path: Path, pattern: str, start: int = 1) -> tuple[int, str]:
    """pattern(정규식)이 처음 맞는 줄 번호(1부터)와 그 줄. 없으면 (0, '')."""
    compiled = re.compile(pattern)

    for number, line in enumerate(read_lines(path), start=1):
        if number >= start and compiled.search(line):
            return number, line.strip()

    return 0, ""


def where(path: Path, pattern: str, start: int = 1) -> str:
    number, _ = line_of(path, pattern, start)
    return f"{relative(path)}:{number}" if number else f"{relative(path)}:(못 찾음)"


def constant_of(path: Path, name: str) -> tuple[object, int]:
    """모듈 최상위 `name = 값` 의 값과 줄. 리터럴이 아니면 소스 문자열."""
    tree = ast.parse(path.read_bytes().decode("utf-8"))

    for node in tree.body:
        if isinstance(node, ast.Assign):
            for target in node.targets:
                names = [target] if isinstance(target, ast.Name) else list(getattr(target, "elts", []))

                for position, element in enumerate(names):
                    if isinstance(element, ast.Name) and element.id == name:
                        value_node = node.value

                        if isinstance(value_node, ast.Tuple) and len(names) > 1:
                            value_node = value_node.elts[position]

                        try:
                            return ast.literal_eval(value_node), node.lineno
                        except ValueError:
                            return ast.unparse(value_node), node.lineno

    return None, 0


def constant_source(path: Path, name: str) -> tuple[object, str]:
    value, number = constant_of(path, name)
    return value, f"{relative(path)}:{number}"


def text_line_index(path: Path, pattern: str, start: int = 0) -> int:
    """txt 안에서 pattern 이 처음 맞는 줄의 0부터 위치. 없으면 -1."""
    compiled = re.compile(pattern)

    for position, line in enumerate(read_lines(path)):
        if position >= start and compiled.search(line):
            return position

    return -1


def text_match(path: Path, pattern: str, start: int = 0, stop: int | None = None) -> tuple[re.Match | None, int]:
    compiled = re.compile(pattern)
    lines = read_lines(path)

    for position in range(start, len(lines) if stop is None else min(stop, len(lines))):
        found = compiled.search(lines[position])

        if found:
            return found, position + 1

    return None, 0


def text_matches(path: Path, pattern: str, start: int = 0, stop: int | None = None) -> list[tuple[re.Match, int]]:
    compiled = re.compile(pattern)
    lines = read_lines(path)
    result = []

    for position in range(start, len(lines) if stop is None else min(stop, len(lines))):
        found = compiled.search(lines[position])

        if found:
            result.append((found, position + 1))

    return result


def load_module(path: Path, alias: str):
    if str(path.parent) not in sys.path:
        sys.path.insert(0, str(path.parent))

    loader_specification = importlib.util.spec_from_file_location(alias, path)
    module = importlib.util.module_from_spec(loader_specification)
    loader_specification.loader.exec_module(module)
    return module


# ---------------------------------------------------------------- 대조 장부


def reported_number(text: str) -> float:
    cleaned = str(text).replace("−", "-").replace(",", "")
    found = NUMBER_PATTERN.search(cleaned)

    if not found:
        return float("nan")

    return float("nan") if found.group(0).lstrip("+-") == "nan" else float(found.group(0).replace(",", ""))


def reported_decimals(text: str) -> int:
    cleaned = str(text).replace("−", "-").replace(",", "")
    found = NUMBER_PATTERN.search(cleaned)

    if not found or "." not in found.group(0):
        return 0

    return len(found.group(0).split(".")[1])


class Ledger:
    """시험 하나의 대조 기록. kind 는 '재계산'(parquet에서 다시 계산) 또는 '산출물 대조'(parquet·json 값과 txt)."""

    def __init__(self, test: str) -> None:
        self.test = test
        self.rows: list[dict] = []

    def number(self, label: str, computed: float, reported_text: str, source: str, kind: str = "재계산", scale: float = 1.0) -> bool:
        reported = reported_number(reported_text)
        decimals = reported_decimals(reported_text)
        value = float(computed) * scale if computed is not None else float("nan")

        if math.isnan(reported) and math.isnan(value):
            matched = True
        elif math.isnan(reported) or math.isnan(value):
            matched = False
        else:
            tolerance = 0.5 * 10 ** (-decimals) + 1e-9 * max(1.0, abs(reported))
            matched = abs(value - reported) <= tolerance

        shown = "nan" if math.isnan(value) else f"{value:+,.{decimals + 2}f}"
        self.rows.append({"label": label, "computed": shown, "reported": str(reported_text).strip(), "source": source, "kind": kind,
                          "matched": matched})
        return matched

    def text(self, label: str, computed: str, reported: str, source: str, kind: str = "재계산") -> bool:
        matched = str(computed).strip() == str(reported).strip()
        self.rows.append({"label": label, "computed": str(computed), "reported": str(reported), "source": source, "kind": kind,
                          "matched": matched})
        return matched

    def missing(self, label: str, source: str) -> None:
        self.rows.append({"label": label, "computed": "–", "reported": "(txt에서 못 찾음)", "source": source, "kind": "파싱",
                          "matched": False})

    @property
    def total(self) -> int:
        return len(self.rows)

    @property
    def matched_count(self) -> int:
        return sum(row["matched"] for row in self.rows)

    @property
    def mismatches(self) -> list[dict]:
        return [row for row in self.rows if not row["matched"]]


DISCREPANCIES: list[dict] = []


def note_discrepancy(test: str, description: str, code_source: str, text_source: str, still_differs: bool) -> None:
    """코드와 설명(txt 머리말·PLAN)이 다른 곳. still_differs 가 거짓이면 적지 않는다."""

    if still_differs:
        DISCREPANCIES.append({"test": test, "description": description, "code": code_source, "text": text_source})


# ---------------------------------------------------------------- 독립 계산


def max_drawdown(profits) -> float:
    values = np.asarray(profits, float)
    curve = np.r_[0.0, np.cumsum(values)]
    return float((curve - np.maximum.accumulate(curve)).min())


def plain_t(values) -> float:
    values = np.asarray(values, float)
    values = values[np.isfinite(values)]

    if len(values) < 2:
        return float("nan")

    deviation = values.std(ddof=1)
    return float(values.mean() / (deviation / math.sqrt(len(values)))) if deviation > 0 else float("nan")


def welch(left, right) -> float:
    left = np.asarray(left, float)
    right = np.asarray(right, float)

    if len(left) < 2 or len(right) < 2:
        return float("nan")

    error = math.sqrt(left.var(ddof=1) / len(left) + right.var(ddof=1) / len(right))
    return float((left.mean() - right.mean()) / error) if error > 0 else float("nan")


def cluster_regression(values, flag, clusters) -> tuple[float, float]:
    """values = 상수 + 차이 × flag 회귀. (차이, 묶음 CR1 t). 보정 G/(G−1)·(n−1)/(n−2)."""
    values = np.asarray(values, float)
    flag = np.asarray(flag, float)
    design = np.column_stack([np.ones(len(values)), flag])
    inverse = np.linalg.inv(design.T @ design)
    estimate = inverse @ design.T @ values
    residuals = values - design @ estimate
    scores = pd.DataFrame(design * residuals[:, None]).groupby(np.asarray(clusters)).sum().to_numpy()
    groups = len(scores)
    count = len(values)
    covariance = groups / (groups - 1) * (count - 1) / (count - 2) * inverse @ (scores.T @ scores) @ inverse
    error = math.sqrt(covariance[1, 1])
    return float(estimate[1]), float(estimate[1] / error) if error > 0 else float("nan")


def cluster_mean_t(values, clusters) -> float:
    """평균의 묶음 CR1 t. sqrt(G/(G−1)·Σ(묶음별 잔차 합)²)/n."""
    values = np.asarray(values, float)
    clusters = np.asarray(clusters)
    finite = np.isfinite(values)
    values, clusters = values[finite], clusters[finite]
    count = len(values)

    if count < 3:
        return float("nan")

    residual = values - values.mean()
    sums = pd.Series(residual).groupby(clusters).sum().to_numpy()
    groups = len(sums)

    if groups < 2:
        return float("nan")

    error = math.sqrt(groups / (groups - 1) * float((sums ** 2).sum())) / count
    return float(values.mean() / error) if error > 0 else float("nan")


# ---------------------------------------------------------------- HTML 조각


def escape(text) -> str:
    return html.escape(str(text), quote=True)


def is_missing(value) -> bool:
    return value is None or (isinstance(value, (float, np.floating)) and math.isnan(value))


def show(value, digits: int = 2, sign: bool = True, suffix: str = "") -> str:
    if is_missing(value):
        return "–"

    number = float(value)
    text = f"{number:+,.{digits}f}" if sign else f"{number:,.{digits}f}"
    return text.replace("-", "−") + suffix


def money(value) -> str:
    """만 원 금액. 천 단위 쉼표와 부호를 붙인다."""
    return show(value, 0, suffix="만")


def percent(value, digits: int = 2) -> str:
    return show(value, digits, suffix="%")


def tone(value) -> str:
    if is_missing(value):
        return ""

    return "pos" if value > 0 else ("neg" if value < 0 else "")


def number_cell(value, digits: int = 2, sign: bool = True, suffix: str = "", colored: bool = True) -> dict:
    sort_value = "" if is_missing(value) else f"{float(value):.10g}"
    classes = "num " + (tone(value) if colored else "")
    return {"markup": escape(show(value, digits, sign, suffix)), "sort": sort_value, "class": classes.strip()}


def money_cell(value) -> dict:
    return number_cell(value, 0)


def count_cell(value) -> dict:
    return {"markup": f"{int(value):,}", "sort": str(int(value)), "class": "num"}


def text_cell(text, classes: str = "") -> dict:
    return {"markup": escape(text), "sort": None, "class": classes}


def markup_cell(markup: str, classes: str = "") -> dict:
    return {"markup": markup, "sort": None, "class": classes}


def blank_cell() -> dict:
    return {"markup": "–", "sort": None, "class": "num muted"}


def badge(text: str, kind: str) -> str:
    """kind: yes(초록)·no(빨강)·partial(노랑)·note(회색)."""
    return f'<span class="badge badge-{kind}">{escape(text)}</span>'


def verdict_cell(passed: bool, yes: str = "통과", no: str = "미달") -> dict:
    return {"markup": badge(yes if passed else no, "yes" if passed else "no"), "sort": "1" if passed else "0", "class": "verdict-cell"}


def note_cell(text: str) -> dict:
    return {"markup": badge(text, "note"), "sort": None, "class": "verdict-cell"}


def named_cell(label: str, code: str = "") -> dict:
    """사람 말 라벨 + 옆에 작은 회색 코드 이름."""
    code_markup = f' <span class="code-name">{escape(code)}</span>' if code and code != label else ""
    return {"markup": f"{escape(label)}{code_markup}", "sort": None, "class": "label"}


def as_cell(item) -> dict:
    if isinstance(item, dict):
        return item

    if isinstance(item, (int, np.integer)) and not isinstance(item, bool):
        return count_cell(item)

    if isinstance(item, (float, np.floating)):
        return number_cell(float(item))

    return text_cell(item)


def render_table(headers: list[str], rows: list[list], sortable: bool = False, caption: str = "", classes: str = "",
                 stacked: bool = False) -> str:
    """stacked = 좁은 화면에서 줄마다 카드로 쌓는다(칸마다 머리 이름을 data-label로 붙인다)."""
    cell_rows = [[as_cell(item) for item in row] for row in rows]
    numeric = []

    for position in range(len(headers)):
        column = [row[position] for row in cell_rows if position < len(row)]
        numeric_count = sum("num" in cell.get("class", "").split() for cell in column)
        numeric.append(bool(column) and numeric_count * 2 >= len(column))

    head_parts = []

    for position, header in enumerate(headers):
        class_attribute = ' class="num"' if numeric[position] else ""
        head_parts.append(f"<th{class_attribute}>{escape(header)}</th>")

    body_parts = []

    for row in cell_rows:
        cells = []

        for position, cell in enumerate(row):
            sort_attribute = f' data-sort="{escape(cell["sort"])}"' if cell.get("sort") not in (None, "") else ""
            class_attribute = f' class="{cell["class"]}"' if cell.get("class") else ""
            label_attribute = f' data-label="{escape(headers[position])}"' if stacked and position < len(headers) else ""
            cells.append(f"<td{class_attribute}{sort_attribute}{label_attribute}>{cell['markup']}</td>")

        body_parts.append("<tr>" + "".join(cells) + "</tr>")

    caption_markup = f'<p class="table-caption">{escape(caption)}</p>' if caption else ""
    table_class = ' class="sortable"' if sortable else ""
    wrap_class = "table-wrap" + (f" {classes}" if classes else "") + (" stacked" if stacked else "")
    return (f'{caption_markup}<div class="{wrap_class}"><table{table_class}><thead><tr>{"".join(head_parts)}</tr></thead>'
            f'<tbody>{"".join(body_parts)}</tbody></table></div>')


def nice_ticks(low: float, high: float, count: int = 5) -> list[float]:
    if high <= low:
        high = low + 1.0

    raw_step = (high - low) / count
    magnitude = 10 ** math.floor(math.log10(raw_step))
    step = next(multiple * magnitude for multiple in (1, 2, 2.5, 5, 10) if multiple * magnitude >= raw_step)
    first = math.floor(low / step) * step
    last = math.ceil(high / step) * step
    ticks = []
    value = first

    while value <= last + step * 1e-9:
        ticks.append(round(value, 10))
        value += step

    return ticks


def bar_chart(categories: list[str], series: list[tuple[str, list[float]]], unit: str, title: str) -> str:
    """묶음 막대. 축 눈금은 실제 값. series = [(이름, 값 목록)]."""
    width, height = 760, 280
    left, right, top, bottom = 64, 12, 18, 44
    plot_width = width - left - right
    plot_height = height - top - bottom
    finite = [value for _, values in series for value in values if value is not None and np.isfinite(value)]
    ticks = nice_ticks(min(finite + [0.0]), max(finite + [0.0]))
    low, high = ticks[0], ticks[-1]

    def y_of(value: float) -> float:
        return top + (high - value) / (high - low) * plot_height

    parts = [f'<svg class="chart" viewBox="0 0 {width} {height}" role="img" aria-label="{escape(title)}">']

    for tick in ticks:
        position = y_of(tick)
        label = show(tick, 0 if float(tick).is_integer() else 1, sign=True)
        parts.append(f'<line class="grid" x1="{left}" x2="{width - right}" y1="{position:.1f}" y2="{position:.1f}"/>')
        parts.append(f'<text class="axis-text" x="{left - 6}" y="{position + 4:.1f}" text-anchor="end">{escape(label)}</text>')

    parts.append(f'<line class="zero" x1="{left}" x2="{width - right}" y1="{y_of(0):.1f}" y2="{y_of(0):.1f}"/>')
    group_width = plot_width / max(len(categories), 1)
    bar_width = group_width * 0.8 / max(len(series), 1)

    for category_position, category in enumerate(categories):
        group_left = left + category_position * group_width + group_width * 0.1

        for series_position, (name, values) in enumerate(series):
            value = values[category_position]

            if value is None or not np.isfinite(value):
                continue

            bar_top = min(y_of(value), y_of(0))
            bar_height = abs(y_of(value) - y_of(0))
            parts.append(f'<rect class="bar bar-{series_position + 1}" x="{group_left + series_position * bar_width:.1f}" '
                         f'y="{bar_top:.1f}" width="{max(bar_width - 1, 1):.1f}" height="{max(bar_height, 0.5):.1f}">'
                         f'<title>{escape(category)} {escape(name)} {escape(show(value, 2))}{escape(unit)}</title></rect>')

        parts.append(f'<text class="axis-text" x="{group_left + group_width * 0.4:.1f}" y="{height - bottom + 16}" '
                     f'text-anchor="middle">{escape(category)}</text>')

    parts.append(f'<text class="axis-text" x="{left}" y="{height - 8}">단위 {escape(unit)}</text></svg>')
    legend = "".join(f'<span class="legend-item"><span class="swatch bar-{position + 1}"></span>{escape(name)}</span>'
                     for position, (name, _) in enumerate(series))
    return f'<figure class="chart-box"><figcaption>{escape(title)}</figcaption>{"".join(parts)}<div class="legend">{legend}</div></figure>'


def heatmap(row_labels: list[str], column_labels: list[str], grid: list[list[float]], caption: str, digits: int = 2) -> str:
    finite = [abs(value) for row in grid for value in row if value is not None and np.isfinite(value)]
    limit = max(finite) if finite else 1.0
    head = "<th></th>" + "".join(f'<th class="num">{escape(label)}</th>' for label in column_labels)
    body = []

    for label, row in zip(row_labels, grid):
        cells = []

        for value in row:
            strength = 0 if value is None or not np.isfinite(value) else round(min(1.0, abs(value) / limit) * 45)
            token = "--pos" if (value or 0) > 0 else "--neg"
            cells.append(f'<td class="num heat" style="background:color-mix(in srgb, var({token}) {strength}%, transparent)">'
                         f'{escape(show(value, digits))}</td>')

        body.append(f"<tr><th>{escape(label)}</th>{''.join(cells)}</tr>")

    return (f'<div class="heatmap-box"><p class="table-caption">{escape(caption)}</p><div class="table-wrap"><table class="heatmap">'
            f'<thead><tr>{head}</tr></thead><tbody>{"".join(body)}</tbody></table></div></div>')


def raw_text_block(path: Path) -> str:
    lines = read_lines(path)
    return (f'<details class="inner"><summary>원문 보기 — {escape(relative(path))} ({len(lines)}줄)</summary>'
            f'<pre class="raw">{escape(chr(10).join(lines))}</pre></details>')


def check_block(ledger: Ledger) -> str:
    """대조 결과: 불일치는 늘 보이게, 전체 목록은 접어 둔다."""
    status = (badge(f"{ledger.matched_count:,}/{ledger.total:,} 일치", "yes") if not ledger.mismatches
              else badge(f"불일치 {len(ledger.mismatches)}건", "no"))
    summary = f'<p>{status} 이 시험의 숫자 {ledger.total:,}개를 산출 파일(parquet·json)에서 이 생성기가 다시 계산해 txt 값과 맞춰 봤다.</p>'
    headers = ["항목", "다시 계산한 값", "txt에 적힌 값", "방법", "txt 위치", "결과"]

    def as_rows(rows: list[dict]) -> list[list]:
        return [[row["label"], markup_cell(escape(row["computed"]), "num"), markup_cell(escape(row["reported"]), "num"),
                 row["kind"], markup_cell(f'<code>{escape(row["source"])}</code>'), verdict_cell(row["matched"], "일치", "불일치")]
                for row in rows]

    mismatch_table = render_table(headers, as_rows(ledger.mismatches), caption="불일치 항목") if ledger.mismatches else ""
    full_table = render_table(headers, as_rows(ledger.rows), sortable=True)
    return summary + mismatch_table + f'<details class="inner"><summary>대조 {ledger.total:,}건 전부 보기</summary>{full_table}</details>'


CONDITION_HEADERS = ["항목", "값", "출처(파일:줄)", "코드와 설명"]
SECTION_INDEX: dict[str, dict] = {}


def section(identifier: str, title: str, answer: tuple[str, str], answer_text: str, compare_markup: str, criteria: list[list],
            yearly_markup: str, details: list[tuple[str, str]], conditions: list[list], script: Path, commands: list[str],
            outputs: list[Path], raw_paths: list[Path], ledger: Ledger) -> str:
    """한 시험. ① 질문 ② 답 ③ 비교 ④ 통과 기준 ⑤ 연도별 ⑥ 나머지는 '자세히'로 접는다. answer = (답, 배지 종류)."""
    criteria_table = render_table(["기준", "넘어야 할 값", "실제", "판정"], criteria, classes="criteria")
    yearly_block = f"<h3>연도별</h3>{yearly_markup}" if yearly_markup else ""
    detail_parts = "".join(f"<h4>{escape(heading)}</h4>{markup}" for heading, markup in details)
    condition_markup = render_table(CONDITION_HEADERS, conditions)
    command_markup = "".join(f'<pre class="command">{escape(command)}</pre>' for command in commands)
    output_markup = '<ul class="paths">' + "".join(f"<li><code>{escape(relative(path))}</code></li>" for path in outputs) + "</ul>"
    raw_markup = "".join(raw_text_block(path) for path in raw_paths)
    sources = [script] + [path for path in outputs if path.suffix == ".txt"]
    source_markup = " · ".join(f"<code>{escape(relative(path))}</code>" for path in sources)
    SECTION_INDEX[identifier] = {"title": title, "answer": answer}
    return f"""
<section id="{identifier}" class="test">
<h2>{escape(title)}</h2>
<p class="answer">{badge(answer[0], answer[1])} <b>{escape(answer_text)}</b></p>
<h3>비교</h3>{compare_markup}
<h3>통과 기준</h3>{criteria_table}
{yearly_block}
<details class="more"><summary>자세히 — 조건·전체 표·재계산 대조·원문</summary><div class="more-body">
{detail_parts}
<h4>시험 조건</h4>{condition_markup}
<h4>재계산 대조</h4>{check_block(ledger)}
<h4>다시 돌리기·산출 파일</h4><p class="muted">저장소 루트에서</p>{command_markup}{output_markup}{raw_markup}
</div></details>
<p class="source">출처 {source_markup}</p>
</section>"""


def same(text: str = "같음") -> dict:
    return text_cell(text, "muted")


def differs(text: str) -> dict:
    return text_cell(text, "neg")


# ---------------------------------------------------------------- 1·2 halt_trigger


def halt_evaluate(trades: pd.DataFrame, keep: np.ndarray, base_drawdown: float, seed: int, draws: int, split_year: int,
                  capital: float, years: list[int]) -> dict:
    profits = trades["profit"].to_numpy(float)
    returns = trades["ret"].to_numpy(float)
    early = (trades["year"] < split_year).to_numpy()
    kept_returns = returns[keep]
    dropped_returns = returns[~keep]
    exit_order = trades[keep].sort_values(["exit_date", "entry_date"], kind="mergesort")["profit"].to_numpy(float)
    row = {"n": int(keep.sum()), "mean": float(kept_returns.mean()), "t": plain_t(kept_returns), "win": float((kept_returns > 0).mean() * 100.0),
           "mean_early": float(returns[keep & early].mean()), "mean_late": float(returns[keep & ~early].mean()),
           "total": float(profits[keep].sum()), "drawdown": max_drawdown(profits[keep]), "drawdown_exit_order": max_drawdown(exit_order),
           "drawdown_early": max_drawdown(profits[keep & early]), "drawdown_late": max_drawdown(profits[keep & ~early]),
           "dropped_n": int((~keep).sum()), "dropped_mean": float(dropped_returns.mean()) if len(dropped_returns) else float("nan"),
           "difference_t": welch(dropped_returns, kept_returns)}
    yearly = pd.Series(profits[keep]).groupby(trades["entry_year"].to_numpy()[keep]).sum().reindex(years, fill_value=0.0) / capital * 100.0
    row["yearly"] = [float(value) for value in yearly]
    generator = np.random.default_rng(seed)
    improvements = []

    for _ in range(draws):
        mask = np.ones(len(trades), bool)

        if row["dropped_n"]:
            mask[generator.choice(len(trades), size=row["dropped_n"], replace=False)] = False

        improvements.append(max_drawdown(profits[mask]) - base_drawdown)

    row["improvement"] = row["drawdown"] - base_drawdown
    row["random95"] = float(np.quantile(improvements, 0.95))
    row["random_median"] = float(np.median(improvements))
    return row


ROW_PATTERN = (r"n\s*(\d+) 평균\s*(\S+)% t\s*(\S+) 승\s*(\S+)% \| 10–17\s*(\S+) 18–26\s*(\S+) \| 총 (\S+)만 낙폭 (\S+)만"
               r"\(청산일 순 (\S+)만, 10–17 (\S+)만, 18–26 (\S+)만\)")
DROP_PATTERN = r"걸러냄 n(\d+) 평균 (\S+)% 차이 t(\S+) \| 낙폭 개선 (\S+)만 vs 무작위 중앙 (\S+)만·95% (\S+)만"
JUDGE_NAMES = ("낙폭30%", "손익", "이웃", "두구간", "무작위95")


def halt_parse_block(header_line: int) -> dict | None:
    """header_line(1부터) 다음 세 줄을 읽는다."""
    lines = read_lines(HALT_TEXT)
    row_found = re.search(ROW_PATTERN, lines[header_line])
    drop_found = re.search(DROP_PATTERN, lines[header_line + 1])
    years_found = YEAR_LABEL_PATTERN.findall(lines[header_line + 2])

    if not row_found or not drop_found:
        return None

    keys = ("n", "mean", "t", "win", "mean_early", "mean_late", "total", "drawdown", "drawdown_exit_order", "drawdown_early", "drawdown_late")
    parsed = dict(zip(keys, row_found.groups()))
    parsed.update(dict(zip(("dropped_n", "dropped_mean", "difference_t", "improvement", "random_median", "random95"), drop_found.groups())))
    parsed["yearly"] = [value for _, value in years_found]
    parsed["row_line"] = header_line + 1
    parsed["drop_line"] = header_line + 2
    parsed["year_line"] = header_line + 3
    return parsed


def halt_compare(ledger: Ledger, name: str, computed: dict, parsed: dict) -> None:
    source_row = f"{relative(HALT_TEXT)}:{parsed['row_line']}"
    source_drop = f"{relative(HALT_TEXT)}:{parsed['drop_line']}"
    source_year = f"{relative(HALT_TEXT)}:{parsed['year_line']}"

    for key, label in (("n", "남은 n"), ("mean", "평균 %"), ("t", "t"), ("win", "승률 %"), ("mean_early", "10–17 평균"),
                       ("mean_late", "18–26 평균"), ("total", "총 손익(만)"), ("drawdown", "낙폭(만, 매수일 순)"),
                       ("drawdown_exit_order", "낙폭(만, 청산일 순)"), ("drawdown_early", "10–17 낙폭"), ("drawdown_late", "18–26 낙폭")):
        ledger.number(f"{name} · {label}", computed[key], parsed[key], source_row)

    for key, label in (("dropped_n", "걸러낸 n"), ("dropped_mean", "걸러낸 평균"), ("difference_t", "차이 t"), ("improvement", "낙폭 개선"),
                       ("random_median", "무작위 중앙"), ("random95", "무작위 95%")):
        ledger.number(f"{name} · {label}", computed[key], parsed[key], source_drop)

    for year_label, (value, reported) in zip(range(len(parsed["yearly"])), zip(computed["yearly"], parsed["yearly"])):
        ledger.number(f"{name} · 연도 %[{year_label}]", value, reported, source_year)


def halt_judge(rows: dict, name: str, neighbors: list[str], base: dict) -> dict:
    row = rows[name]
    return {
        "낙폭30%": row["drawdown"] >= 0.7 * base["drawdown"],
        "손익": row["total"] >= 0.8 * base["total"] or row["mean"] > base["mean"],
        "이웃": all(rows[other]["improvement"] > 0 and (rows[other]["dropped_n"] == 0 or rows[other]["dropped_mean"] < rows[other]["mean"])

                  for other in neighbors) and row["dropped_n"] > 0 and row["dropped_mean"] < row["mean"],
        "두구간": row["drawdown_early"] > base["drawdown_early"] and row["drawdown_late"] > base["drawdown_late"],
        "무작위95": row["improvement"] > row["random95"],
    }


def halt_compute() -> dict:
    module = load_module(HALT_SCRIPT, "ledger_halt_trigger")
    seed, _ = constant_of(HALT_SCRIPT, "SEED")
    draws, _ = constant_of(HALT_SCRIPT, "DRAWS")
    split_year, _ = constant_of(HALT_SCRIPT, "SPLIT_YEAR")
    capital, _ = constant_of(HALT_SCRIPT, "CAPITAL_WON")
    trades = pd.read_parquet(HALT_TRADES)
    calendar = pd.read_parquet(INDEX_SNAPSHOT).index.to_numpy()
    years = sorted(int(year) for year in trades["entry_year"].unique())
    all_keep = np.ones(len(trades), bool)
    early = (trades["year"] < split_year).to_numpy()
    profits = trades["profit"].to_numpy(float)
    base = {"drawdown": max_drawdown(profits), "total": float(profits.sum()), "mean": float(trades["ret"].mean()),
            "drawdown_early": max_drawdown(profits[early]), "drawdown_late": max_drawdown(profits[~early])}
    base_row = halt_evaluate(trades, all_keep, base["drawdown"], seed, draws, split_year, capital, years)
    items = module.conditions(trades, calendar)
    rows = {}

    for number, item in enumerate(items):
        rows[item["name"]] = halt_evaluate(trades, item["keep"], base["drawdown"], seed + number + 1, draws, split_year, capital, years)
        rows[item["name"]]["family"] = item["family"]

    for item in items:
        neighbors = module.neighbor_names(items, item)
        rows[item["name"]]["neighbors"] = neighbors
        rows[item["name"]]["checks"] = halt_judge(rows, item["name"], neighbors, base)
        rows[item["name"]]["passed"] = all(rows[item["name"]]["checks"].values())

    # 국면별(T1) 성적
    regimes = {}

    for column in ("label_pre_entry", "label_close_signal"):
        for label in ("RISK_ON", "NEUTRAL", "RISK_OFF"):
            part = trades[trades[column] == label]
            regimes[(column, label)] = {"n": len(part), "mean": float(part["ret"].mean()), "t": plain_t(part["ret"]),
                                        "win": float((part["ret"] > 0).mean() * 100.0), "total": float(part["profit"].sum())}

    return {"trades": trades, "years": years, "base": base, "base_row": base_row, "items": items, "rows": rows, "regimes": regimes,
            "seed": seed, "draws": draws, "split_year": split_year, "capital": capital}


def halt_ledgers(data: dict) -> tuple[Ledger, Ledger]:
    first = Ledger("halt_trigger T1")
    second = Ledger("halt_trigger T2·T3")
    base_index = text_line_index(HALT_TEXT, r"^기준\(거름 없음\)")
    parsed = halt_parse_block(base_index + 1) if base_index >= 0 else None

    if parsed:
        halt_compare(first, "기준 263건", data["base_row"], parsed)
    else:
        first.missing("기준 블록", relative(HALT_TEXT))

    # 국면별
    for column, heading in (("label_pre_entry", r"매수일 개장 전\(실매매 시점\)"), ("label_close_signal", r"^\s+신호일 종가$")):
        start = text_line_index(HALT_TEXT, heading)

        for label in ("RISK_ON", "NEUTRAL", "RISK_OFF"):
            found, line = text_match(HALT_TEXT, rf"^\s+{label}\s+n\s*(\d+) 평균\s+(\S+)% t(\S+) 승(\S+)% 총 (\S+)만", start, start + 4)
            computed = data["regimes"][(column, label)]
            source = f"{relative(HALT_TEXT)}:{line}"

            if not found:
                first.missing(f"국면 {column} {label}", relative(HALT_TEXT))
                continue

            for key, position in (("n", 1), ("mean", 2), ("t", 3), ("win", 4), ("total", 5)):
                first.number(f"국면 {'개장 전' if 'pre' in column else '신호일 종가'} {label} · {key}", computed[key], found.group(position), source)

    for item in data["items"]:
        name = item["name"]
        ledger = first if item["family"] == "T1" else second
        header_index = text_line_index(HALT_TEXT, r"^\[" + item["family"] + r"\] " + re.escape(name) + " — ")

        if header_index < 0:
            ledger.missing(name, relative(HALT_TEXT))
            continue

        header = read_lines(HALT_TEXT)[header_index]
        block = halt_parse_block(header_index + 1)

        if block is None:
            ledger.missing(f"{name} 본문", relative(HALT_TEXT))
            continue

        halt_compare(ledger, name, data["rows"][name], block)
        reported_flags = dict(re.findall(r"(낙폭30%|손익|이웃|두구간|무작위95)([Ox])", header))
        source = f"{relative(HALT_TEXT)}:{header_index + 1}"

        for judge_name in JUDGE_NAMES:
            ledger.text(f"{name} · 판정 {judge_name}", "O" if data["rows"][name]["checks"][judge_name] else "x", reported_flags.get(judge_name, "?"), source)

        ledger.text(f"{name} · 종합", "통과" if data["rows"][name]["passed"] else "탈락", "통과" if " — 통과 " in header else "탈락", source)

    return first, second


def halt_condition_rows(data: dict) -> list[list]:
    filter_line, filter_text = line_of(HALT_SCRIPT, r'events\["depth_low"\] < 15')
    position, position_source = constant_source(HALT_SCRIPT, "POSITION_WON")
    capital, capital_source = constant_source(HALT_SCRIPT, "CAPITAL_WON")
    seed, seed_source = constant_source(HALT_SCRIPT, "SEED")
    draws, draws_source = constant_source(HALT_SCRIPT, "DRAWS")
    split_year, split_source = constant_source(HALT_SCRIPT, "SPLIT_YEAR")
    trades = data["trades"]
    plan_unit = line_of(HALT_SCRIPT, r"최대 낙폭\(원\)")
    return [
        ["언제 사나", f"급등 상자 돌파 뒤 되돌림이 얕은 종목({filter_text}) → 다음 날 시가에 산다", f"{relative(HALT_SCRIPT)}:{filter_line}", same()],
        ["언제 파나", "지켜본 최저 저가 −3%에서 손절, +15%에서 익절, 최대 120거래일", where(HALT_SCRIPT, r"손절 관찰 최저 저가"), same()],
        ["비용", "수수료·세금을 뺀 수익(스터디 35가 만든 값을 그대로 씀)", where(HALT_SCRIPT, r"비용 포함 수익"), same()],
        ["금액", f"종목당 {position:,.0f}만 원, 자본 {capital:,.0f}만 원, 동시 보유 한도 없음", position_source, same()],
        ["거래 가능성", "급등 사건 자료의 거름을 그대로 씀(따로 더 거르지 않음)", where(HALT_SCRIPT, r"EVENTS_PATH ="), same()],
        ["기간", f"매수일 {trades['entry_date'].min()}–{trades['entry_date'].max()}, 앞·뒤 기간 경계 {split_year}년", split_source,
         differs("기간 평균은 급등일 연도로, 연도별 막대는 매수 연도로 나눈다")],
        ["매매 수", f"{len(trades)}건", relative(HALT_TRADES), same()],
        ["판단 시점", "국면 점수는 매수일 개장 전(실매매와 같은 시점)과 신호일 종가 두 가지, 지수는 매수일 직전 종가, 최근 손익은 매수일 전에 끝난 매매만", where(HALT_SCRIPT, r"def attach_index"), same()],
        ["무작위 비교", f"같은 수의 매매를 무작위로 빼기 {draws:,}번(seed {seed})", draws_source, same()],
        ["낙폭 계산 순서", "매수일 순서로 누적(청산일 순서 값도 같이 적음)", where(HALT_SCRIPT, r'"drawdown": max_drawdown\(kept'),
         differs("계획서는 '최대 낙폭(원)'이라 적었지만 실제 단위는 만 원") if plan_unit[0] else same()],
    ]


HALT_FAMILY_LABELS = {"T1": "국면 점수", "T2": "지수 위치", "T3": "최근 손익"}


def halt_label(name: str) -> str:
    return name.replace("-", "−")


def halt_compare_table(data: dict, chosen: list[tuple[str, str]]) -> str:
    """chosen = [(조건 이름, 사람 말 라벨)]. 맨 위에 '그대로' 줄."""
    base_row = data["base_row"]
    rows = [[named_cell("그대로(멈추지 않음)"), count_cell(base_row["n"]), number_cell(base_row["mean"]), money_cell(base_row["total"]),
             money_cell(base_row["drawdown"]), blank_cell()]]

    for name, label in chosen:
        row = data["rows"][name]
        rows.append([named_cell(label), count_cell(row["n"]), number_cell(row["mean"]), money_cell(row["total"]), money_cell(row["drawdown"]),
                     money_cell(row["improvement"])])

    return render_table(["방식", "매매 수", "매매당 평균 %", "총 손익(만 원)", "최대 낙폭(만 원)", "낙폭 개선(만 원)"], rows, classes="compare")


def halt_result_rows(data: dict, family_filter) -> list[list]:
    rows = []

    for item in data["items"]:
        if not family_filter(item["family"]):
            continue

        row = data["rows"][item["name"]]
        rows.append([HALT_FAMILY_LABELS.get(item["family"], item["family"]), halt_label(item["name"]), count_cell(row["n"]), number_cell(row["mean"]),
                     number_cell(row["t"]), money_cell(row["total"]), money_cell(row["drawdown"]), money_cell(row["improvement"]),
                     number_cell(row["random95"], 0, colored=False), count_cell(row["dropped_n"]), number_cell(row["dropped_mean"]),
                     number_cell(row["difference_t"])]
                    + [verdict_cell(row["checks"][name], "O", "x") for name in JUDGE_NAMES] + [verdict_cell(row["passed"])])

    return rows


HALT_HEADERS = ["종류", "조건", "남은 매매", "매매당 평균 %", "t", "총 손익(만 원)", "최대 낙폭(만 원)", "낙폭 개선(만 원)", "무작위 95% 선(만 원)",
                "뺀 매매", "뺀 매매 평균 %", "차이 t", "①", "②", "③", "④", "⑤", "종합"]
HALT_HEADER_NOTE = '<p class="muted">①–⑤는 위 통과 기준 표의 번호. 차이 t는 뺀 매매와 남긴 매매의 평균 차이가 얼마나 확실한지(음수면 뺀 쪽이 나쁨).</p>'


def halt_criteria(data: dict, family_filter) -> list[list]:
    base = data["base"]
    names = [item["name"] for item in data["items"] if family_filter(item["family"])]
    rows = data["rows"]
    total = len(names)

    def count(check: str) -> int:
        return sum(rows[name]["checks"][check] for name in names)

    def partial(found: int) -> dict:
        return verdict_cell(found > 0, f"{found}칸 충족", "충족 칸 없음")

    strong_t = sum(1 for name in names if np.isfinite(rows[name]["difference_t"]) and abs(rows[name]["difference_t"]) >= 3.1)
    passed = [name for name in names if rows[name]["passed"]]
    return [
        ["① 최대 낙폭이 30% 이상 준다", f"낙폭이 {money(0.7 * base['drawdown'])}보다 얕게", f"{count('낙폭30%')}/{total}칸", partial(count("낙폭30%"))],
        ["② 총 손익을 80% 이상 지킨다(또는 매매당 평균이 오른다)", f"총 {money(0.8 * base['total'])} 이상 또는 평균 {percent(base['mean'])} 초과",
         f"{count('손익')}/{total}칸", partial(count("손익"))],
        ["③ 이웃 조건(길이·문턱을 한 칸 바꾼 것)도 같은 방향이다", "이웃 칸 모두 낙폭이 줄고, 뺀 매매가 남긴 매매보다 나쁘다", f"{count('이웃')}/{total}칸", partial(count("이웃"))],
        ["④ 앞 기간(2010–2017)·뒤 기간(2018–2026) 모두 낙폭이 준다", f"앞 {money(base['drawdown_early'])}·뒤 {money(base['drawdown_late'])}보다 얕게",
         f"{count('두구간')}/{total}칸", partial(count("두구간"))],
        ["⑤ 같은 수를 무작위로 뺀 것보다 낫다", f"낙폭 개선이 무작위 {data['draws']:,}번의 상위 5% 선보다 크다", f"{count('무작위95')}/{total}칸", partial(count("무작위95"))],
        ["참고: 뺀 매매가 확실히 나쁘다(계획서 기준, 판정 코드에는 없음)", "차이 |t| ≥ 3.1", f"{strong_t}/{total}칸", partial(strong_t)],
        ["종합: ①–⑤를 모두 넘은 칸", "1칸 이상", f"{len(passed)}/{total}칸" + (f" — {', '.join(halt_label(name) for name in passed)}" if passed else ""),
         verdict_cell(bool(passed), "있음", "없음")],
    ]


def halt_sections(data: dict, first: Ledger, second: Ledger) -> tuple[str, str]:
    years = data["years"]
    year_labels = [str(year)[2:] for year in years]
    base_row = data["base_row"]
    condition_rows = halt_condition_rows(data)
    commands = ["py -X utf8 research/studies/35_surge_box_breakout/halt_trigger.py"]
    outputs = [HALT_TEXT, HALT_TRADES, SURGE / "halt_trigger_conditions.parquet"]

    # 1. 국면 점수
    regime_names = {"RISK_ON": "좋음", "NEUTRAL": "보통", "RISK_OFF": "나쁨"}
    regime_rows = []

    for column, timing in (("label_pre_entry", "매수일 개장 전"), ("label_close_signal", "신호일 종가")):
        for label in ("RISK_ON", "NEUTRAL", "RISK_OFF"):
            regime = data["regimes"][(column, label)]
            regime_rows.append([timing, named_cell(regime_names[label], label), count_cell(regime["n"]), number_cell(regime["mean"]), number_cell(regime["t"]),
                                number_cell(regime["win"], 1, sign=False, colored=False), money_cell(regime["total"])])

    t1_items = [item for item in data["items"] if item["family"] == "T1"]
    engine_name = next(item["name"] for item in t1_items if "개장 전" in item["name"] and "-7" in item["name"])
    engine_row = data["rows"][engine_name]
    pre_entry = [(item["name"], f"개장 전 점수 {item['name'].split('≤ ')[1].split(' ')[0].replace('-', '−')} 이하면 안 삼"
                  + (" (엔진 값)" if item["name"] == engine_name else "")) for item in t1_items if "개장 전" in item["name"]]
    t1_compare = halt_compare_table(data, pre_entry)
    t1_yearly = bar_chart(year_labels, [("그대로", base_row["yearly"]), ("개장 전 점수 −7 이하면 안 삼", engine_row["yearly"])],
                          "% (자본 2,000만 원 대비)", "매수 연도별 손익")
    t1_passed = [item["name"] for item in t1_items if data["rows"][item["name"]]["passed"]]
    t1_answer = (f"아니오. 엔진 정지선(개장 전 점수 −7 이하)에 걸리는 매매가 {base_row['n']}건 중 {engine_row['dropped_n']}건뿐이라 "
                 f"최대 낙폭이 줄지 않았다({money(base_row['drawdown'])} → {money(engine_row['drawdown'])}). 6칸 중 통과 {len(t1_passed)}칸.")
    t1_details = [
        ("국면별 성적(국면 점수가 좋음·보통·나쁨일 때 산 매매)",
         render_table(["판단 시점", "국면", "매매 수", "매매당 평균 %", "t", "승률 %", "총 손익(만 원)"], regime_rows)),
        ("6칸 전체", render_table(HALT_HEADERS, halt_result_rows(data, lambda family: family == "T1"), sortable=True) + HALT_HEADER_NOTE),
    ]
    t1_section = section("t1", "1. 국면 점수가 나쁘면 매수를 멈추면 나아지나?", ("아니오", "no") if not t1_passed else ("일부", "partial"), t1_answer,
                         t1_compare, halt_criteria(data, lambda family: family == "T1"), t1_yearly, t1_details, condition_rows, HALT_SCRIPT,
                         commands, outputs, [HALT_TEXT], first)

    # 2. 지수 위치·최근 손익
    passed_names = [item["name"] for item in data["items"] if item["family"] != "T1" and data["rows"][item["name"]]["passed"]]
    t23_compare = halt_compare_table(data, [(name, halt_label(name)) for name in passed_names])
    series = [("그대로", base_row["yearly"])] + [(halt_label(name), data["rows"][name]["yearly"]) for name in passed_names[:3]]
    t23_yearly = bar_chart(year_labels, series, "% (자본 2,000만 원 대비)", "매수 연도별 손익 — 그대로와 통과 칸")
    differences = ", ".join(show(data["rows"][name]["difference_t"]) for name in passed_names)
    t23_answer = (f"낙폭은 줄었다. 24칸 중 {len(passed_names)}칸이 기준 다섯 개를 모두 넘었지만, 뺀 매매가 실제로 더 나빴다는 근거는 약하다"
                  f"(차이 t {differences}). 3번에서 더 엄격하게 다시 봤다.")
    t23_cells = ("<p>지수 위치 12칸: 코스닥·코스피 각각 종가가 20·60·120일 평균 아래이거나 250일 고점보다 10·15·20% 이상 낮으면 안 삼. "
                 "최근 손익 12칸: 최근 5·10·20건 손익 합이 음수면 멈춤 / 손절이 3·4·5번 이어지면 20·60거래일 쉼 / "
                 "가상 누적 손익이 고점보다 400·600·800만 원 이상 빠지면 멈춤.</p>")
    t23_details = [("24칸 전체(열 머리를 누르면 정렬)", t23_cells + render_table(HALT_HEADERS, halt_result_rows(data, lambda family: family != "T1"), sortable=True)
                    + HALT_HEADER_NOTE)]
    t23_section = section("t23", "2. 코스닥이 이평선 아래이거나 최근 손실이 이어지면 멈추면 나아지나?", ("일부", "partial") if passed_names else ("아니오", "no"),
                          t23_answer, t23_compare, halt_criteria(data, lambda family: family != "T1"), t23_yearly, t23_details, condition_rows,
                          HALT_SCRIPT, commands, outputs, [], second)
    return t1_section, t23_section


# ---------------------------------------------------------------- 3 halt_validate


def validate_features() -> pd.DataFrame:
    index = pd.read_parquet(INDEX_SNAPSHOT).sort_index()
    features = pd.DataFrame(index=index.index)
    high_window, _ = constant_of(VALIDATE_SCRIPT, "HIGH_WINDOW")

    for symbol, label in (("KQ11", "kosdaq"), ("KS11", "kospi")):
        close = index[symbol]

        for days in range(10, 251, 10):
            average = close.rolling(days).mean()
            features[f"{label}_below{days}"] = (close < average).astype(float).where(average.notna())

        features[f"{label}_drawdown"] = (close / close.rolling(high_window, min_periods=20).max() - 1.0) * 100.0

    return features


def validate_keep(frame: pd.DataFrame, condition: str) -> np.ndarray:
    if condition == "none":
        return np.ones(len(frame), bool)

    if "_drawdown" in condition:
        label, depth = condition.split("_drawdown")
        return ~(frame[f"{label}_drawdown"].to_numpy(float) <= -float(depth))

    return ~(frame[condition].to_numpy(float) == 1.0)


def validate_label(condition: str) -> str:
    if condition == "none":
        return "거름 없음"

    korean = "코스닥" if condition.startswith("kosdaq") else "코스피"

    if "_drawdown" in condition:
        return f"{korean} 고점 −{condition.split('_drawdown')[1]}%"

    return f"{korean} {condition.split('_below')[1]}일선"


def validate_compute() -> dict:
    lengths, _ = constant_of(VALIDATE_SCRIPT, "AVERAGE_LENGTHS")
    depths, _ = constant_of(VALIDATE_SCRIPT, "DEPTH_LEVELS")
    seed, _ = constant_of(VALIDATE_SCRIPT, "SEED")
    draws, _ = constant_of(VALIDATE_SCRIPT, "BOOTSTRAP_DRAWS")
    floor, _ = constant_of(VALIDATE_SCRIPT, "DRAWDOWN_FLOOR")
    warmup, _ = constant_of(VALIDATE_SCRIPT, "MINIMUM_TRAINING_YEARS")
    capital, _ = constant_of(VALIDATE_SCRIPT, "CAPITAL_WON")
    trades = pd.read_parquet(HALT_TRADES, columns=["code", "entry_date", "exit_date", "entry_year", "ret", "profit"]).reset_index(drop=True)
    features = validate_features()
    calendar = features.index.to_numpy()
    positions = np.searchsorted(calendar, trades["entry_date"].to_numpy(), side="left") - 1

    for column in features.columns:
        trades[column] = features[column].to_numpy()[positions]

    profits = trades["profit"].to_numpy(float)
    base_drawdown = max_drawdown(profits)
    base_total = float(profits.sum())

    def summary(keep: np.ndarray) -> dict:
        kept_returns = trades["ret"].to_numpy(float)[keep]
        dropped_returns = trades["ret"].to_numpy(float)[~keep]
        return {"n": int(keep.sum()), "total": float(profits[keep].sum()), "drawdown": max_drawdown(profits[keep]),
                "kept_mean": float(kept_returns.mean()), "dropped_n": int((~keep).sum()),
                "dropped_mean": float(dropped_returns.mean()) if len(dropped_returns) else float("nan")}

    # V1 걸어가며 고르기
    conditions = ["none"] + [f"{label}_below{days}" for label in ("kosdaq", "kospi") for days in lengths] \
        + [f"{label}_drawdown{depth}" for label in ("kosdaq", "kospi") for depth in depths]
    masks = {condition: validate_keep(trades, condition) for condition in conditions}
    years = sorted(int(year) for year in trades["entry_year"].unique())
    first_test_year = years[0] + warmup
    keep = np.ones(len(trades), bool)
    choices = []

    for year in years:
        in_year = (trades["entry_year"] == year).to_numpy()

        if year < first_test_year:
            choices.append({"year": year, "label": "거름 없음(자료 쌓는 중)", "training_n": 0, "year_n": int(in_year.sum()), "year_kept": int(in_year.sum())})
            continue

        training = (trades["exit_date"] < year * 10000 + 101).to_numpy()
        scored = []

        for order, condition in enumerate(conditions):
            subset = profits[training & masks[condition]]
            scored.append((float(subset.sum()) / max(abs(max_drawdown(subset)), floor), -order, condition))

        scored.sort(reverse=True)
        best = scored[0]
        keep[in_year] = masks[best[2]][in_year]
        choices.append({"year": year, "label": validate_label(best[2]), "objective": best[0], "training_n": int(training.sum()),
                        "none_objective": next(score for score, _, condition in scored if condition == "none"),
                        "year_n": int(in_year.sum()), "year_kept": int(masks[best[2]][in_year].sum())})

    walk = summary(keep)
    fixed = masks["kosdaq_below120"]

    def yearly(mask: np.ndarray) -> list[float]:
        return list(pd.Series(profits[mask]).groupby(trades["entry_year"].to_numpy()[mask]).sum().reindex(years, fill_value=0.0) / capital * 100.0)

    # V2 넓힌 표본
    wide = pd.read_parquet(VALIDATE_WIDE)
    wide_rows = []

    for days in lengths:
        column = f"kosdaq_below{days}"
        usable = wide[column].notna() & wide["ret_low3_t15"].notna()
        part = wide[usable]
        below = part[column].to_numpy(float)
        values = part["ret_low3_t15"].to_numpy(float)
        difference, t_value = cluster_regression(values, below, part["month"].to_numpy())
        wide_rows.append({"days": days, "below_n": int(below.sum()), "below_mean": float(values[below == 1].mean()),
                          "above_n": int((below == 0).sum()), "above_mean": float(values[below == 0].mean()), "difference": difference,
                          "t": t_value, "plain_t": welch(values[below == 1], values[below == 0])})

    # V3 길이 훑기
    sweep = []

    for days in range(10, 251, 10):
        mask = validate_keep(trades, f"kosdaq_below{days}")
        row = summary(mask)
        row["days"] = days
        row["improvement"] = row["drawdown"] - base_drawdown
        row["missing"] = int(trades[f"kosdaq_below{days}"].isna().sum())
        sweep.append(row)

    band = [row for row in sweep if 60 <= row["days"] <= 200]

    # V4 연도 묶음 재표집
    generator = np.random.default_rng(seed)
    blocks = {year: (trades["entry_year"] == year).to_numpy() for year in years}
    improvements, ratios = [], []

    for _ in range(draws):
        drawn = generator.choice(years, size=len(years), replace=True)
        base_profits = np.concatenate([profits[blocks[year]] for year in drawn])
        filtered_profits = np.concatenate([profits[blocks[year] & fixed] for year in drawn])
        improvements.append(max_drawdown(filtered_profits) - max_drawdown(base_profits))
        ratios.append(filtered_profits.sum() / base_profits.sum() if base_profits.sum() > 0 else float("nan"))

    improvements = np.array(improvements)
    ratios = np.array(ratios)
    return {"trades": trades, "years": years, "choices": choices, "walk": walk, "keep": keep, "base_total": base_total,
            "base_drawdown": base_drawdown, "fixed": summary(fixed), "yearly_base": yearly(np.ones(len(trades), bool)), "yearly_walk": yearly(keep),
            "yearly_fixed": yearly(fixed), "wide_rows": wide_rows, "wide_n": len(wide), "sweep": sweep, "band": band,
            "bootstrap_quantiles": np.quantile(improvements, [0.05, 0.25, 0.5, 0.75, 0.95]),
            "bootstrap_probability": float((improvements <= 0).mean() * 100.0), "bootstrap_total_cut": float(np.nanmean(ratios < 0.8) * 100.0),
            "seed": seed, "draws": draws, "floor": floor, "conditions": conditions, "wide_waits": sorted(int(value) for value in wide["wait"].unique())}


def validate_ledger(data: dict) -> Ledger:
    ledger = Ledger("halt_validate")
    path = relative(VALIDATE_TEXT)
    found, line = text_match(VALIDATE_TEXT, r"120일 거름 총 (\S+)만·낙폭 (\S+)만 n(\d+)")

    if found:
        for key, position in (("total", 1), ("drawdown", 2), ("n", 3)):
            ledger.number(f"120일 고정 거름 · {key}", data["fixed"][key], found.group(position), f"{path}:{line}")

    v1_start = text_line_index(VALIDATE_TEXT, r"^V1 걸어가며 고르기 —")
    v2_start = text_line_index(VALIDATE_TEXT, r"^V2 표본 넓히기 —")

    for choice in data["choices"]:
        if "objective" not in choice:
            continue

        found, line = text_match(VALIDATE_TEXT, rf"^\s+{choice['year']}\s+(\d+)\s+(.+?)\s+(-?[\d.]+) \(\s*(-?[\d.]+)\)\s+.*?(\d+) →\s+(\d+)$", v1_start, v2_start)

        if not found:
            ledger.missing(f"V1 {choice['year']}", path)
            continue

        source = f"{path}:{line}"
        ledger.number(f"V1 {choice['year']} 학습 n", choice["training_n"], found.group(1), source)
        ledger.text(f"V1 {choice['year']} 고른 조건", choice["label"], found.group(2).strip(), source)
        ledger.number(f"V1 {choice['year']} 목표", choice["objective"], found.group(3), source)
        ledger.number(f"V1 {choice['year']} 거름 없음 목표", choice["none_objective"], found.group(4), source)
        ledger.number(f"V1 {choice['year']} 그해 매수 n", choice["year_n"], found.group(5), source)
        ledger.number(f"V1 {choice['year']} 남김", choice["year_kept"], found.group(6), source)

    for position, year in enumerate(data["years"]):
        found, line = text_match(VALIDATE_TEXT, rf"^\s{{4}}{year}\s+(\S+)\s+(\S+)\s+(\S+)$", v1_start, v2_start)

        if found:
            for name, values, group in (("거름 없음", data["yearly_base"], 1), ("걸어가며", data["yearly_walk"], 2), ("120일 고정", data["yearly_fixed"], 3)):
                ledger.number(f"V1 연도 {year} {name} %", values[position], found.group(group), f"{path}:{line}")

    found, line = text_match(VALIDATE_TEXT, r"이어붙인 결과: n(\d+) 총 (\S+)만 낙폭 (\S+)만 .*걸러낸 n(\d+) 평균 (\S+)% vs 남긴 (\S+)%")

    if found:
        walk = data["walk"]
        source = f"{path}:{line}"

        for key, group in (("n", 1), ("total", 2), ("drawdown", 3), ("dropped_n", 4), ("dropped_mean", 5), ("kept_mean", 6)):
            ledger.number(f"V1 이어붙임 · {key}", walk[key], found.group(group), source)
    else:
        ledger.missing("V1 이어붙인 결과", path)

    for row in data["wide_rows"]:
        found, line = text_match(VALIDATE_TEXT, rf"^\s+{row['days']}일\s+(\d+)\s+(\S+)%\s+(\d+)\s+(\S+)%\s+(\S+)%p\s+(\S+) \(\s*(\S+)\)")

        if not found:
            ledger.missing(f"V2 {row['days']}일", path)
            continue

        source = f"{path}:{line}"

        for key, group in (("below_n", 1), ("below_mean", 2), ("above_n", 3), ("above_mean", 4), ("difference", 5), ("t", 6), ("plain_t", 7)):
            ledger.number(f"V2 {row['days']}일 · {key}", row[key], found.group(group), source)

    v3_start = text_line_index(VALIDATE_TEXT, r"^V3 ")
    sweep_frame = pd.read_parquet(VALIDATE_SWEEP)

    for row in data["sweep"]:
        found, line = text_match(VALIDATE_TEXT, rf"^\s+{row['days']}\s+(\d+)\s+(\S+)만\s+(\S+)만\s+(\S+)만\s+(\d+)\s+(\S+)%\s+(\S+)%", v3_start)

        if not found:
            ledger.missing(f"V3 {row['days']}일", path)
            continue

        source = f"{path}:{line}"

        for key, group in (("n", 1), ("total", 2), ("drawdown", 3), ("improvement", 4), ("dropped_n", 5), ("dropped_mean", 6), ("kept_mean", 7)):
            ledger.number(f"V3 {row['days']}일 · {key}", row[key], found.group(group), source)

        stored = sweep_frame[sweep_frame["days"] == row["days"]]

        if len(stored):
            ledger.number(f"V3 {row['days']}일 · 개선(parquet 전 자릿수)", row["improvement"], f"{float(stored['improvement'].iloc[0]):.6f}",
                          relative(VALIDATE_SWEEP), "재계산")

    found, line = text_match(VALIDATE_TEXT, r"개선 분위 5%·25%·50%·75%·95%: (\S+)만 (\S+)만 (\S+)만 (\S+)만 (\S+)만")

    if found:
        for position, label in enumerate(("5%", "25%", "50%", "75%", "95%")):
            ledger.number(f"V4 개선 분위 {label}", data["bootstrap_quantiles"][position], found.group(position + 1), f"{path}:{line}")

    found, line = text_match(VALIDATE_TEXT, r"개선 ≤ 0 확률 (\S+)%, 총 손익 20% 넘게 감소 확률 (\S+)%")

    if found:
        ledger.number("V4 개선 ≤ 0 확률 %", data["bootstrap_probability"], found.group(1), f"{path}:{line}")
        ledger.number("V4 총 손익 20%+ 감소 확률 %", data["bootstrap_total_cut"], found.group(2), f"{path}:{line}")

    return ledger


def validate_section(data: dict, ledger: Ledger) -> str:
    path = VALIDATE_SCRIPT
    seed, seed_source = constant_source(path, "SEED")
    floor, floor_source = constant_source(path, "DRAWDOWN_FLOOR")
    warmup, warmup_source = constant_source(path, "MINIMUM_TRAINING_YEARS")
    wide_text_line = line_of(path, r"대기 3–5")
    wide_code_lines = read_lines(path)[line_of(path, r"def wide_events")[0] - 1:line_of(path, r"def generic_daily_returns")[0]]
    wide_code_has_wait = any('"wait"' in line and "==" in line for line in wide_code_lines)
    conditions = [
        ["언제 사나", "1·2번과 같은 263건(① ③ ④). ②의 넓은 표본은 다음 날 시가 매수·되돌림 25% 미만·상승폭 20% 이상·같은 종목은 매수일 하나",
         where(path, r"def wide_events"),
         differs(f"설명은 '대기 3–5일'인데 코드에는 그 거름이 없다(자료의 대기 값이 {data['wide_waits']}뿐이라 결과는 같다)")
         if wide_text_line[0] and not wide_code_has_wait else same()],
        ["언제 파나", "지켜본 최저 저가 −3% 손절, +15% 익절, 최대 120거래일", where(path, r"^EXIT_RULES"), same()],
        ["비용", "수수료·세금을 뺀 수익", where(path, r"ret_low3_t15"), same()],
        ["거래 가능성", "급등 사건 자료의 거름 그대로", where(path, r"EVENTS_PATH"), same()],
        ["기간", f"매수일 {data['trades']['entry_date'].min()}–{data['trades']['entry_date'].max()}, 미리 골라 쓰기는 {data['years'][0] + warmup}년부터",
         warmup_source, same()],
        ["매매 수", f"263건 / 넓은 표본 {data['wide_n']:,}건", relative(VALIDATE_WIDE), same()],
        ["판단 시점", "매수일 직전 지수 종가", where(path, r"def signal_dates"), same()],
        ["해마다 고르는 기준", f"총 손익 ÷ 최대 낙폭(낙폭이 {floor:,.0f}만 원보다 작으면 {floor:,.0f}만 원으로 봄). 그해 1월 1일 전에 끝난 매매만 씀",
         floor_source, same()],
        ["연도 재표집", f"연도를 통째로 뽑아 다시 묶기 {data['draws']:,}번(seed {seed})", seed_source, same()],
    ]
    walk = data["walk"]
    fixed = data["fixed"]
    base_total, base_drawdown = data["base_total"], data["base_drawdown"]
    base_mean = float(data["trades"]["ret"].mean())
    band = data["band"]
    row_120 = next(row for row in data["wide_rows"] if row["days"] == 120)
    v2_strong = sum(1 for row in data["wide_rows"] if row["t"] <= -3)
    compare_rows = [
        [named_cell("그대로(멈추지 않음)"), count_cell(len(data["trades"])), number_cell(base_mean), money_cell(base_total), money_cell(base_drawdown), blank_cell()],
        [named_cell("코스닥 120일선 아래면 안 삼(결과를 보고 고른 값)"), count_cell(fixed["n"]), number_cell(fixed["kept_mean"]), money_cell(fixed["total"]),
         money_cell(fixed["drawdown"]), number_cell(fixed["dropped_mean"])],
        [named_cell("해마다 그 전 자료로 골라 쓰기"), count_cell(walk["n"]), number_cell(walk["kept_mean"]), money_cell(walk["total"]),
         money_cell(walk["drawdown"]), number_cell(walk["dropped_mean"])],
    ]
    compare = render_table(["방식", "매매 수", "매매당 평균 %", "총 손익(만 원)", "최대 낙폭(만 원)", "뺀 매매 평균 %"], compare_rows, classes="compare")
    criteria = [
        ["① 해마다 골라 써도 최대 낙폭이 30% 이상 준다", f"{money(0.7 * base_drawdown)}보다 얕게",
         f"{money(walk['drawdown'])} ({show((1 - walk['drawdown'] / base_drawdown) * 100, 0, sign=False)}% 감소)", verdict_cell(walk["drawdown"] >= 0.7 * base_drawdown)],
        ["① 해마다 골라 써도 총 손익이 80% 이상 남는다", f"{money(0.8 * base_total)} 이상", money(walk["total"]), verdict_cell(walk["total"] >= 0.8 * base_total)],
        ["② 넓은 표본에서 '이평선 아래'가 확실히 나쁘다", f"길이 {len(data['wide_rows'])}개 중 하나라도 t ≤ −3",
         f"{v2_strong}/{len(data['wide_rows'])}개, 120일선 t {show(row_120['t'])}", verdict_cell(v2_strong > 0)],
        ["③ 이평선 길이를 60–200일로 바꿔도 낙폭이 준다", f"{len(band)}개 길이 모두 개선", f"{sum(row['improvement'] > 0 for row in band)}/{len(band)}개",
         verdict_cell(all(row["improvement"] > 0 for row in band))],
        ["④ 연도를 섞어 다시 뽑아도 낙폭이 준다", "개선이 없을 확률 5% 미만", f"{show(data['bootstrap_probability'], 1, sign=False)}%",
         verdict_cell(data["bootstrap_probability"] < 5)],
    ]
    choice_rows = [[choice["year"], count_cell(choice["training_n"]), choice["label"], number_cell(choice.get("objective", float("nan"))),
                    number_cell(choice.get("none_objective", float("nan"))), count_cell(choice["year_n"]), count_cell(choice["year_kept"])]
                   for choice in data["choices"]]
    wide_rows = [[f"{row['days']}일선", count_cell(row["below_n"]), number_cell(row["below_mean"]), count_cell(row["above_n"]), number_cell(row["above_mean"]),
                  number_cell(row["difference"]), number_cell(row["t"]), number_cell(row["plain_t"])] for row in data["wide_rows"]]
    sweep_rows = [[f"{row['days']}일선", count_cell(row["n"]), money_cell(row["total"]), money_cell(row["drawdown"]), money_cell(row["improvement"]),
                   count_cell(row["dropped_n"]), number_cell(row["dropped_mean"]), number_cell(row["kept_mean"])] for row in data["sweep"]]
    quantiles = data["bootstrap_quantiles"]
    details = [
        ("후보 조건",
         f"<p>거름 없음 + 코스닥·코스피 20·40·60·90·120·150·200일선 아래 + 250일 고점보다 10·15·20% 이상 낮음, 모두 {len(data['conditions'])}개. "
         f"② 넓은 표본은 코스닥 길이 {len(data['wide_rows'])}개, ③은 코스닥 10–250일 25개 길이.</p>"),
        ("① 해마다 고른 조건",
         render_table(["해", "고를 때 쓴 매매", "고른 조건", "고른 조건 점수", "거름 없음 점수", "그해 매수", "남긴 매수"], choice_rows)),
        (f"② 넓은 표본 {data['wide_n']:,}건 — 코스닥 이평선 아래와 위의 차이",
         render_table(["이평선", "아래 매매", "아래 평균 %", "위 매매", "위 평균 %", "차이 %p", "t(같은 달 묶음)", "t(단순)"], wide_rows)),
        ("③ 이평선 길이 훑기(263건)",
         render_table(["이평선", "남은 매매", "총 손익(만 원)", "최대 낙폭(만 원)", "낙폭 개선(만 원)", "뺀 매매", "뺀 매매 평균 %", "남긴 매매 평균 %"],
                      sweep_rows, sortable=True)),
        (f"④ 연도 재표집 {data['draws']:,}번 — 120일선 거름의 낙폭 개선",
         render_table(["", "하위 5%", "25%", "가운데", "75%", "상위 5%", "개선 없을 확률"],
                      [["낙폭 개선(만 원)"] + [money_cell(value) for value in quantiles]
                       + [number_cell(data["bootstrap_probability"], 1, sign=False, suffix="%", colored=False)]])),
    ]
    yearly = bar_chart([str(year)[2:] for year in data["years"]],
                       [("그대로", data["yearly_base"]), ("해마다 골라 쓰기", data["yearly_walk"]), ("120일선 고정", data["yearly_fixed"])],
                       "% (자본 2,000만 원 대비)", "매수 연도별 손익")
    passed_count = sum(1 for row in criteria if row[3]["sort"] == "1")
    answer_text = (f"아니오. 결과를 보고 고른 120일선은 좋아 보이지만, 해마다 그 전 자료로 골라 쓰면 총 손익이 "
                   f"{show((1 - walk['total'] / base_total) * 100, 0, sign=False)}% 줄고(→ {money(walk['total'])}), 넓은 표본에서도 "
                   f"이평선 아래가 더 나쁘다는 근거가 없다(t {show(row_120['t'])}). 기준 {len(criteria)}개 중 {passed_count}개 통과.")
    return section("validate", "3. 그 코스닥 이평선 규칙을 더 엄격하게 검증하면?", ("아니오", "no"), answer_text, compare, criteria, yearly, details,
                   conditions, path, ["py -X utf8 research/studies/35_surge_box_breakout/halt_validate.py"],
                   [VALIDATE_TEXT, VALIDATE_SWEEP, SURGE / "halt_validate_walk.parquet", VALIDATE_WIDE], [VALIDATE_TEXT], ledger)


# ---------------------------------------------------------------- 4 overnight_us


def tertile_cuts(values: pd.Series) -> tuple[float, float]:
    clean = values.dropna()
    return float(clean.quantile(1 / 3)), float(clean.quantile(2 / 3))


def bins_of(values, kind: str, cuts) -> np.ndarray:
    values = np.asarray(values, float)
    result = np.full(len(values), -1)
    finite = np.isfinite(values)

    if kind == "binary":
        result[finite] = values[finite].astype(int)
        return result

    low, high = cuts
    result[finite & (values <= low)] = 0
    result[finite & (values > low) & (values <= high)] = 1
    result[finite & (values > high)] = 2
    return result


def overnight_compare(values, bins: np.ndarray, kind: str, months) -> dict:
    values = np.asarray(values, float)
    valid = np.isfinite(values) & (bins >= 0)
    means = {}

    for label in ((0, 1, 2) if kind == "tertile" else (1, 0)):
        part = values[valid & (bins == label)]
        means[label] = (float(part.mean()) if len(part) else float("nan"), int(len(part)))

    chosen = valid & (bins != 1) if kind == "tertile" else valid
    flag = (bins[chosen] == 0).astype(float) if kind == "tertile" else (bins[chosen] == 1).astype(float)
    difference, t_value = cluster_regression(values[chosen], flag, np.asarray(months)[chosen])
    return {"means": means, "difference": difference, "t": t_value}


def overnight_compute() -> dict:
    module_tree = ast.parse(OVERNIGHT_SCRIPT.read_bytes().decode("utf-8"))
    indicators, _ = constant_of(OVERNIGHT_SCRIPT, "INDICATORS")
    seed, _ = constant_of(OVERNIGHT_SCRIPT, "SEED")
    draws, _ = constant_of(OVERNIGHT_SCRIPT, "BOOTSTRAP_DRAWS")
    floor, _ = constant_of(OVERNIGHT_SCRIPT, "DRAWDOWN_FLOOR")
    warmup, _ = constant_of(OVERNIGHT_SCRIPT, "MINIMUM_TRAINING_YEARS")
    reduced, _ = constant_of(OVERNIGHT_SCRIPT, "REDUCED_WEIGHT")
    limit, _ = constant_of(OVERNIGHT_SCRIPT, "T_LIMIT")
    del module_tree
    features = pd.read_parquet(OVERNIGHT_FEATURES)
    trades = pd.read_parquet(OVERNIGHT_TRADES)
    trades["month"] = trades["entry_date"] // 100
    wide = pd.read_parquet(VALIDATE_WIDE, columns=["entry_date", "month", "ret_low3_t15"])
    wide_features = features.reindex(wide["entry_date"].to_numpy())
    o1 = []

    for column, label, kind in indicators:
        cuts = tertile_cuts(features[column]) if kind == "tertile" else None
        wide_bins = bins_of(wide_features[column].to_numpy(), kind, cuts)
        small_bins = bins_of(trades[column].to_numpy(), kind, cuts)
        wide_result = overnight_compare(wide["ret_low3_t15"].to_numpy(), wide_bins, kind, wide["month"].to_numpy())
        small_result = overnight_compare(trades["ret"].to_numpy(), small_bins, kind, trades["month"].to_numpy())
        same_direction = np.sign(wide_result["difference"]) == np.sign(small_result["difference"])
        o1.append({"column": column, "label": label, "kind": kind, "cuts": cuts, "wide": wide_result, "small": small_result,
                   "passed": bool(abs(wide_result["t"]) >= limit and same_direction), "same_direction": bool(same_direction)})

    # O2 걸어가며 고르기
    items = [{"name": "거름 없음", "column": None}]

    for column, label, kind in indicators:
        for weight in (0.0, reduced):
            action = "안 삼" if weight == 0.0 else "절반"

            if kind == "binary":
                items.append({"name": f"{label} {action}", "column": column, "side": 1, "weight": weight, "kind": kind})
            else:
                for side, side_label in ((0, "아래 3분위"), (2, "위 3분위")):
                    items.append({"name": f"{label} {side_label} {action}", "column": column, "side": side, "weight": weight, "kind": kind})

    profits = trades["profit"].to_numpy(float)
    years = sorted(int(year) for year in trades["entry_year"].unique())
    weights = np.ones(len(trades))
    choices = []

    for year in years:
        in_year = (trades["entry_year"] == year).to_numpy()

        if year < years[0] + warmup:
            choices.append({"year": year, "name": "거름 없음(자료 쌓는 중)", "year_n": int(in_year.sum())})
            continue

        past = features[features.index < year * 10000 + 101]
        cuts = {column: tertile_cuts(past[column]) for column, _, kind in indicators if kind == "tertile"}
        training = (trades["exit_date"] < year * 10000 + 101).to_numpy()
        scored = []

        for order, item in enumerate(items):
            if item["column"] is None:
                item_weights = np.ones(len(trades))
            else:
                bins = bins_of(trades[item["column"]].to_numpy(), item["kind"], cuts.get(item["column"]))
                item_weights = np.where(bins == item["side"], item["weight"], 1.0)

            weighted = profits[training] * item_weights[training]
            scored.append((float(weighted.sum()) / max(abs(max_drawdown(weighted)), floor), -order, item["name"], item_weights))

        scored.sort(key=lambda entry: (entry[0], entry[1]), reverse=True)
        best = scored[0]
        weights[in_year] = best[3][in_year]
        choices.append({"year": year, "name": best[2], "objective": best[0], "training_n": int(training.sum()),
                        "none_objective": next(entry[0] for entry in scored if entry[2] == "거름 없음"), "year_n": int(in_year.sum()),
                        "year_weight": float(best[3][in_year].sum())})

    weighted_profits = profits * weights
    base_total, base_drawdown = float(profits.sum()), max_drawdown(profits)
    walk_total, walk_drawdown = float(weighted_profits.sum()), max_drawdown(weighted_profits)
    less = base_total * walk_drawdown / base_drawdown

    # O3 연도 재표집
    generator = np.random.default_rng(seed)
    blocks = {year: (trades["entry_year"] == year).to_numpy() for year in years}
    improvements, drawdown_improvements = [], []

    for _ in range(draws):
        drawn = generator.choice(years, size=len(years), replace=True)
        base_profits = np.concatenate([profits[blocks[year]] for year in drawn])
        filtered = np.concatenate([profits[blocks[year]] * weights[blocks[year]] for year in drawn])
        drawn_base_drawdown = max_drawdown(base_profits)
        drawn_filtered_drawdown = max_drawdown(filtered)
        drawn_less = base_profits.sum() if drawn_base_drawdown == 0 else base_profits.sum() * drawn_filtered_drawdown / drawn_base_drawdown
        improvements.append(filtered.sum() - drawn_less)
        drawdown_improvements.append(drawn_filtered_drawdown - drawn_base_drawdown)

    improvements = np.array(improvements)
    yearly_base = list(pd.Series(profits).groupby(trades["entry_year"].to_numpy()).sum().reindex(years, fill_value=0.0) / 2000.0 * 100.0)
    yearly_walk = list(pd.Series(weighted_profits).groupby(trades["entry_year"].to_numpy()).sum().reindex(years, fill_value=0.0) / 2000.0 * 100.0)
    return {"o1": o1, "choices": choices, "weights": weights, "stored_weights": trades["walk_weight"].to_numpy(float), "base_total": base_total,
            "base_drawdown": base_drawdown, "walk_total": walk_total, "walk_drawdown": walk_drawdown, "less": less, "items": items,
            "quantiles": np.quantile(improvements, [0.05, 0.25, 0.5, 0.75, 0.95]), "probability": float((improvements <= 0).mean() * 100.0),
            "drawdown_probability": float((np.array(drawdown_improvements) <= 0).mean() * 100.0), "years": years, "yearly_base": yearly_base,
            "yearly_walk": yearly_walk, "seed": seed, "draws": draws, "limit": limit, "indicators": indicators, "wide_n": len(wide), "trades": trades}


def overnight_ledger(data: dict) -> Ledger:
    ledger = Ledger("overnight_us")
    path = relative(OVERNIGHT_TEXT)
    o1_start = text_line_index(OVERNIGHT_TEXT, r"^O1 ")

    for row in data["o1"]:
        header_index = text_line_index(OVERNIGHT_TEXT, r"^\s+\[" + re.escape(row["label"]) + r"\]", o1_start)

        if header_index < 0:
            ledger.missing(f"O1 {row['label']}", path)
            continue

        header = read_lines(OVERNIGHT_TEXT)[header_index]

        if row["kind"] == "tertile":
            cut_found = re.search(r"경계 (\S+) / (\S+)", header)

            if cut_found:
                ledger.number(f"O1 {row['label']} 경계 아래", row["cuts"][0], cut_found.group(1), f"{path}:{header_index + 1}")
                ledger.number(f"O1 {row['label']} 경계 위", row["cuts"][1], cut_found.group(2), f"{path}:{header_index + 1}")

        for sample, prefix in (("wide", r"넓은\s+원:"), ("small", r"263 원:")):
            found, line = text_match(OVERNIGHT_TEXT, prefix + r"(.*)\| 차이\s+(\S+)%p 묶음 t\s+(\S+)", header_index, header_index + 6)

            if not found:
                ledger.missing(f"O1 {row['label']} {sample}", path)
                continue

            source = f"{path}:{line}"
            result = row[sample]
            ledger.number(f"O1 {row['label']} {sample} 차이", result["difference"], found.group(2), source)
            ledger.number(f"O1 {row['label']} {sample} 묶음 t", result["t"], found.group(3), source)
            names = {"아래": 0 if row["kind"] == "tertile" else 1, "가운데": 1, "위": 2 if row["kind"] == "tertile" else 0}

            for name, mean_text, count_text in re.findall(r"(아래|가운데|위)\s+(\S+)%\(n(\d+)\)", found.group(1)):
                mean, count = result["means"][names[name]]
                ledger.number(f"O1 {row['label']} {sample} {name} 평균", mean, mean_text, source)
                ledger.number(f"O1 {row['label']} {sample} {name} n", count, count_text, source)

        found, line = text_match(OVERNIGHT_TEXT, r"판정: (통과|실패)", header_index, header_index + 7)

        if found:
            ledger.text(f"O1 {row['label']} 판정", "통과" if row["passed"] else "실패", found.group(1), f"{path}:{line}")

    o2_start = text_line_index(OVERNIGHT_TEXT, r"^O2 걸어가며 고르기 —")
    o3_start = text_line_index(OVERNIGHT_TEXT, r"^O3 연도 재표집 \d+번")

    for choice in data["choices"]:
        if "objective" not in choice:
            continue

        found, line = text_match(OVERNIGHT_TEXT, rf"^\s+{choice['year']}\s+(\d+)\s+(.+?)\s+(-?[\d.]+) \(\s*(-?[\d.]+)\)\s+.+?\s(\d+) →\s+([\d.]+)$", o2_start, o3_start)

        if not found:
            ledger.missing(f"O2 {choice['year']}", path)
            continue

        source = f"{path}:{line}"
        ledger.number(f"O2 {choice['year']} 학습 n", choice["training_n"], found.group(1), source)
        ledger.text(f"O2 {choice['year']} 고른 조건", choice["name"], found.group(2).strip(), source)
        ledger.number(f"O2 {choice['year']} 목표", choice["objective"], found.group(3), source)
        ledger.number(f"O2 {choice['year']} 거름 없음 목표", choice["none_objective"], found.group(4), source)
        ledger.number(f"O2 {choice['year']} 실린 비중", choice["year_weight"], found.group(6), source)

    found, line = text_match(OVERNIGHT_TEXT, r"이어붙인 결과: 총 (\S+)만 낙폭 (\S+)만 \| 거름 없음 총 (\S+)만 낙폭 (\S+)만")

    if found:
        source = f"{path}:{line}"
        ledger.number("O2 이어붙임 총 손익", data["walk_total"], found.group(1), source)
        ledger.number("O2 이어붙임 낙폭", data["walk_drawdown"], found.group(2), source)
        ledger.number("거름 없음 총 손익", data["base_total"], found.group(3), source)
        ledger.number("거름 없음 낙폭", data["base_drawdown"], found.group(4), source)

    found, line = text_match(OVERNIGHT_TEXT, r"총 (\S+)만 → 거름 − 덜 사기 = (\S+)만")

    if found:
        ledger.number("O4 덜 사기 총 손익", data["less"], found.group(1), f"{path}:{line}")
        ledger.number("O2 거름 − 덜 사기", data["walk_total"] - data["less"], found.group(2), f"{path}:{line}")

    ledger.number("O2 해마다 비중 = parquet walk_weight(최대 차이)", float(np.abs(data["weights"] - data["stored_weights"]).max()), "0.000000",
                  relative(OVERNIGHT_TRADES))
    found, line = text_match(OVERNIGHT_TEXT, r"개선 분위 5·25·50·75·95%: (\S+)만 (\S+)만 (\S+)만 (\S+)만 (\S+)만")

    if found:
        for position, label in enumerate(("5%", "25%", "50%", "75%", "95%")):
            ledger.number(f"O3 개선 분위 {label}", data["quantiles"][position], found.group(position + 1), f"{path}:{line}")

    found, line = text_match(OVERNIGHT_TEXT, r"개선 ≤ 0 확률 (\S+)% \(참고: 낙폭 개선 ≤ 0 확률 (\S+)%\)")

    if found:
        ledger.number("O3 개선 ≤ 0 확률 %", data["probability"], found.group(1), f"{path}:{line}")
        ledger.number("O3 낙폭 개선 ≤ 0 확률 %", data["drawdown_probability"], found.group(2), f"{path}:{line}")

    return ledger


def overnight_section(data: dict, ledger: Ledger) -> str:
    path = OVERNIGHT_SCRIPT
    plan_line = line_of(path, r"0\.05/7")
    decision_hour, decision_source = constant_source(path, "DECISION_HOUR")
    conditions = [
        ["언제 사나", "1·2번과 같은 263건 + 3번의 넓은 표본(시가 매수·되돌림 25% 미만·상승폭 20% 이상)", where(path, r"^대상"), same()],
        ["언제 파나", "지켜본 최저 저가 −3% 손절, +15% 익절, 최대 120거래일", where(path, r'"ret_low3_t15", "hold_low3_t15"'), same()],
        ["비용", "수수료·세금을 뺀 수익", where(path, r"ret_low3_t15"), same()],
        ["거래 가능성", "급등 사건 자료의 거름 그대로", where(path, r"WIDE_PATH ="), same()],
        ["기간", f"매수일 {data['trades']['entry_date'].min()}–{data['trades']['entry_date'].max()}", relative(OVERNIGHT_TRADES), same()],
        ["매매 수", f"263건 / 넓은 표본 {data['wide_n']:,}건", relative(VALIDATE_WIDE), same()],
        ["판단 시점", f"매수일 {decision_hour:02d}:50(한국 시각)에 알 수 있는 직전 미국 정규장, 선물은 재개장 시가", decision_source, same()],
        ["지표", f"{len(data['indicators'])}개: " + ", ".join(label for _, label, _ in data["indicators"]), where(path, r"^INDICATORS"),
         differs(f"계획서는 여러 번 시험한 만큼 문턱을 높이는 계산을 지표 7개 기준으로 적었다. 실제 지표는 {len(data['indicators'])}개이고 "
                 f"문턱 {data['limit']}는 {len(data['indicators'])}개 기준") if plan_line[0] else same()],
        ["통과 문턱", f"넓은 표본에서 |t| ≥ {data['limit']}이고 263건에서도 같은 방향", where(path, r"^T_LIMIT"), same()],
        ["연도 재표집", f"연도를 통째로 뽑아 다시 묶기 {data['draws']:,}번(seed {data['seed']})", where(path, r"^SEED"), same()],
    ]
    o1_rows = []

    for row in data["o1"]:
        kind_label = "위/아래" if row["kind"] == "binary" else "3등분"
        o1_rows.append([row["label"], kind_label, number_cell(row["wide"]["difference"]), number_cell(row["wide"]["t"]),
                        number_cell(row["small"]["difference"]), number_cell(row["small"]["t"]), "같음" if row["same_direction"] else "반대",
                        verdict_cell(row["passed"])])

    choice_rows = [[choice["year"], count_cell(choice.get("training_n", 0)), choice["name"], number_cell(choice.get("objective", float("nan"))),
                    number_cell(choice.get("none_objective", float("nan"))), count_cell(choice["year_n"]),
                    number_cell(choice.get("year_weight", float(choice["year_n"])), 1, sign=False, colored=False)]
                   for choice in data["choices"]]
    passed_count = sum(row["passed"] for row in data["o1"])
    strongest = max(data["o1"], key=lambda row: abs(row["wide"]["t"]) if np.isfinite(row["wide"]["t"]) else -1.0)
    criteria = [
        ["① 미국장 지표 중 하나라도 효과가 확실하다", f"넓은 표본 |t| ≥ {data['limit']}이고 263건에서도 같은 방향",
         f"{passed_count}/{len(data['o1'])}개 (가장 큰 것: {strongest['label']} t {show(strongest['wide']['t'])})", verdict_cell(passed_count > 0)],
        ["② 해마다 골라 쓴 거름이 '금액만 줄이기'보다 많이 번다", f"같은 낙폭에서 {money(data['less'])}보다 많이",
         money(data["walk_total"]), verdict_cell(data["walk_total"] > data["less"])],
        ["③ 연도를 섞어 다시 뽑아도 ②가 유지된다", "더 못할 확률 10% 이하", f"{show(data['probability'], 1, sign=False)}%", verdict_cell(data["probability"] <= 10)],
    ]
    base_mean = float(data["trades"]["ret"].mean())
    compare_rows = [
        [named_cell("그대로(거르지 않음)"), count_cell(len(data["trades"])), number_cell(base_mean), money_cell(data["base_total"]), money_cell(data["base_drawdown"])],
        [named_cell("해마다 그 전 자료로 고른 미국장 거름"), blank_cell(), blank_cell(), money_cell(data["walk_total"]), money_cell(data["walk_drawdown"])],
        [named_cell("거르지 않고 금액만 줄여 같은 낙폭"), blank_cell(), blank_cell(), money_cell(data["less"]), money_cell(data["walk_drawdown"])],
    ]
    compare = render_table(["방식", "매매 수", "매매당 평균 %", "총 손익(만 원)", "최대 낙폭(만 원)"], compare_rows, classes="compare")
    quantiles = data["quantiles"]
    details = [
        ("① 지표별 효과 — 차이 = 아래쪽 3분의 1 − 위쪽 3분의 1(위/아래 지표는 아래 − 위), t는 같은 달 매매를 묶어 계산",
         render_table(["지표", "나눈 방식", "넓은 표본 차이 %p", "넓은 표본 t", "263건 차이 %p", "263건 t", "방향", "판정"], o1_rows, sortable=True)),
        (f"② 해마다 고른 거름(후보 {len(data['items'])}개: 거름 없음 + 지표마다 '안 삼'·'절반만 삼' × 아래·위)",
         render_table(["해", "고를 때 쓴 매매", "고른 거름", "고른 거름 점수", "거름 없음 점수", "그해 매수", "실제 산 비중(건)"], choice_rows)),
        (f"③ 연도 재표집 {data['draws']:,}번 — 거름 손익 − 금액만 줄인 손익",
         render_table(["", "하위 5%", "25%", "가운데", "75%", "상위 5%", "더 못할 확률"],
                      [["차이(만 원)"] + [money_cell(value) for value in quantiles]
                       + [number_cell(data["probability"], 1, sign=False, suffix="%", colored=False)]])),
    ]
    yearly = bar_chart([str(year)[2:] for year in data["years"]], [("그대로", data["yearly_base"]), ("해마다 고른 거름", data["yearly_walk"])],
                       "% (자본 2,000만 원 대비)", "매수 연도별 손익")
    answer_text = (f"아니오. 지표 {len(data['o1'])}개 모두 문턱({data['limit']})을 넘지 못했고(가장 큰 {strongest['label']} t {show(strongest['wide']['t'])}), "
                   f"해마다 고른 거름({money(data['walk_total'])})은 같은 낙폭이 되도록 금액만 줄인 경우({money(data['less'])})보다 적게 번다.")
    return section("overnight", "4. 전날 밤 미국장(VIX·나스닥)으로 멈추면?", ("아니오", "no"), answer_text, compare, criteria, yearly, details, conditions,
                   path, ["py -X utf8 research/studies/35_surge_box_breakout/overnight_us.py"],
                   [OVERNIGHT_TEXT, OVERNIGHT_TRADES, OVERNIGHT_FEATURES], [OVERNIGHT_TEXT], ledger)


# ---------------------------------------------------------------- 5 standalone_swing


def standalone_compute() -> dict:
    columns = ["cell", "entry_date", "exit_date", "net", "benchmark", "excess", "month", "year", "reason", "hold_days", "mae", "mfe"]
    trades = pd.read_parquet(STANDALONE_TRADES, columns=columns)
    split_date, _ = constant_of(STANDALONE_SCRIPT, "SPLIT_DATE")
    windows, _ = constant_of(STANDALONE_SCRIPT, "WINDOWS")
    spans, _ = constant_of(STANDALONE_SCRIPT, "K_VALUES")
    pass_t, _ = constant_of(STANDALONE_SCRIPT, "PASS_T")
    warmup, _ = constant_of(STANDALONE_SCRIPT, "WALK_WARMUP_YEARS")
    test_cells = [f"DB_k{span}" for span in spans] + [f"UP_k{span}_N{window}" for span in spans for window in windows]
    control_cells = [f"DOWN_k{span}_N{window}" for span in spans for window in windows]
    groups = dict(tuple(trades.groupby("cell", sort=False)))
    cells = {}

    for cell in test_cells + control_cells:
        part = groups[cell]
        early = part["entry_date"] < split_date
        winners = part[part["net"] > 0]
        summary = {"n": len(part), "net": float(part["net"].mean()), "benchmark": float(part["benchmark"].mean()), "excess": float(part["excess"].mean()),
                   "t": cluster_mean_t(part["excess"].to_numpy(), part["month"].to_numpy()),
                   "excess_2010_17": float(part.loc[early, "excess"].mean()), "excess_2018_26": float(part.loc[~early, "excess"].mean()),
                   "win_rate": float((part["net"] > 0).mean() * 100.0), "hold_days": float(part["hold_days"].mean()),
                   "stop_share": float((part["reason"] == "stop").mean() * 100.0), "take_share": float((part["reason"] == "take").mean() * 100.0),
                   "winner_mae_median": float(winners["mae"].median()), "winner_mae_p10": float(winners["mae"].quantile(0.1)),
                   "mfe_median": float(part["mfe"].median()), "mfe_p90": float(part["mfe"].quantile(0.9)),
                   "yearly": part.groupby("year")["excess"].mean().to_dict()}
        summary["pass"] = bool(summary["excess"] > 0 and summary["t"] >= pass_t and summary["excess_2010_17"] > 0 and summary["excess_2018_26"] > 0)
        cells[cell] = summary

    years = sorted(int(year) for year in trades["year"].unique())
    walk_parts, choices = [], []
    test_mask = trades["cell"].isin(test_cells)

    for year in range(years[0] + warmup, years[-1] + 1):
        known = trades[(trades["exit_date"] < year * 10000 + 101) & test_mask]
        scores = {cell: cluster_mean_t(group["excess"].to_numpy(), group["month"].to_numpy()) for cell, group in known.groupby("cell")}
        scores = {cell: value for cell, value in scores.items() if np.isfinite(value)}

        if not scores:
            continue

        chosen = max(sorted(scores), key=lambda cell: scores[cell])
        part = groups[chosen][groups[chosen]["year"] == year]
        walk_parts.append(part)
        # 원 스크립트가 소수 셋째 자리로 반올림한 뒤 둘째 자리로 찍으므로 같은 순서로 맞춘다(standalone_swing.py:336).
        choices.append({"year": year, "cell": chosen, "past_t": scores[chosen], "n": len(part), "excess": round(float(part["excess"].mean()), 3)})

    walk = pd.concat(walk_parts, ignore_index=True)
    differences = {}

    for span in spans:
        for window in windows:
            up = groups[f"UP_k{span}_N{window}"]
            down = groups[f"DOWN_k{span}_N{window}"]
            joined = pd.concat([up, down], ignore_index=True)
            flag = np.r_[np.ones(len(up)), np.zeros(len(down))]
            differences[f"k{span}_N{window}"] = cluster_regression(joined["excess"].to_numpy(), flag, joined["month"].to_numpy())

    return {"cells": cells, "test_cells": test_cells, "control_cells": control_cells, "choices": choices, "walk_n": len(walk),
            "walk_excess": float(walk["excess"].mean()), "walk_t": cluster_mean_t(walk["excess"].to_numpy(), walk["month"].to_numpy()),
            "differences": differences, "years": years, "pass_t": pass_t, "total_rows": len(trades)}


STANDALONE_KEYS = ("n", "net", "benchmark", "excess", "t", "excess_2010_17", "excess_2018_26", "win_rate", "hold_days", "stop_share", "take_share")


def standalone_ledger(data: dict) -> Ledger:
    ledger = Ledger("standalone_swing")
    path = relative(STANDALONE_TEXT)
    metrics = json.loads(STANDALONE_METRICS.read_bytes().decode("utf-8"))

    for cell in data["test_cells"] + data["control_cells"]:
        found, line = text_match(STANDALONE_TEXT, rf"^\s+{cell}\s+" + r"\s+".join([r"(\S+)"] * 11) + r"(?:\s+(통과|기각))?\s*$")

        if not found:
            ledger.missing(cell, path)
            continue

        source = f"{path}:{line}"
        computed = data["cells"][cell]

        for position, key in enumerate(STANDALONE_KEYS):
            ledger.number(f"{cell} · {key}", computed[key], found.group(position + 1), source)

        if found.group(12):
            ledger.text(f"{cell} · 판정", "통과" if computed["pass"] else "기각", found.group(12), source)

        stored = metrics["cells"].get(cell, {})

        if "t" in stored:
            ledger.number(f"{cell} · t(metrics.json 전 자릿수)", computed["t"], f"{stored['t']:.6f}", relative(STANDALONE_METRICS))

        found, line = text_match(STANDALONE_TEXT, rf"^\s+{cell}\s+이긴 MAE\s+(\S+) /\s+(\S+)\s+MFE\s+(\S+) /\s+(\S+)")

        if found:
            for position, key in enumerate(("winner_mae_median", "winner_mae_p10", "mfe_median", "mfe_p90")):
                ledger.number(f"{cell} · {key}", computed[key], found.group(position + 1), f"{path}:{line}")

    for name, (difference, t_value) in data["differences"].items():
        found, line = text_match(STANDALONE_TEXT, rf"^\s+{name}\s+차이\s+(\S+)%p 묶음 t\s+(\S+)")

        if found:
            ledger.number(f"상승 − 하향 {name} 차이", difference, found.group(1), f"{path}:{line}")
            ledger.number(f"상승 − 하향 {name} t", t_value, found.group(2), f"{path}:{line}")
        else:
            ledger.missing(f"차이 {name}", path)

    found, line = text_match(STANDALONE_TEXT, r"해마다 그 전 자료로 고르기\(2013–\): n (\d+) 초과 (\S+)% 묶음 t (\S+)")

    if found:
        ledger.number("걸어가며 n", data["walk_n"], found.group(1), f"{path}:{line}")
        ledger.number("걸어가며 초과", data["walk_excess"], found.group(2), f"{path}:{line}")
        ledger.number("걸어가며 묶음 t", data["walk_t"], found.group(3), f"{path}:{line}")

    for choice in data["choices"]:
        found, line = text_match(STANDALONE_TEXT, rf"^\s+{choice['year']} (\S+)\s+\(지난 t (\S+)\) n\s+(\d+) 초과 (\S+)")

        if not found:
            ledger.missing(f"걸어가며 {choice['year']}", path)
            continue

        source = f"{path}:{line}"
        ledger.text(f"걸어가며 {choice['year']} 고른 칸", choice["cell"], found.group(1), source)
        ledger.number(f"걸어가며 {choice['year']} 지난 t", choice["past_t"], found.group(2), source)
        ledger.number(f"걸어가며 {choice['year']} n", choice["n"], found.group(3), source)
        ledger.number(f"걸어가며 {choice['year']} 초과", choice["excess"], found.group(4), source)

    return ledger


def standalone_label(cell: str) -> str:
    """DB_k3 → 쌍바닥 돌파(저점 확인 3일), UP_k3_N10 → 상승 추세(저점 확인 3일)·10일선 지지."""
    parts = cell.split("_")
    span = parts[1][1:]

    if parts[0] == "DB":
        return f"쌍바닥 돌파(저점 확인 {span}일)"

    trend = "상승 추세" if parts[0] == "UP" else "하향 추세"
    suffix = " — 대조군" if parts[0] == "DOWN" else ""
    return f"{trend}(저점 확인 {span}일)·{parts[2][1:]}일선 지지{suffix}"


def standalone_section(data: dict, ledger: Ledger) -> str:
    path = STANDALONE_SCRIPT
    stop, stop_source = constant_source(path, "STOP_BELOW")
    take, take_source = constant_source(path, "TAKE_PROFIT")
    hold, hold_source = constant_source(path, "HOLD_MAX")
    minimum_close, close_source = constant_source(path, "MIN_CLOSE_KRW")
    minimum_turnover, _ = constant_source(path, "MIN_TURNOVER20_KRW")
    signal_from, signal_source = constant_source(path, "SIGNAL_FROM")
    last_date, _ = constant_source(path, "LAST_DATE")
    split_date, split_source = constant_source(path, "SPLIT_DATE")
    cells = data["cells"]
    conditions = [
        ["언제 사나", "신호 다음 날 시가(시가가 손절선 이하면 안 삼). 쌍바닥은 두 번째 저점 뒤 목선을 넘을 때, 상승 추세는 이평선에 닿고 위에서 끝날 때",
         where(path, r"def simulate_exit"), same()],
        ["언제 파나", f"근거 저점 × {stop} 아래면 손절, 산 값 × {take} 위면 익절, 최대 {hold}거래일 뒤 종가. 같은 날 둘 다 닿으면 손절로 침",
         stop_source, same()],
        ["비용", "수수료·세금 왕복 0.23%(스터디 35와 같은 값)", where(path, r"COST"), same()],
        ["거래 가능성", f"신호일 종가 {minimum_close:,.0f}원 이상, 20일 평균 거래대금 {minimum_turnover / 1e8:,.0f}억 원 이상", close_source, same()],
        ["기간", f"신호 {signal_from}–{last_date}, 앞·뒤 기간 경계 {split_date}", signal_source, same()],
        ["매매 수", f"{data['total_rows']:,}건(14칸 합)", relative(STANDALONE_TRADES), same()],
        ["판단 시점", "저점·고점은 k일 뒤에야 확정되므로 확정된 다음 날부터 씀", where(STUDIES / "37_swing_trend" / "swing_definitions.py", r"def "), same()],
        ["시장 대비", "같은 날 시가에 같은 거름을 통과한 전 종목을 사서 같은 날 종가에 판 평균과 비교", where(path, r"benchmark"), same()],
        ["통과 문턱", f"시장 대비 초과 > 0, t ≥ {data['pass_t']}, 앞·뒤 기간 모두 초과 > 0", where(path, r"^PASS_T"), same()],
    ]
    compare_rows = []

    for cell in ("UP_k3_N10", "DOWN_k3_N10", "UP_k5_N10", "DOWN_k5_N10"):
        summary = cells[cell]
        compare_rows.append([named_cell(standalone_label(cell), cell), count_cell(summary["n"]), number_cell(summary["net"]), number_cell(summary["excess"]),
                             number_cell(summary["t"]), number_cell(summary["win_rate"], 1, sign=False, colored=False)])

    compare_rows.append([named_cell("해마다 그 전 자료로 칸을 골라 쓰기"), count_cell(data["walk_n"]), blank_cell(), number_cell(data["walk_excess"]),
                         number_cell(data["walk_t"]), blank_cell()])
    compare = render_table(["방식", "매매 수", "매매당 순수익 %", "시장 대비 초과 %p", "t", "승률 %"], compare_rows, classes="compare")
    passed = [cell for cell in data["test_cells"] if cells[cell]["pass"]]
    test_count = len(data["test_cells"])
    best_cell = max(data["test_cells"], key=lambda cell: cells[cell]["t"])
    differences = data["differences"]
    all_down_better = all(value[0] < 0 for value in differences.values())
    criteria = [
        ["① 시장보다 더 번다", "초과 > 0", f"{sum(cells[cell]['excess'] > 0 for cell in data['test_cells'])}/{test_count}칸",
         verdict_cell(any(cells[cell]["excess"] > 0 for cell in data["test_cells"]), "충족 칸 있음", "없음")],
        ["② 그 차이가 우연이 아니다", f"t ≥ {data['pass_t']}", f"가장 큰 칸 {standalone_label(best_cell)} t {show(cells[best_cell]['t'])}",
         verdict_cell(cells[best_cell]["t"] >= data["pass_t"], "충족 칸 있음", "없음")],
        ["③ 앞 기간(2010–2017)·뒤 기간(2018–2026) 모두 시장보다 낫다", "둘 다 초과 > 0",
         f"{sum(cells[cell]['excess_2010_17'] > 0 and cells[cell]['excess_2018_26'] > 0 for cell in data['test_cells'])}/{test_count}칸",
         verdict_cell(any(cells[cell]["excess_2010_17"] > 0 and cells[cell]["excess_2018_26"] > 0 for cell in data["test_cells"]), "충족 칸 있음", "없음")],
        ["①–③을 모두 넘은 칸", "1칸 이상", f"{len(passed)}/{test_count}칸" + (f" — {', '.join(standalone_label(cell) for cell in passed)}" if passed else ""),
         verdict_cell(bool(passed), "있음", "없음")],
        ["해마다 그 전 자료로 칸을 골라 써도 시장보다 낫다", "초과 > 0", f"{show(data['walk_excess'])}%p, t {show(data['walk_t'])}", verdict_cell(data["walk_excess"] > 0)],
        ["'상승 추세'가 이유다 — 같은 모양의 하향 추세보다 낫다", "상승 − 하향 > 0",
         f"{sum(value[0] > 0 for value in differences.values())}/{len(differences)}쌍", verdict_cell(any(value[0] > 0 for value in differences.values()), "일부", "없음")],
    ]
    headers = ["칸", "매매 수", "순수익 %", "시장 %", "초과 %p", "t", "2010–17 초과", "2018–26 초과", "승률 %", "보유일", "손절 비중 %", "익절 비중 %", "판정"]
    rows = []

    for cell in data["test_cells"] + data["control_cells"]:
        summary = cells[cell]
        rows.append([named_cell(standalone_label(cell), cell), count_cell(summary["n"]), number_cell(summary["net"]), number_cell(summary["benchmark"]),
                     number_cell(summary["excess"]), number_cell(summary["t"]), number_cell(summary["excess_2010_17"]), number_cell(summary["excess_2018_26"]),
                     number_cell(summary["win_rate"], 1, sign=False, colored=False), number_cell(summary["hold_days"], 1, sign=False, colored=False),
                     number_cell(summary["stop_share"], 1, sign=False, colored=False), number_cell(summary["take_share"], 1, sign=False, colored=False),
                     verdict_cell(summary["pass"]) if cell in data["test_cells"] else note_cell("대조군")])

    mae_rows = [[named_cell(standalone_label(cell), cell), number_cell(cells[cell]["winner_mae_median"]), number_cell(cells[cell]["winner_mae_p10"]),
                 number_cell(cells[cell]["mfe_median"]), number_cell(cells[cell]["mfe_p90"])] for cell in data["test_cells"] + data["control_cells"]]

    def pair_label(name: str) -> str:
        span, window = name.split("_")
        return f"저점 확인 {span[1:]}일·{window[1:]}일선"

    difference_rows = [[pair_label(name), number_cell(difference), number_cell(t_value)] for name, (difference, t_value) in differences.items()]
    choice_rows = [[choice["year"], named_cell(standalone_label(choice["cell"]), choice["cell"]), number_cell(choice["past_t"]), count_cell(choice["n"]),
                    number_cell(choice["excess"])] for choice in data["choices"]]
    details = [
        ("칸 이름 읽는 법", "<p>저점 확인 k일 = 저점 뒤 k일 동안 더 낮은 값이 없어야 저점으로 인정. N일선 지지 = 주가가 N일 이동평균선에 닿고 그 위에서 끝난 날. "
         "시험 칸은 쌍바닥 2칸 + 상승 추세 6칸, 하향 추세 6칸은 비교용 대조군.</p>"),
        ("14칸 전체(매매당 평균, 비용 포함, 열 머리를 누르면 정렬)", render_table(headers, rows, sortable=True)),
        ("상승 추세 − 같은 모양의 하향 추세", render_table(["짝", "초과 차이 %p", "t"], difference_rows)),
        (f"해마다 고른 칸 — 합계 {data['walk_n']:,}건, 초과 {show(data['walk_excess'])}%p, t {show(data['walk_t'])}",
         render_table(["해", "고른 칸", "그 전까지 t", "매매 수", "초과 %p"], choice_rows)),
        ("보유 중 가격 폭 — 이긴 매매가 중간에 얼마나 빠졌나(MAE), 매매 중 최고 수익(MFE), %",
         render_table(["칸", "이긴 매매 MAE 가운데", "이긴 매매 MAE 하위 10%", "MFE 가운데", "MFE 상위 10%"], mae_rows)),
    ]
    years = data["years"]
    yearly_cells = ("UP_k3_N10", "UP_k5_N10", "DOWN_k5_N10")
    yearly = bar_chart([str(year)[2:] for year in years], [(standalone_label(cell), [cells[cell]["yearly"].get(year, float("nan")) for year in years])
                                                          for cell in yearly_cells], "%p (시장 대비 초과)", "해별 시장 대비 초과 평균")
    lowest_t = min(value[1] for value in differences.values())

    if all_down_better:
        answer = ("아니오", "no")
        answer_text = (f"아니오. {standalone_label(best_cell)} 한 칸은 기준을 넘었지만(t {show(cells[best_cell]['t'])}), 같은 모양을 하향 추세에서 산 대조군이 "
                       f"{len(differences)}쌍 모두 더 좋았다(차이 t 최저 {show(lowest_t)}). '상승 추세'가 이유라는 근거가 없어 7번에서는 추세 조건을 뺐다.")
    else:
        answer = ("일부", "partial")
        answer_text = f"일부. 기준을 넘은 칸 {len(passed)}개, 상승 − 하향 차이는 부호가 섞였다."

    return section("standalone", "5. 상승 추세·쌍바닥에서 사는 새 전략은 돈이 되나?", answer, answer_text, compare, criteria, yearly, details, conditions, path,
                   ["py -X utf8 research/studies/37_swing_trend/standalone_swing.py"], [STANDALONE_TEXT, STANDALONE_METRICS, STANDALONE_TRADES],
                   [STANDALONE_TEXT], ledger)


# ---------------------------------------------------------------- 6 devscale_trend_filter


DEVSCALE_RULES = {"C0": (None, None), "C1": (3, "not_down"), "C2": (5, "not_down"), "C3": (3, "up_only"), "C4": (5, "up_only")}


def devscale_compute() -> dict:
    trades = pd.read_parquet(DEVSCALE_TRADES, columns=["entry_date", "exit_date", "net_won_mid", "net_won_low", "trend_k3", "trend_k5",
                                                       "return_percent_mid", "entry_month"])
    entry = pd.to_datetime(trades["entry_date"])
    early = (entry < pd.Timestamp("2018-01-01")).to_numpy()
    entry_year = entry.dt.year.to_numpy()
    returns = trades["return_percent_mid"].to_numpy(float)
    masks = {}

    for name, (span, rule) in DEVSCALE_RULES.items():
        if span is None:
            masks[name] = np.ones(len(trades), bool)
        else:
            label = trades[f"trend_k{span}"].to_numpy()
            masks[name] = label != -1 if rule == "not_down" else label == 1

    cells = {}

    for name, mask in masks.items():
        kept = trades[mask]
        difference, t_value = cluster_regression(returns, ~mask, trades["entry_month"].to_numpy()) if (~mask).any() else (float("nan"), float("nan"))
        periods = {}

        for label, period in (("2010–2017", early), ("2018–2026", ~early)):
            periods[label] = (int((mask & period).sum()), float(returns[mask & period].mean()), int((~mask & period).sum()),
                              float(returns[~mask & period].mean()) if (~mask & period).any() else float("nan"))

        cells[name] = {"n": int(mask.sum()), "mean": float(kept["return_percent_mid"].mean()), "win": float((kept["net_won_mid"] > 0).mean() * 100.0),
                       "total": float(kept["net_won_mid"].sum()) / 1e4, "total_low": float(kept["net_won_low"].sum()) / 1e4,
                       "removed_n": int((~mask).sum()), "removed_mean": float(returns[~mask].mean()) if (~mask).any() else float("nan"),
                       "difference": difference, "t": t_value, "periods": periods,
                       "yearly": pd.Series(kept["net_won_mid"].to_numpy() / 1e4).groupby(entry_year[mask]).sum().to_dict()}

    return {"trades": trades, "masks": masks, "cells": cells, "entry_year": entry_year, "years": sorted(int(year) for year in np.unique(entry_year))}


def devscale_ledger(data: dict) -> Ledger:
    ledger = Ledger("devscale_trend_filter")
    path = relative(DEVSCALE_TEXT)
    start = text_line_index(DEVSCALE_TEXT, r"^\[시험 칸 — 금액")
    stop = text_line_index(DEVSCALE_TEXT, r"^\[참고 1")
    data["reported"] = {}

    for name, cell in data["cells"].items():
        found, line = text_match(DEVSCALE_TEXT, rf"^{name} \| (.+?) \| (\d+) \| (\S+) \| (\S+) \| (\S+)만 \| (\S+)만 \| (\S+)만 \| (\S+)만 \| (\S+)만 \| (\d+) \| (\S+) \| (\S+) \| (\S+)$", start, stop)

        if not found:
            ledger.missing(name, path)
            continue

        source = f"{path}:{line}"
        data["reported"][name] = {"drawdown": reported_number(found.group(7)), "less": reported_number(found.group(8)), "label": found.group(1)}

        for key, group in (("n", 2), ("mean", 3), ("win", 4), ("total", 5), ("total_low", 6), ("removed_n", 10), ("removed_mean", 11), ("difference", 12), ("t", 13)):
            ledger.number(f"{name} · {key}", cell[key], found.group(group), source)

        base_total = data["cells"]["C0"]["total"]
        base_drawdown = data["reported"]["C0"]["drawdown"]
        less = base_total * data["reported"][name]["drawdown"] / base_drawdown
        data["reported"][name]["less_computed"] = less
        ledger.number(f"{name} · 덜 사기(낙폭은 txt 값)", less, found.group(8), source)

    for name in ("C1", "C2", "C3", "C4"):
        for label in ("2010–2017", "2018–2026"):
            found, line = text_match(DEVSCALE_TEXT, rf"^{label} \| {name} \| (\d+) \| (\S+) \| (\d+) \| (\S+) \| (예|아니오)$", start, stop)

            if not found:
                ledger.missing(f"{label} {name}", path)
                continue

            kept_n, kept_mean, removed_n, removed_mean = data["cells"][name]["periods"][label]
            source = f"{path}:{line}"
            ledger.number(f"{label} {name} 남긴 n", kept_n, found.group(1), source)
            ledger.number(f"{label} {name} 남긴 평균", kept_mean, found.group(2), source)
            ledger.number(f"{label} {name} 걸러낸 n", removed_n, found.group(3), source)
            ledger.number(f"{label} {name} 걸러낸 평균", removed_mean, found.group(4), source)
            ledger.text(f"{label} {name} 걸러낸 쪽 더 나쁨", "예" if removed_mean < kept_mean else "아니오", found.group(5), source)

    found, line = text_match(DEVSCALE_TEXT, r"^연도별 고른 칸: (.+)$")
    choices = dict(re.findall(r"(\d{4}) (C\d)", found.group(1))) if found else {}
    data["choices"] = {int(year): name for year, name in choices.items()}
    data["choices_line"] = line
    stitched = np.zeros(len(data["trades"]), bool)

    for year, name in data["choices"].items():
        stitched |= data["masks"][name] & (data["entry_year"] == year)

    base_window = data["entry_year"] >= 2013
    net = data["trades"]["net_won_mid"].to_numpy(float) / 1e4
    data["walk"] = {"n": int(stitched.sum()), "total": float(net[stitched].sum()), "base_n": int(base_window.sum()), "base_total": float(net[base_window].sum())}
    found, line = text_match(DEVSCALE_TEXT, r"이어붙임 n (\d+) · 총 손익 (\S+)만 · 최대 낙폭 (\S+)만")

    if found:
        ledger.number("걸어가며 이어붙임 n(txt의 고른 칸으로)", data["walk"]["n"], found.group(1), f"{path}:{line}")
        ledger.number("걸어가며 이어붙임 총 손익", data["walk"]["total"], found.group(2), f"{path}:{line}")
        data["walk"]["drawdown"] = reported_number(found.group(3))

    found, line = text_match(DEVSCALE_TEXT, r"거름 없음 n (\d+) · 총 손익 (\S+)만 · 최대 낙폭 (\S+)만 → 덜 사기 (\S+)만")

    if found:
        ledger.number("2013– 거름 없음 n", data["walk"]["base_n"], found.group(1), f"{path}:{line}")
        ledger.number("2013– 거름 없음 총 손익", data["walk"]["base_total"], found.group(2), f"{path}:{line}")
        data["walk"]["base_drawdown"] = reported_number(found.group(3))
        less = data["walk"]["base_total"] * data["walk"]["drawdown"] / data["walk"]["base_drawdown"]
        data["walk"]["less"] = less
        ledger.number("2013– 덜 사기(낙폭은 txt 값)", less, found.group(4), f"{path}:{line}")

    yearly_start = text_line_index(DEVSCALE_TEXT, r"^\[연도별 안정성")

    for year in data["years"]:
        found, line = text_match(DEVSCALE_TEXT, rf"^{year} \| (\d+) \| (\S+)만 \| (\S+)만 \| (\S+)만 \| (\S+)만 \| (\S+)만 \| (\S+)$", yearly_start)

        if not found:
            ledger.missing(f"연도 {year}", path)
            continue

        for position, name in enumerate(("C0", "C1", "C2", "C3", "C4")):
            ledger.number(f"연도 {year} {name} 총 손익", data["cells"][name]["yearly"].get(year, 0.0), found.group(position + 2), f"{path}:{line}")

    return ledger


DEVSCALE_LABELS = {
    "C0": "거름 없음(지금 방식)",
    "C1": "하향 추세면 안 삼(저점 확인 3일)",
    "C2": "하향 추세면 안 삼(저점 확인 5일)",
    "C3": "상승 추세일 때만 삼(저점 확인 3일)",
    "C4": "상승 추세일 때만 삼(저점 확인 5일)",
}


def drawdown_of(value) -> float:
    """txt의 낙폭은 양수로 적혀 있어 음수로 맞춘다."""
    return float("nan") if is_missing(value) else -abs(float(value))


def devscale_section(data: dict, ledger: Ledger) -> str:
    path = DEVSCALE_SCRIPT
    stop, stop_source = constant_source(path, "STOP_LOSS_PERCENT")
    take, _ = constant_source(path, "TAKE_PROFIT_PERCENT")
    notional, notional_source = constant_source(path, "NOTIONAL_WON")
    top, top_source = constant_source(path, "UNIVERSE_TOP")
    price_minimum, _ = constant_source(path, "PRICE_MIN")
    tstat, tstat_source = constant_source(path, "TSTAT_PASS")
    first_year, first_source = constant_source(path, "WALK_FORWARD_FIRST_YEAR")
    entry_dates = pd.to_datetime(data["trades"]["entry_date"])
    conditions = [
        ["언제 사나", f"전날 거래대금 상위 {top}종목 중 5일선 > 10일선 > 20일선이고 이격이 −8%–+5%인 종목, 시가 또는 종가에 체결(일봉으로 근사)", top_source, same()],
        ["언제 파나", f"+{take}% 익절, −{stop}% 손절, 이격 띠(−12%–+9%)를 벗어나면 정리, 다음 날로 넘김", stop_source, same()],
        ["비용", "연구용 중간 비용으로 판정, 낮은 비용은 총액만 참고", where(path, r"RESEARCH_MID"), same()],
        ["거래 가능성", f"전날 거래대금 상위 {top}종목, 가격 {price_minimum:,}원 이상", top_source, same()],
        ["금액", f"매매 1건 {notional / 1e4:,.0f}만 원, 소수 주식 허용, 동시 보유 한도 없음", notional_source, same()],
        ["기간", f"매수일 {entry_dates.min():%Y-%m-%d}–{entry_dates.max():%Y-%m-%d}, 앞·뒤 기간 경계 2018-01-01", where(path, r"^PERIOD_SPLIT"), same()],
        ["매매 수", f"{len(data['trades']):,}건", relative(DEVSCALE_TRADES), same()],
        ["판단 시점", "매수 전날 장 마감까지 확정된 저점·고점", where(path, r"def keep_mask"), same()],
        ["통과 문턱", f"차이 |t| ≥ {tstat}(거름 4개를 시험한 만큼 높임), 해마다 골라 쓰기는 {first_year}년부터", tstat_source, same()],
        ["최대 낙폭", "보유 중 평가손익까지 넣은 매일 종가 평가. 일별 평가가 저장되지 않아 이 값만은 다시 계산하지 못하고 txt 값을 씀", where(path, r"class Curve"), same()],
    ]
    cells = data["cells"]
    reported = data["reported"]
    compare_rows = []

    for name in ("C0", "C1", "C2", "C3", "C4"):
        cell = cells[name]
        compare_rows.append([named_cell(DEVSCALE_LABELS[name], name), count_cell(cell["n"]), number_cell(cell["mean"]), money_cell(cell["total"]),
                             number_cell(drawdown_of(reported.get(name, {}).get("drawdown")), 0),
                             number_cell(cell["removed_mean"]) if cell["removed_n"] else blank_cell(),
                             number_cell(cell["t"]) if cell["removed_n"] else blank_cell()])

    compare = render_table(["방식", "매매 수", "매매당 평균 %", "총 손익(만 원)", "최대 낙폭(만 원)", "뺀 매매 평균 %", "차이 t"], compare_rows, classes="compare")
    criteria = []

    for name in ("C1", "C2", "C3", "C4"):
        cell = cells[name]
        both = all(cell["periods"][label][3] < cell["periods"][label][1] for label in ("2010–2017", "2018–2026"))
        all_three = cell["removed_mean"] < cell["mean"] and abs(cell["t"]) >= tstat and both
        actual = (f"뺀 매매 {percent(cell['removed_mean'])} vs 남긴 매매 {percent(cell['mean'])}, |t| {show(abs(cell['t']), sign=False)}, "
                  f"두 기간 {'모두 그렇다' if both else '엇갈린다'}")
        criteria.append([f"{DEVSCALE_LABELS[name]}: 뺀 매매가 더 나쁘고, 확실하고, 두 기간 모두 그렇다",
                         f"뺀 쪽 평균 < 남긴 쪽, |t| ≥ {tstat}", actual, verdict_cell(all_three)])

    walk = data["walk"]
    gap = walk["total"] - walk["less"]
    criteria.append(["해마다 그 전 자료로 골라 쓴 거름이 '금액만 줄이기'보다 많이 번다", f"같은 낙폭에서 {money(walk['less'])}보다 많이",
                     f"{money(walk['total'])}(차이 {money(gap)})", verdict_cell(walk["total"] > walk["less"], "통과(차이 미미)" if abs(gap) < 100 else "통과")])
    rows = []

    for name, cell in cells.items():
        rows.append([named_cell(DEVSCALE_LABELS[name], name), count_cell(cell["n"]), number_cell(cell["mean"]), number_cell(cell["win"], 1, sign=False, colored=False),
                     money_cell(cell["total"]), money_cell(cell["total_low"]), number_cell(drawdown_of(reported.get(name, {}).get("drawdown")), 0),
                     money_cell(reported.get(name, {}).get("less_computed", float("nan"))), count_cell(cell["removed_n"]), number_cell(cell["removed_mean"]),
                     number_cell(cell["difference"]), number_cell(cell["t"])])

    period_rows = []

    for name in ("C1", "C2", "C3", "C4"):
        for label in ("2010–2017", "2018–2026"):
            kept_n, kept_mean, removed_n, removed_mean = cells[name]["periods"][label]
            period_rows.append([label, named_cell(DEVSCALE_LABELS[name], name), count_cell(kept_n), number_cell(kept_mean), count_cell(removed_n),
                                number_cell(removed_mean), verdict_cell(removed_mean < kept_mean, "예", "아니오")])

    choice_rows = [[year, named_cell(DEVSCALE_LABELS.get(name, name), name)] for year, name in sorted(data["choices"].items())]
    details = [
        ("다섯 칸 전체(매매 1건 50만 원 기준 합, 열 머리를 누르면 정렬)",
         render_table(["방식", "매매 수", "매매당 평균 %", "승률 %", "총 손익 중간 비용(만 원)", "총 손익 낮은 비용(만 원)", "최대 낙폭(만 원, txt)",
                       "같은 낙폭으로 금액만 줄이면(만 원)", "뺀 매매", "뺀 매매 평균 %", "차이 %p", "차이 t"], rows, sortable=True)),
        ("앞·뒤 기간으로 나눠 보기", render_table(["기간", "방식", "남긴 매매", "남긴 평균 %", "뺀 매매", "뺀 평균 %", "뺀 쪽이 더 나쁨"], period_rows)),
        (f"해마다 고른 칸 — 이어붙인 {walk['n']:,}건 총 {money(walk['total'])}, 같은 기간 거름 없음 {walk['base_n']:,}건 총 {money(walk['base_total'])}, "
         f"금액만 줄이면 {money(walk['less'])}",
         render_table(["해", "고른 칸"], choice_rows)),
    ]
    years = data["years"]
    yearly = bar_chart([str(year)[2:] for year in years], [(DEVSCALE_LABELS[name], [cells[name]["yearly"].get(year, 0.0) for year in years])
                                                          for name in ("C0", "C1", "C4")], "만 원", "매수 연도별 총 손익(중간 비용)")
    base_mean = cells["C0"]["mean"]
    filtered_means = ", ".join(percent(cells[name]["mean"]) for name in ("C1", "C2", "C3", "C4"))
    passed_any = any(row[3]["sort"] == "1" for row in criteria[:4])
    answer_text = (f"{'아니오' if not passed_any else '일부'}. 하향 추세를 빼면 매매 수가 줄어 총 손실은 줄지만({money(cells['C0']['total'])} → "
                   f"{money(cells['C1']['total'])}), 매매당 손실은 그대로다(지금 {percent(base_mean)}, 거른 뒤 {filtered_means}). "
                   "뺀 매매가 더 나빴다는 근거도 없다.")
    return section("devscale", "6. DevScale을 하향 추세 종목에서 안 사면 나아지나?", ("일부", "partial") if passed_any else ("아니오", "no"), answer_text,
                   compare, criteria, yearly, details, conditions, path, ["py -X utf8 research/studies/37_swing_trend/devscale_trend_filter.py"],
                   [DEVSCALE_TEXT, DEVSCALE_TRADES], [DEVSCALE_TEXT], ledger)


# ---------------------------------------------------------------- 7 ma_support_exits


SUPPORT_KEYS = ("n", "net", "excess", "t_net", "t_excess", "net_2010_17", "net_2018_26", "win_rate", "hold_days", "stop_share", "take_share")


def support_compute() -> dict:
    cells = pd.read_parquet(SUPPORT_CELLS)
    walk = pd.read_parquet(SUPPORT_WALK)
    portfolio = pd.read_parquet(SUPPORT_PORTFOLIO)
    portfolio_trades = pd.read_parquet(SUPPORT_PORTFOLIO_TRADES)
    capital, _ = constant_of(SUPPORT_SCRIPT, "CAPITAL_KRW")
    entry = walk["entry_date"].to_numpy()
    months = entry // 100
    early = entry < 20180101
    net = walk["net"].to_numpy(float)
    excess = walk["excess"].to_numpy(float)
    walk_summary = {"n": len(walk), "net": float(net.mean()), "excess": float(np.nanmean(excess)), "t_net": cluster_mean_t(net, months),
                    "t_excess": cluster_mean_t(excess, months), "net_2013_17": float(net[early].mean()), "net_2018_26": float(net[~early].mean()),
                    "win_rate": float((net > 0).mean() * 100.0), "hold_days": float(walk["hold_days"].mean())}
    choices = []

    for year, group in walk.groupby(walk["entry_date"] // 10000):
        choices.append({"year": int(year), "cell": group["cell"].iloc[0], "cells": group["cell"].nunique(), "n": len(group),
                        "net": float(group["net"].mean()), "excess": float(group["excess"].mean())})

    trend_names = {1: "상승", 0: "횡보", -1: "하향", -9: "미정"}
    trends = {}

    for code, name in trend_names.items():
        mask = walk["trend"].to_numpy() == code
        trends[name] = {"n": int(mask.sum()), "net": float(net[mask].mean()), "t": cluster_mean_t(net[mask], months[mask])}

    down = walk["trend"].to_numpy() == -1
    trend_difference = cluster_regression(net, down, months)

    def portfolio_summary(equity: np.ndarray, side: str) -> dict:
        drawdown = equity / np.maximum.accumulate(equity) - 1.0
        part = portfolio_trades[portfolio_trades["side"] == side]
        yearly = {}
        previous = capital

        for year in sorted(portfolio["year"].unique()):
            year_mask = (portfolio["year"] == year).to_numpy()
            end_value = float(equity[year_mask][-1])
            year_trades = part[part["entry_year"] == year]
            yearly[int(year)] = {"return": (end_value / previous - 1.0) * 100.0, "profit": (end_value - previous) / 1e4,
                                 "drawdown": float(drawdown[year_mask].min() * 100.0), "trades": len(year_trades),
                                 "trade_net": float(year_trades["net"].mean()) if len(year_trades) else float("nan")}
            previous = end_value

        return {"total_return": (equity[-1] / capital - 1.0) * 100.0, "max_drawdown": float(drawdown.min() * 100.0), "trades": len(part),
                "trade_net": float(part["net"].mean()), "win_rate": float((part["net"] > 0).mean() * 100.0), "yearly": yearly}

    shape = portfolio_summary(portfolio["equity"].to_numpy(float), "shape")
    baseline = portfolio_summary(portfolio["baseline_equity"].to_numpy(float), "baseline")
    return {"cells": cells, "walk": walk_summary, "choices": choices, "trends": trends, "trend_difference": trend_difference,
            "shape": shape, "baseline": baseline, "portfolio_start": int(portfolio["date"].iloc[0]), "portfolio_end": int(portfolio["date"].iloc[-1])}


def support_ledger(data: dict) -> Ledger:
    ledger = Ledger("ma_support_exits")
    path = relative(SUPPORT_TEXT)
    full_start = text_line_index(SUPPORT_TEXT, r"E2 전체 180칸")
    cells = data["cells"].set_index("cell")

    for name, row in cells.iterrows():
        found, line = text_match(SUPPORT_TEXT, rf"^\s+{name}\s+" + r"\s+".join([r"(\S+)"] * 11) + r"\s*$", full_start)

        if not found:
            ledger.missing(f"180칸 {name}", path)
            continue

        for position, key in enumerate(SUPPORT_KEYS):
            ledger.number(f"{name} · {key}", float(row[key]), found.group(position + 1), f"{path}:{line}", kind="산출물 대조")

    found, line = text_match(SUPPORT_TEXT, r"n ([\d,]+) 순수익 (\S+)% 초과 (\S+)%p 순t (\S+) 초과t (\S+) 2013–17 (\S+) 2018–26 (\S+) 승률 (\S+) 보유일 (\S+)")

    if found:
        for position, key in enumerate(("n", "net", "excess", "t_net", "t_excess", "net_2013_17", "net_2018_26", "win_rate", "hold_days")):
            ledger.number(f"E3(a) 이어붙임 · {key}", data["walk"][key], found.group(position + 1), f"{path}:{line}")
    else:
        ledger.missing("E3(a) 요약", path)

    for choice in data["choices"]:
        found, line = text_match(SUPPORT_TEXT, rf"^\s+{choice['year']} (N\S+)\s+\(지난 순t\s+(\S+)\) n\s+(\d+) 순수익\s+(\S+) 초과\s+(\S+)")

        if not found:
            ledger.missing(f"E3(a) {choice['year']}", path)
            continue

        source = f"{path}:{line}"
        ledger.text(f"E3(a) {choice['year']} 고른 칸", choice["cell"], found.group(1), source)
        ledger.number(f"E3(a) {choice['year']} n", choice["n"], found.group(3), source)
        ledger.number(f"E3(a) {choice['year']} 순수익", choice["net"], found.group(4), source)
        ledger.number(f"E3(a) {choice['year']} 초과", choice["excess"], found.group(5), source)

    for side, label in (("shape", "모양 있음"), ("baseline", "모양 없음")):
        found, line = text_match(SUPPORT_TEXT, rf"{label}: 총수익 (\S+)% 최대 낙폭 (\S+)% 건수 (\d+) 매매당 (\S+)% 승률 (\S+)")

        if not found:
            ledger.missing(f"E4 {label}", path)
            continue

        for position, key in enumerate(("total_return", "max_drawdown", "trades", "trade_net", "win_rate")):
            ledger.number(f"E4 {label} · {key}", data[side][key], found.group(position + 1), f"{path}:{line}")

    for year in data["shape"]["yearly"]:
        found, line = text_match(SUPPORT_TEXT, rf"^\s+{year}\s+(\S+)\s+(\S+)\s+(\S+)\s+(\d+)\s+(\S+) \|\s+(\S+)\s+(\S+)\s+(\S+)\s+(\d+)\s+(\S+)$")

        if not found:
            ledger.missing(f"E4 {year}", path)
            continue

        for offset, side in ((0, "shape"), (5, "baseline")):
            yearly = data[side]["yearly"][year]

            for position, key in enumerate(("return", "profit", "drawdown", "trades", "trade_net")):
                ledger.number(f"E4 {year} {side} · {key}", yearly[key], found.group(offset + position + 1), f"{path}:{line}")

    e5_start = text_line_index(SUPPORT_TEXT, r"^\s+E3\(a\) 이어붙인 매매$")

    for name, trend in data["trends"].items():
        found, line = text_match(SUPPORT_TEXT, rf"^\s+{name}\s+n\s+([\d,]+) 순수익\s+(\S+) 순t\s+(\S+)", e5_start, e5_start + 6)

        if found:
            ledger.number(f"E5 이어붙임 {name} n", trend["n"], found.group(1), f"{path}:{line}")
            ledger.number(f"E5 이어붙임 {name} 순수익", trend["net"], found.group(2), f"{path}:{line}")
            ledger.number(f"E5 이어붙임 {name} 순t", trend["t"], found.group(3), f"{path}:{line}")

    found, line = text_match(SUPPORT_TEXT, r"하향 − 나머지 차이 (\S+)%p 묶음 t (\S+)", e5_start, e5_start + 7)

    if found:
        ledger.number("E5 이어붙임 하향 − 나머지 차이", data["trend_difference"][0], found.group(1), f"{path}:{line}")
        ledger.number("E5 이어붙임 하향 − 나머지 t", data["trend_difference"][1], found.group(2), f"{path}:{line}")

    return ledger


def support_stop_label(stop: str) -> str:
    return "근거 저점 −3% 손절" if stop == "SP" else f"산 값 −{stop[1:]}% 손절"


def support_label(cell: str) -> str:
    """N10_T15_SP_H60 → 10일선·익절 15%·근거 저점 −3% 손절·최대 60일."""
    window, take, stop, hold = cell.split("_")
    return f"{window[1:]}일선·익절 {take[1:]}%·{support_stop_label(stop)}·최대 {hold[1:]}일"


def support_section(data: dict, ledger: Ledger) -> str:
    path = SUPPORT_SCRIPT
    takes, takes_source = constant_source(path, "TAKES")
    stops, _ = constant_source(path, "STOPS")
    holds, _ = constant_source(path, "HOLDS")
    windows, windows_source = constant_source(path, "WINDOWS")
    pool, pool_source = constant_source(path, "BASELINE_POOL")
    trade_amount, trade_source = constant_source(path, "TRADE_KRW")
    capital, _ = constant_source(path, "CAPITAL_KRW")
    daily_new, _ = constant_source(path, "DAILY_NEW")
    maximum_held, _ = constant_source(path, "MAX_HELD")
    pass_walk, pass_source = constant_source(path, "PASS_WALK_T")
    header_line, header_text = line_of(SUPPORT_TEXT, r"비교 = 같은 날 신호일 거름을 지난 전 종목")
    changed_line, _ = line_of(SUPPORT_TEXT, r"^\[실행 뒤 바꾼 것\]")
    window_text = "·".join(f"{window}일" for window in windows)
    conditions = [
        ["언제 사나", f"신호일 저가가 이평선 × 1.01 이하로 닿고, 종가는 이평선 위, 이평선은 오르는 중. 추세 조건은 없음. 이평선 {window_text}. 다음 날 시가",
         windows_source, same()],
        ["언제 파나", f"익절 {'·'.join(str(take) for take in takes)}% × 손절 {len(stops)}가지(산 값 −5·−7·−10%, 근거 저점 −3%) × 최대 보유 "
         f"{'·'.join(str(hold) for hold in holds)}거래일", takes_source, same()],
        ["비용", "수수료·세금 왕복 0.23%(스터디 35와 같은 값)", where(path, r"COST"), same()],
        ["거래 가능성", "신호일 종가 2,000원 이상, 20일 평균 거래대금 5억 원 이상", where(path, r"MIN_TURNOVER20_KRW|TURNOVER"), same()],
        ["기간", f"신호 2010-01-01–2026-10-06, 해마다 골라 쓰기 2013년부터, 자산 곡선 {data['portfolio_start']}–{data['portfolio_end']}",
         where(path, r"SPLIT_DATE"), same()],
        ["매매 수", f"180칸, 이어붙인 매매 {data['walk']['n']:,}건", relative(SUPPORT_CELLS), same()],
        ["판단 시점", "신호일 장 마감에 판단, 다음 거래일 시가에 매수", where(path, r"def summarize"), same()],
        ["자산 곡선", f"자본 {capital / 1e4:,.0f}만 원, 건당 {trade_amount / 1e4:,.0f}만 원, 하루 신규 {daily_new}건·동시 보유 {maximum_held}건까지, 현금이 모자라면 안 삼",
         trade_source, differs(f"첫 실행 뒤 '현금이 모자라면 안 삼'을 더했다(txt {changed_line}줄에 적음)") if changed_line else same()],
        ["자산 곡선 비교군", f"모양 없음 = 그날 거래대금 상위 {pool}종목에서 같은 방식으로 고름", pool_source,
         differs(f"txt 머리말({header_line}줄)은 '거름을 지난 전 종목'이라 적었지만 코드는 상위 {pool}종목") if header_line and pool else same()],
        ["통과 문턱", f"① 해마다 골라 쓴 매매의 순수익 > 0·t ≥ {pass_walk}·두 기간 모두 > 0 ② 마지막에 고른 칸의 이웃도 > 0 ③ 자산 곡선이 이익이고 모양 없음보다 큼",
         pass_source, same()],
    ]
    cells_frame = data["cells"]
    headers = ["칸", "매매 수", "순수익 %", "시장 대비 %p", "순수익 t", "시장 대비 t", "2010–17 순수익 %", "2018–26 순수익 %", "승률 %", "보유일",
               "손절 비중 %", "익절 비중 %"]

    def cell_row(row) -> list:
        return [named_cell(support_label(row["cell"]), row["cell"]), count_cell(row["n"]), number_cell(row["net"]), number_cell(row["excess"]),
                number_cell(row["t_net"]), number_cell(row["t_excess"]), number_cell(row["net_2010_17"]), number_cell(row["net_2018_26"]),
                number_cell(row["win_rate"], 1, sign=False, colored=False), number_cell(row["hold_days"], 1, sign=False, colored=False),
                number_cell(row["stop_share"], 1, sign=False, colored=False), number_cell(row["take_share"], 1, sign=False, colored=False)]

    ordered = cells_frame.sort_values("t_net", ascending=False)
    top_rows = [cell_row(row) for _, row in ordered.head(10).iterrows()]
    all_rows = [cell_row(row) for _, row in ordered.iterrows()]
    heatmaps = []

    for window in windows:
        grid = []

        for take in takes:
            row_values = []

            for stop in stops:
                match = cells_frame[(cells_frame["window"] == window) & (cells_frame["take"] == take) & (cells_frame["stop"] == stop) & (cells_frame["hold"] == 60)]
                row_values.append(float(match["net"].iloc[0]) if len(match) else float("nan"))

            grid.append(row_values)

        heatmaps.append(heatmap([f"익절 {take}%" for take in takes], [support_stop_label(str(stop)) for stop in stops], grid,
                                f"{window}일선 · 최대 60일 보유 — 매매당 순수익 %"))

    choice_rows = [[choice["year"], named_cell(support_label(choice["cell"]), choice["cell"]), count_cell(choice["n"]), number_cell(choice["net"]),
                    number_cell(choice["excess"])] for choice in data["choices"]]
    walk = data["walk"]
    final_cell = data["choices"][-1]["cell"]
    metrics = json.loads(SUPPORT_METRICS.read_bytes().decode("utf-8"))
    neighbors = metrics.get("neighbors", [])
    neighbor_rows = [[named_cell(support_label(item["cell"]), item["cell"]), count_cell(item["n"]), number_cell(item["net"]), number_cell(item["t_net"])]
                     for item in neighbors]
    neighbor_ok = all(item["net"] > 0 for item in neighbors)
    negative_neighbors = [item for item in neighbors if item["net"] <= 0]
    shape, baseline = data["shape"], data["baseline"]
    criteria = [
        ["①-1 해마다 골라 쓴 매매가 비용을 빼고 이익이다", "순수익 > 0", percent(walk["net"]), verdict_cell(walk["net"] > 0)],
        ["①-2 그 이익이 우연이 아니다", f"t ≥ {pass_walk}", show(walk["t_net"]), verdict_cell(walk["t_net"] >= pass_walk)],
        ["①-3 앞·뒤 기간 모두 이익이다", "둘 다 > 0", f"2013–17 {percent(walk['net_2013_17'])}, 2018–26 {percent(walk['net_2018_26'])}",
         verdict_cell(walk["net_2013_17"] > 0 and walk["net_2018_26"] > 0)],
        ["② 마지막에 고른 칸의 이웃(설정을 한 칸 바꾼 것)도 이익이다", f"이웃 {len(neighbors)}칸 모두 > 0",
         f"{len(neighbors) - len(negative_neighbors)}/{len(neighbors)}칸", verdict_cell(neighbor_ok)],
        ["③ 돈을 넣어 굴린 자산 곡선이 이익이다", "총수익 > 0", percent(shape["total_return"]), verdict_cell(shape["total_return"] > 0)],
        ["③ 아무 종목이나 산 경우(모양 없음)보다 낫다", f"{percent(baseline['total_return'])}보다 큼", percent(shape["total_return"]),
         verdict_cell(shape["total_return"] > baseline["total_return"])],
        ["참고: 180칸 중 하나라도 아주 확실한 칸이 있다", "t ≥ 3.5", f"가장 큰 칸 {support_label(ordered.iloc[0]['cell'])} t {show(ordered.iloc[0]['t_net'])}",
         verdict_cell(ordered.iloc[0]["t_net"] >= 3.5)],
    ]
    compare_rows = [
        [named_cell("해마다 그 전 자료로 칸을 골라 이어붙인 매매(금액 제약 없음)"), count_cell(walk["n"]), number_cell(walk["net"]),
         number_cell(walk["win_rate"], 1, sign=False, colored=False), blank_cell(), blank_cell()],
        [named_cell(f"자산 곡선 — 모양 있음({support_label(final_cell)} 등)"), count_cell(shape["trades"]), number_cell(shape["trade_net"]),
         number_cell(shape["win_rate"], 1, sign=False, colored=False), number_cell(shape["total_return"]), number_cell(shape["max_drawdown"])],
        [named_cell("자산 곡선 — 모양 없음(아무 종목)"), count_cell(baseline["trades"]), number_cell(baseline["trade_net"]),
         number_cell(baseline["win_rate"], 1, sign=False, colored=False), number_cell(baseline["total_return"]), number_cell(baseline["max_drawdown"])],
    ]
    compare = render_table(["방식", "매매 수", "매매당 순수익 %", "승률 %", "총수익 %", "최대 낙폭 %"], compare_rows, classes="compare")
    trend_rows = [[name, count_cell(trend["n"]), number_cell(trend["net"]), number_cell(trend["t"])] for name, trend in data["trends"].items()]
    trend_rows.append(["하향 − 나머지", blank_cell(), number_cell(data["trend_difference"][0]), number_cell(data["trend_difference"][1])])
    years = sorted(shape["yearly"])
    yearly_rows = [[year, number_cell(shape["yearly"][year]["return"]), money_cell(shape["yearly"][year]["profit"]), number_cell(shape["yearly"][year]["drawdown"]),
                    count_cell(shape["yearly"][year]["trades"]), number_cell(shape["yearly"][year]["trade_net"]),
                    number_cell(baseline["yearly"][year]["return"]), money_cell(baseline["yearly"][year]["profit"]), number_cell(baseline["yearly"][year]["drawdown"]),
                    count_cell(baseline["yearly"][year]["trades"]), number_cell(baseline["yearly"][year]["trade_net"])] for year in years]
    details = [
        ("칸 이름 읽는 법", f"<p>180칸 = 이평선 {len(windows)}가지 × 익절 {len(takes)}가지 × 손절 {len(stops)}가지 × 최대 보유 {len(holds)}가지. "
         "코드 이름 N10_T15_SP_H60은 '10일선·익절 15%·근거 저점 −3% 손절·최대 60일'. 근거 저점 = 신호를 만든 저점.</p>"),
        ("순수익 t가 큰 10칸", render_table(headers, top_rows)),
        ("익절 × 손절 — 최대 60일 보유", '<div class="heatmap-row">' + "".join(heatmaps) + "</div>"),
        (f"해마다 고른 칸 — 이어붙인 {walk['n']:,}건, 순수익 {percent(walk['net'])}, t {show(walk['t_net'])}, 시장 대비 t {show(walk['t_excess'])}",
         render_table(["해", "고른 칸", "매매 수", "순수익 %", "시장 대비 %p"], choice_rows)),
        (f"마지막에 고른 칸({support_label(final_cell)})의 이웃", render_table(["칸", "매매 수", "순수익 %", "t"], neighbor_rows)),
        ("이어붙인 매매를 추세별로 나눠 보기(저점 확인 5일)", render_table(["추세", "매매 수", "순수익 %", "t"], trend_rows)),
        ("자산 곡선 연도별",
         render_table(["해", "모양 있음 수익 %", "손익(만 원)", "낙폭 %", "매매 수", "매매당 %", "모양 없음 수익 %", "손익(만 원)", "낙폭 %", "매매 수", "매매당 %"],
                      yearly_rows)),
        ("180칸 전체", f'<details class="inner"><summary>180칸 표 펼치기(열 머리를 누르면 정렬)</summary>{render_table(headers, all_rows, sortable=True)}</details>'),
    ]
    yearly = bar_chart([str(year)[2:] for year in years], [("모양 있음", [shape["yearly"][year]["return"] for year in years]),
                                                          ("모양 없음", [baseline["yearly"][year]["return"] for year in years])],
                       "% (그해 초 자산 대비)", "자산 곡선 연도별 수익률")
    negative_text = ", ".join(support_label(item["cell"]) for item in negative_neighbors)
    answer_text = (f"아니오. 이어붙인 매매는 매매당 {percent(walk['net'])}로 조금 남지만 t {show(walk['t_net'])}로 확실하지 않고"
                   + (f", 이웃 칸 중 {negative_text}이 손실이며" if negative_neighbors else "")
                   + f", 돈을 넣어 굴리면 {percent(shape['total_return'], 1)}(아무 종목 {percent(baseline['total_return'], 1)})이다. "
                   f"시장 대비 t({show(walk['t_excess'])})가 큰 것은 비교 기준인 전 종목 평균이 손실이라서다.")
    all_passed = all(row[3]["sort"] == "1" for row in criteria[:6])
    return section("support", "7. 오르는 이평선에 닿고 위에서 끝난 날 사면?", ("예", "yes") if all_passed else ("아니오", "no"), answer_text, compare, criteria,
                   yearly, details, conditions, path, ["py -X utf8 research/studies/37_swing_trend/ma_support_exits.py"],
                   [SUPPORT_TEXT, SUPPORT_METRICS, SUPPORT_CELLS, SUPPORT_WALK, SUPPORT_PORTFOLIO, SUPPORT_PORTFOLIO_TRADES], [SUPPORT_TEXT], ledger)


# ---------------------------------------------------------------- 코드·설명 어긋남


def collect_discrepancies(halt_data: dict, validate_data: dict, overnight_data: dict, devscale_data: dict) -> None:
    plan_t = line_of(HALT_SCRIPT, r"\|t\| ≥ 3\.1")
    judge_start = line_of(HALT_SCRIPT, r"^def judge")[0]
    judge_end = line_of(HALT_SCRIPT, r"^# -+ 출력")[0]
    judge_uses_t = any("difference_t" in line for line in read_lines(HALT_SCRIPT)[judge_start - 1:judge_end])
    passed = [item["name"] for item in halt_data["items"] if halt_data["rows"][item["name"]]["passed"]]
    passed_t = "·".join(show(halt_data["rows"][name]["difference_t"]) for name in passed)
    note_discrepancy("1·2 매수 멈춤", f"계획서는 '뺀 매매가 확실히 나빠야 한다(차이 |t| 3.1 이상)'를 기준에 넣었지만 판정 코드에는 이 조건이 없다. "
                     f"통과한 {len(passed)}칸의 차이 t는 {passed_t}로 이 기준이면 모두 떨어진다.",
                     f"{relative(HALT_SCRIPT)}:{judge_start}", f"{relative(HALT_SCRIPT)}:{plan_t[0]}", bool(plan_t[0]) and not judge_uses_t)
    unit = line_of(HALT_SCRIPT, r"최대 낙폭\(원\)")
    note_discrepancy("1·2 매수 멈춤", "계획서는 최대 낙폭 단위를 '원'이라 적었지만 실제 값은 만 원이다.", where(HALT_SCRIPT, r"^POSITION_WON"),
                     f"{relative(HALT_SCRIPT)}:{unit[0]}", bool(unit[0]))
    early_line = line_of(HALT_SCRIPT, r'kept\["year"\] < SPLIT_YEAR')
    yearly_line = line_of(HALT_SCRIPT, r'groupby\("entry_year"\)\["profit"\]')
    note_discrepancy("1·2 매수 멈춤", "앞·뒤 기간은 급등이 난 해로 나누고, 연도별 막대는 매수한 해로 묶는다. 해를 넘겨 산 매매는 두 표에서 다른 해로 잡힌다.",
                     f"{relative(HALT_SCRIPT)}:{early_line[0]}", f"{relative(HALT_SCRIPT)}:{yearly_line[0]}", bool(early_line[0] and yearly_line[0]))
    wide_text = line_of(VALIDATE_SCRIPT, r"대기 3–5")
    note_discrepancy("3 이평선 규칙 재검증", f"넓은 표본 설명은 '대기 3–5일'인데 코드에는 그 거름이 없다. 자료의 대기 값이 {validate_data['wide_waits']}뿐이라 결과는 같다.",
                     where(VALIDATE_SCRIPT, r"def wide_events"), f"{relative(VALIDATE_SCRIPT)}:{wide_text[0]}", bool(wide_text[0]))
    plan_seven = line_of(OVERNIGHT_SCRIPT, r"0\.05/7")
    count = len(overnight_data["indicators"])
    note_discrepancy("4 미국장", f"계획서는 여러 번 시험한 만큼 문턱을 높이는 계산을 지표 7개 기준으로 적었지만 실제 지표는 {count}개다. "
                     f"같은 줄에 {count}개 기준 문턱 {overnight_data['limit']}도 적어 두었고 판정은 같다.",
                     where(OVERNIGHT_SCRIPT, r"^INDICATORS"), f"{relative(OVERNIGHT_SCRIPT)}:{plan_seven[0]}", bool(plan_seven[0]))
    header = line_of(SUPPORT_TEXT, r"비교 = 같은 날 신호일 거름을 지난 전 종목")
    pool, pool_line = constant_of(SUPPORT_SCRIPT, "BASELINE_POOL")
    note_discrepancy("7 이평선 지지", f"자산 곡선의 비교군을 txt는 '거름을 지난 전 종목'이라 적었지만 코드는 그날 거래대금 상위 {pool}종목에서만 고른다.",
                     f"{relative(SUPPORT_SCRIPT)}:{pool_line}", f"{relative(SUPPORT_TEXT)}:{header[0]}", bool(header[0] and pool))
    changed = line_of(SUPPORT_TEXT, r"^\[실행 뒤 바꾼 것\]")
    note_discrepancy("7 이평선 지지", "자산 곡선에 첫 실행 뒤 '현금이 200만 원보다 적으면 안 삼'을 더했다(txt에 적혀 있음). 시험 칸과 통과 기준은 그대로다.",
                     where(SUPPORT_SCRIPT, r"no_cash_skips"), f"{relative(SUPPORT_TEXT)}:{changed[0]}", bool(changed[0]))
    devscale_verdict = line_of(DEVSCALE_TEXT, r"^전체 기준 4\)")
    walk = devscale_data.get("walk", {})
    gap_text = (f"{money(walk['total'])} 대 {money(walk['less'])}(차이 {money(walk['total'] - walk['less'])})" if "less" in walk else "거의 같은 값")
    note_discrepancy("6 DevScale 추세 거름", f"'해마다 골라 쓰기가 금액만 줄이기보다 낫다'는 기준이 {gap_text}로 '통과'로 찍힌다. 문장대로면 맞지만 사실상 같은 값이다.",
                     where(DEVSCALE_SCRIPT, r"def walk_forward"), f"{relative(DEVSCALE_TEXT)}:{devscale_verdict[0]}", bool(devscale_verdict[0]))


# ---------------------------------------------------------------- 페이지


STYLE = """
:root{--bg:#f7f7f5;--surface:#ffffff;--surface-2:#f0f0ec;--text:#1d1f21;--muted:#6a6e73;--line:#e3e3de;--line-strong:#c9c9c2;--accent:#2f5d8a;--pos:#1f7a45;--neg:#b3261e;--partial:#8a6100;--pos-bg:#e3f2e8;--neg-bg:#fbe5e3;--partial-bg:#fbf0d4;--note-bg:#ececea;--series-1:#7d8590;--series-2:#2f6fb0;--series-3:#d08a2c;--series-4:#5b9a68;color-scheme:light}
@media (prefers-color-scheme: dark){:root:not([data-theme="light"]){--bg:#15171a;--surface:#1d2024;--surface-2:#24282d;--text:#e6e6e3;--muted:#9a9fa6;--line:#30353b;--line-strong:#454b52;--accent:#8db7e0;--pos:#5cc489;--neg:#f07a70;--partial:#e2b44f;--pos-bg:#173524;--neg-bg:#3d1c1a;--partial-bg:#3a2f12;--note-bg:#2b2f34;--series-1:#8b929b;--series-2:#5d9de0;--series-3:#e0a54f;--series-4:#74b883;color-scheme:dark}}
:root[data-theme="dark"]{--bg:#15171a;--surface:#1d2024;--surface-2:#24282d;--text:#e6e6e3;--muted:#9a9fa6;--line:#30353b;--line-strong:#454b52;--accent:#8db7e0;--pos:#5cc489;--neg:#f07a70;--partial:#e2b44f;--pos-bg:#173524;--neg-bg:#3d1c1a;--partial-bg:#3a2f12;--note-bg:#2b2f34;--series-1:#8b929b;--series-2:#5d9de0;--series-3:#e0a54f;--series-4:#74b883;color-scheme:dark}
*{box-sizing:border-box}
html{-webkit-text-size-adjust:100%}
body{margin:0;background:var(--bg);color:var(--text);font-family:"Noto Sans KR",system-ui,-apple-system,"Segoe UI",sans-serif;font-size:15px;line-height:1.65;overflow-x:hidden}
main{max-width:1100px;margin:0 auto;padding:28px 20px 96px}
a{color:var(--accent);text-decoration:none}
a:hover{text-decoration:underline}
code,pre,.code-name{font-family:"IBM Plex Mono",ui-monospace,Consolas,monospace}
code{font-size:.86em;background:var(--surface-2);padding:1px 5px;border-radius:4px;overflow-wrap:anywhere}
.page-head{display:flex;justify-content:space-between;align-items:flex-start;gap:12px}
h1{font-size:1.7rem;line-height:1.3;margin:0 0 6px}
h2{font-size:1.3rem;line-height:1.4;margin:0 0 12px}
h3{font-size:1rem;margin:28px 0 8px;color:var(--text)}
h4{font-size:.95rem;margin:24px 0 6px}
p{margin:8px 0}
.theme-toggle{flex:none;font:inherit;font-size:.8rem;padding:4px 10px;border:1px solid var(--line-strong);border-radius:6px;background:var(--surface);color:var(--muted);cursor:pointer}
.theme-toggle:hover{color:var(--text)}
.check-line{font-weight:600;margin:4px 0 2px}
.check-line.ok{color:var(--pos)}
.check-line.bad{color:var(--neg)}
.meta,.muted{color:var(--muted)}
.meta{font-size:.88rem}
.summary{margin-top:28px}
.toc{margin:28px 0 0;padding:14px 18px;background:var(--surface);border:1px solid var(--line);border-radius:8px}
.toc ol{margin:6px 0 0;padding-left:1.4em}
.toc li{margin:3px 0}
section.test,section.closing{margin-top:72px;padding-top:28px;border-top:1px solid var(--line-strong)}
.answer{font-size:1.02rem;margin:4px 0 0;padding:12px 14px;background:var(--surface);border:1px solid var(--line);border-radius:8px}
.answer .badge{margin-right:6px}
.source{margin-top:18px;font-size:.84rem;color:var(--muted)}
.table-caption{margin:16px 0 6px;font-size:.88rem;color:var(--muted)}
.table-wrap{overflow-x:auto;margin:8px 0 4px;border:1px solid var(--line);border-radius:8px;background:var(--surface)}
table{border-collapse:collapse;width:100%;font-size:.88rem}
th,td{padding:7px 10px;text-align:left;vertical-align:top;border-bottom:1px solid var(--line)}
thead th{background:var(--surface-2);font-weight:600;white-space:nowrap;border-bottom:1px solid var(--line-strong)}
tbody tr:last-child td,tbody tr:last-child th{border-bottom:none}
tbody tr:hover{background:var(--surface-2)}
.num{text-align:right;font-variant-numeric:tabular-nums;white-space:nowrap}
td.pos{color:var(--pos)}
td.neg{color:var(--neg)}
td.label{min-width:16em}
.code-name{font-size:.76em;color:var(--muted);margin-left:4px;white-space:nowrap}
table.sortable thead th{cursor:pointer}
table.sortable thead th[data-order="ascending"]::after{content:" ▲";font-size:.7em}
table.sortable thead th[data-order="descending"]::after{content:" ▼";font-size:.7em}
.badge{display:inline-block;padding:1px 8px;border-radius:999px;font-size:.8rem;font-weight:600;line-height:1.6;white-space:nowrap}
.badge-yes{background:var(--pos-bg);color:var(--pos)}
.badge-no{background:var(--neg-bg);color:var(--neg)}
.badge-partial{background:var(--partial-bg);color:var(--partial)}
.badge-note{background:var(--note-bg);color:var(--muted)}
td.verdict-cell{white-space:nowrap}
.criteria td:first-child{min-width:14em}
details.more{margin-top:28px;border:1px solid var(--line);border-radius:8px;background:var(--surface)}
details.more>summary{cursor:pointer;padding:10px 14px;font-weight:600;color:var(--accent)}
details.more[open]>summary{border-bottom:1px solid var(--line)}
.more-body{padding:4px 14px 16px}
details.inner{margin:10px 0}
details.inner>summary{cursor:pointer;color:var(--accent);font-size:.9rem}
pre{margin:6px 0;padding:10px 12px;background:var(--surface-2);border-radius:6px;overflow-x:auto;font-size:.82rem;line-height:1.5}
pre.raw{max-height:480px;overflow:auto;white-space:pre}
ul.paths{margin:6px 0;padding-left:1.2em;font-size:.88rem}
.glossary{margin-top:16px;border:1px solid var(--line);border-radius:8px;background:var(--surface)}
.glossary>summary{cursor:pointer;padding:9px 14px;font-weight:600}
.glossary dl{margin:0;padding:4px 14px 12px}
.glossary dt{font-weight:600;margin-top:8px}
.glossary dd{margin:2px 0 0;color:var(--muted)}
.chart-box{margin:8px 0;padding:10px 12px;background:var(--surface);border:1px solid var(--line);border-radius:8px}
.chart-box figcaption{font-size:.88rem;color:var(--muted);margin-bottom:4px}
svg.chart{width:100%;height:auto;display:block}
svg.chart .grid{stroke:var(--line);stroke-width:1}
svg.chart .zero{stroke:var(--line-strong);stroke-width:1.2}
svg.chart .axis-text{fill:var(--muted);font-size:11px}
.bar-1{fill:var(--series-1);background:var(--series-1)}
.bar-2{fill:var(--series-2);background:var(--series-2)}
.bar-3{fill:var(--series-3);background:var(--series-3)}
.bar-4{fill:var(--series-4);background:var(--series-4)}
.legend{display:flex;flex-wrap:wrap;gap:4px 16px;font-size:.84rem;color:var(--muted);margin-top:4px}
.legend-item{display:inline-flex;align-items:center;gap:6px}
.swatch{display:inline-block;width:11px;height:11px;border-radius:2px}
.heatmap-row{display:grid;grid-template-columns:repeat(auto-fit,minmax(300px,1fr));gap:12px}
.heatmap-box .table-wrap{margin-top:0}
table.heatmap td.heat{color:var(--text)}
table.heatmap tbody th{white-space:nowrap;font-weight:500;background:var(--surface-2)}
@media (max-width:640px){main{padding:20px 16px 72px} h1{font-size:1.4rem} h2{font-size:1.15rem} section.test,section.closing{margin-top:56px} .table-wrap.stacked{border:none;background:transparent;overflow:visible} .table-wrap.stacked table,.table-wrap.stacked tbody,.table-wrap.stacked tr,.table-wrap.stacked td{display:block;width:100%} .table-wrap.stacked thead{display:none} .table-wrap.stacked tr{margin:0 0 10px;padding:8px 12px;border:1px solid var(--line);border-radius:8px;background:var(--surface)} .table-wrap.stacked td{border:none;padding:3px 0;text-align:left;white-space:normal} .table-wrap.stacked td::before{content:attr(data-label);display:block;font-size:.76rem;color:var(--muted)}}
"""

SCRIPT = """
(function () {
  function sortTable(header) {
    var table = header.closest("table");
    var body = table.tBodies[0];
    var column = Array.prototype.indexOf.call(header.parentNode.children, header);
    var ascending = header.getAttribute("data-order") !== "ascending";
    Array.prototype.forEach.call(header.parentNode.children, function (other) { other.removeAttribute("data-order"); });
    header.setAttribute("data-order", ascending ? "ascending" : "descending");
    var rows = Array.prototype.slice.call(body.rows);
    rows.sort(function (left, right) {
      var leftCell = left.cells[column];
      var rightCell = right.cells[column];
      var leftKey = leftCell ? leftCell.getAttribute("data-sort") : null;
      var rightKey = rightCell ? rightCell.getAttribute("data-sort") : null;
      var result;
      if (leftKey !== null && rightKey !== null) { result = parseFloat(leftKey) - parseFloat(rightKey); }
      else if (leftKey !== null) { result = -1; }
      else if (rightKey !== null) { result = 1; }
      else { result = (leftCell ? leftCell.textContent : "").localeCompare(rightCell ? rightCell.textContent : "", "ko"); }
      return ascending ? result : -result;
    });
    rows.forEach(function (row) { body.appendChild(row); });
  }

  document.querySelectorAll("table.sortable thead th").forEach(function (header) {
    header.addEventListener("click", function () { sortTable(header); });
  });

  var root = document.documentElement;
  var button = document.querySelector(".theme-toggle");
  try {
    var saved = localStorage.getItem("test-ledger-theme");
    if (saved === "dark" || saved === "light") { root.setAttribute("data-theme", saved); }
  } catch (error) {}

  function isDark() {
    var current = root.getAttribute("data-theme");
    return current ? current === "dark" : window.matchMedia("(prefers-color-scheme: dark)").matches;
  }

  function label() {
    if (button) { button.textContent = isDark() ? "밝게" : "어둡게"; }
  }

  label();
  if (button) {
    button.addEventListener("click", function () {
      var next = isDark() ? "light" : "dark";
      root.setAttribute("data-theme", next);
      try { localStorage.setItem("test-ledger-theme", next); } catch (error) {}
      label();
    });
  }
})();
"""

GLOSSARY = [
    ("t", "평균 차이가 우연인지 보는 값으로, 0에서 멀수록(대략 2–3 이상) 우연일 가능성이 낮고, 같은 달 매매는 한 묶음으로 세어 부풀지 않게 했다."),
    ("최대 낙폭", "누적 손익이 그때까지의 최고점에서 가장 많이 내려간 폭이다."),
    ("낙폭 개선", "바꾼 방식의 최대 낙폭에서 그대로 둔 방식의 최대 낙폭을 뺀 값으로, 양수면 덜 빠졌다는 뜻이다."),
    ("시장 대비 초과", "같은 날 같은 거름을 통과한 전 종목을 사서 같은 날 판 평균보다 얼마나 더 벌었는지다."),
    ("해마다 그 전 자료로 골라 쓰기", "그해 1월 1일 전에 끝난 매매만 보고 가장 나은 규칙을 골라 그해에 쓰는 방식으로, 결과를 보고 고르는 실수를 막는다."),
    ("금액만 줄인 경우", "규칙 없이 매수 금액만 줄여 같은 최대 낙폭을 맞췄을 때의 손익으로, 규칙이 이보다 나아야 고른 보람이 있다."),
    ("무작위 95% 선", "같은 수의 매매를 무작위로 빼는 일을 여러 번 했을 때 낙폭 개선의 상위 5% 경계다."),
    ("넓은 표본", "263건만으로는 적어서 조건을 조금 넓혀 모은 수천 건의 매매로 같은 효과를 다시 본 것이다."),
    ("저점 확인 k일", "어떤 날의 저가 뒤 k일 동안 더 낮은 값이 없어야 그날을 저점으로 인정한다는 뜻이다."),
    ("MAE / MFE", "보유 중 산 값보다 가장 깊이 빠진 폭(MAE)과 가장 높이 오른 폭(MFE)이다."),
]


def overview_rows(entries: list[dict]) -> list[list]:
    rows = []

    for entry in entries:
        info = SECTION_INDEX[entry["identifier"]]
        text, kind = info["answer"]
        rows.append([markup_cell(f'<a href="#{entry["identifier"]}">{escape(info["title"])}</a>', "label"), markup_cell(badge(text, kind), "verdict-cell"),
                     markup_cell(entry["compare"]), text_cell(entry["why"])])

    return rows


def compare_markup(pairs: list[tuple[str, float, str]]) -> str:
    """[(이름, 값, 단위)] → '그대로 +4,095만 / 멈추면 +3,247만', 값마다 색."""
    parts = []

    for name, value, unit in pairs:
        text = money(value) if unit == "만" else show(value, 2, suffix=unit)
        parts.append(f'{escape(name)} <span class="{tone(value)}">{escape(text)}</span>')

    return '<span class="num-inline">' + " / ".join(parts) + "</span>"


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8")
    halt_data = halt_compute()
    first_ledger, second_ledger = halt_ledgers(halt_data)
    t1_markup, t23_markup = halt_sections(halt_data, first_ledger, second_ledger)
    validate_data = validate_compute()
    validate_book = validate_ledger(validate_data)
    validate_markup = validate_section(validate_data, validate_book)
    overnight_data = overnight_compute()
    overnight_book = overnight_ledger(overnight_data)
    overnight_markup = overnight_section(overnight_data, overnight_book)
    standalone_data = standalone_compute()
    standalone_book = standalone_ledger(standalone_data)
    standalone_markup = standalone_section(standalone_data, standalone_book)
    devscale_data = devscale_compute()
    devscale_book = devscale_ledger(devscale_data)
    devscale_markup = devscale_section(devscale_data, devscale_book)
    support_data = support_compute()
    support_book = support_ledger(support_data)
    support_markup = support_section(support_data, support_book)
    collect_discrepancies(halt_data, validate_data, overnight_data, devscale_data)

    rows = halt_data["rows"]
    base_row = halt_data["base_row"]
    engine_name = next(item["name"] for item in halt_data["items"] if item["family"] == "T1" and "개장 전" in item["name"] and "-7" in item["name"])
    engine = rows[engine_name]
    t23_passed = [item["name"] for item in halt_data["items"] if item["family"] != "T1" and rows[item["name"]]["passed"]]
    t23_pick = next((name for name in t23_passed if "120일" in name), t23_passed[0] if t23_passed else None)
    t23_t = "·".join(show(rows[name]["difference_t"]) for name in t23_passed)
    walk = validate_data["walk"]
    row_120 = next(row for row in validate_data["wide_rows"] if row["days"] == 120)
    strongest = max(abs(row["wide"]["t"]) for row in overnight_data["o1"] if np.isfinite(row["wide"]["t"]))
    up, down = standalone_data["cells"]["UP_k3_N10"], standalone_data["cells"]["DOWN_k3_N10"]
    down_better = sum(value[0] < 0 for value in standalone_data["differences"].values())
    devscale_cells = devscale_data["cells"]
    support_walk, shape, baseline = support_data["walk"], support_data["shape"], support_data["baseline"]
    pass_walk, _ = constant_of(SUPPORT_SCRIPT, "PASS_WALK_T")
    entries = [
        {"identifier": "t1", "ledger": first_ledger,
         "compare": compare_markup([("그대로", base_row["total"], "만"), ("멈추면", engine["total"], "만")]),
         "why": f"엔진 정지선에 걸리는 매매가 {base_row['n']}건 중 {engine['dropped_n']}건뿐이고, 최대 낙폭은 오히려 {money(base_row['drawdown'])} → {money(engine['drawdown'])}."},
        {"identifier": "t23", "ledger": second_ledger,
         "compare": compare_markup([("그대로", base_row["total"], "만")] + ([("멈추면", rows[t23_pick]["total"], "만")] if t23_pick else [])),
         "why": f"24칸 중 {len(t23_passed)}칸은 낙폭이 줄었지만, 뺀 매매가 더 나빴다는 근거가 약하다(차이 t {t23_t})."},
        {"identifier": "validate", "ledger": validate_book,
         "compare": compare_markup([("그대로", validate_data["base_total"], "만"), ("미리 골라 쓰면", walk["total"], "만")]),
         "why": f"결과를 보지 않고 해마다 고르면 총 손익이 {show((1 - walk['total'] / validate_data['base_total']) * 100, 0, sign=False)}% 줄고, 넓은 표본 t {show(row_120['t'])}."},
        {"identifier": "overnight", "ledger": overnight_book,
         "compare": compare_markup([("그대로", overnight_data["base_total"], "만"), ("거르면", overnight_data["walk_total"], "만"), ("금액만 줄이면", overnight_data["less"], "만")]),
         "why": f"지표 {len(overnight_data['o1'])}개 모두 문턱 미달(가장 큰 |t| {show(strongest, sign=False)}), 금액만 줄이는 쪽이 더 번다."},
        {"identifier": "standalone", "ledger": standalone_book,
         "compare": compare_markup([("상승 추세", up["excess"], "%p"), ("하향 추세", down["excess"], "%p")]),
         "why": f"같은 모양을 하향 추세에서 산 쪽이 {len(standalone_data['differences'])}쌍 중 {down_better}쌍에서 더 좋아 '상승 추세' 덕이 아니다."},
        {"identifier": "devscale", "ledger": devscale_book,
         "compare": compare_markup([("그대로", devscale_cells["C0"]["total"], "만"), ("하향 빼면", devscale_cells["C1"]["total"], "만")]),
         "why": f"매매 수가 줄어 총 손실이 준 것뿐, 매매당 손실은 {percent(devscale_cells['C0']['mean'])} → {percent(devscale_cells['C1']['mean'])}로 그대로."},
        {"identifier": "support", "ledger": support_book,
         "compare": compare_markup([("모양 있음", shape["total_return"], "%"), ("아무 종목", baseline["total_return"], "%")]),
         "why": f"매매당 {percent(support_walk['net'])}로 조금 남지만 t {show(support_walk['t_net'])}로 문턱 {pass_walk} 미달, 굴리면 손실."},
    ]
    all_checks = sum(entry["ledger"].total for entry in entries)
    all_matched = sum(entry["ledger"].matched_count for entry in entries)
    mismatched = all_checks - all_matched
    check_class = "ok" if mismatched == 0 else "bad"
    glossary = "".join(f"<dt>{escape(term)}</dt><dd>{escape(meaning)}</dd>" for term, meaning in GLOSSARY)
    toc = "".join(f'<li><a href="#{entry["identifier"]}">{escape(SECTION_INDEX[entry["identifier"]]["title"].split(". ", 1)[1])}</a></li>'
                  for entry in entries)
    discrepancy_rows = [[item["test"], item["description"], markup_cell(f"<code>{escape(item['code'])}</code>"), markup_cell(f"<code>{escape(item['text'])}</code>")]
                        for item in DISCREPANCIES]
    discrepancy_table = render_table(["시험", "무엇이 다른가", "코드 위치", "설명 위치"], discrepancy_rows)
    overview = render_table(["질문", "답", "비교", "왜"], overview_rows(entries), classes="overview", stacked=True)
    page = f"""<title>시험 원장</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=IBM+Plex+Mono:wght@400;500&family=Noto+Sans+KR:wght@400;600;700&display=swap" rel="stylesheet">
<style>{STYLE}</style>
<main>
<header class="page-head"><div><p class="meta"><a href="https://claude.ai/artifact/Ahgj1CbDYc7aPfkujiQRQR#studies">← 스터디 목록</a></p><h1>시험 원장</h1>
<p class="check-line {check_class}">모든 숫자 {all_checks:,}개를 원장(parquet)에서 다시 계산해 맞춤, 불일치 {mismatched:,}</p>
<p class="meta">2026-10-06–10-07에 돌린 시험 7개. 돈은 만 원, 비율은 %. 생성기 <code>{escape(relative(Path(__file__).resolve()))}</code></p></div>
<button class="theme-toggle" type="button">어둡게</button></header>
<section class="summary" id="overview">
<h2>한눈에</h2>
{overview}
<details class="glossary"><summary>용어 풀이</summary><dl>{glossary}</dl></details>
</section>
<nav class="toc"><b>질문 목록</b><ol>{toc}<li><a href="#discrepancies">코드와 설명이 어긋난 곳</a></li></ol></nav>
{t1_markup}{t23_markup}{validate_markup}{overnight_markup}{standalone_markup}{devscale_markup}{support_markup}
<section id="discrepancies" class="closing"><h2>코드와 설명이 어긋난 곳</h2>
<p>각 시험의 코드와 txt·계획서 문장을 이 생성기가 다시 읽어, 지금도 서로 다른 곳만 적었다.</p>{discrepancy_table}
<p class="meta">다시 만들기: 저장소 루트에서 <code>py -X utf8 research/studies/build_test_ledger.py</code></p></section>
</main>
<script>{SCRIPT}</script>
"""
    OUTPUT.write_bytes(page.replace("\r\n", "\n").encode("utf-8"))
    print(f"{relative(OUTPUT)} {OUTPUT.stat().st_size / 1e6:.2f}MB, 대조 {all_matched}/{all_checks} 일치")

    for entry in entries:
        for row in entry["ledger"].mismatches:
            print(f"  불일치 [{entry['identifier']}] {row['label']}: 계산 {row['computed']} / txt {row['reported']} ({row['source']})")

    print(f"코드·설명 어긋남 {len(DISCREPANCIES)}건")
    return 0


if __name__ == "__main__":
    sys.exit(main())
