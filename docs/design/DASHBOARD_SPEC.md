# DASHBOARD_SPEC — 관측/대시보드 설계 (백테스트 결과 + 실시간 장중)

> 목적: 두 대시보드(**① 백테스트 결과**, **② 실시간 장중 모니터링**)의 설계를 하나의 문서로 확정한다.
> 대원칙: **대시보드는 정규화된 데이터 계약만 읽는다.** 엔진(C++/Python)과 렌더링을 분리(decouple)해,
> 라이브 데이터 소스를 프론트 변경 없이 `파일 → ZMQ → TimescaleDB`로 교체할 수 있게 한다.
>
> 확정일 2026-08-18 (설계 회의). 관련 로드맵: [PROJECT_GUIDE](../guides/PROJECT_GUIDE.md).

---

## 0. 데이터 계약이 먼저인 이유

정찰 결과, 두 대시보드의 공통 병목은 렌더링이 아니라 **정규화된 데이터가 없다는 것**이다.

| | 백테스트 계열 | 실시간 장중 계열 |
|---|---|---|
| **바로 쓸 수 있는 것** | 계열 A(BT-01/02/03/06)의 `equity/trades/holdings` **3-파일 CSV는 표준화**됨 → 곡선·체결 즉시 소비 | `logs/trades_YYYYMMDD.csv`가 **완전 구조화**(`ts_kst,event,order_id,odno,strategy,ticker,side,type,order_qty,order_price,fill_qty,fill_price,status,reason`) |
| **없는 것(진짜 병목)** | **요약지표(Sharpe/MDD/승률/turnover)가 파일로 export 안 됨** — 콘솔 출력 + 손으로 쓴 md에만. 계열 B(위기 07/08/09)는 스키마 제각각, 08은 데이터파일 자체가 없음 | **포지션 평단·실현손익·스코어·entry_halt가 파일에 없음** — 인메모리 또는 ZMQ에만. `quant_trader.log`는 자유형식 텍스트라 파싱 부적합 |
| **함정** | `PYQuant/bt_*.json`은 확장자만 json, **실내용은 CSV** | `regime.json`은 엔진 **출력이 아니라 입력**(외부 사이드카 `macro_regime_feed.py`가 씀). ZMQ 브리지는 배선됐으나 `#ifdef HAS_ZMQ`로 **현재 빌드에서 꺼짐** |

따라서 **정규화 계약(스키마)을 먼저 정하고**, 대시보드는 그 계약만 읽는다.

---

## 1. 확정 아키텍처

```
[백테스트 엔진(Python)]  → metrics.json (계약①) + equity/trades CSV  ┐
                                                                      ├→ [대시보드]  FastAPI + 경량 HTML/JS
[라이브 엔진(C++)]       → state_snapshot.json (계약②) + trades CSV  ┘        · 백테스트 뷰 = 정적 HTML export 병행(무서버 열람·Artifact 공유)
                                                                             · 라이브 뷰 = 자동갱신(SSE/폴링)
```

- **렌더링 스택 = FastAPI + 경량 HTML/JS 단일 홈.** 라이브는 어차피 서버 프로세스가 필요하므로 그 프로세스가 백테스트 뷰도 서빙하므로 하네스와 같은 Python으로 스택이 수렴한다. 백테스트 뷰는 **정적 HTML로도 export**해 서버 없이 열람·공유 가능.
- **라이브 소스 교체 무손실**: 지금은 `state_snapshot.json` 폴링 → 이후 ZMQ SUB → 최종 TimescaleDB. 프론트는 계약만 보므로 불변.
- **Grafana + TimescaleDB는 MVP에 세우지 않는다**(front-loading 금지). Phase 3에서 라이브 보존·운영 등급이 필요해질 때 옵션으로 붙인다. (목표 아키텍처의 종착지.)

