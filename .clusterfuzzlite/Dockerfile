# ClusterFuzzLite build image (#518). base-builder brings clang, libFuzzer and the
# sanitizer runtimes; what is added is what the normal Linux CI job installs for raylib's
# sake (no window is ever opened, but raylib is linked), plus zip for the seed corpora.
FROM gcr.io/oss-fuzz-base/base-builder

RUN apt-get update && apt-get install -y --no-install-recommends \
        cmake zip unzip \
        libasound2-dev libgl1-mesa-dev libglu1-mesa-dev libwayland-dev libx11-dev \
        libxcursor-dev libxi-dev libxinerama-dev libxkbcommon-dev libxrandr-dev \
    && rm -rf /var/lib/apt/lists/*

COPY . $SRC/econspace
WORKDIR $SRC/econspace
COPY .clusterfuzzlite/build.sh $SRC/build.sh
