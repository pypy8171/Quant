# 프리페치 스레드 수와 스냅샷 복사 비용 — 전략당 스레드 → 공용 풀, 벡터 복사 → 포인터 교체

> 하네스: [Quant/tests/bench_snapshot_swap.cpp](../../../Quant/tests/bench_snapshot_swap.cpp)(스냅샷 비용, 복사·포인터·atomic 세 열),
> [Quant/tests/test_prefetch_pool.cpp](../../../Quant/tests/test_prefetch_pool.cpp)(스레드 수 단언)
> 측정일: 2026-09-22 / 빌드: Release(NDEBUG), Ninja+MSVC / 머신: Windows, 16 HW threads
> 원자료: [data/2026-09-22_bench_snapshot_swap.csv](data/2026-09-22_bench_snapshot_swap.csv)
> 기준 커밋: main 6369412 위 `wt/prefetch-pool` / 결정: [docs/DECISIONS.md](../../DECISIONS.md) D-115

## 1. 왜 쟀나

`DeviationScaleStrategy`는 전략 객체마다 `std::jthread`를 하나 띄워 무거운 REST(일봉·분봉·잔고)를 미리 당겼다.
그 스레드는 종목 수에 비례한다 — 41종목 구성(전략 18개)에서 프로세스 스레드가 36~39개였고, 목표인 2,700종목이면 그대로
2,700개가 된다. D-071 원칙 1(스레드 수는 코어·부하로 정한다)·3(수신 스레드는 얇게)에 어긋난다.

같은 자리에서 평가(`min_action_ms` 3초)마다 일봉 250봉 + 분봉 63봉을 락 안에서 **복사**하고 있었다. 그중 분봉은 ws 체결이
살아 있으면 쓰지도 않고 버려졌다. "복사가 평가 한 번에 얼마나 드는가"를 재서, 포인터 교체로 바꿀 가치가 있는지 확인했다.

## 2. 무엇을 바꿨나

| 항목 | 전 | 후 |
|---|---|---|
| 프리페치 스레드 | 전략마다 `std::jthread` 1개 | 공용 `prefetch::Pool`(코어/2, 2~8개 고정), 전략은 함수 하나를 `add()`/`remove()` |
| 스냅샷 전달 | 락 안에서 `std::vector<MarketData>` 두 개 복사 | 락 안에서 `std::shared_ptr<const std::vector<MarketData>>` 두 개 교체 |
| 분봉 복사 | 평가마다 | `bar_source=rest`일 때만 한 번(ws 체결이 살아 있으면 0) |

## 3. 결과

### ① 스냅샷 잡는 비용 (`bench_snapshot_swap`, 20만 회 평균, 5회 반복)

MarketData 80바이트 × (일봉 250 + 분봉 63) = 25,040바이트를 뮤텍스 안에서 넘긴다.

| 회차 | 전(벡터 복사) ns/평가 | 후(포인터 교체) ns/평가 | 배 |
|---|---|---|---|
| 1 | 472 | 10 | 47.5 |
| 2 | 483 | 10 | 48.1 |
| 3 | 494 | 11 | 46.5 |
| 4 | 480 | 10 | 48.6 |
| 5 | 489 | 10 | 48.1 |

#### 덧 — 락 자체를 없애면? (`std::atomic<std::shared_ptr>`, 같은 날 2차 5회)

포인터 교체도 뮤텍스 안에서 포인터 두 개·버킷·버전 네 값을 읽는다. 넷을 구조체 하나로 묶어 `std::atomic<std::shared_ptr<const Snapshot>>`
하나를 `load()`하면 락이 사라진다. 그 비용을 세 번째 열로 쟀다(원자료 6~10행).

| 회차 | 전(벡터 복사) | 후(뮤텍스 + 포인터 교체) | 락 없이 atomic 하나 |
|---|---|---|---|
| 6~10 | 479~577 | 10~13 | **7** |

