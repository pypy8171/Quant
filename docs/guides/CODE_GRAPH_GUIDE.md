# 코드 의존 그래프 가이드

코드에서 직접 뽑은 모듈·파일 의존 그래프를 만들고, 편집·커밋·리팩터 전 영향범위 산정과 증분빌드 최적화에 쓰는 방법을 정리한다. 생성물 자체는 [../CODE_GRAPH.md](../CODE_GRAPH.md)에 있고, 이 문서는 사용법·연계를 담는다.

## 무엇인가

[../../scripts/gen_code_graph.py](../../scripts/gen_code_graph.py)가 `Quant/include`·`Quant/src`를 스캔해 로컬 `#include "..."` 관계를 뽑는다. 표준·외부 헤더는 제외하고, 모듈(api·core·ipc·modes·risk·strategy·universe·utils·main) 간 의존과 파일 단위 정의존/역의존을 만든다.

산출물 네 가지:

- [../CODE_GRAPH.md](../CODE_GRAPH.md) — 모듈 의존 그래프 + 공용 허브 표 + 파일 상세(Mermaid, 렌더 도구 불필요)
- `docs/code_graph.dot` — Graphviz DOT(설치 시 SVG 렌더용)
- `docs/code_graph.json` — 기계 소비용(정의존·역의존·허브). `--json`으로 생성
- 콘솔 질의 — `--impact`

## 생성·재생성

```bash
py scripts/gen_code_graph.py            # md + dot
py scripts/gen_code_graph.py --json     # md + dot + json
```

산출물은 자동 생성물이라 손으로 고치지 않는다. 코드가 바뀌면 스크립트를 다시 돌려 갱신한다(이 프로젝트 생성기 규약과 동일). 파이썬은 `py` 런처로 실행한다.

## 영향범위 질의 (--impact)

특정 헤더를 건드릴 때 재검증·재빌드해야 할 대상을 파일을 열지 않고 뽑는다. 직접 include와 전이(헤더를 타고 번지는) 포함을 함께 센다.

```bash
py scripts/gen_code_graph.py --impact core/Types.h
```

출력은 직접/전이 파일 목록과 영향 모듈 집합이다. 예로 `core/Types.h`는 직접 13개·전이 포함 33개·8개 모듈에 걸린다.

## 워크플로 연계

코드리뷰·코드작성 밖으로 넓히는 지점:

- **편집 전** — `--impact <헤더>`로 blast radius(영향범위)를 확정해 재읽기·재검증을 해당 모듈로만 좁힌다. CLAUDE.md의 "바뀐 범위만 재검증"을 데이터로 뒷받침한다.
- **커밋 전** — 변경이 닿는 모듈을 확인해 커밋 범위를 점검한다(`@committer` 흐름).
- **리팩터·전략 추가** — 착수 전 의존 강도로 위험을 가늠한다.
- **기계 소비** — `code_graph.json`의 역의존을 서브에이전트가 파일읽기 없이 읽어 탐색 왕복을 줄인다.

토큰 효용은 코드베이스가 커질수록 붙는다. 지금 규모(수십 파일)에선 Grep도 싸서 절감폭은 완만하며, 이 그래프는 탐색을 대체한다기보다 거드는 인덱스다.

## 증분빌드 최적화 (허브 헤더)

[../CODE_GRAPH.md](../CODE_GRAPH.md)의 공용 허브 표는 유입(누가 나를 include)이 많은 헤더를 보여준다. `utils/Logger.h`·`core/Types.h`처럼 유입이 큰 헤더는 한 줄만 바꿔도 그만큼의 번역단위가 재컴파일된다. 증분빌드를 줄이려면 이런 허브 헤더를 얇게 유지한다(무거운 include를 전방선언이나 pimpl로 분리). 이는 런타임이 아니라 컴파일타임(재빌드 팬아웃) 쪽 최적화다.

## 한계

include·모듈 수준 그래프라 함수 호출 관계(call graph)까지는 담지 않는다. 호출 그래프·클래스 다이어그램이 필요하면 Doxygen + Graphviz를 별도로 설치해 보완한다. 지금 목적(아키텍처 문서화·영향범위 산정)에는 include 그래프로 충분하다.
