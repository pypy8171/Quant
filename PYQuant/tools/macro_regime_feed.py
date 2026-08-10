"""
매크로 레짐 사이드카 — risk-on/off 게이트 프로듀서 (2026-08-09 전략회의 Task 3).

목적:
  환율·미국채금리·나스닥선물·VIX 는 서로 상관 0.6~0.9인 "베타(시장 전체 방향)"라
  종목 간 우열을 못 가린다 → 종목 스코어 항이 아니라 **단일 국면 스위치**로 묶는다.
  이 스크립트가 그 스위치를 계산해 regime.json 한 파일로 원자적 발행하면,
  C++ 엔진 데이터 스레드가 폴링 주기마다 읽어 OrderGate::set_entry_halt()를 토글한다.

전송(MVP): ZMQ 대신 **원자적 파일 브리지**(tmp+os.replace).
  이유 — 지금 C++(WinHTTP-only·무의존 빌드)에 libzmq를 새로 얹으면 빌드 리스크.
  파이프라인이 결정론적으로 검증된 뒤 ZMQ PUB/SUB로 업그레이드(목표 아키텍처).
  regime.json 스키마(=C++ 리더 계약)는 아래 build_regime() 반환부 주석 참조.

데이터(무료 시작): yfinance 선물 심볼.
  ⚠️ **현물지수(^GSPC 등)는 한국 장중 죽어있다 → 반드시 선물 심볼 사용**:
    ES=F(S&P500 선물) NQ=F(나스닥100 선물) ^VIX / KRW=X(USDKRW).
    채권은 ^TNX(10Y 국채금리, %)로 표시(가격 선물 ZN=F 대신 — 읽기 직관 우선).
  OANDA v20 FX 스트림은 --oanda-token 주면 USD/KRW 실시간으로 대체(옵션, 기본 off).

⚠️ 임계값은 전부 **검증 필요 가정**(STRATEGIES.md 회의 §검증 필요 가정 3).
   과최적화·국면 표본 부족 위험 — 라이브로 관찰하며 보정할 것.

사용 (PYQuant/ 디렉토리에서):
    python -m tools.macro_regime_feed                       # 3분 주기 무한 발행
    python -m tools.macro_regime_feed --once                # 1회 계산 후 종료(점검용)
    python -m tools.macro_regime_feed --interval 180 --out ../Quant/config/regime.json
주문 없음 — 시세 조회·파일 쓰기 전용, 안전.
"""
import argparse
import json
import os
import sys
import time
from datetime import datetime, timezone, timedelta
from pathlib import Path

KST = timezone(timedelta(hours=9))

# ── 심볼 정의 (yfinance) — 현물 아님, 선물/환율 ─────────────────────────────
#  vote_dir: 이 지표가 "오르면" 위험선호(+1)인지 위험회피(-1)인지.
#    NQ=F↑ → risk-on(+1). VIX↑ → risk-off(지표값↑이 위험이므로 -1).
#    ^TNX(10Y 금리)↑ → 금리상승 → risk-off(-1). KRW=X(USDKRW)↑ → 원화약세 → risk-off(-1).
#    ※ 채권은 "선물가격(ZN=F)" 대신 "금리(^TNX, %)"로 표시 — 읽기 직관 우선.
#      가격↑=금리↓라 방향이 정반대이므로 vote_dir 는 -1(선물가격이면 +1이었음).
SYMBOLS = {
    "NQ_F":  {"yf": "NQ=F",  "vote_dir": +1, "label": "나스닥100 선물"},
    "ES_F":  {"yf": "ES=F",  "vote_dir": +1, "label": "S&P500 선물"},
    "TNX10": {"yf": "^TNX",  "vote_dir": -1, "label": "10Y 미국채금리"},
    "VIX":   {"yf": "^VIX",  "vote_dir": -1, "label": "VIX"},
    "USDKRW":{"yf": "KRW=X", "vote_dir": -1, "label": "USD/KRW"},
}

