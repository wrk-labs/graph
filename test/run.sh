#!/bin/sh
# Test entry point. Runs inside the container from Dockerfile.test:
#
#   docker build -f Dockerfile.test -t graph-test .
#   docker run --rm -v "$PWD:/src" -w /src graph-test
#
# Nothing here writes outside the container. It reconfigures Samba and
# restarts nothing, so running it on a host by hand is a bad idea.

set -u
cd "$(dirname "$0")/.." || exit 1
. test/lib.sh

WORK=/tmp/graph-test
BUILD=/tmp/graph-build
GRAPH=$BUILD/graph
CC=${CC:-cc}

rm -rf "$WORK" "$BUILD"
mkdir -p "$WORK" || exit 1

# /src is mounted read-only and the container runs as root, so the build goes
# to a scratch copy. Building in place would leave root-owned objects in the
# working tree and break the next build on the host.
mkdir -p "$BUILD" || exit 1
cp -R Makefile VERSION src tools templates "$BUILD"/ || exit 1

# The host tree may carry objects built for another platform, and make would
# take them as up to date. Clean the copy so it builds from sources alone.
(cd "$BUILD" && make -s clean) >/dev/null 2>&1

group "build"
if (cd "$BUILD" && make -s) >/dev/null 2>&1; then
	ok "builds clean"
else
	fail "builds clean" "$(cd "$BUILD" && make 2>&1 | tail -20)"
	summary
fi

# --- graph init -----------------------------------------------------------
# The repository contract the SMB work rests on: serve validates against the
# marker init writes, so these stay covered as serve grows.

group "graph init"

check_status "creates a nonexistent path" 0 "$GRAPH" init "$WORK/new"
check "writes the marker" \
	"$(cat "$WORK/new/.graph/repository" 2>/dev/null)" "version=1"

missing=
for d in inbox people organizations finance research knowledge archive \
	journal journal/agents; do
	[ -d "$WORK/new/$d" ] || missing="$missing $d"
done
check "creates the default layout" "$missing" ""

# Every default directory explains itself, and AGENTS.md explains the whole:
# the guidance is part of the repository, not of the tool.
missing=
for f in AGENTS.md README.md inbox/README.md people/README.md \
	organizations/README.md finance/README.md research/README.md \
	knowledge/README.md archive/README.md journal/README.md \
	journal/agents/README.md; do
	[ -s "$WORK/new/$f" ] || missing="$missing $f"
done
check "writes AGENTS.md and a README in each directory" "$missing" ""
check_contains "AGENTS.md describes the link convention" \
	"$(cat "$WORK/new/AGENTS.md")" "[[people/"

# The starting .graphignore: display keeps ignored directories visible but
# unread, and init is what plants the file.
check_contains "writes a starting .graphignore" \
	"$(cat "$WORK/new/.graphignore" 2>/dev/null)" "node_modules"
check_contains "the starter documents are ignored" \
	"$(cat "$WORK/new/.graphignore" 2>/dev/null)" "/AGENTS.md"

mkdir -p "$WORK/empty"
check_status "accepts an existing empty directory" 0 "$GRAPH" init "$WORK/empty"

check_status "refuses an already-initialized repository" 1 \
	"$GRAPH" init "$WORK/new"

mkdir -p "$WORK/full" && : > "$WORK/full/notes.md"
check_status "refuses a non-empty directory" 1 "$GRAPH" init "$WORK/full"

# A stray dotfile counts as content; adopting such a tree would be a surprise.
mkdir -p "$WORK/dotted" && : > "$WORK/dotted/.DS_Store"
check_status "refuses a directory holding only a dotfile" 1 \
	"$GRAPH" init "$WORK/dotted"

: > "$WORK/afile"
check_status "refuses a path that is not a directory" 1 "$GRAPH" init "$WORK/afile"

check_status "refuses without a path" 1 "$GRAPH" init
check_status "refuses more than one path" 1 "$GRAPH" init "$WORK/a" "$WORK/b"

# --- smb module -----------------------------------------------------------
# Rewrites /etc/samba/smb.conf, which is why the suite is container-only.

group "smb configuration"

if $CC -std=c99 -pedantic -Wall -Wextra -Os -D_XOPEN_SOURCE=700 -I"$BUILD/src" \
    -o "$BUILD/smb_test" test/smb_test.c "$BUILD/src/smb.c" 2>"$WORK/cc.log"; then
	ok "smb_test builds clean"
	if "$BUILD/smb_test"; then
		ok "smb module"
	else
		fail "smb module" "see the checks above"
	fi
