# 27 NXT 프리마켓 주도주 포착기

질문: 프리마켓(08:00~08:50, NXT)에 함께 강한 업종·테마가 그날 정규장 주도주가 되는가.
계기: 2026-10-01 철강·제강 종목이 프리마켓부터 강했다. 사지 않고 장 전에 포착만 한다.

## 데이터

- 네이버 시세판 polling 응답의 `overMarketPriceInfo` 칸(`tradingSessionType: "PRE_MARKET"`) — 가격·등락률·거래대금. 앱키를 쓰지 않는다.
- 업종: 네이버 업종(한 종목 한 업종), `industry_map.json`에 7일 캐시.
- 테마: `PYQuant/data/themes/latest.json`(인포스탁, 한 종목 여러 테마).
- 확인용: KIS 거래량 순위 `FHPST01710000`에 시장 코드 `NX`를 주면 프리마켓 값이 온다(2026-10-01 MCP 공식 샘플·모의 앱키로 확인).

## 판정

- 묶음 안 프리마켓 거래 종목이 3개 이상일 때만 본다.
- 동반 강세: 강세 종목(+2% 이상·거래대금 3억 이상) 2개 이상, 등락률 중앙값 +1% 이상, 상승 비율 60% 이상.
- 순위 점수 = 중앙값 × √(강세 종목 수) × 상승 비율. 한 종목만 급등한 묶음이 위로 오지 않게 한다.
- 후보: 동반 강세 묶음의 강세 종목, 최대 20개.

## 실행

```
py -X utf8 research/studies/27_premarket_leaders/premarket_leaders.py snapshot    # 08:00~08:50, daily/YYYY-MM-DD.jsonl 에 한 줄
py -X utf8 research/studies/27_premarket_leaders/premarket_leaders.py evaluate    # 15:30 뒤, 포착 종목이 주도주가 됐는지
py -X utf8 research/studies/27_premarket_leaders/premarket_leaders.py kis-check   # 모의 앱키로 KIS NX 순위 대조(실계좌 앱키면 멈춘다)
```

장 마감 뒤 판정: 포착 종목이 KRX 정규장 거래대금 상위 100 안이고 등락률이 시장 중앙값보다 3%p 이상 높으면 주도주로 친다.
동반 강세 업종이 당일 업종 상위 10(중앙값 기준)에 들었는지도 남긴다.

## 결과

- 2026-10-01 08:50 값: 상장 2,767종목 중 프리마켓 거래 564종목. 업종 1위 철강(중앙값 +6.71%, 12종목 전부 상승, 강세 9/12),
  테마 1위 철강 주요종목(중앙값 +10.49%). 후보는 동국제강·대한제강·고려제강·한국철강·현대제철 등.
- 적중률은 기록이 쌓인 뒤 낸다.

## 엔진 연결

엔진 시세판은 09:00 전 KRX 칸만 읽어 순위 축이 0종목이었다. 같은 응답의 NXT 칸을 읽게 고치는 작업은 `wt/premarket-board`
(`Quant/src/universe/MarketBoard.cpp`).
