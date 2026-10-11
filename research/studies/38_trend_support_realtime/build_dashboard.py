"""스터디 38 대시보드 — 회차별 남는 칸·참고 칸 매매 전부와 회차별 결과 표를 한 페이지로 본다(사용자 지시 2026-10-11).

산출(전부 dashboard/ 아래, gitignore. 페이지는 상대 경로 fetch 로 읽으므로 그 폴더에서 py -m http.server 로 연다):
  index.html            dashboard_index.html 복사(본문부터 — 발행 때 html·head·body 로 감싼다)
  dashboard_app.js      같은 이름 파일 복사
  data/meta.txt         날짜표·종목표·일봉 묶음 위치·칸 목록·문자열 사전·대조 결과
  data/rounds.txt       회차별 결과 표(results/roundN/ 의 json·csv 그대로)와 회차 요약
  data/trades/cN.txt    칸(회차 × cell_role × rule …) 하나의 매매, 열 단위 JSON. 종목·날짜·문자열은 meta 의 번호로 바꿔 담는다
  data/bars/bN.txt      종목 40 개씩 묶은 일봉(수정주가). Int32 [시가, 고가, 저가, 종가]를 전날 종가와의 차로 적는다.
                        meta.bundle_of[code] = [묶음 번호, 묶음 안 위치(Int32 칸), 첫 거래일 번호, 일수]. 빈 날은 MISSING.
모든 data 파일은 gzip 뒤 base64 텍스트다(아티팩트가 이진 파일·.gz 를 받지 않는다). 페이지는 atob → DecompressionStream 으로 푼다.

일봉은 시험과 같은 common.Panel(스터디 33 load_inputs, 거래량 0 인 날 제외)에서 꺼낸다. 이평은 페이지가 값 있는 날만으로 계산하므로
시험의 rolling_mean_known 과 같은 값이 나온다. 2010-01-01 첫 행보다 WARMUP_ROWS 행 앞에서 자른다(이평 200·ATR 60 이 2010 년부터 같게).
대조(meta.checks): 매수가 = 그날 종가, 지지 저점 값 = 그날 저가인지 매매마다 맞춰 본 개수.

재실행(저장소 루트): py research/studies/38_trend_support_realtime/build_dashboard.py
  [--trades <회차 폴더의 *_trades.csv ...>] [--rounds 3 4 5 6 7 8]
  매매 파일의 회차는 그 파일이 든 폴더 이름(roundN)으로 정해 칸에 붙인다. 8회차가 같은 형식으로 나오면
  --trades 에 그 경로를 더하고 --rounds 에 8 을 더한다. 회차 폴더의 csv·json 은 ROUND_TABLE_LIMIT 이하만 싣는다(매매 파일 제외).
"""
from __future__ import annotations

import argparse
import ast
import base64
import gzip
import json
import shutil
import sys
import time
from pathlib import Path

import numpy as np
import pandas as pd

sys.path.insert(0, str(Path(__file__).resolve().parent))
import common  # noqa: E402

STUDY = common.STUDY
RESULTS = STUDY / "results"
OUTPUT = STUDY / "dashboard"
BUNDLE_SIZE = 40
MISSING = -2147483648
WARMUP_ROWS = 210
DEFAULT_TRADES = [RESULTS / "round6" / "survivors_trades.csv", RESULTS / "round6" / "reference_cell_trades.csv",
                  RESULTS / "round7" / "survivors_trades.csv"]
DEFAULT_ROUNDS = [3, 4, 5, 6, 7, 8, 9, 10, 11]
ROUND_TABLE_LIMIT = 700_000
CELL_KEYS = ["source", "cell_role", "scheme", "rule", "turnover_bin", "order_amount_krw", "ratio_cap_percent"]
DICTIONARY_COLUMNS = ["exit_reason", "signal_k_list", "moving_average_windows", "atr_periods"]
ROUND_FILES = ["choices_log.csv", "cross_judgement.csv", "same_month_absolute_control.csv", "decile1_difference.csv",
               "multiple_comparison.json", "cell_table.csv", "diagnostics.csv", "judgement.csv", "judgement_by_amount.csv", "round5_fixed_judgement.csv",
               "period_control.csv", "regime_table.csv", "exit_diagnostics.csv", "mae_mfe_quantiles.csv", "defect_counts.csv"]

