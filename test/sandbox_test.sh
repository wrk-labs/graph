#!/bin/sh
# Sandbox policy test.
#
# Runs a matrix of writes under the exact sandbox a shell tab gets, via the
# shell's `--sandbox-run <repo> -- <cmd>` hook, and checks that the right
# paths are writable and the rest are not. Everything happens under a
# throwaway HOME so a real machine is never touched.
#
# macOS drives Seatbelt (sandbox-exec); Linux (Landlock) is added once the
# shell's confine() lands — until then the Linux branch reports a skip.
#
#   sh test/sandbox_test.sh

set -eu

. "$(dirname "$0")/lib.sh"

case "$(uname -s)" in
Darwin)
	SHELL_BIN=./Graph.app/Contents/MacOS/graph-shell
	PLATFORM=mac
	;;
Linux)
	SHELL_BIN=./graph-shell
	PLATFORM=linux
	;;
*)
	echo "sandbox test: unsupported platform $(uname -s)"; exit 0 ;;
esac

if [ ! -x "$SHELL_BIN" ]; then
	echo "sandbox test: $SHELL_BIN not built; run make first"; exit 0
fi

if [ "$PLATFORM" = linux ]; then
	# confine() is not written yet; skip rather than pretend to pass.
	echo "sandbox test: Linux (Landlock) confinement not implemented yet — skipping"
	exit 0
fi

# A throwaway HOME and repo, deliberately NOT under /tmp (the profile allows
# /tmp wholesale, which would mask the home rules).
BASE="$PWD/tmp/sandbox-test.$$"
FAKE="$BASE/home"
REPO="$BASE/repo"
trap 'rm -rf "$BASE"' EXIT
mkdir -p \
	"$FAKE/.claude" "$FAKE/.config/nvim" "$FAKE/.local/share" \
	"$FAKE/.local/bin" "$FAKE/.ssh" "$FAKE/Documents" \
	"$FAKE/Projects/other" "$REPO"
: > "$FAKE/.zshrc"
: > "$FAKE/Documents/keep.txt"

group "sandbox — write policy"

# One sandboxed shell tries every write and reports W (allowed) or D (denied).
matrix=$(HOME="$FAKE" "$SHELL_BIN" --sandbox-run "$REPO" /bin/sh -c '
	w() { touch "$2" 2>/dev/null && echo "$1=W" || echo "$1=D"; rm -f "$2" 2>/dev/null; }
	w repo     "'"$REPO"'/f"
	w tmp      "/tmp/graph-sbtest.$$"
	w dotdir   "$HOME/.claude/state"
	w history  "$HOME/.zsh_history"
	w config   "$HOME/.config/nvim/init.vim"
	w localshr "$HOME/.local/share/x"
	w zshrc    "$HOME/.zshrc"
	w ssh      "$HOME/.ssh/id"
	w localbin "$HOME/.local/bin/evil"
	w docs     "$HOME/Documents/keep.txt"
	mkdir "$HOME/.brandnew" 2>/dev/null && echo newdot=W || echo newdot=D; rmdir "$HOME/.brandnew" 2>/dev/null
	w newvis   "$HOME/visible"
')

check_contains "repository is writable"        "$matrix" "repo=W"
check_contains "/tmp is writable"              "$matrix" "tmp=W"
check_contains "a dotfolder is writable"       "$matrix" "dotdir=W"
check_contains "shell history is writable"     "$matrix" "history=W"
check_contains "~/.config is writable"         "$matrix" "config=W"
check_contains "~/.local/share is writable"    "$matrix" "localshr=W"
check_contains "a new dotfolder is writable"   "$matrix" "newdot=W"

check_contains "~/.zshrc is read-only"         "$matrix" "zshrc=D"
check_contains "~/.ssh is read-only"           "$matrix" "ssh=D"
check_contains "~/.local/bin is read-only"     "$matrix" "localbin=D"
check_contains "visible home is read-only"     "$matrix" "docs=D"
check_contains "a new visible file is denied"  "$matrix" "newvis=D"

group "sandbox — reads stay open"

reads=$(HOME="$FAKE" "$SHELL_BIN" --sandbox-run "$REPO" /bin/sh -c '
	cat "$HOME/Documents/keep.txt" >/dev/null 2>&1 && echo docs=R || echo docs=X
	cat "$HOME/.zshrc"             >/dev/null 2>&1 && echo zshrc=R || echo zshrc=X
')
check_contains "can read a protected file"     "$reads" "zshrc=R"
check_contains "can read visible home"         "$reads" "docs=R"

summary
