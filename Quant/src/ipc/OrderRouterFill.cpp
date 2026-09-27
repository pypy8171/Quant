// 주문 라우터 — 체결통보 반영, 재연결 뒤 놓친 체결 되찾기, 일별 리셋.
#include "ipc/OrderRouter.h"
#include "core/KstTime.h"
#include "utils/Logger.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include "utils/ThreadName.h"

#include <ctime>
#include <format>
#include <fstream>
#include <string_view>
#include <vector>

// 거래일 YYYYMMDD 정수 — 체결통보 키의 날짜 칸. 문자열을 만들지 않는다.
static uint32_t trade_date_number(std::time_t now_utc)
{
    const std::chrono::year_month_day date = kst::date(now_utc);
    return static_cast<uint32_t>(static_cast<int>(date.year())) * 10000u + static_cast<unsigned>(date.month()) * 100u +
           static_cast<unsigned>(date.day());
}

// 체결 키의 단가 칸 — 원 단가를 100배해 정수로 담는다(소수 둘째 자리까지 구분).
static constexpr int kPriceCentsPerWon = 100;

void OrderRouter::load_order_reasons_locked()
{
    if (order_reasons_loaded_)
    {
        return;
    }

    order_reasons_loaded_ = true;
    std::ifstream in(Logger::instance().path_for("order_reasons_" + kst::date_yyyymmdd(std::time(nullptr)) + ".txt"));

    if (!in)
    {
        return;
    }

    std::string line;
    int count = 0;

    while (std::getline(in, line))
    {
        std::vector<std::string> fields;
        std::string token;
        std::istringstream stream(line);

        while (std::getline(stream, token, '|'))
        {
            fields.push_back(std::move(token));
        }

        if (fields.size() < 8 || fields[0].empty())
        {
            continue;
        }

        OrderReason order_reason;
        order_reason.ticker      = std::move(fields[1]);
        order_reason.side        = OrderSide::from_string(fields[2]);
        order_reason.strategy_id = std::move(fields[6]);
        order_reason.reason      = std::move(fields[7]);

        try
        {
            order_reason.quantity  = std::stoi(fields[3]);
            order_reason.price     = std::stod(fields[4]);
            order_reason.reference_price = std::stod(fields[5]);
        }
        catch (const std::exception&)
        {
            continue;   // 숫자가 깨진 줄은 버린다
        }

        if (order_reason.quantity <= 0)
        {
            continue;
        }

        const uint64_t order_number = digits_to_number(fields[0]);

        if (order_number == 0)
        {
            continue;
        }

        order_reasons_[order_number] = std::move(order_reason);   // 같은 ODNO가 여러 줄이면 마지막 것이 맞다
        ++count;
    }

    if (count > 0)
    {
        LOG_INFO("[OrderRouter] 주문 사유 기록 " + std::to_string(count) + "건 복원");
    }
}


