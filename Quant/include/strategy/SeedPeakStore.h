#pragma once
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
        std::time_t t = std::time(nullptr);
        std::tm tmv{};
#ifdef _WIN32
        localtime_s(&tmv, &t);
#else
        localtime_r(&t, &tmv);
#endif
        char buf[9];
        std::strftime(buf, sizeof(buf), "%Y%m%d", &tmv);
        return buf;
    }

    // 당일 저장분이 있으면 고점, 없거나 날짜가 다르면 0.
    static double load(const std::string& ticker)
    {
        std::lock_guard<std::mutex> lk(mtx());
        nlohmann::json j = read_locked();

        if (j.value("date", "") != today_yyyymmdd())
        {
            return 0.0;
        }

        const auto& peaks = j["peaks"];

        if (!peaks.is_object() || !peaks.contains(ticker))
        {
            return 0.0;
        }

        return peaks[ticker].get<double>();
    }

    // 날짜가 바뀌었으면 어제 표를 통째로 버리고 새로 시작한다.
    static void save(const std::string& ticker, double peak)
    {
        std::lock_guard<std::mutex> lk(mtx());
        nlohmann::json j = read_locked();
        const std::string today = today_yyyymmdd();

        if (j.value("date", "") != today)
        {
            j = nlohmann::json{{"date", today}, {"peaks", nlohmann::json::object()}};
        }

        j["peaks"][ticker] = peak;
        write_locked(j);
    }

    static void erase(const std::string& ticker)
    {
        std::lock_guard<std::mutex> lk(mtx());
        nlohmann::json j = read_locked();

        if (j["peaks"].is_object() && j["peaks"].contains(ticker))
        {
            j["peaks"].erase(ticker);
            write_locked(j);
        }
    }

private:
    static std::mutex& mtx()
    {
        static std::mutex m;
        return m;
    }

    static std::filesystem::path file_path()
    {
        return Logger::instance().base_dir() / "seed_peaks.json";
    }

    static nlohmann::json read_locked()
    {
        nlohmann::json j = nlohmann::json{{"date", ""}, {"peaks", nlohmann::json::object()}};
        std::ifstream in(file_path());

        if (!in)
        {
            return j;
        }

        try
        {
            nlohmann::json parsed = nlohmann::json::parse(in, nullptr, true);

            if (parsed.is_object())
            {
                j["date"] = parsed.value("date", "");
                j["peaks"] = parsed.contains("peaks") && parsed["peaks"].is_object()
                                 ? parsed["peaks"]
                                 : nlohmann::json::object();
            }
        }
        catch (const std::exception& e)
        {
            LOG_WARN(std::string("[SeedPeak] seed_peaks.json 파싱 실패 — 빈 표로 시작: ") + e.what());
        }

        return j;
    }

    static void write_locked(const nlohmann::json& j)
    {
        const auto path = file_path();
        const auto tmp = path.string() + ".tmp";
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);

        {
            std::ofstream out(tmp, std::ios::trunc);

            if (!out)
            {
                LOG_WARN("[SeedPeak] seed_peaks.json 쓰기 실패: " + tmp);
                return;
            }

            out << j.dump();
        }

        std::filesystem::rename(tmp, path, ec);

        if (ec)
        {
            LOG_WARN("[SeedPeak] seed_peaks.json 교체 실패: " + ec.message());
        }
    }
};
