# Linux 빌드 및 실행 가이드 (WSL2 Ubuntu 22.04)

## 환경

- WSL2 Ubuntu 22.04
- Windows 파일은 `/mnt/c/...` 로 직접 접근 가능 → 클론 불필요

---

## Step 1. WSL2 터미널 열기

Windows에서 `wsl` 입력 또는 Ubuntu 앱 실행.

프로젝트 경로로 이동:

```bash
cd /mnt/c/Users/PYH/source/repos/Quant
```

> **성능 팁**: WSL2에서 `/mnt/c/` 경로는 파일 I/O가 느립니다.  
> 빌드만이라도 WSL 홈으로 복사하면 빠릅니다:
> ```bash
> cp -r /mnt/c/Users/PYH/source/repos/Quant ~/Quant
> cd ~/Quant
> ```

---

## Step 2. 의존성 설치

```bash
sudo apt update
sudo apt install -y \
    build-essential \
    cmake \
    ninja-build \
    libcurl4-openssl-dev

# ZMQ (선택 — ipc/ZmqBridge 사용 시)
sudo apt install -y libzmq3-dev
```

---

## Step 3. C++ 빌드

```bash
# 프로젝트 루트에서
cmake -DCMAKE_BUILD_TYPE=Release -G Ninja \
      -B Quant/build \
      -S Quant

cmake --build Quant/build -j$(nproc)

# 확인
ls -lh Quant/build/quant_trader
```

---

## Step 4. C++ 실행 확인

config.json은 Windows에 이미 있으므로 그대로 사용합니다.

### 4-1. REST 인증 확인 (US_TEST — WebSocket 없음)

```bash
./Quant/build/quant_trader Quant/config/config.json US_TEST
```

정상이면 M7 미국주식 시세 출력.

### 4-2. KR_TEST (REST + WebSocket)

```bash
./Quant/build/quant_trader Quant/config/config.json KR_TEST
```

- `[WS:연결]` → 완전 정상
- `[WS:끊김]` → REST는 정상, WebSocket만 연결 실패

> WSL2에서 KIS WebSocket(`ops.koreainvestment.com:21000`)은 plain TCP로 연결을 시도합니다.  
> Windows(WinHTTP)는 TLS로 연결하므로 동작 방식이 다릅니다.  
> `[WS:끊김]`이 떠도 REST 시세(현재가·PBR·PER)는 정상 출력됩니다.

---

## Step 5. Python 셋업

```bash
cd /mnt/c/Users/PYH/source/repos/Quant/PYQuant
# 또는 복사한 경우
cd ~/Quant/PYQuant

# Python 버전 확인 (3.10+ 필요)
python3 --version

# 가상환경
python3 -m venv .venv
source .venv/bin/activate

# 패키지 설치
pip install -r requirements.txt
```

---

## Step 6. Python 실행 확인

### 6-1. 인증 확인

```bash
python3 -c "
from kis.client import from_config
k = from_config()
print('인증 성공' if k.authenticate() else '인증 실패')
"
```

### 6-2. 단일 종목 시세

```bash
python3 -c "
from kis.client import from_config
k = from_config()
k.authenticate()
for b in k.get_daily_ohlcv('005930', 5):
    print(b)
"
```

### 6-3. 백테스팅

```bash
python3 main.py backtest --from 2025-01-01 --to 2025-03-31
```

### 6-4. ZMQ 모니터 (C++ TRADE 모드 실행 중일 때)

터미널 1:
```bash
./Quant/build/quant_trader Quant/config/config.json TRADE
```

터미널 2:
```bash
cd PYQuant && source .venv/bin/activate
python3 main.py monitor
```

---

## 확인 체크리스트

| 순서 | 항목 | 명령 | 성공 기준 |
|------|------|------|-----------|
| 1 | C++ 빌드 | `cmake --build Quant/build` | `[100%] Linking` |
| 2 | C++ REST | `US_TEST` 실행 | M7 시세 출력 |
| 3 | C++ WS | `KR_TEST` 실행 | KOSPI 20종목 표시 |
| 4 | Python 인증 | `python3 -c "..."` | `인증 성공` |
| 5 | Python 시세 | `get_daily_ohlcv` | Bar 5개 출력 |
| 6 | Python 백테스트 | `main.py backtest` | 수익률 리포트 출력 |

---

## 자주 겪는 문제

### `libcurl` 없음

```
CMake Error: Could not find CURL
```

```bash
sudo apt install libcurl4-openssl-dev
```

### 한글 깨짐

```bash
export LANG=ko_KR.UTF-8
export LC_ALL=ko_KR.UTF-8
```

영구 적용:

```bash
echo 'export LANG=ko_KR.UTF-8' >> ~/.bashrc
source ~/.bashrc
```

### `/mnt/c/` 경로에서 빌드 느림

WSL2 홈으로 복사 후 빌드:

```bash
cp -r /mnt/c/Users/PYH/source/repos/Quant ~/Quant
cd ~/Quant
cmake -DCMAKE_BUILD_TYPE=Release -G Ninja -B Quant/build -S Quant
cmake --build Quant/build -j$(nproc)
```

단, 이 경우 config.json도 복사됩니다.
