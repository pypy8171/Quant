#pragma once
#include "core/KstTime.h"
#include "utils/Logger.h"
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// SeedPeakStore — 청산 관리(ITB) 시드분의 당일 고점을 재기동 사이에 보존한다.
//
//  시드분 트레일은 부착 이후 고점 기준이라, 재기동마다 고점이 첫 틱으로 다시 잡혀
//  서서히 미끄러지는 종목은 트레일이 한 번도 걸리지 않았다(09-11 465770, 재기동 8회에 −6%).
//  로그 폴더 seed_peaks.json에 {날짜, 종목→고점}을 두고 부착 시 읽어 peak_ 초기값으로 쓴다.
//  날짜가 다르면 무시한다 — 어제 고점으로 오늘 개장 틱에 투매하지 않기 위해서다. [why D-052]
//
//  호출자는 ITB를 도는 샤드 스레드들이다(여럿일 수 있어 mutex로 감싼다). 파일은 프로세스 밖에서도
//  읽히므로 임시파일+rename으로 쓴다.
// ─────────────────────────────────────────────────────────────────────────────
class SeedPeakStore
{
public:
    static std::string today_yyyymmdd()
    {
        return kst::date_yyyymmdd(std::time(nullptr));
    }

    // 당일 저장분이 있으면 고점, 없거나 날짜가 다르면 0.
    static double load(const std::string& ticker);

    // 날짜가 바뀌었으면 어제 표를 통째로 버리고 새로 시작한다.
    static void save(const std::string& ticker, double peak);

    static void erase(const std::string& ticker);

private:
    static std::mutex& mutex();

    static std::filesystem::path file_path()
    {
        return Logger::instance().base_directory() / "seed_peaks.json";
    }

    static nlohmann::json read_locked();

    static void write_locked(const nlohmann::json& document);
};