# 회차 요약 — 숫자는 results/roundN/ 의 표에서 옮겨 적었다(페이지에 원 표가 같이 실린다).
ROUND_SUMMARY = {
    3: {"test": "보정한 거래대금으로 10분위를 다시 매기고, 체결 제약(주문 금액 4개 × 주문 비율 상한 6개)을 축으로 넣고, "
                "칸 수에 맞춘 다중비교 기준선(월 묶음 부호 뒤집기 1,000회의 최대 t 95% 분위)을 처음 냈다.",
        "target": "보통주 전 종목, 신호일 2010-01-01 – 2026-10-08. 판정 표(k·이평 N·ATR 합본) 1,296칸. "
                  "가장 잘은 칸은 k·N·ATR까지 나눈 칸.",
        "criterion": "기준선 (가) 칸들의 최대 t 95% 분위(판정 표 층 3.91), (나) 자유도 보정 최소 확률 5% 분위.",
        "result": "판정 표 칸 104개가 (가)를 넘었다(분위 0이 52, 분위 1이 50, 분위 2가 2). 가장 잘은 칸은 (가)를 넘은 칸이 없다.",
        "versus": "2회차보다 거래대금 보정·체결 제약·다중비교 기준선이 새로 들어갔다. 이때 문턱·분위는 매수일 그날 거래대금이라 매수 전에 알 수 없는 값이 섞여 있었다(4회차에서 고침)."},
    4: {"test": "문턱·분위를 매수 전에 아는 값(전날까지 20일 평균·전날)으로 바꾼 6판, 동시호가 충격 비용 계수 {0, 0.1, 0.3, 1.0}, 종가 되튐 진단.",
        "target": "판 6개를 모은 판정 표 47,552칸.",
        "criterion": "판 4(문턱·분위 모두 전날까지 20일 평균)·계수 0.3·주문 1억 원·상한 10% 이하에서 (가)를 넘고, 옆 분위 부호 같고, "
                     "양수 연도 비율 0.7 이상, 평균 초과수익 > 0.",
        "result": "판정 영역에서 남는 칸 0개. 같은 판·계수에서 1억 원 칸의 가장 큰 t는 −0.37, 100만 원·상한 0.5% 칸이 5.66.",
        "versus": "3회차 104칸은 그날 거래대금(매수 전에 모르는 값)과 충격 비용 0에서 나온 것이었다. 매수 전 값과 충격 비용을 넣자 판정 영역에서 사라졌다."},
    5: {"test": "같은 매매를 k·N·ATR 묶음마다 여러 번 세던 것을 걷어냈다. P0 합본(4회차 재현), P1 같은 종목·같은 매수일 1건, P2 P1을 날짜로 묶음, "
                "K0–K2 k별. 거래대금 급증 거름 4개 추가. 판정을 주문 금액 4개 각각에서 낸다.",
        "target": "판 P1 칸 32,672개(거래 든 달 24개월 이상 29,728개).",
        "criterion": "판 P1·거름 0·계수 0.3·상한 10% 이하·금액마다: (가)(24개월 이상 칸만, 4.12)와 (나)(정규 환산 4.70) 둘 다 넘고, "
                     "옆 분위 부호 같고, 양수 연도 ≥ 0.7, 평균 > 0.",
        "result": "100만 원에서 1칸 통과(가장 큰 t 5.82). 500만·2,000만·1억 원은 0칸.",
        "versus": "4회차 판정 금액(1억 원)은 그대로 0칸. 판정 금액을 넓히자 100만 원 1칸이 남았다."},
    6: {"test": "지지 저점 결함 수정(상승 추세인데 종가가 이미 마지막 확정 저점 이하인 날 매수가 나던 것), k 8개 중 하나라도 상승이면 1주문인 단일 규칙, "
                "절대 거래대금 구간 7개, 같은 달 대조·시장 거래대금 국면, 청산 진단(MAE·MFE·손절 덧붙임).",
        "target": "단일 규칙 × 판 Q0·Q1·Q2 × 구간 방식(절대 3,744칸·10분위 6,752칸, 판마다). 결함 행은 상승 추세 행의 14.7%.",
        "criterion": "판 Q1·계수 0.3·상한 10% 이하·금액마다 (가)·(나) 둘 다(Q1 절대 (가) 2.97·(나) 2.93, Q1 10분위 (가) 3.54·(나) 4.20), "
                     "옆 구간 부호 같음, 양수 연도 ≥ 0.7, 평균 > 0. 판정은 절대 구간, 10분위는 참고.",
        "result": "절대 구간: 네 금액 모두 0칸(100만 원 가장 큰 t 1.81, 2–5억 E4j5). 10분위(참고): 100만 원 2칸 — 분위 1·상한 0.5%의 E3(t 4.47, 30,597건)과 "
                  "E4j5(t 4.44, 43,746건). 두 칸의 같은 달 대조 t 4.25·5.12. 시장 거래대금 높음 국면(2018–2026)에 매매 대부분이 몰려 있다. "
                  "손절을 덧붙이면 평균 초과수익이 줄었다(E3 1.30% → 손절 3% 0.11%).",
        "versus": "5회차 판정을 결함을 고친 조건으로 다시 내면 100만 원 통과가 1칸 → 2칸(가장 큰 t 5.824 → 5.823). "
                  "판정을 절대 구간으로 옮기자 남는 칸이 없다. 10분위 남는 칸은 5회차와 같은 분위 1·100만 원 자리."},
    7: {"test": "확인용 회차(칸은 6회차 결과를 보고 골랐다). 1분위 효과가 순위에서 오는지 시기에서 오는지 가른다: 교차표(규칙 × 절대 구간 × 그날 순위 10분위 × "
                "시장 거래대금 국면), 같은 절대 구간 안 1분위 대 나머지 순위(같은 달), 6회차 남는 칸의 같은 달·같은 절대 구간 대조, 추세 청산 격자(판정에 안 씀).",
        "target": "E3·E4j5, 주문 100만 원·상한 0.5%·계수 0.3 하나. 교차표 300칸씩 두 판 — X_full(국면 경계를 전 기간 3분위로, 미래 정보 있음), "
                  "X_exp(그 전날까지 분포로, 미래 정보 없음). 기준선 계산에 든 칸(24개월 이상) X_full 102·X_exp 94.",
        "criterion": "교차표: (가)(X_full 3.39·X_exp 3.48)와 (나)(정규 환산 3.40·3.66) 둘 다, 옆 순위 분위 부호 같음, 양수 연도 ≥ 0.7, 평균 > 0. "
                     "1분위 대 나머지: 국면 합친 10검정 통과선 t 3.0.",
        "result": "교차표 통과 X_full 2칸·X_exp 1칸, 모두 2–5억·1분위·국면 높음 자리(X_exp E3 t 5.30, 25,640건). "
                  "같은 절대 구간 안 1분위 대 나머지 차이는 10검정 모두 t 3.0 미만(2–5억 E3 −0.30·E4j5 −0.81). "
                  "6회차 남는 칸을 같은 달·같은 절대 구간 매매와 견주면 차이 t가 E3 0.49·E4j5 −0.29(1분위 빼고 견주면 0.22·−0.14).",
        "versus": "6회차 같은 달 대조(t 4.25·5.12)에 절대 구간까지 맞추자 차이가 거의 사라졌다. 1분위 효과는 순위보다 시기(시장 거래대금 높은 국면)와 "
                  "절대 거래대금 구간에서 온 것으로 보인다. 확인용이라 새 발견으로 세지 않는다."},
    8: {"test": "확인용 회차(칸 구성은 7회차 결과를 보고 정했다). 같은 날·같은 보유 기간·같은 절대 구간(하한 같이) 보통주 전 종목 균등 매수를 대조로 두고, "
                "매매 수익에서 대조를 뺀 값으로 다시 판정한다.",
        "target": "고정 칸 3,640개(주문 금액 4개 × 규칙 13개 × 절대 구간 7개 × 그날 순위 10분위). 매매가 있는 칸 767개, t를 낸 칸 754개, 그중 24개월 이상 546개.",
        "criterion": "대조 뺀 값의 t가 (가) 24개월 이상 칸 기준선 3.70과 (나) 정규 환산 4.16을 둘 다 넘고, 옆 순위 분위 부호 같음, 양수 연도 ≥ 0.7, 평균 > 0.",
        "result": "통과 0칸. 가장 큰 t 2.34(24개월 이상 칸 2.04). 24개월 이상 칸의 79%가 음수 t. "
                  "6·7회차에서 남은 칸 자리(100만 원·2–5억·1분위)는 13개 규칙 모두 대조 뺀 값이 음수.",
        "versus": "7회차까지 보인 시장 대비 수익은 소형 거래대금 구간 자체의 몫으로 보인다. 같은 날 같은 구간 종목을 고루 사도 비슷하게 나온다. "
                  "6회차 1분위 재현(E3 30,597건 평균 1.3013%, E4j5 43,746건 1.0364%)은 일치."},
    9: {"test": "8회차 결과를 보고 연 다른 갈래. 신호 없이 직전 20일 거래대금으로 정한 절대 구간의 보통주 전 종목을 주문 100만 원씩 균등하게 사서 "
                "보유 일수마다 전부 팔고 다시 산다. 시장 전체 균등 보유(B)와 견준다.",
        "target": "고정 칸 63개(절대 구간 7 × 보유 3 × 금액 3), 판정 칸 30개(24개월 이상). 대용값 처리 세 판 A(전부)·B(대용값 행 뺌)·C(대용값 종목 뺌), 판정은 A.",
        "criterion": "B 대비 초과수익 t가 (가) 2.65와 (나) 정규 환산 2.62를 둘 다 넘고, 옆 구간 부호 같음, 양수 연도 ≥ 0.7, 평균 > 0.",
        "result": "30칸 중 2칸 통과, 모두 2–5억·100만 원: 20일 보유 B 대비 +0.56%p(t 5.23, 리밸런스 206회), 60일 보유 +1.35%p(t 3.84, 68회). "
                  "편향 감사에서 보류: B에 바구니와 같은 회전 비용을 물렸고, 거래 정지·상장 폐지 손실이 빠졌고, 8회차를 본 뒤 고른 탐색 경로다.",
        "versus": "8회차에서 신호의 몫이 0에 가깝다고 본 뒤, 2–5억 구간 자체를 들고 있는 것만으로 B보다 높게 나오는지 따로 쟀다. "
                  "보류 사유 셋은 11회차에서 고쳤다."},
    10: {"test": "9회차 2–5억 바구니의 초과수익이 작은 종목·싼 종목·변동성 몫인지 가른다. ①가 시가총액 10분위 대조(2020–), ①나 무수정 종가 5분위 × 60일 변동성 5분위 대조, "
                 "② 남는 종목을 유지하는 바구니 − B. 2025–2026 최근 구간을 따로 본다.",
         "target": "고정 칸 12개(보유 20·60일 × 구간 2–5억·5–10억 × 대조 3).",
         "criterion": "t가 (가) 2.44와 (나) 2.36을 둘 다 넘고, 양수 연도 ≥ 0.7, 평균 > 0.",
         "result": "①가 시가총액 대조(2020–) 통과 0칸(가장 큰 t 2.17). ①나 가격·변동성 대조 2칸(2–5억 20일 +0.28%p t 3.91, 60일 +0.65%p t 2.80). "
                   "② 겹침 유지 − B 3칸. 2025–2026은 약세: 9회차 재현 20일 2–5억이 −0.17%p(t −0.39), 같은 길이 구간 186개 가운데 하위 1%.",
         "versus": "9회차 +0.56%p 가운데 가격·변동성 대조 뒤 남는 몫은 +0.28%p로 절반쯤이다. 시가총액 대조는 기간이 짧아(2020–) 판정 힘이 약하다. "
                   "9회차 감사 지적(B 비용·정지/폐지 손실)은 아직 고치지 않은 판이다."},
    11: {"test": "9회차 편향 감사 지적을 반영해 다시 잰다: 정지 종목은 재개일 종가에 매도, 부실 폐지·정리매매 없음은 −90%, 정지 점프 고침, "
                 "비용은 RESEARCH_MID와 연도별 매도 세율, B는 비용 없이 들고 있는 시장. 판정 값은 크기(시가총액) 대조 칸.",
         "target": "고정 칸 16개(보유 20·60일 × 구간 2–5억·5–10억 × 대조 시가총액(2016–)·가격·변동성 × 매번 전부 다시 삼·남는 종목 유지). 기간별·B 대비 진단은 diagnostics.csv.",
         "criterion": "기준선 max(8회차 가족 4.16, 16칸 (가), (나)) = 4.16. 판정은 시가총액 대조 칸.",
         "result": "크기 대조 통과는 20일·2–5억 2칸뿐(전부 다시 삼 +0.59%p t 4.83, 남는 종목 유지 +0.58%p t 4.77). "
                   "비용 없는 B 대비 2010–2026(선형 충격 0.3): 전부 다시 삼 −0.22%p(t −2.08), 남는 종목 유지 +0.21%p(t 1.97). "
                   "같은 크기 종목보다는 낫지만, 시장을 들고 있는 것을 이긴다는 근거는 없다. "
                   "판정 값(대조에 매기는 비용)을 첫 실행 뒤에 바꿨다: 첫 판은 대조에 그 종목 자기 비용, 지금 판은 바구니 종목과 같은 비용 배수. "
                   "바꾼 쪽이 더 보수적이다(선형 0.3에서 4.16을 넘는 칸 16개 중 7 → 4, 판정 칸 통과 2).",
         "versus": "9회차 B 대비 +0.56%p(t 5.23)는 B에 회전 비용을 물리고 정지·폐지 손실을 뺀 값이었다. 둘을 고치자 B 대비는 0 근처나 음수로 내려갔다. "
                   "결론 아님(check_backtest 대상 아님)."},
}


