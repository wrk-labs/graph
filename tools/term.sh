#!/bin/sh
# Build the shell tab's page: shell/term.html with the vendored xterm.js
# inlined where the @vendor marker is, embedded the way ui.html is so the
# shells stay self-contained.
#
# usage: term.sh > shell/term.h
set -e
tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT
{
	sed '/<!-- @vendor -->/,$d' shell/term.html
	printf '<style>\n'
	cat vendor/xterm/xterm.css
	printf '</style>\n<script>\n'
	cat vendor/xterm/xterm.js
	printf '\n</script>\n<script>\n'
	cat vendor/xterm/addon-fit.js
	printf '\n</script>\n'
	sed '1,/<!-- @vendor -->/d' shell/term.html
} > "$tmp"
sh tools/embed.sh "$tmp" term_html | sed "1s|$tmp|shell/term.html + vendor/xterm|"