// ─── 체결통보 처리 — ODNO 매핑 → 부분/전량 체결 처리 ─────────────────────
//  단계: 재전송 거르기 → 이전 세션 주문 되살리기 → 연결된 주문 찾기 → 연결 체결 / 미연결 체결.
void OrderRouter::on_fill(const FillNotification& fill_notification)
{
    // unique_lock: 원장 갱신까지만 잡고, 파일 쓰기·publish 전에 푼다(W-8).
    std::unique_lock<std::mutex> lock(history_mutex_);
    // 중복 제거 — KIS 체결통보는 at-least-once(재전송/WS 재구독 시 중복 가능).
    // H0STCNI0 전문에 체결고유번호가 없어 kis_order_no+체결시각+수량+단가를 조합 키로 사용.
    // ODNO는 영업일 단위 재사용되고 fill_time은 HHMMSS(날짜 없음)라, 거래일(수신일)을
    // prefix로 붙여, 서로 다른 날의 동일키 충돌로 실체결을 오인해 drop하는 일을 막는다 (V-4).
    const uint64_t order_number = digits_to_number(fill_notification.kis_order_no); // 전문 문자열이 정수가 되는 자리
    const FillKey  fill_key{trade_date_number(std::chrono::system_clock::to_time_t(fill_notification.timestamp)), order_number,
                           static_cast<uint32_t>(digits_to_number(fill_notification.fill_time)), fill_notification.filled_quantity,
                           static_cast<int64_t>(fill_notification.filled_price * kPriceCentsPerWon)};

    // 이 키는 유일하지 않다. 같은 초에 같은 수량·단가로 나뉘어 체결되면 서로 다른 실체결이
    //  같은 키를 갖는다(2026-09-07 ODNO 0000014893, 같은 초 2주 두 건 중 하나가 버려졌다). 그렇다고
    //  키가 겹치는 통보를 전부 받으면 재연결 뒤 재전송도 실체결로 쌓인다 — 잔량 상한은 총량만 막아
    //  체결가·시각 귀속이 틀어지고, 잔량이 취소되면 유령 보유가 남는다. 둘은 실어 온 세션으로 가른다.
    if (is_replayed_fill_locked(fill_key, fill_notification.session_generation))
    {
        lock.unlock();
        LOG_WARN(std::format("[OrderRouter] 재연결 뒤 같은 체결통보 — 재전송으로 보고 원장에 안 넣음 ODNO={} {} {}주 @{} time={} 세션={} (수량은 잔고 대조가 맞춘다)",
                             fill_notification.kis_order_no, fill_notification.ticker, fill_notification.filled_quantity,
                             static_cast<int>(fill_notification.filled_price), fill_notification.fill_time,
                             fill_notification.session_generation));
        return;
    }

    if (order_number != 0)
    {
        restore_from_order_reason_locked(fill_notification, order_number);
    }

    ManagedOrder* matched = find_linked_order_locked(fill_notification, order_number);

    // 부분체결: ACCEPTED(최초) 또는 FILLED(분할 진행 중) 모두 허용. 그 밖의 상태(취소·거부)는 미매핑 경로로.
    if (matched && (matched->status == OrderStatus::ACCEPTED || matched->status == OrderStatus::FILLED))
    {
        ManagedOrder& managed_order = *matched;

        // 조회로 이미 되찾은 체결의 통보가 늦게 왔으면 그만큼은 원장에 다시 넣지 않는다. [why D-149]
        const int credited          = consume_recovered_credit_locked(managed_order, fill_notification);
        const int incoming_quantity = fill_notification.filled_quantity - credited;

        if (credited > 0)
        {
            const int credit_left = managed_order.recovered_credit_quantity;

            if (incoming_quantity <= 0)
            {
                lock.unlock();
            }

            LOG_INFO(std::format("[OrderRouter] 되찾은 체결의 늦은 통보 — {}주는 원장에 다시 안 넣음 ODNO={} {} 통보={}주 time={} (남은 몫 {}주)",
                                 credited, fill_notification.kis_order_no, fill_notification.ticker,
                                 fill_notification.filled_quantity, fill_notification.fill_time, credit_left));

            if (incoming_quantity <= 0)
            {
                return;
            }
        }

        // 이미 주문수량을 다 채운 주문의 추가 통보다. 이걸 아래 미매핑 경로로 흘려보내면 같은 체결이
        //  포지션에 두 번 쌓인다(ODNO는 아는데 잔량만 없는 상태). 재처리하지 않는다.
        if (managed_order.confirmed_quantity >= managed_order.signal.quantity)
        {
            LOG_WARN(std::format("[OrderRouter] 주문수량 충족 후 추가 체결통보 무시 ODNO={} {} {}주 (통보 재전송 추정)", fill_notification.kis_order_no,
                                 fill_notification.ticker, fill_notification.filled_quantity));
            return;
        }

        apply_linked_fill(lock, managed_order, fill_notification, incoming_quantity, std::string_view());
        return;
    }

    apply_unlinked_fill(lock, fill_notification, fill_key, order_number);
}