> **구현 현황(스펙과의 차이)**: 실제 라이브 MVP는 `scripts/dashboard_server.py`로 먼저 나왔고, 위 확정안과 일부 갈린다. FastAPI 대신 **의존성 0 stdlib `http.server`**를 쓰고, 계약②(`state_snapshot.json`)를 거치지 않고 **기존 소스를 직접 읽는다**: 잔고 REST(PYQuant `KisClient`), 국면 `regime.json`, 유니버스 `universe_scan.json`, `quant_trader.log` tail, 체결원장 `trades_YYYYMMDD.csv`, 거래대금 랭킹. 차트/랭킹은 PYQuant 신규 메서드(`get_chart_ohlcv`·`get_minute_ohlcv`·`get_volume_ranking`)를 소비하며 `/api/state`·`/api/chart`로 서빙한다. state_snapshot.json 계약은 아직 미배선이라, 프론트-소스 분리(계약만 읽기)는 이 서버에는 적용되지 않았다.

---

## 2. 데이터 계약 ① — 백테스트 `metrics.json` (`quant.metrics/v1`)

계열 A/B 전부를 **한 스키마**로 통일. 지금까지 md 산문에 흩어져 있던 헤드라인 지표를 파일로 승격.
구현: `PYQuant/backtest/report.py:export_metrics_json`. 실행당 1객체 파일(기존 per-run CSV와 동형), 대시보드/백필이 다수를 취합.

```jsonc
{
  "schema": "quant.metrics/v1",
  "study_id": "BT-06",              // 스터디 ID
  "strategy": "momentum",
  "family": "A_portfolio",          // A_portfolio(종목 포트폴리오) | B_overlay(지수 익스포저 오버레이) — 대시보드 비교군 분리
  "benchmark": "",                  // 계열 B의 오버레이 대상 지수(예: "US(^GSPC)"), A는 ""
  "event": "2022bear",              // 이벤트/구간명(계열 B) 또는 ""
  "window": "2022-01-01~2022-12-31",
  "start_date": "...", "end_date": "...",

  // ── 헤드라인 성과 ──
  "total_return": 10.0,             // %  (계열 B 단기 윈도우의 1차 지표)
  "cagr": 12.3,                     // %  연환산 — 짧은 이벤트 윈도우에선 민감(총수익률 우선)
  "sharpe": 1.2, "sortino": 1.5,
  "mdd": 5.88, "calmar": 2.1,       // calmar = cagr/mdd
  "win_rate": 55.0,                 // %
  "turnover": 3.42,                 // 연환산 회전율
  "turnover_is_proxy": true,        // 엔진이 직접 추적 안 하면 체결로그로 재구성한 프록시
  "n_trades": 42, "total_pnl": 1234567,

  // ── 벤치마크/알파 ──
  "bench_return": 4.0, "bench_mdd": 8.0, "bench_sharpe": 0.6,
  "alpha": 6.0, "kodex_return": 3.0, "regime_off": 2,

  // ── 정직성/검증 라벨 (편향 감사관 필수 필드) ──
  "oos_flag": false,                // out-of-sample 구간인가
  "holdout_flag": false,            // 홀드아웃 검증 결과인가
  "honesty_label": "robust",        // robust | honest_failure | overfit_suspect | unlabeled

  // ── 곡선·체결 링크(기존 CSV 재사용) ──
  "equity_csv_path": "2022bear_momentum_on.csv",
  "trades_csv_path": "2022bear_momentum_on_trades.csv"
}
```

**정직성 필드가 필수인 이유**: 대시보드가 보기 좋아지면 근거 없는 신뢰가 생긴다. BT-09는 홀드아웃(격리검증) 전패·정직한 실패가 다수인데(`research/studies/09_...`), 지표 카드만 크게 띄우면 그 맥락이 지워진다. `honesty_label`·`oos_flag`·`holdout_flag`를 스키마에 못 박아 카드가 맥락과 함께 표시되게 강제한다. → [GUARDRAILS](../../research/GUARDRAILS.md), `bias-auditor`.

**주의(연환산 민감도)**: `cagr`/`calmar`는 연환산 지표라 계열 B의 짧은 이벤트 윈도우(±2~10개월)에서 값이 커진다. 대시보드는 계열 B에 대해 **`total_return`을 1차 지표로** 표시하고 cagr/calmar는 참고로 둔다.

