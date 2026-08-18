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

Paths start at the repository root. Graph fixes the convention now so a later
tool can follow these links; for the moment they sit in your files as ordinary
text.

## Requirements

Serving needs Samba and runs on Linux. Connecting works on Linux and macOS.

## Status

Early, and under active development.
