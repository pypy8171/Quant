#!/usr/bin/env python3
"""청산 사유별 조건부 기대값 표 — research/studies/17_exit_ev.

모의 원장 trades_YYYYMMDD.csv의 매도 체결을 KIS 주문번호(odno)로 합쳐 청산 레그로 만들고, 전략 묶음 × 청산 사유 셀마다
N레그·N일·승률·평균손익·중앙값·기대값·수익률과 일 단위 블록 부트스트랩 95% 신뢰구간을 낸다.
사전등록(구간·분할·비용·판정 규칙)은 research/studies/17_exit_ev/README.md 1절이 정본이고, 이 파일은 그 표를 채운다.

    py scripts/exit_ev.py
    py scripts/exit_ev.py --ledger-dir Quant/build_win/logs --out research/studies/17_exit_ev
"""
from __future__ import annotations

import argparse
import csv
import random
import re
import statistics
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
if str(REPO / "PYQuant") not in sys.path:
    sys.path.insert(0, str(REPO / "PYQuant"))
from backtest.costs import LIVE  # noqa: E402
DEFAULT_LEDGER_DIR = REPO / "Quant" / "build_win" / "logs"
DEFAULT_OUT = REPO / "research" / "studies" / "17_exit_ev"

# 사전등록(README 1절) — realized_pnl 값은 09-08부터 채워져 있다. 분할 경계는 config 변경일(09-11 TRENDX stop_loss 2.5 도입).
FIRST_DAY = "20260908"
LAST_DAY = "20260918"
SEGMENT_A_LAST_DAY = "20260910"
BOOTSTRAP_ROUNDS = 2000
BOOTSTRAP_SEED = 20260919
MIN_DAYS_FOR_VERDICT = 3

# realized_pnl은 엔진이 매도 수수료 + 거래세를 뺀 값(OrderGate.cpp realized_pnl 계산). 매수 수수료는 안 뺐다.
#  요율은 backtest/costs.py LIVE(원장과 한 소스, 0.015% + 0.20% = 0.215%).
SELL_COST_RATE_IN_LEDGER = LIVE.sell_cost_rate
# 비용 감도는 원장 값에 **추가로** 빼는 비율 — 0(원장 그대로), 매수 수수료, 슬리피지 근사, 보수적.
EXTRA_COST_RATES = (0.0, LIVE.commission_rate, 0.003, 0.008)

# 청산 사유 문자열 → 범주. 숫자를 지운 뒤 앞에서부터 처음 맞는 것(seed-trail이 trail보다 앞).
REASON_CATEGORIES = (
    ("익절밴드", "익절밴드"),
    ("손절", "손절"),
    ("장 마감", "장마감"),
    ("존 이탈", "존이탈"),
    ("seed", "seed-trail"),
    ("본전탈출", "본전탈출"),
    ("trail", "trail"),
    ("교체 진입", "교체진입"),
    ("이전 세션", "이전세션"),
    ("먼지", "먼지정리"),
)
# 규칙 정의상 손익 부호가 정해지는 범주 — 판정 대상에서 뺀다(승률·평균손익 부호가 정보가 아니다).
SIGN_FIXED_CATEGORIES = {"익절밴드", "손절", "본전탈출"}
# 전략 판단이 아닌 묶음 — 표에 남기되 판정하지 않는다.
NON_STRATEGY_FAMILIES = {"DISPLACE", "ORPHAN", "UNLINKED", "STARTUP", "MANUAL", "TEST"}


@dataclass
class ExitLeg:
    day: str
    family: str
    ticker: str
    category: str
    quantity: int = 0
    sell_notional: float = 0.0
    cost_basis: float = 0.0
    pnl: float = 0.0
    rows: int = 0


@dataclass
class LoadResult:
    legs: list[ExitLeg] = field(default_factory=list)
    unknown_basis: dict[tuple[str, str], int] = field(default_factory=lambda: defaultdict(int))
    skipped_files: list[str] = field(default_factory=list)


def categorize(entry_reason: str) -> str:
    text = re.sub(r"[\d.,\-]+", "", entry_reason)
    for needle, category in REASON_CATEGORIES:
        if needle in text:
            return category

    return "기타"


def strategy_family(strategy: str) -> str:
    return strategy.split("_")[0] if strategy else "(없음)"


