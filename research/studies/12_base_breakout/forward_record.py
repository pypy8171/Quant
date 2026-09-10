# -*- coding: utf-8 -*-
"""바닥 사건 전방 기록기 — 신호를 파일로만 남기고 주문은 내지 않는다.

이 스터디의 결론은 "엣지가 배선 비용을 감당하는지 아직 모른다"였다. 원인은 독립 사건이
여섯 개뿐이라는 것 하나다. 과거를 더 캘 수는 있어도 한계가 있으니, 앞으로 발생하는 사건을
발생 시점에 그대로 적어 두어 표본을 늘린다. 매매하지 않으므로 비용은 조회뿐이다.

기록하는 것은 두 층이다.
  1) 지수 층 — 매 거래일. 코스피·코스닥 각각의 250일 고점 대비 낙폭과 20일 이동평균 회복
     여부를 남긴다(raw/forward_index.csv). 사건이 언제 열렸는지를 사후에 다시 짜맞추지
     않으려고 매일 적는다.
  2) 종목 층 — 지수 사건이 열린 뒤 15거래일 창 안에서만. 종목이 20일 이동평균을 처음
     회복한 날 그 시점 관측값과 순위를 남긴다(raw/forward_signals.csv).

시세는 네이버 일봉(api.finance.naver.com/siseJson.naver)에서 받는다. 장중 시세 보조
프로세스(scripts/live_prices_feed.py)가 이미 같은 곳을 쓰고 있고, 공공데이터포털은 전일치를
장 시작 전에 주지 않아 하루 뒤처진 값으로 사건일을 오판하게 만든다.

실행:
  py -X utf8 -W ignore research/studies/12_base_breakout/forward_record.py
  py -X utf8 -W ignore research/studies/12_base_breakout/forward_record.py --force-panel
"""
import json
import sys
import time
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import numpy as np
import pandas as pd

sys.path.insert(0, str(Path(__file__).resolve().parent))
from signals import add_features, add_market, t1_ma20_reclaim  # noqa: E402

sys.stdout.reconfigure(encoding="utf-8")
HERE = Path(__file__).resolve().parent
RAW = HERE / "raw"
PANEL = RAW / "forward_panel.parquet"
IDX_OUT = RAW / "forward_index.csv"
SIG_OUT = RAW / "forward_signals.csv"

# 지수 낙폭 임계값. 스터디의 사건 정의와 같은 값을 쓴다 — 여기서 새로 고르지 않는다.
THRESHOLD = {"KOSPI": -0.20, "KOSDAQ": -0.25}
WINDOW_DAYS = 15          # 지수 사건 이후 종목 신호를 받는 거래일 수
MIN_TURNOVER = 5e8        # 20일 평균 거래대금 하한(원)
MIN_CLOSE = 1000          # 종가 하한(원)
HEADERS = {"User-Agent": "Mozilla/5.0", "Referer": "https://finance.naver.com/"}

# 사건을 건너 부호가 유지된 세 축. 전부 음의 부호라 낮을수록 유리하다.
#  셋 다 "아직 반등에 참여하지 않았다"의 다른 표현이므로 사실상 한 축으로 본다.
SURVIVING = ["바닥후경과일", "저점대비반등", "거래량급증"]


def fetch_daily(symbol, start, end):
    """네이버 일봉을 표로. 응답이 비면 None."""
    url = ("https://api.finance.naver.com/siseJson.naver?symbol=%s&requestType=1"
           "&startTime=%s&endTime=%s&timeframe=day" % (symbol, start, end))
    req = urllib.request.Request(url, headers=HEADERS)

    with urllib.request.urlopen(req, timeout=15) as r:
        text = r.read().decode("utf-8").strip()

    rows = json.loads(text.replace(chr(39), chr(34)))

    if len(rows) < 2:
        return None

    d = pd.DataFrame(rows[1:], columns=rows[0][:len(rows[1])])
    d = d.rename(columns={"날짜": "date", "시가": "open", "고가": "high",
                          "저가": "low", "종가": "close", "거래량": "volume"})
    d["date"] = pd.to_datetime(d["date"].astype(str), format="%Y%m%d")
    keep = ["date", "open", "high", "low", "close", "volume"]

    for c in keep[1:]:
        d[c] = pd.to_numeric(d[c], errors="coerce")

    return d[keep].dropna().sort_values("date").reset_index(drop=True)


