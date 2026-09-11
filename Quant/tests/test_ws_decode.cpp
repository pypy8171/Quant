// KIS 실시간 채널 디코더(api/KisWsDecode.h) 단위 테스트. 채널마다 필드 위치·최소 길이·숫자 실패 처리를
// 고정한다. 헤더 전용이라 소켓·암호 링크 없이 돈다. 관련 결정: D-037.
#include "api/KisWsDecode.h"
#include <cassert>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

using kis_ws::Decode;

// 디코더는 원문을 가리키는 뷰(kis_ws::Fields)를 받는다(D-042). 테스트는 std::string 벡터를 만들고
//  호출 직전에 뷰로 바꾼다 — 벡터가 살아 있는 동안만 유효하다.
struct V
{
    std::vector<std::string_view> v;
    V(const std::vector<std::string>& f) : v(f.begin(), f.end()) {}
    operator kis_ws::Fields() const { return kis_ws::Fields(v); }
};

// width개 필드를 "F<i>"로 채운 뒤 호출자가 필요한 칸만 덮어쓴다.
static std::vector<std::string> blank(size_t width)
{
    std::vector<std::string> f(width);

    for (size_t i = 0; i < width; ++i)
    {
        f[i] = "F" + std::to_string(i);
    }

    return f;
}

// 5단계 호가 배열을 채운다: 매도 100+i·잔량 10+i, 매수 90+i·잔량 20+i.
static void put_levels(std::vector<std::string>& f, size_t ask_p, size_t ask_q, size_t bid_p, size_t bid_q)
{
    for (size_t i = 0; i < 5; ++i)
    {
        f[ask_p + i] = std::to_string(100 + i);
        f[ask_q + i] = std::to_string(10 + i);
        f[bid_p + i] = std::to_string(90 + i);
        f[bid_q + i] = std::to_string(20 + i);
    }
}

static void check_levels(const OrderBook& ob)
{
    for (int i = 0; i < 5; ++i)
    {
        assert(ob.asks[i].price == 100.0 + i);
        assert(ob.asks[i].quantity == 10 + i);
        assert(ob.bids[i].price == 90.0 + i);
        assert(ob.bids[i].quantity == 20 + i);
    }
}

static void test_orderbook()
{
    auto f = blank(kis_ws::kMinFieldsOrderbook);
    f[0] = "005930";
    f[1] = "093001";
    put_levels(f, 3, 23, 13, 33);
    OrderBook ob;
    assert(kis_ws::decode_orderbook(V(f), ob) == Decode::kOk);
    assert(ob.ticker == "005930" && ob.time == "093001");
    check_levels(ob);

    // 37필드면 마지막 잔량이 없다 — 구조체를 건드리지 않는다.
    f.pop_back();
    OrderBook untouched;
    assert(kis_ws::decode_orderbook(V(f), untouched) == Decode::kShort);
    assert(untouched.ticker.empty());

    // 숫자 하나가 비면 kBadNumber. 나머지 칸은 채워진다(현재 호출자는 이 레코드를 흘려보낸다).
    f.push_back("37");
    f[5] = "";
    OrderBook partial;
    assert(kis_ws::decode_orderbook(V(f), partial) == Decode::kBadNumber);
    assert(partial.asks[2].price == 0.0);
    assert(partial.asks[2].quantity == 12); // 같은 단계의 잔량은 그대로 읽는다
    assert(partial.bids[4].price == 94.0);
}

static void test_fut_orderbook()
{
    auto f = blank(kis_ws::kMinFieldsFutOrderbook);
    f[0] = "101W09";
    put_levels(f, 2, 22, 7, 27);
    OrderBook ob;
    assert(kis_ws::decode_fut_orderbook(V(f), ob) == Decode::kOk);
    assert(ob.ticker == "101W09");
    check_levels(ob);

    // 건수 블록(12-21)은 읽지 않는다 — 비어 있어도 kOk.
    for (size_t i = 12; i <= 21; ++i)
    {
        f[i] = "";
    }

    OrderBook ob2;
    assert(kis_ws::decode_fut_orderbook(V(f), ob2) == Decode::kOk);

    f.resize(31);
    assert(kis_ws::decode_fut_orderbook(V(f), ob2) == Decode::kShort);
}

