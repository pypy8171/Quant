#!/usr/bin/env python3
"""1분봉 백필(매매 목록 또는 전 시장 보통주) → PYQuant/data/minute/<종목코드>/<YYYYMMDD>.parquet

기본 사용은 --trades: 일봉 시험에서 실제 매수가 난 (종목, 신호일, 매도일) 목록을 받아 그 구간의
거래일만 받는다. --all-market은 전 시장 보통주 전부(2026-10-10 사용자 결정으로 중단, 900종목×1일만 받음).

minute_backfill.py는 PIT 유니버스(시총·거래대금 상위) 위주로 받아 820종목에 그쳤다. 이 도구는
코스피·코스닥 보통주 전부(상장폐지 종목 포함)를 KIS가 남겨 둔 가장 먼 날부터 받는다.

KIS 사실(2026-10-10 확인):
  - TR FHKST03010230, GET /uapi/domestic-stock/v1/quotations/inquire-time-dailychartprice
    (주식일별분봉조회). 입력 FID_COND_MRKT_DIV_CODE(J=KRX)·FID_INPUT_ISCD·FID_INPUT_HOUR_1(HHMMSS)·
    FID_INPUT_DATE_1(YYYYMMDD)·FID_PW_DATA_INCU_YN·FID_FAKE_TICK_INCU_YN. 1콜 최대 120봉, "최대 1년"
    보관. 출처: 공식 샘플 koreainvestment/open-trading-api
    examples_llm/domestic_stock/inquire_time_dailychartprice (MCP kis-code-assistant가 읽는 저장소와 같음.
    이 세션에서는 MCP 도구가 노출되지 않아 원본 샘플을 직접 읽었다).
  - 실측 보관 한계: 2026-10-10에 2025-09-24는 받히고 2025-09-23은 0행. 보관이 날마다 앞으로 밀리므로
    오래된 날부터 받는다(늦게 받는 쪽이 사라진다).
  - 상장폐지 종목도 보관 기간 안이면 받힌다(448740 2025-10-13 등 3종목 실측).
  - 휴장일을 넣으면 직전 거래일 행이 온다 — stck_bsop_date로 걸러 낸다.
  - 초당 한도는 실전 앱키당 20콜(저장소 PYQuant/tools/minute_backfill.py --section 설명과 같은 값).
    여기서는 --rate(기본 18)로 전역 제한을 건다. 한도 초과 응답(EGW00201)은 쉬었다 다시 부른다.

대상 (종목,날짜) 쌍은 PYQuant/data/bars_all_pit_v2.parquet(전 종목 일봉, 상폐 포함)에서 그날
거래량>0인 행만 쓴다 — 거래정지일에 빈 응답을 기다리느라 시간을 버리지 않게.
보통주 판정은 코드 끝자리 0(우선주는 5·7·9·K·L)이고 스팩은 뺀다(내가 정함).

이어받기: 대상 파일이 이미 있으면 건너뛴다(덮어쓰지 않는다). 쓰기는 .tmp에 쓴 뒤 이름을 바꾼다.
멈춤: --stop-at 시각이 지나면 새 요청을 내지 않고 끝낸다(월요일 07:50 트레이더가 같은 앱키로 기동).
진행: PYQuant/data/minute/_fullmarket_progress.json(1분마다), 실패 쌍은 _fullmarket_failed.txt.

사용:
  py -X utf8 PYQuant/tools/minute_fullmarket_backfill.py --trades trades.csv --dry-run   # 쌍 수만 센다
  py -X utf8 PYQuant/tools/minute_fullmarket_backfill.py --trades trades.csv --stop-at 2026-10-12T07:20
  --trades 파일(csv 또는 parquet) 열: code, entry_date, exit_date (날짜는 YYYY-MM-DD 또는 YYYYMMDD).
  구간 [entry_date, exit_date]에서 그 종목 거래량>0인 날만 펼친다. KIS 보관 한계(--start) 밖 날짜는
  빈 응답만 오므로 빼고, 빠진 쌍 수를 따로 알린다.
"""
import argparse
import datetime as dt
import json
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

