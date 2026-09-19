// 운영단말 메인 대화상자 구현. UI 스레드 전용 — 소켓은 OpsLink 작업자 스레드가 잡고 WM_OPS_*로 올린다. [why D-043]
#include "pch.h"

#include "OpsTerminalDlg.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <memory>
#include <unordered_set>
#include <unordered_map>

using nlohmann::json;
using ops::OpsMsg;

namespace
{

CString from_utf8(const std::string& text)
{
    if (text.empty())
    {
        return CString();
    }

    const int count = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    CString   raw;
    wchar_t*  cursor = raw.GetBufferSetLength(count);
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), cursor, count);
    raw.ReleaseBuffer(count);
    return raw;
}

std::string to_utf8(const CString& text)
{
    if (text.IsEmpty())
    {
        return {};
    }

    const int   count = ::WideCharToMultiByte(CP_UTF8, 0, text, text.GetLength(), nullptr, 0, nullptr, nullptr);
    std::string raw(static_cast<size_t>(count), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text, text.GetLength(), raw.data(), count, nullptr, nullptr);
    return raw;
}

CString now_hhmmss()
{
    SYSTEMTIME systemtime;
    ::GetLocalTime(&systemtime);
    CString text;
    text.Format(L"%02d:%02d:%02d", systemtime.wHour, systemtime.wMinute, systemtime.wSecond);
    return text;
}

int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// 정수·실수 어느 쪽으로 와도 읽는다. 없으면 0.
double number_of(const json& document, const char* key)
{
    auto iterator = document.find(key);

    if (iterator == document.end() || !iterator->is_number())
    {
        return 0.0;
    }

    return iterator->get<double>();
}

std::string text_of(const json& document, const char* key)
{
    auto iterator = document.find(key);
    return (iterator != document.end() && iterator->is_string()) ? iterator->get<std::string>() : std::string();
}

bool flag(const json& document, const char* key)
{
    auto iterator = document.find(key);
    return iterator != document.end() && iterator->is_boolean() && iterator->get<bool>();
}

CString format_quantity(double value)
{
    CString text;
    text.Format(L"%lld", static_cast<long long>(value));
    return text;
}

CString format_price(double value)
{
    CString text;
    text.Format(L"%.0f", value);
    return text;
}

CString format_percent(double value)
{
    CString text;
    text.Format(L"%+.2f%%", value);
    return text;
}

// 원 단위 금액에 천 단위 쉼표. 계좌 줄은 자릿수가 커서 쉼표 없이는 읽기 어렵다.
CString format_won(double value)
{
    CString digits;
    digits.Format(L"%.0f", std::fabs(value));
    CString grouped;

    for (int index = 0; index < digits.GetLength(); ++index)
    {
        if (index > 0 && (digits.GetLength() - index) % 3 == 0)
        {
            grouped += L',';
        }

        grouped += digits[index];
    }

    return (value < 0 ? L"-" : L"") + grouped;
}

const wchar_t* state_text(LinkState link_state)
{
    switch (link_state)
    {
    case LinkState::Connecting:
        return L"접속 중";
    case LinkState::Connected:
        return L"연결됨 (HELLO_ACK 대기)";
    case LinkState::Ready:
        return L"준비";
    default:
        return L"끊김";
    }
}

// 포지션 표 열 순서. 매도가능 = 수량 − 대기 매도.
// [wire] POSITIONS의 reserved는 부호 있는 값이다 — 미체결 매도는 음수, 미체결 매수는 양수(OrderGate::reserved_).
//  09-11 처음엔 그대로 빼서 미체결 매도 25주가 매도가능을 25주 늘려 보였다. 음수만 대기 매도로 센다.
enum Col
{
    kColAccount = 0,
    kColTicker,
    kColName,
    kColQty,
    kColAvg,
    kColLast,
    kColChg,
    kColUnrealized, // 평가손익(원) = 수량 × (현재가 − 평단)
    kColReserved,
    kColSellable,
};

} // namespace