else
	fail "smb_test builds clean" "$(cat "$WORK/cc.log")"
fi

# --- graph config ---------------------------------------------------------
# The container has no systemd, so a successful config cannot restart the
# service and says so. What is checked here is everything up to that: the
# argument handling, the generated file, and the shares it must not disturb.

group "graph config"

check_status "refuses without an address" 1 "$GRAPH" config
check_status "refuses an unknown flag" 1 "$GRAPH" config --bogus 127.0.0.1
check_status "refuses --address without a value" 1 "$GRAPH" config --address

rm -f /etc/samba/smb.conf /etc/samba/smb.conf.pre-graph
"$GRAPH" config --address 127.0.0.1 >"$WORK/config.log" 2>&1
# There is no systemd here, so the restart fails and config reports it. The
# configuration is still installed and valid, which is what the rest checks.
# The success path's "listening on ..." line is therefore not covered by the
# suite; it is only reachable on a machine running the service.
check_contains "reports a failed restart" "$(cat "$WORK/config.log")" \
	"could not restart the SMB service"
check_contains "says the configuration is not in use" \
	"$(cat "$WORK/config.log")" "installed but not in use"
check_contains "writes a graph configuration" \
	"$(head -1 /etc/samba/smb.conf)" "Generated by graph"
check_status "the installed configuration is valid" 0 \
	testparm -s /etc/samba/smb.conf

# An address this machine does not have is refused: Samba would accept it and
# then bind nothing, leaving a server that looks configured and serves nobody.
refusal=$("$GRAPH" config --address 203.0.113.1 2>&1)
check_contains "refuses an address this machine lacks" "$refusal" \
	"is not an address of this machine"
check_contains "says what this machine has" "$refusal" "This machine has:"
check_contains "points at every-interface" "$refusal" "Use 0.0.0.0"
check_contains "did not install it" \
	"$(grep 'interfaces =' /etc/samba/smb.conf)" "127.0.0.1"

# The address this container happens to have, discovered rather than assumed:
# a docker bridge address differs between runs.
other=$(printf '%s' "$refusal" | sed -n 's/^graph: This machine has: //p' |
	tr ',' '\n' | sed -n '2p' | tr -d ' ')
if [ -z "$other" ]; then
	fail "found a second local address" "none reported"
else
	ok "found a second local address"

	# Replacing, not adding: what is passed is what is served.
	"$GRAPH" config --address 127.0.0.1 --address "$other" >/dev/null 2>&1
	addrs=$(grep "interfaces =" /etc/samba/smb.conf)
	check_contains "keeps the first address" "$addrs" "127.0.0.1"
	check_contains "adds the second address" "$addrs" "$other"

	"$GRAPH" config --address 127.0.0.1 >/dev/null 2>&1
	addrs=$(grep "interfaces =" /etc/samba/smb.conf)
	check_contains "still listens on the given address" "$addrs" "127.0.0.1"
	case "$addrs" in
	*"$other"*) fail "drops an address left out of the command" "$addrs" ;;
	*) ok "drops an address left out of the command" ;;
	esac
fi

# Every-interface is a request, not an address, so it is never checked against
# one -- and it must lift the restriction rather than bind 0.0.0.0.
"$GRAPH" config --address 0.0.0.0 >/dev/null 2>&1
check_contains "accepts every-interface" \
	"$(grep 'interfaces =' /etc/samba/smb.conf)" "0.0.0.0"
check_contains "lifts the restriction" \
	"$(grep 'bind interfaces only' /etc/samba/smb.conf)" "no"
"$GRAPH" config --address 127.0.0.1 >/dev/null 2>&1
check_contains "restores the restriction for a named address" \
	"$(grep 'bind interfaces only' /etc/samba/smb.conf)" "yes"

# A foreign configuration is displaced with a notice, and preserved once.
rm -f /etc/samba/smb.conf.pre-graph
printf '[global]\n\tworkgroup = OTHER\n' > /etc/samba/smb.conf
"$GRAPH" config --address 127.0.0.1 >"$WORK/displace.log" 2>&1
check_contains "reports a displaced configuration" \
	"$(cat "$WORK/displace.log")" "replaced the existing Samba configuration"
