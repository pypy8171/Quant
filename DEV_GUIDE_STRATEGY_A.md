# DEV_GUIDE_STRATEGY_A — 강세 테마주 5일선 눌림목 일중 추종

> 작성: 2026-05-21  
> 기준 명세: STRATEGY_A.md  
> 원칙: 검증되지 않은 전략에 C++ 실행 배관을 먼저 깔지 않는다.

---

## 0. 선결 조건 — 수급 API 검증 (코드 작성 전 필수)

**이 단계를 건너뛰면 C3(수급 필터)가 백테스트에서 작동하지 않을 수 있다.**

### 0-1. 문제

STEP 0에서 참조한 `FHKST01010900`은 "주식현재가 투자자" TR일 가능성이 크며, 이 경우 **당일 누계만** 반환한다. 백테스트에는 과거 날짜별 외인/기관 순매수 시계열이 필요하다. 당일치만 주는 API라면 C3는 백테스트에 사용 불가.

### 0-2. 검증 절차 (testbed에서 직접 수행)

```python
# testbed 검증 스크립트 (PYQuant/tools/check_investor_api.py 로 저장)
from kis.client import KisClient, from_config

kis = from_config()
kis.authenticate()

# 검증 1: FHKST01010900 응답 구조 확인
resp = kis._get(
    "/uapi/domestic-stock/v1/quotations/inquire-investor",
    {
        "FID_COND_MRKT_DIV_CODE": "J",
        "FID_INPUT_ISCD": "005930",
        "FID_INPUT_DATE_1": "20250101",
        "FID_INPUT_DATE_2": "20250131",
    },
    "FHKST01010900",
)
print(resp)

# 확인할 것:
# 1. output2에 여러 날짜 행이 오는가? (일별 시계열이면 OK)
# 2. 각 행에 날짜 필드가 있는가?
# 3. frgn_ntby_qty(외인), orgn_ntby_qty(기관) 순매수량 필드가 있는가?
# 4. 당일 데이터만 오거나 output이 비어 있으면 → 다른 TR 필요
```

### 0-3. 대안 TR 후보 (검증 실패 시 탐색 순서)

| TR_ID | 명칭 | 비고 |
|-------|------|------|
| `FHKST01010900` | 주식현재가 투자자 | **먼저 검증** |
| `FHKST01010600` | 주식현재가 일자별(투자자) | 후보 |
| `/uapi/domestic-stock/v1/quotations/inquire-daily-trade-volume` | 투자자별 매매동향 | 후보 |

→ **C3(수급)는 API 검증 완료 전까지 보류.** 1차는 C1·C2·C3.5(정배열 + 5일선 위)만으로 시작하고, C3 통과 후 추가한다.

### 0-4. 백테스트 기간 데이터 가용성 확인 (STEP G의 start_date 결정)

수급 API 검증 스크립트(`check_investor_api.py`) 실행 시 다음도 같이 확인한다:

```python
# 검증 2: 일봉이 2022-01-01까지 조회되는지 확인
bars = kis.get_historical_ohlcv("005930", start_date="20220101", end_date="20220110")
print(f"일봉 조회 결과: {len(bars)}개 (기대: 5~7개)")

# 검증 3: 수급도 2022년치가 오는지 확인
resp_old = kis._get(
    "/uapi/domestic-stock/v1/quotations/inquire-investor",
    {"FID_COND_MRKT_DIV_CODE": "J", "FID_INPUT_ISCD": "005930",
     "FID_INPUT_DATE_1": "20220101", "FID_INPUT_DATE_2": "20220131"},
    "FHKST01010900",
)
print(f"2022년 수급 응답: {resp_old}")
```

| 결과 | 조치 |
|------|------|
| 일봉·수급 모두 2022년치 조회 가능 | STEP G start_date = "2022-01-01" 그대로 |
| 일봉만 가능, 수급 2022년치 없음 | C3 기간을 가용 데이터 시작일로 조정 |
| 일봉도 2022년치 없음 | STEP G start_date를 가용 최초 일봉 날짜로 조정. 30회 거래 충족 확인 필수 |

---

## 1. 1차 범위 (Out of scope 먼저 확정)

