# 국면 드릴 가이드 — 강제청산·전략선택 분기 검증

라이브에서 한 번도 실행되지 않은 두 경로를 모의계좌에서 의도적으로 태우는 절차다.
배경과 미룬 경위는 [DEFERRED_ISSUES D-15](../DEFERRED_ISSUES.md), 배선 결정은
[DECISIONS D-021](../DECISIONS.md).

## 국면 입력은 하나다

`regime.json`(매크로 보조 프로세스 `PYQuant/tools/macro_regime_feed.py`)의 라벨 RISK_ON/NEUTRAL/RISK_OFF가
전략 집합 선택·매수 비율·`entry_halt`·`force_liquidate`를 전부 낸다(D-084). 코스피 일봉으로 따로 판정하던
`RegimeController`는 09-14에 지웠다(D-085). 전략 선택은 진입만 켜고 끌 뿐 보유분을 청산하지 않는다 —
전량 청산은 `force_liquidate`뿐이다. 그래서 드릴도 둘로 나뉜다.

## 공통 전제

- **모의계좌 전용.** `Quant/config/config_dev_paper.json` (`kis.is_paper=true`). 실계좌에서는 하지 않는다.
- **장중에만.** 시장가 주문이라 장이 열려 있어야 체결된다.
- **repo 루트에서 실행.** cwd가 다르면 유니버스가 붕괴한다.
- 드릴 전후로 로그를 따로 보관한다: `logs/quant_trader.log` → `logs/archive/`.

---

## D-15a — 강제청산(FORCE_LIQ) 드릴

코드 변경이 필요 없다. 파일 하나를 쓰면 된다.

### 준비

1. 매크로 보조 프로세스(`macro_regime_feed.py`)를 **내린다**. 켜져 있으면 다음 주기에 `regime.json`을 덮어쓴다.
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
| 1 | `[Regime] force_liquidate=TRUE` ERROR가 **1회만** | [RegimeFileBridge.h::step](../../Quant/include/core/RegimeFileBridge.h) `log_liquidation_on`, 문구는 [Engine.cpp::poll_regime_file](../../Quant/src/core/Engine.cpp) |
| 2 | `entry_halt`가 OR로 함께 켜지는가 | [RegimeFileBridge.h::step](../../Quant/include/core/RegimeFileBridge.h) — `test_regime_bridge`가 고정 |
| 3 | 보유 종목마다 SELL/MARKET, 수량 = 보유 − 미체결매도 | [Engine.cpp::strategy_thread_fn](../../Quant/src/core/Engine.cpp) |
| 4 | `reference_price`에 평단이 stamp되는가 | [Engine.cpp::strategy_thread_fn](../../Quant/src/core/Engine.cpp) |
| 5 | 게이트가 SELL을 통과시키는가(BUY만 차단) | [OrderGate.cpp::check](../../Quant/src/risk/OrderGate.cpp) |
| 6 | 명목 백스톱이 `reference_price`로 평가되는가(시장가 우회 없음) | [OrderGate.cpp::check](../../Quant/src/risk/OrderGate.cpp) |
| 7 | 잔량이 남으면 2초 간격 재발주, 비면 멈추는가 | [Engine.cpp::strategy_thread_fn](../../Quant/src/core/Engine.cpp) |
| 8 | 원장에 실현손익이 채워지는가 | 매매원장 CSV |

### 곁가지로 드러나는 D-11

3번의 수량은 `holding.quantity - sell_pending`이다. 증권사 매도가능수량이 아니다.
당일 매수분은 매도가능수량이 보유수량보다 작으므로 "오전 매수 → 오후 청산" 순서로 짜면
거부가 재현된다. 드릴 하나로 D-15a와 D-11을 같이 본다.

### 해제

```json
{"valid": true, "regime": "NORMAL", "risk_score": 0, "entry_halt": false, "force_liquidate": false}
```

`[Regime] force_liquidate 해제` WARN이 뜨고 재발주가 멈추는지 본다.
그 다음 보조 프로세스를 다시 올린다.

> 파일이 갱신 끊김 한도를 넘기면 `poll_regime_file()`이 `force_liquidate_.store()` 전에 return하므로
> 플래그가 직전 값(TRUE)에 머문다. 해제할 때도 반드시 새로 써야 한다.

---

## D-15b — 전략선택 분기

`regime_tuning` 임계값으로 코스피 판정을 뒤집던 방법은 판정기와 함께 사라졌다(D-085). 지금은 `regime.json`을
손으로 써서 태운다 — D-15a와 같은 파일, `regime` 라벨만 다르다.

### 설정

config `regime_strategies`에 라벨별 집합을 다르게 두고(예: `"RISK_ON": ["DEVSCALE_*"]`, `"NEUTRAL": []`),
`regime.json`의 `regime`를 `RISK_ON` → `NEUTRAL` → `RISK_ON`으로 바꿔 쓴다(`valid: true`, 갱신 시각은 현재).

### 관측 항목

- 라벨을 바꿀 때마다 `[RegimeSelect] 국면=<라벨> → 활성=[…] 비활성=[…]` 줄이 한 번 남는가(같은 라벨 재기록에는 안 남아야 한다)
- 비활성이 된 전략이 신규 진입을 안 내는가, 켜진 전략은 다음 계획 회차에 내는가
- **청산이 일어나지 않는지**(진입만 막혀야 정상). 여기서 청산이 나오면 축 설계가 깨진 것이다
- 파일을 stale(`regime_stale_sec` 경과)로 두면 이전 선택이 유지되는가

### 끝나면

`regime.json`은 보조 프로세스가 다음 3분 갱신에 덮어쓴다. `regime_strategies`를 드릴 전 값으로 되돌린다.
