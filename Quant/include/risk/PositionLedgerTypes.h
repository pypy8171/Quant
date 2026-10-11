#pragma once
#include "core/SymbolTable.h"
#include "core/Types.h"
#include "risk/Money.h"
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// PositionLedgerTypes — 주문 장부(PositionLedger)가 주고받는 값 묶음. 장부 클래스 선언과 나눠 둔다.
//  PositionLedger 안에서 같은 이름으로 다시 부른다(PositionLedger::FillResult 등) — 쓰는 쪽 코드는 그대로다. [why D-113]
// ─────────────────────────────────────────────────────────────────────────────
namespace position_ledger
{
// 미체결 선점 한 종목분. 매수와 매도를 따로 센다(둘 다 0 이상). 부호 있는 합 하나로 두던 때는
//  "매수 10·매도 3"이 +7로 뭉쳐, 매수 10이 체결돼 10을 풀면 0에서 멈추며 살아 있는 매도 3까지 지웠다 —
//  그 뒤 매도 가능 수량을 3주 부풀려 봤다.
struct Reservation
{
    int buy  = 0;
    int sell = 0;

    int net() const noexcept
    {
        return buy - sell;
    }

    bool empty() const noexcept
    {
        return buy == 0 && sell == 0;
    }
};

// 리플레이가 남긴 미결 주문 — INTENT는 적혔는데 체결·취소·거부로 닫히지 않은 것들. 엔진이 죽은 순간
//  거래소 호가창에 살아 있었을 주문이다. 재기동 대조(Engine::resolve_open_intents)가 KIS 미체결조회와
//  맞춰, 살아 있으면 라우터 이력에 되살리고(늦은 체결통보가 전략까지 이어진다) 없으면 선점을 푼다. [why D-113]
struct OpenIntent
{
    uint64_t    order_id         = 0;
    uint64_t    kis_order_number = 0; // ACCEPT를 못 본 주문은 0 — 접수됐는지조차 모른다는 뜻
    std::string account;
    std::string ticker;
    std::string strategy_name;
    OrderSide   side      = OrderSide::BUY;
    OrderType   type      = OrderType::MARKET;
    int         remaining = 0;   // 아직 안 닫힌 수량
    double      price     = 0.0;
    bool        accepted  = false;
};

// 주문 하나를 저널 레코드에 잇는 이름표 — 주문 사건(INTENT·ACCEPT·REJECT·FILL·CANCEL)이 같이 받는다.
struct OrderRef
{
    uint64_t  order_id         = 0; // OrderSignal.client_order_number
    uint64_t  kis_order_number = 0; // KIS ODNO 정수(접수 전 0)
    OrderType type             = OrderType::MARKET;
};

// 체결 확인 결과 — on_fill_confirmed가 돌려준다.
struct FillResult
{
    // 금액 칸은 원 단위 정수다. 반올림 규칙은 Quant/include/risk/Money.h.
    money::Won average_price = 0; // 갱신된 매수 평균단가(원, 체결마다 반올림)
    int        net_quantity  = 0; // 체결 후 순 보유수량
    money::Won commission    = 0; // 수수료 (0.015%, 반올림)
    money::Won tax           = 0; // 거래세 (매도 0.20%, 반올림)
    //  출처: 수수료 0.015%는 KIS 공시 온라인 수수료 KRX 0.0140527%·NXT 0.0130527%(유관기관 제비용 포함,
    //  securities.koreainvestment.com 수수료안내 TF04ae010000)를 올려 잡은 값. 거래세 0.20%는 2026-01-01 양도분부터
    //  코스피 거래세 0.05%+농어촌특별세 0.15%, 코스닥 0.20%(증권거래세법 시행령 개정, 헤럴드경제 biz.heraldcorp.com/article/10627001).
    //  둘 다 2026-09-27 확인.
    money::Won realized_pnl = 0; // 이번 체결 실현손익 (SELL만 양수)
    // SELL인데 장부가 평단을 모를 때 true. 그 경우 realized_pnl은 0으로 두고 daily_pnl에도
    //  더하지 않는다 — (price-0)*quantity가 이익으로 잡히면 일일 손실컷이 무력화된다.
    bool   basis_unknown = false;
    // strategy_id별 서브장부(전략별 손익 귀속, D-089) — 위 필드들과 계산은 독립이고
    //  daily_pnl_·kill switch 판정에는 안 들어간다. 참고용 집계만.
    money::Won strategy_realized_pnl  = 0; // 이번 체결의 strategy_id 기준 실현손익 (SELL만)
    bool   strategy_basis_unknown = false; // strategy_id가 비었거나 그 전략의 평단을 모를 때 true
};

// 장부 한 줄 — (계좌, 종목) 하나의 보유·선점·평단·현재가·매도가능·열린 시각. 칸마다 따로 두던 맵 아홉 개를
//  한 맵의 값으로 묶었다 — 체결 한 건이 맵 여섯 개를 따로 찾던 것을 한 번 찾기로 줄이고, 칸 하나를 지우고 다른 칸을
//  남기는 실수를 막는다. "없음"은 칸마다 정한 값으로 적는다(아래 주석). 모든 칸이 없음이면 줄을 지운다(empty()).
//  보유 0인 줄과 줄이 없는 것은 같게 읽는다 — 놓친 매도를 맞춘 줄(absorb_missed_sell)이 보유 0으로 남는다.
struct PositionRow
{
    using TimePoint = std::chrono::steady_clock::time_point;