### 1차에서 하는 것
- C1(유니버스) + C2(정배열 5>10>20>60) + C3.5(5일선 위) 감시 필터 → 일봉 기반 구현
- C3(수급 7/10일) — **수급 API 검증 후 추가** (보류 상태 시작)
- 진입/청산: **일봉 종가 근사** (3분봉 정밀진입은 3차)
- CostModel(이미 반영) 적용 백테스트
- 검증 게이트 판정 로직
- 병행 트랙: `bars_3m` continuous aggregate 생성 (코드 변경 없음)

### 1차에서 하지 않는 것 (Out of scope)
- C++ 신호 연동 (전략 A 검증 전에 만들지 않음)
- 금리 레짐 비중조절 (3차)
- `set_kis`를 `StrategyBase`로 올리는 인터페이스 리팩토링 (기존 코드 건드림, 무관)
- `ValueContraryStrategy` 수정
- 3분봉 정밀 진입/청산 구현
- PBR 필터 (전략 A 명세에 없음. `get_fundamentals` 호출 금지)

---

## 2. 설계 제약 — look-ahead 방지 (반드시 지켜야 하는 불변 조건)

### 2-1. 일봉 look-ahead (이미 해결됨)
`BacktestEngine`은 `visible = [b for b in bars if b.date <= date]`로 미래 봉을 차단하고, 체결은 **다음 봉의 시가**로 가정한다. 변경하지 않는다.

### 2-2. 수급 look-ahead (추가 설계 필요)

**잘못된 설계 예 (look-ahead 발생):**
```python
def on_start(self, universe):
    # ❌ 전체 기간 수급을 한 번에 로드 → 미래 수급으로 과거를 판단하게 됨
    self._flow_cache[ticker] = kis.get_investor_flow(ticker, full_period)
```

**올바른 설계:**
```python
# on_data 호출 시점에 as_of_date를 알 수 있어야 함
# 방법 A: _flow_cache를 {ticker: [(date, ntby), ...]} 형태로 저장
#         on_data에서 "date 이전 슬라이스"만 참조
# 방법 B: BacktestEngine이 on_data에 날짜를 전달 (시그니처 변경 필요)
#         → 1차에서 인터페이스 변경 최소화 위해 방법 A 채택

# 방법 A 구현 개요:
# self._flow_cache[ticker] = sorted([(date, net_buy), ...]) # 날짜 오름차순
# 참조 시: [row for row in cache if row[0] <= sim_date][-N:]
```

**as_of_date 도출 방법 (시그니처 변경 없이 해결):**

`on_data(self, ticker, bars)`는 날짜 인자를 받지 않지만, `bars[-1].date`가 곧 현재 시뮬레이션 날짜다. 따라서 시그니처 변경 없이:

```python
# on_data 안에서 as_of_date 도출
as_of_date = bars[-1].date
net_buy_days = self._count_net_buy_days(ticker, as_of_date, window=10)
```

**슬라이스 기준 — `<` vs `<=`:**

당일 수급은 장 중엔 미확정이므로 **`r.date < as_of_date`** (당일 제외)를 사용한다. `<=`로 쓰면 "오늘" 수급이 당일 신호 판단에 반영되어 look-ahead가 된다.

→ 수급 데이터를 구현할 때 이 설계를 따르지 않으면 백테스트 결과가 무효다. 체크리스트 C3 항목에 "look-ahead 검증 테스트" 포함 필수.

### 2-3. 진입 근사의 한계 (기록용 — 3차 보정 대상)

1차 진입 정의: **"일봉 종가가 SMA5의 (100 - Z)% ~ (100 + Z)% 이내"**  
즉 `abs(close / sma5 - 1) <= Z/100` 조건으로 다음 봉 시가에 진입.

| 한계 | 설명 | 3차 보정 방향 |
|------|------|--------------|
| 장중 터치 못 잡음 | 일봉 종가 기준이라 장중 5일선 터치 후 튕긴 경우 진입 신호 없음 | 3분봉 E3 조건으로 대체 |
| 진입 시가가 5일선 위일 수 있음 | 다음 봉 시가가 gap-up이면 이격도 조건 이미 깨진 상태 | 3분봉 실시간 이격도 재확인 |
| 청산 타이밍 부정확 | X1/X2를 종가로만 판단, 장중 peak를 놓침 | 3분봉 실시간 X1/X2 체크 |

---

## 3. 체크리스트 — 구현 순서 (각 항목 독립 검증 후 다음으로)

> 규칙: 한 항목 완료 + 테스트 통과 → 다음 항목. 건너뛰기 금지.

