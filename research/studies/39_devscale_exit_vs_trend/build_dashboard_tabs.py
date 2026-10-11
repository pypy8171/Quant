"""스터디 39 회차별 결과를 스터디 38 대시보드의 '39' 스터디 탭 데이터(dashboard/data/study39.txt)로 쓴다.

회차마다 무엇을 시험했고(대상·기준), 결과가 어땠고, 왜 다음 회차로 넘어갔는지를 글로 적고, 그 회차의 판정표를 붙인다.
표는 results/ 아래 csv 를 그대로 읽어 열만 골라 싣는다(값을 다시 계산하지 않는다).
사용: py -I -X utf8 research/studies/39_devscale_exit_vs_trend/build_dashboard_tabs.py
"""
import sys
from pathlib import Path

import pandas as pd

HERE = Path(__file__).resolve().parent
RESULTS = HERE / "results"
sys.path.insert(0, str(HERE.parent / "38_trend_stop_intraday"))

import build_dashboard as board  # noqa: E402

# 열 이름 → 화면 이름. 표마다 이 사전에 있는 열만 싣는다.
LABELS = {
    "cell": "조합", "rule": "규칙", "group": "묶음", "family": "가족", "n": "건수", "trades": "건수",
    "net_mean": "건당 순수익%", "net_median": "건당 중앙%", "net_t": "건당 t", "win_rate": "승률",
    "hold_mean": "보유일 평균", "hold_median": "보유일 중앙", "hold_p90": "보유일 90%",
    "control_net_mean": "무작위 건당%", "pair_vs_a_mean": "지금 규칙 대비 %p", "pair_vs_a_t": "지금 규칙 대비 t",
    "t_min": "t(작은 쪽)", "share_positive_years": "양수 해 비율", "annualized_pct": "연환산%",
    "mdd_pct_of_capital": "MDD%", "capital_exhausted": "자본 바닥", "main_bar": "기준선", "passes": "통과",
    "annual_pct": "연%", "index_annual_pct": "지수 대비 연%p", "index_t_min": "지수 대비 t",
    "kospi_annual_pct": "코스피 대비 연%p", "kospi_t_min": "코스피 대비 t",
    "beta": "베타", "beta_annual_pct": "베타 조정 연%p", "beta_t_min": "베타 조정 t",
    "random_annual_pct": "무작위 대비 연%p", "random_t_min": "무작위 대비 t",
    "run": "실행", "board": "종목군", "candidate": "후보", "variant_label": "변형", "timing": "체결 시점",
    "primary_rule": "청산", "valuation": "미청산 평가", "bar_index": "기준선(지수)", "bar_random": "기준선(무작위)",
    "pool": "대상", "scenario": "비용 가정", "overnight_gross_mean_pct": "밤 수익 평균%", "cost_mean_pct": "비용 평균%",
    "net_mean_pct": "순수익 평균%", "breakeven_gross_pct": "본전 필요 수익%", "possible": "본전 가능",
}


def table(path: str, title: str, note: str, sort: str | None = None, query: str | None = None) -> dict:
    frame = pd.read_csv(RESULTS / path)

    if query:
        frame = frame.query(query)

    columns = [column for column in frame.columns if column in LABELS]
    frame = frame[columns]
    return {"title": title, "note": note, "source": f"research/studies/39_devscale_exit_vs_trend/results/{path}",
            "labels": {column: LABELS[column] for column in columns}, "sort": sort, "data": board.clean(frame)}


