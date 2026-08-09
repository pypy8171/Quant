"""
수급 EOD 확정치 forward 적재 로거 (investor-flow forward PIT DB).

배경(2026-08-09 전략회의 결론):
  외인/기관 "종목별 장중 실시간 확정 순매수"는 무료로도 유료로도 구할 수 없다
  (거래소가 장중 투자자구분을 원천 미제공 → 장중은 추정, 확정은 EOD). 그리고
  "그때 그 시각의 수급"은 과거를 되사올 수 없다 → 오늘부터 직접 쌓지 않으면
  이 데이터로 하는 검증(백테스트/ablation)은 영영 불가능하다.

  이 스크립트는 매 거래일 장 마감 후(권장 18:10 KST 이후, KRX 확정 반영) 실행되어,
  거래대금/시총 상위 유니버스의 일별 투자자 순매수(FHKST01010900)를 append-only
  JSONL로 적재한다. 확정치가 목적이지만, 최신일자 행은 잠정치일 수 있으므로 매 실행마다
  captured_at을 찍어 그대로 남긴다(잠정→확정 리비전 드리프트 자체가 연구 자료).

  ⚠️ 필드명 미확정: FHKST01010900 응답 필드명은 아직 라이브 1콜로 확정 안 됨
     (tools/probe_kis_investor.py, check_investor_api.py 참조). 그래서 이 로거는
     각 행의 raw 딕셔너리를 통째로 보존하고, 외인/기관/개인 순매수는 필드명 후보로
     best-effort 파싱만 병행한다 → 나중에 필드명이 확정되면 raw에서 재파싱 가능.

사용 (PYQuant/ 디렉토리에서):
    python -m tools.investor_flow_logger                 # 시총 상위 50 (기본)
    python -m tools.investor_flow_logger --top 30        # 상위 N
    python -m tools.investor_flow_logger --volume-rank   # 거래대금 상위(FHPST01710000)
    python -m tools.investor_flow_logger 005930 000660   # 특정 종목만
    python -m tools.investor_flow_logger --out ../data/investor_flow

멱등성: (date, ticker, captured_date) 조합이 이미 적재돼 있으면 건너뛴다.
        같은 날 여러 번 돌려도 하루 1행만 남는다(그날의 최종 관측치).
주문 없음 — 조회 전용, 안전.
"""
import argparse
import json
import sys
import time
from datetime import datetime, timezone, timedelta
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))  # PYQuant/ 를 패키지 루트로

from kis.client import from_config  # noqa: E402

KST = timezone(timedelta(hours=9))

# FHKST01010900 응답 필드명 후보 (라이브 확정 전까지 best-effort 파싱용).
# 확정되면 여기만 좁히면 된다.
_DATE_KEYS   = ("stck_bsop_date", "bsop_date", "date")
_FOREIGN_KEYS = ("frgn_ntby_qty", "frgn_ntby_tr_pbmn", "frgn_seln_vol", "foreign_ntby")
_INST_KEYS    = ("orgn_ntby_qty", "orgn_ntby_tr_pbmn", "orgn_ntby", "inst_ntby")
_INDIV_KEYS   = ("prsn_ntby_qty", "prsn_ntby_tr_pbmn", "prsn_ntby", "indiv_ntby")


def _pick(row: dict, keys) -> str:
    for k in keys:
        if k in row and row[k] not in ("", None):
            return row[k]
    return ""


def _to_int(v) -> int:
    try:
        return int(float(v))
    except (ValueError, TypeError):
        return 0


def kst_today() -> str:
    return datetime.now(KST).strftime("%Y-%m-%d")


def fetch_volume_rank(kis, top: int) -> list[str]:
    """거래대금 상위 N (FHPST01710000, FID_BLNG_CLS_CODE=3=거래금액순). best-effort."""
    data = kis._get(
        "/uapi/domestic-stock/v1/quotations/volume-rank",
        {
            "FID_COND_MRKT_DIV_CODE": "J",
            "FID_COND_SCR_DIV_CODE":  "20171",
            "FID_INPUT_ISCD":         "0000",
            "FID_DIV_CLS_CODE":       "0",
            "FID_BLNG_CLS_CODE":      "3",   # 0=평균거래량 1=거래증가율 2=평균거래회전율 3=거래금액순 4=거래금액회전율
            "FID_TRGT_CLS_CODE":      "111111111",
            "FID_TRGT_EXLS_CLS_CODE": "000000",
            "FID_INPUT_PRICE_1":      "",
            "FID_INPUT_PRICE_2":      "",
            "FID_VOL_CNT":            "",
            "FID_INPUT_DATE_1":       "",
        },
        "FHPST01710000",
    )
    rows = data.get("output", []) or data.get("output2", []) or []
    tickers: list[str] = []
    for r in rows:
        t = r.get("mksc_shrn_iscd") or r.get("stck_shrn_iscd") or r.get("code") or ""
        if t:
            tickers.append(t)
    if not tickers:
        print(f"[!] volume-rank 빈 응답 (rt_cd={data.get('rt_cd')!r} msg={data.get('msg1')!r}) "
              f"— 응답키={list(data.keys())}. 필드명 확정 필요.")
    return tickers[:top]


