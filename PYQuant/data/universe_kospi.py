"""
정적 KOSPI 대형·중형주 유니버스 (point-in-time 조회 대체).

배경: KRX 데이터포털 MDC 스냅샷 엔드포인트(get_market_cap_by_ticker /
get_market_ohlcv_by_ticker / get_index_portfolio_deposit_file)가 OTP 생성 단계에서
"LOGOUT"을 반환하며 전부 차단됨(KRX 안티스크래핑 — 세션 쿠키·HTTPS·pykrx 1.2.8로도
못 뚫음). per-ticker 시계열 OHLCV·종목명 조회만 살아있다. 따라서 시총 상위 동적
유니버스를 구성할 수 없어, 유동성 높은 대형·중형주를 정적으로 고정한다.

⚠️ 한계 — survivorship bias:
  이 리스트는 '현재 시점'의 우량주 집합이다. 과거 백테스트 기간에 상장폐지·편출된
  종목은 빠져 있고, 그 사이 살아남아 커진 종목이 과대 대표된다. 따라서 2년 백테스트
  수익률은 실제보다 낙관 편향될 수 있다. 횡단면 모멘텀의 '파이프라인·랭킹 폭'을 키우는
  1차 재측정 용도로는 충분하나, 엣지 판정 시 이 편향을 반드시 감안할 것.
  (point-in-time 유니버스가 필요해지면 KIS 종목마스터 파일 파싱으로 대체)

검증: validate_universe()가 각 코드를 종목명 조회로 확인해 오타/편입오류를 걸러낸다.
  값(기대 종목명)과 실제 조회명이 어긋나면 silent error이므로 경고한다.
"""
from __future__ import annotations

# code -> 기대 종목명(검증·가독성용). pykrx 조회명과 대조해 오류를 잡는다.
KOSPI_LARGE_MID: dict[str, str] = {
    # 반도체·IT 하드웨어
    "005930": "삼성전자",     "000660": "SK하이닉스",   "009150": "삼성전기",
    "011070": "LG이노텍",     "000990": "DB하이텍",     "042700": "한미반도체",
    "058470": "리노공업",     "240810": "원익IPS",       "357780": "솔브레인",
    # 인터넷·게임·엔터
    "035420": "NAVER",        "035720": "카카오",        "036570": "엔씨소프트(NC)",
    "251270": "넷마블",       "259960": "크래프톤",      "263750": "펄어비스",
    "293490": "카카오게임즈", "377300": "카카오페이",    "323410": "카카오뱅크",
    "352820": "하이브",
    # 2차전지·소재
    "373220": "LG에너지솔루션","006400": "삼성SDI",       "051910": "LG화학",
    "003670": "포스코퓨처엠",  "247540": "에코프로비엠",  "086520": "에코프로",
    "066970": "엘앤에프",      "014680": "한솔케미칼",
    # 자동차·부품
    "005380": "현대차",       "000270": "기아",          "012330": "현대모비스",
    "011210": "현대위아",     "086280": "현대글로비스",  "161390": "한국타이어앤테크놀로지",
    "204320": "HL만도",
    # 금융
    "105560": "KB금융",       "055550": "신한지주",      "086790": "하나금융지주",
    "316140": "우리금융지주", "138040": "메리츠금융지주","024110": "기업은행",
    "138930": "BNK금융지주",  "175330": "JB금융지주",    "139130": "iM금융지주",
    "071050": "한국금융지주", "005940": "NH투자증권",    "039490": "키움증권",
    "006800": "미래에셋증권", "016360": "삼성증권",      "029780": "삼성카드",
    "001450": "현대해상",     "000810": "삼성화재",      "032830": "삼성생명",
    "088350": "한화생명",
    # 바이오·제약
    "207940": "삼성바이오로직스","068270": "셀트리온",    "326030": "SK바이오팜",
    "302440": "SK바이오사이언스","128940": "한미약품",    "000100": "유한양행",
    "069620": "대웅제약",
    # 화학·정유·소재
    "010950": "S-Oil",        "096770": "SK이노베이션",  "011170": "롯데케미칼",
    "285130": "SK케미칼",     "011780": "금호석유",      "120110": "코오롱인더",
    "298050": "효성첨단소재", "009830": "한화솔루션",
    # 철강·금속
    "005490": "POSCO홀딩스",  "004020": "현대제철",      "103140": "풍산",
    "010130": "고려아연",
    # 조선·기계·방산
    "267250": "HD현대",       "329180": "HD현대중공업",  "042670": "HD현대인프라코어",
    "267270": "HD건설기계",  "010140": "삼성중공업",    "042660": "한화오션",
    "012450": "한화에어로스페이스","047810": "한국항공우주","079550": "LIG디펜스앤에어로스페이스",
    "064350": "현대로템",     "034020": "두산에너빌리티","241560": "두산밥캣",
    "010120": "LS ELECTRIC",  "006260": "LS",
    # 건설
    "000720": "현대건설",     "028050": "삼성E&A",       "047040": "대우건설",
    "006360": "GS건설",       "375500": "DL이앤씨",      "294870": "IPARK현대산업개발",
    # 통신·유틸리티
    "017670": "SK텔레콤",     "030200": "KT",            "032640": "LG유플러스",
    "015760": "한국전력",     "036460": "한국가스공사",
    # 소비재·유통·화장품
    "097950": "CJ제일제당",   "271560": "오리온",        "004990": "롯데지주",
    "023530": "롯데쇼핑",     "139480": "이마트",        "282330": "BGF리테일",
    "007070": "GS리테일",     "161890": "한국콜마",      "090430": "아모레퍼시픽",
    "051900": "LG생활건강",   "008770": "호텔신라",      "033780": "KT&G",
    "005300": "롯데칠성",     "280360": "롯데웰푸드",    "003230": "삼양식품",
    "001680": "대상",
    # 운송
    "003490": "대한항공",     "011200": "HMM",           "028670": "팬오션",
    # 지주·기타 대형
    "003550": "LG",          "034730": "SK",            "028260": "삼성물산",
    "402340": "SK스퀘어",     "066570": "LG전자",        "034220": "LG디스플레이",
    "018260": "삼성에스디에스","035900": "JYP Ent.",     "000150": "두산",
    "010620": "HD현대미포",   "069960": "현대백화점",    "004170": "신세계",
    "002790": "아모레퍼시픽홀딩스","001040": "CJ",        "090435": "아모레퍼시픽우",
}