def leg_key(row: dict[str, str]) -> tuple[str, ...]:
    """한 주문의 부분 체결을 합치는 키. odno(KIS 주문번호)가 정본 — order_id는 프로세스 카운터라 재기동마다 겹친다.

    재기동 전후로 같은 odno의 체결이 다른 라벨(DISPLACE→ORPHAN)로 남는 경우가 있어 전략·사유도 키에 넣는다(레그가 둘로 갈린다).
    종목이 다르면 진짜 충돌이라 load_legs가 예외를 던진다.
    """
    odno = (row.get("odno") or "").strip()
    order = ("odno", odno) if odno and odno != "0000000000" else ("local", row["order_id"])
    return order + (strategy_family(row["strategy"]), categorize(row.get("entry_reason", "")))


def load_legs(ledger_dir: Path, first_day: str, last_day: str) -> LoadResult:
    """SELL FILL 행을 주문 단위로 합쳐 청산 레그로. realized_pnl이 비어 있거나 0인 매도는 평단 미상(재기동 뒤 미시드)이라 결측으로 센다."""
    result = LoadResult()
    for path in sorted(ledger_dir.glob("trades_*.csv")):
        day = path.stem.split("_")[1]
        if not (first_day <= day <= last_day):
            continue

        by_key: dict[tuple[str, ...], ExitLeg] = {}
        with path.open(encoding="utf-8", errors="replace", newline="") as handle:
            reader = csv.DictReader(handle)
            if "realized_pnl" not in (reader.fieldnames or []):
                result.skipped_files.append(path.name)
                continue

            for row in reader:
                if row["event"] != "FILL" or row["side"] != "SELL":
                    continue

                family = strategy_family(row["strategy"])
                category = categorize(row.get("entry_reason", ""))
                pnl_text = (row.get("realized_pnl") or "").strip()
                pnl = float(pnl_text) if pnl_text else 0.0
                if pnl == 0.0:
                    result.unknown_basis[(family, category)] += 1
                    continue

                quantity = int(float(row["fill_qty"] or 0))
                price = float(row["fill_price"] or 0.0)
                if quantity <= 0 or price <= 0.0:
                    continue

                notional = quantity * price
                # 원장 손익은 매도 비용 차감 후. 비용 전 손익으로 되돌린 뒤 평단 = 체결가 − 주당 비용 전 손익.
                gross_pnl = pnl + notional * SELL_COST_RATE_IN_LEDGER
                average_cost = price - gross_pnl / quantity
                key = leg_key(row)
                leg = by_key.get(key)
                if leg is None:
                    leg = ExitLeg(day=day, family=family, ticker=row["ticker"], category=category)
                    by_key[key] = leg
                elif leg.ticker != row["ticker"]:
                    raise RuntimeError(f"{path.name} {key}: 한 주문에 다른 종목이 섞임 {leg.ticker} vs {row['ticker']}")

                leg.quantity += quantity
                leg.sell_notional += notional
                leg.cost_basis += average_cost * quantity
                leg.pnl += pnl
                leg.rows += 1

        result.legs.extend(by_key.values())

    return result


def net_pnl(leg: ExitLeg, extra_cost_rate: float) -> float:
    return leg.pnl - leg.sell_notional * extra_cost_rate


NAN = float("nan")


def cell_stats(legs: list[ExitLeg], extra_cost_rate: float) -> dict[str, float]:
    pnls = [net_pnl(leg, extra_cost_rate) for leg in legs]
    wins = sum(1 for value in pnls if value > 0.0)
    total_cost = sum(leg.cost_basis for leg in legs)
    return {
        "n_legs": len(legs),
        "n_days": len({leg.day for leg in legs}),
        "win_rate": wins / len(pnls) if pnls else 0.0,
        "mean_pnl": statistics.fmean(pnls) if pnls else 0.0,
        "median_pnl": statistics.median(pnls) if pnls else 0.0,
        "total_pnl": sum(pnls),
        "return_pct": (sum(pnls) / total_cost * 100.0) if total_cost > 0.0 else 0.0,
    }


def percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, int(round(fraction * (len(ordered) - 1)))))
    return ordered[index]