def fetch_investor_series(kis, ticker: str) -> list[dict]:
    """FHKST01010900 — 최근 ~30거래일 일별 투자자 순매수. output/output1/output2 다 시도."""
    data = kis._get(
        "/uapi/domestic-stock/v1/quotations/inquire-investor",
        {"FID_COND_MRKT_DIV_CODE": "J", "FID_INPUT_ISCD": ticker},
        "FHKST01010900",
    )
    if data.get("rt_cd", "0") not in ("0", ""):
        print(f"  [{ticker}] rt_cd={data.get('rt_cd')!r} msg={data.get('msg1')!r}")
    return data.get("output2") or data.get("output") or data.get("output1") or []


def load_existing_index(path: Path) -> set:
    """이미 적재된 (date, ticker) 집합 — 멱등 재실행용. 오늘 captured 행만 중복 판정."""
    idx = set()
    if not path.exists():
        return idx
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                rec = json.loads(line)
                idx.add((rec.get("date", ""), rec.get("ticker", ""), rec.get("captured_date", "")))
            except json.JSONDecodeError:
                continue
    return idx


def main() -> None:
    ap = argparse.ArgumentParser(description="수급 EOD 확정치 forward 적재 로거")
    ap.add_argument("tickers", nargs="*", help="특정 종목코드 (없으면 유니버스 자동)")
    ap.add_argument("--top", type=int, default=50, help="유니버스 상위 N (기본 50)")
    ap.add_argument("--volume-rank", action="store_true",
                    help="거래대금 상위(FHPST01710000). 미지정 시 시총 상위(fetch_universe)")
    ap.add_argument("--out", default=None, help="출력 디렉토리 (기본 PYQuant/data/investor_flow)")
    ap.add_argument("--sleep", type=float, default=0.2, help="종목당 호출 간격(초, rate 보호)")
    args = ap.parse_args()

    out_dir = Path(args.out) if args.out else (Path(__file__).resolve().parents[1] / "data" / "investor_flow")
    out_dir.mkdir(parents=True, exist_ok=True)

    kis = from_config()
    if not kis.authenticate():
        print("인증 실패 — config.json app_key/app_secret 확인")
        sys.exit(1)

    # ── 유니버스 결정 ────────────────────────────────────────────────────────
    if args.tickers:
        universe = args.tickers
        src = "명시"
    elif args.volume_rank:
        universe = fetch_volume_rank(kis, args.top)
        src = "거래대금상위"
    else:
        universe = kis.fetch_universe("J")[:args.top]
        src = "시총상위"

    if not universe:
        print("유니버스가 비었습니다 — 종목 지정 또는 API 응답 확인 필요")
        sys.exit(1)

    captured_at = datetime.now(KST).isoformat(timespec="seconds")
    captured_date = kst_today()
    month_file = out_dir / f"investor_flow_{captured_date[:7]}.jsonl"
    existing = load_existing_index(month_file)

    print(f"수급 로거 시작 | 유니버스={len(universe)}종목({src}) | 출력={month_file}")
    print(f"captured_at={captured_at}")

    appended = skipped = empty = 0
    with open(month_file, "a", encoding="utf-8") as f:
        for i, ticker in enumerate(universe, 1):
            rows = fetch_investor_series(kis, ticker)
            if not rows:
                empty += 1
                time.sleep(args.sleep)
                continue
            for row in rows:
                bsop = _pick(row, _DATE_KEYS)
                date = f"{bsop[:4]}-{bsop[4:6]}-{bsop[6:]}" if len(bsop) == 8 else bsop
                key = (date, ticker, captured_date)
                if key in existing:
                    skipped += 1
                    continue
                rec = {
                    "date":          date,           # 거래일
                    "ticker":        ticker,
                    "foreign_net":   _to_int(_pick(row, _FOREIGN_KEYS)),
                    "inst_net":      _to_int(_pick(row, _INST_KEYS)),
                    "indiv_net":     _to_int(_pick(row, _INDIV_KEYS)),
                    "captured_at":   captured_at,     # 관측 시각(잠정/확정 판별용)
                    "captured_date": captured_date,
                    "source_tr":     "FHKST01010900",
                    "raw":           row,             # 원본 통째 보존(필드명 확정 전 안전망)
                }
                f.write(json.dumps(rec, ensure_ascii=False) + "\n")
                existing.add(key)
                appended += 1
            if i % 10 == 0:
                print(f"  ...{i}/{len(universe)} 처리 (append={appended} skip={skipped})")
            time.sleep(args.sleep)

    print("─" * 50)
    print(f"완료 | append={appended}행 skip={skipped}행 empty={empty}종목")
    print(f"파일: {month_file}")
    if appended == 0 and empty == len(universe):
        print("[!] 전 종목 빈 응답 — FHKST01010900 필드명/권한 확인(tools/probe_kis_investor.py로 1콜 점검)")


if __name__ == "__main__":
    main()
