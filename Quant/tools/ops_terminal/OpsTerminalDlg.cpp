// 운영단말 메인 대화상자 구현. UI 스레드 전용 — 소켓은 OpsLink 작업자 스레드가 잡고 WM_OPS_*로 올린다. [why D-043]
#include "pch.h"

#include "OpsTerminalDlg.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <memory>
#include <unordered_set>
#include <unordered_map>

using nlohmann::json;
using ops::OpsMsg;

namespace
{

CString from_utf8(const std::string& s)
{
    if (s.empty())
    {
        return CString();
    }

    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    CString   r;
    wchar_t*  p = r.GetBufferSetLength(n);
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), p, n);
    r.ReleaseBuffer(n);
    return r;
}

std::string to_utf8(const CString& s)
{
    if (s.IsEmpty())
    {
        return {};
    }

    const int   n = ::WideCharToMultiByte(CP_UTF8, 0, s, s.GetLength(), nullptr, 0, nullptr, nullptr);
    std::string r(static_cast<size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, s, s.GetLength(), r.data(), n, nullptr, nullptr);
    return r;
}

CString now_hhmmss()
{
    SYSTEMTIME t;
    ::GetLocalTime(&t);
    CString s;
    s.Format(L"%02d:%02d:%02d", t.wHour, t.wMinute, t.wSecond);
    return s;
}

int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// 정수·실수 어느 쪽으로 와도 읽는다. 없으면 0.
double num(const json& j, const char* key)
{
    auto it = j.find(key);

    if (it == j.end() || !it->is_number())
    {
        return 0.0;
    }

    return it->get<double>();
}

