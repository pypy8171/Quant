"""스터디 43 결과를 스터디 38 대시보드의 '43' 스터디 탭 데이터로 쓴다.

- dashboard/data/study43.txt: 회차(43-A 재현 대조, 43-B 1단계, 43-C 2단계, 43-D 엔진 설정·익절×손절)마다 시험·대상·기준·결과·다음으로 간 이유와 그 회차의 표.
표는 results/ 아래 csv 를 그대로 읽어 열만 골라 싣는다(값을 다시 계산하지 않는다).
사용: py -I -X utf8 research/studies/43_devscale_real_rules/build_dashboard_tabs.py
"""
import sys
from pathlib import Path

import pandas as pd

HERE = Path(__file__).resolve().parent
RESULTS = HERE / "results"
sys.path.insert(0, str(HERE.parent / "38_trend_stop_intraday"))

import build_dashboard as board  # noqa: E402

TABLE_COLUMNS = ["조합 번호", "방식", "신호", "신호 상위 q", "매도", "슬롯", "금액", "미끄러짐 호가", "연 수익 %", "MDD %", "샤프",
                 "코스피 대비 연 초과 %p", "코스닥 대비 연 초과 %p", "코스피 대비 플러스 해", "전체 해", "코스피 월 t", "코스피 연 t",
                 "코스닥 월 t", "코스닥 연 t", "가장 낮은 t", "무작위 대조 승수", "무작위 대조 연 수익 중앙값 %", "매수 수",
                 "건당 순수익 평균 %", "건당 순수익 중앙값 %", "연 회전(매수액÷평균 평가금)", "평균 보유일", "슬롯 평균 사용률 %"]


def read(name: str) -> pd.DataFrame:
    return pd.read_csv(RESULTS / name, encoding="utf-8-sig")


def table(name: str, title: str, note: str, sort: str | None = None, columns: list[str] | None = None,
          frame: pd.DataFrame | None = None) -> dict:
    if frame is None:
        frame = read(name)

    if columns:
        frame = frame[[column for column in columns if column in frame.columns]]

    frame = frame.copy()
    for column in frame.columns:
        if frame[column].dtype == object:
            frame[column] = frame[column].fillna("")

    return {"title": title, "note": note, "source": f"research/studies/43_devscale_real_rules/results/{name}",
            "labels": {column: column for column in frame.columns}, "sort": sort, "data": board.clean(frame)}


def order_summary() -> pd.DataFrame:
    order = read("order_random.csv")
    stage1 = read("stage1_table.csv")
    keys = ["매도", "슬롯", "금액", "미끄러짐 호가"]
    grouped = order.groupby(keys).agg(**{"무작위 순서 연 수익 중앙값 %": ("연 수익 %", "median"),
                                         "무작위 순서 연 수익 최소 %": ("연 수익 %", "min"),
                                         "무작위 순서 연 수익 최대 %": ("연 수익 %", "max"),
                                         "1단계 통과선 넘은 횟수": ("t 통과(1단계 통과선)", "sum")}).reset_index()
    merged = grouped.merge(stage1[keys + ["연 수익 %"]].rename(columns={"연 수익 %": "거래대금 순 연 수익 %"}), on=keys)
    merged["1단계 통과선 넘은 횟수"] = merged["1단계 통과선 넘은 횟수"].astype(int)
    return merged