---

### [PRE] 수급 API testbed 검증
- [ ] `tools/check_investor_api.py` 작성 + 실행
- [ ] FHKST01010900 응답에 과거 일별 시계열 포함 여부 확인
- [ ] **Pass**: 날짜별 외인/기관 순매수량 시계열 반환 → C3 구현 진행
- [ ] **Fail**: 당일치만 반환 → 대안 TR 탐색 후 재검증. C3는 이 단계 통과 전까지 스킵.

---

### [STEP B-0] pykrx 수급 데이터 검증 (C3 부활의 선결조건)

목적: pykrx로 외인/기관 일별 순매수 시계열을 과거 2022년치까지 종목별로 취득 가능한지 확인.

확인할 것:
- (a) 2022년치까지 과거 데이터가 오는가
- (b) 외국인/기관 순매수가 종목별·일별로 분리되어 오는가
- (c) 거래대금 기준인가 수량 기준인가 (C3 판정 기준 통일)

```bash
pip install pykrx
python -m tools.check_pykrx_flow
```

Pass 기준: (a)(b)(c) 모두 확인 → STEP F를 pykrx 기반으로 구현  
Fail 시 폴백: C3를 거래량 급증 대체 (20일 평균 거래량 × N배 초과 시 수급 대리지표)

---

### [STEP A] SMA 계산 헬퍼 (`strategy/indicators.py`)

목적: 정배열·이격도 계산을 전략마다 중복 구현하지 않기 위해 분리.

구현할 것:
```python
def sma(bars: list[Bar], period: int) -> float | None:
    """최근 period개 봉의 단순이동평균 종가. 봉 수 부족 시 None."""

def is_aligned(bars: list[Bar]) -> bool:
    """SMA5 > SMA10 > SMA20 > SMA60 정배열 여부. bars 60개 이상 필요."""

def deviation_from_sma(bars: list[Bar], period: int) -> float | None:
    """이격도(%) = (최근 종가 - SMAn) / SMAn * 100. 봉 수 부족 시 None."""
```

테스트 방법 (pytest):
```python
# tests/test_indicators.py
def test_sma_exact():
    bars = [Bar(date="", open=0, high=0, low=0, close=c, volume=1)
            for c in [1,2,3,4,5]]
    assert sma(bars, 3) == 4.0  # (3+4+5)/3

def test_sma_insufficient():
    bars = [Bar(..., close=c, ...) for c in [1,2]]
    assert sma(bars, 5) is None

def test_is_aligned_true():
    # SMA5 > SMA10 > SMA20 > SMA60 만족하는 bars 준비
    ...

def test_is_aligned_false():
    # 정배열 아닌 cases
    ...

def test_deviation():
    # (close - sma5) / sma5 * 100 수식 검증
    ...
```

완료 기준: 위 테스트 모두 PASS.

---

### [STEP B] 유니버스 + C1 필터 (유니버스 수집)

목적: 백테스트 대상 종목 풀 확보.

구현할 것 (BacktestEngine이 아니라 전략 외부에서):
```python
# tools/build_universe.py 또는 StrategyA.on_start() 내부
# fetch_universe()로 시총 상위 N종목 수집 (이미 있음)
# C1 적용: STRATEGY_A.md의 테마 4그룹 종목 config로 주입
# → config/strategy_a.json에 테마별 종목코드 목록
```

설계 결정:
- C1(유니버스 정의)은 **시총 기반이 아닌 테마 그룹 수동 정의** (STRATEGY_A.md 1-4절)
- `config/strategy_a.json`에 4그룹 × N종목 하드코딩 → 코드에 박지 않음
- 유니버스 변경 시 json만 수정

테스트 방법:
```python
# config/strategy_a.json 로드 → 종목 코드 목록 출력
# KIS testbed로 각 종목 일봉 1개 조회 → 응답 확인 (API 접속 검증)
```

---

### [STEP C] C2 필터 — 정배열 스크리닝

목적: `on_start`에서 C2(SMA5>10>20>60) 통과 종목만 감시 리스트에 올리기.