뮤텍스 → atomic은 평가당 3~4ns 차이다. 3초에 한 번인 평가에서는 0이고, 2,700종목이 전부 같은 순간에 평가해도 1초에 10µs다.
MSVC의 `atomic<shared_ptr>`는 안에서 스핀락을 쓰므로 경합이 생기면 이 7ns도 보장되지 않는다. **보류** — 수치가 안 나온다.
네 값을 구조체 하나로 묶는 정리는 가독성 이유로 따로 할 수 있지만, 락을 없애는 근거로는 쓰지 않는다.

읽는 법: 복사 하나가 약 0.5µs다. 3초에 한 번인 평가에서는 절대값이 작다 — 이 수치로 "느려서 고쳤다"고 말할 수는 없다.
고친 이유는 **평가마다 25KB를 새로 할당·해제하는 것이 종목 수에 비례**한다는 것이고(2,700종목이면 3초마다 67MB), 수치는
바꾼 뒤에 나빠진 것이 없음을 확인하는 용도다.

### ② 스레드 수 (`test_prefetch_pool`, ctest에 붙어 있음)

| 구성 | 전 | 후 |
|---|---|---|
| 작업(전략) 100개 | 스레드 100개 | 스레드 3개(생성자에서 정한 수, 테스트가 단언) |
| 전략 0개(FEED 모드·테스트) | 0개 | 0개(첫 `add()` 전에는 스레드를 안 띄운다) |
| 라이브 41종목·전략 18개 | 프로세스 36~39개(procwatch) | 배포 뒤 확인 — 아래 5절 |

### ③ 배경 표본 — 09-22 장중 라이브 트레이더 perf

같은 날 장중 리눅스 트레이더에서 받은 함수별 CPU 비중: `finish_task_switch` 16%, `do_sched_yield` 12%,
`Engine::shard_thread_fn` 8%. 문맥 교환이 엔진 본체보다 두 배 넘게 컸다. 이 표본의 원자료는 남기지 못했다(콘솔에서 읽은 값).
프리페치 스레드 18개가 그 교환의 몇 %인지는 이 표본만으로 못 가른다 — 배포 뒤 같은 절차([LOAD_TEST_GUIDE.md](../../guides/LOAD_TEST_GUIDE.md) 5절)로
다시 받아 비교하는 것이 다음 회차다.

## 4. 그래서 무엇을 정했나

D-115 채택. 스레드 수를 종목 수에서 떼어 냈고, 평가 경로의 스냅샷 복사를 없앴다. 상세와 버린 대안은
[docs/DECISIONS.md](../../DECISIONS.md) D-115.

## 5. 이 측정의 한계

- ①은 마이크로벤치다. 락 경합이 없는 단일 스레드에서 잰 값이라, 프리페치가 갱신 중일 때의 대기는 안 들어 있다.
  라이브에서는 그 대기가 복사 시간만큼 줄어드는 방향이므로 과소 추정이지 과대 추정은 아니다.
- ②의 "라이브 36~39 → ?"는 아직 안 쟀다. 배포는 장 마감 뒤(D-101)라 다음 거래일 procwatch `proc_stats.thread_count`로
  확인해야 한다. 사람이 열어 보지 않도록 `scripts/check_runtime_health.py`에 판정 행을 넣는 것이 남은 일이다.
- ③은 원자료가 없다. 숫자 자체보다 "문맥 교환이 본체보다 크다"는 모양만 믿는다.
- 다른 세션이 같은 PC에서 빌드 중이면 ①도 흔들린다. 5회 반복의 폭(472~494ns)이 그 흔들림의 크기다.

## 다시 돌려 보기

```cmd
cd /d C:\Users\...\source\repos\Quant
..\quant-devtools\wt_build.cmd
Quant\build_win\bench_snapshot_swap.exe
```

CSV 머리줄과 한 줄이 찍힌다. 그대로 [data/2026-09-22_bench_snapshot_swap.csv](data/2026-09-22_bench_snapshot_swap.csv) 아래에 붙인다.
스레드 수 단언은 `..\quant-devtools\wt_build.cmd test`가 `test_prefetch_pool`로 돌린다.
