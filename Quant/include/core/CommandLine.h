// 실행 인자 한 줄을 뜯는다 — 설정 경로, 모드 오버라이드, 그리고 이 프로세스가 맡는 자리(역할).
//  역할은 config가 아니라 인자로만 받는다. 감시견이 **같은 config 한 장**으로 프로세스 둘을 띄우기 때문이다
//  (config에 두면 파일이 둘로 갈리고, 그러면 계좌·한도가 어긋나도 아무도 못 본다). [why D-114]
#pragma once

#include <string>
#include <string_view>

// 한 exe가 맡는 자리. Both는 지금까지의 한 프로세스(시세·전략·주문·원장 전부), Order는 주문·원장·체결,
//  Strategy는 시세·전략이다. StrategyType·Mode와 같은 스마트enum idiom.
class ProcessRole
{
public:
    enum Value
    {
        Both,
        Order,
        Strategy
    };

    ProcessRole() = default;

    constexpr ProcessRole(Value value) : value_(value)
    {
    }

    constexpr operator Value() const
    {
        return value_;
    }

    // 인자 문자열 → 역할. **모르는 값은 실패다**(Mode::from_string이 TRADE로 낙하하는 것과 다르다) —
    //  오타 하나로 두 프로세스가 같은 자리를 맡으면 같은 계좌에 주문이 두 번 난다(A등급).
    //  대소문자는 가리지 않는다. 성공하면 role에 담고 true.
    [[nodiscard]] static bool from_string(std::string_view text, ProcessRole& role);

    // 로그·오류 문구에 쓰는 이름. from_string이 받는 철자 그대로다.
    [[nodiscard]] const char* to_string() const;

private:
    Value value_ = Both;
};

// 뜯은 결과 한 묶음. error가 비어 있지 않으면 **기동하지 않는다** — 인자를 못 알아들은 채 뜨면
//  설정 경로가 엉뚱한 파일이 되거나(옛 파서는 모르는 인자를 config 경로로 삼았다) 역할이 기본값으로
//  조용히 되돌아간다.
struct CommandLine
{
    std::string config_path = "config/config.json";
    std::string mode_override; // 비어 있으면 config의 "mode"
    ProcessRole role = ProcessRole::Both;
    std::string error;
};

// quant_trader [config] [MODE] [--role both|order|strategy]
//   quant_trader.exe                          → config/config.json, mode from json, 역할 both
//   quant_trader.exe KR_TEST                  → config/config.json, mode=KR_TEST
//   quant_trader.exe config.json TRADE        → 지정 config, mode=TRADE
//   quant_trader.exe config.json --role order → 지정 config, 주문·원장만 맡는다
[[nodiscard]] CommandLine parse_command_line(int argc, char* argv[]);

// 인자를 못 알아들었을 때 로그에 같이 싣는 한 줄. 쓰는 사람이 로그만 보고 고칠 수 있게.
[[nodiscard]] const char* command_line_usage();
