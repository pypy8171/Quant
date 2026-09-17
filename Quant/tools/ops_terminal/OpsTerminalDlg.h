#pragma once
// 운영단말 메인 대화상자. 포지션 표·주문 폼·로그·킬스위치. 소켓 결과는 OpsLink가 WM_OPS_*로 올린다. [why D-043]
#include "pch.h"

#include "OpsLink.h"
#include "resource.h"

#include <map>
#include <string>

struct TerminalArgs
{
    CString host  = L"127.0.0.1";
    int     port  = 7100;
    CString token;
};

class OpsTerminalDlg : public CDialogEx
{
public:
    explicit OpsTerminalDlg(const TerminalArgs& arguments, CWnd* parent = nullptr);

    enum
    {
        IDD = IDD_OPS_TERMINAL
    };

protected:
    void DoDataExchange(CDataExchange* data_exchange) override;
    BOOL OnInitDialog() override;
    void OnCancel() override;

    afx_msg void    OnConnect();
    afx_msg void    OnRefresh();
    afx_msg void    OnSell();
    afx_msg void    OnSellAll();
    afx_msg void    OnBuy();
    afx_msg void    OnKill();
    afx_msg void    OnHalt();
    afx_msg void    OnTimer(UINT_PTR id);
    afx_msg void    OnPositionSelected(NMHDR* header, LRESULT* result);
    afx_msg LRESULT OnOpsFrame(WPARAM, LPARAM lparam);
    afx_msg LRESULT OnOpsState(WPARAM, LPARAM lparam);

    DECLARE_MESSAGE_MAP()

private:
    // 주문 하나의 왕복. cid는 우리가 붙이고 ODNO는 ORDER_RESULT에 실려 온다 — FILL은 ODNO만 있어 이 표로 이어 맞춘다.
    struct PendingOrder
    {
        std::string ticker;
        std::string side;
        int         quantity = 0;
    };

    void place_order(const char* side);
    void handle_frame(const ops::Frame& frame);
    void apply_positions(const std::string& body);
    int  sellable_in_table(const CString& ticker) const;
    double last_in_table(const CString& ticker) const;
    void   refresh_current_price();
    void apply_status(const std::string& body);
    void log(const CString& line);
    void set_order_enabled(bool on);
    void set_halt_button(bool on); // manual_halt_ 갱신 + 버튼 캡션 반영 [why D-091]

    TerminalArgs arguments_;
    OpsLink      link_;
    LinkState    state_ = LinkState::Disconnected;
    bool         authentication_  = false;
    bool         manual_halt_ = false; // 서버가 최근에 알려온 수동 정지 상태(HALT_ACK·STATUS로 갱신)

    CListCtrl positions_;
    CListBox  log_;

    std::map<std::string, PendingOrder> by_client_id_;
    std::map<std::string, std::string>  odno_to_client_id_;

    static constexpr UINT_PTR kStatusTimer = 1;
    static constexpr UINT     kStatusEveryMs = 5000;
    static constexpr int      kLogMaxLines   = 2000;
};
