# Reproducible build of the headless `saida_tool` binary for Linux. This
# binary is a hard runtime dependency of the platform (GET /scene, periodic
# snapshots, agent dry-run): the `apps/api` and `apps/workers` Docker images
# copy it from this stage.
#
#   docker build -f docker/saida-tool.Dockerfile -t saida-tool .
#   docker create --name tmp saida-tool && docker cp tmp:/out/saida_tool . && docker rm tmp
#
# Notes:
# - The tool is headless: Vulkan is only required at compile time (headers +
#   libvulkan.so to link saida_engine), never at runtime — no GPU/ICD is
#   needed in the final image.
# - XR and MCP are out of scope for headless: XR makes no sense without a
#   headset, and the MCP server uses winsock (Windows only).
# - Base is **Debian bookworm** on purpose: the platform images (`node:24-slim`)
#   are bookworm (glibc 2.36) — building on a newer distro produces a binary
#   that requires GLIBC_2.38+ and breaks at COPY --from (verified).

# ── Stage 1: build ───────────────────────────────────────────────────────────
FROM debian:bookworm-slim AS build

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build \
        libvulkan-dev libglfw3-dev libglm-dev glslc \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DSAIDA_ENABLE_XR=OFF \
        -DSAIDA_ENABLE_MCP=OFF \
    && cmake --build build --target saida_tool

# ── Stage 2: minimal artefact ────────────────────────────────────────────────
# Runtime image: the binary + its dynamic libs (Vulkan loader and GLFW — never
# loaded by headless commands, but linked into the exe). Platform images do
# `COPY --from=saida-tool /out/saida_tool /usr/local/bin/` and install the same
# runtime packages.
FROM debian:bookworm-slim
LABEL org.opencontainers.image.source="https://github.com/saias-o/SaidaEngine"
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        libvulkan1 libglfw3 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /src/build/bin/saida_tool /out/saida_tool
# Build-time sanity check: the binary starts and knows its commands.
RUN /out/saida_tool describe-engine >/dev/null
ENTRYPOINT ["/out/saida_tool"]