def write_packed(path: Path, raw: bytes) -> int:
    """gzip 한 바이트를 base64 텍스트로(아티팩트는 이진 파일을 싣지 않는다). 페이지는 atob → 압축 풀기."""
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(base64.b64encode(gzip.compress(raw, 9)))
    return path.stat().st_size


def json_bytes(payload) -> bytes:
    return json.dumps(payload, ensure_ascii=False, separators=(",", ":"), allow_nan=False,
                      default=lambda value: value.item() if hasattr(value, "item") else str(value)).encode("utf-8")


def plain(value):
    """표 값 하나를 JSON 에 넣을 수 있게. nan·inf 는 None."""
    if isinstance(value, (float, np.floating)):
        return float(value) if np.isfinite(value) else None

    if isinstance(value, (np.integer,)):
        return int(value)

    if isinstance(value, (np.bool_,)):
        return bool(value)

    return value


def table_of(frame: pd.DataFrame) -> dict:
    return {"columns": list(frame.columns), "rows": [[plain(value) for value in row] for row in frame.itertuples(index=False)]}


def nested_plain(value):
    if isinstance(value, dict):
        return {str(key): nested_plain(item) for key, item in value.items()}

    if isinstance(value, list):
        return [nested_plain(item) for item in value]

    return plain(value)


