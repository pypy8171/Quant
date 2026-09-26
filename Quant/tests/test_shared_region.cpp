// 공유 쪽지 한 장(ipc::SharedRegion) 경계 시험 — 프로세스가 갈렸을 때 둘이 같은 판을 보는지, 판이 다르면
//  안 붙는지, 앞서 죽은 판이 남아 있어도 다시 만들 수 있는지. 프로세스를 띄우지 않고 한 프로세스 안에서
//  만든 쪽·붙은 쪽 손잡이를 둘 두고 본다 — 공유메모리는 같은 프로세스에서도 매핑이 둘이라 검사가 성립한다.
#include "ipc/SharedRegion.h"

#include <cstring>
#include <iostream>
#include <string>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace
{
int g_checks = 0;

#define CHECK(condition)                                                                 \
    do                                                                                   \
    {                                                                                    \
        ++g_checks;                                                                      \
        if (!(condition))                                                                \
        {                                                                                \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #condition << "\n"; \
            return 1;                                                                    \
        }                                                                                \
    } while (0)

// 같은 이름이 다른 시험 회차와 부딪히지 않게 프로세스 번호를 붙인다(ctest는 시험을 나란히 돌린다).
std::string unique_name(const char* suffix)
{
#ifdef _WIN32
    const unsigned long process_id = GetCurrentProcessId();
#else
    const unsigned long process_id = static_cast<unsigned long>(getpid());
#endif
    return "quant_test_region_" + std::to_string(process_id) + "_" + suffix;
}

constexpr size_t   kRegionBytes   = 4096;
constexpr uint32_t kLayoutVersion = 7;
} // namespace