BEGIN_MESSAGE_MAP(OpsTerminalDlg, CDialogEx)
ON_BN_CLICKED(IDC_CONNECT, &OpsTerminalDlg::OnConnect)
ON_BN_CLICKED(IDC_REFRESH, &OpsTerminalDlg::OnRefresh)
ON_BN_CLICKED(IDC_SELL, &OpsTerminalDlg::OnSell)
ON_BN_CLICKED(IDC_SELL_ALL, &OpsTerminalDlg::OnSellAll)
ON_BN_CLICKED(IDC_BUY, &OpsTerminalDlg::OnBuy)
ON_BN_CLICKED(IDC_KILL, &OpsTerminalDlg::OnKill)
ON_BN_CLICKED(IDC_HALT, &OpsTerminalDlg::OnHalt)
ON_BN_CLICKED(IDC_HALT_SELL, &OpsTerminalDlg::OnHaltSell)
ON_WM_TIMER()
ON_NOTIFY(LVN_ITEMCHANGED, IDC_POSITIONS, &OpsTerminalDlg::OnPositionSelected)
ON_NOTIFY(NM_CUSTOMDRAW, IDC_POSITIONS, &OpsTerminalDlg::OnPositionsCustomDraw)
ON_WM_CTLCOLOR()
ON_MESSAGE(WM_OPS_FRAME, &OpsTerminalDlg::OnOpsFrame)
ON_MESSAGE(WM_OPS_STATE, &OpsTerminalDlg::OnOpsState)
END_MESSAGE_MAP()

OpsTerminalDlg::OpsTerminalDlg(const TerminalArgs& arguments, CWnd* parent)
    : CDialogEx(IDD_OPS_TERMINAL, parent), arguments_(arguments)
{
}

void OpsTerminalDlg::DoDataExchange(CDataExchange* data_exchange)
{
    CDialogEx::DoDataExchange(data_exchange);
    DDX_Control(data_exchange, IDC_POSITIONS, positions_);
    DDX_Control(data_exchange, IDC_LOG, log_);
}

BOOL OpsTerminalDlg::OnInitDialog()
{
    CDialogEx::OnInitDialog();

    SetDlgItemText(IDC_HOST, arguments_.host);
    SetDlgItemInt(IDC_PORT, static_cast<UINT>(arguments_.port));
    SetDlgItemText(IDC_TOKEN, arguments_.token);

    positions_.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    positions_.InsertColumn(kColAccount, L"계좌", LVCFMT_LEFT, 80);
    positions_.InsertColumn(kColTicker, L"종목", LVCFMT_LEFT, 60);
    positions_.InsertColumn(kColName, L"이름", LVCFMT_LEFT, 140);
    positions_.InsertColumn(kColQty, L"수량", LVCFMT_RIGHT, 60);
    positions_.InsertColumn(kColAvg, L"평단", LVCFMT_RIGHT, 80);
    positions_.InsertColumn(kColLast, L"현재가", LVCFMT_RIGHT, 80);
    positions_.InsertColumn(kColChg, L"평단대비", LVCFMT_RIGHT, 64);
    positions_.InsertColumn(kColUnrealized, L"평가손익", LVCFMT_RIGHT, 90);
    positions_.InsertColumn(kColReserved, L"대기매도", LVCFMT_RIGHT, 60);
    positions_.InsertColumn(kColSellable, L"매도가능", LVCFMT_RIGHT, 60);

    set_order_enabled(false);
    log(L"접속 버튼을 누르면 엔진에 붙는다. 토큰이 없으면 조회만 된다.");

    // 토큰을 명령줄·환경변수로 받았으면 바로 붙는다 — 운영자가 매번 접속을 누르지 않게.
    if (!arguments_.token.IsEmpty())
    {
        OnConnect();
    }

    return TRUE;
}

// 닫을 때 작업자를 내리고, 이미 큐에 들어온 WM_OPS_* 포인터를 지운다(안 지우면 새는 것은 그 몇 개뿐이지만 습관으로).
void OpsTerminalDlg::OnCancel()
{
    KillTimer(kStatusTimer);
    link_.stop();

    MSG message;

    while (::PeekMessage(&message, m_hWnd, WM_OPS_FRAME, WM_OPS_STATE, PM_REMOVE))
    {
        if (message.message == WM_OPS_FRAME)
        {
            delete reinterpret_cast<ops::Frame*>(message.lParam);
        }
        else if (message.message == WM_OPS_STATE)
        {
            delete reinterpret_cast<OpsStateMsg*>(message.lParam);
        }
    }

    CDialogEx::OnCancel();
}

