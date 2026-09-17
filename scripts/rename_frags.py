"""약어 조각(fragment) 단위 치환 — 3단계. snake_case 식별자를 `_`로 쪼개 조각마다 풀네임으로 바꾼다.

사용: py scripts/rename_frags.py [--apply] [경로...]
  기본은 dry-run: 바뀌는 (옛 이름 → 새 이름) 쌍을 빈도순으로 출력한다.
  리터럴·#include는 건드리지 않고 주석은 바꾼다(rename_ids.py와 같은 토큰 규칙).

규칙(위에서부터 먼저 맞는 것을 쓴다)
  1. SKIP — C 런타임·Winsock 멤버·표준 별칭은 손대지 않는다. `.str()`·`.ptr`처럼 표준 멤버 이름은 멤버 접근 위치에서만 건너뛴다.
  2. `std::`·`zmq::`·`nlohmann::`·`json::`·`chrono::`로 시작하는 한정 이름은 건드리지 않는다.
  3. PER_FILE(파일 basename별) > WHOLE(정확 일치) > WIRE(KIS 전문 필드 조각이 들어간 이름은 그대로) > FRAG(조각별).
     PER_FILE은 토큰 전체에만, PER_FILE_FRAG는 조각에도 쓴다(`Monitors.cpp`의 `dir`·`trade_dir` 둘 다 방향이다).
  4. 대문자가 섞인 이름(타입·매크로·Win32)은 WHOLE/PER_FILE에 있을 때만 바꾼다.
  5. 단위 접미사(`_ns`·`_ms`·`_us`·`_sec`·`_min`)는 그대로 둔다 — 단위는 약어가 아니라 표기다.
"""
import io
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rename_ids import TOKEN_RE, collect  # noqa: E402

