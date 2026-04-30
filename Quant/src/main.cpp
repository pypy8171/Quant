#include "core/Engine.h"
#include "strategy/MACrossStrategy.h"
#include "strategy/MomentumStrategy.h"
#include "utils/Logger.h"
#include <iostream>
#include <csignal>
#include <memory>
#include <fstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// ─── 글로벌 엔진 포인터 (시그널 핸들러용) ─────────────────────────────────
static Engine* g_engine = nullptr;

void signal_handler(int sig) {
    LOG_INFO("\n[Main] 종료 신호 수신 (" + std::to_string(sig) + ")");
    if (g_engine) g_engine->stop();
}

// ─── 설정 파일 로드 ───────────────────────────────────────────────────────
json load_config(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        throw std::runtime_error("설정 파일을 찾을 수 없습니다: " + path);
    }
    return json::parse(f);
}

int main(int argc, char* argv[]) {
    // 로거 초기화
    Logger::instance().init("quant_trader.log", LogLevel::INFO);
#ifdef _WIN32
    LOG_INFO("=== Quant Trader Windows v1.0 ===");
#else
    LOG_INFO("=== Quant Trader Linux v1.0 ===");
#endif

    // 설정 파일 경로
    std::string config_path = (argc > 1) ? argv[1] : "config/config.json";

    // 설정 로드
    json cfg;
    try {
        cfg = load_config(config_path);
        LOG_INFO("[Main] 설정 로드 완료: " + config_path);
    } catch (const std::exception& e) {
        LOG_ERROR(std::string("[Main] 설정 로드 실패: ") + e.what());
        return 1;
    }

    // KIS 설정
    KisConfig kis_cfg;
    kis_cfg.app_key      = cfg["kis"]["app_key"];
    kis_cfg.app_secret   = cfg["kis"]["app_secret"];
    kis_cfg.account_no   = cfg["kis"]["account_no"];
    kis_cfg.account_type = cfg["kis"]["account_type"].get<std::string>();
    kis_cfg.is_paper     = cfg["kis"]["is_paper"].get<bool>();

    // 종목 목록
    std::vector<std::string> tickers = cfg["tickers"].get<std::vector<std::string>>();

    // 수집 주기
    int interval = cfg.value("fetch_interval_sec", 60);

    // 엔진 생성
    Engine engine(kis_cfg, tickers, interval);
    g_engine = &engine;

    // 시그널 핸들러 등록 (Ctrl+C, kill)
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    // 전략 등록
    for (auto& s : cfg["strategies"]) {
        std::string type   = s["type"];
        std::string ticker = s["ticker"];
        int qty            = s["quantity"];

        if (type == "MA_CROSS") {
            engine.add_strategy(std::make_unique<MACrossStrategy>(
                ticker,
                s["short_period"].get<int>(),
                s["long_period"].get<int>(),
                qty
            ));
        } else if (type == "MOMENTUM") {
            engine.add_strategy(std::make_unique<MomentumStrategy>(
                ticker,
                s["period"].get<int>(),
                qty
            ));
        } else {
            LOG_WARN("[Main] 알 수 없는 전략 타입: " + type);
        }
    }

    // 엔진 시작
    engine.start();

    // 메인 스레드는 엔진이 실행 중인 동안 대기
    while (engine.is_running()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    LOG_INFO("[Main] 프로그램 종료");
    return 0;
}
