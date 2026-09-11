# MFC 운영단말 `ops_terminal` 작업 문서

`Quant/tools/ops_terminal/`에 있는 MFC 대화상자 단말의 정본이다. 무엇을 하는 프로그램인지, 어떻게 빌드·실행하는지,
MFC라서 걸린 함정과 지금까지 손댄 이력을 여기에 모은다. **MFC 쪽을 고치면 이 문서를 같이 고친다**(8절 체크리스트).
채널 자체(프로토콜·서버·콘솔 단말)는 [docs/guides/OPS_TERMINAL.md](OPS_TERMINAL.md), 결정 배경은
[docs/DECISIONS.md](../DECISIONS.md) D-043.

## 1. 무엇인가

엔진(`quant_trader`)이 연 운영단말 포트(기본 127.0.0.1:7100)에 붙어 보유 포지션을 표로 보여 주고, 행을 골라
수동 매도·매수를 내고, 킬스위치를 당기는 창 하나짜리 프로그램이다. 콘솔 단말 `ops_client`가 하는 일을 화면으로
옮긴 것이고, 프로토콜 헤더(`Quant/include/ipc/OpsProtocol.h`)를 서버·콘솔과 그대로 공유한다.

## 2. 파일

| 파일 | 역할 |
|---|---|
| `Quant/tools/ops_terminal/OpsLink.h`, `OpsLink.cpp` | 소켓 작업자 스레드. 접속·재접속·프레임 송수신·하트비트. UI에는 `PostMessage`로만 넘긴다 |
| `Quant/tools/ops_terminal/OpsTerminalDlg.h`, `OpsTerminalDlg.cpp` | 대화상자. 컨트롤·버튼 핸들러·프레임 해석·포지션 표·로그 |
| `Quant/tools/ops_terminal/OpsTerminal.h`, `OpsTerminal.cpp` | `CWinApp`. 명령줄 파싱, 공용 컨트롤 초기화, 대화상자 모달 실행 |
| `Quant/tools/ops_terminal/OpsTerminal.rc`, `resource.h` | 대화상자 레이아웃과 컨트롤 ID. 한글 문자열이라 `#pragma code_page(65001)` |
| `Quant/tools/ops_terminal/pch.h` | 헤더 순서 고정 — `winsock2.h`가 `afxwin.h`보다 먼저 |
| `Quant/CMakeLists.txt` `ops_terminal` 블록 | `afxwin.h`가 있을 때만 타깃을 만든다(`QUANT_OPS_TERMINAL`) |

## 3. 빌드

필요한 것: MSVC와 Visual Studio Installer의 "C++ MFC(최신 v143 빌드 도구)" 구성 요소
(`Microsoft.VisualStudio.Component.VC.ATLMFC`). 설치 여부는
`...\VC\Tools\MSVC\<버전>\atlmfc\include\afxwin.h`와 `atlmfc\lib\x64\mfc140u.lib`가 있는지로 본다. ATL만 깔린
머신에는 `atlmfc\include`에 `atl*.h`만 있고 `afx*.h`가 없다.

설치 명령(관리자, Visual Studio는 닫고):

```
"C:\Program Files (x86)\Microsoft Visual Studio\Installer\setup.exe" modify --installPath "C:\Program Files\Microsoft Visual Studio\2022\Community" --add Microsoft.VisualStudio.Component.VC.ATLMFC --passive --norestart
```

종료 코드 8006은 Visual Studio가 떠 있어서 막힌 것이다.

CMake는 `find_path(QUANT_MFC_INC afxwin.h …)`로 MFC를 찾고, 찾으면 `QUANT_OPS_TERMINAL=ON`·타깃 생성, 못 찾으면
`ops_terminal(MFC) 건너뜀` 한 줄 남기고 넘어간다(엔진·테스트 빌드는 영향 없음). 두 값 모두 캐시에 남으므로
MFC를 나중에 설치했으면 지우고 다시 configure 한다.

```powershell
$env:TEMP='C:\build_tmp'   # 한글 사용자 폴더 TEMP는 LNK1104를 낸다 — 헤드리스 빌드 공통
cmake --preset x64-release -UQUANT_MFC_INC -UQUANT_OPS_TERMINAL
cmake --build out/build/x64-release --target ops_terminal
```

산출물: `out\build\x64-release\Quant\ops_terminal.exe`. 타깃 설정에서 바꾸면 안 되는 것:

- `CMAKE_MFC_FLAG 2` + `_AFXDLL` — MFC DLL 링크. 정적(`1`)으로 바꾸면 `UNICODE` 라이브러리 이름이 달라진다.
- `UNICODE _UNICODE` + `/ENTRY:wWinMainCRTStartup` — 유니코드 MFC의 진입점. 빠지면 `_WinMain@16`/`WinMain` 미해결.
- `WIN32` 서브시스템(`add_executable(... WIN32 ...)`) — 콘솔 창이 같이 뜨지 않게.
- 링크 `ws2_32` — 소켓.