# 어느 위치에서든 같은 뜻인 조각.
FRAG = {
    'agg': 'aggregator', 'cfg': 'config', 'conf': 'config', 'mtx': 'mutex', 'mu': 'mutex', 'qty': 'quantity',
    'px': 'price', 'pct': 'percent', 'avg': 'average', 'cur': 'current', 'prev': 'previous', 'idx': 'index',
    'cnt': 'count', 'str': 'string', 'num': 'number', 'dev': 'deviation', 'tot': 'total', 'tmp': 'temporary',
    'src': 'source', 'srcs': 'sources', 'dst': 'destination', 'dir': 'directory', 'val': 'value', 'vec': 'vector',
    'arr': 'array', 'res': 'result', 'req': 'request', 'resp': 'response', 'rsp': 'response', 'ctx': 'context',
    'env': 'environment', 'fmt': 'format', 'hdr': 'header', 'sz': 'size', 'secs': 'seconds', 'ws': 'websocket',
    'cb': 'callback', 'msg': 'message', 'err': 'error', 'sig': 'signal', 'cmd': 'command', 'desc': 'description',
    'init': 'initialize', 'impl': 'implementation', 'obj': 'object', 'arg': 'argument', 'args': 'arguments',
    'params': 'parameters', 'param': 'parameter', 'opt': 'option', 'iter': 'iterator', 'btn': 'button',
    'dlg': 'dialog', 'wnd': 'window', 'evt': 'event', 'ev': 'event', 'pkt': 'packet', 'seg': 'segment',
    'conn': 'connection', 'sock': 'socket', 'addr': 'address', 'auth': 'authentication', 'tok': 'token',
    'acct': 'account', 'amt': 'amount', 'bal': 'balance', 'vol': 'volume', 'mkt': 'market', 'mrkt': 'market',
    'sym': 'symbol', 'ord': 'order', 'lvl': 'level', 'thr': 'threshold', 'th': 'threshold', 'ths': 'threads',
    'grp': 'group', 'cat': 'category', 'typ': 'type', 'pat': 'pattern', 'dbg': 'debug', 'hist': 'history',
    'stats': 'statistics', 'calc': 'calculate', 'cmp': 'compare', 'diff': 'difference', 'accum': 'accumulated',
    'acc': 'accumulator', 'med': 'median', 'var': 'variance', 'sd': 'standard_deviation', 'corr': 'correlation',
    'coef': 'coefficient', 'lo': 'low', 'hi': 'high', 'ma': 'moving_average', 'sma': 'simple_moving_average',
    'smas': 'simple_moving_averages', 'akis': 'account_kis', 'tkr': 'ticker', 'tk': 'ticker', 'nm': 'name',
    'sfx': 'suffix', 'chg': 'change', 'dup': 'duplicate', 'asc': 'ascending', 'cxl': 'cancel', 'ver': 'version',
    'lbl': 'label', 'hwm': 'high_water_mark', 'hw': 'high_water', 'tid': 'thread_id', 'tz': 'timezone',
    'eps': 'epsilon', 'liq': 'liquidation', 'resv': 'reserved', 'orig': 'original', 'oid': 'order_id',
    'cid': 'client_id', 'rng': 'random_engine', 'sess': 'session', 'rc': 'result_code', 'fd': 'descriptor',
    'mux': 'multiplexer', 'clk': 'clock', 'cli': 'client', 'rd': 'reader', 'kc': 'kis_config', 'rr': 'run_result',
    'fk': 'forward_key', 'nk': 'next_key', 'cont': 'continuation', 'eng': 'engine', 'ln': 'line', 'lat': 'latency',
    'obs': 'observation', 'pctl': 'percentiles', 'raws': 'raw_minutes', 'ref': 'reference', 'cond': 'condition',
    'tab': 'table', 'ack': 'acknowledgement', 'rack': 'reconcile_ack', 'q': 'queue', 'ob': 'order_book',
    'td': 'trade', 'cv': 'condition_variable', 'buf': 'buffer', 'seq': 'sequence', 'eval': 'evaluation',
    'ptr': 'pointer', 'ptrs': 'pointers', 'recs': 'records', 'spec': 'specification', 'specs': 'specifications',
    'disp': 'display', 'jit': 'jitter', 'fut': 'future', 'w': 'weight', 'def': 'default_value', 'got': 'received',
    'hh': 'hour', 'mm': 'minute', 'tv': 'time_value', 'sa': 'socket_address', 'ls': 'listen_socket',
    'uc': 'url_components', 'rit': 'reserved_iterator', 'rb': 'rest_bar', 'cm': 'cancel_ack', 'lt': 'local_time',
    'kb': 'kis_block', 'qc': 'quote_client', 'eqc': 'equity_quote_client', 'hyst': 'hysteresis', 'tol': 'tolerance',
    'inst': 'institution', 'prod': 'produced', 'cons': 'consumed', 'unpr': 'unit_price', 'strat': 'strategy',
    'prio': 'priority', 'bcast': 'broadcast', 'dedup': 'deduplicate', 'evlu': 'evaluation', 'cum': 'cumulative',
    'trunc': 'truncate', 'pos': 'position', 'seqs': 'sequences', 'cdf': 'cumulative_distribution',
}