def rounds() -> list[dict]:
    family = read("family_bars.csv")
    bar_all = family["family_bar"].iloc[1]
    bar_stage1 = family["family_bar"].iloc[0]
    judge = read("judge.csv")
    best_stage2 = judge[judge["단계"] == 2]["가장 높은 '가장 낮은 t'"].max()
    return [
        {"id": "a", "label": "43-A 재현 대조",
         "test": "하네스의 매수 후보 행렬과 OLD 매도를 (가) 스터디 38 원본 패널에, (나) 43 가격(41 종목·정지 점프 보정 비, 하한가 잠김 미룸)에 걸어 "
                 "38 entries_current 와 매수일·매수가·매도일·매도가를 건마다 맞춘다.",
         "target": "2010-01-04 – 2026-10-08 보통주 전 종목, 38 지금 규칙 매수 130,649건.",
         "criterion": "(가) 전 건 일치, (나) 다른 건이 비 보정·잠김 미룸으로 설명되는지(내가 정함).",
         "result": "(가) 130,649건 전부 같다. (나) 매수 99.95% 같은 날, 다른 70건은 매도일이 바뀌어 같은 종목 다음 매수가 밀린 것. "
                   "감사에서 비가 같은 가격인데 1 ± 1e-14로 흔들려 등호 체결 판정을 뒤집은 것(95.4%)을 찾아 1로 맞췄다.",
         "why_next": "매수·매도가 엔진 규칙과 같다는 것을 확인했으니 슬롯 계좌로 굴려 지수와 비교한다.",
         "tables": [table("reproduction.csv", "재현 대조", "걸린 초 열은 실행마다 다르다.")]},
        {"id": "b", "label": "43-B 1단계 실제 규칙",
         "test": "매도 D158(SMA60 종가 이탈 다음 날 시가) · OLD(−6.5% 손절·+3% 익절·존 이탈) × 슬롯 4·10·20 × 금액(고정 500만 · 평가금÷S 복리) "
                 "× 미끄러짐 0·1·2호가 = 36조합. 후보가 많으면 (i) 거래대금 큰 순, (ii) 무작위 순서 20회.",
         "target": "같은 기간·전 종목 3,190개(상장폐지 포함). 조합마다 무작위 종목 대조 20회.",
         "criterion": f"코스피·코스닥 둘 다 월 t·연 t ≥ 묶음 통과선(전체 {bar_all:.2f}, 1단계만 {bar_stage1:.2f}) 그리고 무작위 대조 19승 이상(내가 정함).",
         "result": "통과 0 / 36. 코스피를 넘은 것은 D158·S=4·고정 하나(연 6.47%, 코스피 8.61%보다 연 초과 +0.20%p, t 0.03, 최대 낙폭 −79%). "
                   "OLD는 해마다 −34 – −45%(비용이 건당 0.77%, 평균 3일 보유). D158은 건당 평균 +1%대지만 중앙값 −1.5%, 2011–2020 사이 −75 – −79% 낙폭. "
                   "무작위 순서가 거래대금 순보다 S=10·20에서 4–7%p 낫다. 무작위 순서 720회 중 통과선을 넘은 것 0.",
         "why_next": "매수 신호의 몫이 작다(D158 무작위 종목 대조 중앙값 연 1.1–3.5%로 전략과 비슷). 가치·품질·250일 고점 신호로 고르면 달라지는지 2단계에서 본다.",
         "tables": [table("stage1_table.csv", "1단계 36조합(순서 (i) 거래대금 순)", "t 는 부호 뒤집기 1,000회. 가장 낮은 t 는 코스피·코스닥 월·연 네 값 중 최솟값.",
                          "가장 낮은 t", TABLE_COLUMNS),
                    table("order_random.csv", "순서 (ii) 무작위 순서 20회 요약", "같은 슬롯·금액·미끄러짐의 거래대금 순 결과와 나란히.", None,
                          frame=order_summary()),
                    table("exit_reasons.csv", "매도 이유별 건수·수익(미끄러짐 1호가, 1단계)", "순수익 합은 원.", None,
                          frame=read("exit_reasons.csv").query("단계 == 1").drop(columns=["단계", "방식", "신호", "신호 상위 q"]))]},
        {"id": "c", "label": "43-C 2단계 신호 결합",
         "test": "VALUE·QUAL·HIGH250(41 점수, 오늘 이전 마지막 결정일 값)을 (A) DevScale 후보 순서로, (B) 신호 상위 20%·50%만 매수 허용으로, "
                 "(C) 신호 단독 다음 날 시가 매수 + D158 매도로 붙인다. 378조합.",
         "target": "1단계와 같은 기간·종목. VALUE·QUAL은 재무 자료가 2015-07부터라 그 전에는 점수가 없다.",
         "criterion": f"1단계와 같다(전체 통과선 {bar_all:.2f}).",
         "result": f"통과 0 / 378. 가장 높은 '가장 낮은 t'는 {best_stage2:.2f}((A) HIGH250 순서·D158·S=4·복리, 0호가). "
                   "1호가에서 같은 조합은 연 15.8%, 코스피보다 +7.2%p지만 t 0.77. (C) HIGH250 단독은 연 −14%(고점 근처 매수가 급락을 그대로 맞음).",
         "why_next": "실제 규칙으로는 지수를 이긴다는 근거가 없다. 다음은 엔진 실제 사이징(270만–400만 원)과 거래대금 상위 100종목 스캔, "
                     "D158 매매의 MAE/MFE와 익절%×손절% 격자.",
         "tables": [table("stage2_table.csv", "2단계 378조합", "신호 상위 q 는 (B)만.", "가장 낮은 t", TABLE_COLUMNS),
                    table("judge.csv", "판정 요약", "통과선은 828계열(전체)·72계열(1단계) 묶음.", None),
                    table("family_bars.csv", "묶음 통과선", "뒤집기마다 계열 최댓값의 95% 분위수.", None)]},
        {"id": "d", "label": "43-D 엔진 설정·익절×손절",
         "test": "엔진에서 확인한 값으로 계좌를 다시 짠다: 스캔(장중 거래대금 상위 200 ∪ 거래량 증가 상위 40 ∪ 등락률 상위 100 → 점수 상위 100, "
                 "전일 값으로 근사), 금액 270만 + 130만 × clamp(z/1.5, 0, 1), 보유 100종목·노출 0.95·종목당 1,500만, 시작 6,800만. "
                 "여기에 장중 익절(없음·3–20%) × 손절(없음·3–20%) 격자를 실제 설정·S4·S10·S20(고정·복리)에 건다. 505조합.",
         "target": "1단계와 같은 기간·종목. D158 실제 설정 매매 2,488건의 MAE/MFE.",
         "criterion": f"1단계와 같다. 통과선은 1·2단계와 합친 1,826계열 묶음 {read('round_d/family_bars.csv')['family_bar'].iloc[1]:.2f}. "
                      "가장 좋은 칸이 격자 끝에 있으면 옆 칸 부호가 같은지도 본다(내가 정함).",
         "result": "통과 0 / 505. 엔진 실제 설정 D158 1호가는 연 4.90%, 최대 낙폭 −61.8%, 코스피보다 −3.92%p, 가장 낮은 t −1.04. "
                   "이 값은 그날 후보를 점수 순으로 채운 위쪽 값이고, 무작위 순서 20회 중앙값은 연 2.45%(t −1.65)다(감사). 익절 3–10%는 모두 나빠지고(줄 평균 t 약 −5.9) 손절 폭 사이 차이는 경로 우연(t 표준편차 약 0.4) 안이라 구별되지 않는다. 칸 하나의 t는 경로 따라 흔들리니 줄 평균으로 읽는다. "
                   "MFE 3% 이상 닿은 건의 52%가 결국 순손실로 끝났다. 매수 건당 순수익 중앙값 −1.9%, 평균 0.9%.",
         "why_next": "매도 규칙을 바꿔서는 고칠 수 없고 매수 쪽 기댓값이 문제다. 스캔이 전일 값이라 38 매수 후보가 스캔에 드는 비율이 8.3%뿐이다. "
                     "다음은 1분봉으로 09:00–09:30 누적 거래대금 스캔을 재현해 실제 엔진이 보는 종목군에서 다시 잰다.",
         "tables": [table("round_d/judge.csv", "판정 요약(계좌별)", "가장 높은 값은 그 계좌 72조합 중 최댓값.", None),
                    table("round_d/grid_table.csv", "505조합", "익절·손절은 장중 가격 도달. 같은 날 둘 다 닿으면 손절 먼저.", "가장 낮은 t",
                          ["조합 번호", "계좌", "매도", "익절", "손절", "미끄러짐 호가", "연 수익 %", "MDD %", "코스피 대비 연 초과 %p",
                           "코스닥 대비 연 초과 %p", "가장 낮은 t", "무작위 대조 승수", "매수 수", "매수당 평균 금액(원)",
                           "건당 순수익 평균 %", "건당 순수익 중앙값 %", "평균 보유일", "옆 칸 가장 낮은 t 최소", "옆 칸 모두 같은 부호"]),
                    table("round_d/edge.csv", "가장 좋은 칸이 격자 끝인지", "익절 끝 값·손절 끝 값이 True 면 격자 끝.", None),
                    table("round_d/mae_mfe_quantiles.csv", "MAE/MFE 분위수(실제 설정 D158)", "고점 전 MAE 는 MFE 날 이전의 가장 낮은 값.", None),
                    table("round_d/mfe_then_loss.csv", "MFE 에 닿은 뒤 손실로 끝난 비율", "순손실은 비용 뒤.", None),
                    table("round_d/engine_scan.csv", "스캔 재현 요약", "하루 평균. 전일 값 근사.", None),
                    table("round_d/family_bars.csv", "묶음 통과선", "43-D만 / 1·2단계와 합친 것.", None)]},
    ]


def main() -> None:
    payload = {"title": "43 DevScale 실제 규칙", "goal": "41에서 DevScale을 근사로 넣었던 것을 엔진의 실제 매수·매도 규칙으로 바꿔, "
               "슬롯 계좌 하나로 2010–2026 전 종목을 굴려 코스피·코스닥과 무작위 종목을 이기는지 본다.", "rounds": rounds()}
    print(f"→ study43.txt {board.write_json('study43.txt', payload, compress=True) / 1e3:.0f}KB")


if __name__ == "__main__":
    main()
