# Automate: scripts, pipelines and watch folders

`automate` runs work across every tool in the suite without you typing each command: a **script** is a batch file with variables you fill in when you run it, a **pipeline** is a YAML or JSON file with loops over folders, conditions and captured results, and a **watch** runs a script or pipeline for every file that lands in a folder. On top of that, the **Python client** (`python/wetwaresuite`) turns every command into a Python method, for when you want real code around the suite.

There is deliberately no `automate.shell`: scripts and pipelines can only run suite commands, so a pipeline you got from someone else (or one Claude wrote) can't delete your home folder.

Each task below is shown three ways:

- **CLI**: single `wetwaresuite automate.*` commands.
- **Batch**: a batch file run with `wetwaresuite batch file.txt`, which is also how cron and Task Scheduler run things (see [headless and batch](headless-and-batch.md)).
- **Claude**: a prompt for Claude over MCP and the tools it calls (see [mcp.md](mcp.md)). `wetwaresuite mcp` serves `automate_*` next to every other tool.

Every example was run against the real binary in a scratch folder; outputs are copied from those runs (the folder is shown as `~/work`; long outputs are shortened where marked).

## The example folder

```
~/work
├── logos/acme.svg, globex.svg, initech.svg
├── photos/big.png (3000x2000), medium.jpg (1200x900)
└── scripts/
    ├── thumb.txt      a batch script: one image -> a small copy
    ├── web.yaml       a per-file pipeline: one image -> web JPEG + thumbnail
    ├── logos.yaml     every SVG -> PNGs at four sizes + a contact sheet
    └── logos.json     the same pipeline in JSON
```

## Variables

Scripts and pipelines use `$name` for variables. They come from `vars=` (`vars=input=photos/big.png,width=400`, a `\,` for a comma inside a value, or a JSON object: `vars={"input": "a b.png"}`), from captures (`-> img` in a script, `capture: img` in a pipeline), from loops (`$file`, `$item`) and from these built-ins:

| variable | value |
| --- | --- |
| `$cwd` | the current folder |
| `$date`, `$time` | today as `2026-10-16` and the time as `06-15-39` (safe in file names) |
| `$script_dir`, `$pipeline_dir` | the folder the script or pipeline file is in |
| `$file` | the current file in a `for_each` loop or a watch, or `vars=file=...` |
| `$file.name`, `$file.stem`, `$file.ext`, `$file.dir` | `big.png`, `big`, `png` (lower case), `photos`; also as `$name`, `$stem`, `$ext`, `$dir` |
| `$file.rel` | the path inside the watched folder (watches only) |
| `$index`, `$count` | position in the loop (from 1) and the number of items |
| `$item` | the current value of a `for:` loop (or the `as:` name) |
| `$img.width`, `$img.message`, `$img.ok` | the fields of a captured result, its message, and whether it succeeded |

A variable name is letters, digits, `_` and dots, so **`$stem_x` means the variable `stem_x`**, not `$stem` followed by `_x`. Write `${stem}_x` when text follows straight after a name:

```sh
$ wetwaresuite automate.pipeline 'text=steps:
  - echo: "[$stem_x] [${stem}_x] [$file.stem] [$file.ext] [\$5]"' vars=file=photos/big.png,stem_x=hello
ran pipeline: 0 commands, 0 failed
[hello] [big_x] [big] [png] [$5]
```

`\$` is a literal dollar. Scripts and pipelines both refuse to start when a variable they use isn't given (see Task 1); a `$name` that still isn't defined when a step runs (a capture from a step that was skipped, say) is left as it is.

Paths inside scripts and pipelines are relative to the **current folder**, the way batch files work, not to the script's own folder; use `$script_dir` or `$pipeline_dir` to reach files next to the script.

## Task 1: run a script with variables

`scripts/thumb.txt` is an ordinary batch script with `$input`, `$width` and `$out` in it; its leading `#` comments are its description:

```
# Make a small copy of one image.
# Needs: input (the image), width (pixels) and out (where to save it).
photo.open path=$input -> img
photo.resize doc=$img width=$width
photo.save doc=$img path=$out
```

**CLI.** `automate.list_scripts` shows what's in `./scripts` (or `folder=`, or `$WETWARESUITE_SCRIPTS`), with each one's description and the variables it needs:

```sh
$ cd ~/work
$ wetwaresuite automate.list_scripts
4 scripts in scripts
  logos.json (pipeline): The same pipeline as logos.yaml, in JSON.
  logos.yaml (pipeline): Every SVG in a folder as PNGs at 64, 128, 256 and 512 px, plus a contact sheet.
  thumb.txt (script): Make a small copy of one image. Needs: input (the image), width (pixels) and out (where to save it).
      needs: input, out, width
  web.yaml (pipeline): Resize an image for the web and make a thumbnail.
      needs: file
count: 4
```

Missing variables are reported before anything runs:

```sh
$ wetwaresuite automate.run_script path=scripts/thumb.txt vars=input=photos/big.png
the script needs out, width: pass vars=out=...

$ wetwaresuite automate.run_script path=scripts/thumb.txt vars=input=photos/big.png,width=400,out=big_400.png
ran scripts/thumb.txt: 3 commands, 0 failed
opened photos/big.png as img1 ...
resized to 400x267 ...
saved big_400.png ...
ran: 3
failed: 0
```

Each command's output is cut to its first line (the `...`); `verbose=true` shows everything. `dry_run=true` prints each line with the variables filled in and runs nothing (captures like `$img` only exist once the script really runs, so they stay as they are):

```sh
$ wetwaresuite automate.run_script path=scripts/thumb.txt vars=input=photos/big.png,width=400,out=big_400.png dry_run=true
dry run of scripts/thumb.txt: 3 commands, 0 failed
would run: photo.open path=photos/big.png -> img
would run: photo.resize doc=$img width=400
would run: photo.save doc=$img path=big_400.png
```

A script stops at the first failing line unless `keep_going=true`. Values with spaces are passed through safely: `vars=out=my photos/a.png` becomes one argument. `text=` runs a script given inline instead of a file, and `vars=` also takes JSON:

```sh
$ wetwaresuite automate.run_script 'text=photo.open path=$input -> img' 'vars={"input": "photos/medium.jpg"}'
ran script: 1 command, 0 failed
opened photos/medium.jpg as img1 ...
```

**Batch.** Scripts can call scripts. A batch file that makes three sizes:

```
# sizes.txt
automate.run_script path=scripts/thumb.txt vars=input=photos/big.png,width=1600,out=big_1600.png
automate.run_script path=scripts/thumb.txt vars=input=photos/big.png,width=800,out=big_800.png
automate.run_script path=scripts/thumb.txt vars=input=photos/big.png,width=400,out=big_400.png
```

Nesting stops at 8 levels, so a script that calls itself fails cleanly instead of running for ever.

**Claude.** *"What scripts do I have, and make me a 400 px copy of photos/big.png with the thumb script."* Claude calls `automate_list_scripts` (it reads the `needs` list from the `scripts` field), then `automate_run_script` with `path: "scripts/thumb.txt"` and `vars: "input=photos/big.png,width=400,out=big_400.png"`.

## Task 2: a pipeline: every logo SVG to PNG sizes plus a contact sheet

A pipeline is a list of steps. `scripts/logos.yaml`:

```yaml
name: logo exports
description: Every SVG in a folder as PNGs at 64, 128, 256 and 512 px, plus a contact sheet.
vars:
  src: logos
  out: build/logos
steps:
  - mkdir: $out/png
  - for_each: $src/*.svg
    steps:
      - for: [64, 128, 256, 512]
        as: px
        run: convert.file input=$file output=$out/png/${stem}_${px}.png width=$px overwrite=true
  - run: library.index root=$out/png
  - run: library.thumbnails library=$out/png query="ext:png sort:name" sheet=$out/contact-sheet.png columns=4 size=128
  - echo: wrote $out/contact-sheet.png
```

`vars:` are defaults; `vars=` on the command line wins over them. `for_each` loops over a glob (`*`, `?`, `[abc]`, and `**` for any depth: `in/**/*.png`) in natural order (`walk_2` before `walk_10`); `for` loops over a list of values.

**CLI.** Look first, then run:

