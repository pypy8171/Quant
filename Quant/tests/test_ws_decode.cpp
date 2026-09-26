// KIS 실시간 채널 디코더(api/KisWsDecode.h) 단위 테스트. 채널마다 필드 위치·최소 길이·숫자 실패 처리를
// 고정한다. 헤더 전용이라 소켓·암호 링크 없이 돈다. 관련 결정: D-037.
#include "api/KisWsDecode.h"
#include <cassert>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

using kis_websocket::Decode;

// 디코더는 원문을 가리키는 뷰(kis_websocket::Fields)를 받는다(D-042). 테스트는 std::string 벡터를 만들고
//  호출 직전에 뷰로 바꾼다 — 벡터가 살아 있는 동안만 유효하다.
struct FieldList
{
    std::vector<std::string_view> views;
    FieldList(const std::vector<std::string>& fields) : views(fields.begin(), fields.end()) {}
    operator kis_websocket::Fields() const { return kis_websocket::Fields(views); }
};

// width개 필드를 "F<i>"로 채운 뒤 호출자가 필요한 칸만 덮어쓴다.
static std::vector<std::string> blank(size_t width)
{
    std::vector<std::string> fields(width);

    for (size_t column_index = 0; column_index < width; ++column_index)
    {
        fields[column_index] = "F" + std::to_string(column_index);
    }

    return fields;
}

// 5단계 호가 배열을 채운다: 매도 100+i·잔량 10+i, 매수 90+i·잔량 20+i.
static void put_levels(std::vector<std::string>& fields, size_t ask_price, size_t ask_quantity, size_t bid_price, size_t bid_quantity)
{
    for (size_t index = 0; index < 5; ++index)
    {
        fields[ask_price + index] = std::to_string(100 + index);
        fields[ask_quantity + index] = std::to_string(10 + index);
        fields[bid_price + index] = std::to_string(90 + index);
        fields[bid_quantity + index] = std::to_string(20 + index);
    }
}

static void check_levels(const OrderBook& order_book)
{
    for (int index = 0; index < 5; ++index)
    {
        assert(order_book.asks[index].price == 100.0 + index);
        assert(order_book.asks[index].quantity == 10 + index);
        assert(order_book.bids[index].price == 90.0 + index);
        assert(order_book.bids[index].quantity == 20 + index);
    }
}

static void test_orderbook()
{
    auto fields = blank(kis_websocket::kMinFieldsOrderbook);
    fields[0] = "005930";
    fields[1] = "093001";
    put_levels(fields, 3, 23, 13, 33);
    OrderBook order_book;
    assert(kis_websocket::decode_orderbook(FieldList(fields), order_book) == Decode::kOk);
    assert(order_book.ticker == "005930" && order_book.hhmmss == 93001);
    check_levels(order_book);

    // 37필드면 마지막 잔량이 없다 — 구조체를 건드리지 않는다.
    fields.pop_back();
    OrderBook untouched;
    assert(kis_websocket::decode_orderbook(FieldList(fields), untouched) == Decode::kShort);
    assert(untouched.ticker.empty());

    // 숫자 하나가 비면 kBadNumber. 나머지 칸은 채워진다(현재 호출자는 이 레코드를 흘려보낸다).
    fields.push_back("37");
    fields[5] = "";
    OrderBook partial;
    assert(kis_websocket::decode_orderbook(FieldList(fields), partial) == Decode::kBadNumber);
    assert(partial.asks[2].price == 0.0);
    assert(partial.asks[2].quantity == 12); // 같은 단계의 잔량은 그대로 읽는다
    assert(partial.bids[4].price == 94.0);
}

static void test_kr_trade()
{
    auto fields = blank(kis_websocket::kMinFieldsKrTrade);
    fields[0] = "000660";
    fields[1] = "101500";
    fields[2] = "215000";
    fields[12] = "37";
    fields[13] = "1234567";
    fields[18] = "123.45";
    fields[21] = "5";
    TradeData trade;
    assert(kis_websocket::decode_kr_trade(FieldList(fields), trade) == Decode::kOk);
    assert(trade.ticker == "000660" && trade.hhmmss == 101500);
    assert(trade.price == 215000.0 && trade.quantity == 37 && trade.direction == 5);
    assert(trade.market == Market::KR);
    assert(trade.accumulated_volume == 1234567 && trade.strength == 123.45);

    // 보조 필드(누적거래량·체결강도)는 비거나 깨져도 kOk — 0으로 둔다.
    fields[13] = "";
    fields[18] = "n/a";
    TradeData auxiliary_trade;
    assert(kis_websocket::decode_kr_trade(FieldList(fields), auxiliary_trade) == Decode::kOk);
    assert(auxiliary_trade.accumulated_volume == 0 && auxiliary_trade.strength == 0.0);
    fields[13] = "1234567";
    fields[18] = "123.45";

    fields[21] = "x";
    TradeData bad;
    assert(kis_websocket::decode_kr_trade(FieldList(fields), bad) == Decode::kBadNumber);
    assert(bad.price == 215000.0 && bad.direction == 0);

    // 칸별 실패: 앞 칸(가격)이 깨져도 뒤 칸(수량·방향)은 읽는다. 구 파서는 첫 실패에서 멈춰
    // 수량·방향이 0이었다 — D-037이 바꾼 동작이라 여기서 고정한다.
    fields[2] = "";
    fields[21] = "5";
    TradeData partial;
    assert(kis_websocket::decode_kr_trade(FieldList(fields), partial) == Decode::kBadNumber);
    assert(partial.price == 0.0 && partial.quantity == 37 && partial.direction == 5);

    // 전체가 숫자여야 한다 — "215000abc"·"1,000"은 앞자리만 읽지 않고 kBadNumber(D-039).
    //  한 칸이 밀린 전문이 그럴듯한 값으로 통과하는 것을 막는다.
    fields[2] = "215000abc";
    fields[12] = "1,000";
    TradeData strict;
    assert(kis_websocket::decode_kr_trade(FieldList(fields), strict) == Decode::kBadNumber);

    // 앞의 '+'는 KIS 부호 표기라 허용한다.
    fields[2] = "+215000";
    fields[12] = "1000";
    TradeData signed_ok;
    assert(kis_websocket::decode_kr_trade(FieldList(fields), signed_ok) == Decode::kOk);
    assert(signed_ok.price == 215000.0 && signed_ok.quantity == 1000);

    fields.resize(21);
    assert(kis_websocket::decode_kr_trade(FieldList(fields), bad) == Decode::kShort);
}