### 생성 방법
```bash
# 계열 A — 백테스트 실행 시 metrics.json 동시 생성
python PYQuant/main.py backtest --strategy momentum --from 2022-01-01 --to 2022-12-31 \
  --export out/2022bear_momentum.csv \
  --study BT-06 --event 2022bear --honesty robust --oos
#  → out/2022bear_momentum{,_trades,_holdings}.csv + out/2022bear_momentum_metrics.json
```
```bash
# 계열 B — 위기 연구 스크립트가 실행 시 metrics.json 배열 동시 생성(벤치마크×전략)
python research/studies/08_crisis_response/backtest_crisis_response.py    # → 08_.../metrics.json (14행: 7대응법×2벤치)
python research/studies/09_crisis_strategies/backtest_crisis_strategies.py # → 09_.../metrics.json (28행: 13전략+BH×2벤치)
#  ※ yfinance 네트워크 필요(^GSPC 1928~/^KS11 1996~). 산출은 quant.metrics/v1, family=B_overlay.
```

**계열 B 백필 규약** — 위기 연구는 종목 포트폴리오가 아니라 **지수 익스포저 0~1.2x 오버레이**라 계열 A와 직접 비교 불가. `family="B_overlay"`+`benchmark`로 대시보드가 비교군을 분리한다. 오버레이엔 무의미한 `win_rate`/`n_trades`는 `null`. `mdd`는 계열 A(양수)와 통일해 **양수 크기로 정규화**(연구 스크립트의 `curve_stats`는 음수 mdd → `abs()`). `alpha`=전략 연복리(CAGR)−BH CAGR(%p). BT-09는 홀드아웃(2022) 맥락을 `holdout_calmar`/`train_calmar` extra로 보존(예: C1 train +0.18 → holdout −0.71, 전패 사실이 카드에서 지워지지 않게). 전 행 `honesty_label="robust"`(look-ahead 삼중차단·홀드아웃 잠금으로 **방법론**은 견고, 승패는 지표값이 말함).

- **BT-07(07_crisis_regimes)은 제외** — 위기 국면 *특성화*(peak/trough/max-dd/회복일수/shape)이지 전략 성과가 아니라 `quant.metrics/v1` 스키마에 맞지 않는다. 대시보드에선 위기 배경 참조 데이터셋으로 별도 취급(성과표엔 넣지 않음).

---

## 3. 데이터 계약 ② — 라이브 `state_snapshot.json`

엔진이 N초마다 **원자적으로 write**(임시파일 → rename). 대시보드는 파일을 폴링. 의존성 0.
`logs/trades_YYYYMMDD.csv` tail(체결 스트림)로 보강.

```jsonc
{
  "schema": "quant.state/v1",
  "ts_kst": "2026-08-18T13:05:00+09:00",
  "market_open": true,
  "positions": [
    {"ticker":"005930","name":"삼성전자","net_qty":10,"avg_price":70000,
     "last_price":71500,"unrealized_pnl":15000,"realized_pnl":0}
  ],
  "account": {"equity":10500000,"cash":3000000,
              "total_realized_pnl":120000,"total_unrealized_pnl":15000},
  "regime": {"score":1,"label":"NEUTRAL","entry_halt":false,"force_liquidate":false},
  "strategies": [{"id":"ITB","last_signal":"NONE","state":"watching"}]
}
```

- **왜 스냅샷 파일인가**(ZMQ 대신): ZMQ 브리지는 배선돼 있으나 현재 빌드 OFF + vcpkg zeromq 설치·재빌드(한글 TEMP 빌드 마찰) 필요. 스냅샷 파일은 신규 C++ write 코드만으로 의존성 0·즉시 파싱. **ZMQ는 Phase 3에서 켠다**(publish 지점 TRADE/SIGNAL/ORDER/FILL/HEALTH는 이미 배선됨).
- 포지션 평단·실현/미실현 PnL·regime score·entry_halt는 현재 인메모리/ZMQ에만 있으므로, 이 스냅샷이 **유일한 온전한 라이브 관측점**이 된다.

---

## 4. 3-Phase 로드맵

