# Library: an asset browser for every format

`library` keeps a catalog of the files on a drive or in a project folder, so you can find them again by what they are instead of where you put them: "every 4k texture I haven't used", "the photos rated 4 or more from the beach shoot", "everything bigger than 1 GB that changed this week". It reads every format the suite knows about (images, camera raw, SVG, meshes, audio, video, PDFs, office files and anything else by its magic bytes) and adds what you know about each file: tags, a star rating, a colour label and notes. It finds exact duplicates by content hash, similar-looking images by perceptual hash, makes thumbnails and numbered contact sheets, and keeps saved searches as collections.

There is no GUI yet; the GUI that comes later will call these same commands. Each task below is shown three ways:

- **CLI**: single `wetwaresuite library.*` commands. The catalog lives in a file, so (unlike photo documents) one-shot commands build on each other.
- **Batch**: a script for multi-step work with `-> name` captures and `$name` reuse (see [headless and batch](headless-and-batch.md)).
- **Claude**: a prompt for Claude over MCP and the tools it calls (see [mcp.md](mcp.md)). `wetwaresuite mcp --tools library` serves just the library.

Every example was run against the real binary on the sample drive described below; outputs are copied from those runs (the drive was mounted at `/mnt/drive`; long outputs are shortened where marked).

## How the library works

**The catalog.** `library.index root=<folder>` walks the folder and writes one line of JSON per file to `.wetwaresuite-library.json` in that folder (or wherever `library=` says). Each entry holds the path, size, modification time, a 64-bit content hash, the type detected from the file's contents (the same detection as `toolkit.inspect`), and whatever is cheap to measure: pixel size from image headers, the canvas of an SVG, the bounding box and triangle count of a mesh, the duration and frame size of audio and video (through `ffprobe`, if it's installed) and the page count of a PDF. Paths inside the catalog's folder are stored relative to it, so you can move or remount the whole drive and the catalog still works. It's a plain text file: you can read it, diff it, back it up or put it under version control.

