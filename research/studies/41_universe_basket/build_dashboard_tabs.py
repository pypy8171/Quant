"""스터디 41 결과를 스터디 38 대시보드의 '41' 스터디 탭 데이터로 쓴다.

- dashboard/data/study41.txt: 회차(41-A – 41-D)마다 무엇을 시험했고(대상·기준), 결과가 어땠고, 왜 다음으로 넘어갔는지와 그 회차의 표.
- dashboard/data/problems41.txt: 문제 종목·문제 매매 전부(개수 제한 없음)와 종목 → 차트 파일 위치.
- dashboard/data/chart41/heads.txt: results/chart_data/<이름>.json 마다 종목명·매매 지점과 일봉 위치(JSON 하나로 모음).
- dashboard/data/chart41/<이름>b.txt: 같은 종목들의 일봉을 Int32 열 7개로 붙인 것(아래 pack_bars). JSON 그대로는 57MB라
  아티팩트 한 판 한도(256MiB)에 들어가지 않아 줄였다. 머리 정보를 한 파일로 모은 것은 한 판 파일 수 한도(511개) 때문이다.
표는 results/ 아래 csv 를 그대로 읽어 열만 골라 싣는다(값을 다시 계산하지 않는다). 비율 열만 % 로 바꾼다.
사용: py -I -X utf8 research/studies/41_universe_basket/build_dashboard_tabs.py
"""
import json
import sys
from pathlib import Path

import numpy as np
import pandas as pd

HERE = Path(__file__).resolve().parent
RESULTS = HERE / "results"
sys.path.insert(0, str(HERE.parent / "38_trend_stop_intraday"))

import build_dashboard as board  # noqa: E402

CHECKS = {"c1": "c1 지수 대비 t", "c2": "c2 무작위 대비 t", "c3": "c3 베타 조정 양수", "c4": "c4 옆 조합 양수",
          "c5": "c5 양수 해 ≥0.7", "c6": "c6 해별 중앙 양수", "c7": "c7 상위10 <30%", "c8": "c8 2025–26 빼도 양수",
          "c9": "c9 무작위 20번 모두"}


def label_of(column: str) -> str:
    return CHECKS.get(column, column.replace("옆칸", "옆 조합").replace("칸", "조합").replace("_", " "))


def table(path: str, title: str, note: str, sort: str | None = None, columns: list[str] | None = None,
          frame: pd.DataFrame | None = None) -> dict:
    if frame is None:
        frame = pd.read_csv(RESULTS / path)

    if columns:
        frame = frame[columns]

    return {"title": title, "note": note, "source": f"research/studies/41_universe_basket/results/{path}",
            "labels": {column: label_of(column) for column in frame.columns}, "sort": sort, "data": board.clean(frame)}


CELL_NOTE = ("조합 이름 = 신호-종목군-N(종목 수)-리밸런싱(W 주간·M 월간)-체결(E0 같은 날 종가·E1 다음 날 시가·E2 다음 날 종가)"
             "-버퍼(B0 끔·B1 1.5N). 연% 는 연환산, t 는 달 묶음·해 묶음 중 작은 값.")
JUDGE_COLUMNS = ["칸", "신호", "종목군", "N", "리밸런싱", "체결", "버퍼", "연수익", "지수_연수익", "지수_대비_연초과", "t_min",
                 "무작위_대비_연초과", "무작위_대비_t_min", "베타조정_연초과", "옆칸_최소_연초과", "양수_해_비율", "상위10_기여_비중",
                 "2025_26_뺀_연초과", "밤_연로그", "장중_연로그", "비용_연로그", "연_회전율", "비용0_지수_대비_연초과",
                 "자본2억_연초과", *CHECKS]