static void test_kr_trade()
{
    auto f = blank(kis_ws::kMinFieldsKrTrade);
    f[0] = "000660";
    f[1] = "101500";
    f[2] = "215000";
    f[12] = "37";
    f[21] = "5";
    TradeData td;
    assert(kis_ws::decode_kr_trade(V(f), td) == Decode::kOk);
    assert(td.ticker == "000660" && td.time == "101500");
    assert(td.price == 215000.0 && td.quantity == 37 && td.direction == 5);
    assert(td.market == Market::KR);

    f[21] = "x";
    TradeData bad;
    assert(kis_ws::decode_kr_trade(V(f), bad) == Decode::kBadNumber);
    assert(bad.price == 215000.0 && bad.direction == 0);

    // 칸별 실패: 앞 칸(가격)이 깨져도 뒤 칸(수량·방향)은 읽는다. 구 파서는 첫 실패에서 멈춰
    // 수량·방향이 0이었다 — D-037이 바꾼 동작이라 여기서 고정한다.
    f[2] = "";
    f[21] = "5";
    TradeData partial;
    assert(kis_ws::decode_kr_trade(V(f), partial) == Decode::kBadNumber);
    assert(partial.price == 0.0 && partial.quantity == 37 && partial.direction == 5);

    // 전체가 숫자여야 한다 — "215000abc"·"1,000"은 앞자리만 읽지 않고 kBadNumber(D-039).
    //  한 칸이 밀린 전문이 그럴듯한 값으로 통과하는 것을 막는다.
    f[2] = "215000abc";
    f[12] = "1,000";
    TradeData strict;
    assert(kis_ws::decode_kr_trade(V(f), strict) == Decode::kBadNumber);

    // 앞의 '+'는 KIS 부호 표기라 허용한다.
    f[2] = "+215000";
    f[12] = "1000";
    TradeData signed_ok;
    assert(kis_ws::decode_kr_trade(V(f), signed_ok) == Decode::kOk);
    assert(signed_ok.price == 215000.0 && signed_ok.quantity == 1000);

    f.resize(21);
    assert(kis_ws::decode_kr_trade(V(f), bad) == Decode::kShort);
}

static void test_us_trade()
{
    auto f = blank(kis_ws::kMinFieldsUsTrade);
    f[0] = "AAPL";
    f[2] = "189.25";
    f[8] = "120";
    TradeData td;
    assert(kis_ws::decode_us_trade(V(f), td) == Decode::kOk);
    assert(td.market == Market::US && td.price == 189.25 && td.quantity == 120);
    assert(td.direction == 0); // 20필드 이하면 방향 없음

    auto g = blank(21);
    g[2] = "1";
    g[8] = "2";
    g[20] = "1";
    TradeData td2;
    assert(kis_ws::decode_us_trade(V(g), td2) == Decode::kOk);
    assert(td2.direction == 1);

    g[20] = "x";
    TradeData td3;
    assert(kis_ws::decode_us_trade(V(g), td3) == Decode::kBadNumber);
    assert(td3.price == 1.0 && td3.quantity == 2 && td3.direction == 0);

    g.resize(8);
    assert(kis_ws::decode_us_trade(V(g), td2) == Decode::kShort);
}

static void test_fut_trade()
{
    auto f = blank(kis_ws::kMinFieldsFutTrade);
    f[0] = "101W09";
    f[5] = "412.35";
    f[9] = "3";
    TradeData td;
    td.direction = 7; // 채널이 방향을 안 주므로 0으로 덮어써야 한다
    assert(kis_ws::decode_fut_trade(V(f), td) == Decode::kOk);
    assert(td.price == 412.35 && td.quantity == 3 && td.direction == 0);
    assert(td.market == Market::KR);

    f.resize(18);
    assert(kis_ws::decode_fut_trade(V(f), td) == Decode::kShort);
}

static std::vector<std::string> fill_record(const std::string& side, const std::string& cntg_yn)
{
    auto f = blank(kis_ws::kMinFieldsFill);
    f[2] = "0000123456";
    f[4] = side;
    f[8] = "005930";
    f[9] = "10";
    f[10] = "71500";
    f[11] = "093512";
    f[13] = cntg_yn;
    return f;
}

static void test_fill()
{
    FillNotification fn;
    assert(kis_ws::decode_fill(V(fill_record("02", "2")), fn) == Decode::kOk);
    assert(fn.side == OrderSide::BUY && fn.odno == "0000123456" && fn.ticker == "005930");
    assert(fn.filled_qty == 10 && fn.filled_price == 71500.0 && fn.fill_time == "093512");

    FillNotification sell;
    assert(kis_ws::decode_fill(V(fill_record("01", "2")), sell) == Decode::kOk);
    assert(sell.side == OrderSide::SELL);

    // 접수 통보(1)는 체결이 아니다.
    FillNotification skip;
    assert(kis_ws::decode_fill(V(fill_record("02", "1")), skip) == Decode::kSkip);
    assert(skip.odno.empty());

    // 매매구분이 01/02 밖이면 원장에 못 넣는다 — 채우지 않고 돌려보낸다.
    FillNotification bad_side;
    assert(kis_ws::decode_fill(V(fill_record("03", "2")), bad_side) == Decode::kBadSide);
    assert(bad_side.odno.empty() && bad_side.side == OrderSide::NONE);

    // 수량·단가 변환 실패는 kBadNumber. 호출자가 버린다.
    auto f = fill_record("02", "2");
    f[9] = "";
    FillNotification bad_num;
    assert(kis_ws::decode_fill(V(f), bad_num) == Decode::kBadNumber);

    // 1~2필드 제어 메시지와 13필드 모두 kShort.
    FillNotification s;
    assert(kis_ws::decode_fill(V({"0", "1"}), s) == Decode::kShort);
    assert(kis_ws::decode_fill(V(blank(13)), s) == Decode::kShort);
}

int main()
{
    test_orderbook();
    test_fut_orderbook();
    test_kr_trade();
    test_us_trade();
    test_fut_trade();
    test_fill();
    std::cout << "test_ws_decode: all passed" << std::endl;
    return 0;
}
