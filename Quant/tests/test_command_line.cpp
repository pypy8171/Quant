// tests/test_command_line.cpp
// 실행 인자 뜯기 검증 (D-114 단계 4) — 역할을 잘못 알아듣고 뜨는 길이 없는가.
//
//   ① 인자가 없으면 지금까지와 같은 기본값인가(설정 경로·모드 없음·역할 both)
//   ② 모드 낱말 넷은 여전히 모드로 가고, 나머지 맨 인자는 설정 경로인가
//   ③ --role 두 철자(--role X·--role=X)와 대소문자를 다 받는가
//   ④ 모르는 역할·값 빠진 --role·모르는 깃발은 **멈추는가**(기본값으로 조용히 돌아가지 않는가)
//   ⑤ 멈출 때 설정 경로가 오염되지 않는가(옛 파서는 `--help`를 설정 파일 이름으로 삼았다)
//   ⑥ 역할 이름이 왕복하는가(로그에 찍는 철자 그대로 다시 읽히는가)
//   ⑦ 역할마다 실행 로그 파일이 갈리는가(두 프로세스가 한 파일에 섞여 쓰지 않는가)
//   ⑧ 역할 술어 표 열두 칸이 그대로인가 — 여기가 이 시험의 핵심이다. 술어가 부정형으로 되돌아가면
//      Feed가 주문 쪽·전략 쪽까지 참이 되어 같은 계좌에 주문 프로세스가 둘 생긴다(D-114).
//      Engine의 같은 이름 술어는 ProcessRole의 이것을 부르기만 하므로, Engine을 만들지 않고 여기서 본다.
//
//   사용법: test_command_line