void OpsTerminalDlg::OnConnect()
{
    if (link_.running())
    {
        KillTimer(kStatusTimer);
        link_.stop();
        SetDlgItemText(IDC_CONNECT, L"접속");
        SetDlgItemText(IDC_LINK_STATE, state_text(LinkState::Disconnected));
        set_order_enabled(false);
        log(L"연결을 끊었다.");
        return;
    }

    CString host;
    CString token;
    GetDlgItemText(IDC_HOST, host);
    GetDlgItemText(IDC_TOKEN, token);
    const int port = static_cast<int>(GetDlgItemInt(IDC_PORT));
    host.Trim();

    if (host.IsEmpty() || port <= 0 || port > 65535)
    {
        MessageBox(L"호스트와 포트를 확인해 달라.", L"운영단말", MB_ICONWARNING);
        return;
    }

    arguments_.host  = host;
    arguments_.port  = port;
    arguments_.token = token;
    link_.start(m_hWnd, to_utf8(host), port, to_utf8(token));
    SetDlgItemText(IDC_CONNECT, L"끊기");
}

void OpsTerminalDlg::OnRefresh()
{
    if (!link_.send(OpsMsg::POSITIONS_REQ, "{}"))
    {
        log(L"연결이 없어 새로고침을 보내지 못했다.");
    }
}

void OpsTerminalDlg::OnSell()
{
    place_order("SELL");
}

void OpsTerminalDlg::OnBuy()
{
    place_order("BUY");
}

// 선택한 행의 매도가능 수량을 폼에 넣고 시장가 매도로 간다.
void OpsTerminalDlg::OnSellAll()
{
    const int row = positions_.GetNextItem(-1, LVNI_SELECTED);

    if (row < 0)
    {
        MessageBox(L"먼저 포지션 표에서 종목을 골라 달라.", L"운영단말", MB_ICONINFORMATION);
        return;
    }

    SetDlgItemText(IDC_TICKER, positions_.GetItemText(row, kColTicker));
    SetDlgItemText(IDC_QTY, positions_.GetItemText(row, kColSellable));
    SetDlgItemText(IDC_PRICE, L"0");
    place_order("SELL");
}

void OpsTerminalDlg::OnKill()
{
    const int result = MessageBox(L"킬스위치를 켜고 엔진을 내린다. 남은 주문은 나가지 않는다.\n\n진행할까?", L"킬스위치",
                             MB_YESNO | MB_DEFBUTTON2 | MB_ICONWARNING);

    if (result != IDYES)
    {
        return;
    }

    if (link_.send(OpsMsg::KILL_REQ, "{}"))
    {
        log(L"KILL 전송");
    }
    else
    {
        log(L"연결이 없어 KILL을 보내지 못했다.");
    }
}

void OpsTerminalDlg::OnHalt()
{
    const bool want_on = !manual_buy_halt_;
    const int  result = MessageBox(want_on ? L"전략의 신규 진입(매수)만 막는다. 보유분 매도와 이 창의 수동 주문은 그대로 나간다.\n\n켤까?"
                                       : L"신규 매수 정지를 끈다.\n\n끌까?",
                              L"신규 매수 정지", MB_YESNO | (want_on ? MB_ICONWARNING : MB_ICONQUESTION));

    if (result == IDYES)
    {
        send_halt("BUY", want_on);
    }
}

// 전략 매도 정지 — 손절·트레일·마감 청산까지 전략이 내는 매도 전부가 멈춘다. 이 창의 수동 매도와 국면 강제청산은 예외. [why D-095]
void OpsTerminalDlg::OnHaltSell()
{
    const bool want_on = !manual_sell_halt_;
    const int  result = MessageBox(want_on ? L"전략이 내는 매도를 전부 막는다 — 손절·트레일·마감 청산도 멈춘다.\n"
                                              L"이 창의 수동 매도와 국면 강제청산은 그대로 나간다.\n\n켤까?"
                                       : L"전략 매도 정지를 끈다.\n\n끌까?",
                              L"전략 매도 정지", MB_YESNO | (want_on ? MB_ICONWARNING : MB_ICONQUESTION));

    if (result == IDYES)
    {
        send_halt("SELL", want_on);
    }
}

void OpsTerminalDlg::send_halt(const char* side, bool want_on)
{
    json body;
    body["side"] = side;
    body["on"]   = want_on;

    if (link_.send(OpsMsg::HALT_REQ, body.dump()))
    {
        log(CString(L"HALT_REQ ") + CString(side) + (want_on ? L" ON 전송" : L" OFF 전송"));
    }
    else
    {
        log(L"연결이 없어 HALT_REQ를 보내지 못했다.");
    }
}

void OpsTerminalDlg::OnTimer(UINT_PTR id)
{
    if (id == kStatusTimer && state_ == LinkState::Ready)
    {
        link_.send(OpsMsg::STATUS_REQ, "{}");
    }

    CDialogEx::OnTimer(id);
}

