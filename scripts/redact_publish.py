"""발행물 가림 규칙 한 곳 — 대시보드 생성기가 쓰기 직전에 scrub()을 부르고, 발행 직전 훅이 find_leaks()로 다시 본다.

가리는 것: 계좌번호(전체·일부만 가린 꼴·CANO 값·'계좌' 옆 8자리), 비공개 폴더 경로.
찾기만 하는 것(find_leaks): 위에 더해 설정의 HTS ID·키 값, gate_words·묻기 목록 낱말, 개인정보 꼴, 사용자 폴더 이름, 타 프로젝트 낱말.

[inv] 실제 값(계좌번호·키·낱말)은 저장소에 두지 않는다 — 실행할 때 설정 파일·기준손익 파일 이름·_private/gate_words.txt 에서 읽는다.
"""
import json
import re
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# 기준손익 파일 이름 pnl_baseline_<날짜>_<계좌>.txt — 설정에서 빠진 옛 계좌번호도 여기 남아 있다.
_BASELINE_ACCOUNT = re.compile(r"pnl_baseline_\d{8}_(\d{8})\.txt$")
_SECRET_KEYS = ("app_key", "appkey", "app_secret", "appsecret", "secret", "token", "password")


def _config_values():
    """설정 파일에서 (값, 종류) — 계좌번호 앞 8자리, HTS ID, 키 값 앞 16자."""
    values = {}
    for config_path in (REPO / "Quant" / "config").glob("config*.json"):
        try:
            config = json.loads(config_path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue

        stack = [config]
        while stack:
            node = stack.pop()
            if isinstance(node, dict):
                for key, value in node.items():
                    if isinstance(value, (dict, list)):
                        stack.append(value)
                        continue

                    if not isinstance(value, str) or len(value.strip()) < 4:
                        continue

                    lowered = key.lower()
                    if lowered == "account_no":
                        values[value.strip()[:8]] = "계좌번호"

                    elif lowered == "hts_id":
                        values[value.strip()] = "HTS ID"

                    elif lowered in _SECRET_KEYS:
                        values[value.strip()[:16]] = "설정 비밀값"

            elif isinstance(node, list):
                stack.extend(node)

    return values


def _baseline_accounts():
    accounts = set()
    for logs_directory in (REPO / "Quant" / "build_win").glob("logs*"):
        for path in logs_directory.glob("pnl_baseline_*.txt"):
            match = _BASELINE_ACCOUNT.search(path.name)
            if match:
                accounts.add(match.group(1))

    return accounts


def _private_terms(file_name):
    """_private/<file_name> 의 낱말 — 한 줄에 하나, # 줄은 주석."""
    try:
        lines = (REPO / "_private" / file_name).read_text(encoding="utf-8").splitlines()
    except OSError:
        return []

    return [line.strip() for line in lines if len(line.strip()) >= 2 and not line.startswith("#")]


_CONFIG_VALUES = _config_values()
ACCOUNT_NUMBERS = sorted({value for value, kind in _CONFIG_VALUES.items() if kind == "계좌번호"} | _baseline_accounts())


def _digits_pattern(value):
    """숫자 낱말은 앞뒤가 숫자·소수점이 아닐 때만 — 더 긴 숫자열 안에 우연히 박힌 것은 그 값이 아니다."""
    return re.compile(rf"(?<![\d.]){re.escape(value)}(?![\d.])")


_ACCOUNT_LITERALS = [_digits_pattern(account) for account in ACCOUNT_NUMBERS]
_ACCOUNT_MASKS = [(re.compile(pattern), replacement) for pattern, replacement in (
    (r"CANO=(?:[0-9*]|</?b>)*[0-9*](?:</?b>)?", "CANO=(가림)"),
    (r"\b\d{8}-\d{2}\b",                        "(계좌번호)"),
    (r"(?<![\w*])\d{2,3}\*{2,}\d{2,4}\b",       "(계좌번호)"),
    (r"\*{3,}\d{3,4}\b",                        "(계좌번호)"),
)]
# '계좌' 뒤 60자 안의 8자리 숫자 — 날짜(20xxxxxx)와 쉼표·소수점에 붙은 금액은 뺀다.
_ACCOUNT_WINDOW = re.compile(r"계좌[^<\n]{0,60}")
_BARE_EIGHT_DIGITS = re.compile(r"(?<![\d.,])(?!20\d{6})\d{8}(?![\d.,])")
# 비공개 폴더 경로 — _private/ 뒤 경로 조각까지. 따옴표·괄호·공백·표 칸막이·역슬래시(JSON 안 \n)에서 끊는다.
# 앞이 글자인 _private(is_private 같은 변수 이름)는 경로가 아니라 건드리지 않는다 — 페이지 스크립트를 깨뜨린다.
_PRIVATE_PATH = re.compile(r"(?:[\w./-]*[\\/])?(?<!\w)_private(?:[\\/][^\s\"'`<>()\]|\\]*|\b)")


def scrub(text):
    """계좌번호와 비공개 경로를 지운 글을 돌려준다."""
    for pattern, replacement in _ACCOUNT_MASKS:
        text = pattern.sub(replacement, text)

    for pattern in _ACCOUNT_LITERALS:
        text = pattern.sub("(계좌번호)", text)

    text = _ACCOUNT_WINDOW.sub(lambda match: _BARE_EIGHT_DIGITS.sub("(계좌번호)", match.group(0)), text)
    return _PRIVATE_PATH.sub("(비공개 경로)", text)


# 막는 것(block)과 사람에게 묻는 것(ask). 묻는 쪽은 기술 낱말·종목명과 겹칠 수 있어서다.
_HANGUL = "가-힣"
_LEAK_RULES = [
    ("block", "계좌번호 꼴", re.compile(r"\b\d{8}-\d{2}\b|CANO=(?:[0-9*]|</?b>)*[0-9]|(?<![\w*])\d{2,3}\*{2,}\d{2,4}\b|\*{3,}\d{3,4}\b")),
    ("block", "주민번호 꼴", re.compile(r"\b\d{6}-[1-4]\d{6}\b")),
    ("block", "전화번호 꼴", re.compile(r"(?<![\d.])01[016789]-?\d{3,4}-?\d{4}(?!\d)")),
    ("block", "개인 이메일", re.compile(r"[A-Za-z0-9._%+-]+@(?:gmail|naver|daum|kakao|outlook|hotmail)\.com")),
    ("block", "사용자 폴더 이름", re.compile(r"Users[\\/]+[^\\/\s\"'<>]*[" + _HANGUL + r"]")),
    ("block", "비공개 폴더 경로", _PRIVATE_PATH),
    ("ask",   "계좌 옆 8자리 숫자", re.compile(r"계좌[^<\n]{0,60}?(?<![\d.,])(?!20\d{6})\d{8}(?![\d.,])")),
    ("ask",   "타 프로젝트", re.compile(r"(?i)tigeronline|\btiger\b|project_?dev|ProjectR|MMO ?서버|MMOServer|마블럼블|Desktop[\\/]+Project")),
]


def _literal_rules():
    rules = [("block", "계좌번호", pattern) for pattern in _ACCOUNT_LITERALS]
    rules += [("block", kind, re.compile(re.escape(value)))
              for value, kind in _CONFIG_VALUES.items() if kind != "계좌번호"]
    # 낱말은 한글 낱말 경계에서만 — 종목명 안에 박힌 같은 글자("○○철강")는 그 낱말이 아니다. 조사는 허용한다.
    # 회사 이름과 묻기 목록(_private/publish_ask_words.txt)의 낱말은 종목 리포트·기술 정리에도 정당하게 나오므로
    # 묻기만 한다. 나머지 gate_words(가족 실명 등)는 막는다.
    ask_terms = _private_terms("publish_ask_words.txt")
    for term in dict.fromkeys(_private_terms("gate_words.txt") + ask_terms):
        pattern = re.compile(rf"(?<![{_HANGUL}]){re.escape(term)}(?:(?![{_HANGUL}])|(?=[은는이가을를의와과도께에]))")
        is_ask = term in ask_terms or re.search(r"(?:증권|투자|운용|펀드|은행)$", term)
        rules.append(("ask", "회사·묻기 목록 낱말", pattern) if is_ask else ("block", "비공개 낱말", pattern))

    return rules


def find_leaks(text, context=40):
    """[(등급, 종류, 건수, 문맥 하나)] — 문맥에서 찾은 값 자체는 ▒로 가린다."""
    leaks = []
    for level, kind, pattern in _LEAK_RULES + _literal_rules():
        matches = list(pattern.finditer(text))
        if not matches:
            continue

        start, end = matches[0].span()
        sample = text[max(0, start - context):start] + "▒" * min(8, end - start) + text[end:end + context]
        leaks.append((level, kind, len(matches), re.sub(r"\s+", " ", re.sub(r"<[^>]+>", "", sample))))

    return leaks
