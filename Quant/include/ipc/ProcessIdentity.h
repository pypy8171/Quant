// 프로세스 하나를 번호가 아니라 "번호 + 기동 시각" 한 쌍으로 가리킨다. 번호만으로 물으면 죽은 뒤 그 번호를
//  물려받은 남을 그 프로세스라고 잘못 읽는다 — 공유 쪽지의 주인이 아직 사는지 묻는 자리가 그 오독에 걸린다.
//  기동 시각까지 같아야 같은 프로세스다. [why D-114]
#pragma once

#include <cstdint>

namespace ipc
{

// 프로세스를 가리키는 표. 공유 쪽지 머리에 그대로 실리므로 고정 크기 정수만 둔다.
//  [inv] start_time 의 뜻은 운영체제마다 다르다(Windows: 만든 시각 FILETIME 100ns 단위, 리눅스: 부팅 뒤 클럭 틱).
//   같은 기계 안에서 같은가만 보고 해석하지 않는다 — 기계가 바뀌면 값도 바뀐다.
struct ProcessIdentity
{
    uint32_t process_id = 0;
    uint32_t reserved0  = 0;
    uint64_t start_time = 0; // 0 = 못 읽었다. 그때는 번호만으로 가린다

    [[nodiscard]] bool is_set() const noexcept
    {
        return process_id != 0;
    }

    [[nodiscard]] bool same_as(const ProcessIdentity& other) const noexcept
    {
        // 한쪽이라도 기동 시각을 못 읽었으면 번호만 본다 — 못 읽은 것을 "다르다"로 세면 산 주인을 죽었다고 본다.
        return process_id == other.process_id &&
               (start_time == 0 || other.start_time == 0 || start_time == other.start_time);
    }
};

// 지금 프로세스의 표.
[[nodiscard]] ProcessIdentity current_process_identity() noexcept;

// 그 프로세스가 아직 사는지 운영체제에 묻는다. 번호가 같아도 기동 시각이 다르면 죽은 것으로 본다.
//  표가 비어 있으면(번호 0) 거짓이다.
[[nodiscard]] bool process_is_alive(const ProcessIdentity& identity) noexcept;

} // namespace ipc
