#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""문서 평이화 게이트 — AI 문체·과축약 용어를 기계적으로 잡아 평이한 한국어로 되돌린다.

docs/STYLE_GUIDE.md의 "반복 적발 표현" 표를 코드로 옮긴 것이다. @committer (d-2) 문체 스캔이
이 스크립트를 게이트로 쓴다(exit 1 = 드리프트). check_docs.py(링크·색인 드리프트)와 짝이다.

무엇을 잡나:
  1) 코인드 문구·영어 jargon·과장어(정직한 실패·초과수익 음수·참패·ablation·EOD·Donchian·
     fail-fast·비교 검정·B&H·등가중·카탈로그·H7 …)를 평이한 한국어로.
  2) 백테스트 식별자 BT-NN / "백테스트 NN"을 사람이 읽는 서술형 이름으로(NAME_MAP).

무엇을 건드리지 않나(기계 계층 보호):
  - 코드 파일(.py/.cpp/.h)·데이터(.json)·빌드 산출물은 스캔 대상이 아니다.
    metrics.json의 study_id "BT-NN", strategy="BH" 같은 기계 키는 그대로 둔다.
  - 스캔 대상 .md 안에서도 인라인 코드(`...`)·코드펜스(```)·링크 URL의 (...) 부분은
    마스킹해 건드리지 않는다(식별자·파일명·앵커 보호).

사용:
  python scripts/check_plain_language.py            # 검출만(드리프트 있으면 exit 1)
  python scripts/check_plain_language.py --fix       # 안전 치환 적용 후 잔여 리포트
  python scripts/check_plain_language.py path ...     # 특정 파일/폴더만
"""
import re
import sys
from pathlib import Path

# Windows 콘솔(cp949)에서 출력(— 등) 깨짐/크래시 방지
for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass

_HERE = Path(__file__).resolve()
_REPO = _HERE.parents[1]

# ── 서술형 이름표(정본) ──────────────────────────────────────────────────────
# 기계 study_id(BT-NN)는 metrics.json·코드에 유지. 사람이 보는 문서에서만 이 이름으로.
NAME_MAP = {
    "01": "모멘텀·국면필터 롤링검증(1~5년)",
    "02": "변동성 타게팅 사이징",
    "03": "2022 약세장 국면필터 제거실험",
    "04": "월별 시작시점 스윕",
    "05": "지표 4종 전기간 검증",
    "06": "하락장 유사구간 6구간 비교",
    "07": "위기 레짐 지수레벨 특성화",
    "08": "위기 인과 대응 5법",
    "09": "위기 대응·수익추구 전략 10종",
    "10": "구조 국면 스코어러 제거실험",
    "11": "신호 3축 나란히 비교",
}

# ── 용어 치환 규칙(순서 중요: 더 구체적인 것 먼저) ─────────────────────────────
# (정규식, 치환, 사람이 읽을 설명)
RULES = [
    # 벤치마크 표기
    (r"등가중\s*Buy&Hold", "동일가중 매수 후 보유", "등가중 Buy&Hold"),
    (r"등가중\s*B&H", "동일가중 매수 후 보유", "등가중 B&H"),
    (r"Buy\s*&\s*Hold\(기준선\)", "매수 후 보유(기준선)", "Buy&Hold(기준선)"),
    (r"Buy\s*&\s*Hold", "매수 후 보유", "Buy&Hold"),
    (r"B&H", "매수 후 보유", "B&H"),
    (r"\bBH\b", "매수 후 보유", "BH(산문)"),
    # 등가중 → 동일가중 통일
    (r"등가중", "동일가중", "등가중"),
    # 코인드 문구
    (r"정직한\s*실패", "벤치 못 이김", "정직한 실패"),
    (r"초과수익\(α\)\s*음수", "벤치 못 이김(α<0)", "초과수익(α) 음수"),
    (r"초과수익\s*음수", "벤치 못 이김", "초과수익 음수"),
    (r"빠른\s*실패", "재시도 없이 즉시 실패", "빠른 실패"),
    (r"fail[\s-]?fast", "재시도 없이 즉시 실패", "fail-fast"),
    (r"비교\s*검정", "나란히 비교", "비교 검정"),
    (r"바이오프", "나란히 비교", "바이오프"),
    (r"bake[\s-]?off", "나란히 비교", "bake-off"),
    # 과장어
    (r"참패했", "크게 뒤졌", "참패했다"),
    (r"참패", "크게 뒤짐", "참패"),
    # 영어 jargon / 과축약
    (r"카탈로그", "목록", "카탈로그"),
    (r"(?<![A-Za-z])EOD(?![A-Za-z])", "장 마감", "EOD"),  # 조사 붙은 EOD가·EOD로는도 검출(치환 조사는 수기 확인)
    (r"절제실험\s*\(ablation\)", "제거실험", "절제실험(ablation)"),
    (r"애블레이션", "제거실험", "애블레이션"),
    (r"\bablation\b", "제거실험", "ablation"),
    (r"절제실험", "제거실험", "절제실험"),
    (r"채널\s*돌파\s*\(Donchian\)", "채널 돌파", "채널 돌파(Donchian)"),
    (r"Donchian", "채널 돌파", "Donchian(산문)"),
    # H7 눌림 필터 가설 코드
    (r"H7\s*눌림\s*필터", "눌림 필터", "H7 눌림 필터"),
    (r"H7\s*필터", "눌림 필터", "H7 필터"),
    (r"\bH7\b", "눌림 필터", "H7"),
]
_COMPILED = [(re.compile(pat), rep, desc) for pat, rep, desc in RULES]

# BT-NN / "백테스트 NN" 식별자 검출·치환
_ID_HEAD = re.compile(r"^(#+\s*)(?:백테스트\s*|BT-)(\d\d)R?\s*[·:]\s*")  # 헤딩 접두어
_ID_TOKEN = re.compile(r"(?:백테스트\s*|BT-)(\d\d)(?!\d)R?")  # \d\d 뒤에 숫자 오면 연도 등 → 제외

# ── 스캔 범위 ────────────────────────────────────────────────────────────────
SCAN_GLOBS = [
    "research/**/*.md", "strategies/**/*.md", "docs/**/*.md",
    "README.md", "STRATEGIES.md", "ARCHITECTURE.md", "DECISIONS.md",
    "DAILY_LOG.md", "SESSION_HUB.md",
    "research/studies/PYTHON_학습노트.md", "Quant/CPP_학습노트.md",
    # 대시보드 데이터섬(사후검토·라이브 일지 산문 필드가 그대로 렌더됨)
    "research/dashboard/reviews.json", "research/dashboard/live.json",
]
EXCLUDE_PARTS = {"_private", "out", "build", "build_win", "__pycache__",
                 ".claude", "node_modules", "raw", "archive"}
# 이 용어들을 정의·목록·규칙으로 담는 메타 파일 — 스스로를 파괴하지 않도록 제외.
EXCLUDE_FILES = {"docs/STYLE_GUIDE.md", "docs/GLOSSARY.md"}


def _iter_files(paths):
    seen = set()
    roots = paths or [_REPO]
    for root in roots:
        root = Path(root)
        if root.is_file():
            cands = [root]
        elif paths:  # 사용자가 폴더 지정
            cands = list(root.rglob("*.md"))
        else:        # 기본 스캔 범위
            cands = []
            for g in SCAN_GLOBS:
                cands.extend(_REPO.glob(g))
        for f in cands:
            if any(part in EXCLUDE_PARTS for part in f.parts):
                continue
            if f.resolve().relative_to(_REPO).as_posix() in EXCLUDE_FILES:
                continue
            rp = f.resolve()
            if rp in seen:
                continue
            seen.add(rp)
            yield f


# ── 마스킹(코드·링크 URL 보호) ────────────────────────────────────────────────
_MASK = re.compile(r"`[^`]*`|\]\([^)]*\)|\bhttps?://\S+")


