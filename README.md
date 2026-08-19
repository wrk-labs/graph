# Graph

Graph turns a directory into a repository for the things you keep — documents,
records, notes, media, whatever else accumulates — and serves it over SMB so you
can reach it from any machine you own.

The repository is a plain directory tree. Open it in a file manager, edit it in
your editor, back it up with the tools you already use, or read it on a machine
that has never heard of Graph.

## Install

macOS:

    brew install wrk-labs/tap/graph

Debian and Ubuntu:

    apt install graph

## Usage

Create a repository:

    graph init /srv/graph

Set the address the server listens on:

    graph config --address 192.168.0.102

Share it:

    graph serve /srv/graph --name opsys --user will

Mount it from another machine:

    graph connect 192.168.0.102 opsys --to=/home/opsys

Release it when you are done:

    graph disconnect /home/opsys

Look at it:

    graph display /srv/graph

This opens the repository in a window of its own — notes, files and the links
between them — and edits go straight back to the files. Each repository you
open is a tab; run it again with another path, or use the + in the tab bar.
Without a path it opens the repository you are in, or the ones you had open
last. On macOS the window is Graph, installed with `graph` and there in
Spotlight and the Dock like any other application. With `--no-open`, or where
no window is available, `graph display` serves the same view to your browser
at `127.0.0.1:7373`.

## Layout

`graph init` starts you off with:

    Graph/
    ├── inbox/
    ├── people/
    ├── organizations/
    ├── finance/
    ├── research/
    ├── knowledge/
    └── archive/

Rearrange it however suits you. Nest projects under an organization, scope
research to a client, create your own directories. Graph looks for its marker in
`.graph/` and leaves the rest of the tree to you.

## Linking

Markdown files can point at each other by path:

    See [[people/mike]] and [[organizations/acme/contract.pdf]].

Paths start at the repository root. `graph display` follows these links and
draws them; in the files themselves they are ordinary text.

## Requirements

Serving needs Samba and runs on Linux. Connecting works on Linux and macOS.
Display works on both; the window uses WebKit on macOS and WebKitGTK on
Linux, and falls back to your browser where that is not available.

## Status

Early, and under active development.
