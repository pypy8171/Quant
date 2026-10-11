// 전 종목 부하 하네스 — 합성 체결을 진짜 Engine에 밀어넣어 수신 → 샤드 → 전략 → 주문 → 체결 → 원장 한 바퀴를 돌린다.
//  bench_market_firehose는 큐만(Mock) 재고 test_engine은 종목 셋뿐이라, 그 사이가 비어 있었다.
//  수신 스레드 수 N과 전략 샤드 수 M을 바꿔 가며 처리량·지연·큐 수위를 표로 뽑는 것이 목적이다. [why D-071]
//
//  쓰는 법(저장소 루트에서):
//    bench_engine_load run   --tickers 2700 --lanes 4 --shards 4 --seconds 20
//    bench_engine_load sweep --tickers 2700 --lane-list 1,2,4,8 --shard-list 1,2,4,8 --seconds 10 \
//                            --out logs/bench_engine_load.csv
//
//  유량을 안 묶으면(--rate 0) 큐가 전 구간 포화라 "포화 상태"만 보인다. 천장은 유량을 올려 가며 드롭이
//  처음 생기는 지점으로 읽는다 — --rate-list 100000,200000,400000,800000 처럼 준다.
//
//  [inv] 한 종목은 수신 스레드 하나만 내보내고 전략(샤드) 하나만 본다 — 종목 안 순서 보장(원칙 1·2).
//  [inv] 주문 한도·발주 간격은 전부 풀어 둔다. 여기서 재는 것은 파이프라인의 천장이지 리스크 규칙이 아니다.
//
//  입력 준비는 bench_engine_load_inputs.cpp, 한 구성 실행은 bench_engine_load_run.cpp, 공용 구조체는 bench_engine_load_support.h에 있다.

#include "bench_engine_load_support.h"

using namespace engine_load_bench;