check_contains "preserves the original" \
	"$(cat /etc/samba/smb.conf.pre-graph)" "workgroup = OTHER"

# --- graph serve ----------------------------------------------------------
# Ends by starting smbd and listing the share, so a serve that generates a
# plausible file but does not actually serve cannot pass.

group "graph serve"

rm -f /etc/samba/smb.conf /etc/samba/smb.conf.pre-graph

# An address must be configured first: exposure is never assumed.
out=$("$GRAPH" serve "$WORK/new" --name opsys 2>&1)
check_contains "refuses before an address is configured" "$out" \
	"no listening address is configured"

"$GRAPH" config --address 127.0.0.1 >/dev/null 2>&1

check_status "refuses without a name" 1 "$GRAPH" serve "$WORK/new"
check_status "refuses without a path" 1 "$GRAPH" serve --name opsys

# Never expose a directory that is not a repository, and never initialize one.
mkdir -p "$WORK/plain"
out=$("$GRAPH" serve "$WORK/plain" --name bad 2>&1)
check_contains "refuses a directory that is not a repository" "$out" \
	"is not a Graph repository"
check "did not initialize it" "$([ -d "$WORK/plain/.graph" ] && echo y || echo n)" "n"
check_contains "did not define the share" \
	"$(grep -c '^\[bad\]' /etc/samba/smb.conf)" "0"

# Names reach the configuration and the Samba commands, so they are refused
# rather than escaped.
out=$("$GRAPH" serve "$WORK/new" --name 'a b; rm -rf /' 2>&1)
check_contains "refuses an unusable share name" "$out" "not a usable share name"
out=$("$GRAPH" serve "$WORK/new" --name ok --user 'x;id' 2>&1)
check_contains "refuses an unusable user name" "$out" "not a usable user name"

# The ordinary case.
"$GRAPH" serve "$WORK/new" --name opsys >"$WORK/serve.log" 2>&1
check_contains "defines the share" "$(grep -c '^\[opsys\]' /etc/samba/smb.conf)" "1"
check_contains "points it at the repository" \
	"$(grep -A1 '^\[opsys\]' /etc/samba/smb.conf)" "$WORK/new"
check_status "the configuration is valid" 0 testparm -s /etc/samba/smb.conf

# A second repository must not disturb the first.
"$GRAPH" serve "$WORK/empty" --name work >/dev/null 2>&1
check_contains "keeps the first share" "$(grep -c '^\[opsys\]' /etc/samba/smb.conf)" "1"
check_contains "adds the second share" "$(grep -c '^\[work\]' /etc/samba/smb.conf)" "1"
check_contains "keeps the address" "$(grep 'interfaces =' /etc/samba/smb.conf)" "127.0.0.1"

# A share name is a stable identity: it is never repointed silently.
out=$("$GRAPH" serve "$WORK/empty" --name opsys 2>&1)
check_contains "refuses a name already in use" "$out" "already serves"
check_contains "left the share pointing where it was" \
	"$(grep -A1 '^\[opsys\]' /etc/samba/smb.conf)" "$WORK/new"

# Serving the same repository under the same name changes no configuration,
# but must still make sure the service is running: after a reboot with the
# unit disabled the share exists only on disk. Without systemd here the start
# fails and is reported, which is itself the proof that it was attempted --
# before, this path returned success without touching the service at all.
out=$("$GRAPH" serve "$WORK/new" --name opsys 2>&1)
check_contains "an unchanged config still starts the service" "$out" \
	"could not start the SMB service"
check_contains "and says the share is not being served" "$out" \
	"is not being served"

# The point of all of it: the shares are actually served.
start_smbd || fail "smbd came up" "no answer on 127.0.0.1"
served=$(smbclient -N -L 127.0.0.1 2>&1)
check_contains "smbd lists the first repository" "$served" "opsys"
check_contains "smbd lists the second repository" "$served" "work"

# A share serves no one anonymously, even with no --user given: guest access
# is ruled out for every share graph generates.
for share in opsys work; do
	anon=$(smbclient -N "//127.0.0.1/$share" -c ls 2>&1)
	case "$anon" in
	*NT_STATUS_ACCESS_DENIED*|*NT_STATUS_LOGON_FAILURE*)
		ok "$share refuses an anonymous client" ;;
	*) fail "$share refuses an anonymous client" "$anon" ;;
	esac
