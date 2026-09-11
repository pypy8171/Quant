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
  - 빌드 산출물·metrics.json 같은 기계 데이터는 스캔하지 않는다.
    study_id "BT-NN", strategy="BH" 처럼 코드가 읽는 키는 그대로 둔다.
  - 코드 파일(.h/.cpp/.py/.ps1)는 스캔하되 주석·로그 문구만 바뀌고,
    식별자는 인라인 코드 마스킹과 규칙 범위로 보호한다.
  - 코드의 문자열 리터럴 중 기계가 읽는 자리는 치환하지 않는다(_protected_literal):
    정규식 인자(re.compile(...) 등)·원시 문자열 r"..."·==/in 비교 우변·dict 키·
    첨자 키(x["k"]·.get("k"))·키워드 인자(strategy=·study_id=·key=·benchmark=·id=)·
    치환표의 원문 쪽(("옛말", "새말"))·"BT-NN" 하나로 된 리터럴.
    이 자리를 바꾸면 과거 로그·metrics.json과 어긋나 파서가 조용히 0을 낸다(2026-09-08).
    --fix에서는 그런 자리를 [keep]으로 알리기만 한다.
  - 스캔 대상 .md 안에서도 인라인 코드(`...`)·코드펜스(```)·링크 URL의 (...) 부분은
    마스킹해 건드리지 않는다(식별자·파일명·줄번호 참조 보호).

사용:
  python scripts/check_plain_language.py            # 검출만(드리프트 있으면 exit 1)
  python scripts/check_plain_language.py --fix       # 안전 치환 적용 후 잔여 리포트
  python scripts/check_plain_language.py path ...     # 특정 파일/폴더만
  … | python scripts/check_plain_language.py --stdin  # 표준입력 본문만 검사(쓰기 직전 훅)
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
    # ── 굳은 외래어 (2026-09-09) ────────────────────────────────────
    # 약어는 여기서 잡지 않는다. 정본(docs/STYLE_GUIDE.md 17행)이 약어를 금지어가 아니라
    #  "처음 등장 시 쉬운말(약어) 병기, 반복 등장은 약어만"으로 규정하기 때문이다.
    #  정규식은 첫 등장과 반복을 못 가르므로 매 건을 잡게 되고(2026-09-11 측정: PIT 51건 중
    #  대부분이 반복 등장), 그러면 게이트가 정본과 반대로 판정한다. 병기 표준어는
    #  docs/GLOSSARY.md가 소유한다.
    (r"사이드카", "보조 프로세스", "사이드카"),
    # ── 프로젝트 안에서만 통하던 말 → 밖에서도 읽히는 말 (2026-09-08) ──────────
    # 은유·별명으로 굳은 이름들이다. 처음 보는 사람이 뜻을 짐작할 수 없으면 쓰지 않는다.
    (r"고아\s*체결", "미연결 체결", "고아 체결"),
    (r"고아\s*주문", "미연결 주문", "고아 주문"),
    (r"고아\s*트레이더", "남은 트레이더", "고아 트레이더"),
    (r"고아\s*프로세스", "남은 프로세스", "고아 프로세스"),
    (r"고아", "미연결", "고아"),
    (r"스모크스?\s*프로브", "기동 점검", "스모크 프로브"),
    (r"기동\s*프로브", "기동 점검", "기동 프로브"),
    (r"프로브\s*주문", "기동 점검 주문", "프로브 주문"),
    # 영어 철자도 같은 말이다(2026-09-11, 대화·주석에서 되풀이돼 게이트에 올림). 한글 조사가 붙어도 잡히게
    #  \b 대신 영문자·밑줄 부정 탐색을 쓴다. 식별자(probe_*, $probe, w.probe, probe = …)는 잡지 않는다.
    (r"(?<!:\s)(?<![A-Za-z_$.\"'])probe(?![A-Za-z_;\"']|\s*\+?=)", "점검", "probe"),
    (r"프로브\s*체결", "기동 점검 체결", "프로브 체결"),
    (r"프로브", "기동 점검", "프로브"),
    (r"사다리\s*발주", "분할 매수 발주", "사다리 발주"),
    (r"사다리\s*주문", "분할 매수 주문", "사다리 주문"),
    (r"사다리\s*진입", "분할 매수 진입", "사다리 진입"),
    (r"사다리\s*단계", "분할 매수 단계", "사다리 단계"),
    (r"사다리", "분할 매수", "사다리"),
    (r"청산\s*가디언", "청산 관리", "청산 가디언"),
    (r"가디언", "청산 관리", "가디언"),
    (r"잔고\s*리컨사일", "잔고 대조", "잔고 리컨사일"),
    (r"리컨사일", "잔고 대조", "리컨사일"),
    (r"파일\s*브리지", "파일 전달", "파일브리지"),
    (r"백프레셔", "밀림 처리", "백프레셔"),
    (r"페이싱", "호출 간격 조절", "페이싱"),
    (r"깔때기", "단계별 통과율", "깔때기"),
    (r"핫패스", "지연에 민감한 경로", "핫패스"),
    (r"반사실", "가정 비교", "반사실"),
    (r"껍데기", "빈 창", "껍데기"),
    (r"잔재", "남은 프로세스", "잔재"),
    (r"굶었", "밀렸", "굶었다"),
    (r"굶는", "밀리는", "굶는"),
    (r"두들긴다", "반복 호출한다", "두들긴다"),
    (r"두들기", "반복 호출하", "두들기"),
    # H7 눌림 필터 가설 코드
    (r"H7\s*눌림\s*필터", "눌림 필터", "H7 눌림 필터"),
    (r"H7\s*필터", "눌림 필터", "H7 필터"),
    (r"\bH7\b", "눌림 필터", "H7"),
]
# 코드 파일(.h/.cpp/.py/.ps1)에서는 빼는 규칙.
# 주석이 바로 옆 식별자(enum EOD, DonchianBreakoutStrategy)를 부르는 경우라
# 여기서 바꾸면 주석과 코드가 서로 다른 이름을 말하게 된다.
CODE_SKIP_DESC = {"EOD", "Donchian(산문)", "BH(산문)", "B&H", "Buy&Hold",
                  "ablation", "fail-fast", "H7",
                  "mcap", "mktcap", "tv20", "lo60", "vol20", "MAE", "PIT", "IC"}

_COMPILED = [(re.compile(pat), rep, desc) for pat, rep, desc in RULES]

# BT-NN / "백테스트 NN" 식별자 검출·치환
_ID_HEAD = re.compile(r"^(#+\s*)(?:백테스트\s*|BT-)(\d\d)R?\s*[·:]\s*")  # 헤딩 접두어
_ID_TOKEN = re.compile(r"(?:백테스트\s*|BT-)(\d\d)(?!\d)R?")  # \d\d 뒤에 숫자 오면 연도 등 → 제외

# ── 스캔 범위 ────────────────────────────────────────────────────────────────
SCAN_GLOBS = [
    "research/**/*.md", "strategies/**/*.md", "docs/**/*.md",
    "README.md", "STRATEGIES.md", "ARCHITECTURE.md", "DECISIONS.md",
    "DAILY_LOG.md", "SESSION_HUB.md",
    # 매 세션 컨텍스트에 통째로 들어가는 파일. 여기 말투가 그날 내 말투가 된다.
    "CLAUDE.md", ".claude/*.md",
    "research/**/*.py",
    "research/studies/PYTHON_학습노트.md", "Quant/CPP_학습노트.md",
    # 대시보드 데이터섬(사후검토·라이브 일지 산문 필드가 그대로 렌더됨)
    "research/dashboard/reviews.json", "research/dashboard/live.json",
    # 코드 주석과 로그 문구 — 화면과 로그에 그대로 나오므로 문서와 같은 규칙을 받는다.
    "Quant/include/**/*.h", "Quant/src/**/*.cpp", "Quant/tests/**/*.cpp",
    "scripts/**/*.py", "scripts/**/*.ps1", "PYQuant/**/*.py",
    # 로컬 자동화 설정(커밋 대상은 아니지만 여기 문구가 다음 작업의 말투를 정한다)
    ".claude/commands/**/*.md", ".claude/agents/**/*.md", ".claude/skills/**/*.md",
]
EXCLUDE_PARTS = {"_private", "out", "build", "build_win", "__pycache__",
                 "node_modules", "raw", "archive", "hooks",
                 # 가상환경·외부 패키지는 우리 글이 아니다. 건드리면 라이브러리가 깨진다.
                 ".venv", ".venv-win", "venv", "site-packages", ".git"}
# 이 용어들을 정의·목록·규칙으로 담는 메타 파일 — 스스로를 파괴하지 않도록 제외.
EXCLUDE_FILES = {"docs/STYLE_GUIDE.md", "docs/GLOSSARY.md",
                 "scripts/check_plain_language.py"}


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


# ── 조사 보정 ──────────────────────────────────────────
# 단어를 바꾸면 받침이 바뀌어 뒤 조사가 어긋난다("가디언이" → "청산 관리이").
# 치환한 구절의 끝 글자 받침을 보고 바로 뒤 조사를 맞춘다.
_JOSA_PAIRS = [("이", "가"), ("은", "는"), ("을", "를"),
               ("과", "와"), ("으로", "로")]
# "이다/이라/이며..."는 조사가 아니라 서술격이다. 뒤에 이 글자들이 오면 건드리지 않는다.
_NOT_JOSA_NEXT = "다라며고지야나면님는은이"


def _has_batchim(ch):
    return "가" <= ch <= "힣" and (ord(ch) - 0xAC00) % 28 != 0


def _fix_josa(text, phrase):
    """phrase 바로 뒤에 붙은 조사를 phrase 끝 받침에 맞춘다."""
    tail = phrase.rstrip()[-1:]
    if not tail or not ("가" <= tail <= "힣"):
        return text
    bat = _has_batchim(tail)
    out, i = [], 0
    while True:
        j = text.find(phrase, i)
        if j < 0:
            out.append(text[i:])
            break
        end = j + len(phrase)
        out.append(text[i:end])
        rest = text[end:]
        for withb, without in _JOSA_PAIRS:
            cur, want = (without, withb) if bat else (withb, without)
            if rest.startswith(cur):
                nxt = rest[len(cur):len(cur) + 1]
                if cur in ("이", "가") and nxt and nxt in _NOT_JOSA_NEXT:
                    break
                rest = want + rest[len(cur):]
                break
        text = text[:end] + rest
        i = end
    return "".join(out) if i == 0 else text


def _detect_only(seg):
    """치환하지 않고 적발만 한다. 인라인 코드와 코드블록 안을 볼 때 쓴다.

    그 안은 명령·식별자·로그 원문이라 바꾸면 안 되지만, 금지어가 들어 있는지는
    봐야 한다. 마스킹이 치환과 검출을 같이 막던 것이 백틱 우회의 원인이었다.
    """
    hits = []
    for rx, rep, desc in _COMPILED:
        # CODE_SKIP_DESC 면제는 파일 종류와 무관하게 여기서도 그대로 둔다. 이 집합은
        #  "그 토큰이 곧 기계 이름"인 항목만 담는다(docs/eod 경로, Donchian 클래스명,
        #  ablation 함수명). 백틱 안은 그 이름을 있는 그대로 인용하는 자리라 적발하면
        #  전부 오탐이고, 피하려면 lexicon-ok를 문서마다 흩뿌려야 한다. 그 밖의 규칙은
        #  여기서도 잡는다 — 로그 원문을 백틱으로 인용하며 금지어가 새는 자리다.
        if desc in CODE_SKIP_DESC:
            continue
        if rx.search(seg):
            hits.append(desc)
    return hits


def _apply_line(line, code=False, deep=True):
    """한 줄에 규칙 적용. (새 줄, [적발설명…]) 반환. 코드펜스 밖 줄에만 호출."""
    masked, store = _mask(line)
    hits = []
    # BT-NN은 산문에서만 편다. 코드 파일에서는 그 문자열이 study_id·metrics 키·판정
    #  라벨이라(research/studies/*.py) 이름으로 펴면 조회가 조용히 빈 값을 낸다.
    #  CODE_SKIP_DESC는 용어 규칙에만 걸려 있어 이 치환까지는 막지 못했다.
    if not code:
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
        if code and desc in CODE_SKIP_DESC:
            continue
        if rx.search(masked):
            masked = rx.sub(rep, masked)
            masked = _fix_josa(masked, rep)
            hits.append(desc)
    # 마스킹해 둔 인라인 코드는 치환하지 않되 적발은 한다. 로그 원문을 백틱으로
    #  인용하면서 금지어가 새는 자리다 — 그 로그 문구 자체가 규약 적용 범위다.
    if deep:
        for s in store:
            if s.startswith("`"):
                hits.extend(d + " (인라인 코드 안)" for d in _detect_only(s))
    return _unmask(masked, store), hits


# ── 코드 파일 보호 ──────────────────────────────────────────────────────────
# .h/.cpp/.py/.ps1은 주석과 문자열만 사람이 읽는 글이다. 그 밖을 치환하면
# 식별자가 바뀌어 빌드가 깨진다(2026-09-08에 enum 값 하나가 그렇게 깨졌다).
CODE_EXT = {".h", ".hpp", ".cpp", ".cc", ".py", ".ps1"}
_TRI_D = chr(34) * 3
_TRI_S = chr(39) * 3
_BSL = chr(92)


def _prose_spans(line, in_doc):
    """코드 한 줄에서 산문으로 볼 구간 [(start, end)…]과 다음 줄 상태."""
    if in_doc:
        for t in (_TRI_D, _TRI_S):
            k = line.find(t)
            if k >= 0:
                return [(0, k)], False
        return [(0, len(line))], True

    spans, i, n = [], 0, len(line)
    while i < n:
        c = line[i]
        if line.startswith(_TRI_D, i) or line.startswith(_TRI_S, i):
            t = line[i:i + 3]
            end = line.find(t, i + 3)
            if end < 0:
                spans.append((i + 3, n))
                return spans, True
            spans.append((i + 3, end))
            i = end + 3
            continue
        if c == chr(34) or c == chr(39):
            q, j = c, i + 1
            while j < n:
                if line[j] == _BSL:
                    j += 2
                    continue
                if line[j] == q:
                    break
                j += 1
            spans.append((i + 1, min(j, n)))
            i = min(j, n) + 1
            continue
        if line.startswith('//', i) or c == '#':
            spans.append((i, n))
            break
        if line.startswith('/*', i):
            end = line.find('*/', i + 2)
            spans.append((i + 2, end if end >= 0 else n))
            i = (end + 2) if end >= 0 else n
            continue
        if c == '*' and not line[:i].strip():
            spans.append((i, n))
            break
        i += 1
    return spans, False


# ── 기계가 읽는 문자열 리터럴 ────────────────────────────────────────────────
# 여는 따옴표 앞 문맥(before)과 닫는 따옴표 뒤 문맥(after)으로 판별한다.
_PROTECT_BEFORE = [
    (re.compile(r"\bre\.(?:compile|search|match|fullmatch|findall|finditer|sub|subn|split)\(\s*[rRbB]{0,2}$"), "정규식 인자"),
    (re.compile(r"(?<![\w\"'])[rR][bB]?$"), "원시 문자열"),
    (re.compile(r"(?:==|!=|\bnot\s+in|\bin)\s*[rRbB]{0,2}$"), "비교 우변"),
    (re.compile(r"(?:\[|\.get\(|\.pop\(|\.setdefault\(|\.startswith\(|\.endswith\()\s*[rRbB]{0,2}$"), "첨자 키"),
    (re.compile(r"\b(?:strategy|study_id|key|benchmark|id)\s*=\s*[rRbB]{0,2}$"), "키워드 인자"),
]
_PROTECT_AFTER = [
    (re.compile(r"^\s*:(?!:)"), "dict 키"),
]
_PAIR_OPEN = re.compile(r"[(\[]\s*[rRbB]{0,2}$")
_PAIR_NEXT = re.compile(r"^\s*,\s*[rRbB]{0,2}[\"']")
_ID_ONLY = re.compile(r"BT-\d\d")


def _protected_literal(line, a, b):
    """line[a:b]가 기계가 읽는 문자열 리터럴이면 그 이유를, 아니면 None을 돌려준다."""
    if a < 1 or b >= len(line) or line[a - 1] not in "\"'" or line[b] != line[a - 1]:
        return None
    before, after = line[:a - 1], line[b + 1:]
    if _ID_ONLY.fullmatch(line[a:b]):
        return "식별자 리터럴"
    for rx, why in _PROTECT_BEFORE:
        if rx.search(before):
            return why
    for rx, why in _PROTECT_AFTER:
        if rx.match(after):
            return why
    if _PAIR_OPEN.search(before) and _PAIR_NEXT.match(after):
        return "치환표 원문"
    return None


def process(text, code=False, deep=True):
    """전체 텍스트 처리. (새 텍스트, [(lineno, 설명)…], [(lineno, 보호 사유)…]) 반환.

    셋째 항목은 코드 모드에서 규칙에 걸렸지만 기계 키라 치환하지 않은 자리다.
    """
    out, findings, kept, in_fence, in_doc = [], [], [], False, False
    fence_ok = False
    for i, line in enumerate(text.splitlines(keepends=False), 1):
        if not code and line.lstrip().startswith("```"):
            if not in_fence:
                # 여는 줄에 lexicon-ok를 달면 그 블록만 검사에서 뺀다.
                #  금지어를 예시로 보여줘야 하는 블록(치환표·회피 사례)이 대상이다.
                fence_ok = "lexicon-ok" in line
            in_fence = not in_fence
            out.append(line)
            continue
        if in_fence:
            out.append(line)
            if deep and not fence_ok:
                findings.extend((i, d + " (코드블록 안)") for d in _detect_only(line))
            continue
        # 줄 단위 예외. 금지어 자체를 적어야 하는 줄에 쓴다(치환표, 금지어를 잡는
        #  grep 패턴, 판정 라벨). 파일 전체를 여는 lexicon-ok보다 범위가 좁다.
        if "lexicon-ok" in line:
            out.append(line)
            continue
        if code:
            spans, in_doc = _prose_spans(line, in_doc)
            buf, last, hits = [], 0, []
            for a, b in spans:
                if a >= b:
                    continue
                buf.append(line[last:a])
                why = _protected_literal(line, a, b)
                if why:
                    _, h = _apply_line(line[a:b], code=True)
                    if h:
                        kept.append((i, why + " — " + ", ".join(h)))
                    buf.append(line[a:b])
                    last = b
                    continue
                seg, h = _apply_line(line[a:b], code=True)
                buf.append(seg)
                hits.extend(h)
                last = b
            buf.append(line[last:])
            new = "".join(buf)
        else:
            new, hits = _apply_line(line, deep=deep)
        out.append(new)
        for h in hits:
            findings.append((i, h))
    trailing_nl = "\n" if text.endswith("\n") else ""
    return "\n".join(out) + trailing_nl, findings, kept


def _run_stdin(code=False, deep=True):
    """파일로 쓰기 전의 본문을 그대로 받아 검사한다(쓰기 시점 훅용)."""
    data = sys.stdin.buffer.read().decode("utf-8", "replace")
    _, findings, _ = process(data, code=code, deep=deep)
    if not findings:
        return 0
    for ln, h in findings[:20]:
        print(f"L{ln}: {h}")
    if len(findings) > 20:
        print(f"… 외 {len(findings) - 20}건")
    return 1


def main(argv):
    # --loose: 인라인 코드·코드블록 안을 보지 않는다. 채팅 출력 검사용이다 —
    #  로그 원문을 백틱으로 인용하는 건 증거 제시라 막으면 안 된다.
    if "--stdin" in argv:
        return _run_stdin("--code" in argv, deep="--loose" not in argv)
    fix = "--fix" in argv
    paths = [a for a in argv if not a.startswith("--")]
    total = 0
    manual = 0  # 보호 구역 적발 — 치환하면 명령·식별자가 깨지므로 사람이 고친다
    for f in _iter_files(paths):
        text = f.read_text(encoding="utf-8")
        new, findings, kept = process(text, code=f.suffix.lower() in CODE_EXT)
        rel = f.resolve().relative_to(_REPO).as_posix()
        if fix:
            for ln, why in kept:
                print(f"[keep]  {rel} L{ln}: {why} — 기계 키라 치환하지 않음")
        if not findings:
            continue
        total += len(findings)
        manual += sum(1 for _, h in findings if h.endswith(" 안)"))
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

        if manual:
            print(f"이 중 {manual}건은 인라인 코드·코드블록 안이라 그대로 남았다. "
                  f"직접 고치거나, 금지어를 보여줘야 하는 블록이면 여는 줄에 lexicon-ok를 단다.")

        return 0
    if total:
        print(f"\n드리프트 {total}건 — 평이화 필요(--fix로 자동 치환 후 검토). "
              f"정본: docs/STYLE_GUIDE.md")
        if manual:
            print(f"이 중 {manual}건은 인라인 코드·코드블록 안이라 --fix가 바꾸지 않는다. "
                  f"직접 고치거나, 금지어를 보여줘야 하는 블록이면 여는 줄에 lexicon-ok를 단다.")
        return 1
    print("평이화 게이트 통과 — 적발 0건.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
