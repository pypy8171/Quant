"""한 글자 지역변수·매개변수를 스코프 안에서만 풀어쓴다.

    py scripts/rename_locals.py [--apply] [--override <파일>] [경로 ...]

선언을 찾아 그 선언이 보이는 범위(for 문 본문, 매개변수라면 함수·람다 본문, 지역변수라면
블록 끝까지)를 잡고, 그 범위 안에서만 단어 경계로 바꾼다. 이름은 타입·순회 대상·초기화식으로
고르고, 고를 수 없으면 `?`로 보고만 한다(--override 파일에 `경로:줄:글자=새이름` 한 줄씩).

[inv] 문자열 리터럴·주석·#include는 위치만 맞춘 빈칸으로 가려 놓고 본다 — 리터럴 안은 바꾸지 않는다.
[inv] 안쪽 스코프부터 바꾼다 — 같은 글자를 안팎에서 다른 이름으로 풀어도 안쪽 것이 먼저 굳는다.
"""
import io
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rename_ids import collect, TOKEN_RE  # noqa: E402

KEYWORDS = {'return', 'case', 'else', 'delete', 'new', 'throw', 'goto', 'sizeof', 'typename', 'class', 'struct',
            'enum', 'template', 'operator', 'using', 'typedef', 'friend', 'extern', 'static_assert', 'namespace',
            'if', 'while', 'for', 'switch', 'catch', 'do', 'union', 'co_return', 'co_await', 'public', 'private',
            'protected', 'default', 'break', 'continue', 'constexpr', 'static', 'inline', 'virtual', 'explicit'}

INT_TYPES = re.compile(r'^(const\s+)?(unsigned\s+)?(int|size_t|std::size_t|long(\s+long)?|short|u?int\d+_t|'
                       r'std::u?int\d+_t|std::atomic<[^>]*>|ptrdiff_t|std::ptrdiff_t|DWORD|ULONG|LONG|UINT|WORD|BYTE)\s*[&*]*$')
FLOAT_TYPES = re.compile(r'^(const\s+)?(double|float|long double)\s*[&*]*$')
STRING_TYPES = re.compile(r'^(const\s+)?(std::string|std::string_view|std::wstring|CString|char\s*const\s*\*|'
                          r'const\s+char\s*\*|std::filesystem::path|fs::path)\s*[&*]*$')
CHAR_TYPES = re.compile(r'^(const\s+)?(char|unsigned char|wchar_t|std::byte|uint8_t)\s*[&*]*$')
BOOL_TYPES = re.compile(r'^(const\s+)?bool\s*[&*]*$')
JSON_TYPES = re.compile(r'^(const\s+)?(nlohmann::)?json\s*[&*]*$')

BY_LETTER_INT = {'i': 'index', 'j': 'inner_index', 'k': 'innermost_index', 'n': 'count', 'N': 'count',
                 'q': 'quantity', 'c': 'count', 'w': 'width', 'h': 'height', 'x': 'x_value', 'y': 'y_value',
                 'z': 'z_value', 'v': 'value', 'r': 'result', 'd': 'days', 't': 'time_value', 'm': 'row',
                 'p': 'position', 's': 'size', 'b': 'bit_value', 'a': 'first_value', 'e': 'end_index',
                 'f': 'flags', 'l': 'length', 'u': 'unsigned_value', 'g': 'group', 'o': 'offset',
                 'M': 'row_count', 'T': 'type_value'}
BY_LETTER_FLOAT = {'v': 'value', 'p': 'price', 's': 'sum', 'x': 'x_value', 'y': 'y_value', 'd': 'delta',
                   'r': 'ratio', 'q': 'quantity', 'w': 'weight', 'm': 'mean', 'a': 'amount', 'b': 'base',
                   'c': 'close', 'h': 'high', 'l': 'low', 'o': 'open', 'e': 'edge', 'f': 'factor', 'g': 'gain',
                   't': 'threshold', 'k': 'coefficient', 'n': 'count', 'z': 'z_score', 'u': 'upper', 'i': 'value',
                   'j': 'value2'}