// 재기동 복원 — 이전 세션이 낸 주문이면 접수 때 남긴 사유 기록에서 되살린다.
//  history_는 메모리라 재기동으로 비지만 기록 파일에는 ODNO·종목·수량·전략·사유가
//  그대로 있다. 되살려 history_에 넣으면 연결 체결 경로가 잔량 클램프까지 평소대로
//  처리하므로, 전략 귀속을 잃는 미매핑 경로로 빠지지 않는다.
void OrderRouter::restore_from_order_reason_locked(const FillNotification& fill_notification, uint64_t order_number)
{
    if (!order_reasons_loaded_)
    {
        // 읽기 전에 줄 서 있는 사유를 디스크에 내린다 — 큐에 남은 줄은 아직 파일에 없어서다.
        //  아래 호출이 세션당 한 번만 읽으므로 이 비용도 한 번뿐이다. [why D-124]
        journal_.flush_append_outbox();
    }

    load_order_reasons_locked();   // 첫 체결통보 때 1회만 파일을 읽는다
    const bool known  = history_.find_by_order_number(order_number) != nullptr;
    auto       jitter = known ? order_reasons_.end() : order_reasons_.find(order_number);

    if (jitter == order_reasons_.end())
    {
        return;
    }

    OrderReason& order_reason = jitter->second; // 사유 기록은 아래에서 지우므로 옮겨 온다
    OrderSignal  signal;
    signal.ticker          = std::move(order_reason.ticker);
    signal.side            = order_reason.side;
    signal.type            = OrderType::LIMIT;
    signal.quantity        = order_reason.quantity;
    signal.price           = order_reason.price;
    signal.reference_price = order_reason.reference_price;
    signal.strategy_id     = std::move(order_reason.strategy_id);
    signal.reason          = std::move(order_reason.reason);
    ManagedOrder record    = make_restored_order(next_id(), fill_notification.kis_order_no, std::move(signal),
                                                 fill_notification.timestamp);
    // 선점(reserved_)은 이전 세션과 함께 사라졌다. 연결 체결 처리가
    //  on_fill_confirmed로 선점을 깎으므로, 주문수량만큼 먼저 되살려 순변화를 맞춘다.
    //  일부만 체결되고 나머지가 취소되면 그만큼 선점이 남는데, 주기 잔고 대조의
    //  reset_reserved()가 실제 잔고로 되맞춘다.
    (void)gate_.ledger().on_intent(record.signal.account_id, record.signal.ticker, record.signal.side,
                                   record.signal.quantity,
                                   record.signal.price > 0.0 ? record.signal.price : record.signal.reference_price,
                                   OrderGate::OrderRef{digits_to_number(record.order_id), order_number, record.signal.type},
                                   record.signal.strategy_index);
    LOG_INFO("[OrderRouter] 재기동 복원 [" + record.order_id + "] ODNO=" + fill_notification.kis_order_no + " " +
             record.signal.ticker +
             (record.signal.side == OrderSide::BUY ? " BUY " : " SELL ") +
             std::to_string(record.signal.quantity) + "주 전략=" + record.signal.strategy_id +
             " (주문 사유 기록에서 복구)");
    history_.push(std::move(record));
    order_reasons_.erase(jitter);   // 같은 ODNO를 두 번 되살리지 않는다
}

ManagedOrder* OrderRouter::find_linked_order_locked(const FillNotification& fill_notification, uint64_t order_number)
{
    ManagedOrder* matched = history_.find_by_order_number(order_number); // ODNO 색인 한 번 — 이력을 훑지 않는다

    if (matched)
    {
        return matched;
    }

    // 원주문번호로 한 번 더 찾는다. 정정이 나가면 KIS가 새 ODNO를 주고 이력의 ODNO를 그 값으로 바꾸는데,
    //  정정 응답을 못 받으면(전송 실패·타임아웃) 이력에는 옛 ODNO가 남는다. 그 뒤 체결통보는 새 ODNO로
    //  오므로 위 색인이 비고, 전략 귀속을 잃은 채 미매핑 경로로 떨어진다. 전문 [3]OODER_NO가 그 옛 ODNO라
    //  여기서 되찾는다. 원장에 쓰는 번호는 통보가 준 실제 ODNO 그대로다(바꾸지 않는다).
    const uint64_t original_order_number = digits_to_number(fill_notification.original_order_no);

    if (original_order_number == 0 || original_order_number == order_number)
    {
        return nullptr;
    }

    matched = history_.find_by_order_number(original_order_number);

    if (matched)
    {
        LOG_WARN(std::format("[OrderRouter] 원주문번호로 체결 연결 [{}] 통보ODNO={} 원주문ODNO={} {} {}주 (정정 응답 유실 추정)",
                             matched->order_id, fill_notification.kis_order_no, fill_notification.original_order_no,
                             fill_notification.ticker, fill_notification.filled_quantity));
    }

    return matched;
}