done

# Listing a share only proves the server answered on IPC$. Connecting is what
# proves it can be mounted -- a share section that parses, is listed, and then
# refuses every connection passes a listing check and fails this one.
adduser --disabled-password --gecos "" connecttest >/dev/null 2>&1
printf 'conn-pw-1\nconn-pw-1\n' | smbpasswd -s -a connecttest >/dev/null 2>&1
for share in opsys work; do
	conn=$(smbclient "//127.0.0.1/$share" -U connecttest%conn-pw-1 -c ls 2>&1)
	case "$conn" in
	*NT_STATUS_*) fail "an authenticated client can mount $share" "$conn" ;;
	*) ok "an authenticated client can mount $share" ;;
	esac
done
pkill smbd 2>/dev/null

# A missing VFS module is not something Samba reports: the share parses, is
# listed, and then refuses every connection. serve checks for it instead.
vfsdir=$(smbd -b 2>/dev/null | sed -n 's/.*MODULESDIR: *//p')/vfs
if [ -d "$vfsdir" ]; then
	mv "$vfsdir" "$vfsdir.hidden"
	out=$("$GRAPH" serve "$WORK/new" --name novfs 2>&1)
	st=$?
	mv "$vfsdir.hidden" "$vfsdir"
	check_contains "reports the missing modules" "$out" \
		"missing the VFS modules"
	check_contains "names them" "$out" "streams_xattr"
	check_contains "says how to install them" "$out" "samba-vfs-modules"
	check "fails rather than serving" "$st" "1"
	check_contains "did not define the share" \
		"$(grep -c '^\[novfs\]' /etc/samba/smb.conf)" "0"
else
	fail "found the Samba module directory" "smbd -b reported none"
fi

# --- SMB users ------------------------------------------------------------
# Samba stays the authority: graph inspects it rather than keeping a record
# of its own. The prompts read standard input, so they drive here.

group "smb users"

"$GRAPH" init "$WORK/secure" >/dev/null 2>&1

# A Samba user is built on a system account; without one, say so plainly.
out=$(printf 'y\npw\npw\n' | "$GRAPH" serve "$WORK/secure" --name sec --user nosuchuser 2>&1)
check_contains "reports a missing system account" "$out" \
	"has no account on this machine"
check_contains "did not define the share" \
	"$(grep -c '^\[sec\]' /etc/samba/smb.conf)" "0"

adduser --disabled-password --gecos "" graphtest >/dev/null 2>&1

# Declining the prompt must change nothing.
out=$(printf 'n\n' | "$GRAPH" serve "$WORK/secure" --name sec --user graphtest 2>&1)
check_contains "offers to create the user" "$out" "does not exist"
check_contains "declining changes nothing" "$out" "nothing was changed"
check_contains "no user was created" \
	"$(pdbedit -L 2>/dev/null | grep -c graphtest)" "0"

# Mismatched passwords must not create a user either.
out=$(printf 'y\none\ntwo\n' | "$GRAPH" serve "$WORK/secure" --name sec --user graphtest 2>&1)
check_contains "refuses mismatched passwords" "$out" "do not match"
check_contains "still no user" \
	"$(pdbedit -L 2>/dev/null | grep -c graphtest)" "0"

# Accepting creates the user and restricts the share to them.
out=$(printf 'y\nsecret-pw-1\nsecret-pw-1\n' | "$GRAPH" serve "$WORK/secure" \
	--name sec --user graphtest 2>&1)
check_contains "creates the user" "$out" "created"
check_contains "Samba knows the user" \
	"$(pdbedit -L 2>/dev/null | grep -c graphtest)" "1"
check_contains "restricts the share to the user" \
	"$(sed -n '/^\[sec\]/,/^\[/p' /etc/samba/smb.conf)" "valid users = graphtest"

# An existing user is reused, and its password is not touched.
before=$(pdbedit -Lw graphtest 2>/dev/null)
"$GRAPH" init "$WORK/secure2" >/dev/null 2>&1
out=$(printf '' | "$GRAPH" serve "$WORK/secure2" --name sec2 --user graphtest 2>&1)
check_contains "reuses an existing user" "$out" "found"
after=$(pdbedit -Lw graphtest 2>/dev/null)
check "leaves the password alone" "$before" "$after"

