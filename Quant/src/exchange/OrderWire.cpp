// 부하시험 주문 전문 읽기·적기 — 바이트 모양과 약속은 include/exchange/OrderWire.h.
#include "exchange/OrderWire.h"

#include <cstring>

namespace exchange
{

bool decode_order_wire(const void* data, size_t size, OrderWireBatch& out_batch)
{
    if (data == nullptr || size < kOrderWireHeaderBytes)
    {
        return false;
    }

    // 정렬을 장담할 수 없는 버퍼라 통째로 읽지 않고 복사해 본다 — 묶음마다 한 번뿐이라 비용이 안 된다.
    OrderWireBatchHeader header;
    std::memcpy(&header, data, kOrderWireHeaderBytes);

    if (header.magic != kOrderWireMagic || header.version != kOrderWireVersion)
    {
        return false;
    }

    const size_t body_bytes = size - kOrderWireHeaderBytes;

    if (body_bytes % kOrderWireRecordBytes != 0)
    {
        return false;
    }

    const size_t count = body_bytes / kOrderWireRecordBytes;

    if (count != header.record_count)
    {
        return false;
    }

    out_batch.header = header;
    out_batch.records =
        reinterpret_cast<const OrderWireRecord*>(static_cast<const uint8_t*>(data) + kOrderWireHeaderBytes);
    out_batch.count = count;

    return true;
}

size_t encode_order_wire(void* buffer, size_t capacity, const OrderWireRecord* records, size_t count,
                         uint64_t sent_unix_ns)
{
    const size_t needed = kOrderWireHeaderBytes + count * kOrderWireRecordBytes;

    if (buffer == nullptr || capacity < needed)
    {
        return 0;
    }

    OrderWireBatchHeader header;
    header.magic        = kOrderWireMagic;
    header.version      = kOrderWireVersion;
    header.record_count = static_cast<uint16_t>(count);
    header.sent_unix_ns = sent_unix_ns;

    std::memcpy(buffer, &header, kOrderWireHeaderBytes);

    if (count > 0 && records != nullptr)
    {
        std::memcpy(static_cast<uint8_t*>(buffer) + kOrderWireHeaderBytes, records,
                    count * kOrderWireRecordBytes);
    }

    return needed;
}

IncomingOrder to_incoming_order(const OrderWireRecord& record, symbol::SymbolId symbol_id)
{
    IncomingOrder order;
    order.order_id  = record.order_id;
    order.symbol_id = symbol_id;
    order.price_krw = record.price_krw;
    order.quantity  = record.quantity;
    order.side      = record.side == static_cast<uint8_t>(OrderSide::SELL) ? OrderSide::SELL : OrderSide::BUY;

    return order;
}

} // namespace exchange
