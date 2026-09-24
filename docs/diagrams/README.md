# 그림 색인

구조와 코드 흐름을 그린 HTML이다. 브라우저로 바로 열어도 되고, 같은 내용이 아티팩트로도 올라가 있다
(링크는 `_private/LINKS.md` 표에도 있다).

<!-- sync: Quant/include/ipc/SharedLayout.h@e9d07ee Quant/include/core/CommandLine.h@d0474c5 Quant/src/core/Engine.cpp@7c823d3 docs/code_flow.toml@48548b4 -->

| 파일 | 무엇을 그렸나 | 발행한 주소 |
|---|---|---|
| [engine_processes.html](engine_processes.html) | 엔진을 시세·전략·주문 셋으로 가른 흐름, 둘 사이를 잇는 공유 면 열 개, 가르면서 뒤집힌 전제(D-114) | https://claude.ai/artifact/6ruh7SHu2Bp3sv3D1roysi |
| [code_walk.html](code_walk.html) | KIS 수신부터 주문·체결·DB 적재까지 64걸음. 걸음마다 파일:줄을 적어 코드로 바로 건너뛸 수 있다 | https://claude.ai/artifact/7EwTv8FMNjNmSZTHepiSyt |

## 다시 올리는 법

그림을 고쳤으면 **같은 URL로** 다시 올린다. 새로 올리면 주소가 바뀌어 `_private/dashboards.json`과
`_private/LINKS.md`의 링크가 죽는다. 주소를 굳이 바꿔야 하면 그 두 곳을 같이 고친다.

## 낡음 도장

위 `sync` 줄이 이 그림들이 보고 그린 소스다. 그 소스가 바뀌면 훅이 도장을 낡았다고 잡는다 —
그때 그림을 고쳐 같은 주소로 다시 올리고 도장을 새로 찍는다.

```
py ../quant-devtools/sync_impact.py --restamp docs/diagrams/README.md
```