```sh
$ wetwaresuite automate.pipeline path=scripts/logos.yaml dry_run=true
dry run of logo exports: 14 commands, 0 failed
would create build/logos/png
for_each logos/*.svg: 3 files
would run: convert.file input=logos/acme.svg output=build/logos/png/acme_64.png width=64 overwrite=true
would run: convert.file input=logos/acme.svg output=build/logos/png/acme_128.png width=128 overwrite=true
...
would run: convert.file input=logos/initech.svg output=build/logos/png/initech_512.png width=512 overwrite=true
would run: library.index root=build/logos/png
would run: library.thumbnails library=build/logos/png query="ext:png sort:name" sheet=build/logos/contact-sheet.png columns=4 size=128
wrote build/logos/contact-sheet.png

$ wetwaresuite automate.pipeline path=scripts/logos.yaml
ran logo exports: 14 commands, 0 failed
for_each logos/*.svg: 3 files
converted logos/acme.svg to build/logos/png/acme_64.png ...
converted logos/acme.svg to build/logos/png/acme_128.png ...
...
converted logos/initech.svg to build/logos/png/initech_512.png ...
indexed ~/work/build/logos/png: 12 files (12 new, 0 updated, 0 unchanged, 0 removed) in 0.00 s; library has 12 files ...
12 thumbnails made, 0 already cached, 0 without a thumbnail (of 12 files) ...
wrote build/logos/contact-sheet.png
ran: 14
failed: 0
```

The result is twelve PNGs in `build/logos/png` and a numbered contact sheet with the three logos in rows of four sizes.

The same pipeline in JSON, `scripts/logos.json`, using the structured `command:` + `args:` form (no quoting to think about):

```json
{
  "name": "logo exports",
  "description": "The same pipeline as logos.yaml, in JSON.",
  "vars": {"src": "logos", "out": "build/logos-json"},
  "steps": [
    {"mkdir": "$out/png"},
    {"for_each": "$src/*.svg", "steps": [
      {"for": [64, 128, 256, 512], "as": "px",
       "command": "convert.file",
       "args": {"input": "$file", "output": "$out/png/${stem}_${px}.png", "width": "$px", "overwrite": true}}
    ]},
    {"run": "library.index root=$out/png"},
    {"run": "library.thumbnails library=$out/png query=\"ext:png sort:name\" sheet=$out/contact-sheet.png columns=4 size=128"}
  ]
}
```

```sh
$ wetwaresuite automate.pipeline path=scripts/logos.json vars=src=logos,out=build/json
ran logo exports: 14 commands, 0 failed
for_each logos/*.svg: 3 files
converted logos/acme.svg to build/json/png/acme_64.png ...
...
```

**Conditions and captures.** A step's `if:` (or `unless:`) decides whether it runs, `ext:` is a shorthand for `if: $file.ext in ...`, and `capture:` keeps a result to test later. This pipeline sorts photos by format and makes previews of the big ones only:

```yaml
description: Sort photos into folders by type, and make previews of the big ones.
steps:
  - for_each: photos/*
    steps:
      - if: $file.ext in png,tga
        run: convert.file input=$file output=sorted/lossless/$stem.png overwrite=true
      - ext: jpg,jpeg
        run: convert.file input=$file output=sorted/jpeg/$name overwrite=true
      - run: photo.open path=$file
        capture: img
      - if: $img.width >= 2000
        run: photo.save doc=$img path=sorted/previews/$stem.jpg quality=70
        on_error: continue
      - run: photo.close doc=$img
```

```sh
$ wetwaresuite automate.pipeline path=scripts/sort.yaml dry_run=true
dry run of sort.yaml: 8 commands, 0 failed
for_each photos/*: 2 files
would run: convert.file input=photos/big.png output=sorted/lossless/big.png overwrite=true
would run: photo.open path=photos/big.png
would check: <img>.width >= 2000
would run: photo.save doc=<img> path=sorted/previews/big.jpg quality=70
would run: photo.close doc=<img>
would run: convert.file input=photos/medium.jpg output=sorted/jpeg/medium.jpg overwrite=true
...

$ wetwaresuite automate.pipeline path=scripts/sort.yaml
ran sort.yaml: 7 commands, 0 failed
for_each photos/*: 2 files
converted photos/big.png to sorted/lossless/big.png ...
opened photos/big.png as img1 ...
saved sorted/previews/big.jpg ...
closed img1
converted photos/medium.jpg to sorted/jpeg/medium.jpg ...
opened photos/medium.jpg as img2 ...
closed img2
```

In a dry run, captures don't exist yet (`<img>`), so conditions on them are shown as `would check:` and assumed true.

Conditions: `$a == b`, `$a != b`, `$n > 3` (also `<`, `>=`, `<=`; both sides numbers), `$file.ext in png,jpg`, `$x not in a,b`, `$text contains word`, `$name matches IMG_*` (wildcards), `exists path`, `not <condition>`, and a bare value (true unless empty, `0` or `false`).

