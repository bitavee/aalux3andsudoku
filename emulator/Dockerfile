# Multi-stage build for the AALU native emulator.
#
# Stage 1 compiles the C++ server.
# Stage 2 ships a slim runtime image with only the binary, the web assets,
# and the OpenSSL runtime libs.

# ─── build stage ──────────────────────────────────────────────────────────────
FROM debian:bookworm-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential \
      cmake \
      libssl-dev \
      ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY native/ /src/native/

WORKDIR /src/native/build
RUN cmake -DCMAKE_BUILD_TYPE=Release .. \
    && cmake --build . --parallel \
    && strip aalu_emulator

# ─── runtime stage ────────────────────────────────────────────────────────────
FROM debian:bookworm-slim AS runtime

RUN apt-get update && apt-get install -y --no-install-recommends \
      libssl3 \
      ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/native/build/aalu_emulator /usr/local/bin/aalu_emulator
COPY web/ /web/

ENV AALU_WEB_ROOT=/web \
    AALU_SDCARD=/sdcard \
    AALU_PORT=8080

EXPOSE 8080

CMD ["/usr/local/bin/aalu_emulator"]
