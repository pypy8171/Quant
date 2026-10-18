"""M1(200일선 추세 규칙)을 미국·한국 지수에 같은 기간으로 다시 돌린다 (2026-10-17).

README의 1차 지표는 미국 1928–2026, 한국 1996–2026으로 기간이 달라 비교가 공정하지 않았다.
여기서는 평가 구간을 코스피 쪽에 맞춘다.
  - 코스피: 네이버 siseJson 일봉(1990-01-04부터, 가격 지수라 배당 제외). 받은 값은 kospi_naver_daily.csv에 남긴다.
  - S&P500: IndexSource ^GSPC(가격 지수, 배당 제외). 1980년부터 받아 평가 시작일에 이미 신호가 있게 한다.
  - 신호: 전날 종가까지로 계산한 200일선·20일 기울기(backtest_crisis_response.expo_m1_trend 그대로).
  - 평가 시작: 코스피에서 200일선과 기울기가 처음 계산되는 날. 그날 종가에 두 지수를 1,000만 원어치 샀다고 둔다.
    규칙 쪽도 그날 종가에 산 상태에서 출발하고, 그날 종가 판단부터 규칙을 따른다.
  - 평가 끝: 두 지수 공통 마지막 날.
  - 비용·체결은 원래 스크립트와 같다(종가 결정, 다음 날 적용, 왕복 0.21%/0.5%/1.0%).
실행: py research/studies/08_crisis_response/same_period_m1.py
"""
import csv
import json
import sys
import urllib.request
from pathlib import Path

import numpy as np

_HERE = Path(__file__).resolve()
sys.path.insert(0, str(_HERE.parent))

import backtest_crisis_response as base  # noqa: E402

COSTS = base.COST_GRID
MAIN_COST = 0.0021
START_MONEY = 10_000_000
KOSPI_CSV = _HERE.parent / "kospi_naver_daily.csv"
NAVER_URL = ("https://api.finance.naver.com/siseJson.naver?symbol=KOSPI&requestType=1"
             "&startTime=19900101&endTime={end}&timeframe=day")

# 위기·박스권 구간. 같은 날짜로 두 시장을 함께 잰다.
EPISODES = [
    ("1990–1992 한국 3저 붕괴 뒤 하락", "1990-11-01", "1992-08-31"),
    ("1997 외환위기", "1997-06-02", "1999-06-30"),
    ("2000 닷컴 붕괴", "2000-01-03", "2003-06-30"),
    ("2008 금융위기", "2007-08-01", "2010-06-30"),
    ("2011–2020 코스피 박스권", "2011-05-02", "2020-06-30"),
    ("2020 코로나", "2020-01-02", "2021-01-29"),
    ("2022 금리 인상 하락장", "2021-11-01", "2023-06-30"),
]


