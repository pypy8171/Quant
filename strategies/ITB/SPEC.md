# ITB v2 — 거래대금 스캔 기반 장중 매매 확정 스펙

> 협의체(전략·데이터·리스크) 2026-08-06 결론. 재현 가능한 형태로 정리한다.
> 검증: 내일(다음 개장) 09:00 체 세션 forward 관찰. **백테스트 이력 0 — 모든 임계값은 논리 기반 초기값이지 최적화값이 아니다.**

## 배경 — 왜 v2인가
- v1(ITB) 실증에서 두 문제:
  1. 매수 유니버스 = 보유종목뿐(`universe_from_balance`) → 신규매수 후보 없음. "팔기만" 함.
  2. `avg_loss_pct=0.03`이 이미 -30~45% 물린 보유분을 개장 첫 틱 시장가 투매 유발 → 최악 유동성에서 손실 확정(자해).
- 리스크 에이전트가 코드에서 찾은 **치명 결함 2건**(아래 §0)이 "실제 돈 규율"을 무력화 중.

---

## §0. 안전 선결(SAFETY) — 매수/매도 기준보다 우선. 이거 없으면 규율 성립 안 함.

### C-1 (치명): rest_price_feed 모드에서 원장이 死
- 근거: `Engine.cpp:143` — `if (!rest_price_feed_ && !watch_specs_.empty())` 안에서만 WS 연결 + `set_fill_callback`. rest 모드면 **체결콜백 미등록** → `OrderRouter::on_fill` 영원히 미호출.
- 결과: `daily_pnl_` 0 고정(`OrderGate.cpp:76` 손실컷 절대 미발동), `positions_`/`avg_prices_`/`reserved_` 갱신 안 됨.
- 수정: rest 모드에서 **주기적 잔고 재조회(`get_balance`)로 원장 리컨사일** — `positions_`/`avg_prices_` 재동기 + 일중 평가금 델타로 daily-loss 근사. (완전한 fill 이벤트 합성은 후순위, 우선 리컨사일)

### C-2 (치명): 청산 SELL이 튕기면 영구 방치
- 근거: `IntradayBreakoutStrategy.h:117-124` — 스탑 히트 시 SELL 신호 반환 **전에** `in_position_=false; hold_qty_=0`. 거부되면 `OrderRouter::new_route`가 REJECTED 후 drop(재큐잉 없음, `OrderRouter.cpp:118-128`). C++엔 EGW00201 재시도 없음(Python `forward_trader.py`에만 존재).
- 결과: 오늘 006800·042700이 초당한도로 튕겼고 그 종목은 손절 실패한 채 관리 이탈.
- 수정:
  1. **order_thread 페이싱**(`Engine.cpp:427-444`) — 주문 간 최소 간격 250ms(=4/s, 게이트 5/s 아래). 버스트 자체를 없앰.
  2. **거부된 청산 SELL 재무장** — 전략이 SELL 접수 확인 전까지 `in_position_` 유지하거나, OrderRouter가 rate-limit 거부 SELL을 재큐잉.

### C-3: 손익 기반 자동 킬스위치 없음
- 근거: `set_kill_switch`는 ZMQ 수동 명령/WS 3연속 실패에만. 손익 기반 자동 킬 부재.
- 수정: 일중 손실 -3%(daily-loss -2%보다 harder) 도달 시 신규매수 전면중단(청산은 허용 — BUY-only 킬).

---

## §1. 매수 기준 (신규) — 거래대금 상위 스캔

| 단계 | 규칙 | config 키 | 초기값 | 데이터 근거 |
|------|------|-----------|--------|------------|
| 유니버스 | 거래대금 상위 N | `scan_top_n` | 30 | volume-rank API |
| 필터① 등락률 | `chg_min ≤ 당일등락률 ≤ chg_max` | `chg_min`/`chg_max` | +0.02 / +0.12 | 이미 강세=모멘텀 / 급등 추격금지 |
| 필터② 가격 | `price ≥ min_price` | `min_price` | 3000 | 동전주·호가스프레드 배제 |
| 필터③ 수급(opt) | 외국인 **T-1 확정** 순매수 > 0 | `sd_filter` | true | 장중값은 잠정치 → 전일확정만, 후보 소수에만 조회 |
| 트리거 | 당일시가 앵커 대비 채널 돌파(완결 1분버킷만) + `>anchor×(1+eps)` | `channel_min`/`breakout_eps`/`anchor_mode` | 5 / 0.002 / "day_open" | 기동시점 독립·자기참조 방지 |
| 레짐 게이트 | 코스피 당일 등락률 `< risk_off_index_pct`면 신규매수 전면중단 | `risk_off_index_pct` | -0.01 | 200MA 레짐 무력 → 지수 등락률 fallback |

**데이터 확정(data-sourcer):**
- 현재 `fetch_kr_ranking`은 **시총순위(FHPST01720000) + `price×volume` 근사** → 부정확. **volume-rank API(`/uapi/domestic-stock/v1/quotations/volume-rank`, tr_id `FHPST01710000`, `FID_BLNG_CLS_CODE=3` 거래금액순)** 로 교체, 응답 `acml_tr_pbmn`(누적거래대금) 직접 사용.
- 이 API는 **상위 30행 고정 반환**(페이지네이션 없음). "거래대금 상위 30"은 단일 콜로 확보. N>30은 깔끔한 경로 없음.
- ⚠️ 행수·필드명은 실전 도메인으로 1콜 찍어 확정 필요(스키마 변동 잦음).

