# Working in this repository

This directory is a Graph repository: a personal knowledge hub kept as an
ordinary directory tree. The files are the source of truth; everything Graph
keeps about the structure of those files lives in `.graph/`. Read this file
before adding, changing or moving anything.

## Layout

    inbox/            things that arrived but have not been placed yet
    people/           one directory per person
    organizations/    one directory per company, institution or group
    finance/          accounts, records, statements, taxes
    research/         what is being investigated right now
    knowledge/        what has been understood and distilled, kept for reuse
    archive/          inactive material that is retained
    journal/          the owner's record, by time
    journal/agents/   your record: what each agent did, one file per day
    .graph/           Graph's own: its marker, and the memory if enabled

Each directory has a `README.md` explaining what belongs in it and how entries
are shaped. Read the README of a directory before adding to it. The owner may
have renamed, nested or added directories; the README of whatever exists is
the authority, not this list.

## Entities

A person, an organization, a project, an account — anything worth talking
about — is a directory with a `README.md` inside it:

    people/
    └── alice-martin/
        ├── README.md
        ├── 2026-03-12-call.md
        └── contract.pdf

The README is the front page: who or what this is, why it is here, the
current state, and links to the important files. Files live beside it, with
the context that gives them meaning. Do not organize by file type — there is
no `documents/` or `pdfs/` — a contract belongs with the organization it was
signed with.

Names are lowercase, words separated by hyphens, no spaces: `alice-martin`,
`acme-corp`, `us-visa`. Dates in names and notes are ISO, `YYYY-MM-DD`, so
they sort.

## Links

Markdown files refer to each other by path from the repository root:

    [[people/alice-martin]]
    [[organizations/acme-corp/contract.pdf]]
    [[knowledge/networking/smb]]

A link may point at a file or a directory; `.md` may be left off. A leading
`./` makes the path relative to the current file. Link whenever one thing
bears on another — the links are how the tree becomes a graph.

Links are plain paths with no lookup behind them, so moving or renaming a
file breaks every link that points at it. Do not move or rename anything
unless asked to, and when you do, update the links that pointed at it.

## Rules

- Read before writing. Look for an existing entity before creating a new one;
  extend its README rather than starting a parallel note.
- Not sure where something goes? Put it in `inbox/` and say so. Guessing a
  place is worse than leaving it to be filed.
- Do not reorganize. The structure is the owner's. Suggest changes; do not
  make them unasked.
- Keep derived things — indexes, caches, embeddings, extracted text — outside
  the tree, rebuilt from the files. The files are the truth; `.graph/` is
  Graph's to maintain.
- Record where information came from. A note that says when and from what it
  was written is worth more than one that does not.
- Prefer plain text. Markdown for notes, the original format for documents.
  Do not convert files to other formats unless asked.
- Everything here is private to its owner. Do not copy it elsewhere, quote it
  in places outside the repository, or send it to services not asked for.

## Journal

Keep a journal as you work, in `journal/agents/YYYY-MM-DD-<your-name>.md`.
Open a timed section when a session starts, with what was asked; add to it as
you go — each file created or changed, by link, as you touch it; each decision
and why, as you make it; anything left open. Write as things happen, so the
journal is current while you are working and complete if you stop early.
Append to the day's file; earlier entries stay as they were. The journal is
how any change in the repository is traced back to the session that made it.
`journal/` itself is the owner's; write there only when asked.

## Memory

This repository may carry a memory of its owner: how they decide, rather than
what the files hold. It is served by an MCP server called `self`, and whether
it is switched on is the owner's choice — they turn it on with `graph enable
mcp` and off with `graph disable mcp`. If you have `self` tools, it is there;
if you do not, it is not, and nothing in this repository depends on it.

Where it is there, query it before advising, filing, or making a judgment call
on the owner's behalf: `search_nodes` with the domain you are about to act in,
`open_nodes` for an exact name, `read_graph` when you need the whole picture —
it is small enough to read.

Entities are domains of judgment rather than facts — `Filing`, `Privacy`,
`Money`, `Communication` — and each observation is one policy, written as
when, then and why:

    2026-08-12 (stated): a contract goes under the organization it was
    signed with, never under the signatory — the contract outlives the
    contact.

Two conventions carry the weight. Date every observation: it is how the owner
reads their own drift later, and how you tell a rule that still holds from one
they have outgrown. And mark `(stated)` or `(inferred)` — something they told
you is not something you concluded about them. Write an inference as the
observation that prompted it, never as a trait, and expect to be wrong.

Keep it small. When something new contradicts what is recorded, that is the
most useful moment there is: say so, and revise the observation rather than
adding a second one beside it. A memory that only ever grows stops being
judgment and becomes a pile.

Never copy it out of the repository. It generalizes about a person, which
makes it more revealing than the documents it describes.

## Finding things

Walk the tree, read the READMEs, follow the links. A directory's README is
the best summary of what it holds; start there and go only as deep as the
question needs. Whatever Graph knows about the structure is in `.graph/`.
