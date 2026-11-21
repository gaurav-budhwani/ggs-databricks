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

# Compile C++ engine with PyArrow Arrow/Parquet C++ libraries and native x86-64-v3 optimization
RUN PYARROW_INC=$(python3 -c "import pyarrow; print(pyarrow.get_include())") && \
    PYARROW_LIB=$(python3 -c "import pyarrow; print(pyarrow.get_library_dirs()[0])") && \
    g++ -O3 -std=c++20 -march=x86-64-v3 -DNDEBUG -pthread \
    -isystem "${PYARROW_INC}" \
    /app/src/engine.cpp \
    -o /app/engine \
    -L"${PYARROW_LIB}" \
    -lparquet -larrow \
    -Wl,-rpath,"${PYARROW_LIB}" \
    && apt-get purge -y build-essential \
    && apt-get autoremove -y \
    && rm -rf /var/lib/apt/lists/* \
    && chmod +x /app/engine /app/run.sh

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1

WORKDIR /app
USER 1000
CMD ["/app/run.sh"]
