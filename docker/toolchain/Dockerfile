# The digest pins the compiler/toolchain layer. The libdragon library and host
# tools are rebuilt below from the separately pinned upstream submodule.
FROM ghcr.io/dragonminded/libdragon:preview@sha256:035dee441106aafdfd9a4d35f78c3106428fa81acf0113be30313f7409598ab9

ARG LIBDRAGON_COMMIT=unknown
LABEL org.opencontainers.image.source="https://github.com/elohmeier/tw64"
LABEL org.tw64.libdragon-commit=${LIBDRAGON_COMMIT}

COPY . /opt/libdragon
RUN rm -f /opt/libdragon/.git && \
    cd /opt/libdragon && \
    ./build.sh --no-examples

WORKDIR /workdir
