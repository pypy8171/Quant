"""
MeanReversionContraryStrategy — 횡단면 이격도 역추세 (BACKTEST_LOG 실행 #4 베이스라인).

유니버스 전 종목의 이격도(현재가 vs SMA(sma_period))를 계산해, SMA 아래로 가장 많이
벌어진(가장 음수) top_n 종목을 동일가중 매수. rebalance_every 거래일마다 리밸런싱.
CrossMomentum의 정확한 미러 — 신호 부호만 반대(최상위 → 최하위)이고 엔진·유니버스·
비용·look-ahead를 전부 공유한다. 그래서 실행 #1~#3(모멘텀)과 apples-to-apples 비교된다.

가설: "과매도(SMA 아래로 크게 벌어짐) → 평균회귀 반등"이 한국 주식 일봉에 엣지가 있는가.
개별 손절 없음(베이스라인, 모멘텀 #1과 동일 조건) — 손절/반등확인/리스크사이징은
후속 실행(#5~)에서 변수 하나씩 추가한다.

look-ahead: 결정은 date 종가(visible은 date 미만으로 잘려 옴), 체결은 엔진이 다음봉 시가.
사이징: 목표집합 set[str] 반환 → 엔진이 동일가중(TARGET_WEIGHT) 사이징·청산·비용 책임.
"""
from strategy.base import StrategyBase
from strategy.indicators import deviation_from_sma


class MeanReversionContraryStrategy(StrategyBase):
    def __init__(self, top_n: int = 10, rebalance_every: int = 5,
                 sma_period: int = 20, dev_max: float = 0.0,
                 rebound: bool = False, vol_mult: float = 0.0, vol_win: int = 20):
        super().__init__()
        self.top_n           = top_n
        self.rebalance_every = rebalance_every
        self.sma_period      = sma_period   # 이격 기준 SMA 창(거래일)
        self.dev_max         = dev_max      # 이 이격도(%) 미만만 후보. 0.0 = SMA 아래 전부(과매도)
        # ── C4 반등확인·거래량 필터(하위호환: 기본 off면 기존 베이스라인 동작 불변) ──
        # 둘 다 신호봉 t의 종가/OHLC/거래량만 사용 → look-ahead 없음(체결은 엔진이 t+1 시가).
        self.rebound         = rebound      # True면 신호봉이 양봉(close>open)일 때만 진입(반등확인)
        self.vol_mult        = vol_mult     # >0이면 신호봉 거래량 >= vol_mult×SMA(vol_win) 요구
        self.vol_win         = vol_win      # 거래량 평균 창(거래일)
        self._tick = 0

    def id(self) -> str:
        flt = (("+반등" if self.rebound else "")
               + (f"+vol{self.vol_mult}x" if self.vol_mult > 0 else ""))
        return (f"MEAN_REVERSION_CONTRARY (top{self.top_n}, rb{self.rebalance_every}d, "
                f"sma{self.sma_period}, dev<{self.dev_max}{flt})")

    def on_start(self, universe: list[str]) -> list[str]:
        return []   # 전 종목 감시 — 랭킹은 on_rebalance에서

    def on_data(self, ticker, bars):
        return None  # 횡단면 전략 — on_rebalance만 사용

    def on_rebalance(self, date, visible, flow=None):
        self._tick += 1
        if (self._tick - 1) % self.rebalance_every != 0:
            return None   # 리밸런싱 날 아님

        need = max(self.sma_period, self.vol_win if self.vol_mult > 0 else 0)
        scores: dict[str, float] = {}
        for t, bars in visible.items():
            if len(bars) < need:
                continue
            dev = deviation_from_sma(bars, self.sma_period)   # (종가-SMAn)/SMAn*100
            if dev is None:
                continue
            if dev >= self.dev_max:   # SMA 아래로 벌어진 종목만(과매도 후보)
                continue
            last = bars[-1]
            # 반등확인 양봉 필터 — 신호봉 t가 양봉(종가>시가)일 때만(t 데이터만, 누출 없음)
            if self.rebound and not (last.close > last.open > 0):
                continue
            # 거래량 필터 — 신호봉 거래량 >= vol_mult × 최근 vol_win 평균거래량
            if self.vol_mult > 0:
                vols = [b.volume for b in bars[-self.vol_win:]]
                avg_v = sum(vols) / len(vols) if vols else 0
                if avg_v <= 0 or last.volume < self.vol_mult * avg_v:
                    continue
            scores[t] = dev

        # 가장 음수(가장 과매도)부터 오름차순 → 하위 top_n 매수.
        # (모멘텀은 최상위 top_n; 역추세는 최하위 top_n — 부호만 반대)
        ranked = sorted(scores.items(), key=lambda kv: kv[1])
        return {t for t, _ in ranked[:self.top_n]}   # 목표 집합 — 엔진이 청산·매수·사이징 책임