구현할 것 (`StrategyA.on_start`):
```python
def on_start(self, universe: list[str]) -> list[str]:
    watch = []
    for ticker in universe:
        bars = self._kis.get_historical_ohlcv(ticker, pre_start, today)
        time.sleep(0.3)  # rate limit
        if len(bars) < 60:
            continue
        if not is_aligned(bars):  # STEP A 헬퍼
            continue
        # C3.5: 전일 종가 >= SMA5
        sma5 = sma(bars, 5)
        if sma5 and bars[-1].close >= sma5:
            watch.append(ticker)
    return watch
```

테스트 방법 (수동):
```python
# tools/run_screening.py
# 실제 KIS API로 유니버스 조회 → C2 통과 종목 출력
# 기대: 상승 추세 종목이 통과, 하락/횡보 종목은 탈락
```

완료 기준: 알려진 상승/하락 종목 각 2개로 수동 검증.

---

### [STEP D] C3.5 + 진입 조건 (E1~E3) — 일봉 근사

목적: 감시 리스트 종목 중 당일 종가가 5일선 ±Z% 이내인 경우 다음 봉 시가에 진입 신호.

**⚠ 구조 원칙 — 진입/청산은 상호배타다:**

`on_data` 안에서 보유 여부로 먼저 분기한다. 한 봉에서 진입과 청산을 동시에 평가하면 안 된다.

```python
def on_data(self, ticker: str, bars: list[Bar]) -> Optional[OrderSignal]:
    # 보유 여부로 먼저 분기 — 동일 봉에서 진입/청산 동시 평가 금지
    if self.has_position(ticker):
        return self._check_exit(ticker, bars)   # X1/X2만
    return self._check_entry(ticker, bars)      # E1~E3만
```

구현할 것 (`_check_entry`):
```python
def _check_entry(self, ticker: str, bars: list[Bar]) -> Optional[OrderSignal]:
    if len(bars) < 60:
        return None

    sma5 = sma(bars, 5)
    if sma5 is None:
        return None

    close = bars[-1].close
    dev = (close - sma5) / sma5 * 100  # 이격도

    # E1: 감시 리스트 종목
    if ticker not in self._watch:
        return None
    # E2: 현재가 >= SMA5 (C3.5)
    if close < sma5:
        return None
    # E3: 이격도 <= Z (5일선 눌림목)
    if dev > self.z:
        return None

    self.open_position(ticker, self.quantity, close, bars[-1].date)
    return OrderSignal(ticker=ticker, side="BUY", quantity=self.quantity)
```

파라미터 (클래스 `__init__` 인자, 기본값):
```
z: float = 1.0    # 이격도 진입 허용 상한 (%)
a: float = 7.0    # 이격도 청산 기준 (%)
b: float = 2.0    # 손절 기준 (SMA5 이탈 %)
quantity: int = 1  # 주문 수량
```

한계 명시 (가이드 2-3절 참고): 일봉 종가 기준이므로 장중 터치 미반영. 3차에서 보정.

테스트 방법 (pytest):
```python
# tests/test_strategy_a.py
def test_entry_within_z():
    # bars[-1].close = sma5 * 1.005 (이격도 +0.5%, Z=1.0이면 진입)
    # → BUY 신호 반환 확인

def test_no_entry_above_z():
    # bars[-1].close = sma5 * 1.015 (이격도 +1.5%, Z=1.0이면 미진입)
    # → None 반환 확인

def test_no_entry_below_sma5():
    # close < sma5 → None

def test_no_double_entry():
    # 포지션 보유 중이면 _check_exit으로 분기 → _check_entry 미호출
```

---

### [STEP E] 청산 조건 (X1, X2) — 일봉 근사

목적: 보유 중인 종목에 X1(이격도 초과) 또는 X2(5일선 이탈) 발생 시 매도 신호.

`on_data`의 분기에 의해 `has_position(ticker) == True` 일 때만 호출된다.

구현할 것 (`_check_exit`):
```python
def _check_exit(self, ticker: str, bars: list[Bar]) -> Optional[OrderSignal]:
    if len(bars) < 5:
        return None

    sma5 = sma(bars, 5)
    if sma5 is None:
        return None

    close = bars[-1].close
    dev = (close / sma5 - 1) * 100

    # X1: 이격도 >= A (과익 청산)
    if dev >= self.a:
        self.close_position(ticker)
        return OrderSignal(ticker=ticker, side="SELL", quantity=self.quantity)

    # X2: 현재가 < SMA5 * (1 - B/100) (추세 이탈 손절)
    if close < sma5 * (1 - self.b / 100):
        self.close_position(ticker)
        return OrderSignal(ticker=ticker, side="SELL", quantity=self.quantity)

    # X3(장 마감 청산): 일봉 근사에서는 생략. 3차(3분봉)에서 15:15 청산 추가 예정.
    return None
```

