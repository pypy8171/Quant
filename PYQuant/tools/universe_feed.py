"""data.go.kr 거래대금 top-N 유니버스 피드 → Quant/config/universe_scan.json.
시총 top-N 축은 기본 0이다(D-146) — --n-mktcap으로 옛 시총∪거래대금 선정을 다시 켤 수 있다.

C++ DeviationScale 스캐너의 4번째 후보 축(ETF-free·KIS 30행캡 우회)을 채운다.
data.go.kr 스냅샷은 종목 목록(코드·이름·시장, ETF 없음)만 쓰고, 시총·거래대금·종가는 네이버 벌크
시세(polling API, marketValueFullRaw·accumulatedTradingValueRaw)에서 실행 시점 값으로 받는다.
data.go.kr 는 전영업일 시세를 당일 오전 늦게 올려 08시 스캔이 이틀 전 기준을 받았고(09-14 실측), 그
기준일로 하루를 보내면 전날·오늘 급등한 종목이 후보 풀에서 빠진다. 라이브 재랭킹은 09-26부터 엔진 안
시세판(Quant/src/universe/MarketBoard.cpp)이 하고, 이 파일은 손으로 돌리거나 pit_universe_backfill.py가
build()를 가져다 쓴다(D-147).

핵심 이점(data-sourcer 실측 확인):
  • getStockPriceInfo(금융위 주식시세정보)는 ETF/ETN을 구조적으로 서빙 안 함
    → KIS 랭킹의 장중 ETF 잠식(드롭 9→17)을 원천 차단.
  • 스냅샷 1콜에 시총(mrktTotAmt)·거래대금(trPrc)·종가(clpr)가 다 들어있어
    시총 top-N ∪ 거래대금 top-N union이 추가 API 비용 0.
    (시총 단독은 현대해상·GS건설·한국콜마 같은 중형 트렌딩주를 놓쳐서 union 필수.)

실행:
  set PYTHONIOENCODING=utf-8
  python PYQuant/tools/universe_feed.py [--n-mktcap 100] [--n-turnover 100] [--date YYYY-MM-DD] [--market ALL|KOSPI|KOSDAQ]
  --market ALL: 코스피·코스닥 각각 시총∪거래대금 top-N을 union하고 종목별 "market" 태그를 부여
    → C++ UniverseScanner가 종목 시장별로 코스피(0001)/코스닥(1001) risk_off 게이트를 분기.
키: 환경변수 DATA_GO_KR_KEY (없으면 즉시 에러). [[data-source-constraints]] 참조.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time
import urllib.request
from datetime import date as _date, timedelta
from pathlib import Path

# PYQuant 루트를 path에 올려 data.datagokr_source / kis.client 임포트 가능하게.
_PYQUANT_ROOT = Path(__file__).resolve().parents[1]
if str(_PYQUANT_ROOT) not in sys.path:
    sys.path.insert(0, str(_PYQUANT_ROOT))

from data.datagokr_source import DataGoKrSource  # noqa: E402

# 출력 경로: 레포 루트/Quant/config/universe_scan.json (C++ config의 universe_file과 일치).
_REPO_ROOT = _PYQUANT_ROOT.parent
_OUT_PATH = _REPO_ROOT / "Quant" / "config" / "universe_scan.json"


def _yesterday_iso() -> str:
    """T-1(전영업일 근사). 실제 서빙일은 _snapshot의 7일 백오프가 결정하므로 근사면 충분."""
    d = _date.today() - timedelta(days=1)
    return d.isoformat()


_NAVER_URL = "https://polling.finance.naver.com/api/realtime/domestic/stock/"
_NAVER_UA  = {"User-Agent": "Mozilla/5.0"}
_NAVER_CHUNK = 900   # 한 요청 1,000종목까지 받아주고 1,500부터 HTTP 400 (2026-09-26 실측)

def _prev_weekday_ymd() -> str:
    d = _date.today() - timedelta(days=1)
    while d.weekday() >= 5:
        d -= timedelta(days=1)
    return d.strftime("%Y%m%d")


def _num(v) -> float:
    try:
        return float(str(v).replace(",", ""))
    except (TypeError, ValueError):
        return 0.0


def fetch_naver_live(codes: list[str]) -> dict[str, dict]:
    """네이버 벌크 시세 — 코드별 {px, mcap, val}. 900종목씩 한 요청, 전 시장 2,760종목이면 요청 3개.
    장중에는 현재가·당일 누적 거래대금·현재 시총이고, 장 전에는 직전 종가 기준 값이다.
    실패한 청크는 건너뛴다(그 종목은 data.go.kr 값 유지)."""
    out: dict[str, dict] = {}
    miss = 0
    for i in range(0, len(codes), _NAVER_CHUNK):
        part = codes[i:i + _NAVER_CHUNK]
        try:
            req = urllib.request.Request(_NAVER_URL + ",".join(part), headers=_NAVER_UA)
            with urllib.request.urlopen(req, timeout=10) as r:
                rows = json.loads(r.read().decode("utf-8")).get("datas", [])
        except Exception as e:  # noqa: BLE001 — 청크 하나 실패가 스캔을 멈추면 안 된다
            miss += len(part)
            print(f"[universe_feed] 네이버 청크 {i // _NAVER_CHUNK} 실패: {e}", file=sys.stderr)
            time.sleep(0.5)
            continue
        for r in rows:
            code = r.get("itemCode")
            px = _num(r.get("closePriceRaw"))
            if not code or px <= 0:
                continue
            out[code] = {"px": px,
                         "mcap": _num(r.get("marketValueFullRaw")),
                         "val": _num(r.get("accumulatedTradingValueRaw"))}
    if miss:
        print(f"[universe_feed] 네이버 시세 누락 {miss}종목(청크 실패).", file=sys.stderr)
    return out


def build(on_date: str, n_mktcap: int, n_turnover: int,
          min_turnover: float = 1e9, market: str = "KOSPI",
          with_market_map: bool = False, use_live: bool = True) -> dict | None:
    # market="ALL"이면 코스피·코스닥 각각 시총∪거래대금 top-N을 뽑아 union한다(시장별 균형 —
    # 코스피 대형주가 코스닥 슬롯을 잠식하지 않도록 시장을 나눠 각자 상위 N을 확보). datagokr
    # _snapshot은 시장 무관 전종목을 한 번에 서빙하므로 시장 수와 무관하게 API 비용 동일.
    src = DataGoKrSource(market=market)
    if not src.authenticate():
        print("[universe_feed] DATA_GO_KR_KEY 미설정 — 유니버스 생성 불가.")
        return None

    rows = src._snapshot(on_date)   # 전 시장 스냅샷(휴장일 7일 백오프 내장)
    if not rows:
        print(f"[universe_feed] {on_date} 스냅샷 비어있음(7일 백오프 소진).")
        return None

    # 실제 서빙된 기준일 — _snapshot이 캐시로 쓴 univ_YYYYMMDD.parquet 중 요청일 이하 최신.
    req_ymd = on_date.replace("-", "")
    cached = sorted(p.stem.split("_", 1)[1] for p in src._cache.glob("univ_*.parquet"))
    served = max((d for d in cached if len(d) == 8 and d <= req_ymd), default=req_ymd)

    # 시총·거래대금·종가를 네이버 실행 시점 값으로 바꾼다. 거래대금이 절반 넘게 0이면(장 전에 누적치가
    #  아직 없는 경우) 거래대금 축만 data.go.kr 로 두는데, 그 기준일이 직전 평일보다 오래됐으면 이틀 전
    #  랭킹으로 유니버스를 만드는 것이라 실패로 친다.
    live_hhmm = time.strftime("%H%M")
    mcap_src = val_src = f"data.go.kr {served}"
    if use_live:
        live = fetch_naver_live([row["code"] for row in rows if row.get("code")])
        n_cap = n_val = 0
        for r in rows:
            v = live.get(r.get("code", ""))
            if not v:
                continue
            r["close"] = v["px"]
            if v["mcap"] > 0.0:
                r["mktcap"] = v["mcap"]
                n_cap += 1
            if v["val"] > 0.0:
                r["turnover"] = v["val"]
                n_val += 1
        if n_cap >= len(rows) // 2:
            mcap_src = f"naver-live {live_hhmm}"
        if n_val >= len(rows) // 2:
            val_src = f"naver-live {live_hhmm}"
        print(f"[universe_feed] 네이버 시세 {len(live)}/{len(rows)}종목 — 시총 {n_cap}·거래대금 {n_val} 교체 "
              f"(시총 축 {mcap_src}, 거래대금 축 {val_src}).")
    if val_src.startswith("data.go.kr") and served < _prev_weekday_ymd():
        print(f"[universe_feed] 거래대금 축이 data.go.kr {served} 기준(직전 평일 {_prev_weekday_ymd()} 미만) — "
              f"이틀 전 랭킹으로는 만들지 않는다. 직전 파일 유지.", file=sys.stderr)
        return None

    markets = ["KOSPI", "KOSDAQ"] if market == "ALL" else [market]
    seen: set[str] = set()
    universe: list[dict] = []
    per_market: dict[str, int] = {}
    for mk in markets:
        pool = [r for r in rows
                if r.get("market") == mk
                and r.get("turnover", 0.0) >= min_turnover
                and r.get("mktcap", 0.0) > 0.0]
        if not pool:
            print(f"[universe_feed] {mk} 유효 종목 0개(turnover>={min_turnover:.0f}).")
            continue
        by_cap = sorted(pool, key=lambda r: r["mktcap"],   reverse=True)[:n_mktcap]
        by_val = sorted(pool, key=lambda r: r["turnover"], reverse=True)[:n_turnover]
        # union — 시총순 먼저(기동 점검 우선), 이어 거래대금 상위 중 미포함 중형주. code 중복 제거.
        added = 0
        for r in by_cap + by_val:
            code = r["code"]
            if not code or code in seen:
                continue
            seen.add(code)
            universe.append({
                "ticker": code,
                "name":   r.get("name", ""),
                "close":  round(r.get("close", 0.0)),
                "market": mk,   # 시장별 risk_off 게이트용(C++ UniverseScanner가 코스피/코스닥 지수 분기).
            })
            added += 1
        per_market[mk] = added

    if not universe:
        print(f"[universe_feed] 유효 종목 0개(turnover>={min_turnover:.0f}).")
        return None

    breakdown = " ".join(f"{mk}={n}" for mk, n in per_market.items())
    print(f"[universe_feed] 기준일 {served}: 시총 top{n_mktcap} ∪ 거래대금 top{n_turnover} "
          f"= {len(universe)}종목 ({breakdown}). ETF-free.")
    doc = {
        "source":   "data.go.kr:getStockPriceInfo",
        "market":   market,
        "basDt":    served,
        "count":    len(universe),
        "universe": universe,
    }
    if with_market_map:
        # 전 종목 코드→시장 사전. C++ 스캐너가 KIS 랭킹축으로 들어온 티커의 시장을 여기서
        #  해석한다. 이게 없으면 태그 없는 티커가 KOSPI로 간주돼 kosdaq_enabled 게이트가 샌다.
        #  universe(top-N)와 달리 스냅샷 전종목을 담으므로 PIT 백필 파일에는 넣지 않는다
        #  (246일 × 수천 항목이면 산출물이 불필요하게 커진다).
        doc["market_map"] = {r["code"]: r["market"] for r in rows
                             if r.get("code") and r.get("market") in ("KOSPI", "KOSDAQ")}
        # 전 종목 코드→종목명. 알림 보조 프로세스가 체결 메시지에 이름을 붙이는 데 쓴다.
        #  universe(top-N)에만 이름을 두면 랭킹축으로 들어온 종목이 코드로만 뜬다.
        #  같은 스냅샷에 이미 있는 값이라 추가 조회가 없다.
        doc["name_map"] = {r["code"]: r["name"] for r in rows
                           if r.get("code") and r.get("name")}
        print(f"[universe_feed] market_map {len(doc['market_map'])}종목 · "
              f"name_map {len(doc['name_map'])}종목 동봉.")
    return doc


def main() -> int:
    parser = argparse.ArgumentParser(description="data.go.kr 거래대금 상위 유니버스 피드")
    # 시총 축은 기본으로 끈다 — 대형주가 거래 없이도 자리를 먹었다(D-146). 옛 선정을 재현할 때만 준다.
    parser.add_argument("--n-mktcap",   type=int, default=0, help="시총 상위 N(기본 0=끔, D-146)")
    parser.add_argument("--n-turnover", type=int, default=100, help="거래대금 상위 N")
    parser.add_argument("--date", default=None, help="기준일 YYYY-MM-DD (기본 T-1, 백오프 자동)")
    parser.add_argument("--min-turnover", type=float, default=1e9, help="최소 거래대금(원)")
    parser.add_argument("--market", default="ALL", choices=["KOSPI", "KOSDAQ", "ALL"],
                        help="유니버스 시장(기본 ALL). ALL=코스피·코스닥 각각 top-N union, 종목별 market 태그 부여.")
    parser.add_argument("--out", default=str(_OUT_PATH), help="출력 JSON 경로")
    parser.add_argument("--no-live", action="store_true",
                        help="네이버 시세로 시총·거래대금을 바꾸지 않고 data.go.kr 스냅샷 값만 쓴다(백필·점검용).")
    arguments = parser.parse_args()

    on_date = arguments.date or _yesterday_iso()
    doc = build(on_date, arguments.n_mktcap, arguments.n_turnover, arguments.min_turnover, arguments.market,
                with_market_map=True, use_live=not arguments.no_live)
    if doc is None:
        return 1

    out = Path(arguments.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    # 엔진이 파일이 다시 쓰일 때마다 읽으므로 반쯤 쓰인 파일이 보이면 안 된다 —
    #  임시 파일에 다 쓴 뒤 한 번에 바꿔 넣는다(os.replace는 같은 볼륨에서 원자적).
    tmp = out.with_suffix(out.suffix + ".tmp")
    tmp.write_text(json.dumps(doc, ensure_ascii=False, indent=2), encoding="utf-8")
    os.replace(tmp, out)
    print(f"[universe_feed] 기록 완료 → {out} ({doc['count']}종목, 목록 기준일 {doc['basDt']})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
