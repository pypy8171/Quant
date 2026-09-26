#pragma once
// 프로세스 설정 한 벌 — config.json을 typed 값으로 옮긴 것. json을 읽는 곳은 parse_config() 하나뿐이고,
//  main()·Engine 세터는 여기 값만 받는다. "config.json의 키 X가 어디에 쓰이나"는 이 파일과
//  AppConfig.cpp 두 곳에서 끝난다. 전략별 파라미터(`strategies` 배열)만 예외 — 타입마다 키가 달라
//  strategy/StrategyFactory.cpp가 자기 몫을 읽는다.
#include "api/KisClient.h"
#include "core/RegimeFileJudge.h"
#include "core/Types.h"
#include "regime/RegimeFeed.h"
#include "risk/OrderGate.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

struct AppConfig
{
    // ── 공통 ──
    bool        debug_log = false; // log_level == "DEBUG"
    KisConfig   kis;               // 주문·잔고·기본 피드 키

    // ── TRADE: 엔진 채널 ──
    int                    fetch_interval_sec = 60;
    bool                   bootstrap_ledger_from_balance = false;
    bool                   rest_price_feed = false;
    std::string            capture_directory;
    // 캡처에 담을 종목(비면 구독 종목 전부). 전 종목 호가까지 남기면 하루 GB 단위라 확인용으로 좁힌다. [why D-138]
    std::vector<std::string> capture_tickers;
    // 전략이 안 봐도 WS 칸을 늘 쥐는 종목(체결만). 엔진이 실제로 받는 체결 수를 캡처로 세려고 둔다. [why D-138]
    std::vector<std::string> websocket_pin_tickers;
    // 원장 저널 폴더(ledger_YYYYMMDD.bin). capture_directory와 독립 — 틱 캡처를 안 켜도 이건 켠다(저장량이 틱의
    //  몇 만분의 1). 빈 문자열=끔(테스트·벤치만). fsync=append마다 디스크 동기화. [why D-113]
    std::string            ledger_journal_directory;
    bool                   ledger_journal_fsync = false;
    unsigned               strategy_shards = 1;
    std::vector<KisConfig> feed_keys; // 추가 WS 세션 키(D-071 원칙 1). 기본 키와 계좌·모의 여부 같고 hts_id 없음
    std::string            replay_file;  // 비어 있지 않으면 캡처 파일 리플레이(D-071 원칙 8)
    double                 replay_speed = 1.0;
    double                 replay_cash = 100'000'000.0;
    // 부하시험 주문 수신단(exchange::ZmqOrderFeed). 켜면 WS·KIS 대신 바깥 인젝터가 보낸 가상 주문을 오더북에
    //  넣고, 거기서 난 체결을 시세로 올린다 — KIS로는 아무것도 나가지 않는다. 밤에 돌리므로 장 시간 창은
    //  리플레이와 같이 끈다. 현금은 replay_cash 를 같이 쓴다. [why D-071]
    bool                   load_test_enabled = false;
    unsigned               load_test_lanes = 1;            // 수신 스레드 = 소켓 수. 종목을 순번으로 나눠 맡는다
    int                    load_test_base_port = 5600;     // 수신 스레드 i 는 base_port + i 를 연다
    std::string            load_test_bind_address = "tcp://127.0.0.1";
    int                    load_test_session_hhmmss = 0; // 체결에 찍을 장중 시각 시작점. 0이면 실제 시계
    // 기동 때 종목 순번표를 적을 파일. 인젝터가 이것을 읽어 순번을 맞춘다. 비면 안 적는다
    std::string            load_test_universe_out;
    std::string            regime_file;
    int                    regime_stale_sec = kDefaultRegimeStaleSec;
    int                    regime_halt_expire_min = kDefaultRegimeHaltExpireMin;
    // 국면 판정 피드(regime/RegimeFeed.h). config에 "regime_feed" 노드가 있을 때만 전략 쪽이 띄운다. [why D-147]
    std::optional<regime_feed::FeedConfig> regime_feed;
    // 보호 주문 표(D-114 단계 1) — off/shadow/owner. 기본 shadow는 판정만 로그로 남기고 발주는 전략이 한다.
    //  owner로 두면 표가 발주하고 등록한 전략은 자기 손절·트레일 판정을 건너뛰다.
    std::string            protective_orders = "shadow";
    int                    protective_orders_interval_ms = 200;  // 주문 쪽이 표를 보는 간격
    int                    protective_orders_retry_ms = 30000;   // 청산이 안 먹힐 때 다시 내는 간격
    std::string            zmq_bind_address;
    std::string            zmq_control_token;
    // 한 기계에 엔진이 둘 이상 뜨면 포트가 겹쳐 뒤에 뜬 쪽이 ZMQ 없이 돈다 — 그래서 설정으로 뺀다.
    int                    zmq_pub_port = 5555; // 주문 프로세스 발행(PUB)
    int                    zmq_rep_port = 5556; // 제어 명령(REP) — 주문 프로세스에만 있다
    // 프로세스를 셋으로 가르면 역할마다 발행 포트를 따로 연다. 0이면 zmq_pub_port에서 +2·+3으로 잡는다 —
    //  실계좌 config가 5565를 쓰면 5567·5568이 되어 모의계좌(5557·5558)와 겹치지 않는다. [why D-114]
    int                    zmq_feed_pub_port = 0;     // 시세 프로세스 발행(PUB)
    int                    zmq_strategy_pub_port = 0; // 전략 프로세스 발행(PUB)
    // 한 기계에서 계좌를 둘 이상 돌릴 때 프로세스를 가르는 이름(예: "live"). 마감 표지 파일과
    //  감시견 상태 파일 이름에 붙는다 — 비어 있으면 예전과 같은 이름을 쓴다. [why D-122]
    std::string            instance;
    std::string            ops_bind_address;
    int                    ops_port = 0;
    std::string            ops_token;

    // ── TRADE: 국면→전략 자동선택(G1). 비어 있으면 per-strategy active_regimes 방식 유지 ──
    bool                                       has_regime_strategies = false;
    std::map<Regime, std::vector<std::string>> regime_strategies;

    // ── TRADE: 시세 전용(실전 도메인) 키. 모의 시세 REST가 HTTP 500이라 시세만 실전으로 ──
    std::optional<KisConfig> quote_kis;

    // ── TRADE: 위험 한도·주문 호출 간격. risk 노드가 없어도 매매 창은 채운다 ──
    bool              has_risk = false; // false면 발주 간격은 Engine 기본값 그대로
    OrderGate::Config risk;
    int               order_min_interval_ms = 350;
    int               order_max_retries = 3;
    int               session_end_grace_sec = 120; // 마지막 매매 창이 닫힌 뒤 엔진이 스스로 내려가기까지 기다리는 초 [why D-098]

    // ── TRADE: 전략 배열 — StrategyFactory가 타입별로 읽는다 ──
    nlohmann::json strategies = nlohmann::json::array();
};

// config.json 문서 → AppConfig.
//  값이 틀리면(예: kis.exchange가 KRX/NXT/SOR 밖) std::runtime_error — 네트워크를 건드리기 전에 기동이 멈춘다.
AppConfig parse_config(const nlohmann::json& document);

// 파일 경로 → json 문서. 없으면 std::runtime_error.
nlohmann::json load_config_file(const std::string& path);
