#include "core/CommandLine.h"

#include <cctype>
#include <string>
#include <string_view>

namespace
{

// 대소문자를 가리지 않으려고 한 번 내린다. 역할 이름은 길어야 여덟 글자라 복사가 싸다.
std::string to_lower(std::string_view text)
{
    std::string lowered;
    lowered.reserve(text.size());

    for (const char character : text)
    {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    }

    return lowered;
}

// config의 "mode"를 덮어쓰는 낱말들. 이 넷만 모드로 보고 나머지 맨 인자는 설정 경로다.
bool is_mode_word(std::string_view argument)
{
    return argument == "KR_TEST" || argument == "US_TEST" || argument == "FEED" || argument == "TRADE";
}

} // namespace

bool ProcessRole::from_string(std::string_view text, ProcessRole& role)
{
    const std::string lowered = to_lower(text);

    if (lowered == "both")
    {
        role = ProcessRole(Both);
        return true;
    }

    if (lowered == "order")
    {
        role = ProcessRole(Order);
        return true;
    }

    if (lowered == "strategy")
    {
        role = ProcessRole(Strategy);
        return true;
    }

    return false;
}

const char* ProcessRole::to_string() const
{
    switch (value_)
    {
    case Order:
        return "order";

    case Strategy:
        return "strategy";

    case Both:
    default:
        return "both";
    }
}

const char* command_line_usage()
{
    return "사용법: quant_trader [설정파일] [FEED|KR_TEST|US_TEST|TRADE] [--role both|order|strategy]";
}

CommandLine parse_command_line(int argc, char* argv[])
{
    CommandLine command_line;

    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument = argv[index];

        if (is_mode_word(argument))
        {
            command_line.mode_override = argument;
            continue;
        }

        // --role=order 와 --role order 둘 다 받는다. 값이 빠진 채 끝나면 기본값으로 조용히 돌아가지 않고 멈춘다.
        if (argument == "--role")
        {
            if (index + 1 >= argc)
            {
                command_line.error = "--role 뒤에 값이 없다";
                return command_line;
            }

            ++index;

            if (!ProcessRole::from_string(argv[index], command_line.role))
            {
                command_line.error = std::string("모르는 역할: ") + argv[index];
                return command_line;
            }

            continue;
        }

        if (argument.starts_with("--role="))
        {
            const std::string_view value = argument.substr(std::string_view("--role=").size());

            if (!ProcessRole::from_string(value, command_line.role))
            {
                command_line.error = std::string("모르는 역할: ") + std::string(value);
                return command_line;
            }

            continue;
        }

        // 옛 파서는 여기서 모든 것을 설정 경로로 삼았다 — `--help`가 config 파일 이름이 되어 "설정 로드 실패"
        //  한 줄만 남았다. 모르는 깃발은 멈춘다.
        if (argument.starts_with("-"))
        {
            command_line.error = std::string("모르는 인자: ") + std::string(argument);
            return command_line;
        }

        command_line.config_path = argument;
    }

    return command_line;
}

const char* log_file_name(ProcessRole role)
{
    switch (role)
    {
        case ProcessRole::Order:
            return "quant_trader.order.log";

        case ProcessRole::Strategy:
            return "quant_trader.strategy.log";

        case ProcessRole::Both:
        default:
            return "quant_trader.log";
    }
}
