// api/KisAuth.cpp — OAuth2 토큰 발급·캐시·만료 전 재발급. token_mtx_ 아래에서만 access_token_을 쓴다.
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

KisClient::KisClient(const KisConfig& cfg) : cfg_(cfg)
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
    const char* dir = std::getenv("KIS_TOKEN_CACHE_DIR");
    std::string prefix = (dir && *dir) ? std::string(dir) + "/" : "";
    return prefix + "kis_token_" + app_key.substr(0, 8) + ".json";
}

void KisClient::ensure_authenticated()
{
    // 락을 먼저 잡고 만료를 재확인(double-checked) → 여러 스레드가 만료창에 동시 진입해도
    // 첫 스레드만 발급하고 나머지는 갱신된 만료시각을 보고 건너뜀(thundering-herd 제거).
    std::lock_guard<std::mutex> lk(token_mtx_);
    auto now = std::chrono::system_clock::now();
    auto margin = std::chrono::minutes(5);

    if (access_token_.empty() || now + margin >= token_expires_at_)
    {
        LOG_INFO("[KIS] 토큰 갱신 시작");
        authenticate_locked();
    }
}

// public 진입점 — 기동 시 직접 호출(단일 스레드)에도 안전하도록 락을 잡고 위임.
bool KisClient::authenticate()
{
    std::lock_guard<std::mutex> lk(token_mtx_);
    return authenticate_locked();
}

// token_mtx_를 이미 쥔 상태에서만 호출 — 내부에서 다시 락을 잡지 않는다(비재귀 뮤텍스).
bool KisClient::authenticate_locked()
{
    // ── 캐시 파일에 유효한 토큰이 있으면 재사용 ──────────────────────────
    std::string cache_path = token_cache_path(cfg_.app_key);
    {
        std::ifstream f(cache_path);

        if (f.is_open())
        {
            try
            {
                auto j = json::parse(f);
                std::string token = j.value("access_token", "");
                std::string expires = j.value("expires_at", ""); // ISO "YYYY-MM-DD HH:MM:SS"

                if (!token.empty() && !expires.empty())
                {
                    // 만료 시각 파싱
                    struct tm tm_exp
                    {
                    };
                    int y, mo, d, h, mi, s;

                    if (sscanf(expires.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) == 6)
                    {
                        tm_exp.tm_year = y - 1900;
                        tm_exp.tm_mon = mo - 1;
                        tm_exp.tm_mday = d;
                        tm_exp.tm_hour = h;
                        tm_exp.tm_min = mi;
                        tm_exp.tm_sec = s;
                        tm_exp.tm_isdst = -1;
                        auto exp_t = std::mktime(&tm_exp);
                        auto now_t = std::time(nullptr);

                        // 만료 10분 전까지 사용
                        if (exp_t - now_t > 600)
                        {
                            access_token_ = token;
                            token_expires_at_ = std::chrono::system_clock::from_time_t(exp_t);
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
    json body = {{"grant_type", "client_credentials"}, {"appkey", cfg_.app_key}, {"appsecret", cfg_.app_secret}};

    std::string url = base_url() + "/oauth2/tokenP";
    std::string resp = http_post(url, {"Content-Type: application/json"}, body.dump());

    if (resp.empty())
    {
        LOG_ERROR("[KIS] 토큰 발급 요청 실패");
        return false;
    }

    try
    {
        auto j = json::parse(resp);
        access_token_ = j["access_token"].get<std::string>();

        // 만료 시각 저장 (KIS 응답 필드: access_token_token_expired)
        std::string expires = j.value("access_token_token_expired", "");

        // 인메모리 만료 시각 설정
        {
            struct tm tm_exp
            {
            };
            int y, mo, d, h, mi, s;

            if (sscanf(expires.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) == 6)
            {
                tm_exp.tm_year = y - 1900;
                tm_exp.tm_mon = mo - 1;
                tm_exp.tm_mday = d;
                tm_exp.tm_hour = h;
                tm_exp.tm_min = mi;
                tm_exp.tm_sec = s;
                tm_exp.tm_isdst = -1;
                token_expires_at_ = std::chrono::system_clock::from_time_t(std::mktime(&tm_exp));
            }
            else
            {
                // 파싱 실패 시 24시간 후로 설정
                token_expires_at_ = std::chrono::system_clock::now() + std::chrono::hours(24);
            }
        }

        // 캐시 파일에 저장 — temp 작성 후 atomic rename (C-2).
        // 읽는 쪽(Python balance)이 스트리밍 중인 truncated JSON을 보지 않게 한다.
        json cache_j = {{"access_token", access_token_}, {"expires_at", expires}};
        std::string tmp_path = cache_path + ".tmp";
        {
            std::ofstream cf(tmp_path);

            if (cf.is_open())
            {
                cf << cache_j.dump(2);
            }
        }
#ifdef _WIN32
        MoveFileExA(tmp_path.c_str(), cache_path.c_str(), MOVEFILE_REPLACE_EXISTING);
#else
        ::chmod(tmp_path.c_str(), S_IRUSR | S_IWUSR); // 0600 — 공유 볼륨 평문 토큰 보호 (W-4)
        // rename은 원자적이나 durability는 보장 안 함 — 크래시 시 0바이트 캐시→재발급(403).
        // tmp를 fsync해 데이터를 디스크에 내린 뒤 rename (V-2).
        {
            int fd = ::open(tmp_path.c_str(), O_RDONLY);

            if (fd >= 0) { ::fsync(fd); ::close(fd); }
        }

        std::rename(tmp_path.c_str(), cache_path.c_str()); // POSIX atomic
#endif

        LOG_INFO("[KIS] 토큰 발급 성공 (만료: " + expires + ")");
        return true;
    }
    catch (const std::exception& e)
    {
        LOG_ERROR(std::string("[KIS] 토큰 파싱 오류: ") + e.what());
        return false;
    }
}
