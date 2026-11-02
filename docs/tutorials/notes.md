# Notes: Markdown notes and a knowledge base

`notes` works on a folder of Markdown files the way Obsidian does, without a window: it creates and edits notes, fills in templates, keeps daily notes, reads and writes YAML frontmatter, lists and renames tags, searches everything with ranked results and snippets, follows `[[wikilinks]]` both ways (and rewrites them when you rename or move a note), finds unresolved links and orphans, collects `- [ ]` tasks with due dates from every note, draws the link graph, and publishes notes as HTML, PDF or a whole static website.

The folder is the source of truth. You can keep editing it in Obsidian, VS Code, vim or a phone app at the same time: every command checks the files' sizes and modification times first and re-reads what changed. What `notes` learns from the files (links, tags, headings, tasks, search terms) is cached in a hidden `.wwnotes/` folder inside the vault; delete it whenever you like and it is rebuilt (an old `.achnotes/` cache from before the rename is replaced the same way).

There is no GUI yet; the GUI that comes later will call these same commands. Each task below is shown three ways:

- **CLI**: single `wetwaresuite notes.*` commands. The vault lives on disk, so (unlike photo documents) one-shot commands build on each other.
- **Batch**: a script for multi-step work with `-> name` captures and `$name` reuse (see [headless and batch](headless-and-batch.md)). Batch scripts and MCP sessions can also `undo`.
- **Claude**: a prompt for Claude over MCP and the tools it calls (see [mcp.md](mcp.md)). `wetwaresuite mcp --tools notes` serves just this tool.

Every example was run against the real binary on the sample vault below; outputs are copied from those runs (the vault was at `/home/me/Notes`; long outputs are shortened where marked).

## How notes works

**Vaults.** A vault is any folder of `.md` files, nested as deep as you like. Files that aren't Markdown (images, PDFs, audio) are *attachments*: they can be linked and embedded. Hidden folders (`.obsidian`, `.trash`, `.git`, `.wwnotes`) and `node_modules` are skipped.

**Which vault a command uses.** Every command takes `vault=`: either an id from `notes.vault.open` (`vault1`) or a folder path, which is opened on the spot. Without it, a command uses the vault used last in this session, else looks for a folder holding `.obsidian/` or `.wwnotes/` in the current folder and its parents, the way git finds a repository. So the easy way to work is to `cd` into the vault; scripts and Claude can pass `vault=` or open it first.

**Naming notes.** Wherever a command takes `note=`, you can give a path (`Projects/Garden Redesign.md`, with or without `.md`), a bare name (`Garden Redesign`), a frontmatter title or an alias. Names are resolved like Obsidian resolves `[[links]]`: exact path first, then a path relative to the linking note, then the file name anywhere in the vault (preferring the same folder, then the shortest path), then aliases. Case doesn't matter.

**Obsidian compatibility.** Links are `[[Note]]`, `[[Note#Heading]]`, `[[Note#^block-id]]`, `[[Note|shown text]]`, embeds are `![[Note]]`, `![[image.png|300]]`, and ordinary `[text](relative/path.md)` links count too. Tags are inline `#tags` (nested like `#project/garden`) and frontmatter `tags:`. Aliases come from frontmatter `aliases:`. Daily note and template settings are read from (and written to) Obsidian's own `.obsidian/daily-notes.json` and `.obsidian/templates.json`. Deleted notes go to `.trash/` like Obsidian's "move to Obsidian trash". Code blocks, inline code and `%% comments %%` are ignored when finding links, tags and tasks.

**Undo.** Every command that changes files (create, write, append, rename, move, delete, frontmatter, tags, tasks, daily, templates, trash) records the exact bytes of every file it touched, so `undo` in a batch script or MCP session puts them all back, including notes whose links a rename rewrote, and removes folders it created. Exports (render, site, graph files) are not undoable; they only write where you point them.

## The sample vault

```
/home/me/Notes
├── .obsidian/daily-notes.json     {"folder": "Journal", "format": "YYYY-MM-DD", "template": "Templates/Daily"}
├── .obsidian/templates.json       {"folder": "Templates"}
├── Home.md                        title Home, aliases Start and Dashboard, #hub, links to everything
├── Books to Read.md               links to two books that have no notes yet
├── Inbox.md
├── Attachments/garden-sketch.png
├── Journal/2026-10-15.md
├── People/Priya.md, Sam Lee.md    tagged person
├── Projects/Garden Redesign.md    frontmatter status/tags/due, a table, tasks, ![[garden-sketch.png|400]]
├── Projects/Website Relaunch.md   tasks with due dates, a > [!warning] callout
└── Templates/Daily.md, Meeting.md, Person.md
```

`Projects/Garden Redesign.md` looks like this:

```markdown
---
status: active
tags: [project]
due: 2026-11-30
---
# Garden Redesign

Plan the new beds and the budget for spring. #project/garden

![[garden-sketch.png|400]]

## Budget
| Item | Cost |
|------|-----:|
| Soil | 120 |
| Plants | 340 |

## Tasks
- [ ] Order soil 📅 2026-10-20
- [ ] Ask [[Sam Lee]] about the irrigation timer
- [x] Measure the beds

## Notes
Sam suggested raised beds. See [[Website Relaunch#Timeline]] for the photo shoot date.
```

## Task 1: open a vault and look around

**CLI.** From the vault folder (it has `.obsidian/`, so no `vault=` is needed):

```sh
$ cd ~/Notes
$ wetwaresuite notes.vault.info
/home/me/Notes
  11 notes, 1 attachment, 4 folders, 249 words
  13 links (2 unresolved), 5 tags
  tasks: 6 open, 1 done
  most linked: Projects/Website Relaunch (3), People/Priya (2), People/Sam Lee (2), Projects/Garden Redesign (2), Books to Read (1)
  daily notes: Journal/, format YYYY-MM-DD, template Templates/Daily
  templates: Templates/
path: /home/me/Notes
notes: 11
attachments: 1
...
$ wetwaresuite notes.list sort=-words limit=5
11 notes, showing 5
Projects/Garden Redesign.md  70 words  2026-10-16 08:58 #project #project/garden
Projects/Website Relaunch.md  61 words  2026-10-16 08:58 #project #work
Home.md  28 words  2026-10-16 08:58 #hub
Journal/2026-10-15.md  25 words  2026-10-16 08:58
Books to Read.md  15 words  2026-10-16 08:58
count: 5
total: 11
```

Read a note by its alias, one section of a note, or its outline:

```sh
$ wetwaresuite notes.read note=Dashboard
---
title: Home
aliases: [Start, Dashboard]
tags: [hub]
---
# Home

Current work: [[Garden Redesign]] and [[Website Relaunch]].
People: [[Sam Lee]], [[Priya]].
Reading list lives in [[Books to Read]].

- [ ] Renew the domain 📅 2026-10-12

note: Home.md
title: Home
tags: #hub
words: 28
$ wetwaresuite notes.read note="Garden Redesign" heading=Budget
## Budget
| Item | Cost |
|------|-----:|
| Soil | 120 |
| Plants | 340 |
...
$ wetwaresuite notes.outline note="Garden Redesign"
Projects/Garden Redesign.md: 4 headings
# Garden Redesign  (line 6)
  ## Budget  (line 12)
  ## Tasks  (line 18)
  ## Notes  (line 23)
```

`notes.read` also takes `frontmatter=false` (body only) and `lines=10-20`. From anywhere else, add `vault=~/Notes` to any of these.

**Batch.** `notes.vault.open` gives the vault an id you can capture:

```
notes.vault.open path=/home/me/Notes -> v
notes.vault.info vault=$v
notes.list vault=$v folder=Projects
```

**Claude.** "Open my vault at ~/Notes and tell me what's in it: biggest notes, what links to what, anything broken." Claude calls `notes_vault_open` (with `preview=true` it also sees a picture of the link graph), `notes_vault_info`, `notes_list` and `notes_links_unresolved`.

## Task 2: capture: new notes, templates, daily notes, quick appends

**Templates.** Templates are notes in the templates folder. `{{title}}` is the new note's name, `{{date}}` and `{{time}}` use the vault's formats (`YYYY-MM-DD` and `HH:mm` unless `.obsidian/templates.json` says otherwise), `{{date:dddd D MMMM YYYY}}` takes any moment.js format (as Obsidian does), `{{yesterday}}` and `{{tomorrow}}` work too, and `vars=` fills any other `{{name}}`:

```sh
$ wetwaresuite notes.templates.list
3 templates in Templates/
Templates/Daily.md  uses {{date}} {{date:dddd D MMMM YYYY}}
Templates/Meeting.md  uses {{attendees}} {{date}} {{title}}
Templates/Person.md  uses {{title}}
$ wetwaresuite notes.templates.render template=Person title="Jo Park"
---
tags: [person]
---
# Jo Park
```

`notes.templates.render` writes nothing; it shows what you'd get.

**CLI.** Create a meeting note from a template and fill a section:

```sh
$ wetwaresuite notes.create note="Meetings/Website kickoff" template=Meeting vars="attendees=Priya, Sam" today="2026-10-16 10:00"
created Meetings/Website kickoff.md
Meetings/Website kickoff.md
note: Meetings/Website kickoff.md
path: /home/me/Notes/Meetings/Website kickoff.md
bytes: 117
$ wetwaresuite notes.append note="Website kickoff" heading=Decisions content="- Launch moves to 2026-11-15"
added to Meetings/Website kickoff.md under Decisions
$ cat "Meetings/Website kickoff.md"
---
type: meeting
date: 2026-10-16
---
# Website kickoff

Attendees: Priya, Sam

## Agenda

## Decisions

- Launch moves to 2026-11-15

## Actions
```