테스트 방법 (pytest):
```python
def test_exit_overextended():
    # close = sma5 * 1.08 (이격도 +8%, A=7%) → SELL

def test_exit_stop_loss():
    # close = sma5 * 0.97 (이탈 3%, B=2%) → SELL

def test_hold_within_range():
    # 이격도 +3%, 이탈 없음 → None

def test_entry_exit_mutual_exclusive():
    # 포지션 보유 중: on_data가 _check_exit만 호출하고 _check_entry 미호출 확인
    # 포지션 없음: on_data가 _check_entry만 호출하고 _check_exit 미호출 확인
```

---

### [STEP F] 수급 필터 C3 — pykrx로 부활 (2차 A/B 측정)

**KIS REST API는 과거 일별 수급 미제공 확정 (2026-05-21 PRE 검증).  
대신 pykrx(KRX 공식 데이터)로 외인/기관 일별 순매수 시계열 취득 가능.**

**진행 방침:**
1. **1차 백테스트**: C3 없이 C1+C2+C3.5만으로 베이스라인 측정
2. **2차 백테스트**: C3(pykrx 수급) 추가 후 A/B 비교 → 수급 필터의 기여도 측정

**선결조건 — STEP B-0: pykrx 검증 통과 후에만 STEP F 진행.**  
(STEP B-0 결과에 따라 폴백 → C3를 거래량 급증 대체로 변경 가능)

```python
# look-ahead 방지 설계 (방법 A — on_data 시그니처 변경 없음)
# self._flow_cache: dict[str, list[FlowData]]
# FlowData는 날짜 오름차순 정렬

def _count_net_buy_days(self, ticker: str, as_of_date: str, window: int = 10) -> int:
    rows = self._flow_cache.get(ticker, [])
    # r.date < as_of_date: 당일 수급은 장중 미확정이므로 당일 제외
    # (<= 를 쓰면 당일 수급이 당일 신호 판단에 반영 → look-ahead)
    past = [r for r in rows if r.date < as_of_date][-window:]
    return sum(1 for r in past if r.frgn_ntby + r.orgn_ntby > 0)

# C3 판정: count >= 7이면 통과
```

`_check_entry`에서 as_of_date 도출 방법 (시그니처 변경 없이):

```python
def _check_entry(self, ticker: str, bars: list[Bar]) -> Optional[OrderSignal]:
    ...
    # as_of_date: on_data 시그니처는 (ticker, bars)로 유지하되,
    # bars[-1].date가 현재 시뮬레이션 날짜 — 추가 인자 없이 도출 가능
    as_of_date = bars[-1].date
    if self._count_net_buy_days(ticker, as_of_date, window=10) < 7:
        return None  # C3 미통과
    ...
```

구현할 것:
1. `KisClient.get_investor_flow(ticker: str, start_date: str, end_date: str) -> list[FlowData]`
   - `FlowData`: `date: str, frgn_ntby: int, orgn_ntby: int` (외인·기관 순매수량)
2. `StrategyA.on_start`에서 종목별 수급 시계열 로드 → `self._flow_cache` 저장
3. C3 조건 평가 시 **look-ahead 방지**: 현재 시뮬레이션 날짜 이전 수급만 참조

```python
# look-ahead 방지 설계 (방법 A — on_data 시그니처 변경 없음)
# self._flow_cache: dict[str, list[FlowData]]
# FlowData는 날짜 오름차순 정렬

def _count_net_buy_days(self, ticker: str, as_of_date: str, window: int = 10) -> int:
    rows = self._flow_cache.get(ticker, [])
    # r.date < as_of_date: 당일 수급은 장중 미확정이므로 당일 제외
    # (<= 를 쓰면 당일 수급이 당일 신호 판단에 반영 → look-ahead)
    past = [r for r in rows if r.date < as_of_date][-window:]
    return sum(1 for r in past if r.frgn_ntby + r.orgn_ntby > 0)

# C3 판정: count >= 7이면 통과
```

`_check_entry`에서 as_of_date 도출 방법 (시그니처 변경 없이):

