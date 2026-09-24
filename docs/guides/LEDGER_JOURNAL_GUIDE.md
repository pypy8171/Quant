# 원장 저널 조회 가이드

> 트레이더는 주문을 보내기 전에 원장 이벤트(보유·주문·체결·현금)를 일자별 파일에 먼저 적는다(D-113).
> 이 문서는 그 파일을 사람이 여는 방법을 적는다. 파일 형식의 정본은 `Quant/include/risk/LedgerJournal.h`,
> 조회 도구는 `PYQuant/tools/ledger_dump.py`다.

## 1. 파일이 있는 곳

파일 이름은 `ledger_YYYYMMDD.bin`이고 거래일마다 하나 생긴다. 폴더는 실행한 config의 `ledger_journal_dir` 값이다.
경로는 저장소 루트 기준이다.

| config | 폴더 | 계좌 |
|---|---|---|
| `Quant/config/config_dev_paper.json` | `Quant/build_win/logs_paper/` | 모의투자 |
| `Quant/config/config_live.json` | `Quant/build_win/logs_live/` | 실계좌 |
| `Quant/config/config.json` | `Quant/build_win/logs/` | 기본 |

모의와 실계좌는 폴더를 나눈다. 섞이면 재기동 리플레이가 다른 계좌의 주문을 복원한다.

## 2. 여는 순서

1. Windows 키 + R → `powershell` 입력 → Enter.
2. 저장소 루트로 이동한다.
   ```powershell
   cd <저장소 루트>        # 예: cd $HOME\source\repos\Quant
   ```
3. 어떤 날짜 파일이 있는지 본다.
   ```powershell
   dir Quant\build_win\logs_paper\ledger_*.bin
   ```
4. 그날 원장 전체를 본다. 날짜만 바꿔 쓴다.
   ```powershell
   py PYQuant\tools\ledger_dump.py Quant\build_win\logs_paper\ledger_20260924.bin
   ```

파이썬은 `python`이 아니라 `py`로 부른다(이 PC의 `python`은 스토어 스텁이다).
도구는 파일을 읽기만 하므로 장중에 돌려도 트레이더에 영향이 없다.

## 3. 출력 읽는 법

```
sequence 시각           종류   종목    방향  수량   가격    ODNO 전략  비고
     1 15:37:16.654 SEED   001120  BUY   32   41,991   0          선점 0 · 매도가능 32
```

| 칸 | 뜻 |
|---|---|
| sequence | 그날 몇 번째 기록인지(1부터) |
| 시각 | 기록한 시각(KST) |
| 종류 | 아래 표 |
| 종목 | 종목코드 |
| 방향 | BUY 매수, SELL 매도 |
| 수량·가격 | 주문 또는 체결 수량과 가격 |
| ODNO | KIS 주문번호 |
| 전략 | 주문을 낸 전략 |
| 비고 | 거부 사유, 선점·매도가능 수량 등 |

| 종류 | 뜻 |
|---|---|
| SEED | 기동 때 잔고조회로 불러온 보유 종목 |
| INTENT | 주문을 보내기 직전에 적은 기록 |
| ACCEPT | KIS가 주문을 접수함 |
| REJECT | 주문 거부(비고에 사유) |
| FILL | 체결 |
| CANCEL | 취소 |
| ADJUST | KIS 잔고와 대조해 수량을 맞춤 |
| RESET_RESERVED | 주문용으로 잡아 둔 수량(선점)을 풂 |
| CASH | 주문가능현금·총평가금 |
| DAILY_PNL | 당일 손익 |

## 4. 골라 보기

4번 명령 뒤에 옵션을 붙인다.

| 옵션 | 보는 것 |
|---|---|
| `--kind FILL` | 체결만. 쉼표로 여럿(`--kind FILL,REJECT`) |
| `--ticker 005930` | 종목 하나 |
| `--order <번호>` | 내부 주문번호 하나의 전 과정 |
| `--since-sequence <번호>` | 그 sequence 뒤만 |
| `--positions` | 종목별 보유·평단·선점 재구성 |
| `--open-intents` | 결말을 못 본 주문(재기동 대조 대상) |
| `--csv <파일>` | CSV로 내보내기 |

엑셀로 열 때:

```powershell
py PYQuant\tools\ledger_dump.py Quant\build_win\logs_paper\ledger_20260924.bin --csv ledger_0924.csv
start ledger_0924.csv
```

CSV는 저장소 루트에 생긴다. 다 봤으면 지운다.

## 5. 파일 형식

- 16바이트 헤더(magic·version·레코드 크기·날짜) 뒤에 192바이트 고정 레코드가 이어진다.
- 레코드마다 CRC32가 있다. 꼬리가 잘렸거나 CRC가 틀린 레코드에서 읽기를 멈춘다 — 엔진 리플레이가 멈추는 지점과 같다.
- 도구는 `struct` 포맷 문자열로 C++ 구조체를 그대로 풀어 읽는다. `Record`를 바꾸면 `kVersion`을 올리고
  `PYQuant/tools/ledger_dump.py`의 `RECORD_FORMAT`을 같이 고친다.
- 크기: 레코드 약 5,400개가 1MB다. 지금까지 가장 많은 날(2026-09-23 모의)이 5,210건·1.0MB였다.

## 6. 막힐 때

| 증상 | 확인할 것 |
|---|---|
| `py`를 찾을 수 없음 | 파이썬 설치 여부 |
| 파일이 없다고 나옴 | 3번으로 날짜·폴더(`logs_paper`·`logs_live`·`logs`) 확인 |
| 건수가 생각보다 적음 | 깨진 레코드에서 멈췄을 수 있다. 첫 줄의 "마지막 sequence"를 본다 |
