#!/usr/bin/env python3
"""운영 런북 — docs/RUNBOOK.md(정본)를 docs/RUNBOOK.html(복사 버튼 달린 로컬 페이지)로 렌더한다.

  py scripts/gen_runbook.py            렌더 (gen_facts.py --apply 가 허브와 같이 부른다)
  py scripts/gen_runbook.py --check    코드 블록이 가리키는 저장소 파일이 다 있는지만 본다. 없으면 exit 1 (check_docs.py 가 부른다)

`{ROOT}` 는 저장소 절대경로(Windows 역슬래시)로 치환한다 — 경로에 사용자명이 들어가서 HTML 은 gitignore 다.
마크다운은 이 파일이 쓰는 만큼만 받는다: `## ` 절, 문단, 코드 펜스, `|` 표, `- ` 목록, 인라인 코드·링크·굵게.
절 머리의 <!-- sync: --> 도장은 HTML 에 내지 않는다(낡음 검사는 sync_impact.py 몫).
"""
from __future__ import annotations

import html
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "docs" / "RUNBOOK.md"
OUT = ROOT / "docs" / "RUNBOOK.html"

# 코드 블록 안에서 "저장소 파일"로 보는 토큰. exe·산출물은 빌드 전엔 없으니 보지 않는다
PATH_RE = re.compile(r"(?<![\w.])((?:scripts|PYQuant\\tools|PYQuant/tools|Quant\\config|Quant/config|docs)[\\/][\w\\/.\-]+?\.(?:py|ps1|json|md|toml))")
INLINE_CODE_RE = re.compile(r"`([^`]+)`")
LINK_RE = re.compile(r"\[([^\]]+)\]\(([^)]+)\)")
BOLD_RE = re.compile(r"\*\*([^*]+)\*\*")
STAMP_RE = re.compile(r"^\s*<!--\s*sync:.*?-->\s*$")

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass


def code_blocks(text: str) -> list[str]:
    return re.findall(r"^```[^\n]*\n(.*?)^```", text, flags=re.S | re.M)


def check_paths(text: str) -> list[str]:
    missing = []
    for block in code_blocks(text):
        for token in sorted(set(PATH_RE.findall(block))):
            rel = token.replace("\\", "/")
            if not (ROOT / rel).exists():
                missing.append(f"docs/RUNBOOK.md: 코드 블록의 {token} 가 저장소에 없다 — 명령을 고치거나 절을 지운다")
    return missing


def inline(text: str) -> str:
    """인라인 마크다운 → HTML. 코드 조각 안은 이스케이프만 한다."""
    parts = []
    last = 0
    for m in INLINE_CODE_RE.finditer(text):
        parts.append(inline_plain(text[last:m.start()]))
        parts.append("<code>" + html.escape(m.group(1)) + "</code>")
        last = m.end()
    parts.append(inline_plain(text[last:]))
    return "".join(parts)


def inline_plain(text: str) -> str:
    text = html.escape(text, quote=False)
    text = LINK_RE.sub(lambda m: f'<a href="{m.group(2)}">{m.group(1)}</a>', text)
    return BOLD_RE.sub(r"<strong>\1</strong>", text)


def render_table(rows: list[str]) -> str:
    cells = [[c.strip() for c in r.strip().strip("|").split("|")] for r in rows if not re.match(r"^\s*\|?\s*-{2,}", r)]
    if not cells:
        return ""
    head = "".join(f"<th>{inline(c)}</th>" for c in cells[0])
    body = "".join("<tr>" + "".join(f"<td>{inline(c)}</td>" for c in r) + "</tr>" for r in cells[1:])
    return f"<table><thead><tr>{head}</tr></thead><tbody>{body}</tbody></table>"


def render(md: str, root_text: str) -> str:
    md = md.replace("{ROOT}", root_text)
    lines = md.splitlines()
    out: list[str] = []
    toc: list[tuple[str, str]] = []
    para: list[str] = []
    bullets: list[str] = []
    table: list[str] = []
    title = "운영 런북"
    intro_done = False

    def flush() -> None:
        nonlocal para, bullets, table
        if para:
            out.append("<p>" + inline(" ".join(s.strip() for s in para)) + "</p>")
            para = []
        if bullets:
            out.append("<ul>" + "".join(f"<li>{inline(b)}</li>" for b in bullets) + "</ul>")
            bullets = []
        if table:
            out.append(render_table(table))
            table = []

    index = 0
    while index < len(lines):
        line = lines[index]
        if line.startswith("```"):
            flush()
            index += 1
            code: list[str] = []
            while index < len(lines) and not lines[index].startswith("```"):
                code.append(lines[index])
                index += 1
            out.append('<div class="cmd"><pre>' + html.escape("\n".join(code), quote=False) + '</pre><button class="cp">복사</button></div>')
            index += 1
            continue
        if line.startswith("# "):
            title = line[2:].strip()
            index += 1
            continue
        if line.startswith("## "):
            flush()
            if not intro_done:
                out.append("<!--TOC-->")
                intro_done = True
            heading = line[3:].strip()
            heading_id = "s" + str(len(toc) + 1)
            toc.append((heading_id, heading))
            out.append(f'<h2 id="{heading_id}">{inline(heading)}</h2>')
            index += 1
            continue
        if STAMP_RE.match(line):
            index += 1
            continue
        if line.startswith("|"):
            if para or bullets:
                flush()
            table.append(line)
            index += 1
            continue
        if line.startswith("- "):
            if para or table:
                flush()
            bullets.append(line[2:])
            index += 1
            continue
        if not line.strip():
            flush()
            index += 1
            continue
        if bullets or table:
            flush()
        para.append(line)
        index += 1
    flush()

    nav = "<nav>" + "".join(f'<a href="#{a}">{html.escape(h.split(".", 1)[0] if h[0].isdigit() else h)}. {html.escape(h.split(". ", 1)[-1])}</a>' for a, h in toc) + "</nav>"
    body = "\n".join(out).replace("<!--TOC-->", nav)
    return TEMPLATE.replace("{TITLE}", html.escape(title)).replace("{BODY}", body)