(`today=` pins the date for the example; normally leave it out.) The new folder `Meetings/` is created. `notes.append` adds to the end of the note, or with `heading=` to the end of that heading's section (`prepend=true` puts it right under the heading); a heading that doesn't exist is added. List items join the list above them. Without `create=false`, appending to a note that doesn't exist creates it, which makes an inbox one command:

```sh
$ wetwaresuite notes.append note=Inbox content="- idea: rain barrel"
added to Inbox.md
```

`content=` turns `\n` into line breaks when the text has none of its own, so `content="line one\nline two"` works from a shell; `content_file=draft.md` reads the text from a file. `notes.write` replaces a note's whole text (`keep_frontmatter=true` keeps its properties).

**Daily notes** follow the vault's daily note settings (folder, file name format, template):

```sh
$ wetwaresuite notes.daily today=2026-10-16 append="- 09:30 call with Priya about copy" heading=Log
created Journal/2026-10-16.md
---
created: 2026-10-16
---
# Friday 16 October 2026

## Log

- 09:30 call with Priya about copy

## Tasks

Journal/2026-10-16.md
note: Journal/2026-10-16.md
created: true
date: 2026-10-16
$ wetwaresuite notes.daily date=yesterday today=2026-10-16
Journal/2026-10-15.md
...
created: false
```

`date=` takes `2026-10-16`, `today`, `yesterday`, `tomorrow`, `+3` or `-7`; `{{date}}` in the template is the note's own date. `folder=`, `format=` (e.g. `YYYY/MM/YYYY-MM-DD`) and `template=` override the settings for one call; `notes.vault.configure daily_folder=Journal daily_template=Templates/Daily` changes them for good (in `.obsidian/`, so Obsidian agrees).

**Batch.** `meeting.txt` makes a meeting note, fills it in and links it from today's journal:

```
# meeting.txt: a meeting note from a template, filled in, linked from today's journal
notes.vault.open path=. -> v
notes.create note="Meetings/Garden walk" template=Meeting vars="attendees=Sam Lee" -> m
notes.append note=$m heading=Decisions content="- Raised beds along the south fence"
notes.append note=$m heading=Actions content="- [ ] Get a quote for timber 📅 2026-10-23"
notes.daily append="- Garden walk with [[Sam Lee]], see [[Garden walk]]" heading=Log -> d
notes.backlinks note=$m
```

```sh
$ wetwaresuite batch meeting.txt
opened vault1: /home/me/Notes (13 notes, 1 attachment, 7 open tasks)
...
created Meetings/Garden walk.md
...
Journal/2026-10-16.md
---
created: 2026-10-16
---
# Friday 16 October 2026

## Log

- 09:30 call with Priya about copy
- Garden walk with [[Sam Lee]], see [[Garden walk]]

## Tasks

- [ ] Buy seed trays 📅 2026-10-19
...
1 link to Meetings/Garden walk.md from 1 note
Journal/2026-10-16.md:9  - Garden walk with [[Sam Lee]], see [[Garden walk]]
```

`-> m` captures the new note's path, so `$m` works in every later line.

**Claude.** "Start today's daily note and log that I called Priya about the copy at 9:30." Claude calls `notes_daily` with `append` and `heading=Log`. "Make a meeting note for the website kickoff with Priya and Sam" calls `notes_templates_list`, then `notes_create` with `template=Meeting` and `vars`.

## Task 3: find things

`notes.search` is a full-text search over every note, ranked (BM25, with extra weight for words in the title, file name and aliases), with a snippet per note that bolds the matches and gives its line.

| Query | Means |
|---|---|
| `budget garden` | both words (any order) |
| `"raised beds"` | that exact phrase |
| `-hosting`, `-"photo shoot"` | not this word or phrase |
| `irrig*` | words starting with irrig |
| `tag:project`, `#project` | notes tagged project (nested tags such as project/garden included) |
| `path:Projects/` | notes whose path contains this (a folder, or part of a name) |
| `file:garden` | notes whose file name contains this |
| `-tag:work`, `-path:Archive` | any filter can be negated |

**CLI.**

