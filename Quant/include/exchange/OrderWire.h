// 부하시험 주문 전문 — 파이썬 인젝터가 보내고 C++ 수신단이 읽는 바이트 모양. 양쪽이 이 파일 하나만 보고
//  맞춘다. 한쪽을 고치면 다른 쪽이 조용히 어긋나는 것을 막으려고 매직·판번호를 앞에 둔다.
// 전문 하나 = 묶음머리 16바이트 + 주문 32바이트 × N. ZMQ 메시지가 경계를 주므로 길이 필드는 두지 않고
//  크기에서 건수를 되짚는다. 같은 기계 안에서만 오가므로 리틀엔디언 그대로 싣는다.
//
// 파이썬 쪽 numpy 자료형은 이것과 한 바이트도 다르면 안 된다:
//   numpy.dtype([("order_id", "<u8"), ("symbol_index", "<u4"), ("quantity", "<i4"),
//                ("price_krw", "<i8"), ("side", "u1"), ("command", "u1"), ("reserved", "V6")])
#pragma once
#include "exchange/MatchingEngine.h"

#include <cstddef>
#include <cstdint>

namespace exchange
{

// 'QORD' 를 리틀엔디언 정수로 굳힌 값. 엉뚱한 소켓에서 온 바이트를 주문으로 읽는 것을 여기서 막는다.
inline constexpr uint32_t kOrderWireMagic = 0x44524F51;

// 전문 모양이 바뀌면 올린다. 판번호가 다르면 받는 쪽이 통째로 버린다.
inline constexpr uint16_t kOrderWireVersion = 1;

// 주문 한 건이 차지하는 바이트. 2.7억 건을 미리 만들어 두는 쪽이라 한 건의 크기가 곧 인젝터 메모리다.
inline constexpr size_t kOrderWireRecordBytes = 32;

// 묶음머리가 차지하는 바이트.
inline constexpr size_t kOrderWireHeaderBytes = 16;

// 받는 쪽이 할 일.
enum class WireCommand : uint8_t
{
    ACCUMULATE  = 0, // 맞추지 않고 쌓기만 한다(동시호가)
    MATCH       = 1, // 즉시 맞추고 남으면 쌓는다(연속매매)
    RUN_AUCTION = 2, // 쌓인 것을 단일가로 한 번에 맞춘다. 주문 내용은 안 본다
    CONFIGURE   = 3  // 그 종목의 기준가를 price_krw 로 정하고 호가 격자를 만든다. 주문을 보내기 전에 한 번
};

#pragma pack(push, 1)

// 전문 앞머리. 보낸 시각을 실어 두면 받는 쪽에서 전달 지연을 그대로 잴 수 있다.
struct OrderWireBatchHeader
{
    uint32_t magic        = kOrderWireMagic;
    uint16_t version      = kOrderWireVersion;
    uint16_t record_count = 0;
    uint64_t sent_unix_ns = 0;
};

// 주문 한 건.
struct OrderWireRecord
{
    uint64_t order_id = 0;
    // 종목 자체가 아니라 유니버스 목록의 몇 번째인지다(0부터). 보내는 쪽과 받는 쪽이 같은 목록을 같은 순서로
    //  읽는다는 약속 하나로 맞춘다 — 종목코드 문자열을 싣지 않으려고 그렇게 한다(원칙 6).
    uint32_t symbol_index = 0;
    int32_t  quantity     = 0;
    int64_t  price_krw    = 0; // 0이면 시장가 — kMarketOrderPrice 와 같은 약속이다
    uint8_t  side         = 0; // OrderSide::BUY=0, OrderSide::SELL=1 과 같은 값
    uint8_t  command      = 0; // WireCommand
    uint8_t  reserved[6]  = {0, 0, 0, 0, 0, 0};
};

#pragma pack(pop)

static_assert(sizeof(OrderWireBatchHeader) == kOrderWireHeaderBytes, "묶음머리는 16바이트여야 한다");
static_assert(sizeof(OrderWireRecord) == kOrderWireRecordBytes, "주문 한 건은 32바이트여야 한다");
static_assert(static_cast<uint8_t>(OrderSide::BUY) == 0, "전문의 side 값은 OrderSide 를 그대로 쓴다");
static_assert(static_cast<uint8_t>(OrderSide::SELL) == 1, "전문의 side 값은 OrderSide 를 그대로 쓴다");

// 읽어 낸 전문 한 통. records 는 넘겨받은 버퍼를 그대로 가리킨다.
//  [inv] 버퍼가 살아 있는 동안만 유효하다 — 복사하지 않는다(초당 수십만 건이 지나는 자리라).
struct OrderWireBatch
{
    OrderWireBatchHeader   header;
    const OrderWireRecord* records = nullptr;
    size_t                 count   = 0;
};

// 바이트를 전문으로 읽는다. 매직·판번호·길이가 안 맞으면 false — 받는 쪽은 그 통을 통째로 버린다.
bool decode_order_wire(const void* data, size_t size, OrderWireBatch& out_batch);

// 전문을 바이트로 적는다. 적은 바이트 수를 돌려주고, 자리가 모자라면 0. 시험·벤치가 쓴다.
size_t encode_order_wire(void* buffer, size_t capacity, const OrderWireRecord* records, size_t count,
                         uint64_t sent_unix_ns);

// 전문 한 건을 매칭 엔진이 아는 모양으로 바꾼다. 종목 순번을 엔진의 종목 id로 옮기는 것은 받는 쪽 몫이라
//  id를 따로 받는다.
IncomingOrder to_incoming_order(const OrderWireRecord& record, symbol::SymbolId symbol_id);

} // namespace exchange
