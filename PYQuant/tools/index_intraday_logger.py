"""
장중 지수 스냅샷 forward 적재 로거 (Track B 장중국면 forward PIT DB).

배경 (2026-08-25 3자회의 strategist/reviewer/data-sourcer 결론):
  Track B(장중 국면 자동 전환)의 후보 지표 g/v/p/a(시가대비 낙폭·장중 실현변동성·
  추세지속성·가속)는 전부 **장중 지수 시계열**의 파생이다. 그런데 장중 지수 PIT
  틱/분 히스토리는 KIS에도 data.go.kr에도 없다 → **백테스트 영영 불가**. 수급데이터와
  똑같이 "그 시각의 지수"는 되사올 수 없으므로, 오늘부터 직접 쌓지 않으면 Track B의
  국면전환 규칙은 forward로도 검증 궤도에 못 오른다. 이 로거가 그 유일경로다.

설계 원칙:
  * **원시 스냅샷만 적재.** price/change/change_rate/sign/ts만 남긴다. g/v/p/a·앵커·
    임계는 아직 검증 안 된 가정(회의 reviewer V5)이라 되돌릴 수 없는 로그에 굽지 않는다.
    파생지표는 price 시계열에서 오프라인으로 언제든 재계산 가능 → 규칙을 바꿔가며 ablation.
  * **captured_at 초단위.** data_thread 사이클이 불균일해 폴 간 Δt가 벌어질 수 있다
    (회의 reviewer W). 초단위 타임스탬프를 남겨 오프라인에서 Δt로 정규화/필터 가능.
  * **다중 프록시 동시 적재.** 0001(코스피)·1001(코스닥)·2001(KOSPI200) 3코드를 함께
    쌓는다(회의 data-sourcer: ITB=중소형/코스닥이면 0001과 거동 괴리 → 시장별 앵커 필요).
    합쳐 3콜/30s=6콜/분, rate limit(초5/분20) 여유 충분.
  * 토큰캐시를 C++ 엔진과 공유(kis/client.py) → 엔진과 동시 실행해도 추가 인증부담 없음.

전송: 없음. append-only JSONL. 조회 전용, 주문 없음 — 안전.

사용 (PYQuant/ 디렉토리에서):
    python -m tools.index_intraday_logger                 # 30s 주기, 장중(09:00-15:30 KST)만 적재
    python -m tools.index_intraday_logger --once          # 1회 폴 후 출력·종료(지연/값 실측·점검)
    python -m tools.index_intraday_logger --all-hours     # 장외에도 적재(테스트)
    python -m tools.index_intraday_logger --interval 30 --codes 0001 1001 2001
    python -m tools.index_intraday_logger --out ../data/index_intraday

멱등성: (ts, code)가 오늘 파일에 이미 있으면 건너뜀(재기동 안전).
"""
import argparse
import json
import os
import sys
import time
from datetime import datetime, timezone, timedelta
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))  # PYQuant/ 를 패키지 루트로

from kis.client import from_config  # noqa: E402

KST = timezone(timedelta(hours=9))

DEFAULT_CODES = ["0001", "1001", "2001"]  # 코스피종합 / 코스닥종합 / KOSPI200
_CODE_LABEL = {"0001": "코스피", "1001": "코스닥", "2001": "KOSPI200"}

# 정규 세션 (동시호가 제외한 연속매매 구간). 앞뒤로 약간 여유.
SESSION_START = (9, 0)
SESSION_END = (15, 30)


def now_kst() -> datetime:
    return datetime.now(KST)


def in_session(dt: datetime) -> bool:
    """평일 09:00-15:30 KST 여부. 주말·장외면 False."""
    if dt.weekday() >= 5:  # 5=토 6=일
        return False
    hm = (dt.hour, dt.minute)
    return SESSION_START <= hm <= SESSION_END


def day_file(out_dir: Path, dt: datetime) -> Path:
    return out_dir / f"index_{dt.strftime('%Y-%m-%d')}.jsonl"