```sh
$ wetwaresuite notes.search query=budget
2 matches
Projects/Website Relaunch.md  (1.58)
    18: > [!warning] **Budget** > The hosting **budget** is capped at 20 a month.
Projects/Garden Redesign.md  (1.54)
    8: Plan the new beds and the **budget** for spring. #project/garden ![[garden-sketch.png|400]] ## **Budget** | Item | Cost | ...
count: 2
total: 2
notes: Projects/Website Relaunch.md
Projects/Garden Redesign.md
$ wetwaresuite notes.search query='"raised beds"'
1 match
Projects/Garden Redesign.md  (4.18)
    24: Sam suggested **raised beds**. See [[Website Relaunch#Timeline]] for the photo shoot date.
$ wetwaresuite notes.search query="budget -hosting"
1 match
Projects/Garden Redesign.md  (1.54)
...
$ wetwaresuite notes.search query="garden tag:project path:Projects/" snippets=false
2 matches
Projects/Garden Redesign.md  (3.71)
Projects/Website Relaunch.md  (0.45)
$ wetwaresuite notes.search query="irrig*" limit=3
2 matches
People/Sam Lee.md  (2.03)
    7: Neighbour, knows **irrigation**. Met through the [[Garden Redesign]].
Projects/Garden Redesign.md  (0.97)
    20: - [ ] Ask [[Sam Lee]] about the **irrigation** timer - [x] Measure the beds ...
```

A query of only filters lists the matching notes, newest first:

```sh
$ wetwaresuite notes.search query="tag:person"
3 matches
People/Priya.md
    4: # Priya Copy editor for the [[Website Relaunch]].
People/Sam Lee.md
    5: # Sam Lee Neighbour, knows irrigation. Met through the [[Garden Redesign]].
Templates/Person.md
    4: # {{title}}
```

The `notes` field holds the matching paths one per line, and `json=true` adds every hit (path, title, score, line, snippet) as JSON in `items`, for scripts.

**Batch.** Search results feed other commands; a capture takes the first field when there's no value, so use the paths in a shell loop or let Claude read the list:

```
notes.vault.open path=/home/me/Notes
notes.search query="tag:project -tag:done" json=true
```

**Claude.** "What did I write about the garden budget? Quote the lines." Claude calls `notes_search` with `query="garden budget"`, then `notes_read` with `heading=Budget` on the best hit.

## Task 4: links, backlinks, renaming and moving

**CLI.** Who links to a note, and what it links to:

```sh
$ wetwaresuite notes.backlinks note="Sam Lee" mentions=true
2 links to People/Sam Lee.md from 2 notes
Home.md:9  People: [[Sam Lee]], [[Priya]].
Projects/Garden Redesign.md:20  - [ ] Ask [[Sam Lee]] about the irrigation timer
unlinked mentions: 0
count: 2
notes: 2
mentions: 0
$ wetwaresuite notes.links note="Garden Redesign"
3 links in Projects/Garden Redesign.md
10: ![[garden-sketch.png|400]] -> Attachments/garden-sketch.png
20: [[Sam Lee]] -> People/Sam Lee.md
24: [[Website Relaunch#Timeline]] -> Projects/Website Relaunch.md
```

`mentions=true` also finds *unlinked mentions*: notes that use the note's name or an alias as text without linking it. Links that go nowhere, and notes nothing links to:

```sh
$ wetwaresuite notes.links.unresolved
2 unresolved targets
Deep Work  <- Books to Read.md
Designing Data-Intensive Applications  <- Books to Read.md
$ wetwaresuite notes.orphans
4 orphans
Inbox.md
Journal/2026-10-15.md
Journal/2026-10-16.md
Meetings/Website kickoff.md
```

(`notes.create note="Deep Work"` resolves the first one. Templates are left out of the orphan list unless `templates=true`; `incoming_only=true` lists notes nothing links to even if they link out.)

**Rename and move.** `notes.rename` rewrites every link to the note across the vault, keeping headings, block ids and display text (`[[Website Relaunch#Timeline]]` becomes `[[Website 2.0#Timeline]]`, `[[Projects/Beta|the beta]]` keeps its path style and alias, `[text](Beta.md)` gets the new relative path):

```sh
$ wetwaresuite notes.rename note="Website Relaunch" to="Website 2.0"
renamed Projects/Website Relaunch.md -> Projects/Website 2.0.md; updated 3 links in 3 notes
  Home.md
  People/Priya.md
  Projects/Garden Redesign.md
Projects/Website 2.0.md
note: Projects/Website 2.0.md
from: Projects/Website Relaunch.md
links_updated: 3
notes_changed: 3
$ wetwaresuite notes.links note="Garden Redesign"
...
24: [[Website 2.0#Timeline]] -> Projects/Website 2.0.md
```

A bare `to=` name keeps the folder; `to="Archive/Website 2.0"` moves it too. `notes.move note=Inbox folder=Archive` moves without renaming (and fixes relative Markdown links inside the moved note). Attachments can be renamed and moved the same way, and their embeds follow. When the new name is shared by another note, links are written with the path so they still point to the right one. `update_links=false` skips the rewriting.

**Delete and the trash.** Deleting moves the file into `.trash/`, keeping its folder path, and says how many links now go nowhere:

```sh
$ wetwaresuite notes.move note=Inbox folder=Archive
moved Inbox.md -> Archive/Inbox.md
$ wetwaresuite notes.delete note=Archive/Inbox
moved Archive/Inbox.md to .trash/Archive/Inbox.md
Archive/Inbox.md
note: Archive/Inbox.md
broken_links: 0
$ wetwaresuite notes.trash.list
1 file in /home/me/Notes/.trash
Archive/Inbox.md
$ wetwaresuite notes.trash.restore path=Archive/Inbox.md
restored Archive/Inbox.md
```

`permanent=true` deletes outright; `notes.trash.empty` empties the trash.

**Batch.** In a script (or over MCP) you can look at a rename and take it back; undo restores every file the rename touched:

```
# rename.txt: reorganise, look, then change your mind
notes.vault.open path=.
notes.rename note="Sam Lee" to="Sam Lee (gardener)"
notes.read note="Garden Redesign" heading=Tasks
undo
notes.read note="Garden Redesign" heading=Tasks
```

```sh
$ wetwaresuite batch rename.txt
opened vault1: /home/me/Notes (15 notes, 1 attachment, 9 open tasks)
...
renamed People/Sam Lee.md -> People/Sam Lee (gardener).md; updated 3 links in 3 notes
  Home.md
  Journal/2026-10-16.md
  Projects/Garden Redesign.md
...
## Tasks
- [x] Order soil 📅 2026-10-20 ✅ 2026-10-16
- [ ] Ask [[Sam Lee (gardener)]] about the irrigation timer
- [x] Measure the beds
- [ ] Book the skip 📅 2026-10-22
...
undone
## Tasks
- [x] Order soil 📅 2026-10-20 ✅ 2026-10-16
- [ ] Ask [[Sam Lee]] about the irrigation timer
...
```

**Claude.** "Rename Website Relaunch to Website 2.0 and make sure nothing breaks." Claude calls `notes_rename`, then `notes_links_unresolved` to confirm. "Which of my notes are disconnected? Suggest links for them." calls `notes_orphans`, `notes_read` on each and `notes_search` for related notes, then (once you agree) `notes_append` with the new `[[links]]`. Anything it did can be taken back with `undo`.

## Task 5: tags and properties (frontmatter)

**CLI.** Every tag with how many notes carry it (`nested=true` counts `#project/garden` for `#project` too), the notes with a tag, and renaming a tag everywhere (inline and in frontmatter, nested tags included):

```sh
$ wetwaresuite notes.tags nested=true
5 tags
#person  3
#project  2
#work  2
#hub  1
#project/garden  1
$ wetwaresuite notes.tags.find tag=project
2 notes tagged #project
Projects/Garden Redesign.md
Projects/Website 2.0.md
$ wetwaresuite notes.tags.rename from=person to=people
renamed the tag in 3 notes
```