def universe_codes() -> list[str]:
    """결정성 위해 코드 오름차순 정렬 리스트 반환."""
    return sorted(KOSPI_LARGE_MID.keys())


def validate_universe(source, *, verbose: bool = True) -> tuple[list[str], list[str]]:
    """
    각 코드를 종목명 조회로 검증. 기대명과 실제명이 일치(부분일치)하면 통과.
    source: ticker_name(code)->str 를 제공하는 객체(KrxSource).
    반환: (valid_codes, problems)  problems는 사람이 읽는 진단 문자열 목록.
    """
    valid: list[str] = []
    problems: list[str] = []
    for code in universe_codes():
        expected = KOSPI_LARGE_MID[code]
        try:
            actual = source.ticker_name(code) or ""
        except Exception as e:                       # noqa: BLE001
            problems.append(f"{code} ({expected}): 조회 실패 {type(e).__name__}")
            continue
        if not actual:
            problems.append(f"{code} ({expected}): 미상장/조회 빈값 — 제외")
            continue
        # 이름 정규화 후 부분일치(공백·표기 차이 허용). 불일치면 코드 오타 의심.
        norm = lambda s: s.replace(" ", "").replace(".", "").replace("(주)", "")
        if norm(expected) not in norm(actual) and norm(actual) not in norm(expected):
            problems.append(f"{code}: 기대 '{expected}' ≠ 실제 '{actual}' — 코드 오류 의심, 제외")
            continue
        valid.append(code)
    if verbose:
        print(f"[universe] 검증 통과 {len(valid)}/{len(KOSPI_LARGE_MID)}종목")
        for p in problems:
            print(f"  [!] {p}")
    return valid, problems


if __name__ == "__main__":
    # 전수 확인용: 131종목 (코드 / 기대명 / 실제 pykrx 조회명 / 일치여부) 표 출력.
    #   실행:  python3 PYQuant/data/universe_kospi.py
    import sys
    from pathlib import Path
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))  # PYQuant/ 패키지 루트
    from data.krx_source import KrxSource

    src = KrxSource(market="KOSPI")
    norm = lambda s: s.replace(" ", "").replace(".", "").replace("(주)", "")
    print(f"{'No.':>3} {'코드':<7} {'기대명':<16} {'실제(pykrx)':<16} 판정")
    print("-" * 60)
    ok = 0
    for i, code in enumerate(universe_codes(), 1):
        expected = KOSPI_LARGE_MID[code]
        try:
            actual = src.ticker_name(code) or ""
        except Exception as e:                       # noqa: BLE001
            actual = f"<조회실패 {type(e).__name__}>"
        match = bool(actual) and (norm(expected) in norm(actual) or norm(actual) in norm(expected))
        ok += match
        mark = "OK" if match else "  <<< 불일치"
        print(f"{i:>3} {code:<7} {expected:<16} {actual:<16} {mark}")
    print("-" * 60)
    print(f"총 {len(KOSPI_LARGE_MID)}종목 중 {ok}종목 일치"
          + ("  ✔ 전수 통과" if ok == len(KOSPI_LARGE_MID) else "  ⚠ 불일치 확인 요망"))
