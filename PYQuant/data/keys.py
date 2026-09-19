"""외부 API 키 로더 — `_private/keys.json`(gitignore) 한 곳에서만 읽는다.

키 값은 로그·주석·응답·예외 메시지 어디에도 찍지 않는다. 없는 이름이면 이름만 든 예외를 낸다.

사용:
    from PYQuant.data.keys import load_key
    api_key = load_key("dart")        # "dart" | "fred" | "ecos" | "datagokr"
"""

import json
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
KEYS_PATH = REPO_ROOT / "_private" / "keys.json"


def load_key(name: str) -> str:
    """이름에 해당하는 키 값을 돌려준다. 파일이나 이름이 없으면 KeyError(이름만 포함)."""
    if not KEYS_PATH.exists():
        raise KeyError(f"{KEYS_PATH.relative_to(REPO_ROOT)} 가 없다 — 키 이름 {name!r}")

    keys = json.loads(KEYS_PATH.read_text(encoding="utf-8"))
    value = keys.get(name)

    if not value:
        raise KeyError(f"키 이름 {name!r} 이 {KEYS_PATH.relative_to(REPO_ROOT)} 에 없다")

    return str(value)