_PYQUANT_ROOT = Path(__file__).resolve().parents[1]
if str(_PYQUANT_ROOT) not in sys.path:
    sys.path.insert(0, str(_PYQUANT_ROOT))

from kis.client import from_config   # noqa: E402

_BARS = _PYQUANT_ROOT / "data" / "bars_all_pit_v2.parquet"
_OUT_DIR = _PYQUANT_ROOT / "data" / "minute"
_PATH = "/uapi/domestic-stock/v1/quotations/inquire-time-dailychartprice"
_TRANSACTION_ID = "FHKST03010230"
_MIN_BARS = 300


class RateLimiter:
    """전 스레드 공용 호출 간격 제한. 초당 rate회를 넘지 않게 다음 호출 시각을 나눠 준다."""

    def __init__(self, rate: float):
        self.interval = 1.0 / rate
        self.next_time = time.monotonic()
        self.lock = threading.Lock()

    def wait(self) -> None:
        with self.lock:
            now = time.monotonic()
            slot = max(now, self.next_time)
            self.next_time = slot + self.interval
        delay = slot - time.monotonic()
        if delay > 0:
            time.sleep(delay)


def build_pairs(start: str, end: str) -> list[tuple[str, str]]:
    """(날짜, 종목) 쌍. 오래된 날짜부터 — 보관 한계가 날마다 밀리기 때문이다."""
    import pandas as pd
    frame = pd.read_parquet(_BARS, columns=["Date", "code", "name", "Volume"])
    frame = frame[(frame.Date >= pd.Timestamp(start)) & (frame.Date <= pd.Timestamp(end))]
    frame = frame[frame.code.str.match(r"^[0-9A-Z]{5}0$")]
    frame = frame[~frame.name.fillna("").str.contains("스팩")]
    frame = frame[frame.Volume > 0]
    frame = frame.assign(ymd=frame.Date.dt.strftime("%Y%m%d")).sort_values(["ymd", "code"])
    return list(zip(frame.ymd, frame.code))


def build_trade_pairs(path: str, start: str, end: str) -> tuple[list[tuple[str, str]], int]:
    """매매 목록 → (날짜, 종목) 쌍(중복 제거, 오래된 날짜부터)과 보관 한계 밖이라 뺀 쌍 수."""
    import pandas as pd
    trades = pd.read_parquet(path) if path.endswith(".parquet") else pd.read_csv(path, dtype=str)
    trades = trades.assign(
        code=trades["code"].astype(str).str.zfill(6),
        entry=pd.to_datetime(trades["entry_date"].astype(str).str.replace("-", ""), format="%Y%m%d"),
        exit=pd.to_datetime(trades["exit_date"].astype(str).str.replace("-", ""), format="%Y%m%d"),
    )
    codes = set(trades.code)
    frame = pd.read_parquet(_BARS, columns=["Date", "code", "Volume"])
    frame = frame[frame.code.isin(codes) & (frame.Volume > 0)]
    days_by_code = {code: group.Date.values for code, group in frame.groupby("code")}
    wanted: set[tuple[str, str]] = set()
    for row in trades.itertuples(index=False):
        dates = days_by_code.get(row.code)
        if dates is None:
            continue
        for day in dates[(dates >= row.entry.to_datetime64()) & (dates <= row.exit.to_datetime64())]:
            wanted.add((pd.Timestamp(day).strftime("%Y%m%d"), row.code))
    low, high = start.replace("-", ""), end.replace("-", "")
    inside = sorted(pair for pair in wanted if low <= pair[0] <= high)
    return inside, len(wanted) - len(inside)