# The share really is restricted: an anonymous client must not reach it.
start_smbd || fail "smbd came up" "no answer on 127.0.0.1"
anon=$(smbclient -N '//127.0.0.1/sec' -c 'ls' 2>&1)
case "$anon" in
*NT_STATUS_ACCESS_DENIED*|*NT_STATUS_LOGON_FAILURE*|*NT_STATUS_NO_SUCH_USER*)
	ok "an anonymous client is refused" ;;
*) fail "an anonymous client is refused" "$anon" ;;
esac
auth=$(smbclient '//127.0.0.1/sec' -U graphtest%secret-pw-1 -c 'ls' 2>&1)
case "$auth" in
*NT_STATUS_*) fail "the named user can read the repository" "$auth" ;;
*) ok "the named user can read the repository" ;;
esac
pkill smbd 2>/dev/null

# --- graph unserve --------------------------------------------------------
# The counterpart to serve. There is no systemd here, so stopping the service
# fails and is reported; what is checked is the configuration it leaves.

group "graph unserve"

rm -f /etc/samba/smb.conf /etc/samba/smb.conf.pre-graph
"$GRAPH" config --address 127.0.0.1 >/dev/null 2>&1
"$GRAPH" serve "$WORK/new" --name one >/dev/null 2>&1
"$GRAPH" serve "$WORK/empty" --name two >/dev/null 2>&1

check_status "refuses without a name" 1 "$GRAPH" unserve
out=$("$GRAPH" unserve nosuch 2>&1)
check_contains "refuses a name that is not served" "$out" "is not served"
check_contains "changed nothing" "$(grep -c '^\[one\]' /etc/samba/smb.conf)" "1"

"$GRAPH" unserve one >/dev/null 2>&1
check_contains "removes the named share" \
	"$(grep -c '^\[one\]' /etc/samba/smb.conf)" "0"
check_contains "keeps the other share" \
	"$(grep -c '^\[two\]' /etc/samba/smb.conf)" "1"
check_contains "keeps the address" \
	"$(grep 'interfaces =' /etc/samba/smb.conf)" "127.0.0.1"
check_status "the configuration is still valid" 0 testparm -s /etc/samba/smb.conf

# unserve withdraws a share, never the repository behind it.
check "left the repository alone" \
	"$([ -f "$WORK/new/.graph/repository" ] && echo y || echo n)" "y"

# A released name can be used again, which nothing could do before.
"$GRAPH" serve "$WORK/new" --name one >/dev/null 2>&1
check_contains "the name can be reused" \
	"$(grep -c '^\[one\]' /etc/samba/smb.conf)" "1"
# ...and it may point somewhere new, since the old share is gone.
"$GRAPH" unserve one >/dev/null 2>&1
"$GRAPH" serve "$WORK/secure" --name one >/dev/null 2>&1
check_contains "a released name may point elsewhere" \
	"$(sed -n '/^\[one\]/,/^\[/p' /etc/samba/smb.conf)" "$WORK/secure"

# Removing the last share stops the service rather than leaving it listening
# on an empty configuration.
"$GRAPH" unserve one >/dev/null 2>&1
out=$("$GRAPH" unserve two 2>&1)
check_contains "reports the last share going" "$out" "no longer served"
# Without systemd the stop fails, and that must not hide the removal that
# already succeeded.
check_contains "still reports the removal when the stop fails" "$out" \
	"two no longer served"
check_contains "says the service is still running" "$out" \
	"still running"
check_contains "no shares remain" \
	"$(testparm -s /etc/samba/smb.conf 2>/dev/null | grep -c '^\[' )" "1"
check_contains "the address survives for the next serve" \
	"$(grep 'interfaces =' /etc/samba/smb.conf)" "127.0.0.1"

# --- graph connect / disconnect -------------------------------------------
# The whole point of serve, exercised from a client: a real SMB mount, a real
# write through it, and a real release. This is why the container runs
# privileged -- mounting needs kernel privileges an ordinary one lacks.

group "graph connect"

check_status "refuses without a destination" 1 "$GRAPH" connect 127.0.0.1 opsys
check_status "refuses without a name" 1 "$GRAPH" connect 127.0.0.1 --to=/mnt/g
out=$("$GRAPH" connect '1.2.3.4; id' share --to=/mnt/g 2>&1)
check_contains "refuses an unusable address" "$out" "not a usable address"
out=$("$GRAPH" connect 127.0.0.1 'a b;id' --to=/mnt/g 2>&1)
check_contains "refuses an unusable share name" "$out" "not a usable share name"

