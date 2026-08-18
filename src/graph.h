/* See LICENSE file for copyright and license details. */
/* graph — filesystem repository over SMB */

#ifndef GRAPH_H
#define GRAPH_H

#include <stddef.h>
#include <sys/types.h>

/* Directory holding Graph's own repository metadata. */
#define GRAPH_DIR ".graph"

/* Marker file identifying a directory as a Graph repository. Its presence is
 * what `graph serve` validates against; the format stays minimal on purpose. */
#define GRAPH_MARKER "repository"

/* Repository format version written to the marker. */
#define GRAPH_FORMAT 1

/* commands */
int cmd_init(int argc, char *argv[]);
int cmd_display(int argc, char *argv[]);

/* util.c */
void die(const char *fmt, ...);
void warn(const char *fmt, ...);
int join_path(char *dst, size_t size, const char *base, const char *name);
int dir_is_empty(const char *path);
int is_graph_repo(const char *path);
int mkdir_p(const char *path, mode_t mode);

#endif /* GRAPH_H */
