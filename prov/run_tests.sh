#!/bin/bash
#
# Exercise the uet libfabric provider over network namespaces: a target in
# ofi-a (10.89.0.1) and writers in ofi-b (.2) and ofi-c (.3), on veth pairs
# bridged in ofi-br. Needs sudo; the tests run as the calling user with
# CAP_NET_RAW only.
#
#   prov/run_tests.sh setup | run | teardown | all
#
set -u
DIR=$(cd "$(dirname "$0")" && pwd)
UID_=$(id -u)
GID_=$(id -g)
TMP=$(mktemp -d /tmp/uet-fi-test.XXXXXX)
trap 'rm -rf "$TMP"' EXIT

setup() {
	local i=1 ns
	sudo ip netns add ofi-br
	sudo ip netns exec ofi-br ip link add br0 type bridge
	sudo ip netns exec ofi-br ip link set br0 up
	for ns in ofi-a ofi-b ofi-c; do
		sudo ip netns add $ns
		sudo ip link add ofi0 netns $ns type veth peer name $ns-br \
			netns ofi-br
		sudo ip netns exec ofi-br ip link set $ns-br master br0 up
		sudo ip netns exec $ns ip addr add 10.89.0.$i/24 dev ofi0
		sudo ip netns exec $ns ip link set ofi0 up
		sudo ip netns exec $ns ip link set lo up
		i=$((i + 1))
	done
}

teardown() {
	local ns
	for ns in ofi-a ofi-b ofi-c ofi-br; do
		sudo ip netns del $ns 2>/dev/null
	done
}

# inns <netns> <cmd...>: run as the caller with CAP_NET_RAW
inns() {
	local ns=$1
	shift
	sudo ip netns exec "$ns" setpriv --reuid="$UID_" --regid="$GID_" \
		--init-groups --inh-caps=+net_raw --ambient-caps=+net_raw -- \
		env FI_PROVIDER_PATH="$DIR" UET_IFNAME=ofi0 "$@"
}

wait_file() {
	local i
	for i in $(seq 200); do
		[ -s "$1" ] && return 0
		sleep 0.1
	done
	return 1
}

FAILED=0
result() {
	if [ "$1" = 0 ]; then
		echo "PASS  $2"
	else
		echo "FAIL  $2"
		FAILED=1
	fi
}

# scenario <name> <window bytes> <writers 1|2> [writer options...]:
# writers fill disjoint halves of the target window
scenario() {
	local name=$1 total=$2 writers=$3 half A K B S rc=0 pids=()
	shift 3
	rm -f "$TMP/addr"
	inns ofi-a "$DIR/test_rma" -o "$TMP/addr" -t 120 target "$total" \
		"$writers" > "$TMP/target.log" 2>&1 &
	local tpid=$!
	wait_file "$TMP/addr" || { result 1 "$name (target)"; return; }
	read -r A K B S < "$TMP/addr"
	if [ "$writers" = 1 ]; then
		inns ofi-b "$DIR/test_rma" -t 120 "$@" write "$A" "$K" "$B" 0 \
			"$total" > "$TMP/w1.log" 2>&1 || rc=1
	else
		half=$((total / 2))
		inns ofi-b "$DIR/test_rma" -t 120 "$@" write "$A" "$K" "$B" 0 \
			"$half" > "$TMP/w1.log" 2>&1 &
		pids+=($!)
		inns ofi-c "$DIR/test_rma" -t 120 "$@" write "$A" "$K" "$B" \
			"$half" $((total - half)) > "$TMP/w2.log" 2>&1 &
		pids+=($!)
		for p in "${pids[@]}"; do
			wait "$p" || rc=1
		done
	fi
	wait "$tpid" || rc=1
	grep -a -h "^WROTE\|^VERIFIED\|^TIMEOUT\|^FAILED\|^completion error" \
		"$TMP"/target.log "$TMP"/w*.log | sed 's/^/      /'
	rm -f "$TMP"/w*.log
	result $rc "$name"
}

pair() {
	local rc=0
	rm -f "$TMP/pb" "$TMP/pc"
	inns ofi-b "$DIR/test_rma" -o "$TMP/pb" -t 60 pair $((16 << 20)) \
		"$TMP/pc" > "$TMP/pb.log" 2>&1 &
	local p1=$!
	inns ofi-c "$DIR/test_rma" -o "$TMP/pc" -t 60 -m -c 1048576 -q 4 \
		pair $((16 << 20)) "$TMP/pb" > "$TMP/pc.log" 2>&1 || rc=1
	wait $p1 || rc=1
	grep -a -h "^PAIR\|^TIMEOUT\|^MISMATCH" "$TMP"/p?.log | sed 's/^/      /'
	result $rc "pair: two endpoints, each initiator and target, 16 MiB each way"
}

bad_key() {
	local A K B S rc
	rm -f "$TMP/addr"
	inns ofi-a "$DIR/test_rma" -o "$TMP/addr" -t 10 target 65536 1 \
		> /dev/null 2>&1 &
	local tpid=$!
	wait_file "$TMP/addr" || return
	read -r A K B S < "$TMP/addr"
	inns ofi-b "$DIR/test_rma" -n -t 10 write "$A" 8001000000000ff5 "$B" 0 \
		4096 > "$TMP/w1.log" 2>&1
	rc=$?
	sudo ip netns exec ofi-a pkill -f "test_rma -o $TMP/addr"
	wait $tpid 2>/dev/null
	grep -a -h "^completion error" "$TMP/w1.log" | sed 's/^/      /'
	# the write must fail with an error completion
	result $([ $rc = 1 ] && grep -q "^completion error" "$TMP/w1.log";
		 echo $?) "unregistered key reported as an error completion"
}

run() {
	echo "== FI_PROVIDER_PATH=$DIR fi_info -p uet -v (in ofi-a)"
	sudo ip netns exec ofi-a env FI_PROVIDER_PATH="$DIR" fi_info -p uet -v
	echo
	scenario "1 MiB, one writer, fi_writemsg" $((1 << 20)) 1
	scenario "1 MiB, two writers, 4 KiB writes" $((1 << 20)) 2 -c 4096
	scenario "32 MiB, two writers, 64 KiB writes" $((32 << 20)) 2
	scenario "64 MiB, two writers, 4 MiB fi_write, registered source" \
		$((64 << 20)) 2 -a write -r -c $((4 << 20)) -q 4
	scenario "8 MiB, one writer, 100000-byte writes" $((8 << 20)) 1 \
		-c 100000 -q 8
	pair
	bad_key
	[ $FAILED = 0 ] && echo "all passed" || echo "some failed"
	return $FAILED
}

case "${1:-}" in
setup) setup ;;
run) run ;;
teardown) teardown ;;
all) setup && { run; rc=$?; teardown; exit $rc; } ;;
*) echo "usage: $0 setup|run|teardown|all" >&2; exit 2 ;;
esac