std::string str(const json& j, const char* key)
{
    auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

bool flag(const json& j, const char* key)
{
    auto it = j.find(key);
    return it != j.end() && it->is_boolean() && it->get<bool>();
}

CString fmt_qty(double v)
{
    CString s;
    s.Format(L"%lld", static_cast<long long>(v));
    return s;
}

CString fmt_price(double v)
{
    CString s;
    s.Format(L"%.0f", v);
    return s;
}

CString fmt_pct(double v)
{
    CString s;
    s.Format(L"%+.2f%%", v);
    return s;
}

const wchar_t* state_text(LinkState s)
{
    switch (s)
    {
    case LinkState::Connecting:
        return L"접속 중";
    case LinkState::Connected:
        return L"연결됨 (WELCOME 대기)";
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
ON_WM_TIMER()
ON_NOTIFY(LVN_ITEMCHANGED, IDC_POSITIONS, &OpsTerminalDlg::OnPositionSelected)
ON_MESSAGE(WM_OPS_FRAME, &OpsTerminalDlg::OnOpsFrame)
ON_MESSAGE(WM_OPS_STATE, &OpsTerminalDlg::OnOpsState)
END_MESSAGE_MAP()

OpsTerminalDlg::OpsTerminalDlg(const TerminalArgs& args, CWnd* parent)
    : CDialogEx(IDD_OPS_TERMINAL, parent), args_(args)
{
}

void OpsTerminalDlg::DoDataExchange(CDataExchange* dx)
{
    CDialogEx::DoDataExchange(dx);
    DDX_Control(dx, IDC_POSITIONS, positions_);
    DDX_Control(dx, IDC_LOG, log_);
}

BOOL OpsTerminalDlg::OnInitDialog()
{
    CDialogEx::OnInitDialog();

    SetDlgItemText(IDC_HOST, args_.host);
    SetDlgItemInt(IDC_PORT, static_cast<UINT>(args_.port));
    SetDlgItemText(IDC_TOKEN, args_.token);

    positions_.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    positions_.InsertColumn(kColAccount, L"계좌", LVCFMT_LEFT, 80);
    positions_.InsertColumn(kColTicker, L"종목", LVCFMT_LEFT, 60);
    positions_.InsertColumn(kColName, L"이름", LVCFMT_LEFT, 140);
    positions_.InsertColumn(kColQty, L"수량", LVCFMT_RIGHT, 60);
    positions_.InsertColumn(kColAvg, L"평단", LVCFMT_RIGHT, 80);
    positions_.InsertColumn(kColLast, L"현재가", LVCFMT_RIGHT, 80);
    positions_.InsertColumn(kColChg, L"평단대비", LVCFMT_RIGHT, 64);
    positions_.InsertColumn(kColReserved, L"대기매도", LVCFMT_RIGHT, 60);
    positions_.InsertColumn(kColSellable, L"매도가능", LVCFMT_RIGHT, 60);

    set_order_enabled(false);
    log(L"접속 버튼을 누르면 엔진에 붙는다. 토큰이 없으면 조회만 된다.");

    // 토큰을 명령줄·환경변수로 받았으면 바로 붙는다 — 운영자가 매번 접속을 누르지 않게.
    if (!args_.token.IsEmpty())
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

    MSG m;

    while (::PeekMessage(&m, m_hWnd, WM_OPS_FRAME, WM_OPS_STATE, PM_REMOVE))
    {
        if (m.message == WM_OPS_FRAME)
        {
            delete reinterpret_cast<ops::Frame*>(m.lParam);
        }
        else if (m.message == WM_OPS_STATE)
        {
            delete reinterpret_cast<OpsStateMsg*>(m.lParam);
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

    args_.host  = host;
    args_.port  = port;
    args_.token = token;
    link_.start(m_hWnd, to_utf8(host), port, to_utf8(token));
    SetDlgItemText(IDC_CONNECT, L"끊기");
}

void OpsTerminalDlg::OnRefresh()
{
    if (!link_.send(OpsMsg::POS_REQ, "{}"))
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
    const int r = MessageBox(L"킬스위치를 켜고 엔진을 내린다. 남은 주문은 나가지 않는다.\n\n진행할까?", L"킬스위치",
                             MB_YESNO | MB_DEFBUTTON2 | MB_ICONWARNING);

    if (r != IDYES)
    {
        return;
    }

    if (link_.send(OpsMsg::KILL, "{}"))
    {
        log(L"KILL 전송");
    }
    else
    {
        log(L"연결이 없어 KILL을 보내지 못했다.");
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

void OpsTerminalDlg::OnPositionSelected(NMHDR* hdr, LRESULT* result)
{
    auto* lv = reinterpret_cast<NMLISTVIEW*>(hdr);
    *result  = 0;

    if ((lv->uNewState & LVIS_SELECTED) && !(lv->uOldState & LVIS_SELECTED) && lv->iItem >= 0)
    {
        SetDlgItemText(IDC_TICKER, positions_.GetItemText(lv->iItem, kColTicker));
        SetDlgItemText(IDC_QTY, positions_.GetItemText(lv->iItem, kColSellable));
        SetDlgItemText(IDC_PRICE, L"0");
        refresh_cur_price();
    }
}

LRESULT OpsTerminalDlg::OnOpsState(WPARAM, LPARAM lp)
{
    std::unique_ptr<OpsStateMsg> m(reinterpret_cast<OpsStateMsg*>(lp));
    state_ = m->state;
    SetDlgItemText(IDC_LINK_STATE, CString(state_text(state_)) + L" — " + from_utf8(m->detail));
    log(CString(L"[링크] ") + state_text(state_) + L": " + from_utf8(m->detail));

    if (state_ == LinkState::Ready)
    {
        SetTimer(kStatusTimer, kStatusEveryMs, nullptr);
        link_.send(OpsMsg::STATUS_REQ, "{}");
    }
    else if (state_ == LinkState::Disconnected)
    {
        KillTimer(kStatusTimer);
        auth_ = false;
        set_order_enabled(false);
        SetDlgItemText(IDC_ENGINE_STATE, L"엔진 상태: -");

        if (!link_.running())
        {
            SetDlgItemText(IDC_CONNECT, L"접속");
        }
    }

    return 0;
}

LRESULT OpsTerminalDlg::OnOpsFrame(WPARAM, LPARAM lp)
{
    std::unique_ptr<ops::Frame> f(reinterpret_cast<ops::Frame*>(lp));
    handle_frame(*f);
    return 0;
}

void OpsTerminalDlg::handle_frame(const ops::Frame& f)
{
    json j;

    if (!f.body.empty())
    {
        j = json::parse(f.body, nullptr, false);

        if (j.is_discarded())
        {
            log(CString(L"[수신] ") + from_utf8(ops::msg_name(f.type)) + L" 본문이 JSON이 아니다");
            return;
        }
    }

    switch (static_cast<OpsMsg>(f.type))
    {
    case OpsMsg::WELCOME:
    {
        auth_ = flag(j, "auth");
        CString s;
        s.Format(L"WELCOME engine=%s paper=%d auth=%d", from_utf8(str(j, "engine")).GetString(), flag(j, "paper") ? 1 : 0,
                 auth_ ? 1 : 0);
        log(s);

        if (!auth_)
        {
            log(L"토큰 인증이 안 됐다 — 조회만 된다. 주문·킬은 서버가 거절한다.");
        }

        set_order_enabled(auth_);
        break;
    }

    case OpsMsg::POSITIONS:
        apply_positions(f.body);
        break;

    case OpsMsg::STATUS:
        apply_status(f.body);
        break;

    case OpsMsg::PONG:
        break;

    case OpsMsg::ORDER_ACK:
    {
        const std::string cid = str(j, "cid");
        auto              it  = by_cid_.find(cid);
        CString           who = it == by_cid_.end() ? from_utf8(cid)
                                                    : from_utf8(it->second.ticker + " " + it->second.side + " " +
                                                                std::to_string(it->second.qty));
        log((flag(j, "accepted") ? L"[ACK] 인테이크 적재 " : L"[ACK] 거절 ") + who + L" — " + from_utf8(str(j, "msg")));
        break;
    }

    case OpsMsg::ORDER_RESULT:
    {
        const std::string cid  = str(j, "cid");
        const std::string odno = str(j, "odno");

        if (!odno.empty() && !cid.empty())
        {
            odno_to_cid_[odno] = cid;
        }

        // 전략 주문 결과도 같은 채널로 오므로 내 주문(by_cid_에 있는 cid)은 표식을 붙여 구분한다 —
        //  09-11 첫 운용에서 거절 한 줄이 전략 [결과] 줄에 묻혀 안 보였다.
        const bool mine = !cid.empty() && by_cid_.count(cid) > 0;
        CString    s;
        s.Format(L"%s[결과] %s %s %s %lld주 %s odno=%s %s", mine ? L"★내 주문 " : L"",
                 flag(j, "ok") ? L"접수" : L"거절", from_utf8(str(j, "strategy")).GetString(),
                 from_utf8(str(j, "ticker")).GetString(), static_cast<long long>(num(j, "qty")),
                 from_utf8(str(j, "side")).GetString(), from_utf8(odno).GetString(),
                 from_utf8(str(j, "msg")).GetString());
        log(s);

        if (mine)
        {
            SetDlgItemText(IDC_LAST_RESULT, s);
        }

        break;
    }

    case OpsMsg::FILL:
    {
        const std::string odno = str(j, "odno");
        auto              it   = odno_to_cid_.find(odno);
        CString           s;
        s.Format(L"[체결] %s %s %lld주 @%s odno=%s%s", from_utf8(str(j, "ticker")).GetString(),
                 from_utf8(str(j, "side")).GetString(), static_cast<long long>(num(j, "qty")),
                 fmt_price(num(j, "price")).GetString(), from_utf8(odno).GetString(),
                 it == odno_to_cid_.end() ? L"" : (L" ← 내 주문 " + from_utf8(it->second)).GetString());
        log(s);
        break;
    }

    case OpsMsg::KILL_ACK:
        log((flag(j, "ok") ? L"[KILL] 확인 — " : L"[KILL] 거절 — ") + from_utf8(str(j, "msg")));
        break;

    case OpsMsg::ERROR_MSG:
        log(L"[서버 오류] " + from_utf8(str(j, "msg")));
        break;

    default:
        log(CString(L"[수신] 알 수 없는 타입 ") + from_utf8(ops::msg_name(f.type)));
        break;
    }
}

// 표를 통째로 다시 그린다. 선택은 종목코드로 되살린다 — 서버가 1초마다 push해도 고른 행이 튀지 않게.
// 표에서 종목 행의 현재가를 읽는다. 행이 없거나 틱이 없던 종목(0)이면 0.
double OpsTerminalDlg::last_in_table(const CString& ticker) const
{
    for (int i = 0; i < positions_.GetItemCount(); ++i)
    {
        if (positions_.GetItemText(i, kColTicker) == ticker)
        {
            return _wtof(positions_.GetItemText(i, kColLast));
        }
    }

    return 0.0;
}

// 주문 폼 옆 "현재가" 한 줄을 종목 입력칸 기준으로 다시 쓴다 — 선택이 바뀔 때와 POSITIONS가 올 때.
void OpsTerminalDlg::refresh_cur_price()
{
    CString ticker;
    GetDlgItemText(IDC_TICKER, ticker);
    ticker.Trim();
    const double last = ticker.IsEmpty() ? 0.0 : last_in_table(ticker);
    SetDlgItemText(IDC_CUR_PRICE, last > 0 ? L"현재가 " + fmt_price(last) : CString(L"현재가 —"));
}

// 표에서 종목 행의 매도가능을 읽는다. 행이 없으면 -1.
int OpsTerminalDlg::sellable_in_table(const CString& ticker) const
{
    for (int i = 0; i < positions_.GetItemCount(); ++i)
    {
        if (positions_.GetItemText(i, kColTicker) == ticker)
        {
            return _wtoi(positions_.GetItemText(i, kColSellable));
        }
    }

    return -1;
}

void OpsTerminalDlg::apply_positions(const std::string& body)
{
    json j = json::parse(body, nullptr, false);

    if (j.is_discarded() || !j.contains("positions") || !j["positions"].is_array())
    {
        return;
    }

    // DeleteAllItems 후 다시 채우면 매 push(현재가가 바뀌는 1초마다)마다 스크롤이 맨 위로 튄다 —
    //  09-11 운용에서 보던 자리를 잃는다는 지적. 종목 키로 행을 찾아 바뀐 칸만 고치고, 새 종목은 끝에
    //  붙이고, 사라진 종목만 지운다. 선택·스크롤은 컨트롤이 그대로 갖고 있다.
    std::unordered_map<std::wstring, int> row_of;

    for (int i = 0; i < positions_.GetItemCount(); ++i)
    {
        row_of[positions_.GetItemText(i, kColTicker).GetString()] = i;
    }

    auto set_cell = [this](int row, int col, const CString& text)
    {
        if (positions_.GetItemText(row, col) != text)
        {
            positions_.SetItemText(row, col, text);
        }
    };

    std::unordered_set<std::wstring> seen;
    positions_.SetRedraw(FALSE);

    for (const auto& p : j["positions"])
    {
        const double qty      = num(p, "qty");
        const double reserved = num(p, "reserved");
        const double sell_pending = reserved < 0 ? -reserved : 0;
        const double sellable     = qty - sell_pending > 0 ? qty - sell_pending : 0;
        const double avg      = num(p, "avg_price");
        const double last     = num(p, "last");
        const CString ticker  = from_utf8(str(p, "ticker"));
        seen.insert(ticker.GetString());

        auto it  = row_of.find(ticker.GetString());
        int  row = it != row_of.end() ? it->second : -1;

        if (row < 0)
        {
            row = positions_.InsertItem(positions_.GetItemCount(), from_utf8(str(p, "account")));
            positions_.SetItemText(row, kColTicker, ticker);
            row_of[ticker.GetString()] = row;
        }
        else
        {
            set_cell(row, kColAccount, from_utf8(str(p, "account")));
        }

        set_cell(row, kColName, from_utf8(str(p, "name")));
        set_cell(row, kColQty, fmt_qty(qty));
        set_cell(row, kColAvg, fmt_price(avg));
        set_cell(row, kColLast, last > 0 ? fmt_price(last) : CString(L"—"));
        set_cell(row, kColChg, last > 0 && avg > 0 ? fmt_pct((last - avg) / avg * 100.0) : CString(L"—"));
        set_cell(row, kColReserved, fmt_qty(sell_pending));
        set_cell(row, kColSellable, fmt_qty(sellable));
    }

    // 뒤에서부터 지워야 앞 행의 인덱스가 밀리지 않는다.
    for (int i = positions_.GetItemCount() - 1; i >= 0; --i)
    {
        if (seen.count(positions_.GetItemText(i, kColTicker).GetString()) == 0)
        {
            positions_.DeleteItem(i);
        }
    }

    positions_.SetRedraw(TRUE);
    positions_.Invalidate();
    refresh_cur_price();
}

void OpsTerminalDlg::apply_status(const std::string& body)
{
    json j = json::parse(body, nullptr, false);

    if (j.is_discarded())
    {
        return;
    }

    CString s;
    s.Format(L"엔진 상태: running=%d data=%d signal=%d order=%d | kill=%d entry_halt=%d force_liq=%d | paper=%d",
             flag(j, "running") ? 1 : 0, flag(j, "data") ? 1 : 0, flag(j, "signal") ? 1 : 0, flag(j, "order") ? 1 : 0,
             flag(j, "kill") ? 1 : 0, flag(j, "entry_halt") ? 1 : 0, flag(j, "force_liq") ? 1 : 0,
             flag(j, "paper") ? 1 : 0);

    if (j.contains("strategies") && j["strategies"].is_array())
    {
        CString n;
        n.Format(L" | 전략 %d개", static_cast<int>(j["strategies"].size()));
        s += n;
    }

    SetDlgItemText(IDC_ENGINE_STATE, s);
}

// 확인 대화상자 한 번을 거쳐 ORDER_REQ를 보낸다. 거절·결과는 프레임으로 돌아와 로그에 남는다.
void OpsTerminalDlg::place_order(const char* side)
{
    if (!auth_)
    {
        MessageBox(L"토큰 인증이 안 된 연결이다. 토큰을 넣고 다시 접속해 달라.", L"운영단말", MB_ICONWARNING);
        return;
    }

    CString ticker;
    GetDlgItemText(IDC_TICKER, ticker);
    ticker.Trim();
    const int qty   = static_cast<int>(GetDlgItemInt(IDC_QTY));
    const int price = static_cast<int>(GetDlgItemInt(IDC_PRICE));

    if (ticker.GetLength() != 6 || qty <= 0 || price < 0)
    {
        MessageBox(L"종목코드 6자리와 1 이상의 수량이 필요하다.", L"운영단말", MB_ICONWARNING);
        return;
    }

    const bool   sell = std::string(side) == "SELL";
    const double last = last_in_table(ticker);
    CString      ask;
    ask.Format(L"%s %s %d주 %s (현재가 %s)\n\n엔진 게이트·브로커를 거쳐 실제로 나간다. 진행할까?", ticker.GetString(),
               sell ? L"매도" : L"매수", qty, price == 0 ? L"시장가" : (L"지정가 " + fmt_price(price)).GetString(),
               last > 0 ? fmt_price(last).GetString() : L"—");

    // 표가 아는 매도가능을 넘으면 미리 알린다 — 게이트는 어차피 거부하지만, 이유(미체결 매도가 잡고 있음)를
    //  주문 전에 보는 쪽이 낫다. 표에 없는 종목(보유 0)은 그대로 보낸다.
    if (sell)
    {
        const int sellable = sellable_in_table(ticker);

        if (sellable >= 0 && qty > sellable)
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

    const std::string cid = "mfc-" + std::to_string(now_ms());
    json              req;
    req["cid"]       = cid;
    req["ticker"]    = to_utf8(ticker);
    req["side"]      = side;
    req["qty"]       = qty;
    req["price"]     = price;
    // 시장가(price 0)의 명목 한도 평가 기준가. 표의 현재가를 찍고, 없으면 0으로 두어 엔진이 자기 최근가·평단으로 채운다.
    req["ref_price"] = last;
    req["account"]   = "";

    if (!link_.send(OpsMsg::ORDER_REQ, req.dump()))
    {
        log(L"연결이 없어 주문을 보내지 못했다.");
        return;
    }

    by_cid_[cid] = PendingOrder{to_utf8(ticker), side, qty};
    log(L"[주문] 전송 " + from_utf8(cid) + L" " + ticker + (sell ? L" SELL " : L" BUY ") + fmt_qty(qty) +
        (price == 0 ? CString(L" 시장가") : L" @" + fmt_price(price)));  // C++20 조건식은 양쪽 형식이 같아야 한다
}

void OpsTerminalDlg::log(const CString& line)
{
    while (log_.GetCount() >= kLogMaxLines)
    {
        log_.DeleteString(0);
    }

    const int idx = log_.AddString(now_hhmmss() + L"  " + line);
    log_.SetTopIndex(idx);
}

void OpsTerminalDlg::set_order_enabled(bool on)
{
    for (int id : {IDC_SELL, IDC_SELL_ALL, IDC_BUY, IDC_KILL})
    {
        GetDlgItem(id)->EnableWindow(on ? TRUE : FALSE);
    }
}