BY_LETTER_STRING = {'t': 'ticker', 's': 'text', 'k': 'key', 'm': 'message', 'n': 'name', 'b': 'begin', 'e': 'end',
                    'p': 'cursor', 'c': 'code', 'r': 'raw', 'v': 'value', 'f': 'field', 'h': 'header',
                    'l': 'line', 'w': 'word', 'd': 'data', 'q': 'query', 'a': 'argument', 'x': 'text',
                    'u': 'url', 'i': 'id_text', 'o': 'output', 'g': 'group', 'j': 'json_text', 'z': 'text'}
BY_LETTER_CHAR = {'c': 'character', 'b': 'byte_value', 'p': 'cursor', 'e': 'end', 's': 'start', 'd': 'digit',
                  'x': 'character', 'q': 'character', 'a': 'character', 'k': 'character', 'w': 'character',
                  'r': 'character', 'i': 'character', 'n': 'character', 't': 'character', 'h': 'character',
                  'v': 'character', 'm': 'character', 'l': 'character', 'f': 'character', 'g': 'character',
                  'j': 'character', 'o': 'character', 'u': 'character', 'y': 'character', 'z': 'character'}
BY_LETTER_BOOL = {'b': 'flag', 'f': 'flag', 'r': 'result', 'v': 'value', 'x': 'flag', 'a': 'flag', 'c': 'flag',
                  'd': 'flag', 'e': 'flag', 'g': 'flag', 'h': 'flag', 'i': 'flag', 'j': 'flag', 'k': 'flag',
                  'l': 'flag', 'm': 'flag', 'n': 'flag', 'o': 'flag', 'p': 'flag', 'q': 'flag', 's': 'flag',
                  't': 'flag', 'u': 'flag', 'w': 'flag', 'y': 'flag', 'z': 'flag'}

# 클래스 이름 → 변수 이름. 없으면 CamelCase를 snake_case로 푼다.
CLASS_NAMES = {
    'OrderSignal': 'signal', 'StrategyBase': 'strategy', 'KisClient': 'kis', 'Client': 'client',
    'std::ifstream': 'file', 'std::ofstream': 'file', 'std::fstream': 'file', 'FILE': 'file',
    'kis_ws::Fields': 'fields', 'Fields': 'fields', 'Frame': 'frame', 'ops::Frame': 'frame', 'PosKey': 'key',
    'Holding': 'holding', 'sync::WakeGate': 'gate', 'WakeGate': 'gate', 'Rig': 'rig', 'RunResult': 'result',
    'Supervisor': 'supervisor', 'RegimeFileJudge': 'bridge', 'Series': 'series', 'strategy::Router': 'router',
    'Router': 'router', 'FrameReader': 'reader', 'Outcome': 'outcome', 'WatchSpec': 'spec', 'WSADATA': 'wsa_data',
    'Value': 'value', 'LedgerReconciler': 'reconciler', 'Cli': 'cli', 'feed::Record': 'record', 'Regime': 'regime',
    'AccountBalance': 'balance', 'QuoteTable': 'quotes', 'OrderGate::Config': 'config', 'Config': 'config',
    'Result': 'result', 'Record': 'record', 'std::mutex': 'mutex', 'HINTERNET': 'handle', 'BarSlot': 'slot',
    'OpenOrder': 'open_order', 'RankingStock': 'stock', 'std::thread': 'thread', 'std::jthread': 'thread',
    'std::exception': 'exception', 'std::runtime_error': 'exception', 'std::stop_token': 'stop_token',
    'std::error_code': 'error_code', 'zmq::error_t': 'zmq_error', 'TradeData': 'trade', 'OrderBook': 'order_book', 'MarketData': 'market_data',
    'ManagedOrder': 'managed_order', 'FillNotification': 'fill_notification', 'Bar': 'bar',
    'std::istringstream': 'stream', 'std::ostringstream': 'stream', 'std::stringstream': 'stream',
    'std::tm': 'time_parts', 'tm': 'time_parts', 'std::time_t': 'time_value', 'time_t': 'time_value',
    'sockaddr_in': 'address', 'SOCKET': 'socket', 'HANDLE': 'handle', 'HWND': 'window', 'CDC': 'device_context',
    'CRect': 'rect', 'CPoint': 'point', 'CString': 'text', 'std::stop_source': 'stop_source',
    'std::unique_lock': 'lock', 'std::lock_guard': 'lock', 'std::scoped_lock': 'lock', 'std::shared_lock': 'lock',
    'std::vector': 'values', 'std::set': 'items', 'std::map': 'items', 'std::unordered_map': 'items',
    'std::deque': 'items', 'std::array': 'values', 'std::span': 'values', 'std::pair': 'pair',
    'std::optional': 'maybe', 'std::function': 'callback', 'std::chrono::steady_clock::time_point': 'time_point',
    'std::chrono::system_clock::time_point': 'time_point', 'std::size_t': 'count',
}