// ── ODNO 미매핑 체결 — 이 프로세스가 낸 주문이 아니다 ────────────────────
//  history_는 메모리에만 있어서 장중 재시작하면 이전 세션의 미체결 주문이 사라진다.
//  거래소 호가창에는 그 주문이 그대로 살아있으므로, 나중에 체결되면 여기로 떨어진다.
//  2026-09-07 ODNO 0000014893이 이 경우다 — 09:58 접수, 10:38 재시작, 11:07 91주 전량
//  체결이 통째로 버려져 원장·포지션이 91주(약 498만원) 어긋났다.
//  체결 자체는 실재하므로 버리지 않고 원장·포지션에 반영한다. 전략 귀속만 알 수 없어
//  strategy_id를 "UNLINKED"로 남긴다(사후 분석에서 구분 가능).
//  선점(reserved_)은 이전 세션과 함께 사라졌다. on_fill_confirmed는 선점 해제를 전제로
//  reserved_를 깎으므로, 그대로 부르면 음수 선점이 생겨 이후 한도 계산이 왜곡된다.
//  같은 수량을 on_intent로 먼저 되살린 뒤 해제시켜 순변화를 0으로 맞춘다(원장에도 INTENT→FILL 두 줄로 남는다).
//  미연결은 history_에 없어 우리 쪽 주문수량을 모른다. 대신 전문 [16]ODER_QTY가 그 주문의 총수량이라,
//  있으면 연결된 주문과 같은 방식으로 누적 체결을 그 수량까지 묶는다. 상한이 키가 아니라 수량이 되므로
//  같은 초·같은 수량·단가로 갈라진 진짜 분할체결도 잃지 않는다(종전 W-6의 손실을 되돌린다).
//  전문이 그 칸을 안 주면(order_quantity==0) 종전대로 키(거래일:ODNO:시각:수량:단가) 중복 제거로 막는다 —
//  분할체결을 잃을 수 있지만 두 번 쌓는 쪽이 더 큰 사고다(W-6).
//  주문수량이 이번 통보 수량보다 작으면 전문을 믿지 않는다(칸이 밀렸거나 뜻이 다른 값).
void OrderRouter::apply_unlinked_fill(std::unique_lock<std::mutex>& lock, const FillNotification& fill_notification,
                                      const FillKey& fill_key, uint64_t order_number)
{
    auto& ledger = gate_.ledger();

    // [inv] 주문 단위 키 = 체결 건별 칸(시각·수량·단가)을 0으로 둔 FillKey. ODNO는 영업일마다 재사용되므로
    //  거래일을 같이 담는다(체결 건별 키와 같은 이유, V-4).
    const FillKey  unlinked_order_key{fill_key.trade_date, fill_key.order_number, 0, 0, 0};
    UnlinkedOrder& unlinked_order    = unlinked_orders_[unlinked_order_key];
    int            unlinked_quantity = fill_notification.filled_quantity;

    if (fill_notification.order_quantity >= fill_notification.filled_quantity && fill_notification.order_quantity > 0)
    {
        unlinked_order.order_quantity = fill_notification.order_quantity;
    }

    if (unlinked_order.order_quantity > 0)
    {
        const int outstanding = unlinked_order.order_quantity - unlinked_order.confirmed_quantity;

        if (outstanding <= 0)
        {
            LOG_WARN(std::format("[OrderRouter] 미매핑 주문수량 충족 후 추가 체결통보 무시 ODNO={} {} {}주 (주문수량 {}주 전량 반영 완료)",
                                 fill_notification.kis_order_no, fill_notification.ticker,
                                 fill_notification.filled_quantity, unlinked_order.order_quantity));
            return;
        }

        if (unlinked_quantity > outstanding)
        {
            LOG_WARN(std::format("[OrderRouter] 미매핑 주문잔량 초과 체결통보 — 잔량으로 클램프 ODNO={} {} 통보={}주 잔량={}주",
                                 fill_notification.kis_order_no, fill_notification.ticker,
                                 fill_notification.filled_quantity, outstanding));
            unlinked_quantity = outstanding;
        }
    }
    else if (!unlinked_fill_keys_.insert(fill_key).second)
    {
        LOG_WARN(std::format("[OrderRouter] 미매핑 체결 재통보 무시 ODNO={} {} {}주 time={} (주문수량 미상 — 같은 키 재수신)",
                             fill_notification.kis_order_no, fill_notification.ticker,
                             fill_notification.filled_quantity, fill_notification.fill_time));
        return;
    }

    unlinked_order.confirmed_quantity += unlinked_quantity;

    ManagedOrder unlinked_fill;
    unlinked_fill.order_id              = next_id();
    unlinked_fill.kis_order_no          = fill_notification.kis_order_no;
    unlinked_fill.status                = OrderStatus::FILLED;
    unlinked_fill.confirmed_quantity    = unlinked_quantity;
    unlinked_fill.signal.strategy_id    = "UNLINKED";
    unlinked_fill.signal.strategy_index = unlinked_strategy_index_;
    unlinked_fill.signal.ticker         = fill_notification.ticker;
    unlinked_fill.signal.side           = fill_notification.side;
    unlinked_fill.signal.type           = OrderType::LIMIT;
    unlinked_fill.signal.quantity       = unlinked_quantity;
    unlinked_fill.signal.price          = fill_notification.filled_price;
    unlinked_fill.signal.reason         = "이전 세션 주문 체결(ODNO 미매핑)";
    unlinked_fill.submitted_at          = fill_notification.timestamp;
    unlinked_fill.updated_at            = fill_notification.timestamp;

    const int unlinked_order_quantity = unlinked_order.order_quantity; // 로그용 — unlinked_orders_ 원소는 락 밖에서 안 읽는다

    const OrderGate::OrderRef unlinked_reference{digits_to_number(unlinked_fill.order_id), order_number,
                                                 unlinked_fill.signal.type};
    (void)ledger.on_intent(unlinked_fill.signal.account_id, fill_notification.ticker, fill_notification.side,
                           unlinked_quantity, fill_notification.filled_price, unlinked_reference,
                           unlinked_fill.signal.strategy_index);
    const auto result = ledger.on_fill_confirmed(unlinked_fill.signal.account_id, fill_notification.ticker, fill_notification.side,
                                                 unlinked_quantity, fill_notification.filled_price,
                                                 unlinked_fill.signal.strategy_index, unlinked_reference);
    lock.unlock(); // 원장 갱신 끝 — 파일 쓰기는 락 밖에서

    LOG_WARN(std::format("[OrderRouter] 미매핑 체결 원장 반영 [{}] ODNO={} {} {} {}주 @{} (주문수량 {}) — 이전 세션 주문으로 추정(재시작 전 접수분)",
                         unlinked_fill.order_id, fill_notification.kis_order_no, fill_notification.ticker, fill_notification.side == OrderSide::BUY ? "BUY" : "SELL",
                         unlinked_quantity, static_cast<int>(fill_notification.filled_price),
                         unlinked_order_quantity > 0 ? std::to_string(unlinked_order_quantity) + "주" : std::string("미상")));

    emit_fill(unlinked_fill, fill_notification, unlinked_quantity, result);
    publish_fill_result(fill_notification, unlinked_fill.signal.strategy_id, result);
}