## 4. 실행

```powershell
cd c:\Users\<사용자>\source\repos\Quant
.\out\build\x64-release\Quant\ops_terminal.exe --token <Quant/config/config_dev_paper.json의 ops_token>
```

인자: `--host`(기본 127.0.0.1) `--port`(7100) `--token`. 토큰은 환경변수 `QUANT_OPS_TOKEN`으로도 받는다.
토큰을 받았으면 뜨자마자 붙고, 없으면 접속 버튼을 눌러야 하며 이때는 조회만 된다(주문·킬 버튼 비활성).
엔진은 감시견(`scripts/auto_trade_day.ps1`)이 띄운 것에 붙는다 — 단말을 위해 엔진을 손으로 띄우지 않는다.

## 5. 화면

위에서 아래로:

1. 접속란 — 호스트·포트·토큰(가림)·접속/끊기 버튼·연결 상태(끊김/접속 중/연결됨/준비).
2. 엔진 상태 한 줄 — running·data·signal·order·kill·entry_halt·force_liq·paper·전략 수. 준비 상태에서 5초마다 갱신.
3. 포지션 표 — 계좌·종목·이름·수량·평단·현재가·평단대비(%)·대기매도·매도가능(=수량−대기매도). 서버 push(1초, 변화 시)와
   새로고침 버튼. 현재가는 엔진이 마지막으로 본 체결가(`last`)라 장 밖이나 기동 직후엔 `—`로 비어 있다가 첫 폴링(30초 주기)
   뒤 채워진다. 표는 종목 키로 바뀐 칸만 고치므로 갱신이 와도 스크롤·선택이 그대로다.
4. 주문 폼 — 종목(6자리)·수량·가격(0=시장가)·매도·보유 전량 매도·매수·현재가. 표에서 행을 고르면 종목과 매도가능 수량이
   들어가고 오른쪽 "현재가" 줄이 그 종목으로 바뀐다. 확인창에도 현재가가 붙고, `ORDER_REQ.ref_price`에 이 값을 찍어 보낸다
   (시장가 주문의 명목 한도 기준가. 0이면 엔진이 자기 최근가·평단으로 채운다).
   폼 아래 "내 주문 결과" 한 줄은 내가 낸 주문의 마지막 `ORDER_RESULT`만 보여 준다(전략 주문 결과에 묻히지 않게).
5. 로그 — 시각 접두, 2000줄 상한. `ORDER_ACK`·`ORDER_RESULT`·`FILL`·`ERROR`가 여기 쌓인다. 전략 주문 결과도 같은 채널로 오므로
   내 주문 줄에는 `★내 주문` 표식이 붙는다.
6. 킬스위치 — 확인창 뒤 `KILL`. 엔진이 주문을 막고 내려간다.

매도·매수·킬은 전부 확인창을 거친다. 수동 주문의 cid는 `mfc-<ms>`로 찍혀 엔진 로그 `[MANUAL]` 줄과 맞출 수 있다.

## 6. 스레드 모델

스레드는 둘이다.

- **UI 스레드** — MFC 컨트롤은 여기서만 만진다. `OpsLink::send()`는 송신 큐에 넣고 조건변수로 작업자를 깨울 뿐이라
  버튼 핸들러가 소켓을 기다리지 않는다.
- **작업자 스레드(`OpsLink`)** — 소켓과 `FrameReader`를 혼자 잡는다. 논블로킹 `connect`(select 3초 상한) → `HELLO` →
  200ms `select()` 루프에서 수신·송신·하트비트를 돌린다. 받은 프레임은 `WM_OPS_FRAME`, 상태 변화는 `WM_OPS_STATE`로
  `PostMessage` 한다. LPARAM은 `new`한 포인터고 받는 쪽(대화상자)이 `delete` 한다. `PostMessage`가 실패하면 보내는 쪽이
  지운다.

재접속은 1→2→4…30초 backoff. 10초마다 `PING`, 30초 무수신이면 죽은 연결로 보고 끊는다. 끊긴 연결에 남아 있던
송신분은 버린다 — 주문은 결과를 보고 다시 낸다. 닫을 때는 타이머를 끄고 `stop()`으로 작업자를 join 한 뒤, 이미
메시지 큐에 들어와 있는 `WM_OPS_*` 포인터를 `PeekMessage`로 꺼내 지운다.

cid→ODNO 대응은 단말이 든다. `ORDER_RESULT`에 둘이 같이 오면 맵에 적어 두고, 뒤에 오는 `FILL`(ODNO만 있음)에
`← 내 주문 cid=…`를 붙여 로그에 낸다.