# ── 일봉 ────────────────────────────────────────────────────────────────

def write_bars(panel: common.Panel, stock_indices: list[int], first_position: int) -> dict:
    """종목 40 개씩 묶어 [시가, 고가, 저가, 종가] 의 전날 종가 차를 Int32 로. 날짜 번호는 first_position 부터 센다."""
    bundle_of = {}

    for bundle, start in enumerate(range(0, len(stock_indices), BUNDLE_SIZE)):
        chunks = []
        offset = 0

        for stock in stock_indices[start:start + BUNDLE_SIZE]:
            first_row, last_row = trimmed_rows(panel, stock)
            rows = np.arange(first_row, last_row)
            positions = panel.market_position[rows] - first_position
            length = int(positions[-1] - positions[0] + 1)
            prices = np.full((length, 4), np.nan)
            local = positions - positions[0]
            prices[local, 0] = panel.open_price[rows]
            prices[local, 1] = panel.high[rows]
            prices[local, 2] = panel.low[rows]
            prices[local, 3] = panel.close[rows]
            base = pd.Series(prices[:, 3]).ffill().shift(1).to_numpy()
            base[0] = 0.0
            base = np.where(np.isfinite(base), base, 0.0)
            delta = np.where(np.isfinite(prices), np.round(prices - base[:, None]), MISSING).astype(np.int64)
            delta = np.clip(delta, MISSING, 2147483647).astype(np.int32)
            chunks.append(delta.ravel())
            bundle_of[panel.codes[stock]] = [bundle, offset, int(positions[0]), length]
            offset += delta.size

        write_packed(OUTPUT / "data" / "bars" / f"b{bundle}.txt", np.concatenate(chunks).astype("<i4").tobytes())

    return bundle_of