ALT_NAMES = {'index': 'position', 'inner_index': 'inner_position', 'count': 'total', 'ticker': 'code',
             'trade': 'tick', 'document': 'parsed', 'node': 'element', 'spec': 'watch', 'result': 'outcome',
             'value': 'item', 'text': 'content', 'key': 'map_key', 'row': 'record', 'found': 'lookup',
             'signal': 'order_signal', 'strategy': 'strategy_item', 'json': 'parsed', 'holding': 'held',
             'price': 'price_value', 'quantity': 'quantity_value', 'exception': 'error', 'frame': 'frame_item',
             'file': 'stream', 'fields': 'field_list', 'entry': 'pair', 'header': 'header_line'}

PLURAL_MAP = {'strategies': 'strategy', 'entries': 'entry', 'series': 'point', 'children': 'child',
              'indices': 'index', 'data': 'datum', 'list': 'item', 'items': 'item', 'M7': 'ticker'}

QUALIFIERS = re.compile(r'^\s*(const|noexcept|override|final|mutable|volatile|&&|&|\s)+')
ARROW_RET = re.compile(r'^\s*->\s*[^{]*?(?=\{)')
DECL_PIECE = re.compile(r'^\s*(?P<type>(?:[A-Za-z_][\w:]*(?:\s*<[^;{}()]*?>)?\s*[&*]*\s+)+?)'
                        r'(?P<name>[A-Za-z])\s*(?:\[[^\]]*\])?\s*(?:(?P<init>[=({].*))?$', re.S)
BINDING = re.compile(r'^\s*(?P<type>(?:const\s+)?auto\s*[&*]*)\s*\[(?P<names>[^\]]*)\]\s*(?P<init>[=:].*)?$', re.S)
LOCAL_LINE = re.compile(r'^[ \t]*(?P<type>(?:const\s+|static\s+|constexpr\s+)*[A-Za-z_][\w:]*(?:\s*<[^;{}]*?>)?\s*[&*]*)'
                        r'\s+(?P<name>[A-Za-z])\s*(?:\[[^\]]*\])?\s*(?P<tail>=|;|\(|\{)', re.M)
LOCAL_BINDING = re.compile(r'^[ \t]*(?P<type>(?:const\s+)?auto\s*[&*]*)\s*\[(?P<names>[^\]]*)\]\s*=', re.M)
CLASS_HEAD = re.compile(r'(class|struct|union|namespace|enum(\s+class)?)\s+[\w:]*\s*(final\s*)?(:[^{;]*)?$')
IDENT = re.compile(r'[A-Za-z_]\w*')


def blank(src):
    """리터럴·주석·#include를 같은 길이의 빈칸으로 바꿔 위치를 보존한다."""
    def repl(match):
        text = match.group(0)
        return ''.join('\n' if ch == '\n' else ' ' for ch in text)

    return TOKEN_RE.sub(repl, src)