// ─── 연결된 주문에 체결 반영 ─────────────────────────────────────────────────
//  체결통보와 조회로 되찾은 체결이 같은 길을 탄다 — 잔량 상한·원장·원장 CSV·미결 파일·발행이 한 곳에 있어야
//  둘의 결과가 어긋나지 않는다.
void OrderRouter::apply_linked_fill(std::unique_lock<std::mutex>& lock, ManagedOrder& managed_order,
                                    const FillNotification& fill_notification, int incoming_quantity, std::string_view note)
{
    auto& ledger = gate_.ledger();

    // 주문 잔량 상한 — 누적 체결이 주문수량을 넘지 못하게 클램프한다.
    //  통보 재전송으로 같은 체결이 두 번 와도 과체결로 원장이 부풀지 않는다.
    const int outstanding    = outstanding_of(managed_order);
    const int apply_quantity = (incoming_quantity > outstanding) ? outstanding : incoming_quantity;

    managed_order.confirmed_quantity += apply_quantity;
    managed_order.confirmed_amount   += std::llround(apply_quantity * fill_notification.filled_price);
    managed_order.updated_at          = fill_notification.timestamp;

    if (managed_order.confirmed_quantity >= managed_order.signal.quantity)
    {
        managed_order.status = OrderStatus::FILLED;
    }

    // 포지션 원장 갱신 (average_price 재계산 + 실현손익) — 원주문의 계좌로 파티션.
    // 현재는 단일 CANO 전제라 ODNO가 유일 → managed_order.signal.account_id 매핑이 정확하다.
    // TODO(다계좌): 진짜 다중 CANO 라우팅 시 ODNO가 계좌별로 재사용되므로 체결 매칭 키를
    //   (kis_order_no + account) 또는 CANO별 H0STCNI 피드 분리로 확장해야 오적립을 막는다.
    const uint64_t order_number = digits_to_number(fill_notification.kis_order_no);
    auto result = ledger.on_fill_confirmed(managed_order.signal.account_id, fill_notification.ticker, fill_notification.side,
                                          apply_quantity, fill_notification.filled_price, managed_order.signal.strategy_index,
                                          OrderGate::OrderRef{digits_to_number(managed_order.order_id), order_number,
                                                              managed_order.signal.type});

    // 락 밖에서 쓰려고 복사한다 — managed_order는 history_ 원소라 record()의 축출로 참조가 죽을 수 있다.
    ManagedOrder   snapshot    = managed_order;
    std::string    open_orders = snapshot_open_orders_locked(); // 잔량이 줄었으니 부속 파일을 다시 쓴다
    const uint64_t sequence    = ++open_orders_sequence_;
    lock.unlock();

    if (!note.empty())
    {
        snapshot.reject_reason = std::string(note); // 원장 CSV 사유 칸 — 사본에만 적는다
    }

    // 로그 문장은 락을 푼 뒤 사본으로 만든다 — 체결마다 도는 자리라 history_mutex_를 잡은 채 문자열을
    //  잇지 않는다(CODE_REVIEW S-3).
    if (apply_quantity < incoming_quantity)
    {
        LOG_WARN(std::format("[OrderRouter] 주문잔량 초과 체결통보 — 잔량으로 클램프 [{}] ODNO={} 통보={}주 잔량={}주",
                             snapshot.order_id, fill_notification.kis_order_no, incoming_quantity, outstanding));
    }

    LOG_INFO(std::format("[OrderRouter] 체결 확인 [{}] ODNO={} {} {} {}주 @{} (누적 {}/{}주){}", snapshot.order_id,
                         fill_notification.kis_order_no, fill_notification.ticker,
                         fill_notification.side == OrderSide::BUY ? "BUY" : "SELL", apply_quantity,
                         static_cast<int>(fill_notification.filled_price), snapshot.confirmed_quantity,
                         snapshot.signal.quantity, note.empty() ? std::string() : " — " + std::string(note)));

    // 거래 원장 CSV — 실제 체결(부분/전량)을 한 줄로 영속화. 실현손익을 같이 남기려고
    //   gate_.ledger().on_fill_confirmed() 뒤에 쓴다(managed_order.status는 위에서 이미 갱신됨).
    emit_fill(snapshot, fill_notification, apply_quantity, result);
    journal_.queue_open_orders_file(std::move(open_orders), sequence);
    publish_fill_result(fill_notification, snapshot.signal.strategy_id, result);
}