def rounds() -> list[dict]:
    metrics = json.loads((RESULTS / "stage_b_metrics.json").read_text(encoding="utf-8"))
    bar = metrics["bar"]
    return [
        {"id": "a", "label": "41-A 종목군 진단",
         "test": "신호 없이 종목군마다 무작위 10·20종목(무작위 선택 20번) 또는 종목군 전체를 같은 비중으로 담고 매달 바꾼다. "
                 "월말 종가 → 다음 월말 종가와 다음 날 시가 → 시가 두 형태, 비용 0과 S0 두 가지. 밤사이·장중·비용으로 나눠 본다.",
         "target": "2010-01-04 – 2026-10-08 전 종목 3,190개(상장폐지 포함), 그 시점 기준 종목군 23개(거래대금 순위·시가총액·시장·가격·변동성).",
         "criterion": "판정용이 아니다(사전 등록한 진단). 종목군 자체가 지수보다 나은지만 본다.",
         "result": "거래대금 상위 T50–T500은 비용을 빼기 전에도 지수에 진다(종목군 전체 같은 비중, 비용 0: 연 −9.4 – −17.2%p, t −4.6 – −6.4). "
                   "S0 비용을 넣은 무작위 바구니는 연 −19.5 – −26.2%p. 반대로 T500 밖·시가총액 하위(CAP1)·가격 하위(PRICE1)·코스닥·전 종목 전체 같은 비중은 "
                   "비용 0에서 지수보다 연 +2.4 – +8.2%p지만 t 는 모두 2.5 미만이다. 무작위 10·20종목은 해마다 14–24번 갈아 치워 비용이 연 로그 5.6–27.4를 깎는다. "
                   "거래대금 상위는 밤사이에 벌고(T50 연 로그 +46) 장중에 더 크게 잃는다(−58). "
                   "편향 감사: 2020년 전 상장폐지 종목의 거래대금을 대신 값으로 채워 상위 종목군 손실이 조금 커 보일 수 있다. 2020–26만 보면 T50 전체가 지수보다 연 −11.6%p(감사 측정).",
         "why_next": "DevScale이 고르는 거래대금 상위 종목군은 담기만 해도 지수에 진다. 그 안에서 신호로 고르면 이 차이를 뒤집는지, "
                     "다른 종목군에서는 되는지를 41-B에서 판정한다.",
         "tables": [table("stage_a_table.csv", "종목군 × 바구니 × 형태 × 비용 (276행)",
                          "연% 는 무작위 선택 20번 평균. 비용 0 은 수수료·충격·호가·세금 모두 0.", "지수_대비_연초과"),
                    table("stage_a_years.csv", "해마다 지수 대비 연초과%", "", None)]},
        {"id": "b", "label": "41-B 신호 바구니 5,520개",
         "test": "종목군 23 × 신호 10(ZONE·HIGH250·LOWVOL·FLOW5·FLOW20·MOM·REV·VALUE·QUAL·VQ) × N 10·20 × 주간·월간 × 체결 3 × 버퍼 끔·켬 "
                 "= 5,520개 조합을 한 가족으로. 자본 200만, 정수 주식, 무수정 가격·호가 단위, 상한가 잠김·하한가·정지 반영.",
         "target": "2010-01-04 – 2026-10-08, 전 종목.",
         "criterion": f"통과선 = max(가족 기준선 {bar:.3f}, 3.0 내가 정함). c1 지수 대비 t ≥ 통과선, c2 같은 형태 무작위 대비 t ≥ 통과선, "
                      "c3 베타 조정 양수, c4 옆 조합 모두 양수, c5 양수 해 ≥ 0.7, c6 해별 중앙 양수, c7 상위 10종목 기여 < 30%, "
                      "c8 2025–26 빼도 양수, c9 무작위 20번 각각 이김. 아홉 개 모두.",
         "result": "통과 0. 지수 대비 t 가 2를 넘는 조합도 0이다(가장 큰 t_min 1.74, 통과선 3.0 아래라 기각 유지). 지수 대비 연초과가 양수인 조합은 358개. "
                   "가장 나은 HIGH250-PRICE1-N20-M-E0-B1(250일 고가에 가까운 저가주 20종목, 월간)은 연 +11.9%p, t 1.74. "
                   "그러나 같은 종목군에서 무작위로 고정해 고른 대조가 이미 지수보다 연 +8.9%p라, 신호 몫은 연 +3.8%p(t 0.80)뿐이다. "
                   "양수 해 0.65·상위 10종목 기여 46%·자본 2억이면 연 −13.6%p. "
                   "DevScale 근사(ZONE-T200-N20-W-E1-B0)는 연 −20.3%p, t −4.50, 해마다 28.7번 갈아 치운다. "
                   "편향 감사로 고친 것: 처음 판의 +15.3%p·t 1.82는 자본 환급을 분할로 처리한 수정가(한국ANKOR유전 152550, 베트남개발1 096300)와 "
                   "펀드형 종목에서 나왔다. 펀드형 65종목과 수정 비율이 1.5배 넘게 바뀌는 날 앞 30일(1,746건)을 종목군에서 뺐다(1.5배·30일은 내가 정함). "
                   "무작위 대조도 결정마다 새로 뽑던 것을 시드마다 고정 순위로 바꿨다(새로 뽑는 쪽은 연 회전이 커서 지수보다 크게 지는 엉터리 기준이었다. 참고 열로 남김). "
                   "고정 대조로는 c2(무작위 대비 t ≥ 통과선)를 14개 조합이 넘는데, 모두 거래대금 상위 안의 LOWVOL·QUAL·VQ다. 종목군 안에서 고르는 힘은 있지만 지수에는 진다. "
                   "비용 0이면 t ≥ 3이 46개(REV 32·HIGH250 14, 가장 큰 t 5.91)이고, S1 비용에 호가 0틱이면 0개(가장 큰 t 2.73)라 비용이 결정적이다.",
         "why_next": "통과가 없어서 사전 등록대로 왜 지는지(종목군·비용·밤과 장중·보유 형태)를 나누고, 지수에 조금 섞는 방법이 나은지를 41-C에서 본다. 종목군 안에서는 고르는 힘이 있는데(c2 14개) 종목군이 지수에 지는 폭이 더 크다는 점이 41-C에서 볼 핵심이다.",
         "tables": [table("representative_cells.csv", "대표 조합 11개(신호마다 지수 대비 t 가 가장 큰 것 + DevScale 근사)", CELL_NOTE, None,
                          ["칸", "고른_이유", "신호", "종목군", "N", "리밸런싱", "체결", "버퍼", "지수_대비_연초과", "t_min",
                           "무작위_대비_연초과", "무작위_대비_t_min", "통과"]),
                    table("stage_b_judge.csv", "5,520개 조합 전부", CELL_NOTE + " c1–c9 는 판정 항목 통과 여부.", "t_min", JUDGE_COLUMNS)]},
        {"id": "c", "label": "41-C 실패 원인·지수와 섞기",
         "test": "① 종목군 몫(무작위 대조 − 지수) ② 비용 몫(비용 0 − S0) ③ 밤사이·장중 몫(바구니 − 지수) ④ 보유 형태 몫(한 축만 다른 짝의 차) "
                 "⑤ 다중 비교 몫으로 나눈다. 코스피 지수(코어)에 바구니(위성)를 비중 w 0 – 1.0으로 섞어 날마다 비중을 맞춘다.",
         "target": "41-B 5,520개 조합. 위성은 가족에서 t 가 가장 큰 조합과 DevScale 근사 조합 두 개(41-A 결과로 고르지 않음).",
         "criterion": "판정용이 아니다(진단).",
         "result": "조합 전체 중앙값 연 −11.4%p. ① 같은 종목군에서 고정 무작위로 담으면 지수보다 연 −3.2%p. 신호는 그보다 연 −6.2%p 낮다 "
                   "(고정 무작위는 순위가 그대로라 거의 갈아 치우지 않으므로, 이 차이에는 회전 비용이 섞여 있다). "
                   "② 비용이 연 6.6%p. ③ 밤사이 지수보다 −12.7%p, 장중 +10.7%p. ④ 주간이 월간보다 연 −7.0%p. "
                   "거래대금 상위 T50은 고정 무작위 대조부터 지수보다 연 −28.4%p라 신호(+12.0%p)로 메우지 못한다. "
                   "코스피 단독 연 8.6%·MDD −43.9%. 가장 나은 조합을 w 0.5로 섞으면 연 14.7%·MDD −32.9%지만, 5,520개 중 가장 좋은 것을 "
                   "골랐기 때문에 부풀려진 값이다. DevScale 근사를 w 0.5로 섞으면 연 −1.2%·MDD −66.0%.",
         "why_next": "평균으로는 원인이 보인다. 실제로 어느 종목·어느 매매가 손실을 냈는지를 41-D에서 하나씩 본다.",
         "tables": [table("stage_b_failure.csv", "실패 원인 나누기(묶음별 중앙값, 연%p)", "c1–c9 열은 그 항목을 통과한 조합 수.", None),
                    table("stage_b_core_satellite.csv", "코스피 + 바구니 섞기", "w = 바구니 비중. 날마다 비중을 맞춘다. 코스피 몫 비용은 0.", None)]},
        {"id": "d", "label": "41-D 문제 매매·종목",
         "test": "대표 조합 11개의 매매 25,597건 가운데 손실이거나 지수에 진 매매를 전부 모으고, 손실 원인을 순서대로 하나 붙인다: "
                 "상장폐지 → 보유 중 수정가 100원 미만 → 비용(부족분의 50% 이상) → 추세 이탈을 계속 보유(보유 중 전날 종가 SMA20 이상에서 그날 아래로 내려간 첫 날 뒤의 손실이 부족분의 50% 이상) "
                 "→ 밤사이 갭·장중 중 더 나쁜 쪽.",
         "target": "문제 매매 16,376건, 문제 종목 1,612개. 개수 제한 없음.",
         "criterion": "판정용이 아니다(원인 확인).",
         "result": "장중 6,858건 −7,492만 원(손실의 47%), 추세 이탈을 계속 보유 4,418건 −6,296만 원(39%), 밤사이 갭 2,191건 −1,801만 원(11%), "
                   "비용 2,874건 −242만 원, 100원 미만 가격 14건 −105만 원, 상장폐지 21건 −81만 원(손익 합, 자본 200만 기준 조합 11개 합). "
                   "어느 원인도 50%를 넘지 않아 '주원인'은 없다. 장중 손실의 44%(−3,313만 원)는 주간 역추세(REV) 조합 하나에서 나왔고, "
                   "REV를 빼면 추세 이탈을 계속 보유가 가장 크다. "
                   "편향 감사로 고친 것: 처음 판은 추세 이탈을 '보유 중 SMA20 아래인 첫 날'로 잡아 SMA20 아래에서 산 매매가 모두 추세 이탈로 몰렸다(7,225건 −1억 1,040만, 77%). "
                   "지금은 전날 SMA20 이상 → 그날 아래로 내려간 날만 센다. 수정가가 1–2원까지 떨어지는 종목(008080 등)은 밤사이·장중 값이 망가져 '100원 미만 가격'으로 따로 뺐다(100원은 내가 정함). "
                   "'문제 종목' 탭에서 종목을 누르면 일봉과 매수·매도 지점이 나온다.",
         "why_next": "편향 감사 지적을 모두 고쳐 다시 돌렸고 기각은 그대로다. 남은 단서는 둘이다: 비용 0에서만 살아나는 REV·HIGH250(비용을 줄이는 체결 방법이 있는지), 거래대금 상위 안에서 무작위를 이기는 LOWVOL(종목군을 지수에 지지 않는 쪽으로 바꿀 수 있는지). 손실은 장중과 추세 이탈 보유가 나눠 갖는다.",
         "tabs": ["problems41"],
         "tables": [table("problem_causes.csv", "조합별 손실 원인", "손익 합·지수 대비 부족액 합은 원.", None)]},
    ]


