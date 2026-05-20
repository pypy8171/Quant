FROM python:3.11-slim

ENV TZ=Asia/Seoul \
    PYTHONUNBUFFERED=1

RUN apt-get update && apt-get install -y --no-install-recommends \
    tzdata \
    && ln -snf /usr/share/zoneinfo/$TZ /etc/localtime \
    && echo $TZ > /etc/timezone \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY PYQuant/requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY PYQuant/ .

# 기본: C++ 엔진 이벤트 모니터링
# 오버라이드 예시: docker compose run quant-python backtest
ENTRYPOINT ["python", "main.py"]
CMD ["monitor"]
