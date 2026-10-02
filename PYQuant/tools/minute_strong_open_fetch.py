#!/usr/bin/env python3
"""스터디 30-A(강한 종목 첫 VWAP 눌림)용 1분봉 2단계 적재 — KIS FHKST03010230, 모의 앱키 전용.

왜 2단계인가:
  "09:30에 +6~20%이고 그 안에서 거래대금 상위 40"인 종목을 고르려면 후보 전부의 09:00~09:30 분봉이 있어야 한다.
  일봉 상위집합(고가/전일종가 >= 1.06)은 하루 평균 344종목(2025-09~2026-09)이라 전부 하루치(4콜)를 받으면
  33만 콜, 모의 초당 2콜로 45시간이다. 그래서
    1단계: 후보마다 09:30에서 끝나는 1콜(120봉 = 그날 09:00~09:30 31봉 + 전날 꼬리 89봉)만 받는다.
           전날 꼬리의 마지막 봉(15:30 단일가) 종가가 그날의 무수정 전일 종가다.
    2단계: 09:30 순위 상위 40만 09:31~15:30을 3콜로 마저 받는다.
  1단계도 줄인다(미래 참조 없음 — 일봉은 "받을지"만 정하고 선정은 09:30 분봉으로만 한다):
    후보를 하루 거래대금 상한(고가 x 거래량)의 내림차순으로 받는다. 09:30까지 누적 거래대금은 그 상한을
    넘을 수 없으므로, 이미 자격자 40개가 모였고 다음 후보의 상한이 40번째 자격자의 09:30 거래대금보다
    작으면 그 뒤 후보는 전부 순위에 못 든다 — 받지 않고 끝낸다.
    단, 일봉이 수정주가라(분할 이전 날짜는 가격이 작아져 상한이 과소) 창 안에 분할·병합 흔적이 있는 종목은
    잘라내지 않고 항상 받는다(data.go.kr 월말 무수정 종가 / 네이버 수정 종가 비율이 1에서 2% 넘게 벗어난 종목).

입력: research/studies/30_strong_stock_strategies/data/pairs_all.json (build_minute_pairs.py 산출)
      PYQuant/data/bars_all_pit_v2.parquet, PYQuant/data/cache/datagokr_shares/univ_*.parquet
      이미 있는 하루치 분봉 PYQuant/data/minute/<code>/<ymd>.parquet 는 콜 없이 그대로 쓴다.
산출: <out>/open30/<ymd>.parquet   1단계 받은 봉 전부(code, date, hms, open, high, low, close, volume)
      <out>/open30/<ymd>.json      그날 판정(후보 수·받은 수·잘라낸 수·자격자 순위·무수정 전일 종가)
      <out>/minute/<code>/<ymd>.parquet  2단계 하루치(09:00~15:30, PYQuant/data/minute 와 같은 열)
      <out>/_failed.txt          빈 응답·한도 초과가 끝까지 풀리지 않은 (ymd, code, 단계)
재개: open30/<ymd>.json 이 있으면 1단계를 건너뛰고, 2단계는 파일이 있으면 건너뛴다.

KIS 과거 분봉 창(2026-10-01 실호출): 2025-09-18 까지 나오고 2025-09-17 부터 0봉 — 하루에 하루씩 밀려난다.
그래서 오래된 날짜부터 받는다.

실행(저장소 루트):
  py PYQuant/tools/minute_strong_open_fetch.py --start 2025-09-18 --end 2026-09-29
  py PYQuant/tools/minute_strong_open_fetch.py --start 2025-09-18 --end 2025-09-18 --max-candidates 5   # 기동 점검
  모의 트레이더가 같은 앱키를 쓰므로 기본값은 08:00~20:05 사이에 쉰다(--quiet 0800-2005, 끄려면 --quiet none).
"""
from __future__ import annotations

import argparse
import datetime as dt
import glob
import json
import sys
import time
from pathlib import Path

import pandas as pd

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "PYQuant"))
from kis.client import from_config   # noqa: E402

STUDY_DATA = ROOT / "research" / "studies" / "30_strong_stock_strategies" / "data"
BARS = ROOT / "PYQuant" / "data" / "bars_all_pit_v2.parquet"
SHARED_MINUTE = ROOT / "PYQuant" / "data" / "minute"
DATAGOKR_GLOB = str(ROOT / "PYQuant" / "data" / "cache" / "datagokr_shares" / "univ_*.parquet")
PATH = "/uapi/domestic-stock/v1/quotations/inquire-time-dailychartprice"
TRANSACTION_ID = "FHKST03010230"
RATE_LIMIT_CODE = "EGW00201"
COLUMNS = ["date", "hms", "time", "open", "high", "low", "close", "volume"]
SNAPSHOT_HMS = "093000"   # 이 봉 미만(09:00~09:29 봉)의 마지막 종가가 09:30 시점 가격