class Fetcher:
    def __init__(self, client, limiter: RateLimiter, stop_at: dt.datetime):
        self.client = client
        self.limiter = limiter
        self.stop_at = stop_at
        self.token_lock = threading.Lock()

    def stopped(self) -> bool:
        return dt.datetime.now() >= self.stop_at

    def call(self, ticker: str, date_yyyymmdd: str, hour: str) -> list | None:
        """한 페이지. 빈 목록이면 진짜 끝, None이면 실패(한도 초과·네트워크 재시도 소진)."""
        parameters = {
            "FID_COND_MRKT_DIV_CODE": "J",
            "FID_INPUT_ISCD": ticker,
            "FID_INPUT_HOUR_1": hour,
            "FID_INPUT_DATE_1": date_yyyymmdd,
            "FID_PW_DATA_INCU_YN": "Y",
            "FID_FAKE_TICK_INCU_YN": "N",
        }
        for attempt in range(6):
            if self.stopped():
                return None
            with self.token_lock:          # 토큰 갱신이 여러 스레드에서 겹쳐 새 토큰을 여러 번 받지 않게
                self.client._ensure_token()
            self.limiter.wait()
            data = self.client._get(_PATH, parameters, _TRANSACTION_ID)
            if data.get("rt_cd") == "0":
                return data.get("output2") or []
            # EGW00201 = 초당 거래건수 초과. 그 밖의 실패도 잠깐 쉬고 다시 부른다.
            time.sleep(0.5 * (attempt + 1))
        return None

    def fetch_day(self, ticker: str, date_yyyymmdd: str) -> list[dict] | None:
        rows: dict[str, dict] = {}
        hour = "153000"
        for _page in range(6):
            items = self.call(ticker, date_yyyymmdd, hour)
            if items is None:
                return None
            times = []
            for item in items:
                time_of_day = item.get("stck_cntg_hour", "")
                if not time_of_day or not item.get("stck_prpr"):
                    continue
                times.append(time_of_day)
                if item.get("stck_bsop_date") != date_yyyymmdd:   # 휴장일·보관 밖이면 다른 날 행이 온다
                    continue
                try:
                    rows[time_of_day] = {
                        "date": date_yyyymmdd,
                        "hms": time_of_day,
                        "time": time_of_day[:4],
                        "open": float(item.get("stck_oprc", 0) or 0),
                        "high": float(item.get("stck_hgpr", 0) or 0),
                        "low": float(item.get("stck_lwpr", 0) or 0),
                        "close": float(item.get("stck_prpr", 0) or 0),
                        "volume": int(item.get("cntg_vol", 0) or 0),
                    }
                except (ValueError, TypeError):
                    continue
            if not times or len(items) < 120:
                break
            next_hour = int(min(times)) - 100
            if next_hour < 90000:
                break
            hour = f"{next_hour:06d}"
        return [rows[key] for key in sorted(rows)]