**Batch.** Pipelines are commands like any other, so a batch file can chain them: `automate.pipeline path=scripts/logos.yaml` on one line and `automate.pipeline path=scripts/sort.yaml` on the next.

**Claude.** *"Export every logo in logos/ as PNG at 64, 128, 256 and 512 and show me a contact sheet."* Claude writes the YAML above (or passes it inline as `text`), calls `automate_pipeline` with `dry_run: true` to check the file list, then runs it, then `library_thumbnails` with `preview: true` (or `photo_open` on the sheet) to look at the result.

## Task 2b: build a pipeline without writing one

The files app has a **Pipeline builder** tab (also on the Start screen's Automate card: **Build a pipeline**). It makes the same pipelines as Task 2, by clicking:

1. Tick **Run for every file in** and type a pattern like `photos/*.png` (or pick a folder with **Folder...**, which gives `folder/*`).
2. **+ Add block** and type what it should do ("convert", "tag", "index"). Each block is one command. A new block's file setting starts as `$file`.
3. Click a block to fill in its settings on the right. Settings left empty use the command's default. **Browse** picks a file, and **$file** puts in the file this round is for. Output names can use `$stem`, `$ext` and `$dir`, like `out/$stem.jpg`.
4. Drag blocks (or use the arrows) to change the order. **off** skips a block without losing it, and **x** removes it. Under each block's settings, **Only for files ending in** limits it to some extensions, and **Keep going if this block fails** carries on past errors.
5. **Try it (nothing changes)** is a dry run that lists every command with the files filled in. **Run** does it.
6. **Save** writes the pipeline as JSON (through `automate.pipeline_save`), so it also runs from the command line, a watch folder or Claude. **Open...** loads a pipeline the builder saved, or any JSON pipeline made only of command steps. **Copy as text** puts the JSON on the clipboard.

![the pipeline builder converting a folder of PNGs to JPEGs and indexing them](images/files-pipeline-builder.png)

The blocks are the commands of the app you're in: the files app has convert, library, toolkit and automate. In the all-in-one window (`wetwaresuite gui --all`) every command in the suite is a block. Pipelines saved from the builder run anywhere the `wetwaresuite` command does.

## Task 3: a watch folder that resizes and converts dropped images

`scripts/web.yaml` handles one image: it uses `$file` and `$stem` without a `for_each`, which makes it a **per-file pipeline**: a watch gives it each file in turn, and by hand you pass `vars=file=...`:

```yaml
# web.yaml: one image -> a web JPEG (at most 1600 px wide) and a 256 px thumbnail
description: Resize an image for the web and make a thumbnail.
vars:
  out: web
steps:
  - mkdir: $out/thumbs
  - run: photo.open path=$file
    capture: img
  - if: $img.width > 1600
    run: photo.resize doc=$img width=1600
  - run: photo.save doc=$img path=$out/$stem.jpg quality=85
  - run: photo.resize doc=$img width=256
  - run: photo.save doc=$img path=$out/thumbs/$stem.jpg quality=80
  - run: photo.close doc=$img
```

```sh
$ wetwaresuite automate.pipeline path=scripts/web.yaml
scripts/web.yaml runs once per file: pass vars=file=<path>, or run it with automate.watch

$ wetwaresuite automate.pipeline path=scripts/web.yaml vars=file=photos/big.png,out=web-test
ran web.yaml: 6 commands, 0 failed
opened photos/big.png as img1 ...
resized to 1600x1067 ...
saved web-test/big.jpg ...
resized to 256x171 ...
saved web-test/thumbs/big.jpg ...
closed img1
```

**CLI: a foreground watch.** `automate.watch` polls the folder (every `interval` seconds) and runs the pipeline for each new or changed file once it has stopped changing for `settle` seconds, so half-copied files wait. It blocks and prints a line per file on stderr; stop it with Ctrl+C, or give `duration=` (seconds) or `max_runs=`. Here two files were copied into `drop/` while it ran:

```sh
$ wetwaresuite automate.watch folder=drop pipeline=scripts/web.yaml pattern=*.png,*.jpg vars=out=web-drop interval=0.5 settle=0.5 duration=6 log=watch.log
watching drop every 0.5 s
ok   drop/holiday.png: ran web.yaml: 6 commands, 0 failed
ok   drop/fractal.jpg: ran web.yaml: 5 commands, 0 failed
processed 2 files, 0 failed
  ok   drop/holiday.png: ran web.yaml: 6 commands, 0 failed
  ok   drop/fractal.jpg: ran web.yaml: 5 commands, 0 failed
runs: 2
failed: 0

$ ls -R web-drop
web-drop:
fractal.jpg  holiday.jpg  thumbs

web-drop/thumbs:
fractal.jpg  holiday.jpg

$ cat watch.log
2026-10-16 06-22-58 watching drop every 0.5 s
2026-10-16 06-23-00 ok   drop/holiday.png: ran web.yaml: 6 commands, 0 failed
2026-10-16 06-23-01 ok   drop/fractal.jpg: ran web.yaml: 5 commands, 0 failed
```

(The 1200 px `fractal.jpg` ran one command fewer: its resize to 1600 was skipped by the `if:`.) Files already in the folder when the watch starts are left alone unless `existing=true`; hidden files (names starting with a dot) are always ignored, `exclude=out,*.tmp` ignores more, and `recursive=true` watches subfolders. Write outputs outside the watched folder (or `exclude` them), or each output triggers a run of its own. Each file runs in a fresh session, so documents never leak from one file to the next. A batch script works too: `script=scripts/shrink.txt` instead of `pipeline=`, with `$file`, `$name`, `$stem`, `$ext` and `$dir` set.

**`once=true`** processes every matching file that's there now and stops, which is what cron and Task Scheduler want:

```sh
$ wetwaresuite automate.watch folder=photos pipeline=scripts/web.yaml pattern=*.png,*.jpg once=true vars=out=web
ok   photos/big.png: ran web.yaml: 6 commands, 0 failed
ok   photos/medium.jpg: ran web.yaml: 5 commands, 0 failed
processed 2 files, 0 failed
...
```

Add **`state=done.txt`** and the watch remembers which files it processed (by size and modification time), so the next run only takes new or changed files and a file that failed is tried again. The state file is plain text, one `size time path` line per file; delete it to process everything again.

**Claude: a background watch.** Under MCP a blocking command would freeze the conversation, so Claude uses `automate.watch_start`, which watches in the background and returns at once; `automate.watch_status` shows what it has done and `automate.watch_stop` ends it. *"Watch ~/work/drop and turn anything I drop there into web JPEGs in web-drop."* Claude calls `automate_watch_start` with `folder: "drop"`, `pipeline: "scripts/web.yaml"`, `pattern: "*.png,*.jpg"` and `vars: "out=web-drop"`, and later `automate_watch_status` when you ask how it's going. Background watches end when the MCP server (the Claude session) exits. The same thing from Python (output from a real run):

```
watching drop-py as w1 (every 0.5 s); stop with automate.watch_stop id=w1
1 watch
  w1: drop-py -> scripts/web.yaml, 1 run, 0 failed, 3 scans, since 2026-10-16 06-23-37
    ok   drop-py/dropped.jpg: ran web.yaml: 5 commands, 0 failed
stopped 1 watch
  w1: 1 run
```

## Task 4: from Python

The client in [`python/`](../../python/README.md) is plain Python 3.8+ with no dependencies. It starts `wetwaresuite mcp` and gives you every command as a method, built from the server's own tool list, so new tools appear without updating the client.

```sh
pip install ./python          # or: export PYTHONPATH=/path/to/wetwaresuite/python
```

It finds the binary through `$WETWARESUITE_BIN`, then `PATH`, then the repository's `build/` folder. This script was run in `~/work`:

```python
import shutil
import time
from pathlib import Path

import wetwaresuite

with wetwaresuite.Suite() as suite:
    # every command is a method; its docstring lists the parameters
    print(len(suite.commands()), "commands, e.g.", suite.commands()[:3])
    print(suite.photo.resize.__doc__.splitlines()[0])

    # results are structured: .doc, fields by name, previews as bytes
    img = suite.photo.open(path="photos/medium.jpg").doc
    r = suite.photo.resize(doc=img, width=300, preview=True)
    print(r.message, "|", r["width"], "x", r["height"], "|", len(r.image), "bytes of PNG preview")
    r.save_image("medium_preview.png")
    suite.photo.save(doc=img, path="medium_300.jpg", quality=80)

    # a failed command raises CommandError with the suite's message
    try:
        suite.photo.open(path="nope.png")
    except wetwaresuite.CommandError as e:
        print("error:", e)

    # pipelines from Python: vars can be a dict
    r = suite.automate.pipeline(path="scripts/web.yaml", vars={"file": "photos/big.png", "out": "web-py"})
    print(r.message.splitlines()[0], "| ran", r["ran"])

    # a background watch while Python carries on
    Path("drop-py").mkdir(exist_ok=True)
    w = suite.automate.watch_start(folder="drop-py", pipeline="scripts/web.yaml", pattern="*.png,*.jpg",
                                   vars={"out": "web-py"}, interval=0.5, settle=0.5)
    print(w.message)
    shutil.copy("photos/medium.jpg", "drop-py/dropped.jpg")
    for _ in range(40):
        if suite.automate.watch_status()["runs"] >= 1:
            break
        time.sleep(0.25)
    print(suite.automate.watch_status().message)
    print(suite.automate.watch_stop().message)

    # lists come back as Python data (json=true is set for you)
    for script in suite.automate.list_scripts()["scripts"]:
        print(script["kind"], script["path"], "needs:", script["needs"])

# one-off batch text in its own process
out = wetwaresuite.run_batch("photo.open path=photos/medium.jpg -> a\nphoto.info doc=$a\n")
print(out.output.splitlines()[0])
```

```
201 commands, e.g. ['automate.list_scripts', 'automate.pipeline', 'automate.run_script']
Scale the whole image. Give width, height or both; with only one, the other keeps the aspect ratio.
resized to 300x225 | 300 x 225 | 92631 bytes of PNG preview
error: photo.open: file not found: nope.png
ran web.yaml: 6 commands, 0 failed | ran 6
watching drop-py as w1 (every 0.5 s); stop with automate.watch_stop id=w1
1 watch
  w1: drop-py -> scripts/web.yaml, 1 run, 0 failed, 3 scans, since 2026-10-16 06-23-37
    ok   drop-py/dropped.jpg: ran web.yaml: 5 commands, 0 failed
stopped 1 watch
  w1: 1 run
pipeline scripts/logos.json needs: []
pipeline scripts/logos.yaml needs: []
pipeline scripts/sort.yaml needs: []
script scripts/thumb.txt needs: ['input', 'out', 'width']
pipeline scripts/web.yaml needs: ['file']
opened photos/medium.jpg as img1
```

(The command count depends on which tools your build has.) The pieces:

- `suite.<tool>.<command>(...)` takes the command's parameters as keyword arguments; a single positional argument fills `doc`. `Path` values become strings, lists become arrays (or comma lists), dicts become JSON.
- A `Result` has `ok`, `message` (what a person reads), `text` (the message plus every field, as the CLI prints it), `data` (the fields; `r["width"]` works too, and JSON list fields are decoded), `doc`, `images` / `image` (previews as PNG bytes), `save_image()` and `attachments`.
- `Suite(raise_errors=False)` returns failed results with `ok == False` instead of raising; `Suite(tools=["photo", "convert"])` serves fewer tools; `timeout=` limits each command.
- `suite.undo()`, `suite.redo()`, `suite.history()` and `suite.documents()` work on the session's documents, which stay open between calls, like under Claude.
- `wetwaresuite.run_batch(text)` runs batch text in a fresh `wetwaresuite batch` process and returns `ok`, `output` and `errors`.

## Task 5: on a schedule: cron and Task Scheduler

There's no daemon: a scheduler runs `wetwaresuite batch` (or a single command) and it exits when it's done. `once=true` and `state=` turn a watch into a scheduled job.

**Batch.** `nightly.txt`:

```
# nightly.txt: process whatever arrived in drop/ today, then rebuild the logo exports
automate.watch folder=drop pipeline=scripts/web.yaml pattern=*.png,*.jpg vars=out=web-drop state=drop-done.txt once=true quiet=true
automate.pipeline path=scripts/logos.yaml
```

```sh
$ wetwaresuite batch nightly.txt
processed 2 files, 0 failed
  ok   drop/fractal.jpg: ran web.yaml: 5 commands, 0 failed
  ok   drop/holiday.png: ran web.yaml: 6 commands, 0 failed
runs: 2
failed: 0
ran logo exports: 14 commands, 0 failed
...

$ wetwaresuite batch nightly.txt          # the next night, nothing new
processed 0 files, 0 failed
runs: 0
...
```

`quiet=true` drops the per-file lines on stderr (the summary is still printed). A failing line makes `wetwaresuite batch` exit with a non-zero code, which cron mails to you and Task Scheduler shows as the last run result; add `--keep-going` to run the remaining lines anyway.

**cron** (Linux, macOS): `crontab -e`, then

```
# every night at 02:30
30 2 * * * cd $HOME/work && wetwaresuite batch nightly.txt >> $HOME/work/nightly.log 2>&1
# every 10 minutes, just the drop folder
*/10 * * * * cd $HOME/work && wetwaresuite automate.watch folder=drop pipeline=scripts/web.yaml pattern=*.png,*.jpg vars=out=web-drop state=drop-done.txt once=true quiet=true
```

cron runs with a minimal `PATH`: use the full path to `wetwaresuite` if it isn't in `/usr/bin` or `/usr/local/bin`.

**Task Scheduler** (Windows), from a Command Prompt:

```bat
schtasks /Create /TN "wetwaresuite nightly" /SC DAILY /ST 02:30 ^
  /TR "cmd /c cd /d C:\work && C:\Tools\wetwaresuite\wetwaresuite.exe batch nightly.txt >> nightly.log 2>&1"

schtasks /Create /TN "wetwaresuite drop folder" /SC MINUTE /MO 10 ^
  /TR "cmd /c cd /d C:\work && C:\Tools\wetwaresuite\wetwaresuite.exe automate.watch folder=drop pipeline=scripts/web.yaml pattern=*.png,*.jpg vars=out=web-drop state=drop-done.txt once=true quiet=true"

schtasks /Run /TN "wetwaresuite nightly"      &:: try it now
schtasks /Delete /TN "wetwaresuite nightly"   &:: remove it
```

Or keep a watch running all the time: `wetwaresuite automate.watch folder=drop pipeline=scripts/web.yaml state=drop-done.txt` as a systemd user service, a launchd agent, or a Task Scheduler task triggered "At log on". With `state=`, files dropped while it was stopped are picked up when it starts again.

**Claude.** *"Set up the drop folder to be processed every 10 minutes."* Claude can't edit your crontab (there's no shell command), so it writes `nightly.txt` or the command line, runs it once with `automate_watch` and `once: true` to show it works, and gives you the `crontab` or `schtasks` line to add.

