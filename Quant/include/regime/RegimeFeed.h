#pragma once
// 국면 판정 피드 — 해외·국내 지표 8개의 등락으로 점수를 내 regime.json을 쓰는 엔진 안 스레드.
//  예전에는 파이썬 보조 프로세스(macro_regime_feed.py, 09-27 삭제)가 같은 파일을 썼다. 판정식·임계·파일 모양은
//  그대로 옮겼고, 읽는 쪽(core/EngineRegime.cpp·대시보드·알림)은 바뀌지 않는다. 파이썬에만 있던 FinanceDataReader
//  폴백은 뺐다 — 참고 지표 셋은 같은 원천(FRED CSV)을 직접 받는다. [why D-147]
//
//  한 사이클(기본 180초): 네이버 지수 1건 → Yahoo 차트 8건(나스닥·S&P는 현물 직전 세션 2건 더) → FRED 3건(30분에
//   한 번) → 장초 기준점 → 점수·판정 → 파일 교체·이력 한 줄.
//  모의·실계좌 엔진이 같은 출력 파일을 쓰도록 설정돼 있으면 먼저 쓴 쪽 하나만 쓰고 다른 쪽은 쉰다(파일 수정 시각으로 판정).
#include <atomic>
#include <ctime>
#include <filesystem>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace regime_feed
{

// 판정 경계. 기본값은 파이썬 원본의 상수와 같다 — 정지 −7, 청산 −11, 위험선호 표시 +3(D-083).
struct Thresholds
{
    int halt_score      = -7;
    int liquidate_score = -11;
    int on_score        = 3;
};

struct FeedConfig
{
    std::string out_path;                // regime.json을 쓸 곳. 비면 피드를 띄우지 않는다
    std::string history_path;            // 사이클마다 한 줄 쌓는 jsonl. 비면 안 쌓는다
    std::string open_reference_path;     // 장초 기준점 파일(재기동해도 그날 기준점 유지)
    int         interval_sec = 180;
    Thresholds  thresholds;
};

// 지표 하나를 받아 온 결과. pct가 비면 그 지표는 표결에서 빠진다.
struct Change
{
    std::optional<double> percent;
    std::optional<double> price;
    std::string           error;
    std::string           source;           // "naver"·"yahoo"·"fred"
    std::optional<double> previous_percent;     // 개장 전이면 어제 등락, 나스닥·S&P면 현물 직전 세션 등락
    std::optional<double> since_settle_percent; // 나스닥·S&P 선물의 정산 뒤 변동
    bool                  premarket = false;
};

// 그날 09:00 이후 첫 사이클의 가격. 장중 방향표의 기준이다.
struct OpenReference
{
    std::string                   date; // YYYY-MM-DD
    std::string                   timestamp;
    std::map<std::string, double> prices;
};

using Changes = std::map<std::string, Change>;

// ── 순수 함수(네트워크·파일 없음, tests/test_regime_feed.cpp) ──

// 네이버 실시간 지수 응답 → itemCode별 결과. 장이 안 열렸으면 percent 0·previous_pct에 오늘 표시 등락.
std::map<std::string, Change> parse_naver_index(std::string_view body);

// Yahoo 차트 응답의 현재가·전일 종가 → 등락. 오늘 정규장이 아직 안 열렸으면 percent 0·previous_pct에 받은 값.
Change parse_yahoo_chart(std::string_view body, std::time_t now_utc);

// Yahoo 일봉 응답 → 가장 최근 완결 세션의 종가 등락(그 앞 세션 대비). 못 구하면 비움.
std::optional<double> parse_last_session_percent(std::string_view body, std::time_t now_utc);

// FRED 그래프 CSV(날짜,값) → 마지막 두 값의 등락. "."·빈 값은 휴일이라 건너뛴다.
Change parse_fred_csv(std::string_view body);

// 방향표. 위험선호 쪽이면 +, 강도는 warn 이상 1·strong 이상 2.
int vote_for(std::string_view key, double percent);

// 점수 → 매수 명목 비율(0~1, 0.1 단위). 정지선 이하 0, on_score 이상 1, 사이는 네 점을 잇는 직선.
double entry_scale(int score, const Thresholds& thresholds);

// 기존 기준점 파일 내용과 이번 가격으로 오늘 기준점을 고른다. 새로 잡았으면 should_save가 true.
//  09:00 전이거나 기준 지표 가격이 없으면 빈 기준점(date가 빈 문자열).
OpenReference choose_open_reference(std::string_view existing_text, const Changes& changes, std::time_t now_utc,
                                    bool& should_save);

// 지표 결과 → regime.json 문서. 키 순서는 파이썬 원본과 같다.
nlohmann::ordered_json build_regime(const Changes& changes, const OpenReference& open_reference,
                                    const Thresholds& thresholds, std::time_t now_utc);

// regime.json 문서 → 이력 한 줄(jsonl).
std::string history_line(const nlohmann::ordered_json& regime, const Thresholds& thresholds);

// 엔진 로그 한 줄 요약.
std::string summary_line(const nlohmann::ordered_json& regime);

// ── 스레드 ──
class RegimeFeed
{
public:
    ~RegimeFeed();

    void start(const FeedConfig& config);
    void stop();

    // 한 사이클만 돌고 끝낸다(tools/regime_feed_once.cpp, 손으로 한 번 돌려 보는 용도). 다른 쓰는 쪽 확인은 건너뛴다.
    void run_once(const FeedConfig& config);

private:
    void run();
    void cycle();
    bool another_writer_owns_file();
    Changes fetch_changes();
    void    fetch_info_changes(Changes& changes);

    FeedConfig        config_;
    std::atomic<bool> running_{false};
    std::thread       worker_;

    std::optional<std::filesystem::file_time_type> last_written_; // 내가 마지막으로 쓴 파일의 수정 시각
    bool                                           resting_logged_ = false;
    Changes                                        info_cache_;
    std::time_t                                    info_fetched_at_ = 0;
};

} // namespace regime_feed
