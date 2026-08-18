import os
from datetime import date


def as_of() -> date:
    """백테스트 기준일. BACKTEST_AS_OF=YYYY-MM-DD 설정 시 고정(재현성), 미설정 시 date.today()(대화형 불변)."""
    v = os.environ.get("BACKTEST_AS_OF")
    return date.fromisoformat(v) if v else date.today()