def match_brace(code, open_pos):
    """code[open_pos]가 여는 괄호일 때 짝 위치를 돌려준다."""
    pairs = {'(': ')', '{': '}', '[': ']'}
    close = pairs[code[open_pos]]
    depth = 0
    pos = open_pos

    while pos < len(code):
        ch = code[pos]

        if ch in pairs:
            depth += 1
        elif ch in ')}]':
            depth -= 1

            if depth == 0:
                return pos if ch == close else -1

        pos += 1

    return -1


def enclosing_block(code, pos):
    """pos를 감싸는 가장 가까운 { } 블록의 (열림, 닫힘)을 돌려준다. 없으면 None."""
    depth = 0
    cursor = pos - 1

    while cursor >= 0:
        ch = code[cursor]

        if ch in ')}]':
            depth += 1
        elif ch in '({[':
            if depth == 0:
                if ch == '{':
                    return cursor, match_brace(code, cursor)

                return None

            depth -= 1

        cursor -= 1

    return None


def is_class_body(code, open_pos):
    head = code[max(0, open_pos - 200):open_pos]
    head = head.split(';')[-1].split('}')[-1]
    return bool(CLASS_HEAD.search(head.strip()))


def split_top(text, sep):
    """꺾쇠·괄호 안의 sep는 무시하고 나눈다."""
    out = []
    depth = 0
    start = 0

    for pos, ch in enumerate(text):
        if ch in '<([{':
            depth += 1
        elif ch in '>)]}':
            depth -= 1
        elif ch == sep and depth == 0:
            out.append(text[start:pos])
            start = pos + 1

    out.append(text[start:])
    return out


def snake(name):
    name = name.split('::')[-1]
    name = re.sub(r'<.*', '', name)
    name = re.sub(r'(?<=[a-z0-9])([A-Z])', r'_\1', name)
    name = re.sub(r'([A-Z]+)([A-Z][a-z])', r'\1_\2', name)
    return name.lower().strip('_')


def singular(word):
    if word in PLURAL_MAP:
        return PLURAL_MAP[word]

    if word.endswith('ies') and len(word) > 4:
        return word[:-3] + 'y'

    if re.search(r'(ss|us|is)$', word):
        return None

    if word.endswith('es') and re.search(r'(sh|ch|x|z)es$', word):
        return word[:-2]

    if word.endswith('s') and len(word) > 3:
        return word[:-1]

    return None


def range_name(expr):
    """`for (auto& x : expr)`의 expr에서 원소 이름을 고른다."""
    expr = expr.strip()
    expr = re.sub(r'\(\s*\)$', '', expr)
    idents = IDENT.findall(expr)

    if not idents:
        return None

    last = idents[-1].strip('_')

    if last in ('first', 'second', 'value', 'get', 'begin', 'end', 'data', 'items', 'list', 'all', 'view'):
        last = idents[-2].strip('_') if len(idents) > 1 else last

    if re.match(r'^by_\w+$', last) or 'map' in last.lower():
        return 'entry'

    return singular(last) or None


