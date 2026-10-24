#!/usr/bin/env bash
# Two clients at once, which nothing checked until #107 -- and that is why one account being
# playable by two connections at the same time (#105) reached main. Deliberately over sockets
# rather than in-process: the hello handler, the per-name account files and the transport are
# exactly the parts that only exist there. One script for every CI job that runs it (#359).
#
#   bash tools/ci/two_clients.sh [--other-data]
#
# --other-data also checks that a client reading other data is refused (#400).
set -e
. "$(dirname "$0")/server.sh"

start_server mc.log
# However this ends -- a failed check under set -e included -- stop the server and show its
# log. A step that dies without the server's side of the story is the one nobody can diagnose
# afterwards.
trap 'stop_server; echo "--- server log ---"; cat mc.log' EXIT
port=$SERVER_PORT

# Every client is bounded twice: by its own watchdog (econagent selftest gives up and names
# what it was waiting for), and here, for a process that will not exit at all.
agent() { timeout -k 10 300 "$BIN/agent/econagent$EXE" "$@"; }

# Two people sharing a galaxy. Each has to complete its own scripted run. Each client writes
# to its own file: two on one pipe mix their lines and the log no longer says which one
# failed (#419).
agent selftest 127.0.0.1 $port alice alice-secret > alice.log 2>&1 &
alice=$!
agent selftest 127.0.0.1 $port bob bob-secret > bob.log 2>&1 &
bob=$!
status=0
wait $alice || status=1
wait $bob || status=1
for c in alice bob; do echo "--- $c ---"; cat $c.log; done
[ $status -eq 0 ] || exit 1

# One account, one session: a second login for a name displaces the first, and the newcomer
# still plays normally afterwards. The second login waits until the first has actually
# joined -- after a fixed three seconds it sometimes had not, and then the "first" displaced
# the second and the check failed.
sleep 30 | agent connect 127.0.0.1 $port carol carol-secret --start random >/dev/null 2>&1 &
holder=$!
wait_for mc.log "carol joined"
agent selftest 127.0.0.1 $port carol carol-secret

# And the secret is not decoration: the wrong one is refused (#106).
if agent selftest 127.0.0.1 $port carol not-the-secret; then
    echo "a wrong secret was accepted"
    exit 1
fi
kill $holder 2>/dev/null || true
grep -q "displaced" mc.log

# A brand-new account has no file until its first save. While its first session is still
# playing, a login with another secret must be refused, not registered over it (#201).
sleep 30 | agent connect 127.0.0.1 $port dave dave-secret --start random >/dev/null 2>&1 &
dave=$!
wait_for mc.log "dave joined"
if agent selftest 127.0.0.1 $port dave someone-elses-secret; then
    echo "a new account was taken over during its first session"
    exit 1
fi
kill $dave 2>/dev/null || true

if [ "$1" = "--other-data" ]; then
    # Other data is refused with a Bye that names the file (#400): a client whose goods.json
    # is not the server's would price and list a market that is not there.
    rm -rf build/ci-otherdata
    cp -r data build/ci-otherdata
    echo " " >> build/ci-otherdata/goods.json
    if agent selftest 127.0.0.1 $port erin erin-secret --data build/ci-otherdata; then
        echo "a client with other data was let in"
        exit 1
    fi
    grep -q "erin: refused -- Your game data differs from this server's in goods.json" mc.log
fi
