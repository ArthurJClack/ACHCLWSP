# Files: a file manager with undo

`explorer` is a file manager. It can:

- browse folders, and copy, move, rename and delete;
- rename many files at once, with find and replace or numbering;
- find files by name, contents, size or age;
- show what is taking up space, find duplicate files, and compare two folders;
- make checksums;
- open zip, tar and tar.gz archives, and make them.

Every change can be undone:

- **Deleting** moves things to your computer's trash (Recycle Bin, Trash, or the desktop trash on Linux). Undo, or `explorer.restore`, brings them back.
- **Copies, moves, renames and new folders** can be taken back with undo.
- **Extracting** an archive can be undone too: undo moves what came out to the trash.

`explorer` is part of the system app (`wetwaresuite-system`) and the all-in-one `wetwaresuite`. Everything happens on your own computer; nothing goes online. Each task is shown in the window, on the command line (CLI), in batch scripts (see [headless and batch](headless-and-batch.md)) and from Claude (see [mcp.md](mcp.md)). Outputs are copied from real runs on Linux, using a sample folder `/tmp/demo/photos`.

## Naming files

Wherever a command takes `paths=`, you can give one path or several. Separate several paths with `|` or new lines. Spaces are not separators, because file names can contain spaces. Relative paths start from the current folder. `~` is your home folder.

## The Files window

Start the window with `wetwaresuite-system` and click **Open** on the **Files** card (called **File manager** in the advanced view). To open a folder straight away:

```sh
wetwaresuite-system ~/Pictures
```

- **Places** on the left: your home, desktop, documents, downloads, pictures, music, videos, temporary files, every drive, and the **Trash**.
- **Top bar**: **<** and **>** go back and forward. **Up** goes to the folder this one is in. The path box is next to them: type a path and press Enter to go there.
- **The list**: Name, Size, Modified and Type columns. Click a heading to sort by it, and click again to reverse. Narrow windows drop the Type and Modified columns first.
  - **Selecting.** Click to select. **Ctrl+click** adds a file to the selection, and **Shift+click** selects a range.
  - **Opening.** Double-click to open. Folders open in the list. Archives open as a list of what is inside, with **Extract here**. Pictures, documents and other files that another tool in the app knows open in that tool. Text and code open in the code editor. Anything else opens in its usual app.
  - **Right-click menu**: Open, Open with its usual app, Copy, Cut, Move to trash, Extract here and Info.
- **Buttons**: **New folder**, **New file**, **Copy**, **Cut**, **Paste**, **Rename**, **Delete** (to the trash), **Zip** (pack the selected files), **Extract** (when an archive is selected), **Terminal here** and **Open as project** (opens the folder in the code editor).
- **The advanced view** (View > Show advanced options) adds:
  - **Hidden**, which shows dot-files;
  - **Find...**: search below this folder by name (`*.pdf`) and by text inside, with **Find in subfolders**;
  - **Checksum** for the selected files;
  - **Folder size**: what is biggest in this folder.
- **Preview** on the right, when there is room: the first lines of a text file, a picture, or a folder's contents.
- **Keys**:
  - **Delete** moves the selection to the trash.
  - **Backspace** goes up a folder.
  - **Ctrl+C**, **Ctrl+X** and **Ctrl+V** copy, cut and paste.
  - **F5** refreshes the list.
  - **Ctrl+Z** undoes the last change, even a delete.
- **Trash**: everything deleted from the suite, with where it came from, when and how big. Each item has a **Restore** button. **Empty trash** asks before it deletes for good.

## Task 1: look around

```
$ wetwaresuite explorer.list folder=/tmp/demo/photos/raw sort=size
/tmp/demo/photos/raw
    2.9 KB  2026-10-17 00:03  copy-of-first.jpg
    2.9 KB  2026-10-17 00:03  IMG_0001.jpg
    5.9 KB  2026-10-17 00:03  IMG_0002.jpg
    8.8 KB  2026-10-17 00:03  IMG_0003.jpg

$ wetwaresuite explorer.info path=notes.txt
/tmp/demo/photos/notes.txt
type: Plain text
size: 37 bytes
modified: 2026-10-17 00:03
permissions: rw-r--r--

$ wetwaresuite explorer.preview path=notes.txt
notes.txt
Plain text, 37 bytes
Trip notes
Day 1: beach
Day 2: hills
```

More options:

- `explorer.list` takes `sort=name|size|date|type`, `descending=true` and `hidden=true`.
- `explorer.preview` gives the first `lines=` of a text file. For a picture, it gives a thumbnail (`size=`) as an attachment. For other files, it gives a hex dump of the first `bytes=`.
- `explorer.places` lists the shortcuts the window shows on the left.