void OpsTerminalDlg::OnPositionSelected(NMHDR* header, LRESULT* result)
{
    auto* list_view = reinterpret_cast<NMLISTVIEW*>(header);
    *result  = 0;

    if ((list_view->uNewState & LVIS_SELECTED) && !(list_view->uOldState & LVIS_SELECTED) && list_view->iItem >= 0)
    {
        SetDlgItemText(IDC_TICKER, positions_.GetItemText(list_view->iItem, kColTicker));
        SetDlgItemText(IDC_QTY, positions_.GetItemText(list_view->iItem, kColSellable));
        SetDlgItemText(IDC_PRICE, L"0");
        refresh_current_price();
    }
}

LRESULT OpsTerminalDlg::OnOpsState(WPARAM, LPARAM lparam)
{
    std::unique_ptr<OpsStateMsg> ops_state_message(reinterpret_cast<OpsStateMsg*>(lparam));
    state_ = ops_state_message->state;
    SetDlgItemText(IDC_LINK_STATE, CString(state_text(state_)) + L" — " + from_utf8(ops_state_message->detail));
    log(CString(L"[링크] ") + state_text(state_) + L": " + from_utf8(ops_state_message->detail));

    if (state_ == LinkState::Ready)
    {
        SetTimer(kStatusTimer, kStatusEveryMs, nullptr);
        link_.send(OpsMsg::STATUS_REQ, "{}");
    }
    else if (state_ == LinkState::Disconnected)
    {
        KillTimer(kStatusTimer);
        authentication_ = false;
        set_order_enabled(false);
        SetDlgItemText(IDC_ENGINE_STATE, L"엔진 상태: -");
        SetDlgItemText(IDC_ACCOUNT_STATE, L"계좌: -");

        if (!link_.running())
        {
            SetDlgItemText(IDC_CONNECT, L"접속");
        }
    }

    return 0;
}

LRESULT OpsTerminalDlg::OnOpsFrame(WPARAM, LPARAM lparam)
{
    std::unique_ptr<ops::Frame> frame(reinterpret_cast<ops::Frame*>(lparam));
    handle_frame(*frame);
    return 0;
}