def drop_incomplete(d):
    """장 마감 전이면 오늘 봉을 버린다.

    네이버는 장중에도 오늘 행을 주는데 그 종가는 현재가라 확정이 아니다. 그대로 쓰면
    20일 이동평균 회복 여부가 하루 종일 깜빡이고, 그날 기록한 순위가 마감 뒤 값과 달라진다.
    전방 기록의 값어치는 "그때 그 값 그대로"에 있으므로 확정봉만 남긴다.
    """
    now = pd.Timestamp.now()
    today = now.normalize()

    if len(d) and d["date"].iloc[-1] == today and (now.hour, now.minute) < (15, 40):
        return d.iloc[:-1].reset_index(drop=True)

    return d


def load_codes():
    """수집 대상 종목. 스터디 메타를 그대로 쓴다."""
    meta = pd.read_csv(RAW / "meta.csv", dtype={"code": str})
    meta = meta[meta["market"].isin(["KOSPI", "KOSDAQ"])].drop_duplicates("code")
    return meta[["code", "name", "market"]].reset_index(drop=True)


def index_state(market):
    """지수 한 개의 오늘 상태. 낙폭과 20일 이동평균 회복 여부를 돌려준다."""
    symbol = "KOSPI" if market == "KOSPI" else "KOSDAQ"
    end = pd.Timestamp.today().strftime("%Y%m%d")
    start = (pd.Timestamp.today() - pd.Timedelta(days=700)).strftime("%Y%m%d")
    d = fetch_daily(symbol, start, end)

    if d is not None:
        d = drop_incomplete(d)

    if d is None or len(d) < 260:
        return None

    c = d["close"]
    d["ma20"] = c.rolling(20).mean()
    d["hi250"] = c.rolling(250, min_periods=120).max()
    d["dd"] = c / d["hi250"] - 1
    d["reclaim"] = (c > d["ma20"]) & (c.shift(1) <= d["ma20"].shift(1))
    d["event"] = d["reclaim"] & (d["dd"] <= THRESHOLD[market])
    d["market"] = market

    return d


def recent_event(idx, market):
    """가장 최근 사건과 그로부터 지난 거래일 수. 창 밖이면 경과일이 None."""
    pos = np.flatnonzero(idx["event"].values)

    if len(pos) == 0:
        return None, None

    last = int(pos[-1])
    elapsed = len(idx) - 1 - last

    if elapsed > WINDOW_DAYS:
        return idx["date"].iloc[last], None

    return idx["date"].iloc[last], elapsed


def seed_panel():
    """과거 패널을 씨앗으로 복사한다. 처음 한 번만 든다."""
    if PANEL.exists():
        return pd.read_parquet(PANEL)

    src = RAW / "universe_long.parquet"

    if not src.exists():
        raise SystemExit("씨앗 패널이 없다: %s — fetch_universe_long.py를 먼저 돌린다" % src)

    d = pd.read_parquet(src)
    # 씨앗을 장중에 받았으면 마지막 날짜가 미확정 종가다. 그 하루를 지우고 다음 갱신이 다시 받게 한다.
    now = pd.Timestamp.now()

    if (now.hour, now.minute) < (15, 40):
        d = d[d["date"] < now.normalize()]

    d.to_parquet(PANEL, index=False)
    print("패널 씨앗 복사: %d행 %d종목" % (len(d), d["code"].nunique()), flush=True)

    return d


