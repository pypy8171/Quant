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
//  호출자는 전략 스레드 하나뿐이지만 파일은 프로세스 밖에서도 읽히므로 임시파일+rename으로 쓴다.
// ─────────────────────────────────────────────────────────────────────────────
class SeedPeakStore
{
public:
    static std::string today_yyyymmdd()
    {
        return kst::date_yyyymmdd(std::time(nullptr));
    }

    // 당일 저장분이 있으면 고점, 없거나 날짜가 다르면 0.
    static double load(const std::string& ticker)
    {
        std::lock_guard<std::mutex> lock(mutex());
        nlohmann::json document = read_locked();

        if (document.value("date", "") != today_yyyymmdd())
        {
            return 0.0;
        }

        const auto& peaks = document["peaks"];

        if (!peaks.is_object() || !peaks.contains(ticker))
        {
            return 0.0;
        }

        return peaks[ticker].get<double>();
    }

    // 날짜가 바뀌었으면 어제 표를 통째로 버리고 새로 시작한다.
    static void save(const std::string& ticker, double peak)
    {
        std::lock_guard<std::mutex> lock(mutex());
        nlohmann::json document = read_locked();
        const std::string today = today_yyyymmdd();

        if (document.value("date", "") != today)
        {
            document = nlohmann::json{{"date", today}, {"peaks", nlohmann::json::object()}};
        }

        document["peaks"][ticker] = peak;
        write_locked(document);
    }

    static void erase(const std::string& ticker)
    {
        std::lock_guard<std::mutex> lock(mutex());
        nlohmann::json document = read_locked();

        if (document["peaks"].is_object() && document["peaks"].contains(ticker))
        {
            document["peaks"].erase(ticker);
            write_locked(document);
        }
    }

private:
    static std::mutex& mutex()
    {
        static std::mutex store;
        return store;
    }

    static std::filesystem::path file_path()
    {
        return Logger::instance().base_directory() / "seed_peaks.json";
    }

    static nlohmann::json read_locked()
    {
        nlohmann::json document = nlohmann::json{{"date", ""}, {"peaks", nlohmann::json::object()}};
        std::ifstream in(file_path());

        if (!in)
        {
            return document;
        }

        try
        {
            nlohmann::json parsed = nlohmann::json::parse(in, nullptr, true);

            if (parsed.is_object())
            {
                document["date"] = parsed.value("date", "");
                document["peaks"] = parsed.contains("peaks") && parsed["peaks"].is_object()
                                 ? parsed["peaks"]
                                 : nlohmann::json::object();
            }
        }
        catch (const std::exception& exception)
        {
            LOG_WARN(std::string("[SeedPeak] seed_peaks.json 파싱 실패 — 빈 표로 시작: ") + exception.what());
        }

        return document;
    }

    static void write_locked(const nlohmann::json& document)
    {
        const auto path = file_path();
        const auto temporary = path.string() + ".tmp";
        std::error_code error_code;
        std::filesystem::create_directories(path.parent_path(), error_code);

        {
            std::ofstream out(temporary, std::ios::trunc);

            if (!out)
            {
                LOG_WARN("[SeedPeak] seed_peaks.json 쓰기 실패: " + temporary);
                return;
            }

            out << document.dump();
        }

        std::filesystem::rename(temporary, path, error_code);

        if (error_code)
        {
            LOG_WARN("[SeedPeak] seed_peaks.json 교체 실패: " + error_code.message());
        }
    }
};
