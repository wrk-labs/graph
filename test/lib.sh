# Minimal test harness. POSIX sh, no framework.
#
# Sourced by test/run.sh. Each check reports one line and bumps a counter;
# the run fails if anything failed, so a broken case cannot pass unnoticed.

tests_run=0
tests_failed=0

ok() {
	tests_run=$((tests_run + 1))
	printf '  ok   %s\n' "$1"
}

fail() {
	tests_run=$((tests_run + 1))
	tests_failed=$((tests_failed + 1))
	printf '  FAIL %s\n' "$1"
	[ -n "$2" ] && printf '       %s\n' "$2"
	return 0
}

check() {
	if [ "$2" = "$3" ]; then
		ok "$1"
	else
		fail "$1" "expected [$3], got [$2]"
	fi
}

# check_status <name> <expected status> <command...>
check_status() {
	name=$1 want=$2
	shift 2
	out=$("$@" 2>&1)
	got=$?
	if [ "$got" = "$want" ]; then
		ok "$name"
	else
		fail "$name" "exit $got, wanted $want: $out"
	fi
}

# check_contains <name> <haystack> <needle>
check_contains() {
	case "$2" in
	*"$3"*) ok "$1" ;;
	*) fail "$1" "no [$3] in: $2" ;;
	esac
}

group() {
	printf '\n%s\n' "$1"
}

summary() {
	printf '\n%d run, %d failed\n' "$tests_run" "$tests_failed"
	[ "$tests_failed" -eq 0 ] || exit 1
	exit 0
}