**Which catalog a command uses.** Every command takes `library=` (the catalog file, or the folder it's in). Without it, a command uses the catalog the session used last, else looks for `.wetwaresuite-library.json` in the current folder and its parents, the way git finds a repository. So the easiest way to work is `cd` into the drive and run commands from there; scripts and Claude can pass `library=` explicitly.

**Incremental indexing.** Re-running `library.index` (or `library.rescan`, which re-indexes every folder the catalog covers) only reads files whose size or modification time changed, so it is fast on a drive that has been indexed before. Files that disappeared are dropped; files that were moved or renamed outside the library are recognised by their content hash and keep their tags, rating, label and notes. There is no background file watching: rescan whenever you want the catalog fresh (by hand, from cron, or from `automate.watch`).

**Hashes.** The content hash is FNV-1a 64 of the whole file for files up to 2 MiB, and of the first and last MiB plus the size for bigger ones, which is enough to tell files apart while keeping indexing of large videos instant. `library.duplicates` re-reads large candidates in full before calling them copies, and `full_hash=true` on index hashes everything.

**Thumbnails.** `library.thumbnails` renders PNG thumbnails (256 px by default) into `.wetwaresuite-thumbs/` next to the catalog, named by content hash, so moved files keep their thumbnails. Images and textures are drawn by the raster engine, SVGs by the vector tool, meshes by the mesh renderer, video frames and audio waveforms by `ffmpeg`, and the first page of a PDF by `pdftoppm` (from poppler), each when installed. Contact sheets lay thumbnails out on a numbered grid, and `preview=true` attaches one to the result so Claude can see what it found.

**Undo.** Tag, untag, rate, label, note and forget are undoable (in a batch script or an MCP session): undo puts back exactly what they replaced. Moving, renaming and copying files are not; use `dry_run=true` first.

## The sample drive

```
/mnt/drive
├── audio/theme.wav                 12 s
├── docs/handbook.pdf, todo.md
├── logos/badge.svg, wordmark.svg
├── models/crate.stl
├── photos/2024-beach/IMG_0001..3.jpg
├── photos/2024-city/IMG_0101.jpg (portrait), IMG_0102.jpg (last modified in 2023)
├── photos/inbox/IMG_0002.jpg, "IMG_0001 (1).jpg"   copies of two beach photos
├── photos/inbox/beach_small.jpg    IMG_0003 scaled down to 800x600
├── textures/rock_4k.jpg, sky_4k.jpg (4096x4096), noise_4k.png (4096x2048), grid_2k.png
└── video/intro.mp4                 1280x720, 6 s
```

## Task 1: index a drive

**CLI.** From the drive's folder:

```sh
$ cd /mnt/drive
$ wetwaresuite library.index root=.
indexed /mnt/drive: 19 files (19 new, 0 updated, 0 unchanged, 0 removed) in 0.20 s; library has 19 files
library: /mnt/drive/.wetwaresuite-library.json
files: 19
added: 19
updated: 0
unchanged: 0
removed: 0
moved: 0
errors: 0
total: 19

$ wetwaresuite library.index root=.
indexed /mnt/drive: 19 files (0 new, 0 updated, 19 unchanged, 0 removed) in 0.01 s; library has 19 files
...
```

The second run reads nothing: every file's size and time are unchanged. Hidden files and folders (names starting with a dot) are skipped unless `hidden=true`; `include=*.jpg,*.png` and `exclude=cache,*.tmp` narrow it further, `recursive=false` stays in the top folder, and `probe=false` skips the slower measurements (ffprobe, PDF pages, loading meshes) for a first pass over a huge drive. Indexing runs on several threads (`threads=` to choose).

To keep the catalog somewhere else (a drive that's read-only, or one catalog for several drives), give it a path; indexing another folder into the same catalog adds it as another root:

```sh
wetwaresuite library.index root=/mnt/drive library=~/catalogs/assets.jsonl
wetwaresuite library.index root=/mnt/backup library=~/catalogs/assets.jsonl
```

`library.stats` summarises what's there:

```sh
$ wetwaresuite library.stats
19 files, 7.2 MB
by type:
  image: 12 files, 4.2 MB
  video: 1 files, 1.9 MB
  audio: 1 files, 1.0 MB
  document: 2 files, 9.7 KB
  cad: 1 files, 684 B
  vector: 2 files, 369 B
top extensions:
  jpg: 10 files, 3.1 MB
  png: 2 files, 1.1 MB
  ...
tagged 0, rated 0, labelled 0, with notes 0
duplicates: 2 groups, 280.8 KB reclaimable
collections: 0
roots:
  /mnt/drive
last indexed: 2026-10-16 06:05 UTC
catalog: /mnt/drive/.wetwaresuite-library.json
...
```

**Batch.** A nightly refresh is two lines (see Task 9 for a fuller weekly script):

```
# refresh.txt
library.rescan library=/mnt/drive
library.stats library=/mnt/drive
```

**Claude.** *"Index my asset drive at /mnt/drive and tell me what's on it."* Claude calls `library_index` with `root: "/mnt/drive"`, then `library_stats`.

## Task 2: search

`library.search` takes a query; everything else in the library builds on the same language, so it's worth a look. Words are ANDed together.

```sh
$ wetwaresuite library.search query="type:image width>=3840 sort:-size"
3 matches
/mnt/drive/textures/rock_4k.jpg  [image] 1.6 MB 4096x4096
/mnt/drive/textures/noise_4k.png  [image] 1.0 MB 4096x2048
/mnt/drive/textures/sky_4k.jpg  [image] 204.4 KB 4096x4096
count: 3
total: 3

$ wetwaresuite library.search query="type:video,audio"
2 matches
/mnt/drive/audio/theme.wav  [audio] 1.0 MB 0:12
/mnt/drive/video/intro.mp4  [video] 1.9 MB 1280x720 0:06

$ wetwaresuite library.search query="IMG_* -in:photos/inbox sort:name" limit=3
5 matches, showing 3
/mnt/drive/photos/2024-beach/IMG_0001.jpg  [image] 77.0 KB 1600x1200
/mnt/drive/photos/2024-beach/IMG_0002.jpg  [image] 203.8 KB 1600x1200
/mnt/drive/photos/2024-beach/IMG_0003.jpg  [image] 28.9 KB 1600x1200

$ wetwaresuite library.search query="modified>30d"
1 match
/mnt/drive/photos/2024-city/IMG_0102.jpg  [image] 677.4 KB 1600x1200

$ wetwaresuite library.search query="size>1mb sort:-size"
4 matches
/mnt/drive/video/intro.mp4  [video] 1.9 MB 1280x720 0:06
/mnt/drive/textures/rock_4k.jpg  [image] 1.6 MB 4096x4096
/mnt/drive/audio/theme.wav  [audio] 1.0 MB 0:12
/mnt/drive/textures/noise_4k.png  [image] 1.0 MB 4096x2048

$ wetwaresuite library.search query=orientation:portrait
1 match
/mnt/drive/photos/2024-city/IMG_0101.jpg  [image] 30.5 KB 1200x1600
```

Each line shows the type, size, dimensions or duration, stars for the rating, the label in brackets and `#tags`. `limit` defaults to 100 (`limit=0` for everything) and `offset` pages through long results. `json=true` adds an `items` field with every result as a JSON list (path, name, type, format, ext, size, modified, hash, width, height, duration, pages, triangles, bounds, tags, rating, label, notes), for scripts; the Python client turns it on for you.

`library.info` shows everything about one file. A file that isn't in the library is read on the spot (and not added):

```sh
$ wetwaresuite library.info path=models/crate.stl
/mnt/drive/models/crate.stl
type: cad (STL mesh)
size: 684 B (684 bytes)
modified: 2026-10-16 06:05
dimensions: 12 tris
bounds: 40 x 40 x 20
hash: 9ba1e7154ceee48f
...
```

`preview=true` on `library.info` attaches the file's thumbnail; on `library.search` it attaches a contact sheet of the first 60 results.

### The query language

| you write | it matches |
| --- | --- |
| `sunset` | "sunset" anywhere in the file name, a tag or the notes (case doesn't matter) |
| `"red car"` | a phrase with a space in it |
| `*.png`, `IMG_00??.jpg` | file names matching the wildcard |
| `type:image` | the detected type: `image`, `raw`, `vector`, `cad`, `3d`, `texture`, `audio`, `video`, `document`, `spreadsheet`, `slides`, `archive`, `font`, `data`, `code`, `executable`, `binary` |
| `type:picture` | any raster picture: image, raw or texture (`.tga`, `.dds` count as textures) |
| `type:mesh` | 3D models by extension: stl, obj, ply, off, gltf, glb, 3mf, fbx, dae, usd, usdz, blend |
| `type:image,video` | any of a comma list (works for every field) |
| `ext:png` | the extension |
| `tag:beach`, `tag:proj-*` | a tag (wildcards allowed) |
| `label:red` | a colour label |
| `format:jpeg` | text in the format name ("JPEG image") |
| `name:logo`, `name:*_v2.*` | text or a wildcard in the file name only |
| `path:clients/acme` | text or a wildcard anywhere in the full path |
| `in:photos/2024-beach` | inside a folder (relative to where you run it, or absolute) |
| `has:tags`, `has:notes`, `has:rating`, `has:label`, `has:duration` | files that have one |
| `notes:licence` | text in the notes |
| `hash:8460237` | content hash starting with these digits |
| `orientation:landscape` | landscape, portrait or square (files with pixel sizes) |
| `collection:best` | files in a saved collection (Task 6) |
| `rating>=3`, `rating=0` | star rating, 0 to 5 |
| `size>10mb`, `size<=500k` | size; units b, k/kb, m/mb, g/gb, t/tb (1024-based) |
| `width>=3840`, `height<1080` | pixel size |
| `megapixels>12` (or `mp>12`) | width x height / 1 000 000 |
| `duration>60`, `duration<=2m` | seconds; units s, m, h |
| `pages>10`, `triangles>100000` | PDF pages, mesh triangles |
| `modified<7d` | changed within the last 7 days; units s, h, d, w, m (30 days), y |
| `modified>30d` | not changed for more than 30 days |
| `modified>2024-01-01`, `modified<2024-06-01T12:00` | before or after a date (UTC) |
| `indexed<1h` | (re)indexed within the last hour |
| `-tag:used`, `not:type:video` | negation: everything that doesn't match |
| `sort:-size` | sort by path, name, size, modified, indexed, rating, width, height, pixels, duration, pages, type, ext or label; `-` for descending |
| `limit:20` | at most 20 results |

The comparisons are `=`, `!=`, `<`, `<=`, `>`, `>=`; `rating:>=3` works as well as `rating>=3`. Quote the whole query on a shell command line, since `>` and `<` mean redirection to the shell.

**Claude.** *"Show me the landscape photos over 12 megapixels from last month."* Claude calls `library_search` with `query: "type:picture orientation:landscape megapixels>12 modified<1m"`, and with `preview: true` when it wants to look at them.

## Task 3: tag, rate, label and add notes

**CLI.** Tag commands act on files given three ways: `paths=` (a list, comma or newline separated), `query=` (every match of a search; `query=""` means every file) or `collection=`. Paths that aren't indexed yet are added on the way.

```sh
$ wetwaresuite library.tag query=in:photos/2024-beach tags=beach,holiday
tagged 3 of 3 files
  /mnt/drive/photos/2024-beach/IMG_0001.jpg  [image] 77.0 KB 1600x1200 #beach #holiday
  /mnt/drive/photos/2024-beach/IMG_0002.jpg  [image] 203.8 KB 1600x1200 #beach #holiday
  /mnt/drive/photos/2024-beach/IMG_0003.jpg  [image] 28.9 KB 1600x1200 #beach #holiday
changed: 3
files: 3

$ wetwaresuite library.rate paths=photos/2024-beach/IMG_0002.jpg,photos/2024-city/IMG_0101.jpg rating=5
rated 2 of 2 files
  /mnt/drive/photos/2024-beach/IMG_0002.jpg  [image] 203.8 KB 1600x1200 ***** #beach #holiday
  /mnt/drive/photos/2024-city/IMG_0101.jpg  [image] 30.5 KB 1200x1600 *****

$ wetwaresuite library.rate query="in:photos/2024-beach rating<5" rating=3
rated 2 of 2 files
  /mnt/drive/photos/2024-beach/IMG_0001.jpg  [image] 77.0 KB 1600x1200 *** #beach #holiday
  /mnt/drive/photos/2024-beach/IMG_0003.jpg  [image] 28.9 KB 1600x1200 *** #beach #holiday

$ wetwaresuite library.label query=type:video label=green
labelled 1 of 1 files
  /mnt/drive/video/intro.mp4  [video] 1.9 MB 1280x720 0:06 (green)

$ wetwaresuite library.note paths=textures/rock_4k.jpg text="from the texture shop, licence 2291"
noted 1 of 1 files
  /mnt/drive/textures/rock_4k.jpg  [image] 1.6 MB 4096x4096

$ wetwaresuite library.search query="rating>=3 sort:-rating"
4 matches
/mnt/drive/photos/2024-beach/IMG_0002.jpg  [image] 203.8 KB 1600x1200 ***** #beach #holiday
/mnt/drive/photos/2024-city/IMG_0101.jpg  [image] 30.5 KB 1200x1600 *****
/mnt/drive/photos/2024-beach/IMG_0001.jpg  [image] 77.0 KB 1600x1200 *** #beach #holiday
/mnt/drive/photos/2024-beach/IMG_0003.jpg  [image] 28.9 KB 1600x1200 *** #beach #holiday

$ wetwaresuite library.search query=licence
1 match
/mnt/drive/textures/rock_4k.jpg  [image] 1.6 MB 4096x4096
```

`library.untag tags=old` removes tags (`tags=*` removes them all), `label=none` clears a label, `rating=0` clears a rating, and `library.note append=true` adds a line to existing notes instead of replacing them. Tags are matched without regard to case.

**Batch.** In a batch script (or an MCP session) these are undo steps:

```
# tag-session.txt
library.index root=/mnt/drive
library.tag query="in:photos/2024-city" tags=city
library.untag query="tag:city" tags=city
undo
library.search query="tag:city"
```

```sh
$ wetwaresuite batch tag-session.txt
indexed /mnt/drive: 19 files (0 new, 0 updated, 19 unchanged, 0 removed) in 0.00 s; library has 19 files
...
tagged 2 of 2 files
  /mnt/drive/photos/2024-city/IMG_0101.jpg  [image] 30.5 KB 1200x1600 ***** #city
  /mnt/drive/photos/2024-city/IMG_0102.jpg  [image] 677.4 KB 1600x1200 #city
...
untagged 2 of 2 files
...
undone
2 matches
/mnt/drive/photos/2024-city/IMG_0101.jpg  [image] 30.5 KB 1200x1600 ***** #city
/mnt/drive/photos/2024-city/IMG_0102.jpg  [image] 677.4 KB 1600x1200 #city
```

**Tags that travel with the file.** Tags, ratings, labels and notes live in the catalog, which is quick but stays on this computer. `library.write_tags` also writes them into the files as XMP, the format Lightroom, Bridge, darktable, digiKam and most file managers read. JPEG and PNG get them embedded (only the metadata block changes; the picture isn't re-encoded and EXIF stays). Everything else, camera raw included, gets a `.xmp` sidecar next to it (`IMG_0101.xmp`). Any other XMP already in the file or sidecar, like Lightroom's develop settings, is kept. It is undoable like the rest:

```
$ wetwaresuite library.write_tags query="tag:city"
saved tags into 2 of 2 files
rows: /mnt/drive/photos/2024-city/IMG_0101.jpg	the file
      /mnt/drive/photos/2024-city/IMG_0102.jpg	the file
```

It works the other way too. Indexing a new file picks up the tags, rating, label and description inside it (embedded XMP, a sidecar, or the EXIF rating and keywords Windows Explorer writes), so photos tagged in another program arrive tagged. `library.read_tags` does the same for files that are already in the catalog. It adds tags and only fills empty fields, unless you pass `replace=true`. Sidecar `.xmp` files aren't listed as files of their own.

**Claude.** *"Tag everything in photos/2024-beach as beach and holiday, give the best one five stars, and note on rock_4k.jpg that it's licence 2291."* Claude calls `library_tag` (`query: "in:photos/2024-beach"`, `tags: "beach,holiday"`), looks at the photos with `library_search` and `preview: true` to pick one, then `library_rate` and `library_note`. "Undo that" calls `undo`.

## Task 4: find duplicates

**CLI.**

```sh
$ wetwaresuite library.duplicates
2 groups of duplicates, 4 files, 280.8 KB reclaimable
2 copies of 203.8 KB (hash 84602376cfc05e36):
  keep /mnt/drive/photos/2024-beach/IMG_0002.jpg
       /mnt/drive/photos/inbox/IMG_0002.jpg
2 copies of 77.0 KB (hash c9f9dd2a54b884a8):
  keep /mnt/drive/photos/2024-beach/IMG_0001.jpg
       /mnt/drive/photos/inbox/IMG_0001 (1).jpg
extras: /mnt/drive/photos/inbox/IMG_0002.jpg
/mnt/drive/photos/inbox/IMG_0001 (1).jpg
groups: 2
files: 4
wasted_bytes: 287559
```

Duplicates are files with the same size and content hash; large ones are re-read in full first (`verify=false` skips that). Nothing is deleted. In each group, `keep` picks the copy to keep (`first` by path, `oldest`, `newest`, `shortest` path, or the best `rated`) and `extras` lists all the others, ready to hand to `library.move`, `library.tag` or `library.forget`. `query=` limits the search (`query="type:picture"`), `min_size=1mb` ignores small files.

**Batch.** `-> extras` captures the list (it's the first field), so moving every extra copy to a review folder is two lines:

```
# dupes.txt
library.duplicates keep=first -> extras
library.move paths=$extras to=../dupes
```

```sh
$ wetwaresuite batch dupes.txt
2 groups of duplicates, 4 files, 280.8 KB reclaimable
...
moved 2 files, skipped 0
  /mnt/drive/photos/inbox/IMG_0002.jpg -> /mnt/dupes/IMG_0002.jpg
  /mnt/drive/photos/inbox/IMG_0001 (1).jpg -> /mnt/dupes/IMG_0001 (1).jpg
moved: 2
skipped: 0
```

The moved files stay in the library (at their new place) until you `library.forget` them or delete them and rescan, so you can still look at them. If there are no duplicates, `$extras` is empty and the move line stops the script with "say which files".

**Claude.** *"Find duplicate photos on the drive and move the extra copies into /mnt/dupes, keeping whichever copy is in a dated folder."* Claude calls `library_duplicates` (`query: "type:picture"`, `keep: "shortest"` or reads the groups itself), shows you the plan, then calls `library_move` with the `extras` paths, ideally with `dry_run: true` first.

## Task 5: find similar images

Exact duplicates miss resized, re-saved and lightly edited copies. `library.similar` compares perceptual hashes: a 64-bit fingerprint of what the picture looks like, compared by Hamming distance (how many of the 64 bits differ: 0 is the same picture, up to about 10 is "similar").

**CLI.** Images like one file:

```sh
$ wetwaresuite library.similar path=photos/2024-beach/IMG_0003.jpg
1 image like /mnt/drive/photos/2024-beach/IMG_0003.jpg
  4  /mnt/drive/photos/inbox/beach_small.jpg  [image] 9.6 KB 800x600
paths: /mnt/drive/photos/inbox/beach_small.jpg
count: 1
distances: 4
hashed: 14
```

Every set of similar images:

```sh
$ wetwaresuite library.similar
3 groups of similar images among 14
2 alike:
  0  /mnt/drive/photos/2024-beach/IMG_0003.jpg  [image] 29.6 KB 1600x1200 *** #beach #holiday
  4  /mnt/drive/photos/inbox/beach_small.jpg  [image] 9.6 KB 800x600
2 alike:
  0  /mnt/drive/photos/2024-beach/IMG_0001.jpg  [image] 77.0 KB 1600x1200 *** #beach #holiday
  0  /mnt/dupes/IMG_0001 (1).jpg  [image] 77.0 KB 1600x1200
2 alike:
  0  /mnt/drive/photos/2024-beach/IMG_0002.jpg  [image] 203.8 KB 1600x1200 ***** #beach #holiday
  0  /mnt/dupes/IMG_0002.jpg  [image] 203.8 KB 1600x1200
groups: 3
files: 6
hashed: 0
```

The first run hashes every picture (and SVG) in scope from its thumbnail and stores the hashes in the catalog (`hashed: 14`); later runs reuse them (`hashed: 0`). The 800x600 copy is found at distance 4. `threshold=` sets how far apart still counts (default 10), `query=` limits it (`query="in:photos"`), and `path=` can be a file that isn't in the library at all ("do I already have this picture?").

`method=phash` (the default) compares low DCT frequencies, which survives resizing, recompression and colour tweaks. `method=dhash` compares neighbouring brightness: it's fine for detailed photos but sees smooth gradients and flat graphics as alike, as on this drive:

```sh
$ wetwaresuite library.similar method=dhash
4 groups of similar images among 14
3 alike:
  0  /mnt/drive/logos/wordmark.svg  [vector] 189 B 400x120
  8  /mnt/drive/photos/2024-city/IMG_0101.jpg  [image] 37.9 KB 1200x1600 ***** #city
  9  /mnt/drive/textures/sky_4k.jpg  [image] 208.1 KB 4096x4096
...
```

**Claude.** *"Are there near-duplicates among my beach photos? Show me."* Claude calls `library_similar` with `query: "tag:beach"` and `preview: true`, which attaches a contact sheet of the groups so it can tell you which ones are really the same shot.

## Task 6: thumbnails and contact sheets

**CLI.**

```sh
$ wetwaresuite library.thumbnails query= sheet=../sheet_all.png columns=5
4 thumbnails made, 14 already cached, 1 without a thumbnail (of 19 files)
  1: no thumbnailer for document files
wrote contact sheet ../sheet_all.png (1328x1064):
  1: /mnt/drive/audio/theme.wav
  2: /mnt/drive/docs/handbook.pdf
  3: /mnt/drive/docs/todo.md
  4: /mnt/drive/logos/badge.svg
  5: /mnt/drive/logos/wordmark.svg
  6: /mnt/drive/models/crate.stl
  7: /mnt/drive/photos/2024-beach/IMG_0001.jpg
  ...
  17: /mnt/drive/video/intro.mp4
  18: /mnt/dupes/IMG_0001 (1).jpg
  19: /mnt/dupes/IMG_0002.jpg
made: 4
cached: 14
missing: 1
cache: /mnt/drive/.wetwaresuite-thumbs
sheet: ../sheet_all.png
```

(The 14 cached ones were made by `library.similar` in Task 5.) The sheet has the audio waveform, the PDF's first page, both SVGs, an isometric render of the STL, every picture and a frame of the video, each numbered to match the list; `todo.md` gets an empty cell. Only the 4k textures:

```sh
$ wetwaresuite library.thumbnails query="type:picture width>=3840" sheet=../sheet_4k.png
0 thumbnails made, 3 already cached, 0 without a thumbnail (of 3 files)
wrote contact sheet ../sheet_4k.png (800x272):
  1: /mnt/drive/textures/noise_4k.png
  2: /mnt/drive/textures/rock_4k.jpg
  3: /mnt/drive/textures/sky_4k.jpg
```

`size=` picks the thumbnail size (cached separately per size), `force=true` remakes cached ones (say, after a suite upgrade that draws them better), `sheet_limit=` caps the files on a sheet (default 100). Thumbnails are made in parallel.

**Claude.** *"Make me a contact sheet of every 4k texture."* Claude calls `library_thumbnails` with `query: "type:picture width>=3840"` and `preview: true`; the sheet comes back as an image it can look at, and `sheet:` writes it to a file if you asked for one.

## Task 7: collections, and "find all 4k textures I haven't used"

A **smart collection** is a saved search: it's re-run every time you use it, so new files that match appear in it. A **static collection** is a fixed list of files (it follows them through `library.move` and `library.rename`). Use either as `collection=<name>` on any command that takes `query=`, or as `collection:<name>` inside a query.

The library doesn't know what "used" means for you, so mark it: tag textures as `used` when they go into a project (by hand, from your project's export script, or with `automate`). Then:

```sh
$ wetwaresuite library.tag paths=textures/rock_4k.jpg tags=used
tagged 1 of 1 files
  /mnt/drive/textures/rock_4k.jpg  [image] 1.6 MB 4096x4096 #used

$ wetwaresuite library.collection_save name=unused-4k query="type:picture width>=3840 -tag:used sort:-size"
saved smart collection unused-4k (2 files now)

$ wetwaresuite library.collection_save name=portfolio paths=photos/2024-beach/IMG_0002.jpg,photos/2024-city/IMG_0101.jpg,logos/badge.svg
saved static collection portfolio (3 files now)

$ wetwaresuite library.collection_save name=best query="rating>=4"
saved smart collection best (2 files now)

$ wetwaresuite library.collections
3 collections
  best (2 files): rating>=4
  portfolio (3 files): static list
  unused-4k (2 files): type:picture width>=3840 -tag:used sort:-size

$ wetwaresuite library.search query= collection=unused-4k
2 matches
/mnt/drive/textures/noise_4k.png  [image] 1.0 MB 4096x2048
/mnt/drive/textures/sky_4k.jpg  [image] 204.4 KB 4096x4096

$ wetwaresuite library.search query="collection:portfolio type:image"
2 matches
/mnt/drive/photos/2024-beach/IMG_0002.jpg  [image] 203.8 KB 1600x1200 ***** #beach #holiday
/mnt/drive/photos/2024-city/IMG_0101.jpg  [image] 30.5 KB 1200x1600 ***** #city
```

Smart collections update themselves:

```sh
$ wetwaresuite library.rate paths=textures/sky_4k.jpg rating=4
$ wetwaresuite library.search query= collection=best
3 matches
/mnt/drive/photos/2024-beach/IMG_0002.jpg  [image] 203.8 KB 1600x1200 ***** #beach #holiday
/mnt/drive/photos/2024-city/IMG_0101.jpg  [image] 30.5 KB 1200x1600 ***** #city
/mnt/drive/textures/sky_4k.jpg  [image] 204.4 KB 4096x4096 ****
```

`static=true` with a `query=` freezes today's results as a static list ("the files I sent on Monday"). Saving an existing name replaces it; `library.collection_delete name=` removes one (the files stay).

**Claude.** *"Find all 4k textures I haven't used."* With the drive served over MCP, Claude calls:

```json
{"name": "library_search", "arguments": {"query": "type:picture width>=3840 -tag:used", "preview": true}}
```

and gets back the list plus a contact sheet image:

```
2 matches
/mnt/drive/textures/noise_4k.png  [image] 1.0 MB 4096x2048
/mnt/drive/textures/sky_4k.jpg  [image] 208.1 KB 4096x4096 ****
count: 2
total: 2
attachment: contact-sheet.png (34954 bytes)
```

Follow-ups work the same way: *"save that as a collection called unused-4k"* (`library_collection_save`), *"I just used sky_4k in the level"* (`library_tag` with `tags: "used"`), *"copy the unused ones into my new project's textures folder"* (`library_copy` with `collection: "unused-4k"`, `to: ...`).

## Task 8: move, rename and copy, keeping the catalog right

Moving files with your file manager is fine (the next rescan notices and keeps their tags), but these commands do it and update the catalog in one go. All three take `paths=`, `query=` or `collection=`, skip files that already exist at the destination unless `overwrite=true`, and support `dry_run=true`.

**Rename** with a template: `{name}` (old name without its extension), `{ext}` (with its dot), `{n}` (a counter in `sort=` order, `{n:3}` pads to 001, `start=` sets the first), `{date}` (modified date), `{width}`, `{height}`, `{rating}`, `{type}`, `{label}`, `{hash}`. Files stay in their folders.

```sh
$ wetwaresuite library.rename query=in:photos/2024-beach name="beach_{n:2}{ext}" dry_run=true
would rename 3 files, skipped 0
  IMG_0001.jpg -> beach_01.jpg
  IMG_0002.jpg -> beach_02.jpg
  IMG_0003.jpg -> beach_03.jpg

$ wetwaresuite library.rename query=in:photos/2024-beach name="beach_{n:2}{ext}"
renamed 3 files, skipped 0
  IMG_0001.jpg -> beach_01.jpg
  ...
```

**Copy** gathers files somewhere else; the copies join the library with the same tags, rating, label and notes:

```sh
$ wetwaresuite library.copy collection=portfolio to=../client-handoff
copied 3 files, skipped 0
  /mnt/drive/logos/badge.svg -> /mnt/client-handoff/badge.svg
  /mnt/drive/photos/2024-beach/beach_02.jpg -> /mnt/client-handoff/beach_02.jpg
  /mnt/drive/photos/2024-city/IMG_0101.jpg -> /mnt/client-handoff/IMG_0101.jpg
```

(The `portfolio` collection followed the rename to `beach_02.jpg`.) **Move** puts files into a folder (moves across drives copy, then delete):

```sh
$ wetwaresuite library.move query=type:document to=docs-archive
moved 2 files, skipped 0
  /mnt/drive/docs/handbook.pdf -> /mnt/drive/docs-archive/handbook.pdf
  /mnt/drive/docs/todo.md -> /mnt/drive/docs-archive/todo.md
```

**Claude.** *"Rename the beach photos to beach_01, beach_02... in date order."* Claude calls `library_rename` with `query: "tag:beach"`, `name: "beach_{n:2}{ext}"`, `sort: "modified"`, `dry_run: true`, shows you the plan, then runs it again without `dry_run`.

## Task 9: export lists, keep the catalog fresh

**Export** a search or collection as CSV (opens in any spreadsheet; tags are joined with `;`), JSON or JSON lines, with the columns you choose:

```sh
$ wetwaresuite library.export output=../catalog.csv query="type:picture" fields=name,width,height,rating,tags,path
wrote 14 files to ../catalog.csv

$ head -4 ../catalog.csv
name,width,height,rating,tags,path
beach_02.jpg,1600,1200,5,beach;holiday,/mnt/client-handoff/beach_02.jpg
IMG_0101.jpg,1200,1600,5,city,/mnt/client-handoff/IMG_0101.jpg
beach_01.jpg,1600,1200,3,beach;holiday,/mnt/drive/photos/2024-beach/beach_01.jpg

$ wetwaresuite library.export output=../portfolio.json collection=portfolio fields=name,type,rating,tags
wrote 3 files to ../portfolio.json

$ cat ../portfolio.json
[
  {
    "name": "badge.svg",
    "type": "vector",
    "rating": 0,
    "tags": []
  },
  ...
]
```

Fields: path, name, type, format, ext, size, modified, mtime (seconds since 1970), width, height, duration, pages, triangles, bounds, rating, label, tags, notes, hash, dhash, phash, indexed.

**Rescan** after changes made outside the library. Here a new picture was dropped into the inbox, `audio/theme.wav` was deleted and `video/intro.mp4` was moved to `archive/` with a file manager:

```sh
$ wetwaresuite library.rescan
indexed 1 folder: 17 files (1 new, 0 updated, 15 unchanged, 1 removed, 1 moved) in 0.10 s; library has 22 files
...
$ wetwaresuite library.info path=archive/intro.mp4
/mnt/drive/archive/intro.mp4
type: video (MP4 video)
...
label: green
```

The moved video kept its green label. **Forget** drops entries without touching files (undoable), for folders you no longer want in the catalog:

```sh
$ wetwaresuite library.forget query="in:../dupes"
removed 2 files from the library (the files are untouched)
removed: 2
total: 20
```

**Batch.** A weekly job that refreshes the catalog, tags extra copies of anything and makes a contact sheet of the week's pictures:

```
# weekly.txt
library.rescan library=/mnt/drive
library.duplicates library=/mnt/drive -> extras
library.tag paths=$extras tags=duplicate
library.thumbnails query="type:picture modified<7d -tag:duplicate" sheet=week.png columns=8
```

Run it from cron or Task Scheduler with `wetwaresuite batch weekly.txt` (see [automate](automate.md#run-it-on-a-schedule-cron-and-task-scheduler)), or every time something lands in a folder with `automate.watch`.

**Claude.** *"I reorganised the drive: update the library and tell me what changed."* Claude calls `library_rescan` and reads the counts; *"what did I add this week?"* is `library_search` with `query: "indexed<7d"` (or `modified<7d`).

## Parameter reference

Every command also takes `library=` (catalog file or its folder; see "Which catalog a command uses"). "Selection" means `paths=` (comma or newline separated), `query=` (a search; `""` = every file) or `collection=`; give `paths=` or the other two, not both.

| command | what it does | main parameters |
| --- | --- | --- |
| `library.index` | scan a folder into the catalog | `root` (required), `library` (default `root/.wetwaresuite-library.json`), `recursive` (true), `incremental` (true), `include`, `exclude`, `hidden` (false), `probe` (true), `prune` (true), `full_hash` (false), `threads` (0 = auto) |
| `library.rescan` | re-index every root | same options as index, no `root` |
| `library.search` | find files | `query`, `collection`, `sort`, `limit` (100, 0 = all), `offset`, `preview` (contact sheet), `columns` (6), `json` (items list) |
| `library.info` | one file in detail | `path` (required), `preview` (thumbnail) |
| `library.tag` / `library.untag` | add / remove tags (undoable) | `tags` (comma list; untag `*` = all) + selection |
| `library.rate` | star rating (undoable) | `rating` 0..5 + selection |
| `library.label` | colour label (undoable) | `label`: red, orange, yellow, green, blue, purple, gray, none + selection |
| `library.note` | notes (undoable) | `text`, `append` (false) + selection |
| `library.write_tags` | save tags, rating, label and notes into the files as XMP (undoable) | `where`: auto (embed in JPEG/PNG, sidecar for the rest), embed, sidecar + selection |
| `library.read_tags` | load tags from the files' XMP/EXIF (undoable) | `replace` (false) + selection |
| `library.forget` | drop entries, keep files (undoable) | selection |
| `library.duplicates` | identical files | `query`, `verify` (true), `keep`: first, oldest, newest, shortest, rated; `min_size` (1), `json` |
| `library.similar` | look-alike images | `path` (compare to one file), `query`, `threshold` (10), `method`: phash, dhash; `threads`, `preview`, `json` |
| `library.thumbnails` | cached thumbnails, contact sheet | selection (default all), `size` (256), `force`, `sheet` (PNG path), `columns` (6), `sheet_limit` (100), `threads`, `preview` |
| `library.collection_save` | save a collection | `name` (required), `query` (smart) or `paths` (static), `static` (freeze a query) |
| `library.collection_delete` | delete a collection | `name` |
| `library.collections` | list collections | `json` |
| `library.move` | move files into a folder | `to` (required), `overwrite`, `dry_run` + selection |
| `library.rename` | rename with a template | `name` (required), `sort` (path), `start` (1), `overwrite`, `dry_run` + selection |
| `library.copy` | copy files, copies keep metadata | `to` (required), `overwrite`, `dry_run` + selection |
| `library.stats` | counts and sizes | `query`, `collection`, `json` (per-type/extension tables) |
| `library.export` | CSV / JSON / JSON lines | `output` (required), `fields`, `sort` + selection (default all) |

Over MCP the tool names use underscores (`library_search`). Read-only tools (search, info, duplicates, collections, stats) are marked as such for Claude.

## Limitations

- No live file watching: changes made outside the library show up after `library.index` or `library.rescan` (fast, since unchanged files are skipped). `automate.watch` can trigger a rescan when files arrive.
- Dimensions come from headers for PNG, JPEG, GIF, BMP, WebP, PSD, TGA, PNM, TIFF, DDS and QOI; other image formats (HEIC, AVIF, EXR, most camera raw) are measured through `ffprobe` when it's installed, and camera raw files may report the embedded preview's size.
- Durations and video sizes need `ffprobe`; video and audio thumbnails need `ffmpeg`; PDF thumbnails need `pdftoppm` and page counts are most accurate with `pdfinfo` (both from poppler; without them pages are counted from the file's text, which misses compressed object streams). Office documents, fonts and archives have no thumbnails.
- Thumbnails for camera raw, HEIC and other formats the raster engine can't read go through `ffmpeg`, which may not support them all.
- Hashes of files over 2 MiB cover the first and last MiB plus the size; `library.duplicates` re-checks those in full before reporting them (unless `verify=false`), but `hash:` searches and move detection use the fast hash.
- Perceptual hashes compare whole pictures: crops, rotations and mirrored copies are not found as similar. Comparing every image with every other is quadratic; it's instant for tens of thousands of images, slower beyond that.
- The catalog is one JSON-lines file loaded into memory: comfortable to a few hundred thousand files. Two programs writing the same catalog at the same moment can lose one's changes (the last write wins).
- Tags only go into files when you run `library.write_tags`; plain `library.tag` stays in the catalog, so it never touches your photos without asking. TIFF, WebP and HEIC get sidecars rather than embedded XMP. Lightroom's hierarchical keywords (`lr:hierarchicalSubject`) aren't read.
- "Used" is whatever you tag as used: the library doesn't look inside project files to see which assets they reference.
- Query dates are UTC; `{date}` in rename templates is the file's UTC modification date.