def trimmed_rows(panel: common.Panel, stock: int) -> tuple[int, int]:
    first, last = int(panel.starts[stock]), int(panel.starts[stock + 1])
    signal_row = first + int(np.searchsorted(panel.dates[first:last], common.SIGNAL_FROM))
    return max(first, signal_row - WARMUP_ROWS), last


# ── 매매 ────────────────────────────────────────────────────────────────

def read_trades(paths: list[Path]) -> pd.DataFrame:
    frames = []

    for path in paths:
        frame = pd.read_csv(path, dtype={"code": str, "turnover_bin": str, "signal_k_list": str, "moving_average_windows": str,
                                         "atr_periods": str})
        frame.insert(0, "source", path.resolve().parent.name)
        frames.append(frame)

    trades = pd.concat(frames, ignore_index=True)
    trades["code"] = trades["code"].str.zfill(6)
    return trades


def cell_payload(frame: pd.DataFrame, stock_number: dict, dates: np.ndarray, dictionaries: dict) -> dict:
    """열 단위. 종목은 meta.stocks 번호, 날짜는 meta.dates 번호, 문자열은 meta.dictionaries 번호."""
    def day_index(values: np.ndarray) -> np.ndarray:
        position = np.searchsorted(dates, values)
        found = (position < len(dates)) & (dates[np.minimum(position, len(dates) - 1)] == values)
        return np.where(found, position, -1)

    columns = {
        "stock": frame["code"].map(stock_number).to_numpy(np.int64),
        "entry": day_index(frame["entry_date"].to_numpy(np.int64)),
        "exit": day_index(frame["exit_date"].to_numpy(np.int64)),
        "support": day_index(frame["support_low_date"].fillna(0).to_numpy(np.int64)),
    }

    for column in ["entry_price_adjusted", "exit_price_adjusted", "support_low_value_adjusted", "chosen_k", "slices",
                   "distinct_exits_across_windows"]:
        columns[column] = frame[column].to_numpy()

    for column in ["entry_price_raw", "exit_price_raw"]:
        columns[column] = np.round(frame[column].to_numpy(np.float64), 1)

    columns["turnover_10k"] = np.round(frame["previous_20d_turnover_krw"].to_numpy(np.float64) / 1e4)

    for column in ["excess_c0_3", "mae_percent", "mfe_percent", "realized_percent"]:
        columns[column] = np.round(frame[column].to_numpy(np.float64), 3)

    columns["hold_days"] = np.round(frame["hold_days"].to_numpy(np.float64), 2)

    for column in DICTIONARY_COLUMNS:
        lookup = {text: index for index, text in enumerate(dictionaries[column])}
        columns[column] = frame[column].fillna("").map(lookup).to_numpy(np.int64)

    return {name: [plain(value) for value in values] for name, values in columns.items()}