# 토큰 전체가 정확히 맞을 때만 — 파일마다 뜻이 같은 짧은 지역 이름.
WHOLE = {
    'p_': 'parameters_', 'ask_p': 'ask_price', 'bid_p': 'bid_price', 'now_p': 'now_time_point',
    'p_frame': 'payload_frame', 'ask_q': 'ask_quantity', 'bid_q': 'bid_quantity', 'mm_qty': 'market_making_quantity',
    'hReq': 'request_handle', 'hConnect': 'connect_handle', 'hSession': 'session_handle', 'hSess': 'session_handle',
    'hConn': 'connection_handle', 'hAlg': 'algorithm_handle', 'hKey': 'key_handle', 'hOut': 'output_handle',
    'hWs': 'websocket_handle', 't_conn': 'thread_connection', 't_recv': 'receive_thread', 't_str': 'strategy_thread',
    't_ord': 'order_thread', 't_send': 'send_thread', 't_ws': 'websocket_thread', 't_acc': 'accept_thread',
    'th_': 'thread_', 'arg_i64': 'argument_int64', 'arg_dbl': 'argument_double', 'arg_str': 'argument_string',
    'hi_n': 'high_count', 's_dev': 'deviation20_percent', 's_eq': 'static_equity', 'balance_c': 'balance_three',
    'now_c': 'now_steady', 'slot_c': 'slot_three', 'tr_n': 'true_range_count', 'tr_sum': 'true_range_sum',
    'tr': 'true_range', 'td_it': 'trade_iterator', 'oit': 'owned_iterator', 'pct_us': 'percentile_us',
    'pct_ns': 'percentile_ns', 'ms': 'milliseconds', 'ns': 'nanoseconds', 'sec': 'seconds', 'sock_t': 'socket_handle_t',
    'ob_cap': 'order_book_capacity', 'td_cap': 'trade_capacity', 'order_cap': 'order_capacity',
    'sp_ticker': 'startup_ticker', 'sp_qty': 'startup_quantity', 'kis_ws': 'kis_websocket',
    'ws_platform': 'websocket_platform', 'fp_': 'file_', 'all0': 'strategy_all_first', 'all1': 'strategy_all_second',
    'a0': 'strategy_a_first', 'a1': 'strategy_a_second', 's0': 'shard_a', 'b0': 'decoded_book', 'in_sig': 'in_signal',
    'lat': 'latencies', 'lat_ns': 'latencies_ns', 'w_liq': 'weight_liquidity', 't_strat': 'strategy_thread', 'fill_tr': 'fill_notice_tr_id', 'jit_pct': 'jitter_percent',
    'd_ma': 'daily_averages', 'd_prev': 'daily_averages_previous', 'prev_c': 'previous_close', 'mk_ts': 'make_timestamp',
    'acml_base': 'accumulated_base', 'odno_to_cid_': 'odno_to_client_id_',
    'psbl_cap': 'possible_quantity_cap', 'got_lane': 'received_lane', 'fut': 'is_future', 'u01': 'uniform01',
    'maxr': 'max_records', 'valb': 'value_bits', 'wurl': 'wide_url', 'word': 'wide_text', 'fund': 'fundamentals',
    'fmt1': 'format_one_decimal', 'tt': 'now_time', 'ap': 'average_price_iterator', 'ar': 'active_regimes',
    'mh': 'guardians_node', 'pp': 'reserved_price_iterator', 'lg': 'logger', 'oo': 'open_order', 'mk': 'mask_key',
    'pj': 'parsed_json', 'ep': 'exception_pointers', 'bd': 'business_date', 'iv': 'initialization_vector',
    'av': 'average_value', 'cd': 'cooldown_iterator', 'rv': 'reserved_found', 'mj': 'regime_json',
    'itq': 'quote_iterator', 'itv': 'quote_found', 'itp': 'quote_entry', 'gc': 'gate_config', 'o2': 'output2_node',
    'bq': 'base_quantity', 'oi': 'opened_iterator', 'pt': 'price_target_node', 'dp': 'deviation_params',
    'rj': 'risk_json', 'lv': 'list_view', 'rq': 'rung_quantity', 'sb': 'buy_sell_code', 'wr': 'write_set',
    'mq': 'market_quote', 'ta': 'thread_a', 'tb': 'thread_b', 'kp': 'kept_row', 'icc': 'common_controls',
    'dx': 'data_exchange', 'hd': 'holding', 'es': 'entry_scale_node', 'nv': 'value_count', 'led': 'ledger_sellable',
    'hp': 'held_position', 'act': 'action_text', 'gcfg': 'gate_config', 'rmap': 'regime_map', 'tmi': 'time_info',
    'rst': 'ansi_reset', 'ik': 'krw_iterator', 'ft': 'timestamp_iterator', 'uit': 'universe_iterator',
    'zt': 'z_trend', 'zp': 'z_pull', 'zv': 'z_volume', 'zl': 'z_liquidity', 'rts': 'run_start_ns',
    'obc': 'order_book_count', 'tdc': 'trade_count', 'aux': 'auxiliary_trade', 'lq': 'ledger_quantity',
    'lav': 'ledger_average', 'pit': 'position_iterator', 'pr': 'daily_probe', 'sit': 'strategy_iterator',
    'ex': 'exception', 'sel': 'selected', 'mx': 'max_value', 'ip': 'index_price', 'fp': 'future_price',
    'sp': 'watch_specification', 'ss': 'stream', 'lp': 'last_price_iterator', 'rev': 'revise_ack', 'rl': 'read_lock',
    'wl': 'write_lock', 'si': 'to_int64', 'sc': 'score_node', 'nb': 'nonblocking', 'sv': 'sellable_after',
    'bp': 'buy_price', 'eq': 'equity', 't1': 'end_time', 't2': 'later_time', 'r1': 'result_a', 'r2': 'result_b',
    'r3': 'result_c', 'd1': 'from_date', 'd2': 'to_date', 'p1': 'page_a', 'p2': 'page_b', 'p3': 'page_c',
    'h1': 'account_hash', 'h2': 'ticker_hash', 's1': 'snapshot_one', 'd3': 'snapshot_three', 'f2': 'fields_two',
    'td2': 'trade_two', 'td3': 'trade_three', 'row1': 'print_row', 'have': 'found', 'want': 'wanted_count',
    'Pctl': 'PercentileSummary', 'Pct': 'PercentileSummary', 'Smas': 'SimpleMovingAverages', 'Msg': 'Message',
    'Feat': 'Features', 'Cli': 'Client', 'Rep': 'Representation', 'Pred': 'Predicate', 'Dur': 'Duration',
    's5': 'average_5', 's10': 'average_10', 's20': 'average_20', 's60': 'average_60',
    'ma5': 'moving_average_5', 'ma10': 'moving_average_10', 'ma20': 'moving_average_20', 'ma60': 'moving_average_60',
    'bid1': 'best_bid', 'ask1': 'best_ask', 'mid': 'mid_price', 'hms': 'time_of_day', 'of': 'output_file',
    'ext': 'extended_length', 'tm_info': 'time_info', 'pos_it': 'position_iterator',
}

