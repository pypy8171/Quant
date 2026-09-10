# zone_check — 네이버 일봉으로 점수·존 판정을 재현한다

엔진이 종목을 고르는 순서를 밖에서 그대로 따라 해 보는 단독 실행 파일이다.
계좌에 접속하지 않고 발주도 하지 않는다. KIS 인증 정보도 쓰지 않는다.

```
네이버 일봉 수집 → 정배열 프리필터 → 횡단면 z-score 점수 → 순위·비중배수 → 존 판정
```

각 단계가 대응하는 엔진 코드는 `main.cpp` 머리 주석에 적어 뒀다.

## 실행 방법 1 — Visual Studio 2022로 폴더 열기 (권장)

1. Visual Studio 2022를 연다.
2. **파일 → 열기 → 폴더**(File → Open → Folder)에서 `tools\zone_check` 폴더를 고른다.
   `CMakeLists.txt`가 있어 VS가 CMake 프로젝트로 알아서 구성한다.
   (설치할 때 "C++를 사용한 데스크톱 개발" 워크로드에 CMake 도구가 포함돼 있어야 한다.
   없으면 Visual Studio Installer → 수정 → 개별 구성 요소에서 `Windows용 C++ CMake 도구`를 추가한다.)
3. 하단 출력 창에 `CMake 생성이 완료되었습니다`가 뜰 때까지 기다린다.
4. 상단 시작 항목 드롭다운에서 `zone_check.exe`를 고른다.
5. **Ctrl+F5**(디버깅하지 않고 시작)로 실행한다. F5로 실행하면 콘솔이 바로 닫힌다.

## 실행 방법 2 — 명령줄

시작 메뉴에서 **x64 Native Tools Command Prompt for VS 2022**를 연다. 그 다음 그대로 복사해 붙인다.

```cmd
cd /d %USERPROFILE%\source\repos\Quant\tools\zone_check
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -B build -S .
cmake --build build
build\zone_check.exe
```

Ninja가 없으면 생성기를 바꾼다.

```cmd
cmake -G "Visual Studio 17 2022" -A x64 -B build -S .
cmake --build build --config Release
build\Release\zone_check.exe
```

컴파일러만으로 한 줄에 만들 수도 있다.

```cmd
cl /utf-8 /EHsc /O2 /std:c++17 main.cpp winhttp.lib /Fe:zone_check.exe
zone_check.exe
```

## 종목 지정

인자를 주지 않으면 오늘 유니버스에 있던 15종목을 쓴다. 인자를 주면 그것으로 대체한다.

```cmd
zone_check.exe 005930 000660 047050 036930
```

## 출력 읽는 법

수집 구간에서 종목마다 한 줄이 나온다.

```
  047050 포스코인터           일봉 245개  종가     57050  SMA20   56722.5  이격  +0.58%  정배열 Y
```

그 다음 슬리브마다 표가 나온다. `이격%`는 현재가의 SMA20 대비 편차이고,
`존`은 `정배열 && SMA20 확보 && 진입밴드 안` 세 조건을 모두 만족했는지다.

```
프리필터: 입력=15 역배열컷=6 데이터부족=0 과확장컷=2 통과=7
순위   종목     이름                 현재가      SMA20    이격%     점수   배수     존
 1/7   047050   포스코인터            57050    56722.5     0.58    1.842   1.41   활성
 ...
 6/7   030200   KT                    48200    46100.0     4.56   -0.913   0.71   활성  (슬롯밖)
```

`(슬롯밖)`은 점수 순위가 슬리브 슬롯 수를 넘어 등록되지 않는다는 뜻이다.

## 이 파일이 재현하지 않는 것

실제 발주 여부는 여기서 알 수 없다. 존 활성 뒤에도 `OrderGate`가 네 가지를 더 본다.

| 관문 | 코드 |
|---|---|
| 점수 우선순위 기준선 | [OrderGate.cpp](../../Quant/src/risk/OrderGate.cpp) `check` §3c 유효 랭크 |
| 슬롯 수 | [OrderGate.cpp](../../Quant/src/risk/OrderGate.cpp) `check` §3c 동시 보유 상한 |
| 명목·총노출 한도 | [OrderGate.cpp](../../Quant/src/risk/OrderGate.cpp) `clamp_buy_qty` |
| 중복·간격 | `min_action_ms`, `min_rebuild_sec` |

그리고 엔진은 3분봉 현재가를 얹어 SMA를 갱신하지만 여기서는 일봉 종가만 쓴다.
장중에 돌리면 `현재가`가 전일 종가로 잡혀 이격이 엔진 값과 조금 어긋난다.
장 마감 뒤에 돌리면 두 값이 맞는다.

## 알려진 제약

- 네이버가 응답 형식을 바꾸면 `parse_sise()`가 빈 배열을 돌려준다. 그때는 수집 실패로 표시된다.
- 호출 간격을 120ms 두었다. 종목을 수십 개로 늘리면 그만큼 느려진다.
- 정배열 판정에 `align_ma_tol_pct` 허용오차는 넣지 않았다(엔진 기본값 0과 같다).

## 종목을 적게 주면 배수가 커지는 이유

`배수`는 슬리브 총목표를 상위 슬롯 종목들에 나눠 주는 값이다
(`scale = target_total_pct / (base_pct × Σ상위 raw)`). 3종목만 주면 80%를 셋이 나눠 갖게 되어
배수가 7 근처까지 뜬다. 엔진에서는 17~25종목이 들어오므로 1 근처에 모인다.
배수를 실제와 비슷하게 보고 싶으면 종목을 15개 이상 준다.
