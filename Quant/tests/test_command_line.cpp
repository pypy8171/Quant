// tests/test_command_line.cpp
// 실행 인자 뜯기 검증 (D-114 단계 4) — 역할을 잘못 알아듣고 뜨는 길이 없는가.
//
//   ① 인자가 없으면 지금까지와 같은 기본값인가(설정 경로·모드 없음·역할 both)
//   ② 모드 낱말 넷은 여전히 모드로 가고, 나머지 맨 인자는 설정 경로인가
//   ③ --role 두 철자(--role X·--role=X)와 대소문자를 다 받는가
//   ④ 모르는 역할·값 빠진 --role·모르는 깃발은 **멈추는가**(기본값으로 조용히 돌아가지 않는가)
//   ⑤ 멈출 때 설정 경로가 오염되지 않는가(옛 파서는 `--help`를 설정 파일 이름으로 삼았다)
//   ⑥ 역할 이름이 왕복하는가(로그에 찍는 철자 그대로 다시 읽히는가)
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
                                   ProcessRole(ProcessRole::Strategy)})
    {
        ProcessRole parsed;
        check(ProcessRole::from_string(role.to_string(), parsed), std::string("왕복: ") + role.to_string());
        check(parsed == role, std::string("왕복 값: ") + role.to_string());
    }

    check(std::string(ProcessRole(ProcessRole::Both).to_string()) == "both", "이름 both");
    check(std::string(ProcessRole(ProcessRole::Order).to_string()) == "order", "이름 order");
    check(std::string(ProcessRole(ProcessRole::Strategy).to_string()) == "strategy", "이름 strategy");
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
    std::cout << "=== 전부 통과 (" << g_checks << " checks) ===\n";
    return 0;
}
