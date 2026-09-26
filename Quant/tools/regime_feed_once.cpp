// 국면 판정 한 사이클만 돌리는 도구 — 엔진 안 피드(regime/RegimeFeed.h)를 파이썬 원본과 같은 시각에 돌려 대조한다.
//  사용: regime_feed_once <출력 regime.json> [이력 jsonl] [장초 기준점 json]   (repo 루트에서) [why D-147]
#include "regime/RegimeFeed.h"
#include "utils/Logger.h"

#include <cstdio>

int main(int argument_count, char** arguments)
{
    if (argument_count < 2)
    {
        std::fprintf(stderr, "사용: regime_feed_once <출력 regime.json> [이력 jsonl] [장초 기준점 json]\n");
        return 2;
    }

    regime_feed::FeedConfig config;
    config.out_path            = arguments[1];
    config.history_path        = argument_count > 2 ? arguments[2] : "";
    config.open_reference_path = argument_count > 3 ? arguments[3] : "logs/regime_open_ref.json";

    regime_feed::RegimeFeed feed;
    feed.run_once(config);
    return 0;
}