| Phase | 내용 | 인프라 | 상태 |
|---|---|---|---|
| **P1 백테스트** | `report.py` `metrics.json` exporter + 계열 B 백필 → 백테스트 HTML 대시보드(전략×이벤트 지표표 + equity 곡선 오버레이 + 정직성 라벨) | 0 | exporter/CLI/문서 + 계열 B 백필(BT-08/09, 42행) **완료**, HTML **잔여** |
| **P2 라이브 MVP** | 엔진 `state_snapshot.json` write(N초) + FastAPI 라이브 뷰(자동갱신) + trades CSV tail | 0 | 예정 |
| **P3 로드맵 수렴** | ZMQ 켜기(vcpkg zeromq) → collector가 TimescaleDB 적재 → (선택)Grafana 운영 대시보드 | DB+Grafana | 예정 |

**사용자 결정(2026-08-18)**: 착수순서=백테스트 먼저 / 라이브 채널=`state_snapshot.json` / 렌더링=FastAPI+HTML.

---

## 5. Phase 1 진행 상황

- [x] `export_metrics_json` (`PYQuant/backtest/report.py`) — `quant.metrics/v1` 스키마, calmar/sortino/turnover 파생, 정직성 라벨.
- [x] `main.py backtest` 배선 — `--export` 시 `*_metrics.json` 동시 생성. CLI: `--study --event --honesty --oos --holdout`.
- [x] 계열 B 백필 — BT-08/09 스크립트에 `_emit_metrics` 훅 추가, 실행 시 `metrics.json` 배열(42행) 산출. `family=B_overlay`, 최대낙폭(MDD) 양수정규화, 홀드아웃 맥락 보존. **BT-07 제외**(특성화 데이터, 성과 아님).
- [x] 백테스트 HTML 대시보드 — `PYQuant/dashboard/build_dashboard.py`가 `metrics.json`(계열 A 단일객체 + 계열 B 배열)과 `research/dashboard/live.json`을 발견→**탭 분리**(백테스트/라이브 매매) 렌더. 백테스트 탭은 `family`별 지표표 + 초과CAGR(alpha) **인라인 막대**(0 중심, 그룹 최대치 스케일) + 홀드아웃 **배너 1건**(그룹 전패를 캡션으로 통합, 26개 반복 ⚠ 제거) + `honesty_label`·`caveat`(⚠ 툴팁 + 접이식 비고 각주). 가독성: sticky 전략열·zebra·빈 컬럼 제외(계열 A는 cagr/calmar/sortino/turnover 미보유 → 컬럼 제거). 자체완결 정적 HTML(CDN·외부참조 0, 3-state 테마 토큰, tabular-nums, 키보드 정렬·탭 토글) → `research/dashboard/dashboard.html`. Artifact 발행으로 공유링크 겸용.
- [x] 계열 A 백필 — `PYQuant/dashboard/backfill_series_a.py`: BACKTEST_LOG(실행 #1~#5, 큐레이션 상수·`source` 태그) + 06 `summary_{yf,datagokr}.tsv`(프로그램 파싱) → `studies/{01,02,03,06}/metrics.json`(46행). 재실행(DATA_GO_KR_KEY·시점정합(PIT) 유니버스) 대신 **기록된 결과를 표면화**(재현 리스크 회피). `honesty_label`에 `context_required`(맥락필수, 비참여/생존편향) 추가, `caveat` 필드로 편향 주석.
- [x] 라이브 백필 — `PYQuant/dashboard/backfill_live.py`: `logs/trades_*.csv` 일자별 롤업(상태·전략·종목) + `strategies/*/live/*.md` 카드(제목·전략·한줄) → `research/dashboard/live.json`(`quant.live/v1`). 체결가·손익은 원장(별도)이라 주문흐름만 집계(손익 미생성).
  - **잔여(후속)**: equity 곡선 오버레이(계열 B full-curve가 단일 정규화파일로 없음 — exporter에 곡선 덤프 추가 필요). 계열 A `--export` 재현 경로(현재는 기록 표면화 백필). P2 FastAPI 라이브 자동갱신(`state_snapshot.json`).
