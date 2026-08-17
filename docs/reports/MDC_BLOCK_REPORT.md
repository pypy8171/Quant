# KRX pykrx 데이터 차단 진단 리포트 (웹 Claude 전달용)

## 요약 (TL;DR)
- **per-ticker 시계열 엔드포인트는 정상**, **"특정일 전체종목 스냅샷 / 리스트 / 투자자 수급" 엔드포인트는 전부 차단**.
- 차단 메커니즘: KRX 데이터포털(`data.krx.co.kr`)의 **OTP 생성(GenerateOTP) 응답이 `"LOGOUT"`** 6바이트 문자열을 반환 → 이후 CSV/JSON 다운로드가 **빈 응답(0바이트)**. pykrx는 이 빈 응답을 JSON 파싱하다 `Expecting value: line 1 column 1 (char 0)` 후 빈 DataFrame을 만들고, 컬럼 접근에서 `KeyError`로 터짐.
- **로그인(KRX_ID/PW) 문제가 아님.** 시가총액·종목리스트는 공개 데이터이며 로그인 불필요. `"KRX 로그인 실패: KRX_ID 또는 KRX_PW 환경 변수가 설정되지 않았습니다."` 메시지는 pykrx가 import/호출 시 무조건 찍는 무해한 경고일 뿐, 차단의 원인이 아니다.

## 환경
- OS: Windows 11 Pro (26200), Python venv `.venv-win`
- pykrx **1.2.8** (pip 최신 — `pip index versions pykrx` 결과 1.2.8이 LATEST, 업그레이드 불가)
- requests 2.34.2, pandas 2.3.3, numpy 2.4.6
- KRX_ID / KRX_PW 환경변수 **미설정**

## 테스트 케이스 & 결과

### ✅ 정상 동작 (per-ticker 시계열 — Naver/legacy chart 백엔드)
| 호출 | 결과 |
|---|---|
| `stock.get_market_ohlcv('20240105','20240105','005930')` | OK, 1행 (시가/고가/저가/종가/거래량/등락률) |
| `stock.get_market_ohlcv(start, end, ticker)` (날짜범위) | OK — 백테스트 OHLCV는 이걸로 정상 수집 |
| `stock.get_market_ticker_name('005930')` | OK → '삼성전자' (무효코드는 빈 DataFrame) |

### ❌ 차단 (MDC OTP→download 백엔드 — 스냅샷/리스트/수급)
모두 동일 증상: `Error occurred in <fn>: Expecting value: line 1 column 1 (char 0)` → 빈 DataFrame → `KeyError`/`IndexError`.

| 호출 | 결과 |
|---|---|
| `get_market_cap_by_ticker('20240105', market='KOSPI')` | 빈 응답 → KeyError("None of ['종목','시가총액','거래량','거래대금']") |
| `get_market_ohlcv('20240105', market='KOSPI')` (특정일 전체종목) | 빈 응답 → KeyError |
| `get_market_ticker_list('20240105', market='KOSPI')` | len 0 |
| `get_index_ticker_list()` | KeyError |
| `get_index_portfolio_deposit_file('1028')` (KOSPI200 구성종목) | IndexError (size 0) |
| `get_index_portfolio_deposit_file('1028','20240105')` | len 0 |
| `get_market_trading_value_by_date('20240102','20240110','005930')` (투자자별 순매수금액) | 0행 |
| `get_market_trading_value_by_date(..., detail=True)` | 0행 |
| `get_market_trading_volume_by_date(...)` | 0행 |
| `get_market_net_purchases_of_equities('20240102','20240131','KOSPI','외국인')` | 0행 |

→ **수급(투자자별 매매동향) 데이터 전부 차단.** 수급 기반 전략은 pykrx로는 데이터 확보 불가.

### 🔬 근본 원인 — MDC OTP 엔드포인트 직접 프로빙 (requests)
pykrx 내부와 동일한 2단계(OTP 생성 → 다운로드)를 직접 재현:

1. **헤더만 (UA + Referer):**
   ```python
   GET http://data.krx.co.kr/comm/fileDn/GenerateOTP/generate.cmd
       ?mktId=STK&trdDd=20240105&...&url=dbms/MDC/STAT/standard/MDCSTAT01501
   → status 200, body = "LOGOUT" (len 6)
   POST .../download_csv/download.cmd  data={code:"LOGOUT"}
   → status 200, body = b'' (0 bytes)
   ```

2. **세션 워밍업 (포털 페이지 먼저 GET → JSESSIONID 쿠키 확보) 후 재시도:**
   ```python
   s.get('.../mdiLoader/index.cmd?menuId=MDC0201020201')  # JSESSIONID 발급됨 (확인)
   s.get(GenerateOTP, ...)  → body = "LOGOUT" (여전)
   ```

3. **HTTPS + 풀 브라우저 헤더 (User-Agent, Accept, X-Requested-With:XMLHttpRequest, Referer):**
   ```python
   GET https://data.krx.co.kr/comm/fileDn/GenerateOTP/generate.cmd?...
   → body = "LOGOUT" (여전)
   ```

**결론:** 유효한 JSESSIONID 쿠키가 있어도, HTTPS·완전한 브라우저 헤더를 넣어도 GenerateOTP가 `"LOGOUT"` 센티넬을 반환. KRX가 자동화 클라이언트를 능동 차단하는 것으로 보이며, pykrx 1.2.8도 동일하게 막힌다(pykrx 자체 버그가 아니라 서버 측 차단).

## 웹 Claude에게 묻고 싶은 것
1. KRX MDC `GenerateOTP`가 `"LOGOUT"`을 반환하는 알려진 원인/우회법이 있는가? (특정 쿠키 시퀀스, `__smVisitorID`, 추가 헤더, IP/지역 이슈, 또는 KRX의 2024~2025 정책 변경 등)
2. pykrx 1.2.8에서 이 백엔드를 살리는 알려진 패치/이슈가 있는가?
3. 우회 불가하다면, **투자자별 수급(외인/기관 순매수) 일별 데이터**를 한국 시장에서 얻는 현실적 대안은? (KIS Open API 투자자별 매매동향 TR의 과거 데이터 깊이, 네이버 금융 파싱, 유료 데이터 등)
4. **point-in-time 유니버스(과거 시점 KOSPI200 구성종목)** 를 survivorship bias 없이 얻는 방법은?

## 현재 우회 (이미 적용)
- 유니버스: 정적 KOSPI 대형·중형주 **131종목** 하드코딩 (`PYQuant/data/universe_kospi.py`, 코드+종목명 131/131 검증 통과). ⚠ survivorship bias 존재.
- 수급 데이터: 미해결 (KIS REST 투자자매매동향 구현 검토 중).
