#!/bin/sh
# Embed a text file as a NUL-terminated byte array so the binary stays
# self-contained. A string literal would work too, but C99 only guarantees
# 4095 characters and -pedantic says so.
#
# usage: embed.sh <file> <symbol>
set -e
printf '/* generated from %s by tools/embed.sh — do not edit */\n' "$1"
printf 'static const char %s[] = {\n' "$2"
od -An -v -tu1 "$1" | sed \
	-e 's/^[[:space:]]*//' \
	-e 's/[[:space:]]*$//' \
	-e '/^$/d' \
	-e 's/[[:space:]]\{1,\}/,/g' \
	-e 's/$/,/'
printf '0};\n'
