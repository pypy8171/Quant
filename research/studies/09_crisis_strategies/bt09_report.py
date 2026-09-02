#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""BT-09 리포트 계층 — 평가 결과(dict)를 README.md 마크다운으로 직렬화.

엔트리(backtest_crisis_strategies.main)가 eval_benchmark/run_sweep 산출물을 넘기면
write_readme 가 정직성 배너·전략표·1차지표·홀드아웃·비용감도·커버리지·이벤트별·스윕·
용어·경계 섹션을 한 파일로 쓴다. 계산은 하지 않는다(표현 전용).
"""
import sys
from pathlib import Path

# ── 부트스트랩(멱등): BT-08 엔진(fmt/median/WARMUP) 배선 ──────────────────────
_HERE = Path(__file__).resolve()
_REPO = _HERE.parents[3]
_PYQ = _REPO / "PYQuant"
_BT08 = _REPO / "research" / "studies" / "08_crisis_response"
for _p in (str(_PYQ), str(_BT08)):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import backtest_crisis_response as bt08                       # noqa: E402
from bt09_signals import SIGNAL_TICKERS, TODAY                # noqa: E402
from bt09_strategies import STRATS                            # noqa: E402

fmt    = bt08.fmt
median = bt08.median
WARMUP = bt08.WARMUP


def write_readme(bms, results, sweeps, readme_path):
    L = []
    L.append("# 위기 대응·수익추구 전략 10종 인과 백테스트 (BT-09)\n")
    L.append("> ⚠️ **정직성 배너.** BT-08의 방어 5종(M1~M5)을 넘어, 시장 도메인 지식(지표·이벤트·"
             "크로스에셋 연관)에서 도출한 **방어 5 + 공세 5 = 10전략**을 동일 규율로 검증한다. "
             "익스포저 e[t]는 **오직 t-1 종가까지의 정보**(수익=e[t-1]×지수수익). "
             "**US→KR 시차**: KR 벤치마크에선 US계열 신호(^GSPC·^VIX·^TNX·^SOX·CL=F·DXY)를 "
             "직전 세션값으로 정렬(미국장이 한국장 뒤 마감 → 같은날 US종가는 룩어헤드). "
             "**매크로는 신호전용** — 수익곡선은 오직 거래대상 지수(금리·유가로 곡선 안 만듦). "
             "결측신호 → 중립(e=1)+커버리지 명시. 1차 지표=단일곡선 Calmar 1개, "
             "이벤트별은 진단·중앙값만. 임계값 사전등록+전량스윕, 홀드아웃(격리검증) 2022 잠금, "
             f"비용 0.21/0.5/1.0% 감도. yfinance 무키. 생성일 {TODAY}.\n")

    # 설계 논의
    L.append("## 논의(strategist ↔ reviewer) → 중재 핵심\n")
    L.append("- **원인구분(cause-differentiation)**: 유가↑가 금리↑와 **동반**이면 공급충격(비용상승·"
             "방어 C1), 금리는 잠잠한데 유가만↑이면 수요견인(리스크온·공세 O4). 같은 유가상승도 "
             "금리 동조 방향으로 성격이 갈린다.\n")
    L.append("- **국면 의존성**: 추세장에선 추세추종, 횡보·약세장에선 평균회귀 — 하나의 규칙이 "
             "두 국면에서 반대로 작동(O1). BT-06 '레짐필터 양날의검'의 계승.\n")
    L.append("- **크로스에셋 확인은 사전방어가 아니다**(reviewer 경고): VIX·DXY·KRW 동시 리스크오프"
             "(C2)나 롤오버 재진입(O5)은 **동행·반응** 신호다. '위기 전에 미리 안다'가 아니라 "
             "'이미 벌어지는 리스크오프에 규율 있게 반응'으로만 주장한다.\n")
    L.append("- **데이터 floor 정직**: VIX 1990·CL=F 2000-08·KRW 2003-12·SOX 1994. 이전 위기"
             "(1997 IMF·1998 등)엔 해당 신호가 없어 **그 구간 중립(e=1)** — 방어를 주장하지 않는다.\n")

    # 전략표
    L.append("\n## 전략 10종 (방어 5 · 공세 5)\n")
    L.append("| 코드 | 구분 | 전략 | 트리거(후행·신호전용) | 필요 데이터 |")
    L.append("|---|---|---|---|---|")
    for code, label, fn, side, desc, needs in STRATS:
        L.append(f"| {code} | {side} | {label} | {desc} | {needs} |")

    # full-curve
    for bm in bms:
        r = results[bm["name"]]
        bh = r["bh"]
        L.append(f"\n## 1차 지표 — 단일 연결곡선 · {bm['name']} ({bm['span']}, {bm['nbars']}봉)\n")
        L.append(f"> Buy&Hold: 연복리(CAGR) {fmt(bh['cagr'],2)}% · 최대낙폭(MDD) {fmt(bh['mdd'])}% · "
                 f"샤프(위험조정수익) {fmt(bh['sharpe'],2)} · **Calmar {fmt(bh['calmar'],2)}**\n")
        L.append("| 전략 | 구분 | CAGR% | MDD% | Sharpe | **Calmar** | 낙폭축소%p | 초과CAGR%p | 활성% | 토글 |")
        L.append("|---|---|---|---|---|---|---|---|---|---|")
        for row in r["rows"]:
            b = row["base"]
            L.append(f"| {row['code']} {row['label']} | {row['side']} | {fmt(b['cagr'],2)} | "
                     f"{fmt(b['mdd'])} | {fmt(b['sharpe'],2)} | **{fmt(b['calmar'],2)}** | "
                     f"{fmt(row['mdd_red'])} | {fmt(row['cagr_delta'],2)} | "
                     f"{fmt(row['active'],0)} | {row['toggles']} |")
        L.append("\n*낙폭축소%p=|BH MDD|−|전략 MDD|(+면 방어). 초과CAGR%p=전략−BH 연율수익"
                 "(+면 초과수익). 활성%=신호가 발동해 e≠1인 날 비중(낮으면 대부분 BH와 동일=희소이벤트). "
                 "토글=익스포저 급변 횟수(휘프소 대리).*\n")

        # 홀드아웃
        L.append(f"\n### 홀드아웃 격리 · {bm['name']} (2022 잠금, Calmar)\n")
        tb = r["train_bh"]; hb = r["hold_bh"]
        L.append(f"> 참고 BH — train(2022제외) Calmar {fmt(tb['calmar'],2) if tb else 'NA'} · "
                 f"holdout(2022) Calmar {fmt(hb['calmar'],2) if hb else 'NA'}\n")
        L.append("| 전략 | train Calmar | holdout(2022) Calmar | train CAGR% | holdout CAGR% |")
        L.append("|---|---|---|---|---|")
        for row in r["rows"]:
            tr, ho = row["train"], row["hold"]
            L.append(f"| {row['code']} | {fmt(tr['calmar'],2) if tr else 'NA'} | "
                     f"{fmt(ho['calmar'],2) if ho else 'NA'} | "
                     f"{fmt(tr['cagr'],2) if tr else 'NA'} | {fmt(ho['cagr'],2) if ho else 'NA'} |")
        L.append("\n*2022는 손대지 않고 잠가둔 구간. train과 holdout의 Calmar 부호·크기가 "
                 "일관되면 곡선맞춤이 아닐 가능성↑, 크게 어긋나면 in-sample 튜닝 신호.*\n")

        # 비용감도
        L.append(f"\n### 거래비용 감도 · {bm['name']} (Calmar)\n")
        L.append("| 전략 | 0.21% | 0.5% | 1.0% |")
        L.append("|---|---|---|---|")
        for row in r["rows"]:
            cs = row["by_cost"]
            L.append(f"| {row['code']} | {fmt(cs[0.0021]['calmar'],2)} | "
                     f"{fmt(cs[0.005]['calmar'],2)} | {fmt(cs[0.010]['calmar'],2)} |")

        # 커버리지
        L.append(f"\n### 신호 커버리지 · {bm['name']} (해당날짜에 신호값 존재비율%)\n")
        cov = r["cov"]
        L.append("| " + " | ".join(SIGNAL_TICKERS.keys()) + " |")
        L.append("|" + "|".join(["---"] * len(SIGNAL_TICKERS)) + "|")
        L.append("| " + " | ".join(fmt(cov[k], 0) for k in SIGNAL_TICKERS) + " |")
        L.append("\n*낮은 커버리지 = 그 신호를 쓰는 전략이 대부분 구간에서 중립(e=1)이었다는 뜻. "
                 "예: KRW=X는 2003-12부터라 그 이전 KR 위기에서 C2가 중립.*\n")

    # 이벤트별
    for bm in bms:
        r = results[bm["name"]]
        if not r["ev_rows"]:
            continue
        L.append(f"\n## 이벤트별 진단 · {bm['name']} (기본비용, 진단용 — best 셀 판정 금지)\n")
        L.append("방어(C*)=**낙폭축소%p**(+면 덜 빠짐) / 공세(O*)=**초과수익%p**(+면 BH초과). "
                 "trough 앵커는 hindsight·평가전용.\n")
        codes = [c for c, *_ in STRATS]
        L.append("| 이벤트 | 거동 | BH낙폭% | BH수익% | " + " | ".join(codes) + " |")
        L.append("|---|---|---|---|" + "|".join(["---"] * len(codes)) + "|")
        agg = {c: [] for c in codes}
        side_of = {c: s for c, _, _, s, _, _ in STRATS}
        for row in r["ev_rows"]:
            cells = []
            for c in codes:
                m = row["methods"].get(c)
                if not m:
                    cells.append("NA"); continue
                val = m["mdd_red"] if side_of[c] == "방어" else m["excess"]
                agg[c].append(val)
                cells.append(fmt(val))
            L.append(f"| {row['eid']} | {row['behavior']} | {fmt(row['bh_dd'])} | "
                     f"{fmt(row['bh_tot'])} | " + " | ".join(cells) + " |")
        L.append("| **중앙값** | — | — | — | " +
                 " | ".join(fmt(median(agg[c])) for c in codes) + " |")
        L.append("\n*방어전략은 낙폭축소 중앙값이 +일수록, 공세전략은 초과수익 중앙값이 +일수록 "
                 "'해당 성격의 위기에서' 값어치. 단 실질표본 n≈6~9라 단일 이벤트 과대해석 금지.*\n")

    # 스윕
    for bm in bms:
        sw = sweeps[bm["name"]]
        L.append(f"\n## 임계값 사전등록 스윕 · {bm['name']} (full-curve Calmar, 전량공개)\n")
        L.append("| 전략 | 파라미터 | Calmar | MDD% | 총수익% |")
        L.append("|---|---|---|---|---|")
        for code, param, cal, mdd, tot in sw:
            L.append(f"| {code} | {param} | {fmt(cal,2)} | {fmt(mdd)} | {fmt(tot)} |")
        L.append("\n*여러 파라미터 Calmar 분산 그대로 노출. 특정 셀이 높다고 엣지가 아니라 "
                 "소표본 곡선맞춤일 수 있음.*\n")

    # 용어
    L.append("\n## 용어 설명\n")
    L.append("- **익스포저(exposure) e**: 위험자산 보유배수(0=현금, 1=100%, 1.2=완만 레버리지). 전략은 이 값을 조절.")
    L.append("- **방어/공세**: 방어=위험시 e를 낮춰 낙폭축소, 공세=기회시 e를 1.2까지 높여 초과수익 추구.")
    L.append("- **MDD·CAGR·Sharpe·Calmar**: 최대낙폭 / 연복리수익 / 위험대비효율 / CAGR÷|MDD|(1차 지표).")
    L.append("- **낙폭축소%p**: |BH MDD|−|전략 MDD|. +면 그만큼 덜 빠짐(방어 값어치).")
    L.append("- **초과CAGR%p**: 전략 CAGR − BH CAGR. +면 BH보다 매년 더 벎(공세 값어치).")
    L.append("- **활성%**: 신호가 실제 발동해 e≠1이던 날 비중. 낮으면 희소이벤트 전략(대부분 BH와 동일).")
    L.append("- **커버리지**: 그 신호값이 존재한 날 비율. 데이터 시작연도·다운로드 결측을 그대로 노출.")
    L.append("- **홀드아웃(holdout)**: 튜닝에 쓰지 않고 잠가둔 검증구간(여기선 2022). train과 크게 어긋나면 과최적화 신호.")
    L.append("- **원인구분**: 같은 자산변동도 다른 자산의 동조 방향으로 원인(공급/수요·리스크온/오프)을 나눠 대응.")
    L.append("- **US→KR 시차**: 미국장이 한국장 뒤 마감 → 같은날 US종가를 KR 신호로 쓰면 1일 룩어헤드. 직전세션값으로 정렬해 차단.")
    L.append("- **동행/반응 vs 사전**: VIX·DXY·KRW 신호는 위기와 함께 움직이는 동행·반응 지표. '미리 안다'가 아니라 '규율 있게 반응'으로만 해석.")

    # 경계
    L.append("\n## 데이터·방법 경계(정직성)\n")
    for bm in bms:
        L.append(f"- **{bm['name']}**: {bm['span']}, {bm['nbars']}봉(~{bm['bpy']:.0f}/년). "
                 f"워밍업 {WARMUP}봉 e=1.")
    L.append("- **신호 가용 시작**: ^VIX 1990 · CL=F 2000-08 · KRW=X 2003-12 · ^SOX 1994 · "
             "DXY 1985 · ^TNX 1985(raw%). 이전 구간은 해당 신호 결측 → 중립(e=1). "
             "따라서 **1997 IMF·1998 등엔 KRW·oil 기반 전략(C2·O5·C1·O4)이 정직하게 침묵**한다.")
    L.append("- **CL=F 2020-04-20 음수(-37$) 종가는 c<=0 필터로 드롭** → 2020년 유가 20일변화가 "
             "그 부근에서 왜곡. **oil 전략(C1·O4)의 2020 신호는 신뢰불가**로 간주.")
    L.append("- **크로스에셋 확인(C2·O5)은 사전방어가 아님** — 동행·반응. 'US-매크로→KR' 성능은 "
             "직전세션 정렬(시차 반영)로만 보고. 무시차판은 룩어헤드로 과대추정.")
    L.append("- **공세 e≤1.2 완만 레버리지** — 조달비용·증거금·갭리스크 미반영(낙관편향 가능). "
             "비용감도 1.0%를 보수적 하한 참조.")
    L.append("- **자유도**: 10전략 임계값 다수 vs 성격별 실질표본 n≈6~9. 사전등록+전량스윕+홀드아웃"
             "(2022)으로 곡선맞춤을 노출하되 '발견'이 아닌 '규율검증'으로 프레이밍.")
    L.append(f"\n---\n생성 스크립트: `research/studies/09_crisis_strategies/backtest_crisis_strategies.py`"
             f"(단독실행, IndexSource 전용, BT-08 엔진 재사용, 결정론적 재현). 관련: BT-06·BT-07·BT-08.\n")

    readme_path.write_text("\n".join(L), encoding="utf-8")
