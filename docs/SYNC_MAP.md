# SYNC_MAP — 문서 동기화 맵 (드리프트 방지)

> 목적: **자식(소스) 파일을 고쳤을 때 같이 갱신해야 할 대표(색인) 파일**을 즉시 특정한다.
> 색인·요약·링크가 실제 트리와 어긋나는 드리프트(과거 07/08/09 스터디가 상위 색인에서 누락됐던 사고)를 막는다.

두 층으로 검사한다:
1. **결정론 검사(자동)** — [`scripts/check_docs.py`](../scripts/check_docs.py). 깨진 내부 링크 + 색인 커버리지를 기계적으로 잡는다. `@committer`가 커밋 전 실행한다.
2. **의미적 체크리스트(사람)** — 스크립트가 못 잡는 "요약 문구가 최신인가·결론이 바뀌었나"를 아래 표로 확인한다.

---

## 1. 소유권 원칙 — 1서사 = 1소유자

같은 내용을 여러 곳에 복제하지 않는다. 각 서사는 **소유자 1곳**만 풀텍스트로 쓰고, 나머지는 링크한다.

| 서사 | 소유자(풀텍스트) | 링크만(복제 금지) |
|---|---|---|
| 백테스트 실행 상세(규칙변경·결과표·해석) | `research/BACKTEST_LOG.md` | `research/BACKTESTS.md` 카드, `research/README.md` 타임라인 |
| 스터디 내용(방법·결과·종목원장) | `research/studies/<NN>/README.md` | `research/studies/README.md` 색인, `BACKTESTS.md` 카드 |
| 전략 스펙·실증 | `strategies/<전략>/`(SPEC·live/) | `strategies/README.md` 표, 루트 `README.md` |
| 백테스트 규율 | `research/GUARDRAILS.md` | 각 study README, `bias-auditor` |

---

## 2. 의존 표 — 자식이 바뀌면 이 대표 파일을 갱신하라

| 대표(색인) 파일 | 색인/요약하는 소스 | 갱신 트리거 |
|---|---|---|
| 루트 [`README.md`](../README.md) | research 허브·strategies 허브·아키텍처·OrderGate | 결론(regime 판정 등)·구조 변경 시 요약 1줄 동기화 |
| [`research/README.md`](../research/README.md) (허브) | BACKTESTS·BACKTEST_LOG·studies/·GUARDRAILS·RESEARCH_COUNCIL·BACKTEST_FLOW·_TEMPLATE | 새 문서 추가·계열 결론 변경 시 지도표·타임라인 갱신 |
| [`research/BACKTESTS.md`](../research/BACKTESTS.md) | studies 01~06 README, BACKTEST_LOG 실행#, runs/ | 계열 A 새 실행 시 카탈로그 한 줄 추가 |
| [`research/studies/README.md`](../research/studies/README.md) | studies `<NN>/README.md` (A:01·02·03·06 / B:07·08·09) | 새 스터디 폴더 추가 시 해당 계열 섹션에 등재 |
| [`strategies/README.md`](../strategies/README.md) | `strategies/<전략>/SPEC·live/` | 새 전략 폴더·새 SPEC 추가 시 표에 행 추가 |
| [`CLAUDE.md`](../CLAUDE.md) | 빌드 명령·스레드 모델·핵심 타입 | 빌드/아키텍처 코드 변경 시 |
| [`docs/guides/PROJECT_GUIDE.md`](guides/PROJECT_GUIDE.md) | 디렉터리 구조·기술스택·코드 파일 링크 | 파일 이동/리네임 시 링크 재검 |
| [`docs/GLOSSARY.md`](GLOSSARY.md) | 전략·인프라·데이터·설정 약어 사전 | 새 전략/개념 추가·약어 신설 시 항목 추가 |

> `research/BACKTEST_LOG.md`는 **소스(소유자)**라 위 표의 "대표"가 아니다 — 다른 문서가 이걸 링크한다.
> `STRATEGY_LAB.md`·`ARCHITECTURE.md` 등 gitignore 개인문서는 GitHub에 없으므로 색인에서 **하드링크하지 말 것**(텍스트+"로컬전용" 표기만).

---

## 3. 커밋 전 체크리스트 (스크립트가 못 잡는 의미적 최신성)

`scripts/check_docs.py`는 **링크 깨짐·색인 등재 여부**만 본다. 아래는 사람이 확인한다:

- [ ] **새 백테스트 실행** → `BACKTEST_LOG`에 원문 prepend + `BACKTESTS` 카탈로그 한 줄 + `research/README` 타임라인 서사 1줄(수치는 링크).
- [ ] **새 스터디 폴더** → `studies/README`의 **올바른 계열**(A 종목레벨 / B 지수레벨위기) 섹션에 요지 1줄로 등재. (등재 누락 자체는 스크립트가 잡음)
- [ ] **새 전략 폴더** → `strategies/README` 표에 SPEC·실증·검증경로 행 추가.
- [ ] **결론이 바뀜**(예: regime 유효성 재판정, 지표 채택) → 루트 `README`·`research/README`의 요약 문구를 **함께** 고쳤나.
- [ ] **파일 이동/리네임** → `python scripts/check_docs.py` 통과 확인(깨진 상대링크 0).

---

## 4. 사용

```bash
python scripts/check_docs.py   # exit 0 = 통과, 1 = 드리프트(항목별 출력)
```

`@committer`는 문서를 포함한 커밋 전 이 스크립트를 실행하고, 실패 시 커밋을 멈추고 보고한다.
