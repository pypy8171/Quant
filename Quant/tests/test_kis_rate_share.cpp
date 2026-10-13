// KisClient 초당 한도 버킷 공유 시험. 같은 (앱키, 모의 여부)로 만든 클라이언트는 생성자에서 한 버킷을 받고,
//  키나 도메인이 다르면 따로 받는다. 몫(rest_calls_per_second)이 다른 두 설정이 한 키를 쓰면 작은 몫을 따른다.
//  네트워크는 쓰지 않는다 — 생성자와 버킷 조회만 본다.
// 빌드: cmake --build <directory> --target test_kis_rate_share
#include "api/KisClient.h"

#include <iostream>

namespace
{
int g_checks = 0;

#define CHECK(condition)                                                                        \
    do                                                                                          \
    {                                                                                           \
        ++g_checks;                                                                             \
        if (!(condition))                                                                       \
        {                                                                                       \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #condition << "\n";     \
            return 1;                                                                           \
        }                                                                                       \
    } while (0)

KisConfig make_config(const char* app_key, bool is_paper, double calls_per_second)
{
    KisConfig config;
    config.app_key               = app_key;
    config.app_secret            = "secret";
    config.account_no            = "00000000";
    config.account_type          = "01";
    config.is_paper              = is_paper;
    config.rest_calls_per_second = calls_per_second;
    return config;
}
} // namespace

int main()
{
    // 1. 같은 키·같은 도메인 — 기동 코드가 묶지 않아도 한 버킷이다.
    {
        KisClient order_client(make_config("key-a", false, 0.0));
        KisClient quote_client(make_config("key-a", false, 0.0));
        CHECK(order_client.shares_rate_bucket_with(quote_client));
        CHECK(order_client.rate_plan().published_limit == 20.0);
        CHECK(quote_client.share_rate_limit_with(order_client)); // 옛 호출도 그대로 참
    }

    // 2. 키가 다르거나 도메인이 다르면 따로 센다.
    {
        KisClient live_client(make_config("key-b", false, 0.0));
        KisClient paper_client(make_config("key-b", true, 0.0));
        KisClient other_client(make_config("key-c", false, 0.0));
        CHECK(!live_client.shares_rate_bucket_with(paper_client));
        CHECK(!live_client.shares_rate_bucket_with(other_client));
        CHECK(paper_client.rate_plan().published_limit == 2.0);
    }

    // 3. 몫 — 한 키를 몫 14와 몫 6으로 쓰면 작은 6을 따른다(뒤에 만든 쪽이 작아도, 앞에 만든 쪽이 작아도).
    {
        KisClient wide_client(make_config("key-d", false, 14.0));
        CHECK(wide_client.rate_plan().published_limit == 14.0);
        CHECK(wide_client.rate_plan().capacity == 3.5);

        KisClient narrow_client(make_config("key-d", false, 6.0));
        CHECK(wide_client.shares_rate_bucket_with(narrow_client));
        CHECK(wide_client.rate_plan().published_limit == 6.0);

        KisClient default_client(make_config("key-d", false, 0.0)); // 칸 없는 설정이 몫을 다시 넓히지 않는다
        CHECK(default_client.rate_plan().published_limit == 6.0);
    }

    // 4. 클라이언트가 다 사라지면 버킷도 사라지고, 같은 키의 다음 클라이언트는 새 몫으로 시작한다.
    {
        KisClient fresh_client(make_config("key-d", false, 0.0));
        CHECK(fresh_client.rate_plan().published_limit == 20.0);
    }

    std::cout << "test_kis_rate_share OK (" << g_checks << " checks)\n";
    return 0;
}
