#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""장중 자동매매가 쓰는 수치·주기를 한 장으로 모은다 → _private/TUNING_SHEET.md (gitignore, 통째로 생성물).

두 곳에서 읽는다.
  1. 실행 중인 config(감시견 상태 파일 _private/_auto_trade_day.json 의 config, 없으면 Quant/config/config_dev_paper.json).
     키·값·`//` 주석을 그대로 싣는다. 비밀 키(앱키·계좌)는 뺀다.
  2. 코드에 박힌 상수 — docs/tuning_sheet.toml 의 [[anchor]] 가 파일·정규식으로 가리킨다. 값은 소스에서 그때그때 뽑으므로
     코드를 고치면 시트도 따라온다. 정규식이 못 잡으면 --check 가 exit 1 로 막는다(턴 끝 sync-gate · 커밋 docs-gate).

시트를 손으로 고치지 않는다. 수치를 바꾸려면 시트의 "어디서" 칸이 가리키는 config 키나 파일:줄을 고친다.

사용:
    py scripts/gen_tuning_sheet.py                 # 생성(= --apply)
    py scripts/gen_tuning_sheet.py --check         # 시트가 낡았거나 앵커가 깨졌으면 exit 1
    py scripts/gen_tuning_sheet.py --config <경로>  # 다른 config 기준으로
