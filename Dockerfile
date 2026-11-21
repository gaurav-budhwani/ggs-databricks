# Shopping Sessions: High-Performance Engine
# Target platform: linux/amd64 (Intel Xeon Cascade Lake or newer)

FROM python:3.12-slim-bookworm

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    && rm -rf /var/lib/apt/lists/*

RUN pip install --no-cache-dir pyarrow

RUN useradd --uid 1000 --no-create-home --home-dir /scratch app

WORKDIR /app
COPY src/ /app/src/
COPY run.sh /app/run.sh

# Compile C++ engine with native optimization (pure standard C++20, no external C++ libraries needed)
RUN g++ -O3 -std=c++20 -march=x86-64-v3 -DNDEBUG -pthread \
    /app/src/engine.cpp \
    -o /app/engine \
    && apt-get purge -y build-essential \
    && apt-get autoremove -y \
    && rm -rf /var/lib/apt/lists/* \
    && chmod +x /app/engine /app/run.sh

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1

WORKDIR /app
USER 1000
CMD ["/app/run.sh"]
