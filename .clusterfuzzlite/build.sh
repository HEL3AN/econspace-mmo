#!/bin/bash -eu
# Builds the fuzz targets for ClusterFuzzLite (#518). $CC, $CXX, $CFLAGS, $CXXFLAGS,
# $LIB_FUZZING_ENGINE, $OUT and $WORK are set by the base-builder image; CFLAGS carry the
# sanitizer chosen by the workflow and the coverage instrumentation.

cd "$SRC/econspace"

cmake -S . -B "$WORK/build" \
    -DECON_FUZZ=ON \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
    -DCMAKE_C_FLAGS="$CFLAGS" -DCMAKE_CXX_FLAGS="$CXXFLAGS" \
    -DCMAKE_EXE_LINKER_FLAGS="$CXXFLAGS"

cmake --build "$WORK/build" -j "$(nproc)" \
    --target fuzz_frames fuzz_decode fuzz_login fuzz_seeds

TARGETS="fuzz_frames fuzz_decode fuzz_login"
for t in $TARGETS; do
    cp "$WORK/build/bin/fuzz/$t" "$OUT/$t"
    cp tests/fuzz/econ.dict "$OUT/$t.dict"
done
# A frame is capped at 64 KB on the server; to reach the cap the input has to be longer.
cp tests/fuzz/fuzz_frames.options "$OUT/fuzz_frames.options"

# The seed corpora are real messages, written by the encoders (tests/fuzz/fuzz_seeds.cpp).
# The folders are named after the targets they feed.
SEEDS="$WORK/seeds"
rm -rf "$SEEDS"
"$WORK/build/bin/fuzz/fuzz_seeds" "$SEEDS"
(cd "$SEEDS/frames" && zip -q -r "$OUT/fuzz_frames_seed_corpus.zip" .)
(cd "$SEEDS/decode" && zip -q -r "$OUT/fuzz_decode_seed_corpus.zip" .)
(cd "$SEEDS/login" && zip -q -r "$OUT/fuzz_login_seed_corpus.zip" .)