## Task 2: find things, and what takes up space

```
$ wetwaresuite explorer.search folder=/tmp/demo/photos name=*.jpg min_size=5000
2 matches
/tmp/demo/photos/raw/IMG_0003.jpg
/tmp/demo/photos/raw/IMG_0002.jpg

$ wetwaresuite explorer.search folder=/tmp/demo/photos contains=hills
1 match
/tmp/demo/photos/notes.txt

$ wetwaresuite explorer.size folder=/tmp/demo/photos
/tmp/demo/photos: 21 KB in 5 files
     21 KB  99.8%  raw/
  37 bytes   0.2%  notes.txt

$ wetwaresuite explorer.duplicates folder=/tmp/demo/photos
1 set of duplicates, 2.9 KB in extra copies
2.9 KB x 2:
  /tmp/demo/photos/raw/IMG_0001.jpg
  /tmp/demo/photos/raw/copy-of-first.jpg
```

`explorer.search` can combine these filters:

- `name=` (wildcards `*` and `?`);
- `contains=` (text inside; `regex=true` for a pattern);
- `kind=file|folder`;
- `min_size` and `max_size` in bytes;
- `newer_days` and `older_days`.

`explorer.duplicates` compares sizes first and then contents, so it is quick on big folders. `min_size=` skips small files.

## Task 3: tidy a folder (rename, move, delete, zip), and undo

```
# tidy.txt: rename photos in order, file them, delete a copy (and change your mind), zip them, check the zip
explorer.batch_rename folder=/tmp/demo/photos/raw files=IMG_*.jpg number="beach-{n}.jpg" start=1
explorer.new_folder path=/tmp/demo/photos/2026-beach
explorer.move paths="/tmp/demo/photos/raw/beach-1.jpg|/tmp/demo/photos/raw/beach-2.jpg|/tmp/demo/photos/raw/beach-3.jpg" to=/tmp/demo/photos/2026-beach
explorer.delete paths=/tmp/demo/photos/raw/copy-of-first.jpg
explorer.trash
undo
explorer.list folder=/tmp/demo/photos/raw
explorer.archive paths=/tmp/demo/photos/2026-beach output=/tmp/demo/beach.zip
explorer.archive_list path=/tmp/demo/beach.zip
explorer.checksum paths=/tmp/demo/beach.zip
explorer.extract path=/tmp/demo/beach.zip to=/tmp/demo/check
explorer.compare a=/tmp/demo/photos/2026-beach b=/tmp/demo/check/2026-beach
```

```
$ wetwaresuite batch tidy.txt
renamed 3 files
IMG_0001.jpg -> beach-1.jpg
IMG_0002.jpg -> beach-2.jpg
IMG_0003.jpg -> beach-3.jpg

made /tmp/demo/photos/2026-beach
moved beach-1.jpg, beach-2.jpg, beach-3.jpg to /tmp/demo/photos/2026-beach

moved copy-of-first.jpg to the trash
1792195451-00001  /tmp/demo/photos/raw/copy-of-first.jpg  (2.9 KB, deleted 2026-10-17 00:04)

undone
/tmp/demo/photos/raw
    2.9 KB  2026-10-17 00:04  copy-of-first.jpg

packed 3 files into /tmp/demo/beach.zip (18 KB, zip)
/tmp/demo/beach.zip (zip)
            2026-10-17 00:04  2026-beach/
    2.9 KB  2026-10-17 00:03  2026-beach/beach-1.jpg
    5.9 KB  2026-10-17 00:03  2026-beach/beach-2.jpg
    8.8 KB  2026-10-17 00:03  2026-beach/beach-3.jpg
4 items, 18 KB unpacked

4c8319d6bb17944bc4229229a094f05ccf94b45eacb341a75424925379bcc2c1  /tmp/demo/beach.zip

unpacked 3 files into /tmp/demo/check
the folders have the same files (3 files)
```

**Renaming many files.** `explorer.batch_rename` takes:

- `find` and `replace` (with `regex=true`, `$1` puts back a group);
- `number=` with `{n}`, or `{n:3}` for 001. The extension is kept, so `"beach {n}"` and `"beach-{n}.jpg"` give the same names.

Always try `dry_run=true` first. Swapping names (a to b and b to a) works, because it goes through temporary names.

**Archives.**

- **Formats.** `explorer.archive` makes `zip` (the default), `tar` or `tar.gz`; `level=` sets zip compression from 0 to 9. `explorer.extract` reads zip, tar, tar.gz/tgz and single `.gz` files.
- **Where things go.** Without `to=`, everything is unpacked into a new folder next to the archive.
- **Picking files.** `only=` picks out some of the files.
- **Safety.** Files that would land outside the target folder ("zip slip") are refused.