```python
def _check_entry(self, ticker: str, bars: list[Bar]) -> Optional[OrderSignal]:
    ...
    # as_of_date: on_data 시그니처는 (ticker, bars)로 유지하되,
    # bars[-1].date가 현재 시뮬레이션 날짜 — 추가 인자 없이 도출 가능
    as_of_date = bars[-1].date
    if self._count_net_buy_days(ticker, as_of_date, window=10) < 7:
        return None  # C3 미통과
    ...
```

테스트 방법 (pytest):
```python
def test_c3_look_ahead_blocked():
    # cache에 미래 날짜 수급 포함 → as_of_date 이전만 반환하는지 확인

def test_c3_net_buy_count():
    # 10일 중 7일 외인/기관 순매수 → count_net_buy_days == 7
```

---

### [STEP G] 백테스트 실행 + 검증 게이트 판정

목적: 파라미터 기본값 (Z=1.0, A=7.0, B=2.0)으로 백테스트 실행 후 게이트 통과 확인.

```python
# tools/run_backtest_a.py
from backtest.engine import BacktestEngine, CostModel
from strategy.strategy_a import StrategyA
from kis.client import from_config

kis = from_config()
kis.authenticate()

strategy = StrategyA(z=1.0, a=7.0, b=2.0, quantity=3)
engine = BacktestEngine(kis, strategy,
                        initial_cash=10_000_000,
                        cost_model=CostModel())  # 기본값: 0.015%+0.18%+5bp

result = engine.run(
    universe=load_universe("config/strategy_a.json"),
    start_date="2022-01-01",
    end_date="2023-12-31",   # 학습 기간
)
print_gate(result)
```

검증 게이트 판정 함수 (STRATEGY_A.md 2절 기준):

| 게이트 | 기준 | 판정 |
|--------|------|------|
| 거래 횟수 | ≥ 30회 | 미달 시 → 파라미터/유니버스 재검토 |
| 비용 반영 후 순손익 | > 0 | 최우선. 미달 시 전략 재설계 |
| 손익비 (Profit Factor) | ≥ 1.3 | 총이익 / 총손실 |
| MDD | < 20% (본인 기준) | |
| 파라미터 강건성 | Z ±0.5%, A ±2%, B ±0.5% 인접값 모두 +수익 | 단일 최적값이면 과적합 |
| 기간 분할 | 학습(2022~23) + 검증(2024~25) 각각 +수익 | 검증기간 미통과 시 무효 |

```python
def print_gate(result: BacktestResult) -> None:
    ...  # 위 6개 기준 자동 판정 + PASS/FAIL 출력
```

---

### [STEP H] 파라미터 강건성 검증 (STEP G PASS 후)

목적: 단일 최적값이 아닌 "범위에서 작동"하는지 확인.

```python
# tools/param_sweep_a.py
for z in [0.5, 1.0, 1.5]:
    for a in [5.0, 7.0, 9.0]:
        for b in [1.5, 2.0, 2.5]:
            result = engine.run(...)
            record(z, a, b, result.total_pnl, result.trade_count, result.sharpe)
# 전체 출력 후 heat-map 또는 표로 확인
```

완료 기준: 기본값(Z=1.0, A=7.0, B=2.0) 인접 조합 ≥ 70%에서 +수익.

---

## 4. 병행 트랙 — 3분봉 DB 적재 (코드 변경 없음)

### 현황
- `ticks` 테이블: ZMQ TRADE 이벤트로 이미 체결 데이터 적재 중
- C++ `KR_TEST` 모드 실행 중이면 3분봉 원시 데이터가 계속 쌓임

### 추가할 것 (SQL만, 코드 없음)

