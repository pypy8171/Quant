#pragma once
// 프로세스 설정 한 벌 — config.json을 typed 값으로 옮긴 것. json을 읽는 곳은 parse_config() 하나뿐이고,
//  main()·Engine 세터·모니터 모드는 여기 값만 받는다. "config.json의 키 X가 어디에 쓰이나"는 이 파일과
//  AppConfig.cpp 두 곳에서 끝난다. 전략별 파라미터(`strategies` 배열)만 예외 — 타입마다 키가 달라
//  strategy/StrategyFactory.cpp가 자기 몫을 읽는다.
#include "api/KisClient.h"
#include "core/RegimeFileJudge.h"
#include "core/Types.h"
#include "risk/OrderGate.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

struct AppConfig
{
    // ── 공통 ──
    std::string mode = "FEED"; // FEED / KR_TEST / US_TEST / TRADE (Mode::from_string)
    bool        debug_log = false; // log_level == "DEBUG"
    KisConfig   kis;               // 주문·잔고·기본 피드 키

    // ── 관찰 모드(FEED) ──
    std::vector<std::string> tickers; // 구독 종목. TRADE는 전략이 동적으로 구성하므로 안 쓴다
    std::vector<std::string> futures; // 국내 선물 실시간(H0IFCNT0/H0IFASP0). 실계좌 WS 도메인 전용

    // ── TRADE: 엔진 채널 ──
    int                    fetch_interval_sec = 60;
    bool                   bootstrap_ledger_from_balance = false;
    bool                   rest_price_feed = false;
    std::string            capture_directory;
    // 원장 저널 폴더(ledger_YYYYMMDD.bin). capture_directory와 독립 — 틱 캡처를 안 켜도 이건 켠다(저장량이 틱의
    //  몇 만분의 1). 빈 문자열=끔(테스트·벤치만). fsync=append마다 디스크 동기화. [why D-113]
    std::string            ledger_journal_directory;
    bool                   ledger_journal_fsync = false;
    unsigned               strategy_shards = 1;
    std::vector<KisConfig> feed_keys; // 추가 WS 세션 키(D-071 원칙 1). 기본 키와 계좌·모의 여부 같고 hts_id 없음
    std::string            replay_file;  // 비어 있지 않으면 캡처 파일 리플레이(D-071 원칙 8)
    double                 replay_speed = 1.0;
    double                 replay_cash = 100'000'000.0;
    std::string            regime_file;
    int                    regime_stale_sec = kDefaultRegimeStaleSec;
    int                    regime_halt_expire_min = kDefaultRegimeHaltExpireMin;
    std::string            zmq_bind_address;
    std::string            zmq_control_token;
    // 한 기계에 엔진이 둘 이상 뜨면 포트가 겹쳐 뒤에 뜬 쪽이 ZMQ 없이 돈다 — 그래서 설정으로 뺀다.
    int                    zmq_pub_port = 5555; // 시세·주문 발행(PUB)
    int                    zmq_rep_port = 5556; // 제어 명령(REP)
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

// config.json 문서 → AppConfig. mode_override가 비어 있지 않으면 문서의 "mode"를 덮는다.
//  값이 틀리면(예: kis.exchange가 KRX/NXT/SOR 밖) std::runtime_error — 네트워크를 건드리기 전에 기동이 멈춘다.
AppConfig parse_config(const nlohmann::json& document, const std::string& mode_override);

// 파일 경로 → json 문서. 없으면 std::runtime_error.
nlohmann::json load_config_file(const std::string& path);