int main()
{
    // 1. 만들면 머리가 채워지고, 쓸 수 있는 칸은 구역 크기에서 머리를 뺀 만큼이다.
    {
        const std::string  name = unique_name("basic");
        ipc::SharedRegion  region;
        CHECK(region.create(name, kRegionBytes, kLayoutVersion));
        CHECK(region.is_open());
        CHECK(region.is_owner());
        CHECK(region.name() == name);
        CHECK(region.last_error().empty());
        CHECK(region.payload_bytes() == kRegionBytes - ipc::SharedRegion::header_bytes());

        const ipc::SharedRegionHeader* header = region.header();
        CHECK(header != nullptr);
        CHECK(header->magic == ipc::kSharedRegionMagic);
        CHECK(header->layout_version == kLayoutVersion);
        CHECK(header->bytes == kRegionBytes);
        CHECK(header->created_at_ns > 0);
        CHECK(header->creator_process_id != 0);

        // 새로 만든 칸은 비어 있다 — 앞선 판의 찌꺼기를 읽지 않게 만들면서 지운다.
        CHECK(region.payload()[0] == std::byte{0});
        CHECK(region.payload()[region.payload_bytes() - 1] == std::byte{0});
    }

    // 2. 붙은 쪽은 만든 쪽이 쓴 것을 그대로 본다. 붙은 쪽은 주인이 아니다.
    {
        const std::string name = unique_name("attach");
        ipc::SharedRegion owner;
        CHECK(owner.create(name, kRegionBytes, kLayoutVersion));

        const char        message[] = "체결통보";
        std::memcpy(owner.payload(), message, sizeof(message));

        ipc::SharedRegion guest;
        CHECK(guest.attach(name, kRegionBytes, kLayoutVersion, ipc::SharedAttachRole::kStrategy));
        CHECK(guest.is_open());
        CHECK(!guest.is_owner());
        CHECK(std::memcmp(guest.payload(), message, sizeof(message)) == 0);
        CHECK(guest.header()->created_at_ns == owner.header()->created_at_ns);

        // 붙은 쪽이 쓴 것도 만든 쪽이 본다 — 같은 종이 한 장이다.
        guest.payload()[0] = std::byte{0x7f};
        CHECK(owner.payload()[0] == std::byte{0x7f});
    }

    // 3. 판이 다르거나 크기가 다르면 붙지 않는다 — 옛 exe가 새 배치에 붙어 엉뚱한 칸을 읽는 것을 막는다.
    {
        const std::string name = unique_name("layout");
        ipc::SharedRegion owner;
        CHECK(owner.create(name, kRegionBytes, kLayoutVersion));

        ipc::SharedRegion other_layout;
        CHECK(!other_layout.attach(name, kRegionBytes, kLayoutVersion + 1, ipc::SharedAttachRole::kStrategy));
        CHECK(!other_layout.is_open());
        CHECK(!other_layout.last_error().empty());

        ipc::SharedRegion other_size;
        CHECK(!other_size.attach(name, kRegionBytes * 2, kLayoutVersion, ipc::SharedAttachRole::kStrategy));
        CHECK(!other_size.is_open());
    }

    // 4. 없는 이름에는 붙지 않는다(주문 쪽이 아직 안 떴을 때 전략 쪽이 보는 경우).
    {
        ipc::SharedRegion guest;
        CHECK(!guest.attach(unique_name("absent"), kRegionBytes, kLayoutVersion, ipc::SharedAttachRole::kStrategy));
        CHECK(!guest.is_open());
        CHECK(!guest.last_error().empty());
    }

    // 5. 살아 있는 구역을 또 만들려 하면 실패한다 — 엔진이 둘 뜬 것을 기동 자리에서 잡는다.
    {
        const std::string name = unique_name("twice");
        ipc::SharedRegion first;
        CHECK(first.create(name, kRegionBytes, kLayoutVersion));

        ipc::SharedRegion second;
        CHECK(!second.create(name, kRegionBytes, kLayoutVersion));
        CHECK(!second.is_open());
        CHECK(!second.last_error().empty());
    }

    // 6. 닫은 뒤에는 같은 이름으로 다시 만들 수 있고, 만든 시각이 바뀐다 — 붙은 쪽이 재기동을 구분하는 값이다.
    {
        const std::string name = unique_name("restart");
        int64_t           first_created_at_ns = 0;
        {
            ipc::SharedRegion first;
            CHECK(first.create(name, kRegionBytes, kLayoutVersion));
            first_created_at_ns = first.header()->created_at_ns;
        }

        ipc::SharedRegion second;
        CHECK(second.create(name, kRegionBytes, kLayoutVersion));
        CHECK(second.header()->created_at_ns >= first_created_at_ns);
    }

    // 7. 잘못된 인자는 열지 않는다 — 머리도 못 담는 크기, 빈 이름.
    {
        ipc::SharedRegion region;
        CHECK(!region.create("", kRegionBytes, kLayoutVersion));
        CHECK(!region.create(unique_name("tiny"), ipc::SharedRegion::header_bytes() - 1, kLayoutVersion));
        CHECK(!region.is_open());
        CHECK(region.header() == nullptr);
        CHECK(region.payload() == nullptr);
        CHECK(region.payload_bytes() == 0);
    }

    // 8. 옮기면 손잡이도 같이 간다 — 옮겨 간 쪽이 열려 있고 옮긴 쪽은 닫혀 있다(Engine이 멤버로 들고 있을 자리).
    {
        const std::string name = unique_name("move");
        ipc::SharedRegion source;
        CHECK(source.create(name, kRegionBytes, kLayoutVersion));
        source.payload()[0] = std::byte{0x21};

        ipc::SharedRegion moved = std::move(source);
        CHECK(moved.is_open());
        CHECK(moved.is_owner());
        CHECK(moved.payload()[0] == std::byte{0x21});
        CHECK(!source.is_open()); // NOLINT(bugprone-use-after-move) — 옮긴 뒤 닫혔는지가 시험 대상이다
        CHECK(source.payload() == nullptr);

        ipc::SharedRegion target;
        target = std::move(moved);
        CHECK(target.is_open());
        CHECK(target.payload()[0] == std::byte{0x21});
    }

    // 9. 주인 표와 종료 사유 — 만든 쪽은 자기 번호·기동 시각을 적고, 붙은 쪽은 그 둘을 그대로 읽는다.
    //  정상 종료를 적으면 붙은 쪽이 본다. 안 적힌 0이 "크래시"라서 이 칸 하나가 둘을 가른다.
    {
        const std::string name = unique_name("owner");
        ipc::SharedRegion owner;
        CHECK(owner.create(name, kRegionBytes, kLayoutVersion));

        const ipc::ProcessIdentity mine = ipc::current_process_identity();
        CHECK(mine.is_set());
        CHECK(owner.creator_identity().same_as(mine));
        CHECK(owner.creator_is_alive());
        CHECK(owner.boot_generation() == 1);
        CHECK(owner.shutdown_reason() == ipc::SharedShutdownReason::kNone);
        CHECK(!owner.took_over_stale());

        ipc::SharedRegion peer;
        CHECK(peer.attach(name, kRegionBytes, kLayoutVersion, ipc::SharedAttachRole::kStrategy));
        CHECK(peer.creator_identity().same_as(mine));
        CHECK(peer.boot_generation() == 1);
        CHECK(peer.shutdown_reason() == ipc::SharedShutdownReason::kNone);

        owner.mark_clean_shutdown(ipc::SharedShutdownReason::kSessionEnd);
        CHECK(peer.shutdown_reason() == ipc::SharedShutdownReason::kSessionEnd);

        // 붙은 쪽은 주인 칸이 아니라 제 역할 칸에 적는다 — 주인 칸에 적히면 남은 쪽이 주인의 크래시를
        //  정상 종료로 읽는다.
        peer.mark_clean_shutdown(ipc::SharedShutdownReason::kOperator);
        CHECK(owner.shutdown_reason() == ipc::SharedShutdownReason::kSessionEnd);
        CHECK(owner.participant_shutdown_reason(ipc::SharedAttachRole::kStrategy) ==
              ipc::SharedShutdownReason::kOperator);

        // 먼저 적은 사유가 남는다 — stop()은 소멸자에서 한 번 더 불리고, 그때는 왜 내려갔는지를 모른다.
        owner.mark_clean_shutdown(ipc::SharedShutdownReason::kStartupFail);
        CHECK(owner.shutdown_reason() == ipc::SharedShutdownReason::kSessionEnd);

        // 사유 없음을 적는 것은 아무것도 안 하는 것이다 — 정상 종료 표시를 지워 크래시로 바꾸지 않는다.
        owner.mark_clean_shutdown(ipc::SharedShutdownReason::kNone);
        CHECK(owner.shutdown_reason() == ipc::SharedShutdownReason::kSessionEnd);
    }

    // 10. 죽은 표를 묻는다 — 번호가 같아도 기동 시각이 다르면 다른 프로세스다(번호 재사용).
    {
        CHECK(!ipc::process_is_alive(ipc::ProcessIdentity{}));

        ipc::ProcessIdentity mine = ipc::current_process_identity();
        CHECK(ipc::process_is_alive(mine));

        if (mine.start_time != 0)
        {
            ipc::ProcessIdentity reused = mine;
            reused.start_time += 1; // 내 번호를 남이 물려받은 모양
            CHECK(!ipc::process_is_alive(reused));
        }
    }

    // 11. 주인 없이 남은 옛 판을 물려받는다 — 기동 번호가 하나 오르고, 앞선 판이 크래시였음을 남긴다.
    //  윈도우는 짝이 핸들을 쥐고 있어 이름이 남고, 리눅스는 쥔 곳이 없어도 /dev/shm에 남는다. 둘 다 이 길로 온다.
    {
        const std::string name = unique_name("stale");
        ipc::SharedRegion dead_owner;
        CHECK(dead_owner.create(name, kRegionBytes, kLayoutVersion));
        CHECK(dead_owner.boot_generation() == 1);

        ipc::SharedRegion holder; // 짝 — 이것이 붙어 있어 윈도우에서 이름이 사라지지 않는다
        CHECK(holder.attach(name, kRegionBytes, kLayoutVersion, ipc::SharedAttachRole::kStrategy));

        // 주인이 죽은 모양을 만든다. 번호는 내 것 그대로 두고 기동 시각만 어긋내면 "번호는 살아 있는데
        //  그 프로세스는 아니다"가 되어, 죽은 주인과 번호 재사용을 한 번에 흉내 낸다.
        auto* forged = const_cast<ipc::SharedRegionHeader*>(holder.header());
        CHECK(forged != nullptr);
        forged->creator_start_time = forged->creator_start_time + 1;

        // 짝도 죽은 모양으로 둔다 — 짝이 살아 있으면 물려받지 않는다(13번).
        forged->attached[static_cast<size_t>(ipc::SharedAttachRole::kStrategy)].start_time += 1;

        if (ipc::current_process_identity().start_time == 0)
        {
            // 기동 시각을 못 읽는 자리에서는 번호로만 가린다 — 그때는 이 흉내가 성립하지 않아 건너뛴다.
            std::cout << "test_shared_region: 기동 시각을 못 읽어 11번을 건너뛴다\n";
        }
        else
        {
            ipc::SharedRegion taker;
            CHECK(taker.create(name, kRegionBytes, kLayoutVersion));
            CHECK(taker.took_over_stale()); // 앞선 판에 종료 사유가 안 적혀 있었다 = 크래시
            CHECK(taker.boot_generation() == 2);
            CHECK(taker.creator_identity().same_as(ipc::current_process_identity()));
        }
    }

    // 12. 붙는 쪽이 둘이 되면 종료 사유도 역할마다 따로 남는다 — 전략이 곱게 내려간 날 시세가 크래시했는지를
    //  가려야 재기동이 무엇을 되살릴지 정한다. 칸 하나를 나눠 쓰면 나중에 적은 쪽이 앞선 쪽을 덮는다. [why D-114]
    {
        const std::string name = unique_name("roles");
        ipc::SharedRegion owner;
        CHECK(owner.create(name, kRegionBytes, kLayoutVersion));

        // 아무도 안 붙은 자리는 "안 붙었다"다 — 크래시로 읽으면 매번 되살리기가 돈다.
        CHECK(!owner.participant_attached(ipc::SharedAttachRole::kStrategy));
        CHECK(!owner.participant_attached(ipc::SharedAttachRole::kFeed));
        CHECK(!owner.participant_is_alive(ipc::SharedAttachRole::kFeed));

        ipc::SharedRegion strategy_side;
        CHECK(strategy_side.attach(name, kRegionBytes, kLayoutVersion, ipc::SharedAttachRole::kStrategy));
        CHECK(strategy_side.attached_role() == ipc::SharedAttachRole::kStrategy);

        ipc::SharedRegion feed_side;
        CHECK(feed_side.attach(name, kRegionBytes, kLayoutVersion, ipc::SharedAttachRole::kFeed));
        CHECK(feed_side.attached_role() == ipc::SharedAttachRole::kFeed);

        const ipc::ProcessIdentity mine = ipc::current_process_identity();
        CHECK(owner.participant_attached(ipc::SharedAttachRole::kStrategy));
        CHECK(owner.participant_attached(ipc::SharedAttachRole::kFeed));
        CHECK(owner.participant_identity(ipc::SharedAttachRole::kFeed).same_as(mine));
        CHECK(owner.participant_is_alive(ipc::SharedAttachRole::kStrategy));

        // 전략만 곱게 내려간다. 시세 칸은 아무것도 안 적힌 채 남아야 한다 = 크래시.
        strategy_side.mark_clean_shutdown(ipc::SharedShutdownReason::kSessionEnd);
        CHECK(owner.participant_shutdown_reason(ipc::SharedAttachRole::kStrategy) ==
              ipc::SharedShutdownReason::kSessionEnd);
        CHECK(owner.participant_shutdown_reason(ipc::SharedAttachRole::kFeed) == ipc::SharedShutdownReason::kNone);
        CHECK(feed_side.participant_shutdown_reason(ipc::SharedAttachRole::kStrategy) ==
              ipc::SharedShutdownReason::kSessionEnd);

        // 주인 칸은 둘 중 누가 적어도 안 바뀐다.
        CHECK(owner.shutdown_reason() == ipc::SharedShutdownReason::kNone);

        // 시세도 내려간다 — 그제야 제 칸에 사유가 실린다.
        feed_side.mark_clean_shutdown(ipc::SharedShutdownReason::kOperator);
        CHECK(owner.participant_shutdown_reason(ipc::SharedAttachRole::kFeed) ==
              ipc::SharedShutdownReason::kOperator);
        CHECK(owner.participant_shutdown_reason(ipc::SharedAttachRole::kStrategy) ==
              ipc::SharedShutdownReason::kSessionEnd);

        // 이름은 로그에 그대로 싣는다.
        CHECK(ipc::role_name(ipc::SharedAttachRole::kStrategy) == "strategy");
        CHECK(ipc::role_name(ipc::SharedAttachRole::kFeed) == "feed");

        // 다시 붙으면 사유가 지워진다 — 지난 판의 종료 사유가 이번 판의 판정에 섞이면 안 된다.
        ipc::SharedRegion feed_again;
        CHECK(feed_again.attach(name, kRegionBytes, kLayoutVersion, ipc::SharedAttachRole::kFeed));
        CHECK(owner.participant_shutdown_reason(ipc::SharedAttachRole::kFeed) == ipc::SharedShutdownReason::kNone);
        CHECK(owner.participant_shutdown_reason(ipc::SharedAttachRole::kStrategy) ==
              ipc::SharedShutdownReason::kSessionEnd);
    }

    // 13. 주인은 죽었는데 짝이 살아 있으면 물려받지 않는다 — 판을 0으로 밀면 살아 있는 짝의 보내는 커서와
    //  새 커서가 어긋나 링이 멈춘다. 짝이 내려간 뒤에는 물려받는다.
    {
        const std::string name = unique_name("live_peer");
        ipc::SharedRegion dead_owner;
        CHECK(dead_owner.create(name, kRegionBytes, kLayoutVersion));

        ipc::SharedRegion feed_side;
        CHECK(feed_side.attach(name, kRegionBytes, kLayoutVersion, ipc::SharedAttachRole::kFeed));

        auto* forged = const_cast<ipc::SharedRegionHeader*>(feed_side.header());
        forged->creator_start_time = forged->creator_start_time + 1;

        if (ipc::current_process_identity().start_time == 0)
        {
            std::cout << "test_shared_region: 기동 시각을 못 읽어 13번을 건너뛴다\n";
        }
        else
        {
            const uint64_t generation_before = feed_side.boot_generation();
            ipc::SharedRegion taker;
            CHECK(!taker.create(name, kRegionBytes, kLayoutVersion));
            CHECK(taker.last_error().find("짝이 아직 떠 있다") != std::string::npos);
            CHECK(taker.last_error().find("feed") != std::string::npos);
            CHECK(feed_side.boot_generation() == generation_before); // 판은 그대로다

            // 짝이 죽은 모양이 되면 물려받는다.
            forged->attached[static_cast<size_t>(ipc::SharedAttachRole::kFeed)].start_time += 1;
            ipc::SharedRegion retaker;
            CHECK(retaker.create(name, kRegionBytes, kLayoutVersion));
            CHECK(retaker.boot_generation() == generation_before + 1);
        }
    }

    std::cout << "test_shared_region OK (" << g_checks << " checks)\n";
    return 0;
}