# 지표별 % 변화 임계(검증 필요) — |chg| 가 warn 이상이면 방향표 1표, strong 이상이면 2표.
#  VIX 는 절대 % 변화가 크므로 별도 임계. TNX10(금리)은 하루 %변동이 채권선물보다 커 별도.
THRESHOLDS = {
    "NQ_F":   {"warn": 0.4, "strong": 0.9},
    "ES_F":   {"warn": 0.4, "strong": 0.9},
    "TNX10":  {"warn": 1.5, "strong": 3.0},
    "VIX":    {"warn": 4.0, "strong": 9.0},
    "USDKRW": {"warn": 0.4, "strong": 0.9},
}

# 종합 판정(검증 필요):
#   risk_score = Σ(방향표). 음수일수록 위험회피.
#   entry_halt = risk_score <= HALT_SCORE  (신규 진입 정지)
#   force_liquidate = risk_score <= LIQ_SCORE (극단 — C++는 우선 로그만, 강제청산 배선은 후속)
HALT_SCORE = -3
LIQ_SCORE  = -6


def now_kst_iso() -> str:
    return datetime.now(KST).isoformat(timespec="seconds")


def fetch_changes(symbols: dict) -> dict:
    """yfinance로 각 심볼의 당일 % 변화(전일 종가 대비)를 best-effort 수집.

    반환: {key: {"pct": float|None, "price": float|None, "err": str|None}}
    네트워크/패키지 실패는 pct=None 으로 격리(하나 죽어도 나머지로 판정).
    """
    try:
        import yfinance as yf
    except ImportError:
        print("[!] yfinance 미설치 — `pip install yfinance` 후 재실행", file=sys.stderr)
        return {k: {"pct": None, "price": None, "err": "no_yfinance"} for k in symbols}

    out = {}
    for key, meta in symbols.items():
        pct = price = None
        err = None
        try:
            t = yf.Ticker(meta["yf"])
            prev = last = None
            # 1순위: fast_info (가벼움). 없으면 2일 일봉으로 폴백.
            fi = getattr(t, "fast_info", None)
            if fi is not None:
                last = fi.get("last_price") if hasattr(fi, "get") else getattr(fi, "last_price", None)
                prev = fi.get("previous_close") if hasattr(fi, "get") else getattr(fi, "previous_close", None)
            if not last or not prev:
                hist = t.history(period="2d", interval="1d")
                if len(hist) >= 2:
                    prev = float(hist["Close"].iloc[-2])
                    last = float(hist["Close"].iloc[-1])
                elif len(hist) == 1:
                    prev = float(hist["Open"].iloc[-1])
                    last = float(hist["Close"].iloc[-1])
            if last and prev and prev != 0:
                price = float(last)
                pct = (float(last) - float(prev)) / float(prev) * 100.0
            else:
                err = "no_data"
        except Exception as e:  # noqa: BLE001 — 심볼 하나 실패가 전체를 멈추면 안 됨
            err = f"{type(e).__name__}:{e}"
        out[key] = {"pct": pct, "price": price, "err": err}
    return out


def vote_for(key: str, pct: float) -> tuple[int, int]:
    """(방향표, |표수|) — 방향표는 risk-on(+)/off(-) 부호, |표수|는 0/1/2 강도."""
    th = THRESHOLDS[key]
    mag = abs(pct)
    strength = 2 if mag >= th["strong"] else (1 if mag >= th["warn"] else 0)
    if strength == 0:
        return 0, 0
    # 지표 상승(pct>0)의 위험선호 방향 = vote_dir. 하락이면 반대.
    direction = SYMBOLS[key]["vote_dir"] * (1 if pct > 0 else -1)
    return direction * strength, strength