class Pacer:
    """호출 사이 최소 간격을 지킨다(모의 계정 초당 2콜 상한)."""

    def __init__(self, interval: float):
        self.interval = interval
        self.last = 0.0

    def wait(self) -> None:
        gap = time.monotonic() - self.last
        if gap < self.interval:
            time.sleep(self.interval - gap)
        self.last = time.monotonic()


def wait_quiet_hours(quiet: str) -> None:
    """quiet='0800-2005' 이면 그 시각 안에서는 끝날 때까지 잔다. 모의 트레이더와 앱키 한도를 나누지 않기 위해서다."""
    if quiet == "none":
        return
    begin, end = quiet.split("-")
    while True:
        now = dt.datetime.now().strftime("%H%M")
        if not (begin <= now < end):
            return
        print(f"  [쉼] {now} 은 {quiet} 안이다 — 5분 뒤 다시 본다", flush=True)
        time.sleep(300)


def call(client, pacer: Pacer, code: str, trade_date: str, hour: str, include_previous: bool) -> tuple[list[dict], str]:
    """한 콜. (output2 행, 상태) — 상태는 ok / empty / limit / error."""
    query_parameters = {"FID_COND_MRKT_DIV_CODE": "J", "FID_INPUT_ISCD": code, "FID_INPUT_HOUR_1": hour,
              "FID_INPUT_DATE_1": trade_date, "FID_PW_DATA_INCU_YN": "Y" if include_previous else "N",
              "FID_FAKE_TICK_INCU_YN": "N"}
    for backoff in (0, 2, 5, 15):
        if backoff:
            time.sleep(backoff)
        pacer.wait()
        data = client._get(PATH, query_parameters, TRANSACTION_ID)
        if data.get("msg_cd") == RATE_LIMIT_CODE:
            continue
        if data.get("rt_cd") not in ("0", None) or not data:
            # 토큰 만료(하룻밤을 넘기는 적재)일 수 있다 — 캐시가 유효하면 파일만 읽고, 만료면 새로 받는다.
            client.authenticate()
            continue
        rows = [row for row in (data.get("output2") or []) if row.get("stck_cntg_hour") and row.get("stck_prpr")]
        return rows, ("ok" if rows else "empty")
    return [], "limit"


def to_bar(row: dict) -> dict:
    time_hhmmss = row["stck_cntg_hour"]
    return {"date": row["stck_bsop_date"], "hms": time_hhmmss, "time": time_hhmmss[:4],
            "open": float(row.get("stck_oprc") or 0), "high": float(row.get("stck_hgpr") or 0),
            "low": float(row.get("stck_lwpr") or 0), "close": float(row.get("stck_prpr") or 0),
            "volume": int(float(row.get("cntg_vol") or 0))}


def snapshot(bars: pd.DataFrame, previous_close: float) -> dict | None:
    """09:30 시점 등락률·누적 거래대금(봉 종가 x 거래량 합). 09:30 미만 봉이 없으면 None."""
    early = bars[bars.hms < SNAPSHOT_HMS].sort_values("hms")
    if early.empty or not previous_close:
        return None
    price = float(early.close.iloc[-1])
    return {"price_0930": price, "gain_0930": price / previous_close - 1,
            "value_0930": float((early.close * early.volume).sum()), "prev_close": previous_close}


def split_flagged_codes(bars: pd.DataFrame, start: str) -> set[str]:
    """창 안에 분할·병합 흔적이 있는 종목. 월말 data.go.kr 무수정 종가와 네이버 수정 종가 비율이 2% 넘게 벗어나면 표시."""
    flagged: set[str] = set()
    adjusted = bars.set_index(["code", "Date"]).Close
    for path in sorted(glob.glob(DATAGOKR_GLOB)):
        month = pd.read_parquet(path, columns=["ticker", "close", "date"])
        if month.date.max() < pd.Timestamp(start) - pd.Timedelta(days=40):
            continue
        for ticker, close, date in month.itertuples(index=False):
            key = (ticker, date)
            if key in adjusted.index and adjusted[key] > 0 and close > 0:
                if abs(close / adjusted[key] - 1) > 0.02:
                    flagged.add(ticker)
    return flagged