void OpsTerminalDlg::handle_frame(const ops::Frame& frame)
{
    json document;

    if (!frame.body.empty())
    {
        document = json::parse(frame.body, nullptr, false);

        if (document.is_discarded())
        {
            log(CString(L"[수신] ") + from_utf8(ops::message_name(frame.type)) + L" 본문이 JSON이 아니다");
            return;
        }
    }

    switch (static_cast<OpsMsg>(frame.type))
    {
    case OpsMsg::HELLO_ACK:
    {
        authentication_ = flag(document, "auth");
        CString text;
        text.Format(L"HELLO_ACK engine=%s paper=%d auth=%d", from_utf8(text_of(document, "engine")).GetString(), flag(document, "paper") ? 1 : 0,
                 authentication_ ? 1 : 0);
        log(text);

        if (!authentication_)
        {
            log(L"토큰 인증이 안 됐다 — 조회만 된다. 주문·킬은 서버가 거절한다.");
        }

        set_order_enabled(authentication_);
        break;
    }

    case OpsMsg::POSITIONS_ACK:
    case OpsMsg::POSITIONS_NTF:
        apply_positions(frame.body);
        break;

    case OpsMsg::STATUS_ACK:
        apply_status(frame.body);
        break;

    case OpsMsg::PING_ACK:
        break;

    case OpsMsg::ORDER_ACK:
    {
        const std::string client_id = text_of(document, "cid");
        auto              iterator  = by_client_id_.find(client_id);
        CString           who = iterator == by_client_id_.end() ? from_utf8(client_id)
                                                    : from_utf8(iterator->second.ticker + " " + iterator->second.side + " " +
                                                                std::to_string(iterator->second.quantity));
        log((flag(document, "accepted") ? L"[ACK] 인테이크 적재 " : L"[ACK] 거절 ") + who + L" — " + from_utf8(text_of(document, "msg")));
        break;
    }

    case OpsMsg::ORDER_RESULT_NTF:
    {
        const std::string client_id  = text_of(document, "cid");
        const std::string kis_order_no = text_of(document, "odno");

        if (!kis_order_no.empty() && !client_id.empty())
        {
            odno_to_client_id_[kis_order_no] = client_id;
        }

        // 전략 주문 결과도 같은 채널로 오므로 내 주문(by_cid_에 있는 client_id)은 표식을 붙여 구분한다 —
        //  09-11 첫 운용에서 거절 한 줄이 전략 [결과] 줄에 묻혀 안 보였다.
        const bool mine = !client_id.empty() && by_client_id_.count(client_id) > 0;
        CString    text;
        text.Format(L"%s[결과] %s %s %s %lld주 %s odno=%s %s", mine ? L"★내 주문 " : L"",
                 flag(document, "ok") ? L"접수" : L"거절", from_utf8(text_of(document, "strategy")).GetString(),
                 from_utf8(text_of(document, "ticker")).GetString(), static_cast<long long>(number_of(document, "qty")),
                 from_utf8(text_of(document, "side")).GetString(), from_utf8(kis_order_no).GetString(),
                 from_utf8(text_of(document, "msg")).GetString());
        log(text);

        if (mine)
        {
            SetDlgItemText(IDC_LAST_RESULT, text);
        }

        break;
    }

    case OpsMsg::FILL_NTF:
    {
        const std::string kis_order_no = text_of(document, "odno");
        auto              iterator   = odno_to_client_id_.find(kis_order_no);
        CString           text;
        text.Format(L"[체결] %s %s %lld주 @%s odno=%s%s", from_utf8(text_of(document, "ticker")).GetString(),
                 from_utf8(text_of(document, "side")).GetString(), static_cast<long long>(number_of(document, "qty")),
                 format_price(number_of(document, "price")).GetString(), from_utf8(kis_order_no).GetString(),
                 iterator == odno_to_client_id_.end() ? L"" : (L" ← 내 주문 " + from_utf8(iterator->second)).GetString());
        log(text);
        break;
    }

    case OpsMsg::KILL_ACK:
        log((flag(document, "ok") ? L"[KILL] 확인 — " : L"[KILL] 거절 — ") + from_utf8(text_of(document, "msg")));
        break;

    case OpsMsg::HALT_ACK:
        if (flag(document, "ok"))
        {
            set_halt_buttons(flag(document, "manual_buy_halt"), flag(document, "manual_sell_halt"));
            CString text;
            text.Format(L"[HALT] 확인 — 신규 매수 정지 %s, 전략 매도 정지 %s", manual_buy_halt_ ? L"ON" : L"OFF",
                        manual_sell_halt_ ? L"ON" : L"OFF");
            log(text);
        }
        else
        {
            log(L"[HALT] 거절 — " + from_utf8(text_of(document, "msg").empty() ? std::string("미인증") : text_of(document, "msg")));
        }

        break;

    case OpsMsg::ERROR_NTF:
        log(L"[서버 오류] " + from_utf8(text_of(document, "msg")));
        break;

    default:
        log(CString(L"[수신] 알 수 없는 타입 ") + from_utf8(ops::message_name(frame.type)));
        break;
    }
}

// 표를 통째로 다시 그린다. 선택은 종목코드로 되살린다 — 서버가 1초마다 push해도 고른 행이 튀지 않게.
// 표에서 종목 행의 현재가를 읽는다. 행이 없거나 틱이 없던 종목(0)이면 0.
double OpsTerminalDlg::last_in_table(const CString& ticker) const
{
    for (int item_index = 0; item_index < positions_.GetItemCount(); ++item_index)
    {
        if (positions_.GetItemText(item_index, kColTicker) == ticker)
        {
            return _wtof(positions_.GetItemText(item_index, kColLast));
        }
    }

    return 0.0;
}

// 주문 폼 옆 "현재가" 한 줄을 종목 입력칸 기준으로 다시 쓴다 — 선택이 바뀔 때와 POSITIONS가 올 때.
void OpsTerminalDlg::refresh_current_price()
{
    CString ticker;
    GetDlgItemText(IDC_TICKER, ticker);
    ticker.Trim();
    const double last = ticker.IsEmpty() ? 0.0 : last_in_table(ticker);
    SetDlgItemText(IDC_CUR_PRICE, last > 0 ? L"현재가 " + format_price(last) : CString(L"현재가 —"));
}

// 표에서 종목 행의 매도가능을 읽는다. 행이 없으면 -1.
int OpsTerminalDlg::sellable_in_table(const CString& ticker) const
{
    for (int item_index = 0; item_index < positions_.GetItemCount(); ++item_index)
    {
        if (positions_.GetItemText(item_index, kColTicker) == ticker)
        {
            return _wtoi(positions_.GetItemText(item_index, kColSellable));
        }
    }

    return -1;
}