**Checksums.** `explorer.checksum` gives `sha256` (the default), `sha1`, `md5` or `crc32`. With `expected=`, it says whether the file matches, which is handy for checking a download.

`explorer.compare` compares two files, or two folders file by file.

## Task 4: the trash

```
$ wetwaresuite explorer.delete paths=/tmp/demo/photos/notes.txt
moved notes.txt to the trash
$ wetwaresuite explorer.trash
1792195516-00001  /tmp/demo/photos/notes.txt  (37 bytes, deleted 2026-10-17 00:05)
$ wetwaresuite explorer.restore id=1792195516-00001
restored /tmp/demo/photos/notes.txt
$ wetwaresuite explorer.empty_trash older_days=30
deleted 0 items for good
```

Deleting uses your computer's own trash: the Recycle Bin on Windows, the Trash on macOS, and the desktop trash on Linux (the freedesktop one that GNOME, KDE and others share). So a file deleted in the suite can also be restored from your file manager. The suite keeps a note of each item (in `~/.local/share/wetwaresuite/trash` on Linux), so `explorer.trash` lists it as "in the computer's trash" and `explorer.restore` puts it back.

`explorer.delete system=false`, or `WETWARESUITE_TRASH=suite` in the environment, uses the suite's own trash folder instead. That is handy on servers that have no desktop trash. A file the computer's trash refuses (another drive, for example) goes to the suite's trash, and the result's `rows` say which one each file went to.

- **Restoring.** `explorer.restore` puts a file back where it was, or into `to=`.
- **Emptying.** `explorer.empty_trash` without `older_days` deletes everything in the trash for good, and that one can't be undone.

## Claude

```sh
claude mcp add wetwaresuite-files -s user -- wetwaresuite-system mcp --tools explorer
```

Example prompts and the tools Claude calls:

- *"What's eating my disk in ~/Downloads?"*: `explorer_size` with `top=20`, then `explorer_duplicates`.
- *"Rename my holiday photos to 'Rome 001.jpg' and so on, show me first"*: `explorer_batch_rename` with `dry_run=true`, then for real.
- *"Zip up the report folder and give me the sha256"*: `explorer_archive`, then `explorer_checksum`.
- *"Delete every .tmp file under ~/projects older than a week"*: `explorer_search` with `name=*.tmp older_days=7`, then `explorer_delete` with the paths.

Deletes go to the trash, so a mistaken delete by Claude can be undone in the same session or restored later.

## Parameter reference

| Command | What it does | Main parameters |
|---|---|---|
| `explorer.list` | a folder's contents | `folder`, `sort`, `descending`, `hidden` |
| `explorer.info`, `explorer.preview` | about one file | `path`, `lines`, `bytes`, `size` |
| `explorer.places` | shortcuts and drives | |
| `explorer.search` | find files | `folder`, `name`, `contains`, `regex`, `kind`, `min_size`, `max_size`, `newer_days`, `older_days`, `hidden`, `limit` |
| `explorer.size`, `explorer.duplicates` | space used | `folder`, `top`, `min_size` |
| `explorer.copy`, `explorer.move` | copy or move into a folder | `paths`, `to`, `overwrite` |
| `explorer.rename`, `explorer.batch_rename` | rename one or many | `path`, `name`; `folder`, `files`, `find`, `replace`, `regex`, `number`, `start`, `dry_run` |
| `explorer.new_folder`, `explorer.new_file` | make things | `path`, `text`, `overwrite` |
| `explorer.delete`, `explorer.trash`, `explorer.restore`, `explorer.empty_trash` | the trash | `paths`, `id`, `to`, `older_days` |
| `explorer.archive`, `explorer.archive_list`, `explorer.extract` | zip and tar | `paths`, `output`, `format`, `level`, `path`, `to`, `only`, `overwrite` |
| `explorer.checksum`, `explorer.compare` | check files | `paths`, `algorithm`, `expected`, `a`, `b` |
| `explorer.open` | open in its usual app | `path` |

## Limitations

- Files deleted outside the suite don't show up in `explorer.trash`, even though they sit in the same computer trash.
- Emptying the computer's trash from your file manager removes suite items too; the suite drops its note on the next look.
- Zip archives over 4 GB, or with more than 65,535 files, need ZIP64, which isn't supported. Use `tar.gz` for those. Encrypted zips can't be opened.
- Tar archives keep files and folders. Links and device files are skipped.
- There is no network browsing (SMB, FTP, cloud drives), apart from what your system already mounts as a folder.