def refresh_panel(panel, codes):
    """패널을 오늘까지 늘린다. 종목별로 마지막 날짜 다음부터만 받는다."""
    have = panel.groupby("code")["date"].max().to_dict()
    end = pd.Timestamp.today().strftime("%Y%m%d")
    fallback = (pd.Timestamp.today() - pd.Timedelta(days=500)).strftime("%Y%m%d")

    def one(code):
        last = have.get(code)
        start = (last + pd.Timedelta(days=1)).strftime("%Y%m%d") if last is not None else fallback

        if start > end:
            return None

        for _ in range(3):
            try:
                d = fetch_daily(code, start, end)

                d = drop_incomplete(d) if d is not None else None

                if d is None or d.empty:
                    return None

                d["code"] = code
                return d
            except Exception:
                time.sleep(0.4)

        return None

    with ThreadPoolExecutor(max_workers=8) as ex:
        got = [x for x in ex.map(one, codes) if x is not None and not x.empty]

    if not got:
        print("패널 신규 행 없음", flush=True)
        return panel

    add = pd.concat(got, ignore_index=True)
    out = pd.concat([panel, add], ignore_index=True)
    out = out.drop_duplicates(["code", "date"], keep="last").sort_values(["code", "date"])
    out.to_parquet(PANEL, index=False)
    print("패널 갱신: +%d행 (%d종목)" % (len(add), add["code"].nunique()), flush=True)

    return out.reset_index(drop=True)


def feats_at(d, i):
    """신호 시점 i에서만 보이는 값. 미래 봉을 참조하지 않는다(run_crosssec_pit.py와 같은 정의)."""
    c = d["close"].iloc[i]
    return dict(
        낙폭깊이=float(-d["dd"].iloc[i]),
        거래대금로그=float(np.log(max(d["tv20"].iloc[i], 1))),
        거래량급증=float(d["volume"].iloc[i] / max(d["vol20"].iloc[i], 1)),
        변동성=float(d["vol20"].iloc[i]),
        저점대비반등=float(c / max(d["lo60"].iloc[i], 1) - 1),
        바닥후경과일=float(d["days_since_lo60"].iloc[i]),
        상대강도120=float(d["rs"].iloc[i] / max(d["rs"].iloc[max(0, i - 120)], 1e-9) - 1),
        상대강도20=float(d["rs"].iloc[i] / max(d["rs"].iloc[max(0, i - 20)], 1e-9) - 1),
        직전1년상승=float(c / max(d["close"].iloc[max(0, i - 250)], 1) - 1),
        주가로그=float(np.log(max(c, 1))),
        이평200이격=float(c / max(d["ma200"].iloc[i], 1) - 1),
    )


def scan_signals(panel, meta, idx, market, event_date, days):
    """창 안의 각 날짜에 20일 이동평균을 처음 회복한 종목을 골라 관측값과 순위를 매긴다.

    창이 열린 날부터 훑는다. 오늘치만 적으면 기록기를 늦게 켠 창은 앞부분이 비고, 그 결함이
    나중에 "상위권이 잘 나왔다"는 착시로 돌아온다. 패널이 이미 로컬에 있어 소급 비용은 없다.
    """
    codes = set(meta.loc[meta["market"] == market, "code"])
    big = panel[panel["code"].isin(codes)]
    idx_s = idx.rename(columns={"close": "mclose"})[["date", "mclose"]]
    day_rows = {d: [] for d in days}

    for code, d in big.groupby("code", sort=False):
        if len(d) < 300:
            continue

        f = add_market(add_features(d), idx_s).reset_index(drop=True)
        s = t1_ma20_reclaim(f).fillna(False)
        pos = {t: i for i, t in enumerate(f["date"])}

        for day in days:
            i = pos.get(day)

            if i is None or not bool(s.iloc[i]):
                continue      # 그날 봉이 없거나(거래정지) 회복일이 아니면 건너뛴다

            if f["tv20"].iloc[i] < MIN_TURNOVER or f["close"].iloc[i] < MIN_CLOSE:
                continue

            r = feats_at(f, i)
            r.update(code=code, market=market, asof=day.date(), event=event_date.date(),
                     close=float(f["close"].iloc[i]))
            day_rows[day].append(r)

    out = []

    for day in days:
        if not day_rows[day]:
            continue

        df = pd.DataFrame(day_rows[day])
        # 살아남은 세 축은 전부 낮을수록 유리하다. 값의 단위가 달라 표준화 대신 순위를 평균해 점수로 쓴다.
        df["점수"] = -sum(df[c].rank(pct=True) for c in SURVIVING) / len(SURVIVING)
        df = df.sort_values("점수", ascending=False).reset_index(drop=True)
        df["순위"] = np.arange(1, len(df) + 1)
        out.append(df)

    if not out:
        return pd.DataFrame()

    return pd.concat(out, ignore_index=True).merge(meta[["code", "name"]], on="code", how="left")