void OpsTerminalDlg::apply_positions(const std::string& body)
{
    json document = json::parse(body, nullptr, false);

    if (document.is_discarded() || !document.contains("positions") || !document["positions"].is_array())
    {
        return;
    }

    // DeleteAllItems 후 다시 채우면 매 push(현재가가 바뀌는 1초마다)마다 스크롤이 맨 위로 튄다 —
    //  09-11 운용에서 보던 자리를 잃는다는 지적. 종목 키로 행을 찾아 바뀐 칸만 고치고, 새 종목은 끝에
    //  붙이고, 사라진 종목만 지운다. 선택·스크롤은 컨트롤이 그대로 갖고 있다.
    std::unordered_map<std::wstring, int> row_of;

    for (int item_index = 0; item_index < positions_.GetItemCount(); ++item_index)
    {
        row_of[positions_.GetItemText(item_index, kColTicker).GetString()] = item_index;
    }

    auto set_cell = [this](int row, int column, const CString& text)
    {
        if (positions_.GetItemText(row, column) != text)
        {
            positions_.SetItemText(row, column, text);
        }
    };

    std::unordered_set<std::wstring> seen;
    positions_.SetRedraw(FALSE);

    for (const auto& position_node : document["positions"])
    {
        const double quantity      = number_of(position_node, "qty");
        const double reserved = number_of(position_node, "reserved");
        const double sell_pending = reserved < 0 ? -reserved : 0;
        const double sellable     = quantity - sell_pending > 0 ? quantity - sell_pending : 0;
        const double average      = number_of(position_node, "avg_price");
        const double last     = number_of(position_node, "last");
        const CString ticker  = from_utf8(text_of(position_node, "ticker"));
        seen.insert(ticker.GetString());

        auto iterator  = row_of.find(ticker.GetString());
        int  row = iterator != row_of.end() ? iterator->second : -1;

        if (row < 0)
        {
            row = positions_.InsertItem(positions_.GetItemCount(), from_utf8(text_of(position_node, "account")));
            positions_.SetItemText(row, kColTicker, ticker);
            row_of[ticker.GetString()] = row;
        }
        else
        {
            set_cell(row, kColAccount, from_utf8(text_of(position_node, "account")));
        }

        set_cell(row, kColName, from_utf8(text_of(position_node, "name")));
        set_cell(row, kColQty, format_quantity(quantity));
        set_cell(row, kColAvg, format_price(average));
        set_cell(row, kColLast, last > 0 ? format_price(last) : CString(L"—"));
        set_cell(row, kColChg, last > 0 && average > 0 ? format_percent((last - average) / average * 100.0) : CString(L"—"));
        const double unrealized = last > 0 && average > 0 ? quantity * (last - average) : 0.0;
        set_cell(row, kColUnrealized, last > 0 && average > 0 ? format_won(unrealized) : CString(L"—"));
        // 부호만 행 데이터로 남긴다 — 커스텀 드로우가 칸 색을 고를 때 글자를 다시 파싱하지 않는다.
        positions_.SetItemData(row, static_cast<DWORD_PTR>(unrealized > 0 ? 1 : unrealized < 0 ? 2 : 0));
        set_cell(row, kColReserved, format_quantity(sell_pending));
        set_cell(row, kColSellable, format_quantity(sellable));
    }

    // 뒤에서부터 지워야 앞 행의 인덱스가 밀리지 않는다.
    for (int index = positions_.GetItemCount() - 1; index >= 0; --index)
    {
        if (seen.count(positions_.GetItemText(index, kColTicker).GetString()) == 0)
        {
            positions_.DeleteItem(index);
        }
    }

    positions_.SetRedraw(TRUE);
    positions_.Invalidate();
    refresh_current_price();
}

void OpsTerminalDlg::apply_status(const std::string& body)
{
    json document = json::parse(body, nullptr, false);

    if (document.is_discarded())
    {
        return;
    }

    set_halt_buttons(flag(document, "manual_buy_halt"), flag(document, "manual_sell_halt"));

    CString text;
    text.Format(L"엔진 상태: running=%d data=%d signal=%d order=%d | kill=%d entry_halt=%d buy_halt=%d sell_halt=%d force_liq=%d | paper=%d",
             flag(document, "running") ? 1 : 0, flag(document, "data") ? 1 : 0, flag(document, "signal") ? 1 : 0, flag(document, "order") ? 1 : 0,
             flag(document, "kill") ? 1 : 0, flag(document, "entry_halt") ? 1 : 0, flag(document, "manual_buy_halt") ? 1 : 0,
             flag(document, "manual_sell_halt") ? 1 : 0, flag(document, "force_liq") ? 1 : 0, flag(document, "paper") ? 1 : 0);

    if (document.contains("strategies") && document["strategies"].is_array())
    {
        CString name;
        name.Format(L" | 전략 %d개", static_cast<int>(document["strategies"].size()));
        text += name;
    }

    SetDlgItemText(IDC_ENGINE_STATE, text);
    apply_account(document);
}