Frontmatter (Obsidian's *properties*):

```sh
$ wetwaresuite notes.frontmatter.get note="Garden Redesign"
status: active
tags: project
due: 2026-11-30
note: Projects/Garden Redesign.md
keys: 3
frontmatter: {"due":"2026-11-30","status":"active","tags":["project"]}
$ wetwaresuite notes.frontmatter.set note="Garden Redesign" key=status value=done
Projects/Garden Redesign.md: status: done
$ wetwaresuite notes.frontmatter.set note="Sam Lee" key=aliases value="Sam, S. Lee" list=true
People/Sam Lee.md: aliases: Sam, S. Lee
$ wetwaresuite notes.frontmatter.get note=Sam key=aliases
Sam, S. Lee
found: true
value: ["Sam","S. Lee"]
list: true
```

`frontmatter.set` changes only that key's lines; the rest of the YAML (comments, nested maps, order) and the note stay byte for byte. A list written `[a, b]` stays in that style; a new list is written as `- item` lines. `value="[a, b]"` or `list=true` makes a list, `append=true` adds items to an existing one, `delete=true` removes the key, and a note without frontmatter gets some. The new alias works straight away (`note=Sam` above).

**Batch.**

```
notes.vault.open path=/home/me/Notes
notes.tags.rename from=person to=people
notes.frontmatter.set note="Website 2.0" key=status value=active
notes.frontmatter.set note="Website 2.0" key=tags value=launch append=true
notes.search query="tag:launch"
```

**Claude.** "Tag every note in Projects that mentions the budget with #finance." Claude calls `notes_search` with `query="budget path:Projects/"`, then `notes_frontmatter_set` with `key=tags append=true value=finance` on each hit.

## Task 6: tasks across the vault

Any `- [ ]` line in any note is a task (`* [ ]`, `1. [ ]` and tasks inside quotes count too). Due dates are read from `📅 2026-10-20` (the Tasks plugin's style), `due:2026-10-20`, `@due(2026-10-20)` and `[due:: 2026-10-20]` (Dataview).

**CLI.**

```sh
$ wetwaresuite notes.tasks today=2026-10-16
6 tasks
[ ] Renew the domain 📅 2026-10-12  (Home.md:12, due 2026-10-12) OVERDUE
[ ] Draft the new home page due:2026-10-18  (Projects/Website 2.0.md:15, due 2026-10-18)
[ ] Order soil 📅 2026-10-20  (Projects/Garden Redesign.md:19, due 2026-10-20)
[ ] Review copy with [[Priya]] 📅 2026-10-25  (Projects/Website 2.0.md:16, due 2026-10-25)
[ ] Email Priya the draft  (Journal/2026-10-15.md:10)
[ ] Ask [[Sam Lee]] about the irrigation timer  (Projects/Garden Redesign.md:20)
count: 6
total: 6
overdue: 1
$ wetwaresuite notes.tasks due=week today=2026-10-16
3 tasks
[ ] Renew the domain 📅 2026-10-12  (Home.md:12, due 2026-10-12) OVERDUE
[ ] Draft the new home page due:2026-10-18  (Projects/Website 2.0.md:15, due 2026-10-18)
[ ] Order soil 📅 2026-10-20  (Projects/Garden Redesign.md:19, due 2026-10-20)
$ wetwaresuite notes.tasks tag=work status=all
2 tasks
[ ] Draft the new home page due:2026-10-18  (Projects/Website 2.0.md:15, due 2026-10-18)
[ ] Review copy with [[Priya]] 📅 2026-10-25  (Projects/Website 2.0.md:16, due 2026-10-25)
```

Tick tasks by text (it must pick one task) or by note and line; add tasks to any note, or to today's daily note when no note is given:

```sh
$ wetwaresuite notes.tasks.toggle match="order soil" stamp=true today=2026-10-16
done: - [x] Order soil 📅 2026-10-20 ✅ 2026-10-16  (Projects/Garden Redesign.md:19)
$ wetwaresuite notes.tasks.add text="Buy seed trays" due=+3 today=2026-10-16
added to Journal/2026-10-16.md: - [ ] Buy seed trays 📅 2026-10-19
$ wetwaresuite notes.tasks.add note="Garden Redesign" heading=Tasks text="Book the skip" due=2026-10-22
added to Projects/Garden Redesign.md: - [ ] Book the skip 📅 2026-10-22
```

`stamp=true` adds a done date like the Tasks plugin (unticking removes it); `done=false` reopens; `style=dataview` or `style=plain` writes `[due:: date]` or `due:date` instead of the emoji. `today=` only pins the date for these examples.

**Batch.** A weekly review note:

```
# review.txt: a weekly review written into a note
notes.vault.open path=. -> v
notes.create note="Reviews/Week 42" content="# Week 42 review" -> r
notes.tasks due=overdue -> late
notes.tasks.add note=$r heading="Carry over" text="Go through the overdue list"
notes.links.unresolved
notes.orphans
notes.frontmatter.set note=$r key=tags value=[review]
notes.read note=$r
```

```sh
$ wetwaresuite batch review.txt
...
1 task
[ ] Renew the domain 📅 2026-10-12  (Home.md:12, due 2026-10-12) OVERDUE
...
---
tags:
  - review
---
# Week 42 review

## Carry over

- [ ] Go through the overdue list
```

**Claude.** "What's overdue or due this week? Tick off ordering the soil." Claude calls `notes_tasks` with `due=week`, then `notes_tasks_toggle` with `match="order soil"`.

## Task 7: the graph

**CLI.** The link graph as JSON (nodes with positions and degree, edges), Graphviz DOT, PNG or SVG. `focus=` and `depth=` make a local graph around one note:

```sh
$ wetwaresuite notes.graph format=dot focus="Garden Redesign" depth=1
4 nodes, 6 links
nodes: 4
edges: 6
graph: digraph vault {
  graph [overlap=false, splines=true];
  node [shape=box, style="rounded,filled", fillcolor="#eef0ff", fontname="Helvetica"];
  "Home.md" [label="Home"];
  "People/Sam Lee.md" [label="Sam Lee"];
  "Projects/Garden Redesign.md" [label="Garden Redesign"];
  "Projects/Website 2.0.md" [label="Website 2.0"];
  "Home.md" -> "Projects/Garden Redesign.md";
  ...
}
$ wetwaresuite notes.graph.export out=graph.png unresolved=true labels=all
wrote graph.png (15 nodes, 12 links)
$ wetwaresuite notes.graph.export out=graph.json
wrote graph.json (13 nodes, 10 links)
```

The picture uses a force-directed layout (the same seed always gives the same picture): notes are blue dots sized by how linked they are, unresolved links grey (`unresolved=true`), tags green (`tags=true`), attachments orange (`attachments=true`), the focus note orange. `folder=` limits it to part of the vault, `orphans=false` hides unlinked notes, `labels=auto|all|none` (auto shows names up to 150 nodes). The DOT output goes straight into Graphviz: `dot -Tsvg graph.dot > graph.svg`.

**Batch.**

```
notes.vault.open path=/home/me/Notes
notes.graph.export out=graph-projects.png folder=Projects tags=true
notes.graph.export out=graph-sam.svg focus="Sam Lee" depth=2
```

**Claude.** "Show me how my project notes connect." Claude calls `notes_graph` with `format=png` (or `preview=true`) and sees the picture, then can call `notes_backlinks` on the hubs.

## Task 8: publish: HTML, PDF and a static site

**CLI.** One note as a self-contained HTML page (images inlined, properties table, callouts, tables, task checkboxes, embedded notes rendered in place, links pointing to the other notes' `.html` files next to it):

```sh
$ wetwaresuite notes.render note="Garden Redesign" out=garden.html
wrote garden.html (1 note, 4757 bytes)
```

Without `out=`, the HTML comes back in the result's `html` field (handy over MCP). Leave out `note=` for the whole vault (or a `folder=`) in one file, with a table of contents and in-page links; `backlinks=true` lists the notes linking to each one:

```sh
$ wetwaresuite notes.render out=vault.html backlinks=true
wrote vault.html (10 notes, 11093 bytes)
```

PDF goes through the office layout engine (A4, page numbers, one note per page when rendering several); wikilinks become text, embedded notes and images are inlined:

```sh
$ wetwaresuite notes.render note="Garden Redesign" out=garden.pdf
wrote garden.pdf (1 page, 1 note)
$ wetwaresuite notes.render folder=Projects out=projects.pdf
wrote projects.pdf (2 pages, 2 notes)
```

`preview=true` attaches page 1 as a PNG.

**A static site.** One `.html` page per note at the same path, `index.html` (the vault's `index`, `Home`, `README` or `Welcome` note if there is one, or `home=`, then every note by folder), `tags.html`, a light/dark `style.css`, the attachments the notes use, and a "Linked from" list on every page:

```sh
$ wetwaresuite notes.export.site out=../site title="Our notes" graph=true
exported 10 pages, 5 tags and 1 attachment to ../site (open index.html)
../site/index.html
path: ../site
pages: 10
attachments: 1
tags: 5
```

Open `../site/index.html` in a browser, or upload the folder to any static host. `graph=true` adds the graph picture to the home page; `attachments=true` copies every attachment, not only the linked ones; `folder=Projects` publishes part of the vault. Templates are left out.

**Batch.** Publish nightly from cron:

```
# publish.txt
notes.vault.open path=/home/me/Notes
notes.export.site out=/var/www/notes title="Our notes"
notes.render folder=Projects out=/var/www/notes/projects.pdf
```

```sh
0 2 * * * wetwaresuite batch /home/me/publish.txt
```

**Claude.** "Make me a PDF of the garden project to send to Sam." Claude calls `notes_render` with `note="Garden Redesign" out=... preview=true` and looks at the page before telling you where it is.

## Parameter reference

Every command takes `vault=` (an id or folder; default: the last used, or the vault the current folder is in). `note=` accepts a path, name, title or alias.

| Command | What it does | Main parameters |
|---|---|---|
| `notes.vault.open` | open a folder as a vault | `path`, `preview` |
| `notes.vault.create` | make a vault folder with `.obsidian/` | `path`, `daily_folder`, `templates_folder` |
| `notes.vault.info` | counts, most linked, settings | `preview` |
| `notes.vault.configure` | daily note and template settings (in `.obsidian/`) | `daily_folder`, `daily_format`, `daily_template`, `templates_folder`, `date_format`, `time_format` |
| `notes.vault.reindex` | delete the `.wwnotes` cache and read everything again | |
| `notes.list` | notes with words, tags, dates | `folder`, `tag`, `sort` (path, name, title, modified, -modified, words, -words), `limit`, `json` |
| `notes.read` | a note's text | `note`, `heading`, `frontmatter`, `lines` |
| `notes.create` | new note, optionally from a template | `note`, `folder`, `template`, `vars`, `content`/`content_file`, `overwrite`, `today` |
| `notes.write` | replace a note's text | `note`, `content`/`content_file`, `create`, `keep_frontmatter` |
| `notes.append` | add text at the end or under a heading | `note`, `content`/`content_file`, `heading`, `prepend`, `create` |
| `notes.rename` | rename, rewriting links | `note`, `to`, `update_links` |
| `notes.move` | move to a folder, rewriting links | `note`, `folder`, `update_links` |
| `notes.delete` | to `.trash/` (or for good) | `note`, `permanent` |
| `notes.trash.list` / `.restore` / `.empty` | the trash | `path`, `to` |
| `notes.daily` | today's (or any day's) daily note | `date`, `folder`, `format`, `template`, `vars`, `append`, `heading`, `today` |
| `notes.templates.list` / `.render` / `.insert` | templates | `template`, `title`, `vars`, `note`, `today` |
| `notes.frontmatter.get` / `.set` | properties | `note`, `key`, `value`, `list`, `append`, `delete` |
| `notes.tags` | all tags with counts | `prefix`, `sort` (count, name), `nested`, `limit`, `json` |
| `notes.tags.find` / `.rename` | notes with a tag; rename a tag | `tag`; `from`, `to` |
| `notes.search` | ranked full-text search | `query`, `limit` (20), `snippets`, `json` |
| `notes.backlinks` | links to a note, unlinked mentions | `note`, `mentions`, `json` |
| `notes.links` | a note's outgoing links | `note`, `json` |
| `notes.links.unresolved` | links to notes that don't exist | `limit`, `json` |
| `notes.orphans` | notes without links | `incoming_only`, `templates` |
| `notes.tasks` | tasks across the vault | `status` (open, done, all), `due` (any, overdue, today, week, dated, undated), `before`, `after`, `note`, `tag`, `folder`, `query`, `sort`, `limit`, `today`, `json` |
| `notes.tasks.toggle` | tick/untick a task | `note`+`line` or `match`, `done`, `stamp`, `today` |
| `notes.tasks.add` | add a task | `text`, `note` (default: today's daily note), `due`, `heading`, `style` |
| `notes.outline` | a note's headings | `note`, `json` |
| `notes.graph` | the link graph as JSON/DOT/SVG text or PNG | `format`, `focus`, `depth`, `folder`, `tags`, `unresolved`, `attachments`, `orphans`, `width`, `height`, `labels`, `seed`, `preview` |
| `notes.graph.export` | the graph to a .json/.dot/.png/.svg file | `out` plus the graph parameters |
| `notes.render` | a note, folder or vault to HTML or PDF | `note`, `folder`, `format`, `out`, `title`, `properties`, `backlinks`, `embed_images`, `preview` |
| `notes.export.site` | a static website | `out`, `folder`, `home`, `title`, `attachments`, `graph`, `properties` |

Template variables: `{{title}}`, `{{date}}`, `{{time}}`, `{{date:FORMAT}}`, `{{time:FORMAT}}`, `{{yesterday}}`, `{{tomorrow}}`, `{{folder}}` and anything in `vars=` (`"a=1; b=two"`). Date formats (moment.js tokens as Obsidian uses them): `YYYY YY`, `MMMM MMM MM M`, `DD D Do DDDD`, `dddd ddd dd`, `HH H hh h mm m ss s A a`, ISO week `WW W` and year `gggg`, and `[literal text]`.

The Markdown renderer handles ATX and setext headings (with anchor ids), paragraphs with Obsidian's line breaks, `*em*`, `**strong**`, `~~strike~~`, `==highlight==`, inline and fenced/indented code (with a `language-` class; `mermaid` blocks are kept for mermaid.js), bullet, numbered, nested and task lists, quotes and `> [!type] callouts` (foldable with `+`/`-`), pipe tables with alignment, horizontal rules, links, autolinks and bare URLs, images with `|width`, `[[wikilinks]]` to notes, headings and blocks, `![[embeds]]` of notes, sections, blocks, images, audio, video and PDFs, `#tags`, `%% comments %%` (removed), `$math$` and `$$math$$` (left for KaTeX/MathJax) and raw HTML.

## Limitations

- **Markdown.** The renderer is CommonMark-ish, not a full CommonMark implementation: unusual emphasis nesting, link reference definitions (`[x][ref]`), footnotes, HTML blocks that contain blank lines and lazy continuation in nested quotes can render differently from Obsidian. Math is passed through for a client-side library, not typeset.
- **PDF.** PDFs use the standard PDF fonts, the built-in font for Greek and Cyrillic, and a font already on the computer for other scripts and emoji (see the office tutorial's "Scripts in PDFs"); the 📅 and ✅ task markers are written as "due" and "done"; wikilinks become plain text; image sizes from `|300` are ignored; callouts become bold-titled quotes.
- **Search** tokenises on letters and digits, so it has no stemming (budget does not find budgets; use `budget*`), no fuzzy matching, and no OR operator. CJK text without spaces is indexed as long runs.
- **Links.** Renaming a heading does not update `[[Note#Heading]]` links; renaming or moving whole folders is not a command (move notes one by one). Links inside frontmatter values are found and followed but `rename` only rewrites them when they are plain `[[...]]` text.
- **YAML.** Frontmatter is read with a small YAML subset (scalars, quoted strings, flow and block lists, block scalars); nested maps are kept as raw text and shown as such.
- **Daily notes** don't support Obsidian's periodic (weekly/monthly) notes plugin settings; use `notes.daily format="gggg-[W]WW"` or `notes.create` with a template for those.
- **The site** is plain static HTML without client-side search; Dataview queries and plugin code blocks are shown as code.
- **No live watching.** Changes made by other programs are seen at the next command, not pushed; there is no conflict detection if two programs write the same note in the same instant.