    static constexpr int kUnknown = -1; // 매도가능·잔고 관측 칸의 "없음"

    int                      position         = 0;        // 실체결 순보유 수량(양수 = 롱)
    Reservation              reserved;                    // 미체결 선점(매수·매도 따로)
    money::Won               reserved_price   = 0;        // 매수 선점가(원, §3d 총노출). 0 = 없음. 매수 선점이 0이 되면 같이 0
    money::Won               average_price    = 0;        // 평단(원). 0 = 모름
    money::Won               mark_price       = 0;        // 잔고 현재가(원, §3d 총노출 분자). 0 = 없음
    int                      sellable         = kUnknown; // 매도가능수량(주). 모르면 보유수량으로 읽는다
    int                      missed_sell_seen = kUnknown; // 직전 대조에서 본 잔고 수량(놓친 매도 2회 연속 확인)
    int                      missed_buy_seen  = kUnknown; // 매수 쪽 같은 용도
    std::optional<TimePoint> opened_at;                   // 0에서 열린 시각(교체 최소 보유 판정)

    // 자리(슬롯)를 무는가 — 보유가 있거나, 보유 없이 매수 선점만 있다. 슬롯 면제는 부른 쪽이 따로 본다.
    bool takes_slot() const noexcept
    {
        return position > 0 || reserved.buy > 0;
    }

    // 매도가능수량, 모르면 fallback.
    int sellable_or(int fallback) const noexcept
    {
        return sellable >= 0 ? sellable : fallback;
    }

    bool empty() const noexcept
    {
        return position == 0 && reserved.empty() && reserved_price == 0 && average_price == 0 && mark_price == 0 &&
               sellable < 0 && missed_sell_seen < 0 && missed_buy_seen < 0 && !opened_at;
    }
};

// 장부에 없는 줄을 읽을 때 돌려주는 빈 줄.
inline const PositionRow kNoRow{};

// 매도가능수량이 0으로 잘린 이유를 로그에 남길 조각. 장부 보유·잔고 주문가능(시드/대조값)·
//  이 세션 미체결 매도(선점). 장부가 모르는 종목이면 held=0이고 나머지도 0이다.
struct SellableView
{
    int held     = 0;
    int possible_quantity_cap = 0;
    int pending  = 0;
};

// 보유 포지션 스냅샷 한 줄 — snapshot_positions()가 돌려준다.
// symbol은 장부 키의 종목 id — 강제청산·한도 정리가 미체결 잔량을 물을 때 문자열 대신 이 번호로 묻는다.
struct HeldPos
{
    std::string      account;
    std::string      ticker;
    int              quantity;
    double           average_price;
    symbol::SymbolId symbol      = symbol::kNone;
    bool             slot_exempt = false; // 슬롯 계산 밖 종목(바스켓 슬리브 소유). 강제청산·초과 정리는 이 종목을 건너뛴다 [why D-109]
};
} // namespace position_ledger
