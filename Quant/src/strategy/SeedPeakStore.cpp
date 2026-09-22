#include "strategy/SeedPeakStore.h"

double SeedPeakStore::load(const std::string& ticker)
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

void SeedPeakStore::save(const std::string& ticker, double peak)
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

void SeedPeakStore::erase(const std::string& ticker)
{
    std::lock_guard<std::mutex> lock(mutex());
    nlohmann::json document = read_locked();

    if (document["peaks"].is_object() && document["peaks"].contains(ticker))
    {
        document["peaks"].erase(ticker);
        write_locked(document);
    }
}

std::mutex& SeedPeakStore::mutex()
{
    static std::mutex store;
    return store;
}

nlohmann::json SeedPeakStore::read_locked()
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
                                    ? std::move(parsed["peaks"]) // parsed는 여기서 버려지므로 옮긴다
                                    : nlohmann::json::object();
        }
    }
    catch (const std::exception& exception)
    {
        LOG_WARN(std::string("[SeedPeak] seed_peaks.json 파싱 실패 — 빈 표로 시작: ") + exception.what());
    }

    return document;
}

void SeedPeakStore::write_locked(const nlohmann::json& document)
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
