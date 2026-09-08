#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""결정 원장 → 파생 문서 자동 갱신.

`docs/DECISIONS.md`가 단일 소스다. 거기에 아래 두 줄 중 하나를 적으면 파생 문서가 따라온다.

    **원장**: 한 줄 요약(실험 결과 원장에 남길 문장)
    **음성결과**: 한 줄 요약(다시 주장하지 않을 것)

파생 대상:
  - research/STRATEGY_LAB.md  §2-b 결정 원장(자동)  + §3-d 실배선 현황
  - .claude/PROJECT_FACTS.md  알려진 음성 결과

원장을 손으로 옮겨 적는 규약은 실제로 지켜지지 않았다(2026-06-11 이후 3개월 정체).
사람이 적는 자리를 한 곳으로 줄이고 나머지는 생성한다.

사용:
    py scripts/sync_ledgers.py            # 갱신
    py scripts/sync_ledgers.py --check    # 드리프트만 검사(변경 있으면 exit 1)
"""
import io
import json
import os
import re
import sys

sys.stdout.reconfigure(encoding='utf-8')

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DECISIONS = os.path.join(ROOT, 'docs', 'DECISIONS.md')
LAB = os.path.join(ROOT, 'research', 'STRATEGY_LAB.md')
FACTS = os.path.join(ROOT, '.claude', 'PROJECT_FACTS.md')
CONFIG = os.path.join(ROOT, 'Quant', 'config', 'config_dev_paper.json')

HEAD_RE = re.compile(r'^### (D-\d+) (.+?) \((\d{4}-\d{2}-\d{2})\)\s*$')
FIELD_RE = re.compile(r'^\*\*(원장|음성결과|상태)\*\*:\s*(.+?)\s*$')


def read(path):
    return io.open(path, encoding='utf-8').read()


def parse_decisions():
    """### D-NNN 절을 훑어 원장·음성결과 마커를 모은다."""
    out = []
    cur = None
    for line in read(DECISIONS).splitlines():
        m = HEAD_RE.match(line)
        if m:
            cur = {'id': m.group(1), 'title': m.group(2).strip(),
                   'date': m.group(3), '상태': '', '원장': '', '음성결과': ''}
            out.append(cur)
            continue
        if cur is None:
            continue
        f = FIELD_RE.match(line)
        if f and not cur[f.group(1)]:
            cur[f.group(1)] = f.group(2)
    return out


def block(name, body):
    """마커로 감싼 자동생성 구간. 마커 사이는 손으로 고치지 않는다."""
    return ('<!-- AUTO:%s — scripts/sync_ledgers.py가 생성한다. 여기 직접 쓰지 말고 '
            'docs/DECISIONS.md에 적는다. -->\n%s\n<!-- /AUTO:%s -->' % (name, body, name))


def splice(text, name, body):
    """마커 구간을 갈아끼운다. 없으면 실패 — 자리는 사람이 한 번 정한다."""
    pat = re.compile(r'<!-- AUTO:%s.*?-->\n.*?\n<!-- /AUTO:%s -->' % (name, name), re.S)
    if not pat.search(text):
        raise SystemExit('[sync_ledgers] AUTO:%s 마커가 없다 — 삽입 위치를 먼저 정하라' % name)
    return pat.sub(lambda _: block(name, body), text, count=1)


def gen_lab_ledger(ds):
    rows = [d for d in ds if d['원장']]
    if not rows:
        return '_(원장 마커가 붙은 결정 없음)_'
    out = ['| 결정 | 날짜 | 제목 | 원장 |', '|---|---|---|---|']
    for d in rows:
        out.append('| %s | %s | %s | %s |' %
                   (d['id'], d['date'][5:], d['title'], d['원장']))
    return '\n'.join(out)


def gen_facts(ds):
    rows = [d for d in ds if d['음성결과']]
    if not rows:
        return '_(음성결과 마커가 붙은 결정 없음)_'
    return '\n'.join('- **%s**: %s' % (d['id'], d['음성결과']) for d in rows)


def gen_wiring():
    """문서가 말하는 기본값이 아니라 config에 실제로 들어 있는 값을 적는다."""
    cfg = json.load(io.open(CONFIG, encoding='utf-8'))
    lines = []
    for s in cfg.get('strategies', []):
        sid = s.get('id_prefix', s.get('type', '?'))
        lines.append('- `%s` — `kosdaq_enabled=%s`, 이격밴드 `%s~%s`, 재스캔 %ss, `base_pct=%s`' % (
            sid,
            json.dumps(s.get('kosdaq_enabled', False)),
            s.get('min_dev_pct', 0.0), s.get('max_dev_pct', '—'),
            s.get('rescan_interval_sec', '—'), s.get('base_pct', '—')))
    rs = cfg.get('regime_strategies', {})
    lines.append('- 국면→전략: ' + ' / '.join(
        '%s=[%s]' % (k, ', '.join(v) if v else '없음') for k, v in rs.items()))
    lines.append('')
    lines.append('> 위 표의 회의 기록은 그때의 판단이고, 이 줄은 **지금 config에 들어 있는 값**이다. '
                 '둘이 어긋나면 어긋난 채로 보인다 — 문서를 고쳐 맞추지 말고 어느 쪽이 의도인지 정한다.')
    return '\n'.join(lines)


def main():
    check = '--check' in sys.argv
    ds = parse_decisions()
    targets = [
        (LAB, [('decisions-ledger', gen_lab_ledger(ds)), ('live-wiring', gen_wiring())]),
        (FACTS, [('negative-results', gen_facts(ds))]),
    ]
    drift = []
    for path, blocks in targets:
        before = read(path)
        after = before
        for name, body in blocks:
            after = splice(after, name, body)
        if after == before:
            continue
        rel = os.path.relpath(path, ROOT).replace(os.sep, '/')
        drift.append(rel)
        if not check:
            io.open(path, 'w', encoding='utf-8', newline='\n').write(after)

    if check:
        if drift:
            print('[sync_ledgers] 원장 드리프트: ' + ', '.join(drift))
            print('  → py scripts/sync_ledgers.py 로 갱신')
            return 1
        print('[sync_ledgers] 원장 동기화 상태 정상')
        return 0
    print('[sync_ledgers] 갱신: ' + (', '.join(drift) if drift else '변경 없음'))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
