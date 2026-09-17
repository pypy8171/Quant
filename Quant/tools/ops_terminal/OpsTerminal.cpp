// 운영단말 진입점. UI 스레드는 이 파일과 OpsTerminalDlg만, 소켓은 OpsLink 작업자 스레드만. [why D-043]
#include "pch.h"

#include "OpsTerminal.h"
#include "OpsTerminalDlg.h"

#include <cstdlib>
#include <cwchar>

namespace
{

// "--host 127.0.0.1 --port 7100 --token T". 없는 값은 기본으로. 토큰은 환경변수 QUANT_OPS_TOKEN도 받는다
//  — 명령행은 작업관리자에 보이므로 개인 PC 밖에서는 환경변수를 권한다.
void parse_cmdline(int argc, wchar_t** argv, TerminalArgs& terminal_args)
{
    for (int index = 1; index + 1 < argc; index += 2)
    {
        const std::wstring key = argv[index];
        const CString      value = argv[index + 1];

        if (key == L"--host")
        {
            terminal_args.host = value;
        }
        else if (key == L"--port")
        {
            terminal_args.port = _wtoi(value);
        }
        else if (key == L"--token")
        {
            terminal_args.token = value;
        }
    }

    if (terminal_args.token.IsEmpty())
    {
        wchar_t* env = nullptr;
        size_t   count   = 0;

        if (_wdupenv_s(&env, &count, L"QUANT_OPS_TOKEN") == 0 && env != nullptr)
        {
            terminal_args.token = env;
            free(env);
        }
    }
}

} // namespace

BOOL OpsTerminalApp::InitInstance()
{
    // 리스트 컨트롤 등 공용 컨트롤 v6 초기화. 이걸 빼면 CListCtrl이 만들어지지 않는다.
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icc);
    CWinApp::InitInstance();

    TerminalArgs args;
    parse_cmdline(__argc, __wargv, args);

    OpsTerminalDlg dlg(args);
    m_pMainWnd = &dlg;
    dlg.DoModal();
    return FALSE; // 대화상자가 닫히면 끝
}

OpsTerminalApp theApp;