def load_existing_keys(path: Path) -> set:
    """오늘 파일의 (ts, code) 집합 — 멱등 재기동용."""
    keys = set()
    if not path.exists():
        return keys
    try:
        with open(path, encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    rec = json.loads(line)
                    keys.add((rec.get("ts", ""), rec.get("code", "")))
                except json.JSONDecodeError:
                    continue
    except OSError:
        pass
    return keys


def poll_once(kis, codes) -> list[dict]:
    """각 코드 1콜씩 폴 → 레코드 리스트(응답 실패/price<=0 코드는 제외)."""
    dt = now_kst()
    ts = dt.isoformat(timespec="seconds")
    date = dt.strftime("%Y-%m-%d")
    recs = []
    for code in codes:
        try:
            ip = kis.get_index_price(code)
        except Exception as e:  # noqa: BLE001 — 한 코드 실패가 나머지를 멈추면 안 됨
            print(f"  [{code}] 폴 실패: {type(e).__name__}: {e}", file=sys.stderr)
            continue
        if not ip.ok:
            print(f"  [{code}] 빈/이상 응답(price={ip.price}) — 건너뜀", file=sys.stderr)
            continue
        recs.append({
            "date":        date,
            "ts":          ts,                 # 초단위 KST — Δt 정규화용
            "code":        code,
            "price":       ip.price,           # bstp_nmix_prpr
            "change":      ip.change,          # bstp_nmix_prdy_vrss (전일대비)
            "change_rate": ip.change_rate,     # bstp_nmix_prdy_ctrt (전일종가 대비 %)
            "sign":        ip.sign,            # prdy_vrss_sign
            "captured_at": ts,
            "source_tr":   "FHPUP02100000",
        })
    return recs


def append_recs(out_dir: Path, recs: list[dict], existing: set) -> int:
    if not recs:
        return 0
    out_dir.mkdir(parents=True, exist_ok=True)
    dt = now_kst()
    path = day_file(out_dir, dt)
    written = 0
    with open(path, "a", encoding="utf-8") as f:
        for r in recs:
            key = (r["ts"], r["code"])
            if key in existing:
                continue
            f.write(json.dumps(r, ensure_ascii=False) + "\n")
            existing.add(key)
            written += 1
        f.flush()
        os.fsync(f.fileno())
    return written


def _fmt_line(recs: list[dict]) -> str:
    return " ".join(
        f"{_CODE_LABEL.get(r['code'], r['code'])}={r['price']:.2f}({r['change_rate']:+.2f}%)"
        for r in recs
    )


def main() -> None:
    ap = argparse.ArgumentParser(description="장중 지수 스냅샷 forward 적재 로거 (Track B)")
    ap.add_argument("--codes", nargs="*", default=DEFAULT_CODES,
                    help=f"지수코드 (기본 {DEFAULT_CODES})")
    ap.add_argument("--interval", type=float, default=30.0, help="폴 주기(초, 기본 30)")
    ap.add_argument("--out", default=None,
                    help="출력 디렉토리 (기본 PYQuant/data/index_intraday)")
    ap.add_argument("--once", action="store_true", help="1회 폴 후 출력·종료(점검·지연 실측)")
    ap.add_argument("--all-hours", action="store_true", help="장외에도 적재(테스트)")
    args = ap.parse_args()

    out_dir = Path(args.out) if args.out else (Path(__file__).resolve().parents[1] / "data" / "index_intraday")

    kis = from_config()
    if not kis.authenticate():
        print("인증 실패 — config.json app_key/app_secret 확인")
        sys.exit(1)

    # ── 점검 모드: 1회 폴 후 원시값 그대로 출력(지연/값 실측) ───────────────────
    if args.once:
        recs = poll_once(kis, args.codes)
        if not recs:
            print("[!] 전 코드 빈 응답 — 장중인지·시세키 권한 확인")
            sys.exit(1)
        for r in recs:
            print(json.dumps(r, ensure_ascii=False))
        n = append_recs(out_dir, recs, load_existing_keys(day_file(out_dir, now_kst())))
        print(f"[점검] {len(recs)}코드 폴, {n}행 적재 → {day_file(out_dir, now_kst())}")
        return

    print(f"장중 지수 로거 시작 | 코드={args.codes} | 주기={args.interval}s | 출력={out_dir}")
    print("원시 스냅샷만 적재(g/v/p/a는 오프라인 파생). Ctrl+C로 중단.")

    existing = load_existing_keys(day_file(out_dir, now_kst()))
    cur_date = now_kst().strftime("%Y-%m-%d")
    off_session_notified = 0.0
    total = 0
    try:
        while True:
            dt = now_kst()
            # 날짜 롤오버 → 새 파일 키셋
            if dt.strftime("%Y-%m-%d") != cur_date:
                cur_date = dt.strftime("%Y-%m-%d")
                existing = load_existing_keys(day_file(out_dir, dt))

            if not args.all_hours and not in_session(dt):
                # 장외: 10분마다 한 줄만 찍고 대기(로그 노이즈 억제)
                if time.time() - off_session_notified > 600:
                    print(f"[{dt.strftime('%H:%M')}] 장외 대기 (세션 09:00-15:30 KST)")
                    off_session_notified = time.time()
                time.sleep(args.interval)
                continue

            try:
                recs = poll_once(kis, args.codes)
                n = append_recs(out_dir, recs, existing)
                total += n
                if recs:
                    print(f"[{dt.strftime('%H:%M:%S')}] {_fmt_line(recs)} | +{n}행(누적{total})")
            except Exception as e:  # noqa: BLE001 — 사이클 throw가 프로세스를 죽이지 않게
                print(f"[WARN] 사이클 실패: {type(e).__name__}: {e} — 다음 주기 재시도", file=sys.stderr)

            time.sleep(args.interval)
    except KeyboardInterrupt:
        print(f"\n중단 — 누적 {total}행 적재됨 ({day_file(out_dir, now_kst())})")


if __name__ == "__main__":
    main()