TRADE_COLUMNS = {"칸": "cell", "신호": "signal", "종목코드": "code", "종목명": "name", "시장": "market", "매수일": "buy_date",
                 "매수가": "buy_px", "매도일": "sell_date", "매도가": "sell_px", "보유일": "hold", "매매_종류": "kind",
                 "손익_원": "pnl", "순수익률": "ret_net", "지수_수익률": "index_ret", "지수_대비_초과": "excess", "MAE": "mae",
                 "MFE": "mfe", "진입_근거": "reason", "사건": "events", "밤_로그": "night", "장중_로그": "day", "비용_로그": "cost",
                 "추세이탈_날짜": "break_date", "추세이탈_뒤_로그": "after_break", "주된_손실_원인": "cause",
                 "지수대비_부족액_원": "shortfall"}
PERCENT = ["ret_net", "index_ret", "excess", "mae", "mfe", "night", "day", "cost", "after_break"]


def pack_bars(bars: list, position_of: dict) -> tuple[np.ndarray, int]:
    """일봉 n개 → Int32 7n개: 달력 위치 차, 시가−종가, 고가−종가, 저가−종가, 종가 차, 거래량 앞 두 자리, 거래량 자릿수 차.

    가격은 scale 배 정수(종가가 한 번이라도 1,000원 아래면 100, 아니면 1). 거래량은 앞 두 자리만 남긴다(막대 높이용, 오차 5% 이하).
    페이지 decodeBars 가 이 순서를 그대로 푼다.
    """
    rows = np.asarray(bars, dtype=np.float64)
    rows[:, 1:4] = np.where(np.isfinite(rows[:, 1:4]), rows[:, 1:4], rows[:, [4]])
    scale = 100 if np.nanmin(rows[:, 4]) < 1000 else 1
    close = np.round(rows[:, 4] * scale).astype(np.int64)
    position = np.array([position_of[int(date)] for date in rows[:, 0]], dtype=np.int64)
    volume = rows[:, 5].astype(np.int64)
    exponent = np.maximum(np.floor(np.log10(np.maximum(volume, 1))).astype(np.int64) - 1, 0)
    mantissa = np.round(volume / 10.0 ** exponent).astype(np.int64)
    columns = [np.diff(position, prepend=0), np.round(rows[:, 1] * scale) - close, np.round(rows[:, 2] * scale) - close,
               np.round(rows[:, 3] * scale) - close, np.diff(close, prepend=0), mantissa, np.diff(exponent, prepend=0)]
    return np.concatenate([np.asarray(column, dtype=np.int64) for column in columns]), scale


