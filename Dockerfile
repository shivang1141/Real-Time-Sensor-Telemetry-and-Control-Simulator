# Reproducible build-and-test environment plus a slim runtime image.
#
#   docker build -t sensor-telemetry .            # builds, runs the full test suite, packages
#   docker build --target build -t telemetry-dev .  # stop after build + tests
#   docker compose up                             # server + simulator (see docker-compose.yml)

FROM ubuntu:24.04 AS build
RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build libgtest-dev nlohmann-json3-dev python3 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake --preset release \
    && cmake --build --preset release \
    && ctest --preset release

FROM ubuntu:24.04 AS runtime
RUN useradd --system --no-create-home telemetry
COPY --from=build /src/build/release/apps/telemetry_server \
                  /src/build/release/apps/sensor_sim \
                  /src/build/release/benchmarks/telemetry_loadgen \
                  /usr/local/bin/
COPY config /etc/telemetry
USER telemetry
EXPOSE 9000
ENTRYPOINT ["telemetry_server"]
CMD ["--config", "/etc/telemetry/server.json"]
