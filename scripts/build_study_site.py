# -*- coding: utf-8 -*-
"""주식 스터디 리더 HTML 생성기.

`_private/주식_study/` 아래의 날짜별 저널(`YYYY-MM-DD.md`)과 재무 폴더
(`YYYY-MM-DD_재무/`)를 전부 읽어 단일 HTML로 묶는다. 날짜 전환 탭이 있어
새 스터디를 돌린 뒤 이 스크립트를 다시 돌리면 최신 날짜가 기본으로 열린다.

    py scripts/build_study_site.py [출력경로]

기본 출력은 `_private/주식_study/_site/study_site.html`(gitignore 대상).
"""
import json
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE = os.path.join(REPO, "_private", "주식_study")
DEFAULT_OUT = os.path.join(BASE, "_site", "study_site.html")

DATE_RE = re.compile(r"^(\d{4}-\d{2}-\d{2})$")
FIN_RE = re.compile(r"^(\d{4}-\d{2}-\d{2})_재무$")
CODE_RE = re.compile(r"^\d{6}")
ROW_RE = re.compile(r"^\|(.+)\|\s*$")
H3_RE = re.compile(r"^###\s+(.+?)\s*$")


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def parse_index(readme):
    """README 표에서 리포트 파일별 (섹터, 한 줄 총평)을 뽑는다.

    표 형식이 날짜마다 다르다(섹터별 `###` 분할 표 / 섹터 열이 있는 단일 표).
    링크 셀의 파일명을 키로 잡고 나머지 셀을 형태로 판별해 양쪽을 모두 받는다.
    """
    meta = {}
    order = []
    section = None
    for line in readme.splitlines():
        h = H3_RE.match(line)
        if h:
            section = h.group(1).strip()
            continue
        rm = ROW_RE.match(line)
        if not rm:
            continue
        cells = [c.strip() for c in rm.group(1).split("|")]
        link = None
        link_i = -1
        for i, c in enumerate(cells):
            m = re.search(r"\(([^)]+\.md)\)", c)
            if m:
                link, link_i = os.path.basename(m.group(1)), i
                break
        if link is None or link.lower() == "readme.md":
            continue
        rest = [c for i, c in enumerate(cells) if i != link_i]
        name = rest[0] if rest else ""
        code = next((c for c in rest if CODE_RE.match(c)), "")
        others = [c for c in rest[1:] if c != code]
        if len(others) >= 2:
            sector, oneliner = others[0], max(others[1:], key=len)
        elif others:
            sector, oneliner = (section or ""), others[0]
        else:
            sector, oneliner = (section or ""), ""
        meta[link] = {
            "name": name,
            "code": code.replace("(KQ)", " (KQ)"),
            "sector": sector or (section or "기타"),
            "oneliner": oneliner,
        }
        order.append(link)
    return meta, order


def build_day(date, fin_dir):
    readme_path = os.path.join(fin_dir, "README.md")
    readme = read(readme_path) if os.path.exists(readme_path) else ""
    meta, order = parse_index(readme)

    files = sorted(
        f for f in os.listdir(fin_dir)
        if f.endswith(".md") and f.lower() != "readme.md"
    )
    # README 표 순서를 우선하고, 표에 없는 파일은 뒤에 붙인다.
    ordered = [f for f in order if f in files]
    ordered += [f for f in files if f not in ordered]

    sectors = []
    by_sector = {}
    for fn in ordered:
        m = meta.get(fn)
        if m is None:
            stem = fn[:-3]
            name, _, code = stem.rpartition("_")
            m = {"name": name or stem, "code": code, "sector": "기타", "oneliner": ""}
        sec = m["sector"] or "기타"
        if sec not in by_sector:
            by_sector[sec] = {"name": sec, "items": []}
            sectors.append(by_sector[sec])
        by_sector[sec]["items"].append({
            "name": m["name"],
            "code": m["code"],
            "oneliner": m["oneliner"],
            "file": fn,
            "md": read(os.path.join(fin_dir, fn)),
        })

    journal_path = os.path.join(BASE, date + ".md")
    return {
        "date": date,
        "journal": read(journal_path) if os.path.exists(journal_path) else "",
        "overview": readme,
        "sectors": sectors,
        "count": len(ordered),
    }


def collect_days():
    days = []
    for entry in sorted(os.listdir(BASE), reverse=True):
        m = FIN_RE.match(entry)
        if not m:
            continue
        fin_dir = os.path.join(BASE, entry)
        if not os.path.isdir(fin_dir):
            continue
        days.append(build_day(m.group(1), fin_dir))
    return days