def load_kospi(end_date):
    if not KOSPI_CSV.exists() or KOSPI_CSV.read_text(encoding="utf-8").splitlines()[-1].split(",")[0] < end_date:
        url = NAVER_URL.format(end=end_date.replace("-", ""))
        text = urllib.request.urlopen(url, timeout=300).read().decode("euc-kr", errors="replace")
        rows = []
        for line in text.splitlines():
            line = line.strip().rstrip(",")
            if not line.startswith('["') or line.startswith('["날짜'):
                continue
            fields = json.loads(line.replace("'", '"'))
            day = fields[0]
            rows.append((f"{day[:4]}-{day[4:6]}-{day[6:]}", float(fields[4])))
        with KOSPI_CSV.open("w", newline="", encoding="utf-8") as handle:
            writer = csv.writer(handle)
            writer.writerow(["date", "close"])
            writer.writerows(rows)

    dates, closes = [], []
    with KOSPI_CSV.open(encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            if row["date"] <= end_date and float(row["close"]) > 0:
                dates.append(row["date"])
                closes.append(float(row["close"]))
    return dates, np.asarray(closes, dtype=float)


def slice_window(dates, first_date, last_date):
    start_index = next(index for index, day in enumerate(dates) if day >= first_date)
    end_index = max(index for index, day in enumerate(dates) if day <= last_date) + 1
    return start_index, end_index


def curve(closes, exposure, start_index, end_index, cost):
    window_closes = closes[start_index:end_index]
    returns = base.daily_returns(window_closes)
    net, equity = base.run_curve(window_closes, returns, exposure[start_index:end_index].copy(), cost)
    return net, equity


def drawdown_span(dates, equity):
    peak = np.maximum.accumulate(equity)
    drawdown = equity / peak - 1.0
    trough = int(np.argmin(drawdown))
    peak_index = int(np.argmax(equity[:trough + 1]))
    recover = next((index for index in range(trough, len(equity)) if equity[index] >= equity[peak_index]), None)
    return dict(peak=dates[peak_index], trough=dates[trough],
                recovered=dates[recover] if recover is not None else None, mdd=float(drawdown.min()) * 100.0)


def window_statistics(dates, closes, exposure, start_index, end_index, cost):
    net, equity = curve(closes, exposure, start_index, end_index, cost)
    statistics = base.curve_stats(net, equity)
    statistics["in_market_pct"] = float(np.mean(exposure[start_index:end_index - 1])) * 100.0
    statistics["toggles"] = base.turnover_toggles(exposure[start_index:end_index])
    statistics["final_money"] = START_MONEY * float(equity[-1])
    statistics["worst"] = drawdown_span(dates[start_index:end_index], equity)
    return statistics


def trades(dates, exposure, start_index, end_index):
    """규칙의 매수·매도일(판단한 날 종가 기준, 실제 반영은 다음 거래일)."""
    events = []
    previous = 1.0
    for index in range(start_index, end_index):
        if exposure[index] != previous:
            events.append((dates[index], "매수" if exposure[index] > previous else "매도"))
            previous = exposure[index]
    return events


def episode_statistics(dates, closes, exposure, first_date, last_date):
    start_index, end_index = slice_window(dates, first_date, last_date)
    _, hold = curve(closes, np.ones(len(closes)), start_index, end_index, 0.0)
    _, rule = curve(closes, exposure, start_index, end_index, MAIN_COST)
    return dict(index_return=(hold[-1] - 1.0) * 100.0, index_mdd=base.max_drawdown(hold),
                rule_return=(rule[-1] - 1.0) * 100.0, rule_mdd=base.max_drawdown(rule),
                toggles=base.turnover_toggles(exposure[start_index:end_index]),
                in_market_pct=float(np.mean(exposure[start_index:end_index - 1])) * 100.0)


def main():
    end = base.as_of().isoformat()
    us_dates, us_closes = base.load_series(base.IndexSource(), "^GSPC", "1980-01-01", end)
    kr_dates, kr_closes = load_kospi(end)
    series = {"US": (us_dates, us_closes), "KR": (kr_dates, kr_closes)}
    exposures = {market: base.expo_m1_trend(closes, None, None, {}) for market, (_, closes) in series.items()}

    first_signal_index = base.MA_LONG + base.MA_SLOPE
    common = sorted(set(kr_dates[first_signal_index:]) & set(us_dates))
    first_date, last_date = common[0], common[-1]

    result = dict(window=[first_date, last_date], kr_data_start=kr_dates[0], start_money=START_MONEY, markets={})
    for market, (dates, closes) in series.items():
        start_index, end_index = slice_window(dates, first_date, last_date)
        exposure = exposures[market]
        result["markets"][market] = dict(
            start_close=float(closes[start_index]), end_close=float(closes[end_index - 1]),
            buy_and_hold=window_statistics(dates, closes, np.ones(len(closes)), start_index, end_index, 0.0),
            m1={str(cost): window_statistics(dates, closes, exposure, start_index, end_index, cost) for cost in COSTS},
            trades=trades(dates, exposure, start_index, end_index),
            episodes={name: episode_statistics(dates, closes, exposure, start, stop) for name, start, stop in EPISODES},
        )

    out_path = _HERE.parent / "same_period_m1.json"
    out_path.write_text(json.dumps(result, ensure_ascii=False, indent=1), encoding="utf-8")

    print(f"평가 구간 {first_date} – {last_date} (코스피 데이터 시작 {kr_dates[0]}), 시작 금액 {START_MONEY:,}원")
    for market, block in result["markets"].items():
        bh = block["buy_and_hold"]
        print(f"\n[{market}] 지수 {block['start_close']:.2f} → {block['end_close']:.2f}")
        print(f"  매수보유 | 최종 {bh['final_money']:,.0f}원 | 연 {bh['cagr']:.2f}% | MDD {bh['mdd']:.1f}% "
              f"{bh['worst']} | 샤프 {bh['sharpe']:.2f} | Calmar {bh['calmar']:.2f}")
        for cost, cost_statistics in block["m1"].items():
            print(f"  M1 비용{cost} | 최종 {cost_statistics['final_money']:,.0f}원 | 연 {cost_statistics['cagr']:.2f}% | MDD {cost_statistics['mdd']:.1f}% "
                  f"{cost_statistics['worst']} | 샤프 {cost_statistics['sharpe']:.2f} | Calmar {cost_statistics['calmar']:.2f} | 보유 {cost_statistics['in_market_pct']:.0f}% "
                  f"| 전환 {cost_statistics['toggles']}")
        print(f"  첫 매매 5건: {block['trades'][:5]}")
        for name, episode in block["episodes"].items():
            print(f"  {name}: 지수 {episode['index_return']:+.1f}% (MDD {episode['index_mdd']:.1f}) | 규칙 {episode['rule_return']:+.1f}% "
                  f"(MDD {episode['rule_mdd']:.1f}) | 전환 {episode['toggles']} | 보유 {episode['in_market_pct']:.0f}%")


if __name__ == "__main__":
    main()