def problems() -> None:
    symbols = pd.read_csv(RESULTS / "problem_symbols.csv", dtype={"종목코드": str})
    symbols = symbols.rename(columns={"종목코드": "code", "종목명": "name", "시장": "market", "매매_수": "trades",
                                      "문제_매매_수": "problem_trades", "누적_손익_원": "pnl", "지수_대비_누적_초과_원": "excess_won",
                                      "손실_매매_비중": "loss_share", "주된_손실_원인": "cause", "칸": "cells"})
    symbols["loss_share"] = symbols["loss_share"] * 100
    trades = pd.read_csv(RESULTS / "problem_trades.csv", dtype={"종목코드": str})[list(TRADE_COLUMNS)].rename(columns=TRADE_COLUMNS)
    trades[PERCENT] = trades[PERCENT] * 100
    trades["break_date"] = trades["break_date"].fillna(0).astype(int)
    trades["events"] = trades["events"].fillna("")
    file_of = {}
    bundles = {path.stem: json.loads(path.read_bytes()) for path in sorted((RESULTS / "chart_data").glob("*.json"))}
    index_daily = bundles.pop("index_daily")
    calendar = sorted({bar[0] for bundle in bundles.values() for symbol in bundle.values() for bar in symbol["bars"]}
                      | {row[0] for rows in index_daily.values() for row in rows})
    index_daily["calendar"] = calendar
    board.write_packed(board.OUT / "chart41" / "index_daily.txt", json.dumps(index_daily, separators=(",", ":")).encode("utf-8"))
    position_of = {date: index for index, date in enumerate(calendar)}
    all_heads = {}

    for stem, bundle in bundles.items():
        heads, parts, offset = {}, [], 0

        for code, symbol in bundle.items():
            file_of[code] = stem
            packed, scale = pack_bars(symbol["bars"], position_of)
            heads[code] = {"name": symbol["name"], "market": symbol["market"], "trades": symbol["trades"],
                           "offset": offset, "length": len(symbol["bars"]), "scale": scale}
            parts.append(packed)
            offset += packed.size

        all_heads[stem] = heads
        board.write_packed(board.OUT / "chart41" / f"{stem}b.txt", np.concatenate(parts).astype("<i4").tobytes())

    board.write_packed(board.OUT / "chart41" / "heads.txt", json.dumps(all_heads, ensure_ascii=False, separators=(",", ":")).encode("utf-8"))

    causes = pd.read_csv(RESULTS / "problem_causes.csv").rename(columns={"칸": "조합"})
    payload = {"symbols": board.clean(symbols), "trades": board.clean(trades), "file_of": file_of,
               "info": json.loads((RESULTS / "problem_info.json").read_text(encoding="utf-8")), "causes": board.clean(causes)}
    print(f"→ problems41.txt {board.write_json('problems41.txt', payload, compress=True) / 1e3:.0f}KB, 차트 파일 {len(file_of)}종목")


def main() -> None:
    payload = {"title": "41 종목군·바구니", "goal": "39에서 대상 종목군 자체가 지수에 진다는 것이 공통 원인으로 나왔다. 종목군과 선택 신호, "
               "손절 없는 바구니 리밸런싱으로 지수와 무작위 바구니를 둘 다 이기는 조합이 있는지 본다.", "rounds": rounds()}
    print(f"→ study41.txt {board.write_json('study41.txt', payload, compress=True) / 1e3:.0f}KB")
    problems()


if __name__ == "__main__":
    main()
