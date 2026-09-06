#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""재현 하네스 산출물(run_meta.json) → 정규화 지표 metrics.json 백필(계열 A 종목 포트폴리오).

대시보드는 metrics.json만 읽는다(데이터 계약①). 종목레벨 재현 하네스(run_c*.py)는
실행 요약을 `<run>_run_meta.json`으로 남기므로, 여기서 그 값을 quant.metrics/v1로 옮긴다
(재실행 아님 — DATA_GO_KR_KEY·네트워크·시점정합 유니버스가 필요해 재현 리스크가 있고,
meta는 이미 결정론 확인된 산출물이다). 곡선·체결은 CSV 경로로 링크한다.

앞으로 새 종목레벨 스터디를 추가할 때 이 스크립트를 재사용한다: 아래 SPECS에 스터디 한 개를
declarative하게 기술하면(각 run의 meta 파일·서브키·정직성 라벨·CSV 링크), 같은 스키마로 묶인다.

run_meta 서브딕트의 표준 키(run_c*.py export 공통):
  ret·cagr·sharpe·mdd(양수)·calmar·win·turnover·ntr·bench·bench_mdd·bench_sharpe·alpha·
  kodex·start·end·(선택) holdout2022{start,end,ret,cagr,sharpe,mdd,calmar,bench}

