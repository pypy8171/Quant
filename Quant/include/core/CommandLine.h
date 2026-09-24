// 실행 인자 한 줄을 뜯는다 — 설정 경로, 모드 오버라이드, 그리고 이 프로세스가 맡는 자리(역할).
//  역할은 config가 아니라 인자로만 받는다. 감시견이 **같은 config 한 장**으로 프로세스 둘을 띄우기 때문이다
//  (config에 두면 파일이 둘로 갈리고, 그러면 계좌·한도가 어긋나도 아무도 못 본다). [why D-114]
#pragma once

#include <string>
#include <string_view>

// 한 exe가 맡는 자리. Both는 지금까지의 한 프로세스(시세·전략·주문·원장 전부), Order는 주문·원장·체결,
//  Strategy는 전략·신호, Feed는 WebSocket 소켓 하나와 디코드다. StrategyType·Mode와 같은 스마트enum idiom.
//  값은 뒤에만 더한다 — 앞에 끼우면 저장된 숫자가 다른 역할을 가리킨다.
class ProcessRole
{
public:
    enum Value
    {
        Both,
        Order,
        Strategy,
        Feed
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

    // ── 이 역할이 맡는 일감 ────────────────────────────────────────────────
    // **긍정형으로 적는다** — 여기 적힌 역할만 참이다. 부정형(`value_ != Strategy`)으로 두면 역할이
    //  늘어날 때 새 역할이 아무도 손대지 않은 채 참이 되어, 시세만 맡을 프로세스가 주문 스레드와 원장까지
    //  띄운다. 그건 같은 계좌에 주문 프로세스가 둘이라는 뜻이다. [why D-114]
    // 이 셋이 역할 판정의 정본이다. Engine의 같은 이름 술어는 자기 역할을 넘겨 이것을 부르기만 한다 —
    //  Engine을 만들지 않고도 표를 시험으로 박아 둘 수 있게.
    [[nodiscard]] constexpr bool runs_order_side() const noexcept
    {
        return value_ == Order || value_ == Both;
    }

    [[nodiscard]] constexpr bool runs_strategy_side() const noexcept
    {
        return value_ == Strategy || value_ == Both;
    }

    [[nodiscard]] constexpr bool runs_feed_side() const noexcept
    {
        return value_ == Feed || value_ == Both;
    }

private:
    Value value_ = Both;
};

// 뜯은 결과 한 묶음. error가 비어 있지 않으면 **기동하지 않는다** — 인자를 못 알아들은 채 뜨면
//  설정 경로가 엉뚱한 파일이 되거나(옛 파서는 모르는 인자를 config 경로로 삼았다) 역할이 기본값으로
//  조용히 되돌아간다.
struct CommandLine
{
    std::string config_path = "config/config.json";
    ProcessRole role = ProcessRole::Both;
    std::string error;
};

// quant_trader [config] [--role both|order|strategy|feed]
//   quant_trader.exe                          → config/config.json, 역할 both
//   quant_trader.exe config.json TRADE        → 지정 config. 옛 명령줄의 `TRADE`는 받아 넘긴다
//   quant_trader.exe config.json --role order → 지정 config, 주문·원장만 맡는다
//   quant_trader.exe config.json --role feed  → 지정 config, 시세 소켓만 맡는다
// `FEED`·`KR_TEST`·`US_TEST` 낱말은 지운 실행 모드라 오류로 멈춘다. 역할 `--role feed`와는 다른 것이다. [why D-130]
[[nodiscard]] CommandLine parse_command_line(int argc, char* argv[]);

// 인자를 못 알아들었을 때 로그에 같이 싣는 한 줄. 쓰는 사람이 로그만 보고 고칠 수 있게.
[[nodiscard]] const char* command_line_usage();

// 이 역할이 쓸 실행 로그 파일 이름. 갈라 띄우면 두 프로세스가 한 파일에 섞여 써서 `[큐 고수위]` 줄이
//  어느 쪽 수인지 줄만 보고는 모른다 — 역할을 주고 띄운 경우만 파일을 가른다. Both는 이름을 그대로 둔다,
//  로그를 읽는 스크립트 열 개와 운영 경로가 이 이름을 보기 때문이다. [why D-114]
[[nodiscard]] const char* log_file_name(ProcessRole role);
