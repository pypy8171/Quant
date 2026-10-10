#include "strategy/SeedPeakStore.h"

#include <cstdint>
#include <map>

namespace
{
// 프로세스 하나에 표 하나. 파일 경로가 로그 폴더 하나에 묶여 있어 인스턴스를 나눌 일이 없다.
struct PeakTable
{
    std::mutex                    mutex;
    bool                          loaded = false;          // 파일을 한 번 읽었는가
    std::string                   date;                    // 표의 날짜(YYYYMMDD). 비면 아직 없음
    std::map<std::string, double> peaks;                   // 종목 → 당일 고점
    uint64_t                      version = 0;             // 표가 바뀔 때마다 +1
    uint64_t                      written_version = 0;     // 파일에 닿은 마지막 version
    bool                          writing = false;         // [inv] true인 동안 파일에 쓰는 스레드는 그 하나뿐이다
    std::function<void()>         before_write;            // 시험 전용 걸쇠
};

PeakTable g_table;

struct PeakFile
{
    std::string                   date;
    std::map<std::string, double> peaks;
};

// 락 밖에서 부른다. 파일이 없거나 깨졌으면 빈 표.
PeakFile read_file(const std::filesystem::path& path)
{
    PeakFile      result;
    std::ifstream in(path);

    if (!in)
    {
        return result;
    }

    try
    {
        const nlohmann::json parsed = nlohmann::json::parse(in, nullptr, true);

        if (!parsed.is_object())
        {
            return result;
        }

        result.date = parsed.value("date", "");
        const auto found = parsed.find("peaks");

        if (found == parsed.end() || !found->is_object())
        {
            return result;
        }

        for (const auto& [ticker, peak] : found->items())
        {
            if (peak.is_number())
            {
                result.peaks[ticker] = peak.get<double>();
            }
        }
    }
    catch (const std::exception& exception)
    {
        LOG_WARN(std::string("[SeedPeak] seed_peaks.json 파싱 실패 — 빈 표로 시작: ") + exception.what());
        result = PeakFile{};
    }

    return result;
}

// 락 밖에서 부른다. 성공하면 true.
bool write_file(const std::filesystem::path& path, const PeakFile& snapshot)
{
    nlohmann::json document = nlohmann::json{{"date", snapshot.date}, {"peaks", nlohmann::json::object()}};

    for (const auto& [ticker, peak] : snapshot.peaks)
    {
        document["peaks"][ticker] = peak;
    }

    const auto      temporary = path.string() + ".tmp";
    std::error_code error_code;
    std::filesystem::create_directories(path.parent_path(), error_code);

    {
        std::ofstream out(temporary, std::ios::trunc);

        if (!out)
        {
            LOG_WARN("[SeedPeak] seed_peaks.json 쓰기 실패 - path(" + temporary + ")");
            return false;
        }

        out << document.dump();
    }

    std::filesystem::rename(temporary, path, error_code);

    if (error_code)
    {
        LOG_WARN("[SeedPeak] seed_peaks.json 교체 실패 - error(" + error_code.message() + ")");
        return false;
    }

    return true;
}
}

double SeedPeakStore::load(const std::string& ticker)
{
    ensure_loaded();
    const std::string           today = today_yyyymmdd();
    std::lock_guard<std::mutex> lock(g_table.mutex);

    if (g_table.date != today)
    {
        return 0.0;
    }

    const auto found = g_table.peaks.find(ticker);
    return found == g_table.peaks.end() ? 0.0 : found->second;
}

void SeedPeakStore::save(const std::string& ticker, double peak)
{
    ensure_loaded();
    const std::string today = today_yyyymmdd();

    {
        std::lock_guard<std::mutex> lock(g_table.mutex);

        if (g_table.date != today)
        {
            g_table.date = today;
            g_table.peaks.clear();
        }

        g_table.peaks[ticker] = peak;
        ++g_table.version;
    }

    flush_if_idle();
}

void SeedPeakStore::erase(const std::string& ticker)
{
    ensure_loaded();
    bool changed = false;

    {
        std::lock_guard<std::mutex> lock(g_table.mutex);
        changed = g_table.peaks.erase(ticker) > 0;

        if (changed)
        {
            ++g_table.version;
        }
    }

    if (changed)
    {
        flush_if_idle();
    }
}

void SeedPeakStore::set_before_write_for_test(std::function<void()> hook)
{
    std::lock_guard<std::mutex> lock(g_table.mutex);
    g_table.before_write = std::move(hook);
}

void SeedPeakStore::reset_for_test()
{
    std::lock_guard<std::mutex> lock(g_table.mutex);
    g_table.loaded          = false;
    g_table.date.clear();
    g_table.peaks.clear();
    g_table.version         = 0;
    g_table.written_version = 0;
}

void SeedPeakStore::ensure_loaded()
{
    {
        std::lock_guard<std::mutex> lock(g_table.mutex);

        if (g_table.loaded)
        {
            return;
        }
    }

    // 파일은 락 밖에서 읽는다. 둘이 같이 읽어도 먼저 넣은 쪽만 쓰이고 뒤엣것은 버린다.
    PeakFile                    file = read_file(file_path());
    std::lock_guard<std::mutex> lock(g_table.mutex);

    if (g_table.loaded)
    {
        return;
    }

    g_table.date   = std::move(file.date);
    g_table.peaks  = std::move(file.peaks);
    g_table.loaded = true;
}

void SeedPeakStore::flush_if_idle()
{
    PeakFile              snapshot;
    uint64_t              snapshot_version = 0;
    std::function<void()> before_write;

    {
        std::lock_guard<std::mutex> lock(g_table.mutex);

        if (g_table.writing || g_table.written_version == g_table.version)
        {
            return; // 쓰는 스레드가 이 변경까지 이어서 쓴다
        }

        g_table.writing  = true;
        snapshot.date    = g_table.date;
        snapshot.peaks   = g_table.peaks;
        snapshot_version = g_table.version;
        before_write     = g_table.before_write;
    }

    const std::filesystem::path path = file_path();

    while (true)
    {
        if (before_write)
        {
            before_write();
        }

        const bool written = write_file(path, snapshot);

        std::lock_guard<std::mutex> lock(g_table.mutex);

        // 실패하면 다음 save가 다시 쓴다. 같은 실패를 여기서 되풀이하지 않는다.
        if (!written || g_table.version == snapshot_version)
        {
            if (written)
            {
                g_table.written_version = snapshot_version;
            }

            g_table.writing = false;
            return;
        }

        g_table.written_version = snapshot_version;
        snapshot.date           = g_table.date;
        snapshot.peaks          = g_table.peaks;
        snapshot_version        = g_table.version;
        before_write            = g_table.before_write;
    }
}