def run_day(client, pacer, trade_date, codes, bars_day, previous_adjusted, flagged, out, top, max_candidates, quiet):
    """하루 1단계 + 2단계. 반환은 그날 판정 dict."""
    open_directory, minute_directory = out / "open30", out / "minute"
    verdict_path = open_directory / f"{trade_date}.json"
    if verdict_path.exists():
        verdict = json.loads(verdict_path.read_text(encoding="utf-8"))
    else:
        ranked = bars_day.loc[bars_day.code.isin(codes)].assign(bound=lambda frame: frame.High * frame.Volume)
        ranked = ranked.sort_values("bound", ascending=False)
        if max_candidates:
            ranked = ranked.head(max_candidates)
        fetched_frames, snapshots, failed = [], {}, []
        local = pruned = calls = 0
        for code, bound in zip(ranked.code, ranked.bound):
            qualified = sorted((item["value_0930"] for item in snapshots.values()
                                if 0.06 <= item["gain_0930"] <= 0.20), reverse=True)
            if len(qualified) >= top and bound < qualified[top - 1] and code not in flagged:
                pruned += 1
                continue
            shared = SHARED_MINUTE / code / f"{trade_date}.parquet"
            if shared.exists():
                day_bars = pd.read_parquet(shared)
                # 수정/무수정 비율 = 그날 무수정 시가 / 그날 수정 시가. 전일 수정 종가에 곱해 무수정 전일 종가를 만든다.
                open_adjusted = float(bars_day.loc[bars_day.code == code, "Open"].iloc[0])
                first_open = float(day_bars.sort_values("hms").open.iloc[0])
                factor = first_open / open_adjusted if open_adjusted else 1.0
                item = snapshot(day_bars, previous_adjusted.get(code, 0) * factor)
                local += 1
            else:
                wait_quiet_hours(quiet)
                rows, status = call(client, pacer, code, trade_date, SNAPSHOT_HMS, include_previous=True)
                calls += 1
                if status != "ok":
                    failed.append((code, status))
                    continue
                frame = pd.DataFrame([to_bar(row) for row in rows])
                today = frame[frame.date == trade_date].copy()
                previous = frame[frame.date < trade_date].sort_values(["date", "hms"])
                previous_close = float(previous.close.iloc[-1]) if not previous.empty else 0.0
                if not previous.empty and previous.hms.iloc[-1] < "152000":
                    previous_close = 0.0   # 전날 꼬리가 장 마감까지 안 왔다(전날 정지 등) — 아래에서 일봉으로 대신한다
                if not previous_close and not today.empty:
                    open_adjusted = float(bars_day.loc[bars_day.code == code, "Open"].iloc[0])
                    factor = float(today.sort_values("hms").open.iloc[0]) / open_adjusted if open_adjusted else 1.0
                    previous_close = previous_adjusted.get(code, 0) * factor
                today.insert(0, "code", code)
                fetched_frames.append(today)
                item = snapshot(today, previous_close)
            if item:
                snapshots[code] = item
        open_directory.mkdir(parents=True, exist_ok=True)
        if fetched_frames:
            pd.concat(fetched_frames, ignore_index=True).to_parquet(open_directory / f"{trade_date}.parquet", index=False)
        qualifiers = sorted(((code, item) for code, item in snapshots.items() if 0.06 <= item["gain_0930"] <= 0.20),
                            key=lambda pair: -pair[1]["value_0930"])
        verdict = {"ymd": trade_date, "candidates": len(codes), "examined": len(ranked), "calls_stage1": calls,
                   "local": local, "pruned": pruned, "failed": failed,
                   "qualifiers": [{"code": code, "rank": index + 1, **item} for index, (code, item) in enumerate(qualifiers)],
                   "snapshots": snapshots}
        if failed:
            with open(out / "_failed.txt", "a", encoding="utf-8") as handle:
                for code, status in failed:
                    handle.write(f"{trade_date}\t{code}\tstage1\t{status}\n")
        if len(failed) > max(5, 0.2 * calls):
            # 실패가 많으면 판정을 남기지 않는다 — 다음 실행이 그날을 처음부터 다시 받는다.
            print(f"  {trade_date} 실패 {len(failed)}/{calls} — 판정 보류, 재실행 때 다시 받는다", flush=True)
            verdict["calls_stage2"] = 0
            return verdict
        verdict_path.write_text(json.dumps(verdict, ensure_ascii=False), encoding="utf-8")

    calls2 = 0
    for entry in verdict["qualifiers"][:top]:
        code = entry["code"]
        if (SHARED_MINUTE / code / f"{trade_date}.parquet").exists():
            continue
        destination = minute_directory / code / f"{trade_date}.parquet"
        if destination.exists():
            continue
        open_file = open_directory / f"{trade_date}.parquet"
        morning = pd.read_parquet(open_file) if open_file.exists() else pd.DataFrame(columns=["code"] + COLUMNS)
        bars = {row["hms"]: row for row in morning[morning.code == code].drop(columns="code").to_dict("records")}
        hour, status = "153000", "ok"
        for _ in range(4):
            wait_quiet_hours(quiet)
            rows, status = call(client, pacer, code, trade_date, hour, include_previous=False)
            calls2 += 1
            if status != "ok":
                break
            for row in rows:
                if row["stck_bsop_date"] == trade_date:
                    bars[row["stck_cntg_hour"]] = to_bar(row)
            earliest = min(row["stck_cntg_hour"] for row in rows)
            if earliest <= SNAPSHOT_HMS:
                break
            hour = (dt.datetime.strptime(earliest, "%H%M%S") - dt.timedelta(minutes=1)).strftime("%H%M%S")
        if status == "limit":
            with open(out / "_failed.txt", "a", encoding="utf-8") as handle:
                handle.write(f"{trade_date}\t{code}\tstage2\tlimit\n")
            continue
        if bars:
            destination.parent.mkdir(parents=True, exist_ok=True)
            pd.DataFrame(sorted(bars.values(), key=lambda bar: bar["hms"]))[COLUMNS].to_parquet(destination, index=False)
    verdict["calls_stage2"] = calls2
    return verdict