## 7. MFC라서 걸린 것

| 함정 | 처리 |
|---|---|
| `windows.h`가 `winsock.h`를 끌어와 `winsock2.h`와 충돌 | `pch.h`에서 `winsock2.h`·`ws2tcpip.h`를 `afxwin.h`보다 먼저. `VC_EXTRALEAN` |
| 유니코드 MFC 진입점 | `/ENTRY:wWinMainCRTStartup` + `UNICODE _UNICODE` |
| `CListCtrl`이 안 뜸 | `InitInstance`에서 `InitCommonControlsEx(ICC_WIN95_CLASSES)` 먼저 |
| `.rc` 한글 깨짐 | `#pragma code_page(65001)`, `LANGUAGE LANG_KOREAN`, 글꼴 "Malgun Gothic" 9 |
| 작업자 스레드에서 컨트롤을 만지면 어긋남 | 컨트롤은 UI 스레드만. 작업자는 `PostMessage`만 |
| `SendMessage`로 넘기면 `stop()`의 join과 교착 | `PostMessage`만 쓴다. 닫을 때 남은 포인터는 대화상자가 정리 |
| `std::string`↔`CString` | `from_utf8`/`to_utf8`(MultiByteToWideChar/WideCharToMultiByte, CP_UTF8). 서버 본문은 UTF-8 |
| `ops::msg_name`은 `uint8_t`를 받는다 | `f.type`을 그대로 넘긴다 (`OpsMsg`로 캐스팅하지 않음) |
| POSITIONS `reserved`는 부호 있는 값(미체결 매도 음수) | 음수만 대기 매도로 세고 매도가능 = 수량 − 대기매도(0 하한). 처음엔 그대로 빼서 미체결 매도가 매도가능을 늘려 보였다 |
| `CListCtrl`을 `DeleteAllItems`로 비우고 다시 채우면 스크롤이 맨 위로 튄다 | 종목 키로 행을 찾아 바뀐 칸만 `SetItemText`, 새 종목은 끝에 붙이고 사라진 종목만 뒤에서부터 `DeleteItem`. 현재가가 1초마다 바뀌면서 표가 매번 초기화되던 것 |

## 8. MFC를 고칠 때 같이 고칠 것

- 이 문서 — 파일 표(2절), 화면(5절), 스레드 모델(6절), 함정(7절), 이력(9절) 중 해당 절.
- 실행 인자·산출물 경로·접속 방법이 바뀌면 `_private/LINKS.md` 운영단말 행(gitignore, 복붙용).
- 컨트롤 ID를 더하면 `resource.h`와 `OpsTerminal.rc` 둘 다.
- 프로토콜 메시지를 더하면 `Quant/include/ipc/OpsProtocol.h`가 정본이고, 단말은 `handle_frame`의 `switch`에 가지 하나.
- 빌드 옵션·필요 구성 요소가 바뀌면 3절과 `CLAUDE.md` 빌드 절.
- 편집 뒤 `py scripts/brace_style.py Quant/tools/ops_terminal/<고친 파일>`(인자 없이 돌리지 않는다).

## 9. 이력

| 날짜 | 내용 |
|---|---|
| 2026-09-11 | 첫 작성. 8파일 + CMake 타깃. MFC 구성 요소를 설치하고 첫 빌드 통과. 토큰 인자를 받으면 자동 접속하게 함. 감시견 엔진에 붙어 `HELLO … auth` 확인 |
| 2026-09-11 | 첫 왕복(LG전자 1주 지정가 매도)에서 게이트 거부 "매도가능수량 0" — 표의 매도가능이 `reserved` 부호를 무시해 25주 부풀려 보인 것. 부호 처리 + 매도 확인창에 표 기준 매도가능 초과 경고 |
| 2026-09-11 | 내 주문 거절이 전략 `[결과]` 줄에 묻혀 안 보임 → `★내 주문` 표식 + 폼 아래 "내 주문 결과" 줄(`IDC_LAST_RESULT`). 한전기술 1주 매도로 `ORDER_ACK→ORDER_RESULT(ODNO 0000033740)` 왕복 확인 |
| 2026-09-11 | 현재가가 없어 팔 자리를 볼 수 없음 → 엔진에 종목별 최근 체결가 캐시(`Engine::last_px_`, 전략 스레드가 틱마다 씀)를 두고 POSITIONS에 `last` 필드 추가. 60초 넘게 틱이 없는 보유 종목(유니버스 밖, WS 구독 상한에 밀린 종목)은 데이터 스레드가 매 사이클(30초) REST 현재가로 보충한다. 표에 현재가·평단대비 열, 폼에 현재가 줄(`IDC_CUR_PRICE`), `ref_price` stamp. 갱신마다 스크롤이 맨 위로 가던 것을 행 단위 갱신으로 고침 |