# The destination is the user's choice and is never invented by graph.
out=$("$GRAPH" connect 127.0.0.1 opsys --to="$WORK/nowhere" 2>&1)
check_contains "refuses a destination that does not exist" "$out" "does not exist"
check "did not create it" \
	"$([ -e "$WORK/nowhere" ] && echo y || echo n)" "n"

: > "$WORK/afile2"
out=$("$GRAPH" connect 127.0.0.1 opsys --to="$WORK/afile2" 2>&1)
check_contains "refuses a destination that is not a directory" "$out" \
	"is not a directory"

# Stand up a repository that the SMB user can actually write to.
rm -f /etc/samba/smb.conf /etc/samba/smb.conf.pre-graph
"$GRAPH" config --address 127.0.0.1 >/dev/null 2>&1
"$GRAPH" init "$WORK/shared" >/dev/null 2>&1
adduser --disabled-password --gecos "" client >/dev/null 2>&1
chown -R client:client "$WORK/shared"
printf 'client-pw-1\nclient-pw-1\n' | smbpasswd -s -a client >/dev/null 2>&1
"$GRAPH" serve "$WORK/shared" --name shared --user client >/dev/null 2>&1
start_smbd || fail "smbd came up" "no answer on 127.0.0.1"

mkdir -p "$WORK/mnt"
out=$(printf 'client\nclient-pw-1\n' | "$GRAPH" connect 127.0.0.1 shared --to="$WORK/mnt" 2>&1)
check_contains "connects the repository" "$out" "connected at"

# The kernel is asked what is mounted, not a mount table.
case "$(mount | grep "$WORK/mnt")" in
*cifs*) ok "the destination is a real SMB mount" ;;
*) fail "the destination is a real SMB mount" "$(mount | grep "$WORK/mnt")" ;;
esac
check "the repository is visible through it" \
	"$([ -f "$WORK/mnt/.graph/repository" ] && echo y || echo n)" "y"

# A repository reached over SMB must stay writable with ordinary tools.
if echo written-over-smb > "$WORK/mnt/note.md" 2>/dev/null; then
	ok "a file can be written through the mount"
	check "it landed in the served repository" \
		"$(cat "$WORK/shared/note.md" 2>/dev/null)" "written-over-smb"
else
	fail "a file can be written through the mount" "$(ls -ld "$WORK/shared")"
fi

# Connecting twice to the same place is refused rather than stacked.
out=$(printf 'client\nclient-pw-1\n' | "$GRAPH" connect 127.0.0.1 shared --to="$WORK/mnt" 2>&1)
check_contains "refuses a destination already connected" "$out" "already has"

group "graph disconnect"

check_status "refuses without a destination" 1 "$GRAPH" disconnect
# An ordinary directory must never be unmounted by mistake.
out=$("$GRAPH" disconnect "$WORK/new" 2>&1)
check_contains "refuses a directory that is not connected" "$out" \
	"is not a connected repository"

out=$("$GRAPH" disconnect "$WORK/mnt" 2>&1)
check_contains "releases the repository" "$out" "released"
case "$(mount | grep "$WORK/mnt")" in
*cifs*) fail "the mount is gone" "still mounted" ;;
*) ok "the mount is gone" ;;
esac
# Releasing withdraws the mount, never the data behind it.
check "the served repository still has the file" \
	"$(cat "$WORK/shared/note.md" 2>/dev/null)" "written-over-smb"
out=$("$GRAPH" disconnect "$WORK/mnt" 2>&1)
check_contains "refuses to release it twice" "$out" "is not a connected"
# smbd stays up through the status group. The kernel's SMB client keeps its
# session to 127.0.0.1 across mounts, and a server restarted underneath it
# makes the next mount fail with "host is down" until it notices.

# --- graph status ---------------------------------------------------------
# Read-only, and needs no root: the configuration is readable and the kernel
# is asked what is mounted.

group "graph status"

check_status "accepts no arguments" 0 "$GRAPH" status
check_status "refuses an argument" 1 "$GRAPH" status extra

rm -f /etc/samba/smb.conf /etc/samba/smb.conf.pre-graph
out=$("$GRAPH" status 2>&1)
check_contains "reports nothing configured" "$out" "nothing configured"
check_contains "reports nothing connected" "$out" "no repositories connected"