# 파일별 — 같은 짧은 이름이 파일마다 다른 뜻일 때. 토큰 전체와 조각 양쪽에 적용한다.
PER_FILE = {
    'RegimeFileBridge.h': {'sc': 'score_node', 'sel': 'selected_regime'},
    'StrategyFactory.cpp': {'sc': 'scan_config', 'sp': 'short_period', 'lp': 'long_period', 'sv': 'sleeve_entry'},
    'SupplyDemandPullbackStrategy.h': {'sc': 'score'},
    'UniverseScanner.cpp': {'sc': 'sector_code', 'fp': 'prices_node', 'have': 'cached', 'off': 'risk_off',
                            'want': 'want_off'},
    'test_order_gate.cpp': {'sc': 'signal_c', 'sd': 'signal_d', 'r1': 'fill_a', 'r2': 'fill_b', 'p1': 'plan_a'},
    'Engine.cpp': {'ip': 'index_price', 'nb': 'buy_top_count', 'ns': 'sell_top_count', 'wl': 'specs_lock',
                   'sec': 'sector', 'secs': 'sectors', 'want': 'wanted_tickers'},
    'OpsServer.cpp': {'ip': 'ip_text'},
    'OrderGate.cpp': {'ip': 'position_iterator', 'ss': 'reason_text', 'si': 'sellable_iterator'},
    'bench_intake.cpp': {'hw': 'hardware_threads'},
    'bench_feed_ingest.cpp': {'have': 'received_count', 'off': 'offset'},
    'ops_client.cpp': {'off': 'offset', 'want': 'wanted_type'},
    'test_ops_server.cpp': {'want': 'wanted_type'},
    'test_replay_source.cpp': {'want': 'expected'},
    'BarAggregator.cpp': {'want': 'wanted_count'},
    'feed_latency_probe.cpp': {'sp': 'session', 'want': 'wanted_count'},
    'OpsTerminalDlg.cpp': {'lp': 'lparam', 'str': 'text_of', 'num': 'number_of', 'col': 'column'},
    'OpsTerminalDlg.h': {'lp': 'lparam'},
    'PaperExecutor.h': {'lp': 'last_price_iterator'},
    'test_paper_executor.cpp': {'ex': 'executor', 'rev': 'revise_result'},
    'KisAuth.cpp': {'rl': 'refresh_lock'},
    'KisIndex.cpp': {'sd': 'number_of'}, 'KisUniverse.cpp': {'sd': 'number_of'},
    'KisClient.h': {'ss': 'second'},
    'test_bar_aggregator.cpp': {'ss': 'second', 'd1': 'trade_a', 'd2': 'trade_b', 'acml': 'accumulated_volume'},
    'test_strategy_router.cpp': {'ss': 'strategies'},
    'test_shard_matrix.cpp': {'mx': 'matrix'},
    'DeviationScaleStrategy.h': {'sp': 'sell_price'},
    'Monitors.cpp': {'sp': 'stock_price', 'dir': 'direction', 'col': 'color', 's5': 'average_5_text', 's10': 'average_10_text',
                     's20': 'average_20_text', 's60': 'average_60_text'},
    'ZmqBridge.cpp': {'sp': 'space_position', 'rep': 'reply_socket', 'pub': 'publish_socket'},
    'main.cpp': {'sp': 'startup_probe_node'},
    'test_account_ledger.cpp': {'r1': 'row_a', 'r2': 'row_b'},
    'test_order_router.cpp': {'r1': 'ack_a', 'r2': 'ack_b', 'h1': 'recent_a', 'h2': 'recent_b', 'acc': 'accepted_row'},
    'test_ledger_reconciler.cpp': {'r2': 'second_reconciler'},
    'test_strategy_shard.cpp': {'s1': 'shard_b', 'm0': 'first_shard_index', 'mm': 'shard_index'},
    'test_reconcile_plan.cpp': {'pr': 'row_naver'},
    'test_feed_mux.cpp': {'cap_': 'capacity_', 'pa': 'source_a', 'pb': 'source_b'},
    'TickCapture.h': {'did': 'drained'},
    'bench_wake_gate.cpp': {'rep': 'repeat'},
    'bench_hot_path.cpp': {'ns_old': 'old_ns', 'ns_new': 'new_ns'},
}

