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
void parse_cmdline(int argc, wchar_t** argv, TerminalArgs& terminal_arguments)
{
    for (int index = 1; index + 1 < argc; index += 2)
    {
        const std::wstring key = argv[index];
        const CString      value = argv[index + 1];

        if (key == L"--host")
        {
            terminal_arguments.host = value;
        }
        else if (key == L"--port")
        {
            terminal_arguments.port = _wtoi(value);
        }
        else if (key == L"--token")
        {
            terminal_arguments.token = value;
        }
    }

    if (terminal_arguments.token.IsEmpty())
    {
        wchar_t* environment = nullptr;
        size_t   count   = 0;

        if (_wdupenv_s(&environment, &count, L"QUANT_OPS_TOKEN") == 0 && environment != nullptr)
        {
            terminal_arguments.token = environment;
            free(environment);
        }
    }
}

} // namespace

BOOL OpsTerminalApp::InitInstance()
{
    // 리스트 컨트롤 등 공용 컨트롤 v6 초기화. 이걸 빼면 CListCtrl이 만들어지지 않는다.
    INITCOMMONCONTROLSEX common_controls{};
    common_controls.dwSize = sizeof(common_controls);
    common_controls.dwICC  = ICC_WIN95_CLASSES;
    InitCommonControlsEx(&common_controls);
    CWinApp::InitInstance();

    TerminalArgs arguments;
    parse_cmdline(__argc, __wargv, arguments);

    OpsTerminalDlg dialog(arguments);
    m_pMainWnd = &dialog;
    dialog.DoModal();
    return FALSE; // 대화상자가 닫히면 끝
}

OpsTerminalApp theApp;