def init_name(letter, init):
    """`auto x = init`의 init으로 이름을 고른다. 못 고르면 None."""
    init = init.lstrip('=({ ').strip()

    if re.search(r'json::parse|nlohmann::json', init):
        return 'document'

    if re.search(r'\.find\(|\.lower_bound\(|\.upper_bound\(', init):
        return 'found'

    if re.search(r'fetch_add|fetch_sub', init):
        return BY_LETTER_INT.get(letter, 'count')

    of_match = re.match(r'(?:[\w:]+\.|[\w:]+->|[\w:]+::)*(\w+)_of\s*\(', init)

    if of_match:
        return of_match.group(1)

    if re.search(r'\.begin\(\)', init):
        return 'begin'

    if re.search(r'\.end\(\)', init):
        return 'end'

    match = re.match(r'(?:[\w:]+\.|[\w:]+->|[\w:]+::)*(?:get_|make_|build_|load_|read_|parse_|fetch_|compute_|calc_)(\w+)\s*\(', init)

    if match:
        return match.group(1)

    match = re.match(r'std::make_(?:unique|shared)<([\w:]+)', init)

    if match:
        return snake(match.group(1))

    match = re.match(r'(?:[\w:]+\.|[\w:]+->)*(\w+)\s*\(', init)

    if match and match.group(1) not in ('std', 'static_cast', 'reinterpret_cast', 'const_cast') and len(match.group(1)) > 2:
        word = match.group(1)

        if word.startswith('to_'):
            return word[3:]

        return {'size': 'size', 'count': 'count', 'now': 'now', 'lock': 'lock', 'run': 'result',
                'open': 'opened', 'parse': 'parsed', 'load': 'loaded', 'read': 'read_result',
                'value': 'value', 'at': 'element', 'front': 'front', 'back': 'back', 'top': 'top',
                'emplace_back': 'element', 'push_back': 'element'}.get(word, word if '_' in word else None)

    match = re.match(r'(?:[\w:]+\.|[\w:]+->)*(\w+)\s*$', init.rstrip(';'))

    if match and len(match.group(1)) > 2:
        return match.group(1).strip('_')

    return None


def pick_name(letter, type_text, init, range_expr, override, hint=None):
    if override:
        return override

    if hint:
        return hint

    type_text = re.sub(r'\s+', ' ', type_text.strip())
    bare = re.sub(r'^(const|static|constexpr|volatile)\s+', '', type_text)
    bare = re.sub(r'\s*[&*]+$', '', bare).strip()

    if range_expr is not None:
        name = range_name(range_expr)

        if name:
            return name

    if INT_TYPES.match(type_text) or bare in ('int', 'size_t'):
        return BY_LETTER_INT.get(letter)

    if FLOAT_TYPES.match(type_text):
        return BY_LETTER_FLOAT.get(letter)

    if STRING_TYPES.match(type_text):
        return BY_LETTER_STRING.get(letter)

    if CHAR_TYPES.match(type_text):
        return BY_LETTER_CHAR.get(letter)

    if BOOL_TYPES.match(type_text):
        return BY_LETTER_BOOL.get(letter)

    if JSON_TYPES.match(type_text):
        return 'document' if letter == 'j' else 'node'

    if bare in ('auto', 'decltype(auto)'):
        if init:
            return init_name(letter, init)

        return None

    if bare in CLASS_NAMES:
        return CLASS_NAMES[bare]

    wrapped = re.match(r'^(?:std::)?(?:unique_ptr|shared_ptr|optional|reference_wrapper|weak_ptr)<\s*(?:const\s+)?([\w:]+)', bare)

    if wrapped:
        inner = wrapped.group(1)
        return CLASS_NAMES.get(inner) or snake(inner)

    if re.match(r'^(?:std::)?(?:vector|array|span|deque|set|list)<\s*(?:std::)?string', bare):
        return 'fields' if letter == 'f' else 'parts'

    if re.match(r'^[A-Z]$', bare):
        return 'value' if letter == 'v' else 'item'

    template_base = re.sub(r'<.*$', '', bare)

    if template_base in CLASS_NAMES:
        return CLASS_NAMES[template_base]

    if re.match(r'^[A-Za-z_][\w:]*$', bare) and bare[0].isupper() or '::' in bare:
        return snake(bare)

    return None


class Site:
    def __init__(self, path, line, letter, type_text, decl_pos, scope_start, scope_end, init=None, range_expr=None):
        self.path = path
        self.line = line
        self.letter = letter
        self.type_text = type_text
        self.decl_pos = decl_pos
        self.scope_start = scope_start
        self.scope_end = scope_end
        self.init = init
        self.range_expr = range_expr
        self.new_name = None
        self.note = ''
        self.hint = None


