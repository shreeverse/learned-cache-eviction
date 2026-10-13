# lcache-api in a container (decisions D28). Build from the repo root:
#   docker build -f api/Dockerfile -t lcache-api .
# Traces are not in the image: mount a directory of them at /app/data.

# ---- build: compile only the API binary ----------------------------------------------------
FROM ubuntu:jammy-20260901.2 AS build
RUN apt-get update \
    && apt-get install -y --no-install-recommends cmake ninja-build g++ ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
# The top-level CMakeLists.txt adds every module, so all of their sources must be present.
COPY CMakeLists.txt CMakePresets.json ./
COPY cmake cmake
COPY trace trace
COPY sketch sketch
COPY ml ml
COPY policy policy
COPY sim sim
COPY server server
COPY api api
COPY apps apps
# No tests or benchmarks; no LightGBM: 'learned' runs on the compiled tree evaluator (D19).
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DLC_BUILD_TESTS=OFF -DLC_BUILD_BENCHMARKS=OFF -DLC_WITH_LIGHTGBM=OFF \
    && cmake --build build --target lcache-api

# ---- runtime: the binary and the models, as a non-root user ---------------------------------
FROM ubuntu:jammy-20260901.2
RUN useradd --system --uid 10001 lcache && mkdir -p /app/data
WORKDIR /app
COPY --from=build /src/build/api/lcache-api /usr/local/bin/lcache-api
COPY models models
USER lcache
EXPOSE 8000
CMD ["lcache-api", "--host", "0.0.0.0", "--port", "8000", "--data-dir", "/app/data", \
     "--model", "/app/models/twitter26_1MiB_h10k_32x31.txt"]