def day_block_bootstrap(legs: list[ExitLeg], extra_cost_rate: float, rounds: int, seed: int) -> dict[str, float]:
    """거래일을 복원추출해 승률·평균손익·수익률의 2.5/97.5 백분위. 같은 날 레그는 독립이 아니라 날 단위로 뽑는다.

    N일=2면 조합이 4개, 3이면 27개뿐이라 그 CI는 두세 날 값의 범위에 지나지 않는다(README 1절).
    """
    empty = {name: NAN for name in ("win_rate_low", "win_rate_high", "mean_pnl_low", "mean_pnl_high", "return_pct_low", "return_pct_high")}
    by_day: dict[str, list[ExitLeg]] = defaultdict(list)
    for leg in legs:
        by_day[leg.day].append(leg)

    days = sorted(by_day)
    if len(days) < 2 or rounds <= 0:
        return empty

    generator = random.Random(seed)
    win_rates: list[float] = []
    means: list[float] = []
    returns: list[float] = []
    for _ in range(rounds):
        sample: list[ExitLeg] = []
        for _ in days:
            sample.extend(by_day[generator.choice(days)])

        pnls = [net_pnl(leg, extra_cost_rate) for leg in sample]
        cost = sum(leg.cost_basis for leg in sample)
        win_rates.append(sum(1 for value in pnls if value > 0.0) / len(pnls))
        means.append(statistics.fmean(pnls))
        returns.append(sum(pnls) / cost * 100.0 if cost > 0.0 else 0.0)

    return {
        "win_rate_low": percentile(win_rates, 0.025),
        "win_rate_high": percentile(win_rates, 0.975),
        "mean_pnl_low": percentile(means, 0.025),
        "mean_pnl_high": percentile(means, 0.975),
        "return_pct_low": percentile(returns, 0.025),
        "return_pct_high": percentile(returns, 0.975),
    }


def verdict(family: str, category: str, segment_a: dict[str, float] | None, segment_b: dict[str, float] | None) -> str:
    """README 판정 규칙 — 두 구간 모두 수익률 CI가 0 아래면 음수, 둘 다 위면 양수, 아니면 보류. 부호 고정 범주·비전략 묶음은 판정 안 함."""
    if category in SIGN_FIXED_CATEGORIES:
        return "판정 제외(부호 고정)"

    if family in NON_STRATEGY_FAMILIES:
        return "판정 제외(전략 아님)"

    if segment_a is None or segment_b is None:
        return "판정불가(한쪽 구간 없음)"

    if segment_a["n_days"] < MIN_DAYS_FOR_VERDICT or segment_b["n_days"] < MIN_DAYS_FOR_VERDICT:
        return "판정불가(N일<3)"

    both_negative = segment_a["return_pct_high"] < 0.0 and segment_b["return_pct_high"] < 0.0
    both_positive = segment_a["return_pct_low"] > 0.0 and segment_b["return_pct_low"] > 0.0
    if both_negative:
        return "기대값 음수"

    if both_positive:
        return "기대값 양수"

    return "보류"


Table = dict[tuple[str, str], dict[str, float]]


def build_table(legs: list[ExitLeg], extra_cost_rate: float, rounds: int, seed: int) -> Table:
    cells: dict[tuple[str, str], list[ExitLeg]] = defaultdict(list)
    for leg in legs:
        cells[(leg.family, leg.category)].append(leg)

    table: Table = {}
    for key, cell_legs in cells.items():
        row = cell_stats(cell_legs, extra_cost_rate)
        row.update(day_block_bootstrap(cell_legs, extra_cost_rate, rounds, seed))
        table[key] = row

    return table


def sorted_cells(table: Table):
    return sorted(table.items(), key=lambda item: (-item[1]["n_legs"], item[0]))


TSV_COLUMNS = (
    "family", "category", "n_legs", "n_days", "win_rate", "win_rate_low", "win_rate_high",
    "mean_pnl", "mean_pnl_low", "mean_pnl_high", "median_pnl", "total_pnl",
    "return_pct", "return_pct_low", "return_pct_high",
)


def format_value(value: float) -> str:
    if isinstance(value, int):
        return str(value)

    if value != value:  # nan
        return ""

    return f"{value:.4f}" if abs(value) < 10.0 else f"{value:.1f}"


def write_tsv(path: Path, table: Table) -> None:
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle, delimiter="\t")
        writer.writerow(TSV_COLUMNS)
        for (family, category), row in sorted_cells(table):
            writer.writerow([family, category] + [format_value(row[column]) for column in TSV_COLUMNS[2:]])