def find_sites(path, src, code):
    sites = []

    def line_of(pos):
        return src.count('\n', 0, pos) + 1

    # 1) 괄호 묶음 뒤에 { 가 오는 자리 — 함수·람다 매개변수, for·catch·if 헤더 선언
    for match in re.finditer(r'\(', code):
        open_pos = match.start()
        close_pos = match_brace(code, open_pos)

        if close_pos < 0:
            continue

        after = close_pos + 1
        tail = code[after:after + 400]
        qualifier = QUALIFIERS.match(tail)
        skip = qualifier.end() if qualifier else 0
        arrow = ARROW_RET.match(tail[skip:])

        if arrow:
            skip += arrow.end()

        rest = tail[skip:]

        if rest[:1] == ':' and rest[1:2] != ':':
            # 생성자 초기화 목록 — 깊이 0의 { 까지 건너뛴다
            depth = 0
            cursor = 0

            while cursor < len(rest):
                if rest[cursor] in '([':
                    depth += 1
                elif rest[cursor] in ')]':
                    depth -= 1
                elif rest[cursor] == '{' and depth == 0:
                    break

                cursor += 1

            skip += cursor
            rest = tail[skip:]

        if rest[:1] != '{':
            continue

        body_open = after + skip
        body_close = match_brace(code, body_open)

        if body_close < 0:
            continue

        head = code[max(0, open_pos - 40):open_pos].rstrip()
        keyword = re.search(r'([A-Za-z_]\w*)\s*$', head)
        keyword = keyword.group(1) if keyword else ''
        inner = code[open_pos + 1:close_pos]

        if keyword in ('if', 'while', 'switch', 'return', 'sizeof', 'else', 'do'):
            continue

        if keyword == 'for':
            parts = split_top(inner, ';')

            if len(parts) == 1:
                decl_part, _, range_expr = inner.partition(':')

                if not range_expr:
                    continue

                pieces = [(decl_part, range_expr)]
            else:
                pieces = [(piece, None) for piece in split_top(parts[0], ',')]
        else:
            pieces = [(piece, None) for piece in split_top(inner, ',')]

        offset = open_pos + 1

        for piece, range_expr in pieces:
            piece_start = offset
            offset += len(piece) + 1
            binding = BINDING.match(piece)

            if binding:
                for name in split_top(binding.group('names'), ','):
                    name = name.strip()

                    if re.match(r'^[A-Za-z]$', name):
                        pos = piece_start + piece.find('[') + binding.group('names').find(name) + 1
                        sites.append(Site(path, line_of(pos), name, binding.group('type'), pos, open_pos, body_close,
                                          None, range_expr))

                continue

            decl = DECL_PIECE.match(piece)

            if not decl:
                continue

            type_text = decl.group('type').strip()
            first_word = type_text.split()[0].split('<')[0].rstrip('&*')

            if first_word in KEYWORDS or type_text.endswith('<') or '=' in type_text or '<' in type_text and '>' not in type_text:
                continue

            if keyword == 'for' and range_expr is None and not decl.group('init'):
                continue

            pos = piece_start + decl.start('name')
            site = Site(path, line_of(pos), decl.group('name'), type_text, pos, open_pos, body_close,
                        decl.group('init'), range_expr)

            if keyword.startswith('set_') and len(pieces) == 1 and len(keyword) > 5:
                site.hint = keyword[4:]
            elif keyword == 'for' and range_expr is None and len(parts) >= 2:
                bound = re.search(r'(?<![\w.])' + decl.group('name') + r'\s*<=?\s*([^;&|]+)', parts[1])

                if bound:
                    idents = IDENT.findall(re.sub(r'\.(size|length|count)\(\)', '', bound.group(1)))

                    if idents and idents[-1] not in ('n', 'N', 'count', 'len', 'size', 'static_cast', 'int', 'size_t'):
                        bound_word = re.sub(r'^k(?=[A-Z])', '', idents[-1].strip('_'))
                        bound_word = re.sub(r'^(n_|num_|max_|n|get_)(?=[a-z_])', '', snake(bound_word))
                        bound_word = re.sub(r'(_count|_cnt|_n|_num|_size|_len|_total|_clamped)$', '', bound_word)
                        bound_word = {'width': 'column', 'height': 'row', 'cols': 'column', 'columns': 'column',
                                      'rows': 'row', 'argc': '', 'len': '', 'size': '', 'total': ''}.get(bound_word, bound_word)
                        word = None if bound_word in ('iter', 'iters', 'loop', 'loops', 'rep', 'reps', 'trial', 'trials', 'strat', 'strats') else singular(bound_word) or (bound_word if len(bound_word) >= 3 else None)

                        if word and len(word) >= 3:
                            site.hint = word + '_index'

            sites.append(site)

    # 2) 블록 안의 지역변수 선언 — 선언 위치부터 블록 끝까지
    for match in LOCAL_LINE.finditer(code):
        type_text = match.group('type').strip()
        first_word = re.split(r'[\s<&*]', type_text)[0]

        if first_word in KEYWORDS or type_text.count('<') != type_text.count('>'):
            continue

        block = enclosing_block(code, match.start('name'))

        if not block or block[1] < 0 or is_class_body(code, block[0]):
            continue

        pos = match.start('name')
        init = code[match.start('tail'):code.find(';', match.start('tail'))] if match.group('tail') != ';' else None
        sites.append(Site(path, line_of(pos), match.group('name'), type_text, pos, pos, block[1], init))

    for match in LOCAL_BINDING.finditer(code):
        block = enclosing_block(code, match.start())

        if not block or block[1] < 0 or is_class_body(code, block[0]):
            continue

        names = match.group('names')

        for name in split_top(names, ','):
            name = name.strip()

            if re.match(r'^[A-Za-z]$', name):
                pos = match.start('names') + names.find(name)
                sites.append(Site(path, line_of(pos), name, match.group('type'), pos, pos, block[1], None))

    return sites


