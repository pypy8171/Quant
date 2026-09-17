// api/KisAuth.cpp — OAuth2 토큰 발급·캐시·만료 전 재발급. access_token_은 token_mutex_ 아래에서만 읽고 쓰고,
//  발급 HTTP 왕복은 refresh_mtx_만 쥔 채 돈다. [why D-073]
//  [why D-048] 파일 분할 경위.
#include "KisClientInternal.h"
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h> // MoveFileExA — 토큰 캐시 원자 교체
#endif

// ═══════════════════════════════════════════════════════════════════════════
//  KisClient 구현
// ═══════════════════════════════════════════════════════════════════════════

KisClient::KisClient(const KisConfig& config) : config_(config)
{
#ifndef _WIN32
    curl_global_init(CURL_GLOBAL_DEFAULT);
#endif
}

KisClient::~KisClient()
{
#ifndef _WIN32
    curl_global_cleanup();
#endif
}

// 토큰 캐시 파일: app_key 앞 8자리로 구분 (실계좌/모의 혼용 방지).
// KIS_TOKEN_CACHE_DIR(docker 공유 볼륨) 설정 시 그 경로에 저장 → Python balance가 재사용.
static std::string token_cache_path(const std::string& app_key)
{
    const char* directory = std::getenv("KIS_TOKEN_CACHE_DIR");
    std::string prefix = (directory && *directory) ? std::string(directory) + "/" : "";
    return prefix + "kis_token_" + app_key.substr(0, 8) + ".json";
}

bool KisClient::token_expiring(std::chrono::seconds margin) const
{
    std::lock_guard<std::mutex> lock(token_mutex_);
    return access_token_.empty() || std::chrono::system_clock::now() + margin >= token_expires_at_;
}

void KisClient::set_token(std::string token, std::chrono::system_clock::time_point expires_at)
{
    std::lock_guard<std::mutex> lock(token_mutex_);
    access_token_     = std::move(token);
    token_expires_at_ = expires_at;
}

void KisClient::ensure_authenticated()
{
    if (!token_expiring(std::chrono::minutes(5)))
    {
        return;
    }

    LOG_INFO("[KIS] 토큰 갱신 시작");
    refresh_token(std::chrono::minutes(5));
}

bool KisClient::refresh_token(std::chrono::seconds margin)
{
    // [lock-order] refresh_mutex_ → token_mutex_. 발급은 refresh_mtx_가 직렬화하고, 안에서 만료를 다시 봐
    //  만료창에 같이 들어온 스레드는 첫 발급 뒤 건너뛴다. HTTP 왕복(수백 milliseconds~수 초) 동안 token_mtx_는
    //  잡지 않으므로 다른 스레드의 token()·헤더 조립은 옛 토큰으로 바로 나간다 — 옛 토큰은 margin 안까지
    //  유효하다. 예전엔 token_mtx_를 왕복 내내 쥐어 그 사이 전략·데이터·주문 스레드가 전부 섰다. [why D-073]
    std::lock_guard<std::mutex> refresh_lock(refresh_mutex_);

    if (!token_expiring(margin))
    {
        return true;
    }

    return issue_token();
}

bool KisClient::authenticate()
{
    std::lock_guard<std::mutex> refresh_lock(refresh_mutex_);
    return issue_token();
}