def format_ci(low: float, high: float, scale: float = 1.0, digits: int = 0) -> str:
    if low != low:
        return "—"

    return f"[{low * scale:,.{digits}f}, {high * scale:,.{digits}f}]"


def stats_line(row: dict[str, float]) -> str:
    return (
        f"{row['n_legs']} | {row['n_days']} | {row['win_rate'] * 100:.1f}% | "
        f"{format_ci(row['win_rate_low'], row['win_rate_high'], 100.0, 0)} | {row['mean_pnl']:,.0f} | "
        f"{format_ci(row['mean_pnl_low'], row['mean_pnl_high'])} | {row['median_pnl']:,.0f} | {row['total_pnl']:,.0f} | "
        f"{row['return_pct']:.2f} | {format_ci(row['return_pct_low'], row['return_pct_high'], 1.0, 2)}"
    )


STATS_HEADER = "N레그 | N일 | 승률 | 승률 CI | 평균손익(원) | 평균손익 CI | 중앙값 | 합계 | 수익률% | 수익률 CI"
STATS_ALIGN = "---:|---:|---:|---|---:|---|---:|---:|---:|---"


def write_result_markdown(path: Path, tables: dict[str, Table], cost_tables: dict[float, Table],
                          loaded: LoadResult, ledger_dir: Path, segment_a_last_day: str) -> None:
    legs = loaded.legs
    all_table = tables["all"]
    days = sorted({leg.day for leg in legs})
    unknown_total = sum(loaded.unknown_basis.values())
    lines = [
        "# 17. 결과 — 청산 사유별 조건부 기대값",
        "",
        f"> 생성 `scripts/exit_ev.py`. 원장 `{ledger_dir.relative_to(REPO).as_posix()}` {days[0]}~{days[-1]} {len(days)}거래일, "
        f"청산 레그 {len(legs)}건(KIS 주문번호 단위), 평단 미상으로 뺀 매도 체결 {unknown_total}행.",
        f"> 손익은 원장 `realized_pnl` 그대로 = 매도 수수료·거래세 {SELL_COST_RATE_IN_LEDGER * 100:.3f}% 차감 후, 매수 수수료 전. "
        f"CI = 일 단위 블록 부트스트랩 {BOOTSTRAP_ROUNDS}회(seed {BOOTSTRAP_SEED}) 95%. 판정 규칙은 README 1절.",
        "",
        "## 0. 전략 묶음 합계 — 청산 규칙을 다 합친 결과",
        "",
        f"| 전략 | {STATS_HEADER} |",
        f"|---|{STATS_ALIGN}|",
    ]
    by_family: dict[str, list[ExitLeg]] = defaultdict(list)
    for leg in legs:
        by_family[leg.family].append(leg)

    for family, family_legs in sorted(by_family.items(), key=lambda item: -len(item[1])):
        row = cell_stats(family_legs, 0.0)
        row.update(day_block_bootstrap(family_legs, 0.0, BOOTSTRAP_ROUNDS, BOOTSTRAP_SEED))
        lines.append(f"| {family} | {stats_line(row)} |")

    lines += [
        "",
        "## 1. 전체",
        "",
        f"| 전략 | 청산 사유 | {STATS_HEADER} | 평단 미상 | 판정(A→B) |",
        f"|---|---|{STATS_ALIGN}|---:|---|",
    ]
    for (family, category), row in sorted_cells(all_table):
        lines.append(
            f"| {family} | {category} | {stats_line(row)} | {loaded.unknown_basis.get((family, category), 0)} | "
            f"{verdict(family, category, tables['segment_a'].get((family, category)), tables['segment_b'].get((family, category)))} |"
        )

    leftover = {key: count for key, count in loaded.unknown_basis.items() if key not in all_table}
    if leftover:
        lines += ["", "레그 없이 평단 미상만 있는 셀: " + ", ".join(f"{family} {category} {count}행" for (family, category), count in sorted(leftover.items()))]

    for label, title in (("segment_a", f"2a. 구간 A — {days[0]}~{segment_a_last_day} (09-11 TRENDX stop_loss 2.5 도입 전)"),
                         ("segment_b", f"2b. 구간 B — {segment_a_last_day} 다음 날~{days[-1]} (도입 후)")):
        lines += ["", f"## {title}", "", f"| 전략 | 청산 사유 | {STATS_HEADER} |", f"|---|---|{STATS_ALIGN}|"]
        for (family, category), row in sorted_cells(tables[label]):
            lines.append(f"| {family} | {category} | {stats_line(row)} |")

    lines += [
        "",
        "## 3. 비용 감도 — 원장 손익에서 매도 명목 × 추가 비율을 더 뺀 평균손익(원/레그), 전체 기간",
        "",
        "| 전략 | 청산 사유 | N레그 | " + " | ".join(f"+{rate * 100:.3f}%" for rate in EXTRA_COST_RATES) + " |",
        "|---|---|---:|" + "---:|" * len(EXTRA_COST_RATES),
    ]
    for (family, category), row in sorted_cells(all_table):
        values = " | ".join(f"{cost_tables[rate][(family, category)]['mean_pnl']:,.0f}" for rate in EXTRA_COST_RATES)
        lines.append(f"| {family} | {category} | {row['n_legs']} | {values} |")

    lines += [
        "",
        "## 4. 읽는 법",
        "",
        "- 유효 표본은 레그 수가 아니라 **N일**이다. N일=2의 CI는 두 날 값의 범위, 3은 조합 27개뿐이다.",
        "- 익절밴드·손절·본전탈출은 규칙 정의상 손익 부호가 정해져 있어(익절은 이익, 손절은 손실, 본전탈출은 비용만큼 손실) "
        "승률·평균손익 부호가 정보가 아니다. 크기와 빈도만 읽고 판정에서 뺐다.",
        "- 판정은 구간 A·B의 수익률 CI가 같은 쪽에 있을 때만 난다. 구간 사이에 포지션 크기(`buy_split_steps`)가 바뀌어 원/레그는 단위가 다르므로 수익률로 판정한다. \"보류\"·\"판정불가\"가 정상 결과다.",
        "- DISPLACE·ORPHAN·STARTUP·MANUAL은 슬롯 교체·세션 이월·점검·수동이라 참고만 한다. ORPHAN은 어느 전략의 이월분인지 원장으로 확정할 수 없다(README 2절).",
        "- 이 표는 스탑 **거리**를 말하지 않는다(가격 경로 없음). 어떤 청산 규칙이 돈을 잃는지까지만.",
        "",
    ]
    if loaded.skipped_files:
        lines += ["realized_pnl 열이 없어 건너뛴 원장: " + ", ".join(loaded.skipped_files), ""]

    path.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--ledger-dir", type=Path, default=DEFAULT_LEDGER_DIR)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--first-day", default=FIRST_DAY)
    parser.add_argument("--last-day", default=LAST_DAY)
    parser.add_argument("--segment-a-last-day", default=SEGMENT_A_LAST_DAY)
    parser.add_argument("--rounds", type=int, default=BOOTSTRAP_ROUNDS)
    arguments = parser.parse_args()

    loaded = load_legs(arguments.ledger_dir, arguments.first_day, arguments.last_day)
    legs = loaded.legs
    if not legs:
        print("청산 레그가 없다 — 원장 경로·기간을 확인", file=sys.stderr)
        return 1

    segment_a = [leg for leg in legs if leg.day <= arguments.segment_a_last_day]
    segment_b = [leg for leg in legs if leg.day > arguments.segment_a_last_day]
    tables = {
        "all": build_table(legs, 0.0, arguments.rounds, BOOTSTRAP_SEED),
        "segment_a": build_table(segment_a, 0.0, arguments.rounds, BOOTSTRAP_SEED),
        "segment_b": build_table(segment_b, 0.0, arguments.rounds, BOOTSTRAP_SEED),
    }
    cost_tables = {rate: build_table(legs, rate, 0, BOOTSTRAP_SEED) for rate in EXTRA_COST_RATES}

    arguments.out.mkdir(parents=True, exist_ok=True)
    for label, table in tables.items():
        write_tsv(arguments.out / f"exit_ev_{label}.tsv", table)

    write_result_markdown(arguments.out / "RESULT.md", tables, cost_tables, loaded, arguments.ledger_dir, arguments.segment_a_last_day)
    print(f"청산 레그 {len(legs)}건(구간 A {len(segment_a)}·B {len(segment_b)}), 평단 미상 {sum(loaded.unknown_basis.values())}행 "
          f"→ {arguments.out.relative_to(REPO).as_posix()}/RESULT.md")
    return 0


if __name__ == "__main__":
    sys.exit(main())
