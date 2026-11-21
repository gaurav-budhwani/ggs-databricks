# Shopping Sessions: High-Performance C++20 Pipeline
# Target platform: linux/amd64 (Intel Xeon Cascade Lake or newer)

FROM debian:bookworm-slim

# Install build dependencies and Apache Arrow / Parquet libraries
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    libarrow-dev \
    libparquet-dev \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# The scorer runs as uid 1000; give that uid a name and home
RUN useradd --uid 1000 --no-create-home --home-dir /scratch app

WORKDIR /app
COPY src/ /app/src/
COPY run.sh /app/run.sh

# Compile C++20 with full optimizations:
# -O3, -flto, -march=x86-64-v3 (enables AVX2, FMA, BMI2 for Intel Xeon Cascade Lake)
RUN g++ -O3 -std=c++20 -march=x86-64-v3 -flto -pthread \
    /app/src/pipeline.cpp \
    -larrow -lparquet \
    -o /app/pipeline \
    && rm -rf /app/src \
    && apt-get purge -y build-essential \
    && apt-get autoremove -y \
    && rm -rf /var/lib/apt/lists/* \
    && chmod +x /app/pipeline /app/run.sh

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1

WORKDIR /app
USER 1000
CMD ["/app/run.sh"]