// 총평가·현금·일손익은 엔진의 잔고 대조 주기(브로커 값)로만 바뀌고, 보유 평가·평가손익은 최근 체결가라 매초 움직인다.
//  옛 엔진(필드 없음)에 붙으면 줄을 비워 둔다.
void OpsTerminalDlg::apply_account(const nlohmann::json& document)
{
    if (!document.contains("equity"))
    {
        SetDlgItemText(IDC_ACCOUNT_STATE, L"계좌: (엔진이 계좌 요약을 보내지 않음)");
        SetDlgItemText(IDC_ACCOUNT_PNL, L"");
        return;
    }

    const double equity         = number_of(document, "equity");
    const double cash           = number_of(document, "cash");
    const double daily_pnl      = number_of(document, "daily_pnl");
    const double position_value = number_of(document, "position_value");
    const double unrealized_pnl = number_of(document, "unrealized_pnl");
    const double cost_basis     = position_value - unrealized_pnl;
    const double unrealized_percent = cost_basis > 0.0 ? unrealized_pnl / cost_basis * 100.0 : 0.0;

    CString text;
    text.Format(L"계좌: 총평가 %s원 | 주문가능현금 %s원 | 일손익 %s원 | 보유 평가 %s원",
             format_won(equity).GetString(), format_won(cash).GetString(), format_won(daily_pnl).GetString(),
             format_won(position_value).GetString());
    SetDlgItemText(IDC_ACCOUNT_STATE, text);

    CString pnl_text;
    pnl_text.Format(L"평가손익 %s원 (%s)", format_won(unrealized_pnl).GetString(), format_percent(unrealized_percent).GetString());
    account_unrealized_pnl_ = unrealized_pnl;
    SetDlgItemText(IDC_ACCOUNT_PNL, pnl_text); // SetDlgItemText가 다시 그리게 하므로 OnCtlColor가 새 부호로 색을 고른다
}

COLORREF OpsTerminalDlg::profit_color(double value)
{
    if (value > 0)
    {
        return RGB(214, 0, 0);
    }

    if (value < 0)
    {
        return RGB(0, 80, 214);
    }

    return GetSysColor(COLOR_WINDOWTEXT);
}

HBRUSH OpsTerminalDlg::OnCtlColor(CDC* device_context, CWnd* window, UINT control_type)
{
    HBRUSH brush = CDialogEx::OnCtlColor(device_context, window, control_type);

    if (window != nullptr && window->GetDlgCtrlID() == IDC_ACCOUNT_PNL)
    {
        device_context->SetTextColor(profit_color(account_unrealized_pnl_));
    }

    return brush;
}

// 포지션 표의 평단대비·평가손익 칸만 부호 색. 나머지 칸은 기본색으로 되돌려야 한다 — clrText는 행 단위로 남는다.
void OpsTerminalDlg::OnPositionsCustomDraw(NMHDR* header, LRESULT* result)
{
    auto* draw = reinterpret_cast<NMLVCUSTOMDRAW*>(header);
    *result = CDRF_DODEFAULT;

    switch (draw->nmcd.dwDrawStage)
    {
    case CDDS_PREPAINT:
        *result = CDRF_NOTIFYITEMDRAW;
        break;

    case CDDS_ITEMPREPAINT:
        *result = CDRF_NOTIFYSUBITEMDRAW;
        break;

    case CDDS_ITEMPREPAINT | CDDS_SUBITEM:
    {
        const bool pnl_column = draw->iSubItem == kColChg || draw->iSubItem == kColUnrealized;
        const DWORD_PTR sign = draw->nmcd.lItemlParam;
        draw->clrText = pnl_column ? profit_color(sign == 1 ? 1.0 : sign == 2 ? -1.0 : 0.0) : GetSysColor(COLOR_WINDOWTEXT);
        *result = CDRF_NEWFONT;
        break;
    }

    default:
        break;
    }
}

