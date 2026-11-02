# Code: an editor and IDE for any language

`code` is a code editor that knows 38 languages, from C, C++, Rust, Go, Python and JavaScript to shell scripts, SQL, Markdown and config files. It gives you:

- syntax highlighting, tabs, a file tree and an outline of functions and classes;
- find and replace in one file or across a whole project;
- go to line, auto-indent and comment toggling;
- **Run** for the file you are looking at and **Build** for the whole project;
- errors you can click to jump to the line;
- a git panel to stage, commit, read history and switch branches.

Running and building use the compilers, interpreters and build tools that are already on your computer: gcc, clang, MSVC, cmake, make, ninja, cargo, go, python, node, java, dotnet and about forty more. Nothing is ever downloaded. If a tool is missing, the error says which one to install. No AI is involved, and nothing goes online.

`code` is part of the system app (`wetwaresuite-system`) and the all-in-one `wetwaresuite`. Each task below is shown in the window, on the command line (CLI), in batch scripts (see [headless and batch](headless-and-batch.md)) and from Claude (see [mcp.md](mcp.md)). Every output below was copied from a real run on Linux. The samples are a small C project at `/tmp/demo/proj` and a Python one at `/tmp/demo/shop`.

## How code works

- **Open files are documents.** `code.open` gives an id (`code1`). The edit commands work on that id in memory, and `code.save` writes it to disk. Edits can be undone in a batch script, an MCP session or the window, one step at a time.
- **A project is a folder.** `code.project folder=...` sets the folder that the tree, search, build and git commands use. Without it they use the folder of the file you opened. In a one-shot CLI command, they use the current folder.
- **Run and build are jobs.** `code.run` and `code.build` start a job (`job1`) and wait for it, unless you pass `wait=false`. You can type into a running program with `code.job_input`, read more of its output with `code.job_output` and stop it with `code.job_stop`. When a job ends, its output is scanned for errors and warnings, and those come back as `problems`: file, line, column, kind and message.

## The Code window

Start the window with `wetwaresuite-system` and click **Open** on the **Code editor** card. To open a file straight away:

```sh
wetwaresuite-system main.c
wetwaresuite-system gui --action code.open
```

Opening a file also opens its folder in the tree.

- **Toolbar**: **Open folder...**, **Open file...**, **New**, **Save**, **Run** (F5), **Build** (Ctrl+B), **Find** (Ctrl+F) and **Go to line** (Ctrl+G). In the advanced view (View > Show advanced options) you also get **Test**, **Clean**, **Stop** and **Format**. Format tidies the file with clang-format, rustfmt, gofmt, black or prettier, when one is installed.
- **Left side**: four sections.
  - **Files**: the project tree. Click a file to open it.
  - **Outline**: the functions, classes and headings in the file. Click one to jump to it.
  - **Find**: search every file in the project. Click a hit to open it at that line. In the advanced view there is a replace box and a **Replace in all files** button. Undo puts every file back.
  - **Changes** (called **Git** in the advanced view): see the git panel below.