def build_regime(changes: dict) -> dict:
    """지표 변화 → 레짐 판정. 반환 dict 가 곧 regime.json 스키마(=C++ 리더 계약).

    C++ 리더 계약:
      entry_halt(bool)  → OrderGate::set_entry_halt(entry_halt) 로 그대로 토글.
      force_liquidate(bool) → 극단 신호. 우선 로그만(강제청산 배선은 Task4 후속).
      stale_after_sec   → C++는 (지금 - ts) > 이 값이면 파일을 신뢰하지 말 것(페일세이프).
                          권장: stale 시 halt를 새로 켜지 말고, 자신이 켠 halt만 유지/해제.
      valid(bool)       → 유효 지표가 부족하면 false. C++는 false면 게이트 변경 금지.
    """
    components = {}
    score = 0
    valid_count = 0
    for key, meta in SYMBOLS.items():
        ch = changes.get(key, {})
        pct = ch.get("pct")
        if pct is None:
            components[key] = {"label": meta["label"], "pct": None, "vote": 0, "err": ch.get("err")}
            continue
        vote, _strength = vote_for(key, pct)
        score += vote
        valid_count += 1
        components[key] = {"label": meta["label"], "pct": round(pct, 3), "vote": vote,
                           "price": ch.get("price")}

    # 유효 지표 절반 미만이면 판정 보류(valid=false) — 데이터 공백에 게이트 오작동 방지.
    valid = valid_count >= max(1, (len(SYMBOLS) + 1) // 2)
    entry_halt = valid and score <= HALT_SCORE
    force_liquidate = valid and score <= LIQ_SCORE

    if not valid:
        regime = "UNKNOWN"
    elif score <= HALT_SCORE:
        regime = "RISK_OFF"
    elif score >= 2:
        regime = "RISK_ON"
    else:
        regime = "NEUTRAL"

    return {
        "schema": 1,
        "ts": now_kst_iso(),
        "regime": regime,
        "entry_halt": entry_halt,
        "force_liquidate": force_liquidate,
        "risk_score": score,
        "valid": valid,
        "valid_count": valid_count,
        "stale_after_sec": 600,
        "thresholds": {"halt_score": HALT_SCORE, "liq_score": LIQ_SCORE},
        "components": components,
        "source": "yfinance",
    }


def write_atomic(path: Path, obj: dict) -> None:
    """tmp 파일에 쓰고 os.replace 로 원자 교체 — 리더가 반쪽 파일을 읽지 않게."""
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(obj, f, ensure_ascii=False, indent=2)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)  # 원자적(같은 볼륨)


def main() -> None:
    ap = argparse.ArgumentParser(description="매크로 레짐 사이드카 (risk-on/off 게이트 프로듀서)")
    ap.add_argument("--out", default=None, help="regime.json 경로 (기본 Quant/config/regime.json)")
    ap.add_argument("--interval", type=float, default=180.0, help="발행 주기(초, 기본 180=3분)")
    ap.add_argument("--once", action="store_true", help="1회 계산 후 종료(점검용)")
    args = ap.parse_args()

    if args.out:
        out_path = Path(args.out)
    else:
        # PYQuant/tools/ → repo 루트/Quant/config/regime.json
        out_path = Path(__file__).resolve().parents[2] / "Quant" / "config" / "regime.json"

    print(f"매크로 레짐 사이드카 | 출력={out_path} | 주기={args.interval}s | once={args.once}")
    print("⚠️ 임계값은 검증 필요 가정 — 라이브 관찰하며 보정(STRATEGIES.md 참조)")

    while True:
        changes = fetch_changes(SYMBOLS)
        regime = build_regime(changes)
        write_atomic(out_path, regime)
        comp = " ".join(
            f"{k}={v['pct']}%({v['vote']:+d})" if v.get("pct") is not None else f"{k}=NA"
            for k, v in regime["components"].items()
        )
        print(f"[{regime['ts']}] regime={regime['regime']} score={regime['risk_score']} "
              f"halt={regime['entry_halt']} liq={regime['force_liquidate']} | {comp}")
        if args.once:
            break
        try:
            time.sleep(args.interval)
        except KeyboardInterrupt:
            print("\n중단 — 마지막 regime.json 유지")
            break


if __name__ == "__main__":
    main()
