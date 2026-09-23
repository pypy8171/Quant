#include "ipc/OpsProtocol.h"

namespace ops
{
const char* message_name(uint8_t message_type)
{
    switch (static_cast<OpsMsg>(message_type))
    {
    case OpsMsg::HELLO_REQ:
        return "HELLO_REQ";
    case OpsMsg::HELLO_ACK:
        return "HELLO_ACK";
    case OpsMsg::PING_REQ:
        return "PING_REQ";
    case OpsMsg::PING_ACK:
        return "PING_ACK";
    case OpsMsg::STATUS_REQ:
        return "STATUS_REQ";
    case OpsMsg::STATUS_ACK:
        return "STATUS_ACK";
    case OpsMsg::POSITIONS_REQ:
        return "POSITIONS_REQ";
    case OpsMsg::POSITIONS_ACK:
        return "POSITIONS_ACK";
    case OpsMsg::POSITIONS_NTF:
        return "POSITIONS_NTF";
    case OpsMsg::ORDER_REQ:
        return "ORDER_REQ";
    case OpsMsg::ORDER_ACK:
        return "ORDER_ACK";
    case OpsMsg::ORDER_RESULT_NTF:
        return "ORDER_RESULT_NTF";
    case OpsMsg::FILL_NTF:
        return "FILL_NTF";
    case OpsMsg::KILL_REQ:
        return "KILL_REQ";
    case OpsMsg::KILL_ACK:
        return "KILL_ACK";
    case OpsMsg::HALT_REQ:
        return "HALT_REQ";
    case OpsMsg::HALT_ACK:
        return "HALT_ACK";
    case OpsMsg::SHUTDOWN_REQ:
        return "SHUTDOWN_REQ";
    case OpsMsg::SHUTDOWN_ACK:
        return "SHUTDOWN_ACK";
    case OpsMsg::ERROR_NTF:
        return "ERROR_NTF";
    }

    return "?";
}

std::vector<uint8_t> encode(OpsMsg type, const std::string& body)
{
    if (body.size() > kMaxBody)
    {
        return {};
    }

    const uint32_t count = static_cast<uint32_t>(body.size());
    std::vector<uint8_t> out;
    out.reserve(kHeaderLen + count);
    out.push_back(kMagic0);
    out.push_back(kMagic1);
    out.push_back(kVersion);
    out.push_back(static_cast<uint8_t>(type));
    out.push_back(static_cast<uint8_t>((count >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((count >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((count >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(count & 0xFF));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

void FrameReader::feed(const uint8_t* data, size_t length)
{
    if (bad_)
    {
        return;
    }

    buffer_.insert(buffer_.end(), data, data + length);
}

bool FrameReader::next(Frame& out)
{
    if (bad_ || buffer_.size() < kHeaderLen)
    {
        return false;
    }

    if (buffer_[0] != kMagic0 || buffer_[1] != kMagic1 || buffer_[2] != kVersion)
    {
        bad_ = true;
        return false;
    }

    const uint32_t count = (static_cast<uint32_t>(buffer_[4]) << 24) | (static_cast<uint32_t>(buffer_[5]) << 16) |
                           (static_cast<uint32_t>(buffer_[6]) << 8) | static_cast<uint32_t>(buffer_[7]);

    if (count > kMaxBody)
    {
        bad_ = true;
        return false;
    }

    if (buffer_.size() < kHeaderLen + count)
    {
        return false;
    }

    out.type = buffer_[3];
    out.body.assign(reinterpret_cast<const char*>(buffer_.data() + kHeaderLen), count);
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(kHeaderLen + count));
    return true;
}

} // namespace ops
