#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""유지관리 진입점. 생성기·검사기를 순서대로 부르고 rc를 남긴다.

  --daily   rotate_logs → gen_facts --apply → gen_code_graph --json → sync_ledgers → gen_automation_hub → gen_tuning_sheet. 대시보드는 부르지 않는다.
  --rotate-logs [--dry-run]  로그 정리만. 엔진 로그에서 7일 지난 날의 줄을 날짜별 gz로 떼어내고, 감시견 로그는 7일 지나면 gz·
            90일 지나면 삭제. 엔진이 떠 있으면 엔진 로그는 건너뛴다. 원장 trades_*.csv는 손대지 않는다(여러 날을 함께 읽는 스크립트가 있다).
  --check   check_docs → check_code_refs --diff-only → gen_facts --check → gen_code_graph --check → sync_impact --stamps.
            보고만 한다. 자동 수정·스테이징 없음. 하나라도 실패면 exit 1.
  --weekly  미참조 스크립트·에이전트 죽은 경로·부산물 용량·주석 밀도·훅 배선·.claude 해시 매니페스트
            → docs/reports/MAINTENANCE_WEEKLY.md. .claude/는 읽기만 한다.

단계마다 subprocess.run으로 격리하고 한 단계가 실패해도 다음을 막지 않는다.
로그는 logs/maintenance.log(QUANT_LOG_DIR가 있으면 그 아래).
"""
from __future__ import annotations

import datetime as dt
import gzip
import hashlib
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LOG_DIR = Path(os.environ.get("QUANT_LOG_DIR") or (ROOT / "logs"))
RUN_LOG = LOG_DIR / "maintenance.log"
MANIFEST = LOG_DIR / "claude_manifest.json"
WEEKLY_MD = ROOT / "docs" / "reports" / "MAINTENANCE_WEEKLY.md"
PY = sys.executable
STEP_TIMEOUT = 600

SKIP_DIRS = {"__pycache__", "node_modules", ".git", "logs", "out", "_private"}

# 로그 정리(T-13-10). 엔진 로그는 한 파일에 계속 덧붙여 09-19 기준 81MB(08-07~)라 오늘 줄을 찾는 스크립트마다 전부 읽었다.
ROTATE_KEEP_DAYS = 7      # 이 일수 안의 줄·파일은 그대로 둔다(대시보드·사후검토·감시 세션이 최근 며칠을 읽는다)
ROTATE_DELETE_DAYS = 90   # gz로 묶인 감시견 로그를 지우는 기준. 엔진 로그 gz는 지우지 않는다(리플레이·연구 입력)
ENGINE_LOG_NAME = "quant_trader.log"
WATCHDOG_LOG_PATTERNS = ("auto_trade_day_*.log", "auto_trade_guard_*.log")
COMMENT_TAGS = ("[inv]", "[lock-order]", "[wire]", "[why", "[formula]")
UNTAGGED_BLOCK_MIN = 4

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass


def rel(p: Path) -> str:
    return p.relative_to(ROOT).as_posix()


def log(line: str) -> None:
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    stamp = dt.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    with RUN_LOG.open("a", encoding="utf-8") as f:
        f.write(f"{stamp} {line}\n")


def run_step(name: str, cmd: list[str]) -> tuple[int, float, str]:
    """한 단계를 격리 실행. (rc, 초, 출력 꼬리)."""
    t0 = time.time()
    try:
        r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=STEP_TIMEOUT)
        rc, out = r.returncode, (r.stdout or "") + (r.stderr or "")
    except subprocess.TimeoutExpired:
        rc, out = 124, f"timeout {STEP_TIMEOUT}s"
    except OSError as e:
        rc, out = 127, str(e)
    sec = time.time() - t0
    tail = "\n".join(out.strip().splitlines()[-12:])
    log(f"[{name}] rc={rc} {sec:.1f}s")
    print(f"--- {name}: rc={rc} ({sec:.1f}s)")
    if tail:
        print(tail)
    return rc, sec, tail


# ----------------------------- 로그 정리 -----------------------------

def _engine_running() -> bool:
    """quant_trader.exe가 떠 있는지. 엔진은 로그를 append로 열어 두므로 그때 잘라내면 파일 위치가 어긋난다."""
    if os.name != "nt":
        return False
    try:
        r = subprocess.run(["tasklist", "/FI", "IMAGENAME eq quant_trader.exe", "/NH"],
                           capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=30)
    except (OSError, subprocess.TimeoutExpired):
        return True  # 모르면 떠 있는 것으로 보고 손대지 않는다
    return "quant_trader.exe" in (r.stdout or "")


def _engine_log_dir() -> Path:
    sys.path.insert(0, str(ROOT / "scripts"))
    import _logdir  # noqa: E402  (QUANT_LOG_DIR > 가장 최근 quant_trader.log를 가진 폴더)
    return _logdir.log_dir()


def _line_day(line: str) -> str | None:
    """줄 머리 'YYYY-MM-DD'. 없으면 None(스택·이어지는 줄은 직전 줄의 날짜를 따른다)."""
    head = line[:10]
    if len(head) == 10 and head[4] == "-" and head[7] == "-" and head[:4].isdigit():
        return head
    return None


def rotate_engine_log(log_path: Path, cutoff: dt.date, dry_run: bool = False) -> dict:
    """엔진 로그에서 cutoff 이전 날짜의 줄을 archive/quant_trader_YYYY-MM-DD.log.gz로 옮기고 나머지만 남긴다.
    gz는 append로 열어 같은 날이 두 번 와도 이어 붙는다(gzip은 멤버를 이어 붙여도 한 스트림으로 읽힌다)."""
    result = {"moved_lines": 0, "kept_lines": 0, "days": []}
    if not log_path.exists():
        return result
    archive_dir = log_path.parent / "archive"
    keep_path = log_path.with_name(log_path.name + ".keep")
    cutoff_text = cutoff.isoformat()
    current_day: str | None = None
    current_out = None
    moved_days: list[str] = []
    keep_file = None
    try:
        if not dry_run:
            archive_dir.mkdir(exist_ok=True)
            keep_file = keep_path.open("w", encoding="utf-8", errors="replace", newline="")
        with log_path.open("r", encoding="utf-8", errors="replace", newline="") as source:
            for line in source:
                day = _line_day(line)
                if day is not None:
                    current_day = day
                if current_day is None or current_day >= cutoff_text:
                    result["kept_lines"] += 1
                    if keep_file is not None:
                        keep_file.write(line)
                    continue
                result["moved_lines"] += 1
                if not moved_days or moved_days[-1] != current_day:
                    moved_days.append(current_day)
                    if current_out is not None:
                        current_out.close()
                    current_out = None if dry_run else gzip.open(archive_dir / f"quant_trader_{current_day}.log.gz", "ab")
                if current_out is not None:
                    current_out.write(line.encode("utf-8", errors="replace"))
    finally:
        if current_out is not None:
            current_out.close()
        if keep_file is not None:
            keep_file.close()
    result["days"] = sorted(set(moved_days))
    if dry_run:
        return result
    if result["moved_lines"] == 0:
        keep_path.unlink(missing_ok=True)
        return result
    os.replace(keep_path, log_path)  # 같은 볼륨이라 원자적. 엔진은 다음 기동 때 이 파일에 이어 쓴다
    return result


def rotate_watchdog_logs(directory: Path, today: dt.date, dry_run: bool = False) -> dict:
    """logs/auto_trade_day_YYYYMMDD.log 류 — KEEP일 지나면 gz, DELETE일 지나면 gz 삭제."""
    result = {"gzipped": [], "deleted": []}
    for pattern in WATCHDOG_LOG_PATTERNS:
        for path in sorted(directory.glob(pattern)) + sorted(directory.glob(pattern + ".gz")):
            match = re.search(r"(\d{8})", path.name)
            if not match:
                continue
            try:
                day = dt.datetime.strptime(match.group(1), "%Y%m%d").date()
            except ValueError:
                continue
            age = (today - day).days
            if path.suffix == ".gz":
                if age > ROTATE_DELETE_DAYS:
                    result["deleted"].append(path.name)
                    if not dry_run:
                        path.unlink()
            elif age > ROTATE_KEEP_DAYS:
                result["gzipped"].append(path.name)
                if not dry_run:
                    with path.open("rb") as source, gzip.open(path.with_name(path.name + ".gz"), "wb") as target:
                        target.writelines(source)
                    path.unlink()
    return result


def rotate_logs(dry_run: bool = False) -> int:
    today = dt.date.today()
    cutoff = today - dt.timedelta(days=ROTATE_KEEP_DAYS)
    tag = " (dry)" if dry_run else ""
    engine_log = _engine_log_dir() / ENGINE_LOG_NAME
    if _engine_running():
        log(f"[rotate_logs] 엔진 실행 중 — {engine_log} 건너뜀")
        print("--- rotate_logs: 엔진 실행 중, 엔진 로그 건너뜀")
    else:
        moved = rotate_engine_log(engine_log, cutoff, dry_run)
        line = f"엔진 로그 {moved['moved_lines']}줄을 {len(moved['days'])}일치 gz로, {moved['kept_lines']}줄 남김{tag}"
        log(f"[rotate_logs] {line}")
        print(f"--- rotate_logs: {line}")
    watchdog = rotate_watchdog_logs(ROOT / "logs", today, dry_run)
    log(f"[rotate_logs] 감시견 로그 gz {len(watchdog['gzipped'])}개, 삭제 {len(watchdog['deleted'])}개{tag}")
    print(f"--- rotate_logs: 감시견 로그 gz {watchdog['gzipped']}, 삭제 {watchdog['deleted']}{tag}")
    return 0


# ----------------------------- --daily / --check -----------------------------

def daily() -> int:
    log("daily start")
    try:
        rotate_logs()
    except OSError as error:  # 로그 정리가 실패해도 문서 생성은 돈다
        log(f"[rotate_logs] 실패 {error}")
        print(f"--- rotate_logs: 실패 {error}")
    steps = [
        ("gen_facts --apply", [PY, "scripts/gen_facts.py", "--apply"]),
        ("gen_code_graph --json", [PY, "scripts/gen_code_graph.py", "--json"]),
    ]
    if (ROOT / "scripts" / "sync_ledgers.py").exists():
        steps.append(("sync_ledgers", [PY, "scripts/sync_ledgers.py"]))
    steps.append(("gen_automation_hub", [PY, "scripts/gen_automation_hub.py"]))  # _private/AUTOMATION_HUB.md — 예약작업 실제 상태를 날마다 새로 읽는다
    steps.append(("gen_tuning_sheet", [PY, "scripts/gen_tuning_sheet.py"]))  # _private/TUNING_SHEET.md — 장중 매매 수치·주기 한 장
    worst = 0
    for name, cmd in steps:
        rc, _, _ = run_step(name, cmd)
        worst = worst or rc
    log(f"daily end rc={worst}")
    return 1 if worst else 0


def check() -> int:
    log("check start")
    steps = [
        ("check_docs", [PY, "scripts/check_docs.py"]),
        ("check_code_refs --diff-only", [PY, "scripts/check_code_refs.py", "--diff-only"]),
        ("gen_facts --check", [PY, "scripts/gen_facts.py", "--check"]),
        ("gen_code_graph --check", [PY, "scripts/gen_code_graph.py", "--check"]),
        ("sync_impact --stamps", [PY, "scripts/sync_impact.py", "--stamps"]),
    ]
    rows = []
    for name, cmd in steps:
        if not (ROOT / cmd[1]).exists():
            rows.append((name, "-", "없음"))
            continue
        rc, sec, _ = run_step(name, cmd)
        rows.append((name, str(rc), f"{sec:.1f}s"))
    print("\n| 검사기 | rc | 소요 |\n|---|---|---|")
    for name, rc, sec in rows:
        print(f"| {name} | {rc} | {sec} |")
    failed = any(rc not in ("0", "-") for _, rc, _ in rows)
    log(f"check end failed={failed}")
    return 1 if failed else 0


# ----------------------------- --weekly -----------------------------

def _walk(base: Path, exts: tuple[str, ...], scripts_top_only: bool = False):
    if not base.exists():
        return
    for dirpath, dirs, names in os.walk(base):
        dirs[:] = sorted(d for d in dirs if d not in SKIP_DIRS and not d.startswith((".", "build_")))
        if scripts_top_only and Path(dirpath) != base:
            continue
        for n in sorted(names):
            if n.endswith(exts):
                yield Path(dirpath) / n


def _read(p: Path) -> str:
    try:
        return p.read_text(encoding="utf-8-sig", errors="replace")
    except OSError:
        return ""


def _tracked_text_files() -> list[Path]:
    r = subprocess.run(["git", "-C", str(ROOT), "ls-files"], capture_output=True,
                       text=True, encoding="utf-8", errors="replace")
    exts = (".py", ".md", ".ps1", ".json", ".sh", ".bat", ".cmd", ".txt", ".yml", ".yaml", ".toml", ".cfg")
    return [ROOT / ln for ln in r.stdout.splitlines() if ln.endswith(exts)]


def _schtasks_commands() -> str:
    """등록된 예약작업의 실행 명령 문자열. 실패하면 빈 문자열."""
    try:
        r = subprocess.run(["schtasks", "/query", "/fo", "csv", "/v"], capture_output=True,
                           text=True, encoding="cp949", errors="replace", timeout=30)
        return r.stdout
    except (OSError, subprocess.TimeoutExpired):
        return ""


def unreferenced_scripts() -> list[str]:
    cands = list(_walk(ROOT / "scripts", (".py",), scripts_top_only=True))
    cands += list((ROOT / "PYQuant" / "tools").glob("*.py"))
    corpus_files = _tracked_text_files()
    corpus_files += list(_walk(ROOT / ".claude", (".md", ".json", ".ps1", ".py")))
    corpus_files += list((ROOT / "_private").glob("*.ps1")) + list((ROOT / "_private").glob("*.md"))
    texts = {p: _read(p) for p in corpus_files}
    sched = _schtasks_commands()
    out = []
    for c in cands:
        name = c.name
        stem = c.stem
        pat = re.compile(r"(?<![\w-])" + re.escape(name))
        mod_pat = re.compile(r"(?:import|from|-m)\s+(?:[\w.]+\.)?" + re.escape(stem) + r"\b")
        hit = pat.search(sched) is not None
        for p, t in texts.items():
            if p == c:
                continue
            if pat.search(t) or mod_pat.search(t):
                hit = True
                break
        if not hit:
            out.append(rel(c))
    return out


PATH_TOKEN = re.compile(r"(?<![\w/.-])((?:Quant|PYQuant|scripts|docs|research|strategies|_private|\.claude)/[\w./+*{}<>-]+)")
PLACEHOLDER_HINTS = ("*", "{", "}", "<", ">", "YYYY", "NN")
LINK_RE = re.compile(r"\[[^\]]*\]\(\s*(<[^>]+>|[^)\s]+)")


def dead_paths_in_claude() -> list[tuple[str, int, str]]:
    out = []
    for sub in ("agents", "commands"):
        for md in sorted((ROOT / ".claude" / sub).glob("*.md")):
            in_fence = False
            for no, ln in enumerate(_read(md).splitlines(), 1):
                if ln.strip().startswith("```"):
                    in_fence = not in_fence
                    continue
                if in_fence:
                    continue
                refs = []
                for m in re.finditer(r"`([^`]+)`", ln):
                    refs += [x.rstrip(".,;:") for x in PATH_TOKEN.findall(m.group(1))]
                for m in LINK_RE.finditer(ln):
                    t = m.group(1).strip("<>").split("#")[0]
                    if t and not t.startswith(("http", "mailto", "#")):
                        refs.append(t)
                for r in refs:
                    if any(h in r for h in PLACEHOLDER_HINTS):
                        continue
                    p = ROOT / r if not r.startswith(".") else (md.parent / r)
                    if not p.exists() and not (ROOT / r).exists():
                        out.append((rel(md), no, r))
    return out


def mask_private(r: str) -> str:
    """`_private/` 하위 이름은 리포트에 옮기지 않는다(git에 두지 않는 이름 노출 차단)."""
    return "_private/…" if r.startswith("_private/") else r


def dir_size(p: Path) -> int:
    total = 0
    if not p.exists():
        return 0
    for dirpath, _, names in os.walk(p):
        for n in names:
            try:
                total += (Path(dirpath) / n).stat().st_size
            except OSError:
                pass
    return total


def artifact_sizes() -> list[tuple[str, int]]:
    targets = [ROOT / "logs", ROOT / "research" / "dashboard"]
    targets += sorted(ROOT.glob("Quant/build_*"))
    return [(rel(p), dir_size(p)) for p in targets if p.exists()]


def comment_density() -> tuple[list[tuple[str, int, int]], list[tuple[str, int, int]]]:
    """(파일, 주석줄, 전체줄) 목록과 태그 없는 4줄 이상 연속 주석 블록 (파일, 시작줄, 길이)."""
    files = []
    for base in (ROOT / "Quant" / "include", ROOT / "Quant" / "src"):
        files += list(_walk(base, (".h", ".hpp", ".cpp", ".cc")))
    files += list(_walk(ROOT / "PYQuant", (".py",)))
    files += list(_walk(ROOT / "scripts", (".py",), scripts_top_only=True))
    density, blocks = [], []
    for f in files:
        lines = _read(f).splitlines()
        is_py = f.suffix == ".py"
        n_comment = 0
        run_start, run_len, run_tagged = -1, 0, False
        in_block = False

        def flush():
            if run_len >= UNTAGGED_BLOCK_MIN and not run_tagged:
                blocks.append((rel(f), run_start + 1, run_len))

        for i, ln in enumerate(lines):
            s = ln.strip()
            if is_py:
                is_c = s.startswith("#")
            else:
                if in_block:
                    is_c = True
                    if "*/" in s:
                        in_block = False
                elif s.startswith("/*"):
                    is_c = True
                    in_block = "*/" not in s
                else:
                    is_c = s.startswith("//")
            if is_c:
                n_comment += 1
                if run_len == 0:
                    run_start, run_tagged = i, False
                run_len += 1
                if any(t in s for t in COMMENT_TAGS):
                    run_tagged = True
            else:
                flush()
                run_len = 0
        flush()
        if lines:
            density.append((rel(f), n_comment, len(lines)))
    density.sort(key=lambda t: (-(t[1] / t[2] if t[2] else 0), t[0]))
    return density, blocks


def hook_wiring() -> dict:
    hooks_dir = ROOT / ".claude" / "hooks"
    files = sorted(p.name for p in hooks_dir.glob("*.ps1")) if hooks_dir.exists() else []
    settings = ROOT / ".claude" / "settings.json"
    wired, bad_paths = [], []
    try:
        data = json.loads(_read(settings))
    except json.JSONDecodeError:
        data = {}
    for event, groups in (data.get("hooks") or {}).items():
        for grp in groups:
            for h in grp.get("hooks", []):
                cmd = h.get("command", "")
                m = re.search(r"((?:\.claude[\\/])?hooks[\\/][\w.-]+\.ps1)", cmd)
                if not m:
                    continue
                rp = m.group(1).replace("\\", "/")
                if not rp.startswith(".claude/"):
                    rp = ".claude/" + rp
                wired.append((event, rp))
                if not (ROOT / rp).exists():
                    bad_paths.append((event, rp))
    wired_names = {Path(rp).name for _, rp in wired}
    unwired = [f for f in files if f not in wired_names]
    bom = []
    for f in files:
        raw = (hooks_dir / f).read_bytes()[:3]
        bom.append((f, raw == b"\xef\xbb\xbf"))
    settings_bom = settings.exists() and settings.read_bytes()[:3] == b"\xef\xbb\xbf"
    return {"files": files, "wired": wired, "unwired": unwired, "bad_paths": bad_paths,
            "bom": bom, "settings_bom": settings_bom}


def claude_manifest() -> tuple[dict, dict]:
    """현재 .claude/ 해시와 전주 대비 변경. 매니페스트는 logs/에 둔다."""
    cur = {}
    for f in _walk(ROOT / ".claude", ("",)):
        if f.suffix in (".pyc",):
            continue
        try:
            cur[rel(f)] = hashlib.sha256(f.read_bytes()).hexdigest()[:16]
        except OSError:
            pass
    prev = {}
    if MANIFEST.exists():
        try:
            prev = json.loads(_read(MANIFEST)).get("files", {})
        except json.JSONDecodeError:
            prev = {}
    diff = {
        "added": sorted(k for k in cur if k not in prev),
        "removed": sorted(k for k in prev if k not in cur),
        "changed": sorted(k for k in cur if k in prev and prev[k] != cur[k]),
        "prev_generated": None,
    }
    if MANIFEST.exists():
        try:
            diff["prev_generated"] = json.loads(_read(MANIFEST)).get("generated")
        except json.JSONDecodeError:
            pass
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    MANIFEST.write_text(json.dumps({"generated": dt.date.today().isoformat(), "files": cur},
                                   ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
    return cur, diff


def _mb(n: int) -> str:
    return f"{n / 1_048_576:.1f} MB"


def weekly() -> int:
    log("weekly start")
    today = dt.date.today().isoformat()
    red = 0
    md = [f"<!-- drift-check: snapshot {today} -->", "", f"# 주간 유지관리 리포트 — {today}", "",
          "`py scripts/maintain.py --weekly`가 만든 스냅샷. `.claude/`는 읽기만 했다. 고칠 항목은 사람이 승인한다.", ""]

    unref = unreferenced_scripts()
    md += ["## 1. 미참조 스크립트", "",
           "`scripts/*.py`·`PYQuant/tools/*.py` 중 다른 파일·`.claude/`·`docs/`·예약작업 어디에서도 이름이 안 나오는 것.", ""]
    md += [f"- [!] `{x}`" for x in unref] or ["- 없음"]
    red += len(unref)

    dead = dead_paths_in_claude()
    md += ["", "## 2. 에이전트·커맨드의 죽은 경로", ""]
    #  _private/ 아래 이름은 git에 남기지 않기로 한 것이다. 이 리포트는 추적 대상이므로
    #  파일:줄만 남기고 대상 경로는 접두어까지만 적는다(고칠 위치는 그대로 짚힌다).
    md += [f"- [!] `{f}:{n}` → `{mask_private(r)}`" for f, n, r in dead] or ["- 없음"]
    red += len(dead)

    md += ["", "## 3. 부산물 용량", "", "| 폴더 | 크기 |", "|---|---|"]
    for name, size in artifact_sizes():
        md.append(f"| `{name}` | {_mb(size)} |")

    density, blocks = comment_density()
    md += ["", "## 4. 주석 밀도", "", "게이트가 아니다. 파일별 주석줄/전체줄과, 태그 없는 4줄 이상 연속 주석 블록만 남긴다.", "",
           "| 파일 | 주석줄 | 전체줄 | 비율 |", "|---|---|---|---|"]
    for f, c, n in density:
        md.append(f"| `{f}` | {c} | {n} | {c / n * 100:.0f}% |")
    md += ["", f"태그 없는 연속 주석 블록({UNTAGGED_BLOCK_MIN}줄 이상): {len(blocks)}개", "",
           "| 파일 | 시작줄 | 길이 |", "|---|---|---|"]
    md += [f"| `{f}` | {s} | {l} |" for f, s, l in blocks[:200]]
    if len(blocks) > 200:
        md.append(f"| (이하 {len(blocks) - 200}개 생략) | | |")

    hw = hook_wiring()
    md += ["", "## 5. settings.json 훅 배선", "", "| 이벤트 | 훅 경로 | 실재 |", "|---|---|---|"]
    for ev, rp in hw["wired"]:
        ok = (ev, rp) not in hw["bad_paths"]
        md.append(f"| `{ev}` | `{rp}` | {'있음' if ok else '[!] 없음'} |")
    md += ["", "| 훅 파일 | 배선 | BOM UTF-8 |", "|---|---|---|"]
    for f, has_bom in hw["bom"]:
        wired = f not in hw["unwired"]
        md.append(f"| `{f}` | {'됨' if wired else '[!] 안 됨'} | {'예' if has_bom else '[!] 아니오'} |")
    md.append(f"\n`settings.json` BOM: {'예' if hw['settings_bom'] else '아니오'}")
    red += len(hw["unwired"]) + len(hw["bad_paths"]) + sum(1 for _, b in hw["bom"] if not b)

    cur, diff = claude_manifest()
    md += ["", "## 6. `.claude/` 변경(해시 매니페스트)", "",
           f"파일 {len(cur)}개. 매니페스트는 `logs/claude_manifest.json`. 이전 생성일: {diff['prev_generated'] or '없음(첫 실행)'}", ""]
    for key, label in (("added", "추가"), ("removed", "삭제"), ("changed", "변경")):
        md.append(f"- {label} {len(diff[key])}개" + (": " + ", ".join(f"`{x}`" for x in diff[key][:40]) if diff[key] else ""))

    md[4:4] = [f"빨간 항목: {red}", ""]
    WEEKLY_MD.parent.mkdir(parents=True, exist_ok=True)
    WEEKLY_MD.write_text("\n".join(md) + "\n", encoding="utf-8")
    print(f"[ok] wrote {rel(WEEKLY_MD)} (red={red}, unref={len(unref)}, dead={len(dead)}, blocks={len(blocks)})")
    log(f"weekly end red={red}")
    return 0


def main(argv: list[str]) -> int:
    if "--daily" in argv:
        return daily()
    if "--rotate-logs" in argv:
        return rotate_logs(dry_run="--dry-run" in argv)
    if "--check" in argv:
        return check()
    if "--weekly" in argv:
        return weekly()
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