void OrderRouter::emit_fill(const ManagedOrder& fill_order, const FillNotification& fill_notification, int quantity,
                            const PositionLedger::FillResult& result)
{
    if (result.basis_unknown)
    {
        LOG_WARN(std::format("[OrderRouter] 평단 미상 SELL 체결 — 실현손익 미산정(0) [{}] {} {}주 @{} (원장 재시드 필요)",
                             fill_order.order_id, fill_notification.ticker, quantity, static_cast<int>(fill_notification.filled_price)));
    }

    journal_.write_trade_row("FILL", fill_order, quantity, fill_notification.filled_price, result.realized_pnl,
                             result.strategy_realized_pnl);
}

void OrderRouter::publish_fill_result(const FillNotification& fill_notification, const std::string& strategy_id,
                                      const PositionLedger::FillResult& result)
{
#ifdef HAS_ZMQ
    if (zmq_)
    {
        zmq_->publish_fill(fill_notification, strategy_id, result.commission, result.tax, result.average_price,
                           result.net_quantity, result.realized_pnl);
    }
#else
    (void)fill_notification;
    (void)strategy_id;
    (void)result;
#endif
}

int OrderRouter::consume_recovered_credit_locked(ManagedOrder& managed_order, const FillNotification& fill_notification)
{
    if (managed_order.recovered_credit_quantity <= 0)
    {
        return 0;
    }

    // 조회 시각 뒤에 난 체결은 조회에 없던 것이라 몫에 들지 않는다. 시각을 못 읽으면 몫에 넣지 않는다 —
    //  잘못 깎으면 실체결이 사라지고, 잘못 넣으면 잔고 대조가 맞춘다.
    const auto fill_hhmmss = static_cast<uint32_t>(digits_to_number(fill_notification.fill_time));

    if (fill_hhmmss == 0 || fill_hhmmss > managed_order.recovered_until_hhmmss)
    {
        return 0;
    }

    const int credited = std::min(managed_order.recovered_credit_quantity, fill_notification.filled_quantity);
    managed_order.recovered_credit_quantity -= credited;
    return credited;
}