def _mask(line):
    store = []

    def stash(m):
        store.append(m.group(0))
        return f"\x00{len(store) - 1}\x00"

    return _MASK.sub(stash, line), store


def _unmask(line, store):
    return re.sub(r"\x00(\d+)\x00", lambda m: store[int(m.group(1))], line)


def _apply_line(line):
    """한 줄에 규칙 적용. (새 줄, [적발설명…]) 반환. 코드펜스 밖 줄에만 호출."""
    masked, store = _mask(line)
    hits = []
    # 헤딩 접두어 "백테스트 NN ·" / "BT-NN ·" → 서술형(뒤에 이미 설명이 오므로 접두어만 제거)
    mh = _ID_HEAD.match(masked)
    if mh:
        masked = _ID_HEAD.sub(r"\1", masked)
        hits.append(f"헤딩 ID 접두어 제거(BT-{mh.group(2)})")
    # 남은 BT-NN / 백테스트 NN 토큰 → 서술형 이름
    def _name(m):
        nn = m.group(1)
        hits.append(f"BT-{nn}→{NAME_MAP.get(nn, '?')}")
        return NAME_MAP.get(nn, m.group(0))
    masked = _ID_TOKEN.sub(_name, masked)
    # 용어 규칙
    for rx, rep, desc in _COMPILED:
        if rx.search(masked):
            masked = rx.sub(rep, masked)
            hits.append(desc)
    return _unmask(masked, store), hits


def process(text):
    """전체 텍스트 처리. (새 텍스트, [(lineno, 설명)…]) 반환."""
    out, findings, in_fence = [], [], False
    for i, line in enumerate(text.splitlines(keepends=False), 1):
        if line.lstrip().startswith("```"):
            in_fence = not in_fence
            out.append(line)
            continue
        if in_fence:
            out.append(line)
            continue
        new, hits = _apply_line(line)
        out.append(new)
        for h in hits:
            findings.append((i, h))
    trailing_nl = "\n" if text.endswith("\n") else ""
    return "\n".join(out) + trailing_nl, findings


def main(argv):
    fix = "--fix" in argv
    paths = [a for a in argv if not a.startswith("--")]
    total = 0
    for f in _iter_files(paths):
        text = f.read_text(encoding="utf-8")
        new, findings = process(text)
        if not findings:
            continue
        total += len(findings)
        rel = f.resolve().relative_to(_REPO).as_posix()
        if fix and new != text:
            f.write_text(new, encoding="utf-8")
            print(f"[fixed] {rel} — {len(findings)}건")
        else:
            print(f"[hit]   {rel} — {len(findings)}건")
            for ln, h in findings[:12]:
                print(f"          L{ln}: {h}")
            if len(findings) > 12:
                print(f"          … 외 {len(findings) - 12}건")
    if fix:
        print(f"\n총 {total}건 처리. 재검증: python scripts/check_plain_language.py")
        return 0
    if total:
        print(f"\n드리프트 {total}건 — 평이화 필요(--fix로 자동 치환 후 검토). "
              f"정본: docs/STYLE_GUIDE.md")
        return 1
    print("평이화 게이트 통과 — 적발 0건.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