"$GRAPH" config --address 127.0.0.1 >/dev/null 2>&1
out=$("$GRAPH" status 2>&1)
check_contains "reports the configured address" "$out" "127.0.0.1"
check_contains "reports nothing served yet" "$out" "no repositories served"
# No systemd here, so the service can never be running.
check_contains "reports the service state" "$out" "stopped"

"$GRAPH" serve "$WORK/new" --name shown --user "" >/dev/null 2>&1
"$GRAPH" serve "$WORK/new" --name shown >/dev/null 2>&1
out=$("$GRAPH" status 2>&1)
check_contains "names the served repository" "$out" "shown"
check_contains "gives its path" "$out" "$WORK/new"

# The filesystem is the truth: a path that has stopped being a repository is
# reported as such rather than assumed still good.
mv "$WORK/new/.graph" "$WORK/new/.graph-hidden"
out=$("$GRAPH" status 2>&1)
check_contains "flags a path that is no longer a repository" "$out" \
	"no longer a repository"
mv "$WORK/new/.graph-hidden" "$WORK/new/.graph"
out=$("$GRAPH" status 2>&1)
case "$out" in
*"no longer a repository"*) fail "stops flagging it once restored" "$out" ;;
*) ok "stops flagging it once restored" ;;
esac

# It must see a real mount, which is why this runs in a privileged container.
"$GRAPH" config --address 127.0.0.1 >/dev/null 2>&1
"$GRAPH" init "$WORK/statrepo" >/dev/null 2>&1
adduser --disabled-password --gecos "" statuser >/dev/null 2>&1
chown -R statuser:statuser "$WORK/statrepo"
printf 'stat-pw-1\nstat-pw-1\n' | smbpasswd -s -a statuser >/dev/null 2>&1
"$GRAPH" serve "$WORK/statrepo" --name statshare --user statuser >/dev/null 2>&1
# The smbd from the connect group picks the new share up from the rewritten
# configuration on the next connection; it is only checked to be answering.
wait_smbd || fail "smbd is answering" "no answer on 127.0.0.1"
mkdir -p "$WORK/statmnt"
out=$(printf 'statuser\nstat-pw-1\n' | "$GRAPH" connect 127.0.0.1 statshare \
	--to="$WORK/statmnt" 2>&1)
case "$out" in
*"connected at"*) ok "connects a repository to report" ;;
*) fail "connects a repository to report" \
	"$out; kernel: $(dmesg 2>/dev/null | grep -i cifs | tail -3)" ;;
esac
out=$("$GRAPH" status 2>&1)
check_contains "reports a connected repository" "$out" "$WORK/statmnt"
check_contains "names what it is connected to" "$out" "//127.0.0.1/statshare"
"$GRAPH" disconnect "$WORK/statmnt" >/dev/null 2>&1
out=$("$GRAPH" status 2>&1)
check_contains "stops reporting it once released" "$out" \
	"no repositories connected"
pkill smbd 2>/dev/null

# --- samba environment -----------------------------------------------------
# Proves the harness can start smbd against a configuration and see the share
# it defines. Until serve generates one, this configuration is written by hand.

group "samba environment"

check_status "samba is installed" 0 command -v smbd

cat > /etc/samba/smb.conf <<EOF
[global]
	interfaces = 127.0.0.1
	bind interfaces only = yes
	server min protocol = SMB2
	map to guest = never
	load printers = no
	printcap name = /dev/null
	disable spoolss = yes
	usershare max shares = 0

[probe]
	path = $WORK/new
	browseable = yes
	read only = yes
	follow symlinks = no
EOF

check_status "testparm accepts the configuration" 0 testparm -s /etc/samba/smb.conf

start_smbd || fail "smbd came up" "no answer on 127.0.0.1"

shares=$(smbclient -N -L 127.0.0.1 2>&1)
check_contains "smbd serves the configured share" "$shares" "probe"

# testparm must reject a broken configuration, or validating before install
# proves nothing.
printf '[global]\n\tinterfaces = 127.0.0.1\n[bad\n' > "$WORK/broken.conf"
check_status "testparm rejects a malformed configuration" 1 \
	testparm -s "$WORK/broken.conf"

pkill smbd 2>/dev/null

summary