int main(int argc, char** argv)
{
    // 산출물은 실행파일 옆 logs_bench/ — 기본 logs/는 트레이더·테스트와 같은 폴더다. 주문을 수만 건 내는 하네스가 거기
    //  open_orders.txt를 남기면 다음에 뜨는 트레이더·test_engine의 OrderRouter가 그걸 이전 세션 미체결로 읽어 전부 취소하러 간다.
    Logger::instance().set_base_directory(Logger::executable_directory() / "logs_bench");

    // 건마다 찍히는 INFO·WARN의 파일 I/O가 재려는 구간보다 길다 — 기본은 ERROR만 남기고 --log-level로 푼다.
    Logger::instance().initialize(Logger::instance().path_for("bench_engine_load.log"), LogLevel::ERROR);

    Options     options;
    std::string profile_rate_key; // --profile-rate p50|p90|p99|max
    std::string mode = argc > 1 && argv[1][0] != '-' ? argv[1] : "run";

    for (int index = 1; index < argc; ++index)
    {
        const std::string argument = argv[index];
        const std::string value    = index + 1 < argc ? argv[index + 1] : "";

        if (argument == "--tickers")
        {
            options.universe_size = static_cast<size_t>(std::stoul(value));
        }
        else if (argument == "--lanes")
        {
            options.lane_list = {static_cast<uint32_t>(std::stoul(value))};
        }
        else if (argument == "--shards")
        {
            options.shard_list = {static_cast<uint32_t>(std::stoul(value))};
        }
        else if (argument == "--lane-list")
        {
            options.lane_list = parse_number_list(value);
        }
        else if (argument == "--shard-list")
        {
            options.shard_list = parse_number_list(value);
        }
        else if (argument == "--seconds")
        {
            options.seconds = std::stoi(value);
        }
        else if (argument == "--rate")
        {
            options.rate_list = {static_cast<uint64_t>(std::stoull(value))};
        }
        else if (argument == "--rate-list")
        {
            options.rate_list.clear();

            for (const uint32_t rate : parse_number_list(value))
            {
                options.rate_list.push_back(rate);
            }
        }
        else if (argument == "--order-every")
        {
            options.order_every = std::stoi(value);
        }
        else if (argument == "--no-orders")
        {
            options.order_every = 0;
        }
        else if (argument == "--zipf")
        {
            options.zipf = std::stod(value);
        }
        else if (argument == "--universe")
        {
            options.universe_path = value;
        }
        else if (argument == "--source")
        {
            // synthetic = 합성 생성기(유량을 우리가 정한다) | http = 실제 장 시세(유량을 시장이 정한다)
            options.source = value;
        }
        else if (argument == "--sweep-ms")
        {
            options.sweep_milliseconds = std::stoi(value);
        }
        else if (argument == "--codes-per-call")
        {
            options.codes_per_call = static_cast<size_t>(std::stoul(value));
        }
        else if (argument == "--profile")
        {
            options.profile_path = value;
        }
        else if (argument == "--profile-rate")
        {
            // p50 | p90 | p99 | max — 프로파일의 전 종목 환산 초당 건수를 그대로 유량으로 쓴다
            profile_rate_key = value;
        }
        else if (argument == "--strategy")
        {
            options.strategy_kind = value;
        }
        else if (argument == "--channel-min")
        {
            options.channel_minutes = std::stoi(value);
        }
        else if (argument == "--clock-speed")
        {
            options.clock_speed = std::stod(value);
        }
        else if (argument == "--zmq-bind")
        {
            options.zmq_bind = value;
        }
        else if (argument == "--zmq-port")
        {
            options.zmq_port_base = std::stoi(value);
        }
        else if (argument == "--out")
        {
            options.out_path = value;
        }
        else if (argument == "--log-level")
        {
            // 측정이 아니라 무슨 일이 났는지 보려고 켤 때만. debug/info/warn/error.
            if (value == "debug")
            {
                Logger::instance().set_min_level(LogLevel::DEBUG);
            }
            else if (value == "info")
            {
                Logger::instance().set_min_level(LogLevel::INFO);
            }
            else if (value == "warn")
            {
                Logger::instance().set_min_level(LogLevel::WARN);
            }
            else
            {
                Logger::instance().set_min_level(LogLevel::ERROR);
            }
        }
    }

    // 유량을 실측 프로파일에서 가져온다 — 손으로 적은 숫자 대신 캡처가 정한 값으로 재려는 것이다.
    if (!profile_rate_key.empty() && !options.profile_path.empty())
    {
        const uint64_t profile_rate = load_profile_rate(options.profile_path, profile_rate_key);

        if (profile_rate == 0)
        {
            std::cerr << "[부하] 프로파일에서 " << profile_rate_key << " 유량을 못 읽는다: " << options.profile_path << "\n";
            return 1;
        }

        options.rate_list = {profile_rate};
        std::cout << "[부하] 실측 프로파일 " << options.profile_path << " · " << profile_rate_key
                  << " → 초당 " << profile_rate << "건\n";
    }

    const std::vector<std::string> universe = load_universe(options.universe_path, options.universe_size);
    std::cout << "[부하] 종목 " << universe.size() << "개, 구간 " << options.seconds << "초, 모드 " << mode << "\n";
    std::cout << kCsvHeader << "\n";

    std::ofstream out;

    if (!options.out_path.empty())
    {
        // 상위 폴더가 없으면 ofstream이 아무 말 없이 실패한다 — 한 시간짜리 스윕을 돌리고 빈손이 된다.
        const std::filesystem::path out_file = options.out_path;

        if (out_file.has_parent_path())
        {
            std::error_code ignored;
            std::filesystem::create_directories(out_file.parent_path(), ignored);
        }

        const std::uintmax_t append_offset = file_size_or_zero(out_file.string());
        out.open(out_file, std::ios::app);

        if (!out)
        {
            std::cerr << "[부하] CSV를 못 연다: " << options.out_path << "\n";
            return 1;
        }


        // 이어 붙이는 파일이 비었을 때만 헤더를 쓴다. ios::app 스트림의 tellp()는 첫 쓰기 전까지 0을 주므로
        //  그걸로 판정하면 실행할 때마다 헤더 줄이 끼어든다.
        if (append_offset == 0)
        {
            out << kCsvHeader << "\n";
        }
    }

    for (const uint64_t rate : options.rate_list)
    {
        options.ticks_per_sec = rate;

        for (const uint32_t lanes : options.lane_list)
        {
            for (const uint32_t shards : options.shard_list)
            {
                const RunResult   result = run_once(options, universe, lanes, shards);
                const std::string row    = to_csv_row(options, result);
                std::cout << row << std::endl;

                if (out)
                {
                    out << row << std::endl;
                }
            }
        }
    }

    return 0;
}
