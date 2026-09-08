// WebSocket 다건 프레임 분리(split_records)와 분봉 커서 시각 산술(kis_hhmmss_minus_minutes) 단위 테스트.
//  둘 다 순수 함수라 KIS 연결 없이 헤더만으로 돈다. 임시 폴더에서 실행할 것(repo 루트 금지).
#include "api/KisClient.h"
#include "api/KisWebSocket.h"
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

static std::vector<std::string> make_fields(int records, int width, const std::string& tag)
{
    std::vector<std::string> out;
    for (int r = 0; r < records; ++r)
    {
        for (int i = 0; i < width; ++i)
        {
            out.push_back(tag + std::to_string(r) + "_" + std::to_string(i));
        }
    }
    return out;
}

int main()
{
    // ── split_records ─────────────────────────────────────────────────────
    // count<=1 은 자를 것이 없다 — 빈 벡터(호출부는 단건 경로).
    assert(KisWebSocket::split_records(make_fields(1, 22, "a"), 1, 22).empty());
    assert(KisWebSocket::split_records(make_fields(1, 22, "a"), 0, 22).empty());
    assert(KisWebSocket::split_records({}, 2, 22).empty());

    // 2×22 필드, count=2 → 22폭 레코드 둘. 순서와 경계가 보존된다.
    {
        auto recs = KisWebSocket::split_records(make_fields(2, 22, "t"), 2, 22);
        assert(recs.size() == 2);
        assert(recs[0].size() == 22 && recs[1].size() == 22);
        assert(recs[0][0] == "t0_0" && recs[0][21] == "t0_21");
        assert(recs[1][0] == "t1_0" && recs[1][21] == "t1_21");
    }

    // 3×46 (실측에 가까운 체결 폭), count=3 → 셋.
    {
        auto recs = KisWebSocket::split_records(make_fields(3, 46, "c"), 3, 22);
        assert(recs.size() == 3);
        assert(recs[2][45] == "c2_45");
    }

    // 나누어떨어지지 않으면 자르지 않는다(45필드 / 2).
    assert(KisWebSocket::split_records(make_fields(1, 45, "x"), 2, 22).empty());

    // 폭이 채널 최소 미만이면 자르지 않는다(3×5=15필드를 count=3으로 나누면 폭 5 < 22).
    assert(KisWebSocket::split_records(make_fields(3, 5, "s"), 3, 22).empty());

    // 최소 폭이 0인 알 수 없는 채널도 나누어떨어지기만 하면 자른다.
    assert(KisWebSocket::split_records(make_fields(2, 7, "u"), 2, 0).size() == 2);

    // ── kis_hhmmss_minus_minutes ──────────────────────────────────────────
    assert(kis_hhmmss_minus_minutes("100000", 1) == "095900"); // 10진수 -100이면 "099900"이 나오던 사례
    assert(kis_hhmmss_minus_minutes("093000", 1) == "092900");
    assert(kis_hhmmss_minus_minutes("090100", 1) == "090000");
    assert(kis_hhmmss_minus_minutes("090000", 1) == "085900");
    assert(kis_hhmmss_minus_minutes("000030", 1) == "");       // 자정 아래로 내려가면 빈 문자열
    assert(kis_hhmmss_minus_minutes("153000", 30) == "150000");
    assert(kis_hhmmss_minus_minutes("1530", 1) == "");         // 형식 오류
    assert(kis_hhmmss_minus_minutes("15:0:0", 1) == "");
    assert(kis_hhmmss_minus_minutes("256000", 1) == "");       // 범위 밖 시·분

    std::cout << "test_ws_frame: all passed" << std::endl;
    return 0;
}