ROUNDS = [
    {"id": "r1", "label": "1회차 청산 격자",
     "test": "라이브 DevScale 매수(일봉 근사, 라이브 매수의 62%를 덮음)를 그대로 두고 청산만 바꾼다. 가격 익절×손절 격자, 존 이탈, 이평 이탈 등 252개 조합.",
     "target": "2010-01 – 2026-10-08 전 종목 일봉, 매수 7,946건(P).",
     "criterion": "지금 규칙 대비 건당 순수익 차의 t가 다중 비교 기준선 이상.",
     "result": "지금 규칙은 건당 −0.65%. 청산만 바꿔 지금 규칙을 넘는 조합은 있었지만 건당 순수익이 플러스가 되는 조합은 없었다. 시작 때 갭 매도를 미청산으로 읽는 버그로 P가 1,174건으로 줄었던 것을 고쳤다(7,946건).",
     "why_next": "건당 지표가 보유 기간과 시장 상승을 반영하지 못한다. 기간을 맞춘 비교가 필요해 2회차로 넘어갔다.",
     "tables": [table("cell_table.csv", "조합별 결과(252개 조합)", "건당 순수익, 지금 규칙 대비 차, 무작위 매수와의 차.", "pair_vs_a_t")]},
    {"id": "r2", "label": "2회차 기간 맞춤",
     "test": "1회차 조합을 신호 전체(약 12만 건)로 넓히고 해별 양수 비율·연환산·4슬롯 계좌 시험을 붙였다.",
     "target": "같은 기간, 같은 종목.",
     "criterion": "t(월·종목 묶음 중 작은 쪽) ≥ 기준선, 옆 조합 같은 부호, 지금 규칙보다 나음.",
     "result": "감사에서 기각. ① 건당 지표가 보유 기간·시장 상승을 무시했다 ② 효과의 80%가 무작위 매수에서도 나왔다 ③ 그날 시가·고저를 미리 아는 것처럼 썼다 ④ 겹친 보유를 독립 건으로 셌다 ⑤ 계좌 시험이 무한 신용을 가정했다.",
     "why_next": "다섯 가지를 모두 고쳐 3회차로 다시 돌렸다.",
     "tables": [table("round2/cell_table.csv", "조합별 결과(감사 전, 참고용)", "감사로 기각된 계산이다. 3회차와 비교용으로만 남긴다.", "t_min")]},
    {"id": "r3", "label": "3회차 지수·무작위 대비",
     "test": "날짜별 보유 수익을 코스피·같은 청산의 무작위 매수·베타 조정과 비교. 매수일 시가 매도 금지, 다가간 극값은 종가로 처리. 현금 한도 있는 4슬롯 계좌 시험.",
     "target": "63개 조합(청산 규칙), 2010–2026.",
     "criterion": "코스피 대비 t ≥ 기준선, 무작위 대비 t ≥ 기준선, 양수 해 비율 ≥ 0.7.",
     "result": "무작위 대비로는 이기는 조합이 있었으나 코스피를 이긴 조합은 0(최대 t 0.35). 지금 규칙은 해마다 지수에 졌다(t 약 −12), 4슬롯 연 −16.6%·MDD −95%.",
     "why_next": "코스닥 종목이 많은데 코스피와만 비교했다. 종목마다 자기 시장 지수로 다시 판정하려고 4회차로 넘어갔다.",
     "tables": [table("round3/cell_table.csv", "조합별 결과", "지수는 코스피 하나.", "kospi_t_min")]},
    {"id": "r4", "label": "4회차 시장별 지수",
     "test": "3회차와 같은 조합을 종목마다 자기 시장 지수(코스피·코스닥)와 비교. 코스닥 지수는 네이버 일봉으로 새로 받았다(2009-01-02 – 2026-10-08).",
     "target": "63개 조합.",
     "criterion": "3회차와 같다.",
     "result": "지수를 이긴 조합 0. 21일 이상 보유는 플러스(+6%, 61일 이상 +46%), 6–20일에 자르는 청산이 그 몫을 버린다.",
     "why_next": "청산을 바꿔서는 안 된다. 매수 자체를 바꿔 보려고 5회차(매수 후보)로 넘어갔다.",
     "tables": [table("round4/cell_table.csv", "조합별 결과", "지수는 종목의 시장 지수.", "index_t_min")]},
    {"id": "r5", "label": "5회차 매수 후보",
     "test": "전략 담당이 낸 매수 후보 C1–C5(라이브 재현, 존 변형, 250일 고가 근접 등) 32개 실행 × 청산 = 2,016개 조합.",
     "target": "2010–2026, 시가 체결과 종가 체결.",
     "criterion": "지수 대비 t ≥ 가족 기준선, 무작위 대비 t ≥ 가족 기준선, 미청산 평가 세 가지 모두 통과.",
     "result": "통과 0. 지수 대비 최대 t 2.86. 진단: 밤사이 +2.5%, 장중 −2.6%(17개 해 모두 같음), 왕복 비용 약 0.78%가 신호 우위 0.3–0.5%p보다 크다.",
     "why_next": "가장 나은 C5만 따로 다시 보고, 장중 손실을 피하는 밤 보유가 되는지 보려고 6회차로 넘어갔다.",
     "tables": [table("round5/judge_table.csv", "실행별 판정", "실행마다 대표 청산 한 조합.", "index_t_min")]},
    {"id": "r6", "label": "6회차 C5 재판정·밤 보유",
     "test": "(가) C5를 기준선 4.07로 재판정, 15:20 근사 체결 확인(겹침 95–97%). (나) 종가 매수 → 다음 날 시가 매도(밤 보유)와 비용 가정별 본전 계산.",
     "target": "C5 252개 조합, 밤 보유는 신호 전체 286만 건.",
     "criterion": "5회차와 같다. 밤 보유는 비용을 빼고 플러스가 되는지.",
     "result": "통과 0. 밤 보유는 지금 비용으로 불가능, 호가 0틱·충격 0을 가정해도 C5 95% 조합의 해 t가 3.34로 3.5에 못 미친다.",
     "why_next": "대상 종목군 자체가 지수에 진다는 것이 공통 원인이다. 종목군·바구니 단위로 설계를 바꿔 스터디 41로 넘어갔다.",
     "tables": [table("round6/a_judge_c5_252.csv", "(가) C5 조합별", "기준선 4.07.", "index_t_min"),
                table("round6/b_judge.csv", "(나) 밤 보유 판정", "", None),
                table("round6/b_breakeven.csv", "(나) 비용 가정별 본전", "본전 필요 수익 = 비용 평균.", None)]},
]


def main() -> None:
    payload = {"title": "39 청산 vs 지수", "goal": "DevScale 매수에 어떤 청산을 붙이면 지수와 무작위 매수를 이기는지 찾는다.",
               "rounds": ROUNDS}
    print(f"→ study39.txt {board.write_json('study39.txt', payload, compress=True) / 1e3:.0f}KB")


if __name__ == "__main__":
    main()