USE_RE_CACHE = {}


def use_pattern(name):
    if name not in USE_RE_CACHE:
        USE_RE_CACHE[name] = re.compile(r'(?<![A-Za-z0-9_])(?<!\.)(?<!->)(?<!::)' + re.escape(name) + r'(?![A-Za-z0-9_])(?!\s*::)')

    return USE_RE_CACHE[name]


def rename_in_scope(src, code, site):
    """스코프 안의 사용처를 바꾼다. 멤버 접근(.x ->x)과 리터럴은 두고, src·code를 같이 갱신한다."""
    pattern = use_pattern(site.letter)
    pieces_src = []
    pieces_code = []
    pos = site.scope_start
    count = 0

    for match in pattern.finditer(code, site.scope_start, site.scope_end + 1):
        # `->x`는 lookbehind가 2글자라 별도 확인
        if code[match.start() - 2:match.start()] == '->':
            continue

        pieces_src.append(src[pos:match.start()])
        pieces_code.append(code[pos:match.start()])
        pieces_src.append(site.new_name)
        pieces_code.append(site.new_name)
        pos = match.end()
        count += 1

    pieces_src.append(src[pos:])
    pieces_code.append(code[pos:])
    return ''.join(src[:site.scope_start] + ''.join(pieces_src)), ''.join(code[:site.scope_start] + ''.join(pieces_code)), count


def load_overrides(path):
    overrides = {}

    if not path:
        return overrides

    for raw in io.open(path, encoding='utf-8'):
        raw = raw.strip()

        if not raw or raw.startswith('#'):
            continue

        key, _, value = raw.rpartition('=')
        file_part, line_part, letter = key.rsplit(':', 2)
        overrides[(file_part.replace('\\', '/'), int(line_part), letter)] = value.strip()

    return overrides


