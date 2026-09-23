// tests/test_control_channel.cpp
// 전략→주문 제어 요청 검증 (D-114 단계 2.5 갈래 B) — 표 하나가 여러 줄로 와도 온전할 때만 걸린다.
//
//   큐도 스레드도 쓰지 않는다. 레코드와 모으는 규칙만 손으로 먹여 본다.
//
//   ① 계좌 이름이 칸에 들어가고 그대로 나오는가(칸을 넘으면 잘리는가)
//   ② 열고-쌓고-닫으면 그 줄이 그대로 나오는가
//   ③ 열지 않고 온 줄은 버리고 세는가
//   ④ 남의 표 줄이 섞여 들어오지 않는가(표를 만드는 쪽이 둘일 때)
//   ⑤ 닫기가 센 줄 수와 모은 줄 수가 다르면 그 표는 안 거는가(중간이 큐에서 사라진 경우)
//   ⑥ 상한을 넘긴 표는 반쪽으로 걸리지 않는가
//   ⑦ 닫기를 못 받은 표는 다음 표를 열 때 버려지는가
//
//   사용법: test_control_channel

#include "ipc/ControlChannel.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

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

// 표 한 줄 만들기 — 종목 번호만 다른 줄이 대부분이라 여기서 묶는다.
ipc::ControlRequest row_of(uint64_t batch, symbol::SymbolId symbol)
{
    ipc::ControlRequest request;
    request.kind      = ipc::ControlKind::kSlotExemptEntry;
    request.batch     = batch;
    request.symbol_id = symbol;
    return request;
}
} // namespace

int main()
{
    // ── ① 계좌 칸 ──────────────────────────────────────────────────────────
    {
        ipc::ControlRequest request;
        ipc::set_account(request, "50204275");
        check(ipc::account_of(request) == "50204275", "계좌 이름이 칸에 들어가고 그대로 나온다");

        const std::string long_name(ipc::kControlAccountMax + 20, 'A');
        ipc::set_account(request, long_name);
        check(ipc::account_of(request).size() == ipc::kControlAccountMax - 1, "칸을 넘는 이름은 잘린다");

        ipc::set_account(request, "");
        check(ipc::account_of(request).empty(), "빈 이름은 빈 채로 나온다");
    }

    // ── ② 온전한 표 ────────────────────────────────────────────────────────
    {
        ipc::ControlTableBuilder builder(ipc::kControlTableMax);
        builder.begin(7);
        builder.add(row_of(7, 101));
        builder.add(row_of(7, 202));
        check(builder.is_open(), "닫기 전까지는 열려 있다");
        check(builder.commit(7, 2), "열고-쌓고-닫은 표는 걸린다");
        check(builder.rows().size() == 2, "쌓은 줄이 그대로 나온다");
        check(builder.rows()[0].symbol_id == 101 && builder.rows()[1].symbol_id == 202, "줄 순서가 보낸 순서다");
        check(builder.discarded() == 0, "버린 줄이 없다");
    }

    // ── ③ 열지 않고 온 줄 ──────────────────────────────────────────────────
    {
        ipc::ControlTableBuilder builder(ipc::kControlTableMax);
        builder.add(row_of(3, 101));
        check(builder.discarded() == 1, "열지 않고 온 줄은 버리고 센다");
        check(!builder.commit(3, 1), "열지 않은 표는 닫히지 않는다");
        check(builder.rows().empty(), "걸 줄이 없다");
    }

    // ── ④ 남의 표 줄 ───────────────────────────────────────────────────────
    {
        ipc::ControlTableBuilder builder(ipc::kControlTableMax);
        builder.begin(10);
        builder.add(row_of(10, 101));
        builder.add(row_of(99, 555)); // 다른 쪽이 만드는 표의 줄
        builder.add(row_of(10, 202));
        check(builder.discarded() == 1, "남의 표 줄은 버리고 센다");
        check(builder.commit(10, 2), "내 줄만으로 셈이 맞으면 걸린다");
        check(builder.rows().size() == 2, "남의 줄은 안 섞인다");
        check(!builder.commit(99, 1), "남의 닫기로는 안 닫힌다");
    }

    // ── ⑤ 중간이 사라진 표 ─────────────────────────────────────────────────
    {
        ipc::ControlTableBuilder builder(ipc::kControlTableMax);
        builder.begin(20);
        builder.add(row_of(20, 101));
        // 둘째 줄이 큐에서 사라졌다 — 받는 쪽은 본 적이 없어 모른다. 닫기가 센 수가 그것을 잡는다.
        check(!builder.commit(20, 2), "보낸 줄 수와 모은 줄 수가 다르면 안 건다");
        check(builder.rows().empty(), "반쪽 표는 걸 줄을 비운다");
        check(builder.discarded() == 1, "버린 줄을 센다");
    }

    // ── ⑥ 상한을 넘긴 표 ───────────────────────────────────────────────────
    {
        ipc::ControlTableBuilder builder(2);
        builder.begin(30);
        builder.add(row_of(30, 101));
        builder.add(row_of(30, 202));
        builder.add(row_of(30, 303)); // 상한 초과
        check(!builder.commit(30, 3), "상한을 넘긴 표는 반쪽으로 걸리지 않는다");
        check(builder.rows().empty(), "걸 줄이 없다");
    }

    // ── ⑦ 닫기를 못 받은 표 ────────────────────────────────────────────────
    {
        ipc::ControlTableBuilder builder(ipc::kControlTableMax);
        builder.begin(40);
        builder.add(row_of(40, 101));
        builder.begin(41); // 앞 표는 닫기를 못 받았다
        check(builder.discarded() == 1, "닫기를 못 받은 표의 줄은 다음 표를 열 때 버려진다");
        builder.add(row_of(41, 202));
        check(builder.commit(41, 1), "새 표는 멀쩡히 걸린다");
        check(builder.rows().size() == 1 && builder.rows()[0].symbol_id == 202, "앞 표의 줄이 남지 않는다");
    }

    std::cout << "test_control_channel: " << g_checks << " checks passed\n";
    return 0;
}
