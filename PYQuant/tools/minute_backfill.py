#!/usr/bin/env python3
"""거래일별 1분봉 백필 → PYQuant/data/minute/<ticker>/<YYYYMMDD>.parquet

왜 필요한가(docs/DECISIONS.md D-004, D-009):
  DevScale은 3분봉 위에서 매매하는데 오프라인 3분봉 이력이 없어 지금까지 백테스트가 불가능했다.
  KIS TR FHKST03010230이 과거일 1분봉을 주므로(1콜 120봉, 최소 1년) 여기서 원본을 받아둔다.
  3분봉 집계는 저장하지 않는다 — 1분봉을 원본으로 두면 체결 격자(1분)와 앵커 격자(3분)를
  분리해 리플레이할 수 있고, 집계 규칙을 바꿔도 다시 받을 필요가 없다.

종목 선정은 PIT 유니버스에서 온다(pit_universe_backfill.py 선행). 날짜 D의 분봉은
D 이전에 알 수 있었던 유니버스에 대해서만 받는다. 그래야 리플레이 대상 자체가 PIT다.

호출량: 하루 1종목 = 4콜(391분 / 120봉). 40종목 × 250일 ≈ 40,000콜.
  KIS 지속 처리량이 초당 7~8콜 수준이라 90분쯤 걸린다. 이미 받은 (종목,날짜)는 건너뛴다.

주의(2026-09-07 감사): --top-n은 PIT 파일의 "저장 순서" 상위 N이다. universe_feed가
  시총순 → 거래대금순으로 이어 붙이므로 앞 100개가 전부 시총순이고, --top-n 40은 사실상
  시장별 시총 top40이다. 거래대금 축 종목은 한 개도 안 들어온다. 이 목록으로 받은 분봉만으로
  리플레이하면 결과가 생존 편향으로 무효다 — 오늘 상위인 종목은 그동안 오른 종목이기 때문이다.
  검증용으로는 --pairs(날짜별 대상 종목을 밖에서 지정)나 --top-n 0(그날 PIT 풀 전체)을 쓴다.

사용:
  py PYQuant/tools/minute_backfill.py --start 2025-09-01 --end 2026-09-04 --top-n 40
  py PYQuant/tools/minute_backfill.py --start ... --end ... --top-n 0        # 그날 PIT 풀 전체
  py PYQuant/tools/minute_backfill.py --start ... --end ... --pairs pairs.jsonl
  py PYQuant/tools/minute_backfill.py --start ... --end ... --tickers 005930,000660

--pairs 파일 형식(둘 다 허용):
  JSONL : {"ymd": "20260401", "tickers": ["000660", "005930"]}   (한 줄에 하루)
  JSON  : {"20260401": ["000660", "005930"], ...}
  --start/--end 범위 밖의 날짜는 무시한다. PIT 파일이 없는 날짜도 그대로 처리한다
  (대상 종목을 밖에서 정했으므로 PIT 절단에 다시 걸릴 이유가 없다).
"""
import argparse
import json
import sys
import time
from pathlib import Path

_PYQUANT_ROOT = Path(__file__).resolve().parents[1]
if str(_PYQUANT_ROOT) not in sys.path:
    sys.path.insert(0, str(_PYQUANT_ROOT))

from kis.client import from_config   # noqa: E402

_PIT_DIR = _PYQUANT_ROOT / "data" / "pit_universe"
_OUT_DIR = _PYQUANT_ROOT / "data" / "minute"

# 정규장 09:00~15:30은 391분이다. 이보다 한참 적으면 페이징이 잘린 것으로 본다.
#  거래정지·신규상장·반차 같은 정당한 사유로도 짧을 수 있어 재시도 횟수에 상한을 둔다.
_MIN_BARS = 300
_MAX_ATTEMPTS = 2


def _short_marker(dst: Path) -> Path:
    """짧게 받힌 (종목,날짜)의 시도 횟수 기록. 재개 시 '완결'과 '재시도 소진'을 구분한다."""
    return dst.with_suffix(".short.json")


def _already_done(dst: Path) -> bool:
    """이미 받아둔 것으로 볼지. 짧은 파일은 재시도 상한을 채우기 전까지 다시 받는다.

    파일 존재만으로 건너뛰면 잘린 하루가 조용히 남는다(09-07에 000660 08-24가 240봉으로
    남아 있었다). 마커를 읽는 비용은 파일 하나라 재개 스캔에 부담이 없다.
    """
    if not dst.exists():
        return False
    mk = _short_marker(dst)
    if not mk.exists():
        return True
    try:
        return int(json.loads(mk.read_text(encoding="utf-8")).get("attempts", 0)) >= _MAX_ATTEMPTS
    except (ValueError, OSError):
        return True


def pit_tickers(ymd: str, top_n: int) -> list[str]:
    """날짜 D의 PIT 유니버스 저장 순서 상위 top_n. top_n<=0이면 그날 풀 전체. 없으면 빈 리스트.

    저장 순서는 universe_feed의 `by_cap + by_val`이라 앞쪽이 전부 시총순이다. 따라서 작은
    top_n은 "거래대금 상위"가 아니라 "시총 상위"를 뜻한다(모듈 독스트링의 편향 주의 참고).
    라이브 스캐너는 이 파일을 자르지 않고 전량 후보로 넣으므로, 리플레이 상위집합을 원하면
    top_n=0을 쓴다.
    """
    p = _PIT_DIR / f"{ymd}.json"
    if not p.exists():
        return []
    doc = json.loads(p.read_text(encoding="utf-8"))
    pool = [u["ticker"] for u in doc.get("universe", [])]
    return pool if top_n <= 0 else pool[:top_n]


