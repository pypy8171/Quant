// 주문 라우터 발주 경로의 공용 조각 — 신규(Quant/src/ipc/OrderRouterSubmit.cpp)와 취소·정정(OrderRouterModify.cpp)이
//  같이 쓰는 전송 예외 기록과 닫기 뒤 마무리. 라우터 cpp만 포함한다(우산 헤더 ipc/OrderRouter.h에는 넣지 않는다). [why D-151]
#pragma once
#include <exception>

namespace order_router
{

// 전송 호출의 예외를 경로에 적는다 — 접수 여부를 모르는 실패라 닫는 쪽이 transport_failed를 보고 마무리한다.
template <typename Route, typename Call>
void capture_transport(Route& route, Call&& call) noexcept
{
    try
    {
        call();
    }
    catch (const std::exception& exception)
    {
        route.transport_failed = true;
        route.transport_error  = exception.what();
    }
    catch (...)
    {
        route.transport_failed = true;
        route.transport_error  = "알 수 없는 예외";
    }
}

// 닫기를 하고, 성공이든 예외든 보내는 중 수(finish)를 한 번 내린다. 수를 안 내리면 그 뒤 연결 안 되는 체결을
//  끝없이 붙든다. 소멸자로 하지 않는 것은 finish가 던질 수 있어서다 — 소멸자에서 던지면 프로세스가 끝난다.
template <typename Close, typename Finish>
void close_then_finish(Close&& close, Finish&& finish)
{
    try
    {
        close();
    }
    catch (...)
    {
        finish();
        throw;
    }

    finish();
}

} // namespace order_router