def main() -> int:
    parser = argparse.ArgumentParser(description="전 시장 보통주 1분봉 백필(TR FHKST03010230)")
    parser.add_argument("--start", default="2025-09-24", help="시작일(포함). 기본은 2026-10-10 실측 보관 한계")
    parser.add_argument("--end", default="2026-10-08")
    parser.add_argument("--config", default="Quant/config/config_dev_paper.json")
    parser.add_argument("--section", default="quote_kis", help="실전 도메인 시세 계정 절")
    parser.add_argument("--rate", type=float, default=18.0, help="전역 초당 호출 수(한도 20)")
    parser.add_argument("--workers", type=int, default=8)
    parser.add_argument("--stop-at", required=True,
                        help="YYYY-MM-DDTHH:MM. 이 시각 이후 새 요청 금지(트레이더가 같은 앱키로 뜨기 전)")
    parser.add_argument("--trades", default=None, help="매매 목록 csv/parquet(code, entry_date, exit_date)")
    parser.add_argument("--all-market", action="store_true", help="전 시장 보통주 전부(중단된 방식)")
    parser.add_argument("--dry-run", action="store_true")
    arguments = parser.parse_args()

    import pandas as pd

    stop_at = dt.datetime.strptime(arguments.stop_at.replace(" ", "T"), "%Y-%m-%dT%H:%M")
    if arguments.trades:
        pairs, outside = build_trade_pairs(arguments.trades, arguments.start, arguments.end)
        print(f"[fullmarket] 매매 목록에서 KIS 보관 한계({arguments.start}) 밖이라 뺀 쌍 {outside:,}", flush=True)
    elif arguments.all_market:
        pairs = build_pairs(arguments.start, arguments.end)
    else:
        print("[fullmarket] --trades 또는 --all-market 중 하나가 필요하다")
        return 1
    todo = [(date_yyyymmdd, code) for date_yyyymmdd, code in pairs if not (_OUT_DIR / code / f"{date_yyyymmdd}.parquet").exists()]
    print(f"[fullmarket] 대상 {len(pairs):,}쌍 · 이미 있음 {len(pairs) - len(todo):,} · 받을 것 {len(todo):,}",
          flush=True)
    if arguments.dry_run or not todo:
        return 0

    client = from_config(arguments.config, arguments.section)
    if not client.authenticate():
        print("[fullmarket] KIS 인증 실패")
        return 1

    fetcher = Fetcher(client, RateLimiter(arguments.rate), stop_at)
    progress_path = _OUT_DIR / "_fullmarket_progress.json"
    failed_path = _OUT_DIR / "_fullmarket_failed.txt"
    counter_lock = threading.Lock()
    counts = {"saved": 0, "empty": 0, "short": 0, "failed": 0}
    state = {"current_ymd": todo[0][0]}
    started = time.time()

    def write_progress(final: bool = False) -> None:
        with counter_lock:
            done = counts["saved"] + counts["empty"] + counts["failed"]
            elapsed = time.time() - started
            per_second = done / elapsed if elapsed > 0 else 0.0
            remaining = len(todo) - done
            eta = (dt.datetime.now() + dt.timedelta(seconds=remaining / per_second)).strftime("%Y-%m-%d %H:%M") \
                if per_second > 0 else None
            doc = {
                "updated": dt.datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
                "final": final,
                "range": [arguments.start, arguments.end],
                "todo_total": len(todo),
                "done": done,
                "remaining": remaining,
                "pairs_per_second": round(per_second, 2),
                "eta": eta,
                "stop_at": arguments.stop_at,
                "current_ymd": state["current_ymd"],
                **counts,
            }
        temporary = progress_path.with_suffix(".tmp")
        temporary.write_bytes(json.dumps(doc, ensure_ascii=False, indent=1).encode("utf-8"))
        temporary.replace(progress_path)

    def work(pair: tuple[str, str]) -> None:
        date_yyyymmdd, code = pair
        if fetcher.stopped():
            return
        destination = _OUT_DIR / code / f"{date_yyyymmdd}.parquet"
        if destination.exists():
            return
        rows = fetcher.fetch_day(code, date_yyyymmdd)
        with counter_lock:
            state["current_ymd"] = max(state["current_ymd"], date_yyyymmdd)
        if rows is None:
            if fetcher.stopped():
                return
            with counter_lock:
                counts["failed"] += 1
                with open(failed_path, "ab") as handle:
                    handle.write(f"{code}\t{date_yyyymmdd}\n".encode("utf-8"))
            return
        if not rows:
            with counter_lock:
                counts["empty"] += 1
            return
        destination.parent.mkdir(parents=True, exist_ok=True)
        temporary = destination.with_suffix(".parquet.tmp")
        pd.DataFrame(rows).to_parquet(temporary, index=False)
        temporary.replace(destination)
        with counter_lock:
            counts["saved"] += 1
            if len(rows) < _MIN_BARS:
                counts["short"] += 1

    stop_progress = threading.Event()

    def progress_loop() -> None:
        while not stop_progress.wait(60):
            write_progress()

    threading.Thread(target=progress_loop, daemon=True).start()
    with ThreadPoolExecutor(max_workers=arguments.workers) as pool:
        for _ in pool.map(work, todo):
            pass
    stop_progress.set()
    write_progress(final=True)
    print(f"[fullmarket] 끝 {json.dumps(counts, ensure_ascii=False)} → {_OUT_DIR}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