HTML = r"""<title>주식 스터디 리더</title>
<meta name="description" content="날짜별 스터디 저널과 종목 재무·투자 리포트를 섹터별로 넘겨보는 리서치 리더">
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Noto+Sans+KR:wght@400;500;700&family=Noto+Serif+KR:wght@500;600;700&family=JetBrains+Mono:wght@400;500&display=swap">
<style>
:root{
  --bg:#f6f7f9; --surface:#ffffff; --surface-2:#eef1f4;
  --ink:#1b2430; --muted:#63708a; --faint:#8b97ab;
  --line:#dfe4ea; --line-strong:#c7cfda;
  --accent:#1f6f5c; --accent-soft:#e2f0eb;
  --amber:#8a5d05; --amber-soft:#faf1dc; --amber-line:#e8cf8f;
  --shadow:0 1px 2px rgba(20,30,45,.05),0 8px 24px rgba(20,30,45,.06);
}
@media (prefers-color-scheme:dark){:root:not([data-theme="light"]){
  --bg:#0f1419; --surface:#161d27; --surface-2:#1d2632;
  --ink:#e4e8ee; --muted:#9aa7bb; --faint:#6c7a90;
  --line:#26313f; --line-strong:#33404f;
  --accent:#4fbfa0; --accent-soft:#12332b;
  --amber:#d7ab5a; --amber-soft:#2a2313; --amber-line:#4a3d1c;
  --shadow:0 1px 2px rgba(0,0,0,.3),0 10px 30px rgba(0,0,0,.35);
}}
:root[data-theme="dark"]{
  --bg:#0f1419; --surface:#161d27; --surface-2:#1d2632;
  --ink:#e4e8ee; --muted:#9aa7bb; --faint:#6c7a90;
  --line:#26313f; --line-strong:#33404f;
  --accent:#4fbfa0; --accent-soft:#12332b;
  --amber:#d7ab5a; --amber-soft:#2a2313; --amber-line:#4a3d1c;
  --shadow:0 1px 2px rgba(0,0,0,.3),0 10px 30px rgba(0,0,0,.35);
}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--ink);
  font-family:"Noto Sans KR",system-ui,-apple-system,sans-serif;
  font-size:15px;line-height:1.7;-webkit-font-smoothing:antialiased}
.mono{font-family:"JetBrains Mono",ui-monospace,monospace}

.app{display:grid;grid-template-columns:320px minmax(0,1fr);min-height:100vh}
aside{position:sticky;top:0;height:100vh;overflow-y:auto;
  background:var(--surface);border-right:1px solid var(--line);
  display:flex;flex-direction:column}
main{min-width:0}

.brand{padding:22px 22px 14px;border-bottom:1px solid var(--line)}
.brand h1{font-family:"Noto Serif KR",serif;font-weight:700;font-size:19px;
  margin:0;letter-spacing:-.01em}
.brand .sub{color:var(--muted);font-size:12px;margin-top:4px;letter-spacing:.02em}

.days{display:flex;flex-wrap:wrap;gap:6px;padding:12px 22px 4px}
.daybtn{border:1px solid var(--line-strong);background:var(--bg);color:var(--muted);
  border-radius:999px;padding:4px 11px;font:inherit;font-size:12px;cursor:pointer;
  font-family:"JetBrains Mono",monospace}
.daybtn:hover{border-color:var(--accent);color:var(--accent)}
.daybtn.active{background:var(--accent);border-color:var(--accent);color:#fff}
.daybtn .n{opacity:.7;margin-left:5px;font-size:11px}

.search{margin:10px 22px 6px}
.search input{width:100%;padding:9px 12px;border:1px solid var(--line-strong);
  border-radius:9px;background:var(--bg);color:var(--ink);font:inherit;font-size:13px}
.search input:focus{outline:2px solid var(--accent);outline-offset:1px;border-color:transparent}

nav{padding:8px 12px 24px;flex:1}
.navitem{display:block;width:100%;text-align:left;border:0;background:none;
  color:inherit;font:inherit;cursor:pointer;padding:9px 12px;border-radius:9px;
  display:flex;align-items:baseline;gap:10px}
.navitem:hover{background:var(--surface-2)}
.navitem.active{background:var(--accent-soft)}
.navitem.active .nm{color:var(--accent)}
.navitem .nm{font-weight:500;font-size:14px}
.navitem .code{margin-left:auto;font-size:11px;color:var(--faint)}
.pinned{margin-bottom:6px}
.pinned .navitem{border:1px solid var(--line);border-radius:10px;margin-bottom:6px}
.seclabel{font-size:11px;font-weight:700;letter-spacing:.08em;text-transform:uppercase;
  color:var(--faint);padding:16px 12px 6px}
.seccount{color:var(--faint);font-weight:400}

.reader{max-width:820px;margin:0 auto;padding:40px 44px 96px}
.crumb{display:flex;align-items:center;gap:10px;color:var(--muted);font-size:12px;
  margin-bottom:22px;letter-spacing:.02em;flex-wrap:wrap}
.crumb .dot{width:4px;height:4px;border-radius:50%;background:var(--faint)}
.doc{font-size:15px}
.doc h1{font-family:"Noto Serif KR",serif;font-weight:700;font-size:27px;line-height:1.3;
  margin:6px 0 18px;letter-spacing:-.015em;text-wrap:balance}
.doc h2{font-family:"Noto Serif KR",serif;font-weight:600;font-size:19px;
  margin:38px 0 12px;padding-bottom:7px;border-bottom:1px solid var(--line)}
.doc h3{font-weight:700;font-size:15px;margin:26px 0 8px;color:var(--ink)}
.doc p{margin:12px 0}
.doc ul,.doc ol{margin:12px 0;padding-left:22px}
.doc li{margin:5px 0}
.doc li::marker{color:var(--faint)}
.doc strong{font-weight:700}
.doc a{color:var(--accent);text-decoration:none;border-bottom:1px solid transparent;
  word-break:break-all}
.doc a:hover{border-bottom-color:var(--accent)}
.doc hr{border:0;border-top:1px solid var(--line);margin:30px 0}
.doc code{font-family:"JetBrains Mono",monospace;font-size:.85em;
  background:var(--surface-2);padding:2px 6px;border-radius:5px}
.doc blockquote{margin:16px 0;padding:2px 16px;border-left:3px solid var(--line-strong);
  color:var(--muted)}
.doc h1 code,.doc h2 code{background:none;padding:0}

.tablewrap{overflow-x:auto;margin:16px 0;border:1px solid var(--line);border-radius:10px}
.doc table{border-collapse:collapse;width:100%;font-size:13.5px;
  font-variant-numeric:tabular-nums}
.doc th,.doc td{padding:9px 14px;text-align:left;border-bottom:1px solid var(--line);
  white-space:nowrap}
.doc thead th{background:var(--surface-2);font-weight:700;font-size:12px;
  letter-spacing:.02em;position:sticky;top:0}
.doc tbody tr:last-child td{border-bottom:0}
.doc tbody tr:hover{background:var(--surface-2)}
.doc td:not(:first-child){font-family:"JetBrains Mono",monospace;font-size:12.5px}

.doc .callout{background:var(--amber-soft);border:1px solid var(--amber-line);
  border-radius:10px;padding:14px 18px;margin:18px 0;color:var(--amber)}

.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(220px,1fr));
  gap:12px;margin:20px 0}
.card{border:1px solid var(--line);border-radius:12px;padding:14px 16px;
  background:var(--surface);cursor:pointer;box-shadow:var(--shadow);
  display:flex;flex-direction:column;gap:6px;text-align:left;font:inherit;color:inherit}
.card:hover{border-color:var(--accent);transform:translateY(-1px);transition:.12s}
.card .top{display:flex;align-items:baseline;justify-content:space-between;gap:8px}
.card .nm{font-weight:700;font-size:15px}
.card .code{font-size:11px;color:var(--faint)}
.card .ol{font-size:12.5px;color:var(--muted);line-height:1.55}

.ttoggle{position:fixed;top:14px;right:16px;z-index:20;
  border:1px solid var(--line-strong);background:var(--surface);color:var(--muted);
  border-radius:8px;width:34px;height:34px;cursor:pointer;font-size:15px;
  display:flex;align-items:center;justify-content:center;box-shadow:var(--shadow)}
.ttoggle:hover{color:var(--accent);border-color:var(--accent)}

.menubtn{display:none}
@media(max-width:860px){
  .app{grid-template-columns:1fr}
  aside{position:fixed;z-index:15;width:300px;left:0;top:0;transform:translateX(-100%);
    transition:transform .2s ease;box-shadow:0 0 40px rgba(0,0,0,.25)}
  aside.open{transform:none}
  .menubtn{display:flex;position:fixed;top:14px;left:14px;z-index:16;width:40px;height:40px;
    align-items:center;justify-content:center;border:1px solid var(--line-strong);
    background:var(--surface);color:var(--ink);border-radius:9px;cursor:pointer;
    box-shadow:var(--shadow)}
  .scrim{display:none;position:fixed;inset:0;background:rgba(10,15,22,.4);z-index:14}
  .scrim.open{display:block}
  .reader{padding:64px 20px 80px}
}
@media(prefers-reduced-motion:reduce){*{transition:none!important}}
</style>

<button class="menubtn" id="menubtn" aria-label="목록 열기">☰</button>
<button class="ttoggle" id="ttoggle" aria-label="테마 전환">◐</button>
<div class="scrim" id="scrim"></div>
<div class="app">
  <aside id="aside">
    <div class="brand">
      <h1>주식 스터디</h1>
      <div class="sub mono" id="brandsub"></div>
    </div>
    <div class="days" id="days"></div>
    <div class="search"><input id="search" placeholder="종목·코드 검색…" autocomplete="off"></div>
    <nav id="nav"></nav>
  </aside>
  <main>
    <div class="reader">
      <div class="crumb" id="crumb"></div>
      <div class="doc" id="doc"></div>
    </div>
  </main>
</div>

<script id="data" type="application/json">__PAYLOAD__</script>
<script src="https://cdnjs.cloudflare.com/ajax/libs/marked/12.0.2/marked.min.js"></script>
<script>
const D = JSON.parse(document.getElementById('data').textContent);
marked.setOptions({gfm:true, breaks:false});
const DAYS = D.days;
const byDate = {}; DAYS.forEach(d=>byDate[d.date]=d);
let cur = DAYS[0];

const nav=document.getElementById('nav');
const doc=document.getElementById('doc');
const crumb=document.getElementById('crumb');
const brandsub=document.getElementById('brandsub');
const daysBar=document.getElementById('days');

// ---- 날짜 탭 ----
DAYS.forEach(d=>{
  const b=document.createElement('button');
  b.className='daybtn'; b.dataset.date=d.date;
  b.innerHTML='<span></span><span class="n"></span>';
  b.children[0].textContent=d.date.slice(5);
  b.children[1].textContent=d.count+'종목';
  b.title=d.date;
  b.onclick=()=>{location.hash=d.date+'/home';};
  daysBar.appendChild(b);
});

function navItem(label, code, key){
  const b=document.createElement('button');
  b.className='navitem'; b.dataset.key=key;
  b.innerHTML='<span class="nm"></span>'+(code?'<span class="code mono"></span>':'');
  b.querySelector('.nm').textContent=label;
  if(code) b.querySelector('.code').textContent=code;
  b.onclick=()=>{location.hash=key; closeDrawer();};
  return b;
}

function buildNav(day){
  nav.innerHTML='';
  const pinned=document.createElement('div'); pinned.className='pinned';
  pinned.appendChild(navItem('개요 · '+day.count+'종목 인덱스','',day.date+'/home'));
  if(day.journal) pinned.appendChild(navItem('그날의 스터디 저널','',day.date+'/journal'));
  nav.appendChild(pinned);
  day.sectors.forEach(s=>{
    const lab=document.createElement('div'); lab.className='seclabel';
    lab.innerHTML=s.name+' <span class="seccount">'+s.items.length+'</span>';
    nav.appendChild(lab);
    s.items.forEach(it=>nav.appendChild(navItem(it.name,it.code,day.date+'/'+it.file)));
  });
  applyFilter();
}

function enhance(container, day){
  container.querySelectorAll('table').forEach(t=>{
    const w=document.createElement('div'); w.className='tablewrap';
    t.parentNode.insertBefore(w,t); w.appendChild(t);
  });
  container.querySelectorAll('p').forEach(p=>{
    if(p.textContent.includes('⚠️')) p.classList.add('callout');
  });
  const files={}; day.sectors.forEach(s=>s.items.forEach(it=>files[it.file]=true));
  container.querySelectorAll('a[href$=".md"]').forEach(a=>{
    const href=a.getAttribute('href').split('/').pop();
    if(href===day.date+'.md'){a.onclick=e=>{e.preventDefault();location.hash=day.date+'/journal';};return;}
    if(files[href]){a.onclick=e=>{e.preventDefault();location.hash=day.date+'/'+href;};}
  });
}

function setCrumb(parts){
  crumb.innerHTML=parts.filter(Boolean).map(p=>'<span></span>').join('<span class="dot"></span>');
  const spans=crumb.querySelectorAll('span:not(.dot)');
  parts.filter(Boolean).forEach((p,i)=>{spans[i].textContent=p;});
}

function homeView(day){
  setCrumb(['주식 스터디', day.date, '개요']);
  doc.innerHTML=marked.parse(day.overview||('# '+day.date+' 스터디'));
  enhance(doc, day);
  const grid=document.createElement('div'); grid.className='grid';
  day.sectors.forEach(s=>s.items.forEach(it=>{
    const c=document.createElement('button'); c.className='card';
    c.innerHTML='<div class="top"><span class="nm"></span><span class="code mono"></span></div><div class="ol"></div>';
    c.querySelector('.nm').textContent=it.name;
    c.querySelector('.code').textContent=it.code;
    c.querySelector('.ol').textContent=it.oneliner;
    c.onclick=()=>location.hash=day.date+'/'+it.file;
    grid.appendChild(c);
  }));
  const h=document.createElement('h2'); h.textContent='전체 종목 바로가기';
  doc.appendChild(h); doc.appendChild(grid);
}

function render(){
  let key=decodeURIComponent(location.hash.slice(1));
  let date=cur.date, view='home';
  const slash=key.indexOf('/');
  if(slash>0 && byDate[key.slice(0,slash)]){date=key.slice(0,slash); view=key.slice(slash+1)||'home';}
  else if(byDate[key]){date=key;}
  const day=byDate[date];
  if(day!==cur){cur=day; buildNav(day);}
  brandsub.textContent=day.date+' · 리포트 '+day.count+'종목 · 전체 '+DAYS.length+'일';
  daysBar.querySelectorAll('.daybtn').forEach(b=>b.classList.toggle('active',b.dataset.date===date));
  const fullKey=date+'/'+view;
  nav.querySelectorAll('.navitem').forEach(n=>n.classList.toggle('active',n.dataset.key===fullKey));
  window.scrollTo(0,0);
  if(view==='journal' && day.journal){
    setCrumb(['주식 스터디', day.date, '저널']);
    doc.innerHTML=marked.parse(day.journal); enhance(doc, day); return;
  }
  let found=null, sec='';
  day.sectors.forEach(s=>s.items.forEach(it=>{if(it.file===view){found=it; sec=s.name;}}));
  if(found){
    setCrumb(['주식 스터디', day.date, sec, found.name]);
    doc.innerHTML=marked.parse(found.md); enhance(doc, day); return;
  }
  homeView(day);
}

// ---- 검색 ----
const search=document.getElementById('search');
function applyFilter(){
  const q=search.value.trim().toLowerCase();
  nav.querySelectorAll('.navitem').forEach(n=>{
    if(n.parentNode.classList.contains('pinned')) return;
    const nm=n.querySelector('.nm').textContent.toLowerCase();
    const cd=(n.querySelector('.code')?n.querySelector('.code').textContent:'').toLowerCase();
    n.style.display=(!q||nm.includes(q)||cd.includes(q))?'':'none';
  });
  nav.querySelectorAll('.seclabel').forEach(lab=>{
    let el=lab.nextElementSibling, any=false;
    while(el&&el.classList.contains('navitem')){if(el.style.display!=='none')any=true;el=el.nextElementSibling;}
    lab.style.display=any?'':'none';
  });
}
search.addEventListener('input',applyFilter);

// ---- 테마 ----
const root=document.documentElement, tt=document.getElementById('ttoggle');
try{const sv=localStorage.getItem('study-theme'); if(sv)root.setAttribute('data-theme',sv);}catch(e){}
tt.onclick=()=>{
  const c=root.getAttribute('data-theme');
  const isDark = c ? c==='dark' : matchMedia('(prefers-color-scheme:dark)').matches;
  const next=isDark?'light':'dark';
  root.setAttribute('data-theme',next);
  try{localStorage.setItem('study-theme',next);}catch(e){}
};

// ---- 모바일 서랍 ----
const aside=document.getElementById('aside'),scrim=document.getElementById('scrim');
function closeDrawer(){aside.classList.remove('open');scrim.classList.remove('open');}
document.getElementById('menubtn').onclick=()=>{aside.classList.toggle('open');scrim.classList.toggle('open');};
scrim.onclick=closeDrawer;

buildNav(cur);
window.addEventListener('hashchange',render);
render();
</script>
"""


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_OUT
    days = collect_days()
    if not days:
        raise SystemExit("스터디 폴더를 찾지 못했습니다: " + BASE)
    payload = json.dumps({"days": days}, ensure_ascii=False).replace("<", "\\u003c")
    html = HTML.replace("__PAYLOAD__", payload)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="utf-8") as f:
        f.write(html)
    print("wrote", out, len(html), "bytes")
    for d in days:
        print(" ", d["date"], d["count"], "종목",
              "/ 섹터", len(d["sectors"]),
              "/ 저널", "O" if d["journal"] else "X")


if __name__ == "__main__":
    main()
