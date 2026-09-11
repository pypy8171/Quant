# 운영단말 채널 (OpsServer) 가이드

엔진(`quant_trader`)이 TCP 포트를 하나 열고, 단말이 붙어 상태·포지션을 보고 종목을 골라 직접 팔거나
킬스위치를 당기는 채널이다. 결정 배경과 버린 대안은 [docs/DECISIONS.md](../DECISIONS.md) D-043.

관련 파일

| 파일 | 역할 |
|---|---|
| `Quant/include/ipc/OpsProtocol.h` | 프레임 인코더·디코더(`FrameReader`). 헤더 전용, 의존 없음 — 서버·콘솔 단말·MFC 단말이 같은 파일을 쓴다 |
| `Quant/include/ipc/OpsServer.h`, `Quant/src/ipc/OpsServer.cpp` | 서버. 소켓 전부를 전용 스레드 하나가 `select()`로 다룬다 |
| `Quant/src/core/Engine.cpp` (`start_ops_server`·`drain_manual_inbox`·`ops_*_json`) | 엔진 쪽 배선 — 수동 주문을 strategy_thread에서 `OrderSignal`로 바꾼다 |
| `Quant/tools/ops_client.cpp` | C++ 콘솔 단말. 왕복 검증·운영용 |
| `Quant/tools/ops_terminal/` | MFC 대화상자 단말. `OpsLink.*`(소켓 작업자 스레드)·`OpsTerminalDlg.*`(화면)·`OpsTerminal.rc`(레이아웃) |
| `Quant/tests/test_ops_protocol.cpp`, `Quant/tests/test_ops_server.cpp` | ctest 등록 테스트 |

## 1. 설정

`Quant/config/config.json`(또는 `config_dev_paper.json`) 최상위에 셋을 둔다.

```json
"ops_bind_addr": "127.0.0.1",
"ops_port": 7100,
"ops_token": "임의의 긴 문자열"
```

| 키 | 기본 | 뜻 |
|---|---|---|
| `ops_port` | 0 | 0이거나 없으면 서버를 열지 않는다 |
| `ops_bind_addr` | `127.0.0.1` | bind 주소. `localhost`도 받는다 |
| `ops_token` | 빈 문자열 | HELLO의 `token`과 같아야 주문·KILL을 받는다. 비어 있으면 누구나 붙되 조회만 된다 |

루프백이 아닌 주소(`0.0.0.0`, LAN IP)에 토큰 없이 열려고 하면 서버가 뜨지 않고 ERROR 로그를 남긴다.
토큰은 평문으로 오간다 — 원격에서 쓰려면 6절 "남은 일"을 먼저 본다.

기동 로그에 다음 줄이 있으면 열린 것이다.

```
[Ops] 운영단말 서버 대기 127.0.0.1:7100 (token 인증)
```

Windows에서는 `SO_EXCLUSIVEADDRUSE`로 잡으므로 엔진이 이미 하나 돌고 있으면 두 번째 엔진은
`[Ops] bind/listen 실패 127.0.0.1:7100 err=10048`을 남긴다. 이 줄이 보이면 엔진이 둘 뜬 것이다.

## 2. 프로토콜

### 프레임

한 프레임은 8바이트 헤더와 UTF-8 JSON 본문이다. 여러 프레임이 한 `recv`에 붙어 오거나 한 프레임이
여러 `recv`에 나뉘어 올 수 있다 — `ops::FrameReader`에 받은 바이트를 `feed()`하고 `next()`로 꺼낸다.

| 오프셋 | 길이 | 값 |
|---|---|---|
| 0 | 1 | `'Q'` |
| 1 | 1 | `'P'` |
| 2 | 1 | 버전, 지금은 1 |
| 3 | 1 | 메시지 타입(아래 표) |
| 4 | 4 | 본문 길이, 빅엔디언 uint32, 상한 1 MiB |
| 8 | n | 본문(JSON, 비어 있을 수 있음) |

매직·버전·길이 상한을 어기면 `FrameReader::bad()`가 참이 되고 서버는 그 연결을 끊는다.

### 메시지

| 타입 | 이름 | 방향 | 본문 |
|---|---|---|---|
| 0x01 | HELLO | 단말→서버 | `{"token":"…","client":"이름/버전"}` — 연결 뒤 첫 프레임이어야 한다 |
| 0x02 | WELCOME | 서버→단말 | `{"ok":true,"auth":bool,"engine":"quant_trader","paper":bool}` — 바로 뒤에 POSITIONS 스냅샷이 온다 |
| 0x03 | PING | 단말→서버 | `{}` |
| 0x04 | PONG | 서버→단말 | `{"ts":<ms>}` |
| 0x10 | STATUS_REQ | 단말→서버 | `{}` |
| 0x11 | STATUS | 서버→단말 | `{"running","data","signal","order","kill","entry_halt","force_liq","paper","strategies"}` |
| 0x12 | POS_REQ | 단말→서버 | `{}` |
| 0x13 | POSITIONS | 서버→단말 | `{"positions":[{"account","ticker","name","qty","avg_price","reserved","last"}]}` — 요청 응답이자, 내용이 바뀌면 1초 주기로 push. `reserved`는 부호 있는 미체결 수량: 매도 음수, 매수 양수. `last`는 엔진이 마지막으로 본 체결가(틱이 없던 종목은 0) |
| 0x20 | ORDER_REQ | 단말→서버 | `{"cid":"…","ticker":"005930","side":"SELL"|"BUY","qty":1,"price":0,"ref_price":0,"account":""}` |
| 0x21 | ORDER_ACK | 서버→단말 | `{"cid","accepted":bool,"msg"}` — 인테이크 적재 여부. 게이트·브로커 결과가 아니다 |
| 0x22 | ORDER_RESULT | 서버→전체 | `{"cid","order_id","odno","strategy","ticker","side","qty","price","ok":bool,"msg"}` — 게이트·라우터 결과 |
| 0x23 | FILL | 서버→전체 | `{"odno","ticker","side","qty","price","time"}` — 체결통보 |
| 0x30 | KILL | 단말→서버 | `{}` — 킬스위치를 켜고 엔진을 내린다 |
| 0x31 | KILL_ACK | 서버→단말 | `{"ok":bool,"msg"}` |
| 0x7F | ERROR | 서버→단말 | `{"msg"}` — 규약 위반이면 뒤에 끊고, 알 수 없는 타입이면 연결은 유지 |

