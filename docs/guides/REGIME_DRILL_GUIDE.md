# 국면 드릴 가이드 — 강제청산·BULL/BEAR 분기 검증

라이브에서 한 번도 실행되지 않은 두 경로를 모의계좌에서 의도적으로 태우는 절차다.
배경과 미룬 경위는 [DEFERRED_ISSUES D-15](../DEFERRED_ISSUES.md), 배선 결정은
[DECISIONS D-021](../DECISIONS.md).

## 국면 축이 둘이라는 점부터

| 축 | 입력 | 하는 일 | 청산 |
|---|---|---|---|
| `RegimeController` | 코스피 지수 일봉 | 국면별로 활성 전략 집합을 고른다 | 안 한다 |
| `regime.json` 파일 전달 | 매크로 사이드카 | `entry_halt`·`force_liquidate` | `force_liquidate`가 한다 |

`RegimeController`가 BEAR를 내도 보유분은 청산되지 않는다. 진입만 막힌다.
전량 청산은 `regime.json`의 `force_liquidate`뿐이다. 그래서 드릴도 둘로 나뉜다.

## 공통 전제

- **모의계좌 전용.** `Quant/config/config_dev_paper.json` (`kis.is_paper=true`). 실계좌에서는 하지 않는다.
- **장중에만.** 시장가 주문이라 장이 열려 있어야 체결된다.
- **repo 루트에서 실행.** cwd가 다르면 유니버스가 붕괴한다.
- 드릴 전후로 로그를 따로 보관한다: `logs/quant_trader.log` → `logs/archive/`.

---

## D-15a — 강제청산(FORCE_LIQ) 드릴

코드 변경이 필요 없다. 파일 하나를 쓰면 된다.

### 준비

1. 매크로 사이드카(`macro_regime_feed.py`)를 **내린다**. 켜져 있으면 다음 주기에 `regime.json`을 덮어쓴다.
2. 오전에 정상 매매로 보유 종목을 몇 개 만든다. 당일 매수분이 섞여야 D-11(매도가능수량 미클램프)이 함께 드러난다.

### 발동

`Quant/config/regime.json`에 쓴다. `valid`가 false거나 파일이 `regime_stale_sec`(모의 설정 600초)보다
오래되면 엔진이 통째로 무시하므로, **파일을 새로 써서 mtime을 갱신**해야 한다.

```json
{"valid": true, "regime": "DRILL", "risk_score": -99, "entry_halt": true, "force_liquidate": true}
```

### 관측 항목

| # | 확인할 것 | 코드 위치 |
|---|---|---|
| 1 | `[Regime] force_liquidate=TRUE` ERROR가 **1회만** | [Engine.cpp::poll_regime_file](../../Quant/src/core/Engine.cpp) |
| 2 | `entry_halt`가 OR로 함께 켜지는가 | [Engine.cpp::poll_regime_file](../../Quant/src/core/Engine.cpp) |
| 3 | 보유 종목마다 SELL/MARKET, 수량 = 보유 − 미체결매도 | [Engine.cpp::strategy_thread_fn](../../Quant/src/core/Engine.cpp) |
| 4 | `ref_price`에 평단이 stamp되는가 | [Engine.cpp::strategy_thread_fn](../../Quant/src/core/Engine.cpp) |
| 5 | 게이트가 SELL을 통과시키는가(BUY만 차단) | [OrderGate.cpp::check](../../Quant/src/risk/OrderGate.cpp) |
| 6 | 명목 백스톱이 `ref_price`로 평가되는가(시장가 우회 없음) | [OrderGate.cpp::check](../../Quant/src/risk/OrderGate.cpp) |
| 7 | 잔량이 남으면 2초 간격 재발주, 비면 멈추는가 | [Engine.cpp::strategy_thread_fn](../../Quant/src/core/Engine.cpp) |
| 8 | 원장에 실현손익이 채워지는가 | 매매원장 CSV |

### 곁가지로 드러나는 D-11

3번의 수량은 `h.qty - sell_pending`이다. 증권사 매도가능수량이 아니다.
당일 매수분은 매도가능수량이 보유수량보다 작으므로 "오전 매수 → 오후 청산" 순서로 짜면
거부가 재현된다. 드릴 하나로 D-15a와 D-11을 같이 본다.

### 해제

```json
{"valid": true, "regime": "NORMAL", "risk_score": 0, "entry_halt": false, "force_liquidate": false}
```

`[Regime] force_liquidate 해제` WARN이 뜨고 재발주가 멈추는지 본다.
그 다음 사이드카를 다시 올린다.

> 파일이 갱신 끊김 한도를 넘기면 `poll_regime_file()`이 `force_liquidate_.store()` 전에 return하므로
> 플래그가 직전 값(TRUE)에 머문다. 해제할 때도 반드시 새로 써야 한다.

---

## D-15b — BULL/BEAR 전략선택 분기

2026-09-07에 `regime_tuning` 배선이 들어가면서 태울 수 있게 됐다(D-021).

### 원리

코스피 실측 score는 1이다(종가>200MA에서 +1, 이평 혼조로 0).
`classify()`가 BULL을 먼저 보므로 임계값을 이렇게 준다.

| 목표 | 설정 | 결과 |
|---|---|---|
| BULL | `score_bull_threshold: 1` | 1 >= 1 → BULL |
| BEAR | `score_bear_threshold: 1` (bull은 기본 2 유지) | 1 < 2, 1 <= 1 → BEAR |

가짜 봉도 우회 경로도 없다. 실제 시장 데이터가 그대로 분기를 지나간다.

### 설정

`config_dev_paper.json`에 넣는다.

```json
"regime_tuning": { "score_bull_threshold": 1 }
```

### 관측 항목

- 기동 로그 `[Main] 국면 판정 파라미터 — …` 에 유효값이 찍히는가
- 기본값과 다르면 `[Main] 국면 점수 임계값 오버라이드` WARN이 붙는가
- `apply_regime_selection()`이 `regime_strategies`의 해당 국면 목록으로 전략셋을 바꾸는가
- BEAR 케이스에서 **청산이 일어나지 않는지**(진입만 막혀야 정상). 여기서 청산이 나오면 축 설계가 깨진 것이다
- `regime_reeval_sec` 주기 재평가에서 같은 국면이 유지되는가

### 안전장치

- 실계좌(`is_paper=false`)에서는 임계값 오버라이드를 무시하고 기본값으로 되돌린다(ERROR 기록).
  지수코드·이평 기간은 정당한 튜닝이라 막지 않는다.
- `bull <= bear`면 NEUTRAL이 도달 불능이 되므로 기본값으로 되돌린다.

### 끝나면

`regime_tuning` 블록을 지운다. 미지정이 기본값이라 지우는 것만으로 원복된다.