def append_csv(path, df, keys):
    """같은 키의 기존 행은 갈아끼우고 덧붙인다. 다시 돌려도 중복이 쌓이지 않는다."""
    if df.empty:
        return

    if path.exists():
        old = pd.read_csv(path, dtype={"code": str})

        for k in keys:
            if k in old.columns:
                old[k] = old[k].astype(str)
                df[k] = df[k].astype(str)

        newkeys = set(map(tuple, df[keys].values))
        mask = ~old[keys].apply(lambda r: tuple(r) in newkeys, axis=1)
        df = pd.concat([old[mask], df], ignore_index=True)

    df.to_csv(path, index=False, encoding="utf-8-sig")


def main():
    force = "--force-panel" in sys.argv
    meta = load_codes()
    idx_rows, windows = [], {}

    for market in ("KOSPI", "KOSDAQ"):
        idx = index_state(market)

        if idx is None:
            print("%s 지수 조회 실패 — 건너뛴다" % market, flush=True)
            continue

        last = idx.iloc[-1]
        ev_date, elapsed = recent_event(idx, market)
        idx_rows.append(dict(
            asof=last["date"].date(), market=market, close=float(last["close"]),
            ma20=float(last["ma20"]), hi250=float(last["hi250"]), dd=float(last["dd"]),
            임계값=THRESHOLD[market], 회복=bool(last["reclaim"]), 사건=bool(last["event"]),
            최근사건=ev_date.date() if ev_date is not None else "",
            창내경과일=elapsed if elapsed is not None else "",
        ))
        print("%-6s 종가 %.2f  낙폭 %+.1f%% (임계 %+.0f%%)  20일선회복=%s  사건=%s  창=%s" % (
            market, last["close"], last["dd"] * 100, THRESHOLD[market] * 100,
            "예" if last["reclaim"] else "아니오", "예" if last["event"] else "아니오",
            ("%d/%d일" % (elapsed, WINDOW_DAYS)) if elapsed is not None else "닫힘"), flush=True)

        if elapsed is not None:
            windows[market] = (idx, ev_date, last["date"])

    append_csv(IDX_OUT, pd.DataFrame(idx_rows), ["asof", "market"])
    print("지수 기록 → %s" % IDX_OUT, flush=True)

    if not windows and not force:
        print("열린 창 없음 — 종목 층은 건너뛴다(조회 0건).", flush=True)
        return

    panel = seed_panel()
    panel = refresh_panel(panel, meta["code"].tolist())

    for market, (idx, ev_date, asof) in windows.items():
        days = [d for d in idx["date"] if ev_date <= d <= asof]
        df = scan_signals(panel, meta, idx, market, ev_date, days)

        if df.empty:
            print("%s 창 안 20일 이동평균 회복 종목 없음" % market, flush=True)
            continue

        append_csv(SIG_OUT, df, ["asof", "code"])
        print("%s 신호 %d종 기록(창 %d일치) → %s" % (market, len(df), len(days), SIG_OUT), flush=True)
        cols = ["asof", "순위", "code", "name", "점수", "바닥후경과일", "저점대비반등", "거래량급증"]
        today = df[df["asof"] == asof.date()]
        print((today if not today.empty else df)[cols].head(15).to_string(index=False), flush=True)


if __name__ == "__main__":
    main()
