// bench_feed_ingest 공용 준비의 몸통 — 종목 코드 읽기.
#include "bench_feed_ingest_support.h"

// ─────────────────────────────────────────────────────────────────────────────
// 티커 로드 (universe_full.json 코드배열 → 없으면 합성 6자리) — firehose와 동일 규약
// ─────────────────────────────────────────────────────────────────────────────
std::vector<std::string> load_universe(const std::string& path, int fallback_n)
{
    std::vector<std::string> out;

    if (!path.empty())
    {
        std::ifstream file(path);

        if (file)
        {
            std::string body((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            size_t start = body.find("\"codes\"");
            std::string scan = (start != std::string::npos) ? body.substr(start) : body;

            for (size_t index = 0; index + 1 < scan.size(); ++index)
            {
                if (scan[index] != '"')
                {
                    continue;
                }

                size_t inner_index = index + 1, digits = 0;

                while (inner_index < scan.size() && scan[inner_index] >= '0' && scan[inner_index] <= '9')
                {
                    ++inner_index;
                    ++digits;
                }

                if (digits == 6 && inner_index < scan.size() && scan[inner_index] == '"')
                {
                    out.emplace_back(scan.substr(index + 1, 6));
                    index = inner_index;
                }
            }
        }
    }

    if (out.empty())
    {
        out.reserve(fallback_n);

        for (int fallback_index = 1; fallback_index <= fallback_n; ++fallback_index)
        {
            char buffer[8];
            std::snprintf(buffer, sizeof(buffer), "%06d", fallback_index);
            out.emplace_back(buffer);
        }
    }

    return out;
}
