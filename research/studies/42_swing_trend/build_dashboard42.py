"""스터디 42 대시보드 데이터 — 고점·저점 추세 매매 건 전부, 조건별 다섯 묶음, 거름 단계를 대시보드 파일로 펼친다.

산출(research/studies/38_trend_stop_intraday/dashboard/data/ 아래, gzip + base64 텍스트):
  swing42.txt          거름 단계표(filter_steps.csv), 다섯 묶음표(buckets.csv), 규칙 목록, 거름 4개 정의, bars40·bars42 일봉 묶음 위치
  swing42/<규칙>.txt   그 규칙의 매매 건 전부(자료 오류 건 포함, data_error 로 표시). 건마다 거름 4개 통과 여부(passed)와
                       걸린 거름 비트(fails, 비트 k = filter_rule.STEPS[k] 를 못 넘음)
  bars42/b<n>.txt      스터디 38(bars/)·40(bars40/) 일봉 묶음에 없는 종목만, 같은 형식
사용: py research/studies/42_swing_trend/build_dashboard42.py  (swing_trend.py · feature_buckets.py · filter_rule.py 다음)
"""
import base64
import gzip
import json
import sys
from pathlib import Path

import numpy as np
import pandas as pd

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "38_trend_stop_intraday"))
sys.path.insert(0, str(HERE))

import build_dashboard as board  # noqa: E402
import daily_stop_grid as grid  # noqa: E402
from filter_rule import STEPS  # noqa: E402

ADOPTED = 4  # filter_rule.STEPS 앞 4개를 채택했다(42-3)
RULES = ["w20_retest", "w20_breakout", "w10_retest", "w10_breakout"]
KEEP = ["ticker", "signal_t", "entry_t", "entry_price", "exit_t", "exit_price", "high1", "high2", "low1", "low2", "high1_t",
        "stop", "net", "hold", "open", "data_error", "base_days", "base_depth", "low_rise", "high_rise", "year_high", "run20",
        "stop_gap", "market_trend", "relative60", "above240", "atr_percent", "cap_decile", "turnover20_decile"]


def read_packed(name: str) -> dict:
    return json.loads(gzip.decompress(base64.b64decode((board.OUT / name).read_bytes())))


def write_missing_bars(panel: grid.Panel, tickers: set[int]) -> tuple[dict, dict]:
    """bars/·bars40/ 에 없는 종목만 bars42/ 에 같은 형식(전날 종가와의 차, Int32)으로 쓴다.

    돌려주는 것: (bars40/ 에 있는 매매 종목의 위치, bars42/ 에 새로 쓴 종목의 위치). 페이지가 split40.txt(10MB)를 읽지 않게 한다.
    """
    own, split = read_packed("meta.txt")["bundle_of"], read_packed("split40.txt")["bundle_of"]
    in_bars40 = {str(ticker): split[str(ticker)] for ticker in tickers if str(ticker) not in own and str(ticker) in split}
    missing = sorted(ticker for ticker in tickers if str(ticker) not in own and str(ticker) not in split)
    bundle_of = {}

    for bundle, start in enumerate(range(0, len(missing), board.BUNDLE_SIZE)):
        chunks, offset = [], 0

        for ticker in missing[start:start + board.BUNDLE_SIZE]:
            valid = np.flatnonzero(np.isfinite(panel.close[ticker]))
            first, last = int(valid[0]), int(valid[-1])
            rows = slice(first, last + 1)
            prices = np.stack([panel.open[ticker, rows], panel.high[ticker, rows], panel.low[ticker, rows],
                               panel.close[ticker, rows]], axis=1)
            base = pd.Series(panel.close[ticker, rows]).ffill().shift(1).to_numpy()
            base[0] = 0.0
            delta = np.where(np.isfinite(prices), np.round(prices - base[:, None]), board.MISSING).astype(np.int32)
            chunks.append(delta.ravel())
            bundle_of[str(ticker)] = [bundle, offset, first, last - first + 1]
            offset += delta.size

        board.write_packed(board.OUT / "bars42" / f"b{bundle}.txt", np.concatenate(chunks).astype("<i4").tobytes())

    print(f"  일봉 묶음 추가: 종목 {len(missing)}개")
    return in_bars40, bundle_of


def filter_bits(trades: pd.DataFrame) -> np.ndarray:
    """비트 k = 거름 k 를 못 넘음(값 없음도 못 넘음으로 본다, filter_rule.py 와 같다)."""
    bits = np.zeros(len(trades), np.int64)

    for bit, (_, condition) in enumerate(STEPS[:ADOPTED]):
        bits |= np.where(condition(trades).fillna(False).to_numpy(), 0, 1 << bit)

    return bits


def main() -> None:
    panel = grid.Panel()
    meta_dates = read_packed("meta.txt")["dates"]
    assert len(meta_dates) == len(panel.dates), "대시보드 날짜 축과 패널 날짜 수가 다르다"
    traded, rules = set(), []

    for rule in RULES:
        trades = pd.read_parquet(HERE / "data" / f"trades_{rule}.parquet")
        trades["fails"] = filter_bits(trades)
        trades["passed"] = trades["fails"] == 0
        traded |= set(trades["ticker"].tolist())
        size = board.write_json(f"swing42/{rule}.txt", board.clean(trades[KEEP + ["fails", "passed"]]), compress=True)
        clean = trades[~trades["data_error"]]
        rules.append({"rule": rule, "window": int(rule[1:3]), "mode": rule.split("_")[1], "trades": len(trades),
                      "data_error": int(trades["data_error"].sum()), "passed": int(clean["passed"].sum())})
        print(f"  {rule}: {len(trades):,}건(거름 통과 {clean['passed'].sum():,}) → {size / 1e6:.2f}MB", flush=True)

    payload = {"rules": rules, "filters": [label for label, _ in STEPS[:ADOPTED]], "all_steps": [label for label, _ in STEPS],
               "steps": board.clean(pd.read_csv(HERE / "data" / "filter_steps.csv")),
               "buckets": board.clean(pd.read_csv(HERE / "data" / "buckets.csv").fillna({"bucket": ""}))}
    payload["bars40_of"], payload["bars42_of"] = write_missing_bars(panel, traded)
    print(f"→ swing42.txt {board.write_json('swing42.txt', payload, compress=True) / 1e3:.0f}KB")


if __name__ == "__main__":
    main()