// ─── 체결통보 구독 재개 — 놓친 체결 되찾기 ─────────────────────────────────
void OrderRouter::on_session_resumed(uint32_t session_generation)
{
    {
        std::lock_guard<std::mutex> lock(fill_recovery_mutex_);
        ++fill_recovery_requests_;
    }

    fill_recovery_wake_.notify_one();
    LOG_INFO(std::format("[OrderRouter] 체결통보 구독 확인(세션 {}) — 잠시 뒤 끊긴 사이 체결을 조회로 맞춘다", session_generation));
}

void OrderRouter::fill_recovery_loop(std::stop_token stop_token)
{
    // KIS가 재구독 뒤 밀린 통보를 다시 보내면 그것이 먼저 들어오게 기다린다. 그러면 조회 차이는 0이고,
    //  늦게 오는 통보는 되찾은 몫이 막는다.
    constexpr auto kSettleDelay = std::chrono::seconds(3);

    thread_name::set_current("FillRecovery");
    uint64_t                     handled = 0;
    std::unique_lock<std::mutex> lock(fill_recovery_mutex_);

    while (!stop_token.stop_requested())
    {
        const bool requested = fill_recovery_wake_.wait(lock, stop_token, [&]
        {
            return fill_recovery_requests_ != handled;
        });

        if (!requested)
        {
            break; // 멈춤 요청
        }

        (void)fill_recovery_wake_.wait_for(lock, stop_token, kSettleDelay, []
        {
            return false;
        });

        if (stop_token.stop_requested())
        {
            break;
        }

        handled = fill_recovery_requests_; // 기다리는 사이 온 요청도 이번 조회 한 번이 맡는다
        lock.unlock();

        try
        {
            (void)recover_missed_fills();
        }
        catch (const std::exception& exception)
        {
            LOG_WARN("[OrderRouter] 놓친 체결 조회 실패 — 잔고 대조가 수량을 맞춘다: " + std::string(exception.what()));
        }

        lock.lock();
    }
}

int OrderRouter::recover_missed_fills()
{
    {
        std::lock_guard<std::mutex> lock(history_mutex_);
        const bool any_open = std::any_of(history_.begin(), history_.end(), [](const ManagedOrder& managed_order)
        {
            return managed_order.recoverable && managed_order.status == OrderStatus::ACCEPTED;
        });

        if (!any_open)
        {
            return 0; // 이 프로세스가 낸 미결 주문이 없다 — 조회하지 않는다
        }
    }

    // 조회를 보내기 전 시각이다. 이보다 늦은 체결은 조회에 없을 수 있어 되찾은 몫에 넣지 않는다.
    constexpr int64_t kHourPlace   = 10'000; // HHMMSS의 시 자리
    constexpr int64_t kMinutePlace = 100;    // HHMMSS의 분 자리

    const auto     clock        = kst::time_of_day(std::time(nullptr));
    const uint32_t until_hhmmss = static_cast<uint32_t>(clock.hours().count() * kHourPlace +
                                                        clock.minutes().count() * kMinutePlace + clock.seconds().count());
    const auto     daily_fills  = kis_.get_daily_order_fills();

    if (!daily_fills)
    {
        LOG_WARN("[OrderRouter] 놓친 체결 조회 실패 — 잔고 대조가 수량을 맞춘다: " + error_text(daily_fills));
        return 0;
    }

    int recovered_orders   = 0;
    int recovered_quantity = 0;

    for (const auto& row : *daily_fills)
    {
        std::unique_lock<std::mutex> lock(history_mutex_);
        ManagedOrder* managed_order = history_.find_by_order_number(digits_to_number(row.kis_order_no));

        // 재기동 복원 주문·취소·정정 전 주문은 건너뛴다 — 잔고 시드·취소 해제와 겹쳐 두 번 센다. 잔고 대조가 맡는다.
        if (managed_order == nullptr || !managed_order->recoverable || managed_order->status != OrderStatus::ACCEPTED)
        {
            continue;
        }

        const int missing = std::min(row.filled_quantity - managed_order->confirmed_quantity, outstanding_of(*managed_order));

        if (missing <= 0)
        {
            continue;
        }

        if (row.side != managed_order->signal.side || row.ticker != managed_order->signal.ticker)
        {
            const std::string ticker = managed_order->signal.ticker;
            lock.unlock();
            LOG_WARN(std::format("[OrderRouter] 놓친 체결 조회 — 종목·방향이 이력과 달라 건너뜀 ODNO={} 조회={} 이력={}",
                                 row.kis_order_no, row.ticker, ticker));
            continue;
        }

        // 단가는 누적 금액 차이로 구한다. 앞서 받은 통보 단가의 반올림으로 차이가 0 이하가 되면 누적 평균가로 쓴다.
        int64_t missing_amount = row.filled_amount - managed_order->confirmed_amount;

        if (missing_amount <= 0)
        {
            missing_amount = row.filled_amount * missing / row.filled_quantity;
        }

        FillNotification recovered;
        recovered.kis_order_no    = row.kis_order_no;
        recovered.ticker          = managed_order->signal.ticker;
        recovered.side            = managed_order->signal.side;
        recovered.filled_quantity = missing;
        recovered.filled_price    = static_cast<double>(missing_amount) / missing;
        recovered.fill_time       = std::format("{:06}", until_hhmmss);
        recovered.order_quantity  = row.order_quantity;
        recovered.timestamp       = std::chrono::system_clock::now();

        managed_order->recovered_credit_quantity += missing;
        managed_order->recovered_until_hhmmss     = until_hhmmss;
        apply_linked_fill(lock, *managed_order, recovered, missing, "소켓 끊김 중 체결 — 조회로 되찾음");
        ++recovered_orders;
        recovered_quantity += missing;
    }

    LOG_INFO(std::format("[OrderRouter] 놓친 체결 조회 완료 — 조회 {}건 중 되찾은 주문 {}건 {}주 (조회 시각 {:06})",
                         daily_fills->size(), recovered_orders, recovered_quantity, until_hhmmss));
    return recovered_orders;
}

