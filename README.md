# Graph

Graph is a personal knowledge hub. It turns a directory into a repository for
the things you keep — people, organizations, finances, research, notes, media,
whatever accumulates — shows it as one connected whole, and serves it over SMB
so you can reach it from any machine you own.

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

Stop sharing it:

    graph unserve opsys

See what is served and what is connected:

    graph status

Look at it:

    graph display /srv/graph

This opens the repository in a window of its own — notes, files and the links
between them — and edits go straight back to the files. Each repository you
open is a tab; run it again with another path, or use the + in the tab bar,
and switch between them with ⌘1…9 (Ctrl+1…9 on Linux).
Without a path it opens the repository you are in, or the ones you had open
last. On macOS the window is Graph, installed with `graph` and there in
Spotlight and the Dock like any other application. With `--no-open`, or where
no window is available, `graph display` serves the same view to your browser
at `127.0.0.1:7373`, and prints the address to open — it carries a key made
for that run, so nothing else on the machine can read or change your notes
through it.

## Layout

`graph init` starts you off with:

    Graph/
    ├── inbox/
    ├── people/
    ├── organizations/
    ├── finance/
    ├── research/
    ├── knowledge/
    ├── archive/
    └── journal/
        └── agents/

along with an `AGENTS.md` at the root and a `README.md` in each directory.
They say what belongs where and how entries are shaped, named and linked —
for you, and for any agent you point at the repository. They are ordinary
files: edit them as the tree becomes yours, or delete them.

Rearrange it however suits you. Nest projects under an organization, scope
research to a client, create your own directories. Graph looks for its marker in
`.graph/` and leaves the rest of the tree to you.

## Linking

Markdown files can point at each other by path:

    See [[people/mike]] and [[organizations/acme/contract.pdf]].

Paths start at the repository root. `graph display` follows these links and
draws them; in the files themselves they are ordinary text.

## Requirements

Serving needs Samba, along with its VFS modules (`samba-vfs-modules` on Debian
and Ubuntu), and runs on Linux. Connecting needs SMB mount support — `cifs-utils`
on Linux, built in on macOS — and works on both. Display works on both; the
window uses WebKit on macOS and WebKitGTK on Linux, and falls back to your
browser where that is not available.

Graph generates the whole Samba configuration and assumes the machine is not
already serving SMB for something else. The configuration in place before Graph
first ran is kept at `/etc/samba/smb.conf.pre-graph`.

Whether the SMB service starts at boot is left to the init system. Disable it
(`systemctl disable smbd`) and `graph serve` will start it when you ask for a
repository; `graph unserve` stops it again once nothing is left to serve.

## Status

Early, and under active development.
