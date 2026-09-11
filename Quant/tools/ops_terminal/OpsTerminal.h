#pragma once
// 운영단말 앱 객체. 명령행(--host --port --token)을 읽어 대화상자를 하나 띄운다. [why D-043]
#include "pch.h"

class OpsTerminalApp : public CWinApp
{
public:
    BOOL InitInstance() override;
};