실행:  python scripts/backfill_studies.py
"""
import json
import sys
from pathlib import Path

# Windows 콘솔(cp949)에서 성공 print(📄 등) 깨짐/크래시 방지
for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass

_HERE = Path(__file__).resolve()
_REPO = _HERE.parents[1]                        # .../Quant
sys.path.insert(0, str(_REPO / "PYQuant"))      # backtest.report 임포트용
from backtest.report import metrics_row, write_metrics_rows  # noqa: E402

STUDIES = _REPO / "research" / "studies"


def _relpath(p: Path) -> str:
    """repo 루트 기준 posix 상대경로(대시보드·문서 링크 이식성)."""
    return str(p.resolve().relative_to(_REPO)).replace("\\", "/")


def _abs(v):
    return abs(v) if isinstance(v, (int, float)) else v


def row_from_meta(sub, *, study_id, strategy, event, window, family="A_portfolio",
                  benchmark="동일가중 매수 후 보유", honesty="unlabeled", caveat="",
                  equity_csv="", trades_csv="", source=""):
    """run_meta 서브딕트 한 개 → quant.metrics/v1 행. mdd는 양수 크기로 정규화.
    turnover는 프록시(엔진이 직접 추적 안 함)라 turnover_is_proxy=True."""
    extra = {}
    ho = sub.get("holdout2022")
    if isinstance(ho, dict):
        extra.update({
            "holdout_flag": True,
            "holdout_window": f"{ho.get('start','')}~{ho.get('end','')}",
            "holdout_total": ho.get("ret"),
            "holdout_calmar": ho.get("calmar"),
            "holdout_bench": ho.get("bench"),
        })
    if caveat:
        extra["caveat"] = caveat
    if source:
        extra["source"] = source
    return metrics_row(
        study_id=study_id, strategy=strategy, family=family, benchmark=benchmark,
        event=event, window=window,
        start_date=sub.get("start", ""), end_date=sub.get("end", ""),
        total_return=sub.get("ret"), cagr=sub.get("cagr"), sharpe=sub.get("sharpe"),
        mdd=_abs(sub.get("mdd")), calmar=sub.get("calmar"), win_rate=sub.get("win"),
        turnover=sub.get("turnover"), turnover_is_proxy=True,
        n_trades=sub.get("ntr"),
        bench_return=sub.get("bench"), bench_mdd=_abs(sub.get("bench_mdd")),
        bench_sharpe=sub.get("bench_sharpe"), alpha=sub.get("alpha"),
        kodex_return=sub.get("kodex"),
        honesty_label=honesty, equity_csv_path=equity_csv, trades_csv_path=trades_csv,
        **extra)


def bh_row_from_meta(sub, *, study_id, window, benchmark="동일가중 매수 후 보유",
                     strategy="BH", event="동일가중 매수 후 보유", source=""):
    """벤치(동일가중 매수 후 보유) 기준선 행 — meta의 bench_* 필드로 합성. alpha=0(자기 자신). strategy="BH"는 대시보드 벤치 감지 센티넬(표시는 평이화)."""
    return metrics_row(
        study_id=study_id, strategy=strategy, family="A_portfolio", benchmark=benchmark,
        event=event, window=window,
        start_date=sub.get("start", ""), end_date=sub.get("end", ""),
        total_return=sub.get("bench"), mdd=_abs(sub.get("bench_mdd")),
        sharpe=sub.get("bench_sharpe"),
        bench_return=sub.get("bench"), bench_mdd=_abs(sub.get("bench_mdd")),
        bench_sharpe=sub.get("bench_sharpe"), alpha=0.0,
        honesty_label="robust",
        **({"source": source} if source else {}))


def load_meta(folder: str, fname: str) -> dict:
    return json.loads((STUDIES / folder / fname).read_text(encoding="utf-8"))


# ── 스터디 정의(declarative) ───────────────────────────────────────────────────
# 새 종목레벨 스터디는 여기에 한 블록을 추가한다.

CV_COMMON = ("생존편향: 유니버스=datagokr 시총상위 스냅샷(캐시∩상위) → α 절대값 신뢰금지, "
             "구성 간 상대비교로만. FETCH_FLOOR=2020이라 워밍업 후 실질신호는 2021~, "
             "홀드아웃 2022는 단일 1점.")
CV_C4B = ("눌림 필터(반등 양봉+거래량 확인)로 무필터 대비 α 개선(-34.7→-22.7%p). "
          "2022 홀드아웃 -14.2% vs 매수 후 보유 -19.0%(+4.77%p 상회) — 하락장 국한 방어 단서지, 전천후 엣지 아님. "
          + CV_COMMON)


def build_bt11():
    folder = "11_signal_axes"
    win = "2021-01-01~2024-12-31"
    c1 = load_meta(folder, "cross_momentum_run_meta.json")
    c3 = load_meta(folder, "donchian_breakout_run_meta.json")
    c4 = load_meta(folder, "mean_reversion_run_meta.json")

    def ec(base):   # equity/trades csv repo-relative 경로
        return (_relpath(STUDIES / folder / f"{base}_equity.csv"),
                _relpath(STUDIES / folder / f"{base}_trades.csv"))

    rows = []
    # 벤치(동일가중 매수 후 보유) — 세 축 공통 풀이므로 횡단면모멘텀 동일가중(meta key "ew")의 bench_* 하나로 대표
    rows.append(bh_row_from_meta(c1["ew"], study_id="BT-11", window=win,
                                 source="11_signal_axes/cross_momentum_run_meta.json"))
    # 횡단면 12-1 모멘텀(회의 코드 C1) — EW(headline) / VA(ablation)
    e, t = ec("cross_momentum_equalweight")
    rows.append(row_from_meta(c1["ew"], study_id="BT-11", strategy="횡단면 모멘텀(동일가중)",
                              event="횡단면 12-1 모멘텀 · 동일가중", window=win,
                              honesty="honest_failure", caveat=CV_COMMON,
                              equity_csv=e, trades_csv=t,
                              source="11_signal_axes/cross_momentum_run_meta.json"))
    e, t = ec("cross_momentum_voladj")
    rows.append(row_from_meta(c1["va"], study_id="BT-11", strategy="횡단면 모멘텀(위험조정랭킹)",
                              event="횡단면 12-1 모멘텀 · 1/σ 위험조정 랭킹", window=win,
                              honesty="honest_failure", caveat=CV_COMMON,
                              equity_csv=e, trades_csv=t,
                              source="11_signal_axes/cross_momentum_run_meta.json"))
    # 20일 채널 돌파(회의 코드 C3, 파일/키 stem: donchian_breakout)
    e, t = ec("donchian_breakout")
    rows.append(row_from_meta(c3["main"], study_id="BT-11", strategy="채널 돌파",
                              event="20일 채널 돌파(시계열)", window=win,
                              honesty="honest_failure",
                              caveat="2022 홀드아웃에서 매수 후 보유 대비 -10%p 크게 뒤짐(잦은 반전). " + CV_COMMON,
                              equity_csv=e, trades_csv=t,
                              source="11_signal_axes/donchian_breakout_run_meta.json"))
    # 단기 역추세(회의 코드 C4) — A(무필터) / B(눌림 필터)
    e, t = ec("mean_reversion_nofilter")
    rows.append(row_from_meta(c4["A"], study_id="BT-11", strategy="단기 역추세(무필터)",
                              event="단기 역추세 · 필터 없음", window=win,
                              honesty="honest_failure", caveat=CV_COMMON,
                              equity_csv=e, trades_csv=t,
                              source="11_signal_axes/mean_reversion_run_meta.json"))
    e, t = ec("mean_reversion_filter")
    rows.append(row_from_meta(c4["B"], study_id="BT-11", strategy="단기 역추세(눌림 필터)",
                              event="단기 역추세 · 눌림필터(반등양봉+거래량)", window=win,
                              honesty="honest_failure", caveat=CV_C4B,
                              equity_csv=e, trades_csv=t,
                              source="11_signal_axes/mean_reversion_run_meta.json"))
    return folder, rows


def main():
    total = 0
    for builder in (build_bt11,):
        folder, rows = builder()
        write_metrics_rows(str(STUDIES / folder / "metrics.json"), rows)
        total += len(rows)
    print(f"✅ 종목레벨 스터디 백필 완료: {total}행")


if __name__ == "__main__":
    main()