규칙

- HELLO가 첫 프레임이 아니면 ERROR 뒤 끊는다. 토큰이 틀리면 같다.
- `auth=false`(서버에 토큰이 없거나 HELLO에 토큰을 안 냈을 때)면 ORDER_REQ·KILL은 거부 응답만 온다.
- `cid`는 단말이 붙이는 1~64자 식별자다. 같은 cid의 재전송은 한 번만 처리한다(연결이 끊겨 ACK를 못 받았을 때
  그대로 다시 보내면 된다).
- `price` 0은 시장가, 양수는 지정가. `ref_price`는 시장가의 명목 한도 평가 기준가로, 0이면 엔진이 평단으로 대체한다.
- 매도는 strategy_thread에서 매도가능(보유 − 대기 중 매도 reserved)을 넘으면 ORDER_RESULT `ok=false`로 거절된다.
- 수동 주문은 `strategy_id="MANUAL"`로 게이트·라우터·원장을 전략 주문과 똑같이 지난다. 원장·로그에 `[MANUAL]`로 남는다.
- push는 인증된 연결에만 간다. 연결마다 송신 대기 4 MiB를 넘으면 서버가 끊는다.

## 3. 콘솔 단말 `ops_client`

빌드하면 `Quant/build_win/ops_client.exe`(수동 Ninja) 또는 `out/build/x64-release/Quant/ops_client.exe`가 생긴다.

```
ops_client [--host 127.0.0.1] [--port 7100] [--token T] <명령>
  status                       엔진 상태
  positions                    보유 포지션 표
  sell <ticker> <qty> [price]  수동 매도 (price 없으면 시장가). ORDER_ACK → ORDER_RESULT까지 기다린다
  buy  <ticker> <qty> [price]  수동 매수
  watch                        접속을 유지하며 push(POSITIONS·ORDER_RESULT·FILL)를 출력
  kill                         킬스위치 + 엔진 종료 (토큰 필요)
```

종료 코드: 0 성공 / 1 인자·연결 오류 / 2 거절 / 3 결과 대기 시간 초과(15초).

예 — 저장소 루트에서, 모의 엔진에 1주 매도:

```powershell
cd C:\Users\<사용자>\source\repos\Quant
.\Quant\build_win\ops_client.exe --token <config의 ops_token> positions
.\Quant\build_win\ops_client.exe --token <config의 ops_token> sell 066570 1
```

출력 예

```
연결됨 paper=1 auth=1
인테이크 적재 cid=cli-1789094702775 — 게이트·브로커 결과 대기…
접수 ORD-000001 odno=0000023135
```

엔진 로그에는 같은 주문이 이렇게 남는다.

```
[Ops] 수동주문 → 게이트 cid=cli-1789094702775 066570 SELL 1 시장가
[Strategy] 신호: [MANUAL] 066570(LG전자) SELL 1 | 근거: 운영단말 수동주문 cid=cli-1789094702775
[OrderRouter] 접수 [ORD-000001] ODNO=0000023135 066570 SELL 1주 RTT=3578ms
[WS] 체결통보 ODNO=0000023135 066570 SELL 1주 @199100
```

## 4. MFC 단말 `ops_terminal`

포지션 표에서 종목을 골라 파는 대화상자 단말. 같은 `OpsProtocol.h`를 쓴다. 빌드 조건(MFC 구성 요소)·실행·화면·
스레드 모델·MFC 함정·변경 이력은 [docs/guides/MFC_TERMINAL.md](MFC_TERMINAL.md)가 정본이다.

```powershell
cmake --build out/build/x64-release --target ops_terminal
out\build\x64-release\Quant\ops_terminal.exe --token <config의 ops_token>
```

## 5. 테스트

```powershell
cmake --build out/build/x64-release --target test_ops_protocol test_ops_server
ctest --preset x64-release -R ops
```

`test_ops_server`는 127.0.0.1:17100을 실제로 열고 붙는다. 다른 프로세스가 그 포트를 쓰고 있으면 실패한다.

## 6. 남은 일

- **미체결 취소.** 단말에서 낸 지정가가 안 걸리면 취소할 길이 없다(엔진 `OrderAction::CANCEL`은 전략 경로만 쓴다).
  ORDER_REQ에 `action:"cancel"`+`odno`를 얹고 단말에 미체결 표·취소 버튼을 두는 일.
- **MANUAL 매도 재시도.** `OrderThread`는 "청산 SELL" 거부를 3회 재시도하는데 MANUAL도 이 경로를 탄다. 운영자가
  잘못 넣은 지정가(40270000 상/하한가 오류)도 세 번 나간다. MANUAL은 1회로 끊는 쪽이 맞다.
- **원격 bind.** 지금은 비루프백 주소 + 토큰이면 서버가 뜨지만 토큰·본문이 평문이다. LAN 밖에서 쓰려면
  SSH 터널이나 TLS(스트림 위에 프레임을 그대로 얹을 수 있다)와 접속 허용 IP 목록을 먼저 넣는다.