// 확인 대화상자 한 번을 거쳐 ORDER_REQ를 보낸다. 거절·결과는 프레임으로 돌아와 로그에 남는다.
void OpsTerminalDlg::place_order(const char* side)
{
    if (!authentication_)
    {
        MessageBox(L"토큰 인증이 안 된 연결이다. 토큰을 넣고 다시 접속해 달라.", L"운영단말", MB_ICONWARNING);
        return;
    }

    CString ticker;
    GetDlgItemText(IDC_TICKER, ticker);
    ticker.Trim();
    const int quantity   = static_cast<int>(GetDlgItemInt(IDC_QTY));
    const int price = static_cast<int>(GetDlgItemInt(IDC_PRICE));

    if (ticker.GetLength() != 6 || quantity <= 0 || price < 0)
    {
        MessageBox(L"종목코드 6자리와 1 이상의 수량이 필요하다.", L"운영단말", MB_ICONWARNING);
        return;
    }

    const bool   sell = std::string(side) == "SELL";
    const double last = last_in_table(ticker);
    CString      ask;
    ask.Format(L"%s %s %d주 %s (현재가 %s)\n\n엔진 게이트·브로커를 거쳐 실제로 나간다. 진행할까?", ticker.GetString(),
               sell ? L"매도" : L"매수", quantity, price == 0 ? L"시장가" : (L"지정가 " + format_price(price)).GetString(),
               last > 0 ? format_price(last).GetString() : L"—");

    // 표가 아는 매도가능을 넘으면 미리 알린다 — 게이트는 어차피 거부하지만, 이유(미체결 매도가 잡고 있음)를
    //  주문 전에 보는 쪽이 낫다. 표에 없는 종목(보유 0)은 그대로 보낸다.
    if (sell)
    {
        const int sellable = sellable_in_table(ticker);

        if (sellable >= 0 && quantity > sellable)
        {
            CString warn;
            warn.Format(L"\n\n표 기준 매도가능 %d주 — 미체결 매도 예약이 잡고 있으면 게이트가 거부한다.", sellable);
            ask += warn;
        }
    }

    if (MessageBox(ask, sell ? L"수동 매도" : L"수동 매수", MB_YESNO | MB_ICONQUESTION) != IDYES)
    {
        return;
    }

    const std::string client_id = "mfc-" + std::to_string(now_ms());
    json              request;
    request["cid"]       = client_id;
    request["ticker"]    = to_utf8(ticker);
    request["side"]      = side;
    request["qty"]       = quantity;
    request["price"]     = price;
    // 시장가(price 0)의 명목 한도 평가 기준가. 표의 현재가를 찍고, 없으면 0으로 두어 엔진이 자기 최근가·평단으로 채운다.
    request["ref_price"] = last;
    request["account"]   = "";

    if (!link_.send(OpsMsg::ORDER_REQ, request.dump()))
    {
        log(L"연결이 없어 주문을 보내지 못했다.");
        return;
    }

    by_client_id_[client_id] = PendingOrder{to_utf8(ticker), side, quantity};
    log(L"[주문] 전송 " + from_utf8(client_id) + L" " + ticker + (sell ? L" SELL " : L" BUY ") + format_quantity(quantity) +
        (price == 0 ? CString(L" 시장가") : L" @" + format_price(price)));  // C++20 조건식은 양쪽 형식이 같아야 한다
}

void OpsTerminalDlg::log(const CString& line)
{
    while (log_.GetCount() >= kLogMaxLines)
    {
        log_.DeleteString(0);
    }

    const int index = log_.AddString(now_hhmmss() + L"  " + line);
    log_.SetTopIndex(index);
}

void OpsTerminalDlg::set_order_enabled(bool on)
{
    for (int id : {IDC_SELL, IDC_SELL_ALL, IDC_BUY, IDC_KILL, IDC_HALT, IDC_HALT_SELL})
    {
        GetDlgItem(id)->EnableWindow(on ? TRUE : FALSE);
    }
}

void OpsTerminalDlg::set_halt_buttons(bool buy_on, bool sell_on)
{
    manual_buy_halt_  = buy_on;
    manual_sell_halt_ = sell_on;
    SetDlgItemText(IDC_HALT, buy_on ? L"신규 매수 정지: ON (해제는 클릭)" : L"신규 매수 정지: OFF");
    SetDlgItemText(IDC_HALT_SELL, sell_on ? L"전략 매도 정지: ON (해제는 클릭)" : L"전략 매도 정지: OFF");
}