#include "core/CommandLine.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace
{
int g_checks = 0;

void check(bool condition, const std::string& name)
{
    ++g_checks;

    if (!condition)
    {
        std::cout << "[FAIL] " << name << "\n";
        std::abort();
    }

    std::cout << "[PASS] " << name << "\n";
}

// argv 흉내 — parse_command_line이 char*를 받으므로 수정 가능한 버퍼를 만들어 준다.
CommandLine parse(std::vector<std::string> arguments)
{
    std::string        program = "quant_trader";
    std::vector<char*> argv;
    argv.push_back(program.data());

    for (std::string& argument : arguments)
    {
        argv.push_back(argument.data());
    }

    return parse_command_line(static_cast<int>(argv.size()), argv.data());
}

// ① 기본값 — 인자 없이 뜨던 지금까지의 동작이 그대로인가
void test_defaults()
{
    const CommandLine command_line = parse({});
    check(command_line.error.empty(), "기본값: 오류 없음");
    check(command_line.config_path == "config/config.json", "기본값: 설정 경로");
    check(command_line.mode_override.empty(), "기본값: 모드 오버라이드 없음");
    check(command_line.role == ProcessRole::Both, "기본값: 역할 both");
}

// ② 모드 낱말과 설정 경로
void test_mode_and_config()
{
    for (const std::string mode_word : {"FEED", "KR_TEST", "US_TEST", "TRADE"})
    {
        const CommandLine command_line = parse({mode_word});
        check(command_line.mode_override == mode_word, "모드 낱말 " + mode_word);
        check(command_line.config_path == "config/config.json", "모드만 주면 설정 경로는 기본값 " + mode_word);
    }

    const CommandLine both = parse({"Quant/config/config.json", "TRADE"});
    check(both.config_path == "Quant/config/config.json", "설정 경로 + 모드: 경로");
    check(both.mode_override == "TRADE", "설정 경로 + 모드: 모드");
    check(both.role == ProcessRole::Both, "설정 경로 + 모드: 역할은 건드리지 않는다");
}

// ③ --role 두 철자와 대소문자
void test_role_spellings()
{
    const CommandLine separated = parse({"--role", "order"});
    check(separated.error.empty(), "--role order: 오류 없음");
    check(separated.role == ProcessRole::Order, "--role order");

    const CommandLine joined = parse({"--role=strategy"});
    check(joined.error.empty(), "--role=strategy: 오류 없음");
    check(joined.role == ProcessRole::Strategy, "--role=strategy");

    const CommandLine upper = parse({"--role", "ORDER"});
    check(upper.role == ProcessRole::Order, "--role ORDER (대문자)");

    const CommandLine with_config = parse({"Quant/config/config.json", "--role", "strategy"});
    check(with_config.config_path == "Quant/config/config.json", "설정 경로 + 역할: 경로");
    check(with_config.role == ProcessRole::Strategy, "설정 경로 + 역할: 역할");

    const CommandLine explicit_both = parse({"--role", "both"});
    check(explicit_both.role == ProcessRole::Both, "--role both");

    const CommandLine feed_role = parse({"--role", "feed"});
    check(feed_role.error.empty(), "--role feed: 오류 없음");
    check(feed_role.role == ProcessRole::Feed, "--role feed");

    const CommandLine feed_joined = parse({"--role=FEED"});
    check(feed_joined.error.empty(), "--role=FEED: 오류 없음");
    check(feed_joined.role == ProcessRole::Feed, "--role=FEED (대문자)");

    // 모드 낱말 FEED와 역할 feed는 다른 것이다 — 같이 줘도 서로 덮지 않는다
    const CommandLine mode_word_only = parse({"FEED"});
    check(mode_word_only.mode_override == "FEED", "모드 낱말 FEED: 모드로 간다");
    check(mode_word_only.role == ProcessRole::Both, "모드 낱말 FEED: 역할은 건드리지 않는다");

    const CommandLine mode_and_role = parse({"FEED", "--role", "feed"});
    check(mode_and_role.mode_override == "FEED", "FEED + --role feed: 모드");
    check(mode_and_role.role == ProcessRole::Feed, "FEED + --role feed: 역할");

    const CommandLine trade_and_feed_role = parse({"TRADE", "--role", "feed"});
    check(trade_and_feed_role.mode_override == "TRADE", "TRADE + --role feed: 모드는 TRADE 그대로");
    check(trade_and_feed_role.role == ProcessRole::Feed, "TRADE + --role feed: 역할은 feed");
}

// ④⑤ 멈춰야 하는 자리 — 여기서 기본값으로 낙하하면 두 프로세스가 같은 자리를 맡는다
void test_refusals()
{
    const CommandLine unknown_role = parse({"--role", "orderr"});
    check(!unknown_role.error.empty(), "모르는 역할이면 멈춘다");
    check(unknown_role.error.find("orderr") != std::string::npos, "모르는 역할: 오류에 값이 실린다");

    const CommandLine missing_value = parse({"--role"});
    check(!missing_value.error.empty(), "--role 뒤에 값이 없으면 멈춘다");
    check(missing_value.role == ProcessRole::Both, "--role 값 없음: 역할은 기본값 그대로");

    const CommandLine unknown_flag = parse({"--help"});
    check(!unknown_flag.error.empty(), "모르는 깃발이면 멈춘다");
    check(unknown_flag.config_path == "config/config.json", "모르는 깃발이 설정 경로가 되지 않는다");

    const CommandLine unknown_role_joined = parse({"--role="});
    check(!unknown_role_joined.error.empty(), "--role= 뒤가 비면 멈춘다");

    // 멈춘 뒤의 인자는 읽지 않는다 — 오류 하나로 바로 돌아온다
    const CommandLine after_error = parse({"--role", "nope", "Quant/config/config.json"});
    check(!after_error.error.empty(), "오류 뒤 인자: 멈춘 상태 유지");
    check(after_error.config_path == "config/config.json", "오류 뒤 인자: 설정 경로 오염 없음");
}

// ⑥ 이름 왕복 — 로그에 찍는 철자와 인자로 받는 철자가 같아야 사람이 로그를 보고 다시 띄운다
void test_name_round_trip()
{
    for (const ProcessRole role : {ProcessRole(ProcessRole::Both), ProcessRole(ProcessRole::Order),
                                   ProcessRole(ProcessRole::Strategy), ProcessRole(ProcessRole::Feed)})
    {
        ProcessRole parsed;
        check(ProcessRole::from_string(role.to_string(), parsed), std::string("왕복: ") + role.to_string());
        check(parsed == role, std::string("왕복 값: ") + role.to_string());
    }

    check(std::string(ProcessRole(ProcessRole::Both).to_string()) == "both", "이름 both");
    check(std::string(ProcessRole(ProcessRole::Order).to_string()) == "order", "이름 order");
    check(std::string(ProcessRole(ProcessRole::Strategy).to_string()) == "strategy", "이름 strategy");
    check(std::string(ProcessRole(ProcessRole::Feed).to_string()) == "feed", "이름 feed");
}

// ⑦ 실행 로그 파일 — 갈라 띄운 역할끼리 한 파일에 섞여 쓰면 `[큐 고수위]` 줄이 어느 쪽 수인지 모른다
void test_log_file_names()
{
    check(std::string(log_file_name(ProcessRole::Both)) == "quant_trader.log", "로그 파일 both");
    check(std::string(log_file_name(ProcessRole::Order)) == "quant_trader.order.log", "로그 파일 order");
    check(std::string(log_file_name(ProcessRole::Strategy)) == "quant_trader.strategy.log", "로그 파일 strategy");
    check(std::string(log_file_name(ProcessRole::Feed)) == "quant_trader.feed.log", "로그 파일 feed");
}

// ⑧ 역할 술어 표 — 역할 넷 × 술어 셋, 열두 칸. 이 표가 이 시험의 그물이다.
//
//   역할      | 주문 쪽 | 전략 쪽 | 시세 쪽
//   ----------|---------|---------|--------
//   Both      |  참     |  참     |  참
//   Order     |  참     |  거짓   |  거짓
//   Strategy  |  거짓   |  참     |  거짓
//   Feed      |  거짓   |  거짓   |  참
//
//   Feed 줄의 거짓 둘이 요점이다. 부정형 술어(`role != Strategy`)로 되돌아가면 이 둘이 참이 되어
//   시세 프로세스가 주문 스레드·원장·전략까지 띄운다 — 같은 계좌에 주문 프로세스가 둘이다(D-114).
void test_role_side_table()
{
    struct RoleSideRow
    {
        ProcessRole::Value role;
        const char*        name;
        bool               order_side;
        bool               strategy_side;
        bool               feed_side;
    };

    // 컴파일 시점에도 한 번 못을 박는다 — 술어가 constexpr이라 빌드가 먼저 막아 준다
    static_assert(ProcessRole(ProcessRole::Both).runs_order_side());
    static_assert(ProcessRole(ProcessRole::Both).runs_strategy_side());
    static_assert(ProcessRole(ProcessRole::Both).runs_feed_side());
    static_assert(!ProcessRole(ProcessRole::Feed).runs_order_side());
    static_assert(!ProcessRole(ProcessRole::Feed).runs_strategy_side());
    static_assert(ProcessRole(ProcessRole::Feed).runs_feed_side());

    const RoleSideRow rows[] = {
        {ProcessRole::Both, "both", true, true, true},
        {ProcessRole::Order, "order", true, false, false},
        {ProcessRole::Strategy, "strategy", false, true, false},
        {ProcessRole::Feed, "feed", false, false, true},
    };

    for (const RoleSideRow& row : rows)
    {
        const ProcessRole role(row.role);
        const std::string label = std::string("술어 표 ") + row.name + ": ";
        check(role.runs_order_side() == row.order_side, label + "주문 쪽");
        check(role.runs_strategy_side() == row.strategy_side, label + "전략 쪽");
        check(role.runs_feed_side() == row.feed_side, label + "시세 쪽");
    }
}

} // namespace

int main()
{
    std::cout << "=== 실행 인자 뜯기 (D-114 단계 4) ===\n";
    test_defaults();
    test_mode_and_config();
    test_role_spellings();
    test_refusals();
    test_name_round_trip();
    test_log_file_names();
    test_role_side_table();
    std::cout << "=== 전부 통과 (" << g_checks << " checks) ===\n";
    return 0;
}