TEMPLATE = """<!doctype html><html lang="ko"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>{TITLE}</title>
<style>
:root{--bg:#0f1115;--panel:#171a21;--bd:#2a2f3a;--fg:#e6e9ef;--mut:#8b93a7;--acc:#5b9dff;--ok:#2ec26b;--code:#0b0d12}
@media(prefers-color-scheme:light){:root{--bg:#f4f6fa;--panel:#fff;--bd:#dde1ea;--fg:#1a1d24;--mut:#5c6473;--acc:#2f6fe0;--ok:#0a9d52;--code:#f0f2f7}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.6 -apple-system,'Segoe UI',Roboto,'Malgun Gothic',sans-serif}
.wrap{max-width:960px;margin:0 auto;padding:24px 20px 80px}
h1{font-size:22px;margin:0 0 12px}
h2{font-size:17px;margin:32px 0 10px;padding-top:8px;border-top:1px solid var(--bd)}
nav{display:flex;flex-wrap:wrap;gap:6px 10px;margin:12px 0 20px;position:sticky;top:0;background:var(--bg);padding:10px 0;z-index:2;border-bottom:1px solid var(--bd)}
nav a{color:var(--acc);text-decoration:none;font-size:13px;white-space:nowrap}
p{margin:8px 0}
ul{margin:6px 0 6px 20px;padding:0}
code{background:var(--code);border:1px solid var(--bd);border-radius:4px;padding:1px 5px;font:12.5px/1.5 Consolas,'Cascadia Mono',monospace}
a{color:var(--acc)}
.cmd{position:relative;margin:10px 0;background:var(--code);border:1px solid var(--bd);border-radius:8px}
.cmd pre{margin:0;padding:12px 78px 12px 14px;overflow-x:auto;font:13px/1.55 Consolas,'Cascadia Mono',monospace;white-space:pre}
.cp{position:absolute;top:8px;right:8px;border:1px solid var(--bd);background:var(--panel);color:var(--fg);border-radius:6px;padding:4px 10px;cursor:pointer;font-size:12px}
.cp.ok{color:var(--ok);border-color:var(--ok)}
table{border-collapse:collapse;margin:8px 0;font-size:13px}
th,td{border:1px solid var(--bd);padding:5px 9px;text-align:left;vertical-align:top}
th{background:var(--panel)}
.foot{margin-top:40px;color:var(--mut);font-size:12px}
</style></head><body><div class="wrap">
<h1>{TITLE}</h1>
{BODY}
<p class="foot">docs/RUNBOOK.md 에서 scripts/gen_runbook.py 가 만든다 — 이 HTML 은 손으로 고치지 않는다.</p>
</div>
<script>
document.querySelectorAll('.cmd').forEach(c=>{
  const btn=c.querySelector('.cp'), pre=c.querySelector('pre');
  btn.addEventListener('click',()=>{
    const txt=pre.innerText;
    const done=()=>{btn.textContent='복사됨';btn.classList.add('ok');setTimeout(()=>{btn.textContent='복사';btn.classList.remove('ok');},1200);};
    if(navigator.clipboard&&navigator.clipboard.writeText){navigator.clipboard.writeText(txt).then(done,()=>fallback(txt,done));}
    else fallback(txt,done);
  });
});
function fallback(txt,done){
  const ta=document.createElement('textarea');ta.value=txt;ta.style.position='fixed';ta.style.opacity='0';
  document.body.appendChild(ta);ta.select();try{document.execCommand('copy');done();}catch(e){}document.body.removeChild(ta);
}
</script></body></html>
"""


def main(argv: list[str]) -> int:
    md = SRC.read_text(encoding="utf-8-sig")
    missing = check_paths(md)
    if "--check" in argv:
        for line in missing:
            print(line)
        return 1 if missing else 0
    root_text = str(ROOT).replace("/", "\\")
    OUT.write_text(render(md, root_text), encoding="utf-8", newline="\n")
    print(f"[ok] {OUT.relative_to(ROOT)} ← {SRC.relative_to(ROOT)}" + (f" (경로 없음 {len(missing)}건)" if missing else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