// 같은 세션 안에서 같은 키가 다시 오면 분할체결로 받는다. 세션이 바뀌면 앞 세션까지 받은 횟수를
//  기억해 두고, 새 세션에서 그 횟수 이하로 오는 통보는 재전송으로 본다 — 증권사가 재구독 뒤 옛 통보를
//  다시 보내면 한 건씩 한 번 오므로, 앞에서 받은 수를 넘는 몫만 새 체결이다.
//  잘못 거른 실체결(재연결과 같은 초에 같은 수량·단가로 난 체결)은 수량만 잔고 대조가 되찾는다.
bool OrderRouter::is_replayed_fill_locked(const FillKey& fill_key, uint32_t session_generation)
{
    FillSighting& sighting = fill_sightings_[fill_key];

    if (sighting.seen_in_session == 0 || sighting.session_generation != session_generation)
    {
        sighting.session_generation      = session_generation;
        sighting.accepted_before_session = sighting.accepted;
        sighting.seen_in_session         = 0;
    }

    ++sighting.seen_in_session;

    if (sighting.seen_in_session <= sighting.accepted_before_session)
    {
        replayed_fills_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    ++sighting.accepted;

    if (sighting.accepted > 1)
    {
        LOG_INFO(std::format("[OrderRouter] 동일키 분할체결 {}회차 ODNO={} time={} {}주", sighting.accepted,
                             fill_key.order_number, fill_key.fill_time, fill_key.quantity));
    }

    return false;
}

// ─── 일별 리셋 (장 시작 시 Engine이 호출) ─────────────────────────────────
// 체결 목격 기록(fill_sightings_)의 무한 증가를 해소. 거래일 prefix로 cross-day 충돌은 이미
// 차단되므로, 전일 키는 더 이상 필요 없다.
void OrderRouter::reset_daily()
{
    std::lock_guard<std::mutex> lock(history_mutex_);
    fill_sightings_.clear();
    unlinked_fill_keys_.clear();
    unlinked_orders_.clear();
    // 사유 기록도 거래일이 바뀌면 다시 읽는다(파일이 날짜별이라 어제 것을 들고 있으면 안 된다).
    order_reasons_.clear();
    order_reasons_loaded_ = false;

    // 주문번호는 거래일마다 다시 쓰인다. 어제 주문이 오늘 조회의 같은 번호 행과 엮이지 않게 되찾기 대상에서 뺀다. [why D-149]
    for (auto& managed_order : history_)
    {
        managed_order.recoverable = false;
    }
}