def main() -> int:
    parser = argparse.ArgumentParser(description="스터디 30-A 1분봉 2단계 적재(모의 앱키)")
    parser.add_argument("--start", required=True)
    parser.add_argument("--end", required=True)
    parser.add_argument("--pairs", default=str(STUDY_DATA / "pairs_all.json"))
    parser.add_argument("--out", default=str(STUDY_DATA))
    parser.add_argument("--top", type=int, default=40, help="09:30 거래대금 순위 몇 위까지 하루치를 받나")
    parser.add_argument("--interval", type=float, default=0.55, help="콜 간 최소 간격(초). 모의 상한 초당 2콜")
    parser.add_argument("--quiet", default="0800-2005", help="이 시각 안에서는 쉰다(HHMM-HHMM, none 이면 안 쉼)")
    parser.add_argument("--max-candidates", type=int, default=0, help="기동 점검용: 하루 후보를 앞에서 N개로 자른다")
    parser.add_argument("--config", default="Quant/config/config_dev_paper.json")
    parser.add_argument("--allow-live-key", action="store_true",
                        help="실계좌 앱키 허용(시세 조회만 한다). 실계좌 매매 시간과 겹치지 않게 --quiet 로 07:20~20:05 를 비운다")
    arguments = parser.parse_args()

    config = json.loads((ROOT / arguments.config).read_text(encoding="utf-8"))
    if not config.get("kis", {}).get("is_paper") and not arguments.allow_live_key:
        print("[중단] 모의 계정(is_paper=true) 절이 아니다 — 실계좌 앱키는 --allow-live-key 를 줄 때만 쓴다")
        return 2
    client = from_config(str(ROOT / arguments.config), "kis")
    if not client.authenticate():
        print("[중단] KIS 인증 실패")
        return 1

    first_date, last_date = arguments.start.replace("-", ""), arguments.end.replace("-", "")
    pairs = {trade_date: codes for trade_date, codes in json.loads(Path(arguments.pairs).read_text(encoding="utf-8")).items()
             if first_date <= trade_date <= last_date}
    bars = pd.read_parquet(BARS, columns=["Date", "code", "Open", "High", "Low", "Close", "Volume"])
    bars = bars[bars.Date >= pd.Timestamp(arguments.start) - pd.Timedelta(days=60)].sort_values(["code", "Date"])
    bars["prev_close"] = bars.groupby("code").Close.shift(1)
    flagged = split_flagged_codes(bars, arguments.start)
    print(f"[적재] {len(pairs)}일 · 후보 {sum(map(len, pairs.values())):,}쌍 · 분할 표시 {len(flagged)}종목", flush=True)

    out = Path(arguments.out)
    pacer = Pacer(arguments.interval)
    began = time.time()
    total1 = total2 = 0
    for trade_date in sorted(pairs):
        bars_day = bars[bars.Date == pd.Timestamp(trade_date)]
        previous_adjusted = dict(zip(bars_day.code, bars_day.prev_close.fillna(0)))
        verdict = run_day(client, pacer, trade_date, pairs[trade_date], bars_day, previous_adjusted, flagged, out,
                          arguments.top, arguments.max_candidates, arguments.quiet)
        total1 += verdict.get("calls_stage1", 0)
        total2 += verdict.get("calls_stage2", 0)
        print(f"  {trade_date} 후보 {verdict['candidates']} 받음 {verdict['calls_stage1']} 로컬 {verdict['local']} "
              f"잘라냄 {verdict['pruned']} 자격 {len(verdict['qualifiers'])} 하루치콜 {verdict.get('calls_stage2', 0)} "
              f"실패 {len(verdict['failed'])} | 누적 콜 {total1 + total2:,} {(time.time() - began) / 60:.1f}분", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