static void test_us_trade()
{
    auto fields_f = blank(kis_websocket::kMinFieldsUsTrade);
    fields_f[0] = "AAPL";
    fields_f[2] = "189.25";
    fields_f[8] = "120";
    TradeData trade;
    assert(kis_websocket::decode_us_trade(FieldList(fields_f), trade) == Decode::kOk);
    assert(trade.market == Market::US && trade.price == 189.25 && trade.quantity == 120);
    assert(trade.direction == 0); // 20필드 이하면 방향 없음

    auto fields_g = blank(21);
    fields_g[2] = "1";
    fields_g[8] = "2";
    fields_g[20] = "1";
    TradeData trade_two;
    assert(kis_websocket::decode_us_trade(FieldList(fields_g), trade_two) == Decode::kOk);
    assert(trade_two.direction == 1);

    fields_g[20] = "x";
    TradeData trade_three;
    assert(kis_websocket::decode_us_trade(FieldList(fields_g), trade_three) == Decode::kBadNumber);
    assert(trade_three.price == 1.0 && trade_three.quantity == 2 && trade_three.direction == 0);

    fields_g.resize(8);
    assert(kis_websocket::decode_us_trade(FieldList(fields_g), trade_two) == Decode::kShort);
}

static std::vector<std::string> fill_record(const std::string& side, const std::string& cntg_yn)
{
    auto fields = blank(kis_websocket::kMinFieldsFill);
    fields[2] = "0000123456";
    fields[3] = "0000123455";
    fields[4] = side;
    fields[8] = "005930";
    fields[9] = "10";
    fields[10] = "71500";
    fields[11] = "093512";
    fields[13] = cntg_yn;
    return fields;
}

static void test_fill()
{
    FillNotification fill_notification;
    assert(kis_websocket::decode_fill(FieldList(fill_record("02", "2")), fill_notification) == Decode::kOk);
    assert(fill_notification.side == OrderSide::BUY && fill_notification.kis_order_no == "0000123456" && fill_notification.ticker == "005930");
    assert(fill_notification.filled_quantity == 10 && fill_notification.filled_price == 71500.0 && fill_notification.fill_time == "093512");
    // 원주문번호는 최소 폭 안이라 늘 읽는다.
    assert(fill_notification.original_order_no == "0000123455");
    // 주문수량·거래소는 전문 뒤쪽 칸이다. 짧은 전문이면 "모른다"로 두고 체결 자체는 그대로 만든다.
    assert(fill_notification.order_quantity == 0 && fill_notification.exchange.empty());

    // 26칸 전문이면 보조 필드도 채운다.
    auto wide_record = fill_record("02", "2");
    wide_record.resize(26);
    wide_record[kis_websocket::kFillFieldOrderQuantity] = "91";
    wide_record[kis_websocket::kFillFieldExchange]      = "KRX";
    FillNotification wide;
    assert(kis_websocket::decode_fill(FieldList(wide_record), wide) == Decode::kOk);
    assert(wide.order_quantity == 91 && wide.exchange == "KRX");

    // 같은 구조체를 돌려 써도 앞 레코드의 보조 필드가 남지 않는다.
    assert(kis_websocket::decode_fill(FieldList(fill_record("02", "2")), wide) == Decode::kOk);
    assert(wide.order_quantity == 0 && wide.exchange.empty());

    FillNotification sell;
    assert(kis_websocket::decode_fill(FieldList(fill_record("01", "2")), sell) == Decode::kOk);
    assert(sell.side == OrderSide::SELL);

    // 접수 통보(1)는 체결이 아니다.
    FillNotification skip;
    assert(kis_websocket::decode_fill(FieldList(fill_record("02", "1")), skip) == Decode::kSkip);
    assert(skip.kis_order_no.empty());

    // 매매구분이 01/02 밖이면 원장에 못 넣는다 — 채우지 않고 돌려보낸다.
    FillNotification bad_side;
    assert(kis_websocket::decode_fill(FieldList(fill_record("03", "2")), bad_side) == Decode::kBadSide);
    assert(bad_side.kis_order_no.empty() && bad_side.side == OrderSide::NONE);

    // 수량·단가 변환 실패는 kBadNumber. 호출자가 버린다.
    auto other_fill_record = fill_record("02", "2");
    other_fill_record[9] = "";
    FillNotification bad_number;
    assert(kis_websocket::decode_fill(FieldList(other_fill_record), bad_number) == Decode::kBadNumber);

    // 1~2필드 제어 메시지와 13필드 모두 kShort.
    FillNotification short_fill;
    assert(kis_websocket::decode_fill(FieldList({"0", "1"}), short_fill) == Decode::kShort);
    assert(kis_websocket::decode_fill(FieldList(blank(13)), short_fill) == Decode::kShort);
}

int main()
{
    test_orderbook();
    test_kr_trade();
    test_us_trade();
    test_fill();
    std::cout << "test_ws_decode: all passed" << std::endl;
    return 0;
}