def load_pairs(path: Path, lo: str, hi: str) -> dict[str, list[str]]:
    """--pairs 파일 → {ymd: [ticker…]}. lo~hi 범위 밖 날짜는 버린다.

    JSONL(한 줄 = 하루)과 단일 JSON 객체를 모두 받는다. 같은 날짜가 여러 번 나오면 합집합을
    취하되 등장 순서를 유지한다(호출 순서가 재현되게).
    """
    text = path.read_text(encoding="utf-8").strip()
    raw: dict[str, list[str]] = {}
    try:                                  # 단일 JSON 객체 형식이면 여기서 끝난다
        obj = json.loads(text)
        if isinstance(obj, dict) and "ymd" not in obj:
            raw = {str(k): list(v) for k, v in obj.items()}
    except ValueError:
        pass
    if not raw:                           # JSONL 형식
        for line in text.splitlines():
            line = line.strip()
            if not line:
                continue
            rec = json.loads(line)
            raw.setdefault(str(rec["ymd"]), []).extend(rec.get("tickers", []))
    out: dict[str, list[str]] = {}
    for ymd, tks in raw.items():
        ymd = str(ymd).replace("-", "")
        if not (lo <= ymd <= hi):
            continue
        seen, keep = set(), []
        for t in tks:
            t = str(t).zfill(6)
            if t not in seen:
                seen.add(t)
                keep.append(t)
        if keep:
            out[ymd] = keep
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description="거래일별 1분봉 백필(TR FHKST03010230)")
    ap.add_argument("--start", required=True, help="시작일 YYYY-MM-DD (포함)")
    ap.add_argument("--end",   required=True, help="종료일 YYYY-MM-DD (포함)")
    ap.add_argument("--top-n", type=int, default=40,
                    help="날짜별 PIT 유니버스 저장순서 상위 N종목(기본 40). 0이면 그날 풀 전체. "
                         "저장순서는 시총순이 앞이라 작은 N은 시총 상위를 뜻한다.")
    ap.add_argument("--tickers", default=None,
                    help="PIT 유니버스 대신 고정 종목 목록(쉼표 구분). 기동 점검용.")
    ap.add_argument("--pairs", default=None,
                    help="(종목,날짜) 쌍 파일(JSONL 또는 JSON). 날짜별 대상 종목을 밖에서 "
                         "정한다 — PIT 절단의 선정 편향을 피하려면 이 모드를 쓴다.")
    ap.add_argument("--config", default="Quant/config/config_dev_paper.json")
    ap.add_argument("--out-dir", default=str(_OUT_DIR))
    ap.add_argument("--sleep", type=float, default=0.10,
                    help="종목 간 대기(초). 페이지 간 대기는 클라이언트가 따로 건다.")
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()

    lo, hi = args.start.replace("-", ""), args.end.replace("-", "")
    pairs = load_pairs(Path(args.pairs), lo, hi) if args.pairs else None
    if pairs is not None:
        days = sorted(pairs)
        total = sum(len(v) for v in pairs.values())
        print(f"[minute_backfill] --pairs {args.pairs}: {len(days)}일 · (종목,날짜) {total:,}쌍")
        if not days:
            print(f"[minute_backfill] {lo}~{hi} 범위에 해당하는 쌍이 없다.")
            return 1
    else:
        days = sorted(p.stem for p in _PIT_DIR.glob("*.json") if lo <= p.stem <= hi)
    if not days:
        print(f"[minute_backfill] {_PIT_DIR}에 {lo}~{hi} 범위 PIT 유니버스가 없다. "
              f"pit_universe_backfill.py를 먼저 돌린다.")
        return 1

    try:
        import pandas as pd
    except ImportError:
        print("[minute_backfill] pandas 필요")
        return 1

    c = from_config(args.config)
    if not c.authenticate():
        print("[minute_backfill] KIS 인증 실패")
        return 1

    out_dir = Path(args.out_dir)
    fixed = args.tickers.split(",") if args.tickers else None
    got = skipped = empty = short = 0
    t0 = time.time()

    for ymd in days:
        if pairs is not None:
            tickers = pairs[ymd]
        elif fixed:
            tickers = fixed
        else:
            tickers = pit_tickers(ymd, args.top_n)
        for t in tickers:
            dst = out_dir / t / f"{ymd}.parquet"
            if _already_done(dst) and not args.force:
                skipped += 1
                continue
            rows = c.get_past_minute_ohlcv(t, ymd, count=400)
            if not rows:
                empty += 1
                continue
            dst.parent.mkdir(parents=True, exist_ok=True)
            pd.DataFrame(rows).to_parquet(dst, index=False)
            got += 1
            mk = _short_marker(dst)
            if len(rows) < _MIN_BARS:
                # 짧다. 시도 횟수를 올려 두면 다음 재개 때 한 번 더 받아 본다. 상한에 닿으면
                #  정당하게 짧은 날(거래정지·신규상장)로 보고 그대로 둔다.
                prev = 0
                if mk.exists():
                    try:
                        prev = int(json.loads(mk.read_text(encoding="utf-8")).get("attempts", 0))
                    except (ValueError, OSError):
                        prev = 0
                mk.write_text(json.dumps({"attempts": prev + 1, "bars": len(rows),
                                          "first": rows[0]["hms"]}), encoding="utf-8")
                short += 1
            elif mk.exists():
                mk.unlink()          # 재시도로 채워졌다
            time.sleep(args.sleep)
        el = time.time() - t0
        print(f"  {ymd} 완료  누적 받음={got} 건너뜀={skipped} 빈응답={empty} "
              f"짧음={short}  {el/60:.1f}분", flush=True)

    print(f"[minute_backfill] 받음 {got}  건너뜀 {skipped}  빈응답 {empty}  "
          f"짧음(<300봉) {short}  → {out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
