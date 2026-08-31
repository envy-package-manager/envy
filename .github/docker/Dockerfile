# Linux CI image. Tracks whatever the newest g++-N in the archive is rather than
# pinning, so a weekly rebuild picks up new majors on its own.
# No cmake/ninja: tools/build.py fetches pinned copies into out/cache.
FROM ubuntu:latest

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        binutils \
        ca-certificates \
        curl \
        fish \
        gh \
        git \
        python3 \
        valgrind \
        zsh \
        zstd \
    && ver="$(apt-cache --names-only search '^g\+\+-[0-9]+$' \
        | cut -d' ' -f1 | cut -d- -f2 | sort -n | tail -1)" \
    && if [ -z "$ver" ]; then echo "no g++-N package in the archive" >&2; exit 1; fi \
    && apt-get install -y --no-install-recommends "g++-${ver}" \
    && update-alternatives --install /usr/bin/gcc gcc "/usr/bin/gcc-${ver}" 100 \
        --slave /usr/bin/gcov gcov "/usr/bin/gcov-${ver}" \
    && update-alternatives --install /usr/bin/g++ g++ "/usr/bin/g++-${ver}" 100 \
    && update-alternatives --install /usr/bin/cc cc "/usr/bin/gcc-${ver}" 100 \
    && update-alternatives --install /usr/bin/c++ c++ "/usr/bin/g++-${ver}" 100 \
    && rm -rf /var/lib/apt/lists/*

ENV CC=gcc \
    CXX=g++