"""
from __future__ import annotations

import fnmatch
import json
import re
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = ROOT / "docs" / "tuning_sheet.toml"
OUT = ROOT / "_private" / "TUNING_SHEET.md"
STATUS = ROOT / "_private" / "_auto_trade_day.json"
DEFAULT_CONFIG = "Quant/config/config_dev_paper.json"

# 시트에 절대 싣지 않는 키(값이 실키·계좌). 접두 일치.
SECRET_PREFIXES = ("kis.", "quote_kis.", "startup_probe.")
SECRET_WORDS = ("app_key", "app_secret", "account", "hts_id", "token")

# 키 접미사 → 단위. spec 의 [[config_key]] 가 있으면 그쪽이 우선.
UNIT_BY_SUFFIX = [
    ("_per_day", "회/일"), ("_per_sec", "회/초"), ("_per_min", "회/분"),
    ("_sec", "초"), ("_ms", "ms"), ("_hhmm", "hhmm"), ("_min", "분"), ("_pct", "%"), ("_krw", "원"),
    ("_z", "z"), ("_bars", "봉"), ("_ticks", "틱"), ("_n", "개"), ("_qty", "주"), ("_max", "개"),
    ("_retries", "회"), ("_rungs", "단"),
    ("_limit", "원"), ("_period", "봉"), ("_lookback", "봉"), ("_equity", "원"), ("_turnover", "원"),
    ("_positions", "개"), ("_price", "원"), ("_universe", "개"), ("_port", "포트"),
]
# 주기 표에 올리는 단위와 초 환산 배수
PERIOD_UNITS = {"초": 1.0, "ms": 0.001, "분": 60.0}

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass


def rel(path: Path) -> str:
    return path.relative_to(ROOT).as_posix()


# ----------------------------- config -----------------------------

def running_config_path(argv: list[str]) -> Path:
    if "--config" in argv:
        return ROOT / argv[argv.index("--config") + 1]
    if STATUS.exists():
        try:
            status = json.loads(STATUS.read_text(encoding="utf-8-sig"))
            candidate = ROOT / str(status.get("config", "")).replace("\\", "/")
            if candidate.is_file():
                return candidate
        except (json.JSONDecodeError, OSError):
            pass
    return ROOT / DEFAULT_CONFIG


def is_secret(key: str) -> bool:
    return key.startswith(SECRET_PREFIXES) or any(word in key for word in SECRET_WORDS)


def flatten(node, prefix: str, rows: list[dict], comments: dict[str, str]) -> None:
    """dict 를 `a.b.c` 키로 편다. `//` 로 시작하는 키는 값이 아니라 주석이라 comments 에 모은다."""
    if isinstance(node, dict):
        for key, value in node.items():
            full = f"{prefix}.{key}" if prefix else key
            if key.startswith("//"):
                comments[full] = str(value)
                continue
            flatten(value, full, rows, comments)
        return
    if isinstance(node, list):
        if node and all(isinstance(item, dict) for item in node):
            for index, item in enumerate(node):
                flatten(item, f"{prefix}[{index}]", rows, comments)
        else:
            rows.append({"key": prefix, "value": f"list[{len(node)}]", "raw": node})
        return
    rows.append({"key": prefix, "value": node, "raw": node})


def comment_for(key: str, comments: dict[str, str]) -> str:
    """`//키` 주석이 있으면 그것, 없으면 같은 층의 `//묶음` 주석 중 `키=` 를 담은 것."""
    parent, _, leaf = key.rpartition(".")
    exact = f"{parent}.//{leaf}" if parent else f"//{leaf}"
    if exact in comments:
        return comments[exact]
    best, fallback = "", ""
    # 한 층 위의 `//묶음` 주석(예: strategies[0].//manage_holdings)이 하위 키를 `키(설명)` 꼴로 적기도 한다.
    grand = parent.rpartition(".")[0]
    for name, text in comments.items():
        owner, _, tag = name.rpartition(".")
        if not tag.startswith("//"):
            continue
        if owner == grand and tag == "//" + parent.rpartition(".")[2]:
            match = re.search(rf"(?<!\w){re.escape(leaf)}\(([^()]*(?:\([^()]*\)[^()]*)*)\)", text)
            if match and len(match.group(1)) > len(best):
                best = match.group(1).strip()
            continue
        if owner != parent:
            continue
        # 묶음 주석("scan_top_n=…, value_top_n=…") 안에서 자기 키의 설명 조각만 뗀다. 여러 주석이 같은 키를 설명하면 긴 쪽.
        match = re.search(rf"(?<!\w){re.escape(leaf)}=(.*?)(?=[,.]\s+\w+=|$)", text)
        if match and len(match.group(1)) > len(best):
            best = match.group(1).strip().rstrip(".")
        elif not match and leaf in text and not fallback:
            fallback = f"(참고 `{tag}`) {text}"
    return best or fallback


def unit_for(key: str, overrides: list[dict]) -> str:
    leaf = key.rsplit(".", 1)[-1]
    for override in overrides:
        if fnmatch.fnmatch(key, override["key"]) or fnmatch.fnmatch(leaf, override["key"]):
            if override.get("unit"):
                return override["unit"]
    for suffix, unit in UNIT_BY_SUFFIX:
        if leaf.endswith(suffix):
            return unit
    return ""


def group_for(key: str, groups: list[dict]) -> str:
    """경로 패턴(`*.manage_holdings.*`·`risk.*`)이 마지막 조각 패턴(`rescan_*`)보다 먼저다 — 안 그러면 하위 묶음 키가 상위 묶음에 끌려간다."""
    leaf = key.rsplit(".", 1)[-1]
    for path_pass in (True, False):
        for group in groups:
            for pattern in group.get("keys", []):
                if ("." in pattern) != path_pass:
                    continue
                if fnmatch.fnmatch(key, pattern) or (not path_pass and fnmatch.fnmatch(leaf, pattern)):
                    return group["name"]
    return "기타"


def scope_label(key: str, data: dict) -> str:
    """`strategies[1].x` → `strategies[1] DEVIATION_SCALE(TRENDX)` 처럼 어느 슬리브의 값인지 보이게."""
    match = re.match(r"strategies\[(\d+)\]", key)
    if not match:
        parent = key.rpartition(".")[0]
        return parent or "최상위"
    index = int(match.group(1))
    sleeve = data["strategies"][index]
    label = f"strategies[{index}] {sleeve.get('type', '')}"
    if sleeve.get("id_prefix"):
        label += f"({sleeve['id_prefix']})"
    rest = key[match.end():].lstrip(".").rpartition(".")[0]
    return f"{label} {rest}".rstrip()


def config_rows(config_path: Path, spec: dict) -> tuple[list[dict], dict[str, str]]:
    data = json.loads(config_path.read_text(encoding="utf-8-sig"))
    flat: list[dict] = []
    comments: dict[str, str] = {}
    flatten(data, "", flat, comments)
    overrides = spec.get("config_key", [])
    groups = spec.get("config_group", [])
    rows = []
    for row in flat:
        key = row["key"]
        if is_secret(key):
            continue
        rows.append({
            "kind": "config",
            "key": key.rsplit(".", 1)[-1],
            "full_key": key,
            "value": row["value"],
            "unit": unit_for(key, overrides),
            "group": group_for(key, groups),
            "meaning": comment_for(key, comments),
            "where": f"config · {scope_label(key, data)}",
        })
    return rows, comments


# ----------------------------- 코드 앵커 -----------------------------

def config_override_label(config_key: str, config_values: dict[str, list]) -> str:
    """config_key 가 실행 중 config 에 있으면 그 값(들)을, 없으면 코드 기본값이 적용 중이라는 표시를 만든다."""
    values = config_values.get(config_key)

    if not values:
        return f"config `{config_key}` 없음 → **이 기본값이 적용 중**"

    shown = ", ".join(dict.fromkeys(fmt_value(value) for value in values))
    return f"config `{config_key}` 가 덮어씀: {shown}"


def anchor_rows(spec: dict, config_values: dict[str, list]) -> tuple[list[dict], list[str]]:
    """[[anchor]] 마다 파일을 열어 정규식 첫 그룹을 값으로 뽑는다. 실패는 errors 에 적는다.
    config_values 는 실행 중 config 의 {마지막 조각: [값…]} — config_key 가 있는 앵커에 덮어쓰기 여부를 붙인다."""
    rows, errors = [], []
    for anchor in spec.get("anchor", []):
        path = ROOT / anchor["file"]
        if not path.is_file():
            errors.append(f"{anchor['id']}: 파일 없음 {anchor['file']}")
            continue
        text = path.read_text(encoding="utf-8-sig", errors="replace")
        match = re.search(anchor["regex"], text, re.M)
        if not match:
            errors.append(f"{anchor['id']}: 정규식이 {anchor['file']} 에서 안 잡힘 — {anchor['regex']}")
            continue
        line = text.count("\n", 0, match.start()) + 1
        value = match.group(1) if match.groups() else match.group(0)
        rows.append({
            "kind": "code",
            "key": anchor["id"],
            "value": value,
            "unit": anchor.get("unit", ""),
            "group": anchor.get("group", "기타"),
            "meaning": anchor.get("meaning", ""),
            "where": f"[{anchor['file']}:{line}](../{anchor['file']}#L{line})",
            "config_key": anchor.get("config_key", ""),
            "config_override": config_override_label(anchor["config_key"], config_values) if anchor.get("config_key") else "",
        })
    return rows, errors


# ----------------------------- 렌더 -----------------------------

def fmt_value(value) -> str:
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, float) and value.is_integer() and abs(value) >= 1000:
        return f"{int(value):,}"
    if isinstance(value, int) and abs(value) >= 1000:
        return f"{value:,}"
    return str(value)


def to_seconds(row: dict) -> float | None:
    unit = row["unit"]
    if unit == "hhmm":
        return None
    factor = PERIOD_UNITS.get(unit)
    if factor is None:
        return None
    try:
        number = float(str(row["value"]).replace(",", ""))
    except ValueError:
        return None
    if number <= 0:
        return None
    return number * factor


def human_period(seconds: float) -> str:
    if seconds < 1:
        return f"{seconds * 1000:g}ms"
    if seconds < 60:
        return f"{seconds:g}초"
    if seconds < 3600:
        minutes = seconds / 60
        return f"{minutes:g}분"
    return f"{seconds / 3600:g}시간"


def cell(text) -> str:
    return str(text).replace("|", "\\|").replace("\n", " ")


def render(config_path: Path, rows: list[dict], errors: list[str], spec: dict) -> str:
    out: list[str] = []
    out.append("# 장중 매매 수치·주기 시트 (TUNING_SHEET)")
    out.append("")
    out.append(f"`py scripts/gen_tuning_sheet.py` 가 만든다 — **손으로 고치지 않는다.** 기준 config: `{rel(config_path)}`.")
    out.append("수치를 바꾸려면 \"어디서\" 칸이 가리키는 config 키나 파일:줄을 고친다. 고치면 턴 끝 sync-gate 가 이 시트를 다시 쓴다.")
    out.append("코드 상수의 목록(어느 파일의 어느 줄을 볼지)은 `docs/tuning_sheet.toml` 이 정본이다.")
    out.append("")
    if errors:
        out.append("## ⚠ 앵커 실패 — 코드가 바뀌어 값을 못 뽑았다. `docs/tuning_sheet.toml` 의 정규식을 고친다")
        out.append("")
        for error in errors:
            out.append(f"- {error}")
        out.append("")

    # 1. 주기 시간표
    out.append("## 1. 주기 한눈에 — 무엇이 몇 초마다 도는가")
    out.append("")
    out.append("초·ms·분 단위 값만 모아 짧은 것부터. 같은 주기끼리 묶여 보이므로 \"20초 것들끼리 맞춰야 하나\"를 여기서 본다.")
    out.append("")
    out.append("| 주기 | 이름 | 값 | 묶음 | 어디서 | 의미 |")
    out.append("|---|---|---|---|---|---|")
    periodic = [(to_seconds(row), row) for row in rows]
    periodic = [(seconds, row) for seconds, row in periodic if seconds is not None]
    for seconds, row in sorted(periodic, key=lambda item: (item[0], item[1]["group"], item[1]["key"])):
        out.append(f"| {human_period(seconds)} | `{cell(row['key'])}` | {fmt_value(row['value'])} {row['unit']} | {cell(row['group'])} | {row['where']} | {cell(row['meaning'])} |")
    out.append("")

    # 2. 시각(hhmm)
    clocks = [row for row in rows if row["unit"] == "hhmm"]
    if clocks:
        out.append("## 2. 시각 — 몇 시에 무엇이 일어나는가")
        out.append("")
        out.append("| 시각 | 이름 | 묶음 | 어디서 | 의미 |")
        out.append("|---|---|---|---|---|")
        for row in sorted(clocks, key=lambda item: str(item["value"]).zfill(4)):
            out.append(f"| {str(row['value']).zfill(4)} | `{cell(row['key'])}` | {cell(row['group'])} | {row['where']} | {cell(row['meaning'])} |")
        out.append("")

    # 3. 묶음별 전체
    out.append("## 3. 묶음별 전체 — 데이터 소스·엔진·리스크·전략")
    out.append("")
    order = [group["name"] for group in spec.get("config_group", [])]
    for group in spec.get("anchor_group", []):
        if group["name"] not in order:
            order.append(group["name"])
    if "기타" not in order:
        order.append("기타")
    notes = {group["name"]: group.get("note", "") for group in spec.get("config_group", []) + spec.get("anchor_group", [])}
    by_group: dict[str, list[dict]] = {}
    for row in rows:
        by_group.setdefault(row["group"], []).append(row)
    for name in order:
        members = by_group.get(name)
        if not members:
            continue
        out.append(f"### {name}")
        out.append("")
        if notes.get(name):
            out.append(notes[name])
            out.append("")
        out.append("| 이름 | 값 | 단위 | 어디서 | 의미 |")
        out.append("|---|---|---|---|---|")
        for row in members:
            where = row["where"]
            if row.get("config_key"):
                where += " · " + row["config_override"]
            out.append(f"| `{cell(row['key'])}` | {fmt_value(row['value'])} | {row['unit']} | {where} | {cell(row['meaning'])} |")
        out.append("")
    return "\n".join(out) + "\n"


# ----------------------------- main -----------------------------

def build(argv: list[str]) -> tuple[str, list[str]]:
    with open(SPEC, "rb") as handle:
        spec = tomllib.load(handle)
    config_path = running_config_path(argv)
    rows, _comments = config_rows(config_path, spec)
    config_values: dict[str, list] = {}

    for row in rows:
        config_values.setdefault(row["key"], []).append(row["value"])

    code_rows, errors = anchor_rows(spec, config_values)
    return render(config_path, rows + code_rows, errors, spec), errors


def main(argv: list[str]) -> int:
    text, errors = build(argv)
    if "--check" in argv:
        current = OUT.read_text(encoding="utf-8-sig") if OUT.exists() else ""
        if current.replace("\r\n", "\n") != text or errors:
            for error in errors:
                print(f"[anchor] {error}")
            print(f"{rel(OUT)}: 낡음 (py scripts/gen_tuning_sheet.py 로 재생성)")
            return 1
        print(f"{rel(OUT)}: 최신")
        return 0
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(text, encoding="utf-8", newline="\n")
    for error in errors:
        print(f"[anchor] {error}")
    print(f"[ok] wrote {rel(OUT)} ({len(errors)} 앵커 실패)")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