# 파일별 조각 치환 — 그 파일 안에서는 조각으로 들어가도 같은 뜻일 때만.
PER_FILE_FRAG = {
    'Monitors.cpp': {'dir': 'direction'},
    'Utf8.h': {'w': 'width'},
}

# 손대면 안 되는 이름 — C 런타임·Winsock 멤버·MFC 매크로·표준 별칭·전문 코드 이름.
SKIP = set("""
c_str fd_set sockaddr_in sockaddr u_long in_addr addrinfo ai_addr ai_addrlen ai_family ai_next ai_protocol ai_socktype
sin_addr sin_family sin_port s_addr tv_sec tv_usec afx_msg errno rep npos size_t time_t int64_t uint64_t int32_t uint32_t
int16_t uint16_t int8_t uint8_t wchar_t socket_t ssize_t off_t is_trivially_copyable_v is_same_v decay_t remove_cvref_t
unique_ptr shared_ptr weak_ptr seq_cst fs hh_mm_ss did_work
curl_easy_perform curl_easy_init curl_easy_setopt curl_easy_cleanup curl_slist_append curl_slist_free_all
curl_easy_strerror curl_global_init curl_global_cleanup tm_hour tm_min tm_sec tm_year tm_mon tm_mday tm_wday tm_yday
tm_isdst ns_per_op lld ws_ok tr_id tr_key tr_cont tr_cd rt_cd msg_cd msg1 ctx_area ctx_area_nk100
test_feed_mux bench_feed_mux bench_sleep_res config_dev_paper compare_ws_bars
""".split())

# 멤버 접근 위치(`.x`·`->x`)에서만 건너뛰는 표준 멤버 이름.
MEMBER_SKIP = {'str', 'ptr', 'ec', 'num'}

# KIS 전문 필드를 그대로 옮긴 C++ 이름 — 조각이 하나라도 들어가면 그 이름은 전문 그대로 둔다(grep 가능성).
WIRE = set("""
odno evlu pbmn pchs pric nass bfdy asst gno brno sll dvsn frgn ntby orgn hldg hts otst stpl futs cntg pfls prvs rcdl
excc dnca prsn unpr psbl acml
""".split())

STD_ROOTS = {'std', 'zmq', 'nlohmann', 'json', 'chrono', 'fs', 'filesystem'}
UNIT_SUFFIX = {'ns', 'ms', 'us', 'sec', 'min'}
SNAKE_RE = re.compile(r'^_*[a-z][a-z0-9]*(?:_[a-z0-9]+)*_*$')
ID_RE = re.compile(r'(?<![\w\'])[A-Za-z_]\w*')
COMMENT_PROTECT_RE = re.compile(r'"[^"\n]*"|`[^`\n]*`')
FILE_EXT_RE = re.compile(r'\.(json|py|md|csv|cpp|h|txt|ps1|cmd|log)\b')


def qualified_root(code, start):
    """`a::b::name`의 name 앞이면 a를, 아니면 None."""
    if start < 2 or code[start - 2:start] != '::':
        return None

    end = start - 2
    begin = end

    while begin > 0 and (code[begin - 1].isalnum() or code[begin - 1] in '_:'):
        begin -= 1

    chain = code[begin:end]
    return chain.split('::')[0]