def main(argv):
    apply = '--apply' in argv
    argv = [a for a in argv if a != '--apply']
    override_path = None

    if '--override' in argv:
        index = argv.index('--override')
        override_path = argv[index + 1]
        del argv[index:index + 2]

    roots = argv if argv else ['Quant/src', 'Quant/include', 'Quant/tests', 'Quant/tools']
    overrides = load_overrides(override_path)
    total = 0
    unresolved = 0
    skipped = 0

    for path in collect(roots):
        rel = path.replace(os.sep, '/')
        src = io.open(path, encoding='utf-8', errors='surrogateescape', newline='').read()
        code = blank(src)

        if len(code) != len(src):
            print('!! 길이 불일치', rel)
            continue

        sites = find_sites(rel, src, code)
        # 안쪽(늦게 시작하는) 스코프부터. 같은 시작이면 더 짧은 것부터.
        sites.sort(key=lambda s: (-s.scope_start, s.scope_end - s.scope_start))
        seen = set()
        chosen = []

        for site in sites:
            key = (site.decl_pos, site.letter)

            if key in seen:
                continue

            seen.add(key)
            site.new_name = pick_name(site.letter, site.type_text, site.init, site.range_expr,
                                      overrides.get((rel, site.line, site.letter)), site.hint)

            if site.new_name and (len(site.new_name) < 3 or not re.match(r'^[A-Za-z_]\w*$', site.new_name)
                                  or site.new_name in KEYWORDS or site.new_name in ('int', 'auto', 'char', 'bool')):
                site.new_name = None

            if not site.new_name:
                unresolved += 1
                print('?  %s:%d %s  [%s] %s' % (rel, site.line, site.letter, site.type_text,
                                                 (site.range_expr or site.init or '').strip()[:60]))
                continue

            chosen.append(site)

        # 같은 새 이름이 겹치는 스코프에 둘 이상이면(글자가 다를 때) 양쪽 다 글자를 붙여 구분한다.
        for site in chosen:
            for other in chosen:
                if other is not site and other.new_name.rstrip('_' + other.letter) == site.new_name.rstrip('_' + site.letter)                         and other.letter != site.letter and other.scope_start <= site.scope_end                         and site.scope_start <= other.scope_end and not site.note:
                    site.new_name = site.new_name + '_' + site.letter
                    site.note = '(같은 이름 둘)'
                    break

        for site in chosen:
            scope_text = code[site.scope_start:site.scope_end + 1]

            if use_pattern(site.new_name).search(scope_text):
                alt = ALT_NAMES.get(site.new_name, 'other_' + site.new_name)

                if use_pattern(alt).search(scope_text):
                    skipped += 1
                    print('x  %s:%d %s -> %s 겹침 [%s]' % (rel, site.line, site.letter, site.new_name, site.type_text))
                    continue

                site.note = '(겹침 회피)'
                site.new_name = alt

            print('   %s:%d %s -> %s %s [%s]' % (rel, site.line, site.letter, site.new_name, site.note, site.type_text))
            total += 1

            if apply:
                # 사용처는 blank 코드 기준으로 찾고 src와 code에 같은 길이 차이로 반영한다.
                new_src, new_code, _ = rename_in_scope(src, code, site)
                delta = len(new_src) - len(src)
                src, code = new_src, new_code

                # 뒤에 처리할(앞쪽) 사이트는 위치가 안 변하지만, 같은 시작 위치의 다른 사이트 끝은 밀린다.
                for other in sites:
                    if other.scope_end >= site.scope_start and other is not site:
                        other.scope_end += delta

                        if other.scope_start > site.scope_start:
                            other.scope_start += delta
                            other.decl_pos += delta

        if apply:
            io.open(path, 'w', encoding='utf-8', errors='surrogateescape', newline='').write(src)

    print('바꿈 %d, 미정 %d, 겹침 %d' % (total, unresolved, skipped))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