# ── 회차 표 ─────────────────────────────────────────────────────────────

def round_payload(round_number: int) -> dict:
    folder = RESULTS / f"round{round_number}"
    script = STUDY / f"round{round_number}_test.py"
    docstring = ast.get_docstring(ast.parse(script.read_text(encoding="utf-8"))) if script.exists() else ""
    tables = {}
    skipped = []
    names = sorted(path.name for path in folder.glob("*") if path.suffix in (".csv", ".json") and not path.name.endswith("_trades.csv"))
    names = [name for name in ROUND_FILES if name in names] + [name for name in names if name not in ROUND_FILES]

    for name in names:
        path = folder / name

        if path.stat().st_size > ROUND_TABLE_LIMIT:
            skipped.append([name, path.stat().st_size])
            continue

        if name.endswith(".json"):
            tables[name] = {"json": nested_plain(json.loads(path.read_text(encoding="utf-8")))}
        else:
            tables[name] = table_of(pd.read_csv(path))

    return {"round": round_number, "summary": ROUND_SUMMARY.get(round_number, {}), "docstring": docstring, "tables": tables,
            "skipped": skipped}


# ── 시작 ────────────────────────────────────────────────────────────────

def clear_output() -> None:
    """이 스터디 dashboard/ 안의 이전 산출만 지운다(1회차 trades/<종목>.json 포함)."""
    OUTPUT.mkdir(parents=True, exist_ok=True)

    for child in OUTPUT.iterdir():
        if child.is_dir():
            shutil.rmtree(child)
        else:
            child.unlink()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--trades", nargs="+", type=Path, default=DEFAULT_TRADES)
    parser.add_argument("--rounds", nargs="+", type=int, default=DEFAULT_ROUNDS)
    parser.add_argument("--rounds-only", action="store_true", help="data/rounds.txt 와 페이지 파일만 다시 쓴다(매매·일봉은 그대로)")
    arguments = parser.parse_args()
    started = time.time()

    if arguments.rounds_only:
        write_packed(OUTPUT / "data" / "rounds.txt", json_bytes([round_payload(number) for number in arguments.rounds]))
        shutil.copyfile(STUDY / "dashboard_index.html", OUTPUT / "index.html")
        shutil.copyfile(STUDY / "dashboard_app.js", OUTPUT / "dashboard_app.js")
        print(f"회차 {arguments.rounds} · {(OUTPUT / 'data' / 'rounds.txt').stat().st_size / 1e6:.2f}MB · {time.time() - started:.0f}초")
        return

    trades = read_trades(arguments.trades)
    panel = common.Panel()
    stock_of_code = {code: index for index, code in enumerate(panel.codes)}
    codes = sorted(set(trades["code"]))
    missing_codes = [code for code in codes if code not in stock_of_code]

    if missing_codes:
        raise SystemExit(f"일봉에 없는 종목: {missing_codes[:10]}")

    stock_indices = [stock_of_code[code] for code in codes]
    first_position = min(int(panel.market_position[trimmed_rows(panel, stock)[0]]) for stock in stock_indices)
    dates = panel.market_dates[first_position:].astype(np.int64)
    clear_output()
    bundle_of = write_bars(panel, stock_indices, first_position)
    stock_number = {code: index for index, code in enumerate(codes)}
    dictionaries = {column: sorted(set(trades[column].fillna(""))) for column in DICTIONARY_COLUMNS}

    # 대조: 매수가 = 그날 종가, 지지 저점 값 = 그날 저가
    row_of = {}

    for code in codes:
        stock = stock_of_code[code]
        first, last = int(panel.starts[stock]), int(panel.starts[stock + 1])
        row_of[code] = (first, panel.dates[first:last])

    def row_at(code: str, day: int) -> int:
        first, stock_dates = row_of[code]
        position = int(np.searchsorted(stock_dates, day))
        return first + position if position < len(stock_dates) and stock_dates[position] == day else -1

    entry_rows = np.array([row_at(code, day) for code, day in zip(trades["code"], trades["entry_date"])])
    support_rows = np.array([row_at(code, day) for code, day in zip(trades["code"], trades["support_low_date"])])
    checks = {
        "trades": int(len(trades)),
        "entry_day_found": int((entry_rows >= 0).sum()),
        "entry_price_equals_close": int(((entry_rows >= 0) & (panel.close[entry_rows] == trades["entry_price_adjusted"].to_numpy())).sum()),
        "support_day_found": int((support_rows >= 0).sum()),
        "support_value_equals_low": int(((support_rows >= 0) & (panel.low[support_rows] == trades["support_low_value_adjusted"].to_numpy())).sum()),
        "support_below_entry": int((trades["support_low_value_adjusted"] < trades["entry_price_adjusted"]).sum()),
    }

    cells = []

    for number, (key, frame) in enumerate(trades.groupby(CELL_KEYS, sort=False)):
        frame = frame.sort_values(["entry_date", "code"], kind="mergesort")
        file_name = f"c{number}.txt"
        size = write_packed(OUTPUT / "data" / "trades" / file_name, json_bytes(cell_payload(frame, stock_number, dates, dictionaries)))
        description = dict(zip(CELL_KEYS, key))
        cells.append({**{name: plain(value) for name, value in description.items()}, "file": file_name, "trades": int(len(frame)),
                      "stocks": int(frame["code"].nunique()), "mean_excess": float(frame["excess_c0_3"].mean()),
                      "first_entry": int(frame["entry_date"].min()), "last_entry": int(frame["entry_date"].max()), "bytes": size})

    meta = {
        "built": time.strftime("%Y-%m-%d %H:%M"),
        "sources": [str(path.resolve().relative_to(common.REPOSITORY)).replace("\\", "/") for path in arguments.trades],
        "dates": dates.tolist(),
        "stocks": codes,
        "names": [panel.names[stock_of_code[code]] for code in codes],
        "bundle_of": bundle_of,
        "bundle_size": BUNDLE_SIZE,
        "cells": cells,
        "dictionaries": dictionaries,
        "rule_text": common.RULE_TEXT,
        "windows": list(common.WINDOWS),
        "atr_periods": list(common.ATR_PERIODS),
        "k_values": list(common.K_VALUES),
        "distance_edges": [float(common.DISTANCE_EDGES[0]), float(common.DISTANCE_EDGES[-1])],
        "last_date": common.LAST_DATE,
        "checks": checks,
        "panel_rows": int(panel.row_count),
    }
    write_packed(OUTPUT / "data" / "meta.txt", json_bytes(meta))
    write_packed(OUTPUT / "data" / "rounds.txt", json_bytes([round_payload(number) for number in arguments.rounds]))
    shutil.copyfile(STUDY / "dashboard_index.html", OUTPUT / "index.html")
    shutil.copyfile(STUDY / "dashboard_app.js", OUTPUT / "dashboard_app.js")
    files = sorted(path for path in OUTPUT.rglob("*") if path.is_file())
    total = sum(path.stat().st_size for path in files)
    print(f"파일 {len(files)}개 · {total / 1e6:.1f}MB · 매매 {len(trades):,}건 · 종목 {len(codes):,} · 묶음 {len(set(bundle_entry[0] for bundle_entry in bundle_of.values()))}"
          f" · {time.time() - started:.0f}초")
    print(json.dumps(checks, ensure_ascii=False))


if __name__ == "__main__":
    main()