---

## §2. 매도 기준 (재설계) — 물린분 ≠ 신규분

### (A) 물린 보유분 (start_in_position=true, 앵커≫평단)
- `avg_loss_pct = 0` (**비활성**) — 이미 -30% 아래인데 -3% 스탑은 개장 즉시투매.
- 당일 앵커(시가) 트레일만, 넓게: `seed_trail_pct ≈ 0.035` (당일 고점 대비 -3.5%).
- 본전 탈출 익절: `px ≥ avg_px × (1 - exit_near_avg_pct)`(평단 -2% 이내) 도달 시 청산 — 개장 투매 아니라 **당일 반등에 실어 던지기**.
- EOD 유지.

### (B) 신규 진입분 (start_in_position=false, entry=돌파가)
| 항목 | v1 | v2 | 근거 |
|------|----|----|------|
| trail_pct | 0.01 | **0.028** | 30초폴링+정상 되돌림 한 틱에 털림 |
| hard_pct | 0.015 | **0.019** | 왕복비 0.21%+시장가 슬리피지 마진 |
| avg_loss_pct | 0.03 | **0** | 신규분 entry=평단이라 hard와 중복 |
| 신규진입 금지 | eod(1515) | `no_new_entry_hhmm=1500` | 마감30분 진입은 트레일 발동 전 EOD 강제청산 |
- 선택: +4~5% 도달 시 절반 익절(부분청산) — 후순위.

---

## §3. 포지션 사이징 (실제 돈)

| 항목 | config 키 | 초기값 | 비고 |
|------|-----------|--------|------|
| 종목당 명목 | `notional_per_position` | 700,000 | `entry_qty = max(1, floor(명목/현재가))`. 1주 고정 폐기 |
| 동시보유 상한 | `max_concurrent_positions` | 3 | 전역 카운터(전략 인스턴스 간 공유 필요) |
| 총노출 상한 | `max_total_notional` | 계좌 50% | 슬리피지/미체결 버퍼 남김 |
| 일손실 신규정지 | `daily_loss_limit` | 계좌 -2% | BUY-only(청산 허용). C-1 선결 필수 |
| 킬스위치 | `kill_loss_limit` | 계좌 -3% | daily보다 harder |
- OrderGate 현재 하드코딩(`OrderGate.h:31-42`): max_qty_per_ticker=100, daily_loss_limit=-300,000, 5/s·20/min, max_notional_per_order=50M. **config `"risk"` 블록으로 노출**(`main.cpp`에 파서 신설).
- 켈리·변동성타게팅은 표본 0이라 보류. 고정명목+종목수 상한으로 시작, 20~30거래 후 재검토.

---

## §4. 레이트리밋 예산 (data-sourcer 확정)
- 실전 도메인 시세 20/s 하드캡. 자기부과 150ms/종목 = 6.67/s(안전).
- 30초 사이클 = 200 슬롯. 공식 **N_price + K_수급 + 3(랭킹1+지수2) ≤ 200**.
- **N=150 권장.** 현재 유니버스(스캔 30 + 물린보유 ~18)면 여유. 수급은 후보 소수에만.
- volume-rank 1콜/사이클(정렬 API가 해줌 → `price×volume` 재정렬 제거).

---

## §5. 과적합·표본 경계
- 백테스트 불가(거래대금 순위 스냅·수급 무료 과거경로 없음, MDC 차단). **forward 관찰만이 검증 수단.**
- 수급필터·레짐 fallback은 **ablation(on/off) 병행 관찰** 필수. 수급 순기여 마이너스 가능.
- 라이브 20~30거래 쌓이기 전 파라미터 손대지 말 것(변수 하나씩).
- 개선 우선순위: ① MDD/변동성 통제 → ② 신호 정밀도 → ③ 새 피처.

---

## §6. 구현 체크리스트 (파일 지점)
- [ ] C-1: `Engine.cpp` rest 모드 잔고 리컨사일 훅
- [ ] C-2: `Engine.cpp:427` order_thread 페이싱 + 거부 SELL 재무장
- [ ] C-3: 손익 킬스위치 (Engine 손익 모니터 → `set_kill_switch`)
- [ ] volume-rank: `KisClient.h/.cpp` `fetch_value_ranking(FHPST01710000, FID_BLNG_CLS_CODE=3)`, RankingStock에 `trade_value` 추가
- [ ] 스캔 분기: `main.cpp:~947` `universe_from_scan` 신설(필터→수급→레짐→등록 start_in_position=false, 150~200ms 페이싱)
- [ ] 전략: `IntradayBreakoutStrategy.h` day_open 앵커 주입, seed_trail_pct/exit_near_avg_pct 분기, no_new_entry_hhmm 분리, 명목→수량
- [ ] risk 블록: `main.cpp` `"risk"` 파서 → `OrderGate::Config` 주입
- [ ] `config_itb_paper.json` v2 파라미터 반영 + 빌드