## Task 6: driving pipelines from Claude

Claude sees `automate_list_scripts`, `automate_run_script`, `automate_pipeline`, `automate_pipeline_save`, `automate_watch_start`, `automate_watch_status`, `automate_watch_stop` and `automate_watch` over MCP. A good routine, and the one Claude follows when asked to "run my export pipeline":

1. `automate_list_scripts` (read-only) to find the script and the variables it needs.
2. `automate_pipeline` with `dry_run: true` to see the expanded file list and command lines without touching anything.
3. The real run; `verbose: true` when it needs the full output of each command, for example to read sizes back.
4. A look at the result: `library_thumbnails` with `preview: true`, or `photo_open` on an output with `preview: true`.

Claude can also write pipelines for you: *"Write a pipeline that turns every .wav in recordings/ into an MP3 next to it and save it as scripts/mp3.yaml"*. It writes the file, then dry-runs it so you see what it would do before it does anything. Since every step is a suite command and there is no shell step, a pipeline can't do anything Claude couldn't already do with the individual tools.

## Pipeline reference

A pipeline is JSON, or YAML: mappings, `- ` lists, `[a, b]` flow lists, quoted and plain strings, `#` comments, and `|` / `>` block text. (It's YAML-lite: no anchors, tags or multi-document files; errors say the line number.) The top level is a list of steps, or a mapping with:

| key | meaning |
| --- | --- |
| `name`, `description` | shown by list_scripts and in the result |
| `vars` | default variables (`vars=` on the command line wins) |
| `keep_going` | `true`: carry on after failed steps |
| `steps` | the steps |

A step is a command line string (`- photo.new width=10 height=10`) or a mapping with any of:

| key | meaning |
| --- | --- |
| `run` | a command line, as in a batch script |
| `command` + `args` | the same, structured: `command: photo.resize`, `args: {doc: $img, width: 800}` |
| `capture` (or `as` on a run) | keep the result as `$name`; its fields as `$name.width`, also `$name.message`, `$name.ok` |
| `for_each` | a glob, or a list of globs; runs `steps:` (or the step's own `run:`) once per file, setting `$file` (renamed with `as:`) and `$index`, `$count` |
| `for` | a list of values (`[64, 128]` or `"64,128"`); runs once per value as `$item` (or `as:`) |
| `steps` (or `do`) | the body of a loop, or a group of steps |
| `if`, `unless` | run the step only when the condition holds (or doesn't); inside loops it's checked per file |
| `ext` | `png,jpg` or `[png, jpg]`: shorthand for `if: $file.ext in png,jpg` |
| `set` | `{name: value}`: set variables |
| `mkdir` | create a folder (and its parents) |
| `echo` | add a line to the output |
| `script` (+ `vars`) | run a batch script with these variables |
| `pipeline` (+ `vars`) | run another pipeline |
| `on_error` | `continue`: a failure here doesn't stop the pipeline (it still counts as failed) |
| `name` | a heading for the step in the output |

## Parameter reference

| command | what it does | parameters |
| --- | --- | --- |
| `automate.run_script` | run a batch script with variables | `path` or `text`, `vars`, `keep_going` (false), `dry_run` (false), `verbose` (false) |
| `automate.pipeline` | run a JSON/YAML pipeline | `path` or `text`, `vars`, `keep_going` (false), `dry_run` (false), `verbose` (false) |
| `automate.pipeline_save` | check a pipeline and save it to a file | `path`, `text` (both required), `overwrite` (false) |
| `automate.watch` | watch a folder in the foreground (blocks) | the watch parameters, plus `duration` (0 = until Ctrl+C), `max_runs` (0 = no limit), `once` (false), `quiet` (false) |
| `automate.watch_start` | watch in the background (MCP, Python) | the watch parameters; returns `id` |
| `automate.watch_status` | runs, failures and recent lines | `id` (default: all) |
| `automate.watch_stop` | stop a background watch | `id` (default: all) |
| `automate.list_scripts` | scripts and pipelines in a folder | `folder` (`$WETWARESUITE_SCRIPTS`, else `./scripts`), `recursive` (false), `json` (false: a `scripts` field with path, kind, description, needs, problem) |

The watch parameters: `folder` (required); `script` or `pipeline` (one of them); `pattern` (`*.png,*.jpg`; default every file); `exclude` (wildcards for files or subfolders); `recursive` (false); `interval` (2 s); `settle` (1 s); `existing` (false: also process files there at the start); `vars`; `log` (append a line per file); `state` (remember processed files; implies `existing=true`); `keep_going` (false).

Results carry `ran` and `failed` counts (watches: `runs` and `failed`). Over MCP the tool names use underscores (`automate_pipeline`); `list_scripts` and `watch_status` are read-only.

## Limitations

- The pipeline builder shows command steps, one optional `for_each` and `ext:` filters. Pipelines with `run:` lines, `vars:`, nested loops, conditions or captures open as text instead (edit them in any editor, or the code app).
- No shell step, by design: scripts and pipelines can only run suite commands. To call other programs, run them from a shell script that calls `wetwaresuite` (or from Python).
- Watching is by polling (every `interval` seconds), not OS file events: simple and the same on every platform, but a 2 s interval means up to a few seconds' delay, and huge folders with `recursive=true` cost a scan each time. Files are processed one at a time, in name order.
- A file is processed when its size and modification time stop changing for `settle` seconds; a program that pauses for longer while writing can get its file processed half-written. Raise `settle` for slow network copies.
- Background watches (`watch_start`) live in the process that started them: they end when the MCP session or the Python `Suite` closes. For a permanent watch, run `automate.watch` as a service or from a scheduler with `once=true state=...`.
- The YAML reader covers the common subset (mappings, lists, flow lists, quotes, comments, block text); anchors, aliases, tags and multi-document files are not supported. Use JSON when in doubt.
- Pipeline paths are relative to the current folder, not the pipeline file (use `$pipeline_dir`). The check for missing variables is made from the text before anything runs: a `$name` set only by a nested script or pipeline counts as missing, and one that's still undefined when its step runs is left as it is. `dry_run` and `list_scripts` show what's needed. A value containing `$` followed by the name of an earlier capture in the same script is substituted again by the batch runner.
- Documents opened by a script stay open in the session afterwards (useful in a Claude session, where you can carry on with them); close them in the script if it runs many times. Watches use a fresh session per file, so they don't accumulate.
- Nesting (scripts calling scripts or pipelines) stops at 8 levels.
- The Python client talks to one `wetwaresuite mcp` process; commands run one at a time. Use several `Suite`s (each its own process) for parallel work.