```sql
-- schema_3m.sql (schema.sql에 추가하거나 별도 파일)

-- TimescaleDB continuous aggregate: ticks → bars_3m
-- 3분 경계는 KST 09:00 정렬 기준 (ts는 UTC 저장이므로 00:00 UTC = 09:00 KST).
-- 3차에서 C++ BarBuilder를 만들 때 반드시 동일 경계(KST 09:00 정렬, UTC 기준 절삭)로
-- 집계해야 백테스트-라이브 동치가 성립한다. 경계가 어긋나면 봉 구성이 달라져
-- 백테스트 재현이 불가능해진다.
CREATE MATERIALIZED VIEW bars_3m
WITH (timescaledb.continuous) AS
SELECT
    time_bucket('3 minutes', ts) AS bucket,
    ticker,
    FIRST(price, ts)             AS open,
    MAX(price)                   AS high,
    MIN(price)                   AS low,
    LAST(price, ts)              AS close,
    SUM(volume)                  AS volume,
    market
FROM ticks
GROUP BY bucket, ticker, market;

-- 자동 갱신 정책 (1분 lag: 지연 1분 이내 데이터 반영)
SELECT add_continuous_aggregate_policy('bars_3m',
    start_offset => INTERVAL '1 day',
    end_offset   => INTERVAL '1 minute',
    schedule_interval => INTERVAL '3 minutes');

-- 인덱스
CREATE INDEX IF NOT EXISTS bars_3m_ticker_bucket ON bars_3m (ticker, bucket DESC);
```

검증 방법:
```sql
-- KR_TEST 모드로 30분 이상 실행 후
SELECT bucket, ticker, open, close, volume
FROM bars_3m
WHERE ticker = '005930'
ORDER BY bucket DESC
LIMIT 10;
```

### 수급 DB 테이블 (C3 구현 시 추가)

```sql
-- schema_investor.sql (수급 API 검증 통과 후 추가)
CREATE TABLE IF NOT EXISTS investor_flow (
    date       DATE         NOT NULL,
    ticker     TEXT         NOT NULL,
    frgn_ntby  BIGINT,   -- 외인 순매수량
    orgn_ntby  BIGINT,   -- 기관 순매수량
    PRIMARY KEY (date, ticker)
);
CREATE INDEX IF NOT EXISTS investor_flow_ticker_date
    ON investor_flow (ticker, date DESC);
```

---

## 5. 파일 구조 (1차 생성 대상)

```
PYQuant/
├── config/
│   └── strategy_a.json          # 테마 그룹 + 종목코드 (STEP B)
├── strategy/
│   ├── base.py                  # 기존, 수정 없음
│   ├── indicators.py            # 신규 — SMA 헬퍼 (STEP A)
│   └── strategy_a.py            # 신규 — StrategyA 클래스 (STEP D~F)
├── tests/
│   ├── test_indicators.py       # 신규 (STEP A)
│   └── test_strategy_a.py       # 신규 (STEP D~F)
├── tools/
│   ├── check_investor_api.py    # 신규 — 수급 API testbed 검증 (PRE)
│   ├── build_universe.py        # 신규 — 유니버스 수집 (STEP B, 선택)
│   ├── run_screening.py         # 신규 — C2 수동 검증 (STEP C)
│   ├── run_backtest_a.py        # 신규 — 백테스트 실행 (STEP G)
│   └── param_sweep_a.py         # 신규 — 파라미터 강건성 (STEP H)
└── db/
    ├── schema.sql               # 기존, 수정 없음
    ├── schema_3m.sql            # 신규 — bars_3m aggregate (병행 트랙)
    └── schema_investor.sql      # 신규 — investor_flow (C3 검증 후)
```

---

## 6. 의존 순서 (DAG)

```
PRE(수급API검증)
    └─ STEP F(C3 구현) ← 검증 통과 시만

STEP A(indicators)
    └─ STEP C(C2 스크리닝)
    └─ STEP D(진입 조건)
    └─ STEP E(청산 조건)

STEP B(유니버스 config)
    └─ STEP C
    └─ STEP G(백테스트)

STEP C + D + E
    └─ STEP G(백테스트)
        └─ STEP H(파라미터 스윕)

병행 트랙 (독립):
SQL만 추가, 코드 무관
```

---

## 7. 이번 가이드의 전제 (다음 구현 세션에 반드시 전달)

1. **CostModel은 이미 BacktestEngine에 반영됨** — 별도 추가 불필요
2. **PRE 단계 없이 C3 구현하지 말 것** — 수급 API가 과거 시계열을 주는지 먼저 확인
3. **수급 데이터는 날짜 컷 적용 필수** — `as_of_date` 이전 데이터만 참조
4. **PBR 필터 없음** — 전략 A 명세에 없음
5. **set_kis StrategyBase 리팩토링 없음** — 기존 ValueContrary 패턴 그대로 따름
6. **진입 타이밍 근사의 한계 인지** — 일봉 종가 기반, 3차에서 3분봉으로 보정
7. **한 STEP씩** — 테스트 통과 후 다음 항목
