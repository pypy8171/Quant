"""KRX·NXT·통합 시세 괴리 측정 — 같은 종목을 세 시장구분으로 조회해 차이를 기록한다.

넥스트레이드(NXT)는 2025-03 출범한 대체거래소다. 이 저장소는 시세도 주문도
`FID_COND_MRKT_DIV_CODE="J"`(KRX)에 고정돼 있어, NXT에서만 형성된 가격은 보이지 않는다.
바꿀지 말지를 정하려면 먼저 "얼마나 다른가"를 재야 한다. 주문 라우팅은 건드리지 않고
조회만 세 갈래로 나눠 CSV에 쌓는다.

  J  = KRX 단독
  NX = NXT 단독
  UN = 통합(둘 중 유리한 쪽 반영)

읽는 값은 현재가·시가·고가·저가·누적거래량·누적거래대금이다. 시장구분이 지원되지
않으면 rt_cd가 0이 아니거나 현재가가 0으로 오는데, 그것도 결과로 남긴다.

실행:
  py PYQuant/tools/nxt_divergence_probe.py --minutes 30 --interval 60
  py PYQuant/tools/nxt_divergence_probe.py --tickers 005930,000660 --interval 30

기본 종목은 Quant/config/universe_scan.json 상위 N개, 없으면 삼성전자·SK하이닉스.
출력: logs/nxt_divergence_YYYYMMDD.csv (append)
"""
from __future__ import annotations

import argparse
import csv
import json
import sys
import time
from datetime import datetime
from pathlib import Path

_PYQUANT_ROOT = Path(__file__).resolve().parents[1]
if str(_PYQUANT_ROOT) not in sys.path:
    sys.path.insert(0, str(_PYQUANT_ROOT))

from kis.client import from_config  # noqa: E402

_REPO_ROOT = _PYQUANT_ROOT.parent
_UNIVERSE = _REPO_ROOT / "Quant" / "config" / "universe_scan.json"
_LOG_DIR = _REPO_ROOT / "logs"

DIVS = ("J", "NX", "UN")

_FIELDS = ("stck_prpr", "stck_oprc", "stck_hgpr", "stck_lwpr",
           "acml_vol", "acml_tr_pbmn", "prdy_ctrt")


def _to_f(v) -> float:
    try:
        return float(str(v).strip() or 0)
    except (TypeError, ValueError):
        return 0.0


def probe_one(kis, ticker: str, div: str) -> dict:
    """현재가 1건을 시장구분 div로 조회한다. 실패는 rt_cd/msg로 남긴다."""
    data = kis._get(
        "/uapi/domestic-stock/v1/quotations/inquire-price",
        {"FID_COND_MRKT_DIV_CODE": div, "FID_INPUT_ISCD": ticker},
        "FHKST01010100",
    )
    out = {"rt_cd": str(data.get("rt_cd", "")), "msg": str(data.get("msg1", "")).strip()}
    o = data.get("output") or {}
    for f in _FIELDS:
        out[f] = _to_f(o.get(f))
    return out


def default_tickers(top_n: int) -> list[str]:
    try:
        with open(_UNIVERSE, encoding="utf-8") as f:
            u = json.load(f)
        ts = [row["ticker"] for row in u.get("universe", []) if row.get("ticker")]
        if ts:
            return ts[:top_n]
    except (OSError, ValueError, KeyError):
        pass
    return ["005930", "000660"]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--tickers", default="", help="쉼표 구분. 없으면 universe_scan.json 상위 N")
    ap.add_argument("--top-n", type=int, default=5)
    ap.add_argument("--interval", type=int, default=60, help="폴링 간격(초)")
    ap.add_argument("--minutes", type=int, default=30, help="총 수집 시간(분). 0이면 1회만")
    ap.add_argument("--config", default="", help="KIS config 경로(기본 Quant/config/config.json)")
    args = ap.parse_args()

    tickers = [t.strip() for t in args.tickers.split(",") if t.strip()] or default_tickers(args.top_n)

    kis = from_config(args.config or None)
    if not kis.authenticate():
        print("[nxt_probe] 인증 실패 — config app_key/app_secret 확인")
        return 1

    _LOG_DIR.mkdir(parents=True, exist_ok=True)
    out_path = _LOG_DIR / f"nxt_divergence_{datetime.now():%Y%m%d}.csv"
    new_file = not out_path.exists()

    header = ["ts", "ticker"]
    for d in DIVS:
        header += [f"{d}_rt", f"{d}_msg"] + [f"{d}_{f}" for f in _FIELDS]
    header += ["px_gap", "px_gap_bp"]

    rounds = 1 if args.minutes <= 0 else max(1, args.minutes * 60 // max(1, args.interval))
    print(f"[nxt_probe] {len(tickers)}종목 × {len(DIVS)}구분, {args.interval}초 간격 {rounds}회 → {out_path}")

    with open(out_path, "a", newline="", encoding="utf-8-sig") as f:
        w = csv.writer(f)
        if new_file:
            w.writerow(header)
        for i in range(rounds):
            stamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
            for t in tickers:
                row = [stamp, t]
                px = {}
                for d in DIVS:
                    r = probe_one(kis, t, d)
                    px[d] = r["stck_prpr"]
                    row += [r["rt_cd"], r["msg"]] + [r[fl] for fl in _FIELDS]
                    time.sleep(0.12)   # KIS 초당 한도 여유
                gap = px["NX"] - px["J"] if px["J"] > 0 and px["NX"] > 0 else 0.0
                bp = (gap / px["J"] * 10000.0) if px["J"] > 0 and px["NX"] > 0 else 0.0
                row += [round(gap, 2), round(bp, 2)]
                w.writerow(row)
                print(f"  {stamp} {t}  J={px['J']:.0f}  NX={px['NX']:.0f}  UN={px['UN']:.0f}  "
                      f"괴리 {gap:+.0f}원 ({bp:+.1f}bp)")
            f.flush()
            if i + 1 < rounds:
                time.sleep(args.interval)

    print(f"[nxt_probe] 저장 완료 {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
