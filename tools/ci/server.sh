# Sourced by CI steps that need a live server (#359):
#
#   . tools/ci/server.sh
#   start_server server.log              # sets SERVER_PID and SERVER_PORT
#   wait_for server.log "carol joined"   # what the server says, never a fixed time
#   stop_server
#
# The server takes port 0: the system picks a free one and the server says which (#306). A
# fixed port in the dynamic range can already be some outbound connection's source port.
# Wait for what the server says, never for a fixed time: a sleep is a guess about how fast
# the runner is today, and a guess that is wrong once is a flaky step.
#
# BIN is where the binaries are (./build/bin by default); EXE is their suffix, ".exe" on
# Windows.
BIN=${BIN:-./build/bin}
EXE=${EXE:-}

wait_for() {  # LOG TEXT
    for _ in $(seq 1 150); do grep -q "$2" "$1" && return 0; sleep 0.2; done
    echo "timed out waiting for the server to log: $2"
    return 1
}

start_server() {  # LOG [host flags...]
    local log=$1
    shift
    "$BIN/server/econserver$EXE" host 0 "$@" > "$log" 2>&1 &
    SERVER_PID=$!
    if ! wait_for "$log" "EconSpace server on port"; then
        cat "$log"
        return 1
    fi
    SERVER_PORT=$(sed -n 's/.*EconSpace server on port \([0-9]*\).*/\1/p' "$log")
}

stop_server() {
    kill "$SERVER_PID" 2>/dev/null || true
}