def new_name(name, basename, is_member):
    if name in SKIP or (is_member and name in MEMBER_SKIP):
        return name

    per_file = PER_FILE.get(basename, {})

    if name in per_file:
        return per_file[name]

    if name in WHOLE:
        return WHOLE[name]

    if not SNAKE_RE.match(name):
        return name

    lead = len(name) - len(name.lstrip('_'))
    trail = len(name) - len(name.rstrip('_'))
    core = name[lead:len(name) - trail] if trail else name[lead:]
    pieces = core.split('_')

    if any(piece in WIRE for piece in pieces):
        return name

    out = []

    for position, piece in enumerate(pieces):
        is_last = position == len(pieces) - 1

        if is_last and len(pieces) > 1 and piece in UNIT_SUFFIX:
            out.append(piece)
            continue

        out.append(PER_FILE_FRAG.get(basename, {}).get(piece) or FRAG.get(piece, piece))

    return '_' * lead + '_'.join(out) + '_' * trail


def rewrite(src, basename):
    """리터럴·#include 밖의 식별자를 바꾼다. (새 소스, [(옛, 새)], [(옛, 새)] 멤버 접근 위치)"""
    out = []
    pairs = []
    member_pairs = []
    pos = 0

    def sub_region(text, in_comment=False):
        # 주석 안의 따옴표·백틱 구간(전문 키·config 키)과 파일 이름(x.json·x.py)은 산문이 아니라 참조라 그대로 둔다.
        protected = []

        if in_comment:
            for span in COMMENT_PROTECT_RE.finditer(text):
                protected.append((span.start(), span.end()))

        def repl(match):
            name = match.group(0)
            start = match.start()

            if any(begin <= start < end for begin, end in protected):
                return name

            if in_comment and FILE_EXT_RE.match(text, match.end()):
                return name

            root = qualified_root(text, start)

            if root in STD_ROOTS:
                return name

            is_member = start >= 1 and (text[start - 1] == '.' or text[start - 2:start] == '->')
            renamed = new_name(name, basename, is_member)

            if renamed != name:
                pairs.append((name, renamed))

                if is_member:
                    member_pairs.append((name, renamed))

            return renamed

        return ID_RE.sub(repl, text)

    for token in TOKEN_RE.finditer(src):
        out.append(sub_region(src[pos:token.start()]))

        if token.lastgroup in ('line', 'block'):
            out.append(sub_region(token.group(0), in_comment=True))
        else:
            out.append(token.group(0))

        pos = token.end()

    out.append(sub_region(src[pos:]))
    return ''.join(out), pairs, member_pairs


def main(argv):
    apply = '--apply' in argv
    roots = [a for a in argv if a != '--apply'] or ['Quant/src', 'Quant/include', 'Quant/tests', 'Quant/tools']
    total = 0
    touched = 0
    all_pairs = {}
    all_members = {}

    for path in collect(roots):
        src = io.open(path, encoding='utf-8', errors='surrogateescape', newline='').read()
        new, pairs, member_pairs = rewrite(src, os.path.basename(path))

        if not pairs:
            continue

        total += len(pairs)
        touched += 1

        for old, renamed in pairs:
            all_pairs[(old, renamed)] = all_pairs.get((old, renamed), 0) + 1

        for old, renamed in member_pairs:
            all_members[(old, renamed)] = all_members.get((old, renamed), 0) + 1

        if apply:
            io.open(path, 'w', encoding='utf-8', errors='surrogateescape', newline='').write(new)

    if not apply:
        print('== 바뀌는 쌍(빈도)')

        for (old, renamed), count in sorted(all_pairs.items(), key=lambda kv: -kv[1]):
            print('%5d  %s -> %s' % (count, old, renamed))

        print('== 멤버 접근(.x / ->x) 위치의 쌍 — 표준·C 구조체면 SKIP에 넣는다')

        for (old, renamed), count in sorted(all_members.items(), key=lambda kv: -kv[1]):
            print('%5d  .%s -> .%s' % (count, old, renamed))

    print('%s %d곳 / %d파일' % ('바꿈' if apply else '바꿀 예정', total, touched))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