- **Tabs**: one per open file. A `*` marks unsaved changes. Drag tabs to reorder them, and close one with its x.
- **The editor**: line numbers and highlighting in a monospace font.
  - Errors from the last run or build are marked in the margin.
  - **Enter** keeps the indent, and indents one more step after `{`, `[`, `(`, or `:` in Python.
  - **Tab** and **Shift+Tab** indent and unindent the selected lines.
  - **Home** goes to the first word of the line, then to the very start.
  - **Ctrl+/** comments or uncomments the selected lines.
  - **Ctrl+D** selects the next match of the word or selection with another cursor. **Ctrl+Shift+L** puts a cursor on every match. **Alt+click** adds a cursor anywhere, and **Ctrl+Alt+Up/Down** adds one on the line above or below. Typing, deleting and arrows work at every cursor. **Esc** goes back to one.
  - **Ctrl+Shift+D** duplicates the line.
  - **Folding**: click the arrow in the margin, or press **Ctrl+Shift+[** and **Ctrl+Shift+]**. Folding follows indentation, so it works in every language. View > Fold all / Open all.
  - **Alt+Z** (or View > Wrap) wraps long lines to the window.
  - With a language server on (see Task 7): **Ctrl+Space** completes, **F12** or **Ctrl+click** goes to the definition, and resting the mouse on a name shows its type and docs.
  - **Ctrl+A**, **Ctrl+C**, **Ctrl+X** and **Ctrl+V** work as usual.
  - **Ctrl+Z** undoes, and **Ctrl+Y** or **Ctrl+Shift+Z** redoes.
  - **Ctrl+Home** and **Ctrl+End** go to the start and end of the file (Cmd on a Mac).
  - What you type is saved into the document about half a second after you stop typing. That way, scripts and Claude see the same text.
- **Find bar** (Ctrl+F): **<** and **>** step through the matches (or **F3** and **Shift+F3**). **Aa** matches case and **Word** matches whole words. In the advanced view, **.\*** turns on regular expressions. **Replace** replaces on the current line, and **Replace all** replaces everywhere in the file.
- **Bottom**: the **Output** tab shows what the program or build printed.
  - While a program is running, an input box under the output sends it what you type, and **Stop** ends it.
  - The **Problems** tab lists errors and warnings. Click one to open the file at that line.

## Task 1: open a project and look around

```
$ wetwaresuite code.tree folder=/tmp/demo/shop
/tmp/demo/shop
src/
  cart.py
  report.py

$ wetwaresuite code.count folder=/tmp/demo/shop
language           files     lines    blank   comment
Python                 2        30       12         0
total                  2        30       12         0

$ wetwaresuite code.toolchains
gcc - C (/usr/bin/gcc)
g++ - C++ (/usr/bin/g++)
clang - C (/usr/bin/clang)
cmake - CMake projects (/usr/bin/cmake)
make - Makefiles (/usr/bin/make)
rustc - Rust (/root/.cargo/bin/rustc)
go - Go (/usr/local/go/bin/go)
python3 - Python (/usr/bin/python3)
node - JavaScript (/opt/node22/bin/node)
...
```

`code.languages` lists every language with its file extensions. `code.tree` leaves out `.git` and `node_modules`, and hides dot-files unless you pass `hidden=true`. `code.search` also skips build output (`build`, `target`, `dist`, `out`).

## Task 2: find your way in a file, and rename something everywhere

```
# refactor.txt: look at a file, jump around, rename a constant everywhere, check the result
code.project folder=/tmp/demo/shop
code.open path=src/cart.py -> cart
code.outline doc=$cart
code.goto doc=$cart line=12 context=1
code.find doc=$cart find=items
code.replace_in_files find=TAX_RATE replace=VAT_RATE whole_word=true files=*.py
code.search find=VAT_RATE
undo
code.search find=VAT_RATE
```

```
$ wetwaresuite batch refactor.txt
opened code1 (Python, 24 lines, /tmp/demo/shop/src/cart.py)
class Cart  (line 4)
  function __init__  (line 5)
  function add  (line 8)
  function total  (line 11)
function main  (line 15)

11	    def total(self):
12	        return sum(p for _, p in self.items) * (1 + TAX_RATE)
13

3 matches
6:14:         self.items = []
9:14:         self.items.append((name, price))
12:39:         return sum(p for _, p in self.items) * (1 + TAX_RATE)

replaced 4 matches in 2 files
src/cart.py: 2
src/report.py: 2

4 matches in 2 files
src/cart.py:1: VAT_RATE = 0.2
src/cart.py:12: return sum(p for _, p in self.items) * (1 + VAT_RATE)
src/report.py:1: from cart import Cart, VAT_RATE
src/report.py:5: return f"{len(cart.items)} items, tax {VAT_RATE:.0%}"

undone
no matches
```

How these commands work:

- **Find options.** `code.find`, `code.search`, `code.replace` and `code.replace_in_files` all take `match_case`, `whole_word` and `regex`.
- **Previewing.** `dry_run=true` on `code.replace_in_files` says what would change without changing anything:

  ```
  $ wetwaresuite code.replace_in_files folder=/tmp/demo/shop find=TAX_RATE replace=VAT_RATE whole_word=true dry_run=true
  would replace 4 matches in 2 files
  ```
- **Choosing files.** `files=` limits the search to file names that match a pattern (`*.py`, `src/*.c`).
- **Undo.** `undo` puts every file back exactly as it was.

## Task 3: run a file

```
$ wetwaresuite code.run path=src/cart.py input=apple
job1: run with python3 (finished OK, 0.0 s)
$ python3 -u /tmp/demo/shop/src/cart.py
item? apple: 12.00
[exit code 0]
```

`input=` is typed into the program. `args=` passes command-line arguments, with quotes allowed (`args="a 'b c'"`).

How each kind of file is run:

- **Compiled languages** are built first, then run. C and C++ use gcc, clang or MSVC, Rust uses rustc, and Zig, Swift, C# and Haskell use their own compilers.
- **Scripts** go straight to their interpreter: Python, JavaScript (node, bun or deno), TypeScript, Ruby, Lua, PHP, shell, PowerShell, R and many more.
- **A program that is already built** (an ELF, `.exe` or Mach-O file) runs as it is.

When a program fails, its errors become clickable problems:

```
$ wetwaresuite code.run path=src/bug.py
job1: run with python3 (failed, exit code 1, 0.0 s), 2 errors, 0 warnings
Traceback (most recent call last):
  File "/tmp/demo/shop/src/bug.py", line 5, in <module>
    print(c.total())
  File "/tmp/demo/shop/src/cart.py", line 12, in total
    return sum(p for _, p in self.items) * (1 + TAX_RATE)
TypeError: unsupported operand type(s) for +: 'int' and 'str'
[exit code 1]

errors: 2
problems: /tmp/demo/shop/src/bug.py	5	0	error	TypeError: unsupported operand type(s) for +: 'int' and 'str'
/tmp/demo/shop/src/cart.py	12	0	error	TypeError: unsupported operand type(s) for +: 'int' and 'str'
```

The error reader understands:

- gcc, clang and javac;
- MSVC, which covers C# and `tsc` output too;
- rustc and cargo;
- Python tracebacks, Node stack traces and Lua errors;
- `file:line:col` lines from most other tools, such as Go, eslint and linters.

Run it on any text with `code.errors text=...`.

For a program that keeps running (a server, a game loop or a prompt):

1. Start it with `wait=false`.
2. Type into it with `code.job_input text=...`.
3. Read what it printed with `code.job_output`.
4. End it with `code.job_stop`.

`code.jobs` lists every job.

## Task 4: build a project, fix the error, commit

`/tmp/demo/proj` is a small CMake project whose `main.c` is missing a semicolon:

```
# fix.txt: open the project, build it, fix the error the build found, build again, commit
code.project folder=/tmp/demo/proj
code.open path=main.c -> c
code.build
code.errors
code.edit doc=$c old="+= i" new="+= i;"
code.save doc=$c
code.build
code.run path=build/demo
code.git_status
code.git_commit message="Sum 1 to 10" all=true
code.git_log
```

```
$ wetwaresuite batch --keep-going fix.txt
/tmp/demo/proj
5 files: Python 1, Plain text 1, INI 1, CMake 1, C 1
build system: cmake
git: yes
opened code1 (C, 9 lines, /tmp/demo/proj/main.c)
line 4: job1: build the cmake project (failed, exit code 2, 0.3 s), 1 error, 0 warnings
$ cmake -S . -B build
...
$ cmake --build build --parallel
[ 50%] Building C object CMakeFiles/demo.dir/main.c.o
/tmp/demo/proj/main.c: In function 'main':
/tmp/demo/proj/main.c:5:45: error: expected ';' before 'printf'
    5 |     for (int i = 1; i <= 10; i++) total += i
      |                                             ^
      |                                             ;
[exit code 2]

/tmp/demo/proj/main.c:5:45: error: expected ';' before 'printf'
count: 1

replaced 1 place
saved /tmp/demo/proj/main.c (145 bytes)
job2: build the cmake project (finished OK, 0.1 s)
$ cmake --build build --parallel
[ 50%] Building C object CMakeFiles/demo.dir/main.c.o
[100%] Linking C executable demo
[100%] Built target demo
[exit code 0]

job3: run the program (finished OK, 0.0 s)
$ /tmp/demo/proj/build/demo
55
[exit code 0]

branch master (no commits yet)
  new (not staged): .gitignore
  new (not staged): CMakeLists.txt
  new (not staged): fix.txt
  new (not staged): hello.py
  new (not staged): main.c

committed 08e7a0c: Sum 1 to 10
08e7a0c  Sum 1 to 10  (Sam, 0 seconds ago)
```

`--keep-going` lets the script carry on after the first build fails, which is the point here.

`code.build` finds the build system in the project folder:

- CMake (configured into `build/` the first time);
- Make;
- Meson;
- Cargo and Go modules;
- npm, pnpm or yarn (picked from the lock file);
- Maven and Gradle;
- .NET (`.sln` or `.csproj`);
- Python (`pyproject.toml` or `setup.py`);
- Zig.

`action=test`, `action=run` and `action=clean` run that step instead.

## Task 5: git

The git commands use the `git` installed on your computer:

| Command | What it does |
|---|---|
| `code.git_init` | start tracking a folder |
| `code.git_status` | the branch and every changed file, staged or not |
| `code.git_diff` | what changed (`file=`, `staged=true`, or `commit=` for an old one) |
| `code.git_stage`, `code.git_unstage` | choose what goes in the next commit (`paths=` space separated, or everything) |
| `code.git_commit` | commit (`all=true` stages everything first) |
| `code.git_log` | history (`count=`, `file=`) |
| `code.git_branches`, `code.git_switch` | list, switch to or create (`create=true`) a branch |
| `code.git_merge` | merge a branch into the current one (`abort=true` backs out of a conflicted merge) |
| `code.git_remotes` | list, `add=name url=...`, change `url` or `remove` a remote |
| `code.git_fetch` | fetch and report how far ahead and behind you are |
| `code.git_pull` | pull (`remote`, `branch`, `rebase=true`) |
| `code.git_push` | push; the first push of a branch sets its upstream. `force=true` uses `--force-with-lease`, so it never overwrites work you haven't seen |

In the window, the **Changes** section shows every changed file with a tick box. Ticking it stages the file. Click a file name to see its diff in the output area. Type a message and press **Commit**. With nothing ticked, the button reads **Commit all changes**. Below that are the last 15 commits. In the advanced view there is also a branch picker and **Create branch**. A folder that isn't tracked yet gets a **Start tracking changes (git init)** button.

Push, pull and fetch use your normal git sign-in (ssh keys or a credential helper). They never stop to ask for a password: if git needs one you get a clear "sign in with git first" error instead. A merge or pull that hits conflicts lists the conflicted files in `conflicts` rows. Fix them, stage, and commit, or call `code.git_merge abort=true`.

## Task 6: highlighting for other programs

`code.highlight` colours code for the terminal (`format=ansi`), as HTML (`format=html`, optionally `output=page.html`), or as spans (the window uses those):

```
$ wetwaresuite code.highlight text='x = 1  # one' language=python format=ansi
x = 1  # one        (1 in yellow, the comment in grey)
```

## Task 7: language servers

A language server gives the editor real completion, go-to-definition, hover info and live errors. The suite doesn't ship any. It uses the ones you already have:

| Language | Server it looks for |
|---|---|
| Python | `pyright-langserver`, then `pylsp` |
| C, C++ | `clangd` |
| Rust | `rust-analyzer` |
| Go | `gopls` |
| JavaScript, TypeScript | `typescript-language-server` |

Set `WETWARESUITE_LSP_<LANGUAGE>` (for example `WETWARESUITE_LSP_PYTHON="pylsp -v"`) to use a different one. In the window, turn it on with View > Language server. The status line tells you when a server is installed but off. Errors and warnings from the server show up in the margin and in the Problems tab while you type.

![completion, hover and folding in the code editor](images/code-editor-lsp.png)

From scripts and Claude, the file has to be saved first, because the server reads the project from disk:

```
$ wetwaresuite batch lsp.txt          # lsp.txt holds the lines below
code.open path=/tmp/demo/app.py
code.complete line=12 col=9
code.hover line=4 col=7
code.definition line=12 col=9
code.references line=4 col=7
code.diagnostics wait=5
code.lsp status
```

`code.complete` returns rows of label, kind, detail and the text to insert. `code.diagnostics` returns rows of line, column, severity and message, with `errors` and `warnings` counts. `code.lsp stop` shuts the servers down; they also stop when the suite exits.

## Claude

```sh
claude mcp add wetwaresuite-code -s user -- wetwaresuite-system mcp --tools code
```

Claude gets all 51 `code_*` tools (`code_open`, `code_edit`, `code_run`, `code_build`, `code_git_commit` and the rest). Example prompts:

- *"Build ~/src/game and fix whatever breaks"*: `code_project`, then `code_build`. Claude reads the `problems` rows, calls `code_open` and `code_goto` on each one, fixes it with `code_edit` or `code_replace_lines`, then calls `code_save` and `code_build` again.
- *"Rename TAX_RATE to VAT_RATE across the project, but show me first"*: `code_replace_in_files` with `dry_run=true`, then without it.
- *"Run cart.py with the input apple and tell me what it prints"*: `code_run` with `input`.
- *"Commit what I changed today with a sensible message"*: `code_git_status` and `code_git_diff`, then `code_git_commit`.

Edits made by Claude go through the same documents as the window. If the window is open, Claude's changes show up in the editor, and you can undo them there.

## Parameter reference

| Command | Main parameters |
|---|---|
| `code.open`, `code.new`, `code.close`, `code.files` | `path`, `language`, `name`, `text`, `doc` |
| `code.read` | `doc`, `start`, `end`, `raw` |
| `code.edit` | `doc`, `old`, `new`, `all` |
| `code.insert`, `code.replace_lines`, `code.set_text` | `doc`, `line`/`start`/`end`, `text` |
| `code.find`, `code.replace` | `doc`, `find`, `replace`, `regex`, `match_case`, `whole_word`, `line` |
| `code.search`, `code.replace_in_files` | `find`, `replace`, `folder`, `files`, `regex`, `match_case`, `whole_word`, `limit`, `dry_run` |
| `code.goto`, `code.outline` | `doc`, `line`, `context` |
| `code.save` | `doc`, `path`, `eol` (keep, lf, crlf) |
| `code.format`, `code.set_language`, `code.highlight` | `doc`, `language`, `format`, `output` |
| `code.project`, `code.tree`, `code.count` | `folder`, `depth`, `hidden` |
| `code.run` | `doc` or `path`, `args`, `input`, `wait`, `timeout` |
| `code.build` | `action` (build, test, run, clean), `folder`, `wait`, `timeout` |
| `code.jobs`, `code.job_output`, `code.job_input`, `code.job_stop`, `code.errors` | `job`, `text`, `newline`, `close`, `full` |
| `code.toolchains`, `code.languages` | |
| `code.git_*` | see Task 5 |
| `code.complete`, `code.hover`, `code.definition`, `code.references` | `doc` or `path`, `line`, `col` |
| `code.diagnostics`, `code.lsp` | `doc`, `wait`; `action` (status, stop) |

`wetwaresuite help code.<name>` shows every parameter.

## Limitations

- **Language servers** have to be installed separately (see Task 7). Without one, the outline comes from a fast scanner, not a compiler, and there is no completion.
- **Editor.** Folding follows indentation, not syntax, so a block that isn't indented can't be folded.
- **Programs and input.** A running program gets its input as plain lines through a pipe. Programs that need a real terminal (full-screen tools, password prompts) should be run in the [terminal](terminal.md) or outside the suite.
- **Git.** There is no interactive rebase, and conflicts are fixed by editing the files (no three-way merge view).
