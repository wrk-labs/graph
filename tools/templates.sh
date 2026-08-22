#!/bin/sh
# Embed the files `graph init` writes into a repository as a table of
# (path, bytes, length), one entry per file under templates/, so the binary
# stays self-contained. Paths are relative to templates/ and become paths
# relative to the repository root.
#
# usage: templates.sh <templates dir>
set -e
dir=$1
printf '/* generated from %s by tools/templates.sh — do not edit */\n' "$dir"
i=0
(cd "$dir" && find . -type f | sed 's|^\./||' | LC_ALL=C sort) |
while read -r f; do
	sh "$(dirname "$0")/embed.sh" "$dir/$f" "tpl_$i"
	i=$((i + 1))
done
printf 'static const struct template {\n\tconst char *path;\n'
printf '\tconst unsigned char *data;\n\tsize_t len;\n} templates[] = {\n'
i=0
(cd "$dir" && find . -type f | sed 's|^\./||' | LC_ALL=C sort) |
while read -r f; do
	printf '\t{ "%s", tpl_%d, sizeof(tpl_%d) - 1 },\n' "$f" "$i" "$i"
	i=$((i + 1))
done
printf '};\n'