// refresh_mtx_를 쥔 상태에서만 호출. token_mtx_는 set_token 안에서만 잠깐 잡는다.
bool KisClient::issue_token()
{
    // ── 캐시 파일에 유효한 토큰이 있으면 재사용 ──────────────────────────
    std::string cache_path = token_cache_path(config_.app_key);
    {
        std::ifstream file(cache_path);

        if (file.is_open())
        {
            try
            {
                auto document = json::parse(file);
                std::string token = document.value("access_token", "");
                std::string expires = document.value("expires_at", ""); // ISO "YYYY-MM-DD HH:MM:SS"

                if (!token.empty() && !expires.empty())
                {
                    // 만료 시각 파싱
                    struct tm tm_exp
                    {
                    };
                    int year, month, day, hour, minute, second;

                    if (sscanf(expires.c_str(), "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second) == 6)
                    {
                        tm_exp.tm_year = year - 1900;
                        tm_exp.tm_mon = month - 1;
                        tm_exp.tm_mday = day;
                        tm_exp.tm_hour = hour;
                        tm_exp.tm_min = minute;
                        tm_exp.tm_sec = second;
                        tm_exp.tm_isdst = -1;
                        auto exp_t = std::mktime(&tm_exp);
                        auto now_t = std::time(nullptr);

                        // 만료 10분 전까지 사용
                        if (exp_t - now_t > 600)
                        {
                            set_token(token, std::chrono::system_clock::from_time_t(exp_t));
                            LOG_INFO("[KIS] 캐시 토큰 재사용 (만료: " + expires + ")");
                            return true;
                        }
                    }
                }
            }
            catch (...)
            {
            }
        }
    }

    // ── 새 토큰 발급 ─────────────────────────────────────────────────────
    json body = {{"grant_type", "client_credentials"}, {"appkey", config_.app_key}, {"appsecret", config_.app_secret}};

    std::string url = base_url() + "/oauth2/tokenP";
    std::string response = http_post(url, {"Content-Type: application/json"}, body.dump());

    if (response.empty())
    {
        LOG_ERROR("[KIS] 토큰 발급 요청 실패");
        return false;
    }

    try
    {
        auto document = json::parse(response);
        const std::string token = document["access_token"].get<std::string>();

        // 만료 시각 저장 (KIS 응답 필드: access_token_token_expired)
        std::string expires = document.value("access_token_token_expired", "");
        std::chrono::system_clock::time_point expires_at;

        // 인메모리 만료 시각 설정
        {
            struct tm tm_exp
            {
            };
            int year, month, day, hour, minute, second;

            if (sscanf(expires.c_str(), "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second) == 6)
            {
                tm_exp.tm_year = year - 1900;
                tm_exp.tm_mon = month - 1;
                tm_exp.tm_mday = day;
                tm_exp.tm_hour = hour;
                tm_exp.tm_min = minute;
                tm_exp.tm_sec = second;
                tm_exp.tm_isdst = -1;
                expires_at = std::chrono::system_clock::from_time_t(std::mktime(&tm_exp));
            }
            else
            {
                // 파싱 실패 시 24시간 후로 설정
                expires_at = std::chrono::system_clock::now() + std::chrono::hours(24);
            }
        }

        set_token(token, expires_at);

        // 캐시 파일에 저장 — temp 작성 후 atomic rename (C-2).
        // 읽는 쪽(Python balance)이 스트리밍 중인 truncated JSON을 보지 않게 한다.
        json cache_j = {{"access_token", token}, {"expires_at", expires}};
        std::string temporary_path = cache_path + ".tmp";
        {
            std::ofstream cf(temporary_path);

            if (cf.is_open())
            {
                cf << cache_j.dump(2);
            }
        }
#ifdef _WIN32
        MoveFileExA(temporary_path.c_str(), cache_path.c_str(), MOVEFILE_REPLACE_EXISTING);
#else
        ::chmod(temporary_path.c_str(), S_IRUSR | S_IWUSR); // 0600 — 공유 볼륨 평문 토큰 보호 (W-4)
        // rename은 원자적이나 durability는 보장 안 함 — 크래시 시 0바이트 캐시→재발급(403).
        // tmp를 fsync해 데이터를 디스크에 내린 뒤 rename (V-2).
        {
            int descriptor = ::open(temporary_path.c_str(), O_RDONLY);

            if (descriptor >= 0) { ::fsync(descriptor); ::close(descriptor); }
        }

        std::rename(temporary_path.c_str(), cache_path.c_str()); // POSIX atomic
#endif

        LOG_INFO("[KIS] 토큰 발급 성공 (만료: " + expires + ")");
        return true;
    }
    catch (const std::exception& exception)
    {
        LOG_ERROR(std::string("[KIS] 토큰 파싱 오류: ") + exception.what());
        return false;
    }
}
