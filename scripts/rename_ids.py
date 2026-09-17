"""C++ 식별자를 단어 경계로 바꾼다 — 문자열 리터럴과 #include 줄은 건드리지 않는다.

약어 이름을 풀어쓰는 전수 작업에 쓴다. 한 번에 다 바꾸면 무엇이 깨졌는지 알 수 없어
매핑을 단계로 나눠 주고 단계마다 빌드·테스트를 돌리는 것을 전제로 한다.

    py scripts/rename_ids.py <매핑파일.json> [--files <glob> ...] [--dry]

매핑 파일은 {"옛이름": "새이름"} 하나짜리 JSON이다. 값이 빈 문자열이면 건너뛴다.

[inv] 문자열 리터럴 안은 절대 바꾸지 않는다 — KIS 전문 필드명(tr_id·ODNO·HHMMSS)과
      로그 문구·JSON 키가 거기 있고, 그것이 바뀌면 파서와 대시보드가 깨진다.
"""
import io
import json
import os
import re
import sys

CPP_EXT = ('.cpp', '.h', '.hpp', '.cc')

# 문자열·문자 리터럴, 줄주석, 블록주석, 전처리기 지시. 주석은 바꾸되 리터럴은 두기 위해
#  한 정규식으로 토큰을 잘라 놓고 종류별로 처리한다.
TOKEN_RE = re.compile(
    r'(?P<raw>R"([^(]*)\((?:.|\n)*?\)\2")'       # 원시 문자열 R"tag(...)tag"
    r'|(?P<str>"(?:[^"\\\n]|\\.)*")'             # 문자열 리터럴
    r'|(?P<chr>(?<![0-9])\'(?:[^\'\\\n]|\\.)*\')'  # 문자 리터럴(숫자 구분자 1'000은 제외)
    r'|(?P<line>//[^\n]*)'                       # 줄주석
    r'|(?P<block>/\*(?:.|\n)*?\*/)'              # 블록주석
    r'|(?P<inc>^[ \t]*#[ \t]*include[^\n]*$)',   # #include 줄
    re.M)


def build_pattern(mapping):
    """가장 긴 이름부터 맞춰야 sym이 sym_of를 잘라먹지 않는다."""
    names = sorted((k for k, v in mapping.items() if v), key=len, reverse=True)
    if not names:
        return None

    return re.compile(r'(?<![A-Za-z0-9_])(' + '|'.join(re.escape(n) for n in names) + r')(?![A-Za-z0-9_])')


def rewrite(src, pattern, mapping):
    """리터럴·#include를 뺀 나머지에만 치환을 적용하고, 바꾼 횟수를 함께 돌려준다."""
    out = []
    hits = 0
    pos = 0

    def sub_region(text):
        nonlocal hits

        def repl(match):
            nonlocal hits
            hits += 1
            return mapping[match.group(1)]

        return pattern.sub(repl, text)

    for token in TOKEN_RE.finditer(src):
        out.append(sub_region(src[pos:token.start()]))

        if token.lastgroup in ('line', 'block'):
            # 주석은 바꾼다 — 코드와 같은 이름을 부르고 있어야 주석이 거짓말을 하지 않는다.
            out.append(sub_region(token.group(0)))
        else:
            out.append(token.group(0))

        pos = token.end()

    out.append(sub_region(src[pos:]))
    return ''.join(out), hits


def collect(roots):
    paths = []

    for root in roots:
        if os.path.isfile(root):
            paths.append(root)
            continue

        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames if d not in ('build', 'build_win', 'out', '_deps')]

            for filename in filenames:
                if filename.endswith(CPP_EXT):
                    paths.append(os.path.join(dirpath, filename))

    return sorted(paths)


def main(argv):
    if not argv:
        print(__doc__)
        return 2

    mapping_path = argv[0]
    rest = argv[1:]
    dry = '--dry' in rest
    rest = [a for a in rest if a != '--dry']
    roots = rest if rest else ['Quant/src', 'Quant/include', 'Quant/tests', 'Quant/tools']

    mapping = json.load(io.open(mapping_path, encoding='utf-8'))
    pattern = build_pattern(mapping)

    if pattern is None:
        print('바꿀 이름이 없다')
        return 0

    total = 0
    touched = 0

    for path in collect(roots):
        src = io.open(path, encoding='utf-8', errors='surrogateescape').read()
        new, hits = rewrite(src, pattern, mapping)

        if hits:
            total += hits
            touched += 1
            print('%6d  %s' % (hits, path.replace(os.sep, '/')))

            if not dry:
                io.open(path, 'w', encoding='utf-8', errors='surrogateescape', newline='').write(new)

    print('%s %d곳 / %d파일' % ('바꿀 예정' if dry else '바꿈', total, touched))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
