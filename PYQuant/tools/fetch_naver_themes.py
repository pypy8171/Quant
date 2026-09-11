"""네이버 증권 테마(인포스탁) 전체를 하루 한 번 받아 PYQuant/data/themes/에 스냅샷과 종목→테마 역색인을 남긴다. D-054

    py -X utf8 PYQuant/tools/fetch_naver_themes.py            # data/themes/YYYY-MM-DD.json + latest.json
    py -X utf8 PYQuant/tools/fetch_naver_themes.py --ticker 052710 009150   # 저장된 latest.json에서 종목의 테마를 보인다
    py -X utf8 PYQuant/tools/fetch_naver_themes.py --theme MLCC            # 이름에 MLCC가 든 테마의 구성 종목
"""
from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from naver.theme import fetch_all  # noqa: E402

OUT_DIR = Path(__file__).resolve().parents[1] / "data" / "themes"


def load_latest() -> dict:
    p = OUT_DIR / "latest.json"
    if not p.exists():
        sys.exit(f"{p} 없음 — 먼저 인자 없이 실행해 스냅샷을 만든다")
    return json.loads(p.read_text(encoding="utf-8"))


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ticker", nargs="*", help="저장된 스냅샷에서 이 종목들의 테마를 보인다")
    ap.add_argument("--theme", help="저장된 스냅샷에서 이름에 이 문자열이 든 테마의 구성 종목을 보인다")
    ap.add_argument("--out", help=f"저장 폴더(기본 {OUT_DIR})")
    a = ap.parse_args()
    sys.stdout.reconfigure(encoding="utf-8")

    if a.ticker or a.theme:
        snap = load_latest()
        print(f"[스냅샷 {snap['asof']}] 테마 {len(snap['themes'])}개")
        for tk in a.ticker or []:
            hits = snap["by_ticker"].get(tk, [])
            print(f"\n{tk}: 테마 {len(hits)}개")
            for h in hits:
                print(f"  - {h['name']}: {h['reason'][:80]}")
        if a.theme:
            for t in snap["themes"]:
                if a.theme.lower() in t["name"].lower():
                    print(f"\n{t['name']} (#{t['no']}) {t['count']}종목 당일 {t['pct']}%")
                    for m in t["members"]:
                        print(f"  {m['ticker']} {m['name']:<12} {m['change_rate']:>7}%  {m['reason'][:60]}")
        return

    out_dir = Path(a.out) if a.out else OUT_DIR
    out_dir.mkdir(parents=True, exist_ok=True)
    t0 = time.time()
    snap = fetch_all(on_progress=lambda i, n, nm: print(f"\r  {i}/{n} {nm[:20]:<20}", end="", flush=True))
    print()
    n_members = sum(len(t["members"]) for t in snap["themes"])
    day = snap["asof"][:10]
    body = json.dumps(snap, ensure_ascii=False, indent=0)
    (out_dir / f"{day}.json").write_text(body, encoding="utf-8")
    (out_dir / "latest.json").write_text(body, encoding="utf-8")
    print(f"테마 {len(snap['themes'])}개 · 편입 {n_members}건 · 종목 {len(snap['by_ticker'])}개 · {time.time()-t0:.0f}초 → {out_dir / (day + '.json')}")


if __name__ == "__main__":
    main()
