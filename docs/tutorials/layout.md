# layout tutorial: posters, flyers, cards and booklets for print

`layout` is the suite's print layout and publishing tool, a small headless
take on Scribus or InDesign. You build pages out of frames (text, pictures,
shapes and SVG artwork), give text paragraph and character styles, let long
text flow through linked frames and columns, and export a print-ready PDF
with bleed, crop marks, CMYK colours and embedded fonts, or PNG/JPEG pages.
Like every wetwaresuite tool, each action is a command you can type once, put in
a batch script, or let Claude call over MCP.

The tutorial is built around five real jobs, each shown three ways:

- **task 1**: an A2 event poster with a big headline, a photo and the details;
- **task 2**: a three-column A4 flyer (tri-fold) from a Markdown file;
- **task 3**: business cards, front and back, imposed 10-up on A4 with crop marks;
- **task 4**: a 20-page 6x9" booklet from a Markdown file, with running
  heads and page numbers from a master page;
- **task 5**: preflight and export for a print shop.

Every example below was run against the real `wetwaresuite` binary; the outputs
shown are what it printed (trimmed where marked).

## contents

1. [the ideas you need](#1-the-ideas-you-need)
2. [setting up](#2-setting-up)
3. [task 1: an event poster](#3-task-1-an-event-poster)
4. [task 2: a three-column A4 flyer](#4-task-2-a-three-column-a4-flyer)
5. [task 3: business cards 10-up](#5-task-3-business-cards-10-up)
6. [task 4: a 20-page booklet from Markdown](#6-task-4-a-20-page-booklet-from-markdown)
7. [task 5: preflight and export for a print shop](#7-task-5-preflight-and-export-for-a-print-shop)
8. [text: markup, styles, flow](#8-text-markup-styles-flow)
9. [pictures, shapes and colour](#9-pictures-shapes-and-colour)
10. [arranging: align, distribute, order, undo](#10-arranging-align-distribute-order-undo)
11. [command reference](#11-command-reference)
12. [limitations and troubleshooting](#12-limitations-and-troubleshooting)

## 1. the ideas you need

**Millimetres from the top-left.** Every position and size is in mm, measured
from the top-left corner of the page's *trim* (the finished, cut size), with
`y` growing downwards. `x=-3` is 3 mm outside the left edge, in the bleed.
Font sizes, leading (line spacing) and stroke widths are in points, as in
every print program.

**Documents, pages and frames have ids.** `layout.new` returns a document id
like `lay1`; commands name it with `doc=lay1`. Frames get ids `f1`, `f2`, ...
in the order you make them, and you can also give a frame a `name=` and use
that instead (`frame=photo`). Pages are numbered from 1.

**Frames.** A frame is a rectangle on a page (or a master page) with a type:

| type | what it holds |
|---|---|
| `text` | a *story*: paragraphs of styled text, in one or more columns |
| `image` | a PNG/JPEG/BMP/GIF/TGA/PSD picture, fitted with `fit=fit\|fill\|stretch\|manual` |
| `vector` | SVG artwork (read by the vector tool), fitted like a picture |
| `rect`, `ellipse` | shapes with `fill`, `stroke`, `stroke_width` (pt) and `radius` (rounded corners) |
| `line` | a line from (`x`, `y`) to (`x+w`, `y+h`) |

Any frame can be rotated (`rotation=` degrees clockwise about its centre),
faded (`opacity=`) and have a fill and outline.

**Stories flow.** A text frame shows a story. Linking frames
(`layout.frame_link from=f1 to=f2`) makes the story continue from one into the
next, on the same page or any other, and inside a frame text fills the columns
left to right. Text that doesn't fit anywhere is *overset*: every reply that
touches a text frame tells you how many characters are overset, previews mark
the frame with a red square, and preflight reports it as an error.
`layout.place_text` with autoflow adds pages and linked frames for you until a
whole file fits.

**Styles.** Paragraph styles (font, size, leading, alignment, spacing, indents,
"start in the next frame") and character styles (font, size, bold, italic,
underline, colour, tracking) are named and inherit through `based_on`. Every
document starts with `body`, `h1`, `h2`, `h3`, `quote`, `bullet`, `code`,
`caption`, `folio`, `headline`, `subhead` and the character styles `strong`
and `emphasis`. Change a style and everything using it reflows. Formatting
given directly on `frame_add`/`frame_set` (`size=24 bold=true`) overrides the
style for that frame's text.

**Masters.** A master page holds frames drawn on every page that uses it:
running headers, page numbers, a logo. Text on a master may contain
`{page}`, `{pages}` and `{title}`. In a facing-pages document (books,
magazines) margins become *inside* and *outside*, `align=outside` puts page
numbers on the outer edge, and master frames made by `layout.master_set` mirror
on left-hand pages.

**Bleed and margins.** The bleed (default 3 mm) is the strip outside the trim
that artwork running off the page must extend into, so a slightly off cut
never shows white paper. Margins are where text should stay inside.
Preflight checks both.

**Colours** can be `#rrggbb`, a name (`navy`, `orange`, ...), `cmyk(c,m,y,k)`
in percent, or a named swatch from `layout.color_define`. CMYK colours are
written to the PDF as DeviceCMYK, so the printer gets exactly those inks.
`black` and `white` are always 100% K and paper.

**Fonts.** Helvetica, Times and Courier (the standard PDF fonts, with bold
and italic; `sans`, `serif`, `mono`, `arial` work as aliases) need no files.
Their metrics are built in, so line breaks are identical everywhere. For a
print shop, `embed_fonts=true` embeds metric-compatible TrueType substitutes
found on the system (Liberation, Arimo/Tinos/Cousine, Arial/Times New Roman/
Courier New). Your own `.ttf` families are added with `layout.font_add` and
always embedded.

**Undo.** Every edit is one undo step (`undo`/`redo` lines in batch, `undo`
and `redo` tools over MCP), including a whole `place_text` that added twenty
pages. `info`, `preview`, `preflight`, `save`, `export` and `impose` don't touch
the history.

## 2. setting up

Build the suite (see [getting started](getting-started.md)) and check the tool
is there:

```sh
wetwaresuite list layout
wetwaresuite help layout.frame_add
```

```
layout
  layout.align  Align Frames
  layout.close  Close Layout
  layout.color_define  Define Colour
  ...
  layout.style_apply  Apply Style
  layout.style_define  Define Style
```

### four ways to drive it

- **CLI**: `wetwaresuite <command> key=value ...` runs one command in a fresh
  process. Documents live only inside one process, so a single command can
  create or inspect a layout (`wetwaresuite layout.open path=poster.wwlay`) but
  can't keep editing it. For a few steps from the shell, pipe lines into
  `wetwaresuite batch` (shown in every task).
- **Batch script**: a text file with one command per line, run with
  `wetwaresuite batch poster.txt`. `-> name` at the end of a line saves the result
  (the document id for `new`/`open`, the frame id for `frame_add`,
  `place_text`, `place_image`, `frame_duplicate`) and `$name` reuses it. `#`
  starts a comment. The script stops at the first failing line unless you add
  `--keep-going`.
- **The window**: `wetwaresuite-office poster.wwlay` shows the page with every
  frame outlined. Click a frame to pick it, drag it to move it (half-mm steps,
  hold Shift for free), drag one of its eight handles to resize it, and
  double-click a text frame to retype it right on the page. Arrows nudge 1 mm
  (Shift: 0.1 mm), Delete removes the frame, PageUp/PageDown or the `<` `>`
  buttons change page. Everything else (styles, colours, pictures, export) is
  on the Layout ribbon and in Ctrl+K.
- **Claude over MCP**: `wetwaresuite mcp` (or `wetwaresuite mcp --tools layout`) serves
  every command as a tool named with `_` instead of `.` (`layout_frame_add`).
  Connect it once (see [mcp.md](mcp.md)):

  ```sh
  claude mcp add wetwaresuite -- /path/to/wetwaresuite/build/apps/wetwaresuite/wetwaresuite mcp --tools layout
  ```

  Every edit tool takes `preview=true`, which attaches a PNG of the page
  (with margin, column and frame guides) to the result, and `layout_preview`
  renders any page, so Claude sees the page after each step.

**New lines in text.** In a batch file or the shell, the command line
reader treats a backslash as an escape, so write a paragraph break inside
`text=` as `\\n` (it reaches the tool as `\n`). Over MCP, Claude sends real
new lines or `\n` and both work.

The examples use a working folder with a few pictures. To make the same test
pictures with ffmpeg:

```sh
ffmpeg -f lavfi -i "mandelbrot=size=3000x2000" -frames:v 1 -q:v 3 concert.jpg
ffmpeg -f lavfi -i "testsrc2=size=400x300" -frames:v 1 small.png
ffmpeg -f lavfi -i "gradients=size=1200x900:seed=3" -frames:v 1 -q:v 3 hills.jpg
```

## 3. task 1: an event poster

An A2 poster (420 x 594 mm): an orange band across the top with a huge white
headline, a photo across the full width, the date and place underneath and a
dark footer strip. Everything that touches the paper edge runs 3 mm into the
bleed.

### as a batch script

Save this as `poster.txt`:

```
# An A2 event poster: a big headline on a colour band, a photo, the details.
layout.new preset=poster_a2 title="Summer Jazz Night" -> doc
layout.color_define doc=$doc name=brand cmyk=0,60,100,0
layout.color_define doc=$doc name=night cmyk=80,60,0,60

# a colour band that runs 3 mm past the top and sides (into the bleed)
layout.frame_add doc=$doc type=rect x=-3 y=-3 w=426 h=203 fill=brand name=band
layout.frame_add doc=$doc type=text x=20 y=20 w=380 h=165 text="SUMMER\\nJAZZ NIGHT" style=headline size=120 leading=110 color=white valign=bottom -> headline

# the photo fills the middle, cropped to the frame
layout.place_image doc=$doc path=concert.jpg x=-3 y=200 w=426 h=270 fit=fill name=photo -> photo

# the details, centred
layout.frame_add doc=$doc type=text x=20 y=490 w=380 h=60 text="**Saturday 12 July, 8 pm**\\nRiverside Park bandstand\\nTickets £15 at the door, *bring a blanket*" size=30 leading=40 align=center color=night -> details
layout.frame_add doc=$doc type=rect x=-3 y=560 w=426 h=37 fill=night name=footer
layout.frame_add doc=$doc type=text x=20 y=568 w=380 h=20 text="riversidejazz.example  ·  free for under-12s" size=20 align=center color=white valign=middle

layout.preflight doc=$doc min_dpi=150
layout.save doc=$doc path=poster.wwlay
layout.export doc=$doc path=poster.pdf bleed=true marks=true
layout.export doc=$doc path=poster-preview.png dpi=30
```

```sh
wetwaresuite batch poster.txt
```

What it prints (trimmed):

```
created lay1 ("Summer Jazz Night", 1 page 420 x 594 mm, 0 frames)
defined colour brand (cmyk 0,60,100,0)
...
added text frame f2 on page 1
f2
frame: f2
page: 1
overset: 0
placed concert.jpg in f3 at 179 dpi - under 300 dpi, it may print soft; make the frame smaller or use a bigger picture
...
preflight passed, 2 note(s)
  info [margin] page 1, f6: text frame is outside the margins (fine on purpose, but keep text 3+ mm from the trim)
  info [fonts] document: standard PDF fonts aren't embedded by default; export with embed_fonts=true so the print shop gets the exact glyphs
...
exported 1 page(s) to poster.pdf (638 KB, bleed 3 mm, crop marks); fonts: Helvetica-Bold, Helvetica, Helvetica-Oblique
exported 1 page image(s) at 30 dpi: poster-preview.png
```

How it works:

1. **`preset=poster_a2`** sets the page to 420 x 594 mm with 20 mm margins
   and 3 mm bleed. Other presets: `poster_a3`, `flyer_a5`, `flyer_a4`,
   `card`, `a4_report`, `book_6x9`, `letter`; or give `size=A3` (any of A0-A7,
   B4, B5, DL, letter, legal, tabloid, half_letter, card, card_us, 6x9, 5x8,
   square_210), `orientation=landscape`, or `width=`/`height=` in mm.
2. **The band** starts at `x=-3 y=-3` and is 6 mm wider than the page, so it
   bleeds off the top and both sides.
3. **The headline** uses the built-in `headline` style (bold), made bigger
   with `size=120 leading=110`. `valign=bottom` sits the two lines on the
   bottom of the frame, just above the photo. `\\n` splits paragraphs.
4. **`fit=fill`** scales the photo to cover the whole frame and crops the
   overflow (`fit` shows all of it, `stretch` distorts, `manual` takes
   `image_scale`/`image_x`/`image_y`). The reply gives the effective
   resolution at print size: 179 dpi is fine for a poster seen from a few
   metres, so preflight runs with `min_dpi=150`. For a flyer read at arm's
   length keep the default 300.
5. **`**bold**` and `*italic*`** inside `text=` switch to the bold and
   italic faces.
6. **`bleed=true marks=true`** writes a PDF whose MediaBox includes the bleed
   and a slug with crop marks and a page label; TrimBox and BleedBox are set,
   so the print shop's software knows where to cut.


### from the shell

One command can open and describe a saved layout:

```sh
wetwaresuite layout.open path=poster.wwlay
```

```
opened lay1 ("Summer Jazz Night", 1 page 420 x 594 mm, 6 frames, poster.wwlay)
lay1
doc: lay1
pages: 1
size: 420,594
```

To change and re-export it in one go, pipe lines into `batch`:

```sh
printf 'layout.open path=poster.wwlay -> p\nlayout.export doc=$p path=poster-small.png dpi=20\n' | wetwaresuite batch
```

```
...
exported 1 page image(s) at 20 dpi: poster-small.png
```

### asking Claude

> Make an A2 poster for "Summer Jazz Night": an orange band across the top
> with the title huge in white, concert.jpg filling the middle edge to edge,
> then "Saturday 12 July, 8 pm, Riverside Park bandstand, tickets £15" centred
> underneath in dark blue, and a dark blue footer strip with the website.
> Use CMYK colours, show me a preview, then export a print PDF with bleed and
> crop marks.

Claude works through the same commands, roughly:

1. `layout_new` with `preset: "poster_a2"`, `title: "Summer Jazz Night"` gets `lay1`.
2. `layout_color_define` for `brand` (`cmyk: [0,60,100,0]`) and `night`.
3. `layout_frame_add` for the band (`type: "rect"`, `x: -3, y: -3, w: 426, h: 203`)
   and the headline (`type: "text"`, `text: "SUMMER\nJAZZ NIGHT"`,
   `style: "headline"`, `size: 120`), then `layout_place_image` with
   `fit: "fill"`, each with `preview: true` to look at the page.
4. If the headline reply says characters are overset, `layout_frame_set`
   with a bigger `h` or smaller `size`.
5. `layout_preflight` (`min_dpi: 150` for a poster), then `layout_export`
   with `bleed: true, marks: true`.

## 4. task 2: a three-column A4 flyer

A landscape A4 flyer folded in three: `preset=flyer_a4` is 297 x 210 mm with
10 mm margins and three column guides with a 20 mm gutter, so each panel is
79 mm of text centred on a 99 mm fold. The text comes from a Markdown file;
a photo and an SVG logo go in the right-hand panel.

`flyer.md`:

```markdown
# Green Valley Walking Club

We are a friendly group of walkers who explore the hills, woods and rivers around Green Valley every weekend. All ages and paces are welcome, and the first three walks are free.

## Weekend walks

Every Saturday we meet at **9:30 am** at the old station car park. ...

- Short walks on the first Sunday of the month
- Evening walks in June and July
- A long-distance challenge every autumn

## What to bring
...
> "The best way to see the valley, and the best people to see it with."

## Membership
...
## Contact

Call **01632 960 123** or write to *hello@greenvalley.example*. ...
```

### as a batch script

`flyer.txt`:

```
layout.new preset=flyer_a4 title="Green Valley Walking Club" -> fly
layout.color_define doc=$fly name=green cmyk=85,20,70,15
layout.style_define doc=$fly name=h1 color=green size=18 leading=21
layout.style_define doc=$fly name=h2 color=green size=12 leading=15
layout.style_define doc=$fly name=body size=9.5 leading=13 align=justify
layout.place_text doc=$fly path=flyer.md autoflow=false -> text
layout.frame_set doc=$fly frame=$text h=120
layout.place_image doc=$fly path=small.png x=208 y=135 w=79 h=50 fit=fill name=photo
layout.place_image doc=$fly path=logo.svg x=228 y=10 w=40 h=40 fit=fit
layout.preflight doc=$fly
layout.export doc=$fly path=flyer.pdf bleed=true marks=true
layout.save doc=$fly path=flyer.wwlay
layout.export doc=$fly path=flyer.png dpi=60 guides=true
```

```
placed 14 paragraphs from flyer.md in f1; all text fits
...
preflight: 1 error(s), 0 warning(s), 1 note(s)
  error [low_res] page 1, f2 (photo): small.png prints at 129 dpi (400 x 300 px); 300 dpi needs it at most 33.9 mm wide
  info [fonts] document: standard PDF fonts aren't embedded by default; export with embed_fonts=true so the print shop gets the exact glyphs
```

1. **Styles first**: redefining `body`, `h1` and `h2` changes every paragraph
   that uses them (headings keep left alignment even though body is
   justified). Only the fields you give change.
2. **`place_text`** reads the Markdown: `#`/`##`/`###` become `h1`/`h2`/`h3`,
   `-` and `1.` lists become `bullet` paragraphs with a hanging indent, `>`
   becomes `quote`, fenced code becomes `code`, `**bold**`/`*italic*` work
   inline, links keep their text. It makes one frame filling the margins with
   the document's three columns. `autoflow=false` keeps it on this page.
3. **`frame_set h=120`** shortens the frame so the bottom of the panels is
   free; text reflows across the columns.
4. The **SVG logo** is real vector art in the PDF (paths, not pixels).
5. The **preflight error** is real: a 400 px wide picture can't print sharply
   79 mm wide. Task 5 fixes it.
6. **`guides=true`** on a PNG export draws the margins (magenta), column
   gutters (violet) and frame edges (blue), handy for checking fold lines.

### from the shell

```sh
wetwaresuite batch < flyer.txt
wetwaresuite layout.open path=flyer.wwlay
```

### asking Claude

> Lay out flyer.md as a landscape A4 tri-fold flyer in three columns, justified
> 9.5 pt body text, headings in a CMYK green (85,20,70,15). Keep the bottom of
> the right-hand panel for small.png and put logo.svg at the top of that
> panel. Preview it and tell me if preflight finds anything.

Claude calls `layout_new` (`preset: "flyer_a4"`), `layout_color_define`,
`layout_style_define` three times, `layout_place_text` with
`autoflow: false`, `layout_frame_set` for the height, `layout_place_image`
twice, `layout_preview`, and `layout_preflight`, and reports the
low-resolution photo.

## 5. task 3: business cards 10-up

A two-sided 85 x 55 mm business card, then a print sheet with ten of them on
A4 and crop marks on every cut line.

### as a batch script

`cards.txt`:

```
layout.new preset=card title="Ada Lovelace card" -> card
layout.color_define doc=$card name=ink cmyk=100,70,0,40
layout.frame_add doc=$card type=rect x=-2 y=-2 w=12 h=59 fill=ink
layout.frame_add doc=$card type=text x=16 y=8 w=65 h=12 text="Ada Lovelace" font=helvetica size=14 bold=true color=ink
layout.frame_add doc=$card type=text x=16 y=17 w=65 h=8 text="Analytical Engines Ltd." size=8 italic=true
layout.frame_add doc=$card type=line x=16 y=27 w=40 h=0 stroke=ink stroke_width=0.5
layout.frame_add doc=$card type=text x=16 y=31 w=65 h=20 text="+44 20 7946 0001\\nada@engines.example\\nengines.example" size=7.5 leading=10
layout.page_add doc=$card
layout.frame_add doc=$card type=rect page=2 x=-2 y=-2 w=89 h=59 fill=ink
layout.frame_add doc=$card type=text page=2 x=4 y=4 w=77 h=47 text="AE" size=36 bold=true color=white align=center valign=middle
layout.preflight doc=$card
layout.export doc=$card path=card.png dpi=300
layout.impose doc=$card path=cards-10up.pdf sheet=A4
```

```
...
preflight passed, 1 note(s)
...
exported 2 page image(s) at 300 dpi: card-1.png, card-2.png
imposed 10-up (2 x 5) on 2 A4 sheet(s): cards-10up.pdf
per_sheet: 10
sheets: 2
cols: 2
rows: 5
```

1. **`preset=card`** is 85 x 55 mm with 4 mm margins and 2 mm bleed, so the
   ink bar starts at `-2` and the back's full-bleed rectangle is 89 x 59.
2. **Page 2** is the back. `valign=middle` centres the monogram vertically.
3. **`layout.impose`** works out how many fit (`cols`/`rows` override it),
   centres the grid on the sheet and draws crop marks on every cut line in the
   margins. `repeat=true` (the default) fills each sheet with copies of one
   page, so you get a sheet of fronts and a sheet of backs for duplex
   printing; `repeat=false` places successive pages in turn (tickets, labels).
4. With the default `gap=0` cards butt together and are cut on shared lines,
   so each card is clipped at its trim. `gap=4` (at least twice the bleed)
   keeps every card's bleed; on A4 that fits fewer cards:

```sh
printf 'layout.new preset=card -> c\nlayout.frame_add doc=$c type=rect x=-2 y=-2 w=89 h=59 fill=navy\nlayout.impose doc=$c path=cards-gap.pdf sheet=A4 gap=4\n' | wetwaresuite batch
```

```
...
imposed 8-up (2 x 4) on 1 A4 sheet(s): cards-gap.pdf
```

### asking Claude

> Design a business card for Ada Lovelace, Analytical Engines Ltd., with a
> phone, email and website, a dark blue CMYK bar down the left; the back is
> solid blue with "AE" in white. Then give me a print sheet with ten on A4
> with crop marks.

Claude uses `layout_new` (`preset: "card"`), `layout_color_define`,
`layout_frame_add` for each item (checking with `preview: true`), `layout_page_add`
for the back, and `layout_impose` with `sheet: "A4"`.

## 6. task 4: a 20-page booklet from Markdown

A 6 x 9 inch guidebook with facing pages, a running head with the title,
page numbers on the outside edges, Times body text, every chapter starting a
new page, and the page count padded to 20 (a multiple of 4 for saddle
stitching).

### as a batch script

`guide.md` is an ordinary Markdown file (about 4,800 words: a title, six
`# Chapter` headings with `##` sections, lists and quotes). `booklet.txt`:

```
layout.new preset=book_6x9 title="A Walk Through Riverside" -> book
layout.style_define doc=$book name=body font=times size=10.5 leading=14 align=justify
layout.style_define doc=$book name=h1 font=helvetica size=20 leading=24 break_before=true space_after=12
layout.style_define doc=$book name=h2 font=helvetica size=12 leading=15
layout.place_text doc=$book path=guide.md
layout.page_add doc=$book
layout.info doc=$book page=3
layout.preflight doc=$book
layout.export doc=$book path=booklet.pdf
layout.export doc=$book path=booklet.png pages=2-3 dpi=60
```

```
placed 152 paragraphs from guide.md in f3, added 18 page(s); all text fits
f3
frame: f3
story: t3
paragraphs: 152
pages_added: 18
frames_added: 18
pages: 19
overset: 0
added 1 page(s); now 20
...
lay1: "A Walk Through Riverside", 20 pages 152.4 x 228.6 mm, 19 frames
title: A Walk Through Riverside
page 152.4 x 228.6 mm, margins top 16 bottom 20 inside 19 outside 14, bleed 3, 1 column(s) gutter 5, facing pages
page 3 (152.4 x 228.6 mm, master A):
  f5 text at 19,16 size 119.4x192.6 story t3 -> f6 "A Walk Through Riverside / A small guide to the ..." 36 lines
...
preflight passed, 1 note(s)
...
exported 20 page(s) to booklet.pdf (29 KB); fonts: Times-Roman, Helvetica-Bold, Times-Italic, Times-Bold
exported 2 page image(s) at 60 dpi: booklet-2.png, booklet-3.png
```

1. **`preset=book_6x9`** turns on facing pages (inside margin 19 mm, outside
   14 mm) and makes master `A` with a header frame showing `{title}` and a
   footer frame showing `{page}`, both aligned `outside`. Every page uses it.
2. **`break_before=true`** on `h1` makes each chapter heading start in the
   next linked frame, which here means the next page.
3. **`place_text` with autoflow** (the default) fills a frame in the margins of
   page 1, then keeps adding a page with a linked frame until nothing is
   overset: 18 pages added. The frames on left-hand pages use the mirrored
   margins. The whole placement is one undo step.
4. **`page_add`** pads to 20 pages. Changing a style later (say
   `body size=11`) reflows the whole story; if it then needs more room run
   `layout.place_text` again or add pages and link frames with
   `layout.frame_link`.
5. Page 2 (a left-hand page) has its header and number on the left, page 3
   on the right.

### from the shell

```sh
wetwaresuite batch < booklet.txt
pdfinfo booklet.pdf | grep Pages     # Pages: 20
```

A different header or footer: `layout.master_set doc=$book footer="Page {page} of {pages}" align=center`,
or for an A4 report `preset=a4_report` (centred page numbers, no facing pages).

### asking Claude

> Turn guide.md into a 6x9 inch booklet: Times 10.5/14 justified body text,
> Helvetica headings, every chapter on a new page, the book title at the top of
> each page and page numbers on the outside. Make it 20 pages and export a PDF.

Claude calls `layout_new` (`preset: "book_6x9"`, `title`),
`layout_style_define` for `body`, `h1` (`break_before: true`) and `h2`,
`layout_place_text` (`path: "guide.md"`), `layout_page_add` to reach 20 pages,
`layout_preview` on a couple of spreads, `layout_preflight`, then
`layout_export`.

## 7. task 5: preflight and export for a print shop

The print shop wants: 3 mm bleed, crop marks, fonts embedded, CMYK colours,
images at 300 dpi. Open the flyer from task 2, check it, fix it, export.

### as a batch script

`printshop.txt`:

```
# Get the flyer ready for a print shop that asked for: 3 mm bleed, crop marks,
# embedded fonts, CMYK colours, images at 300 dpi.
layout.open path=flyer.wwlay -> fly
layout.preflight doc=$fly
# fix what preflight found: the photo is too small for its frame at 300 dpi
layout.place_image doc=$fly frame=photo path=hills.jpg fit=fill
layout.color_define doc=$fly name=green cmyk=85,20,70,15
layout.preflight doc=$fly
layout.export doc=$fly path=flyer-print.pdf bleed=true marks=true embed_fonts=true
layout.export doc=$fly path=flyer-proof.jpg dpi=100 quality=85
```

```
opened lay1 ("Green Valley Walking Club", 1 page 297 x 210 mm, 3 frames, flyer.wwlay)
preflight: 1 error(s), 0 warning(s), 1 note(s)
  error [low_res] page 1, f2 (photo): small.png prints at 129 dpi (400 x 300 px); 300 dpi needs it at most 33.9 mm wide
  ...
placed hills.jpg in f2 at 386 dpi
...
preflight passed, 1 note(s)
...
exported 1 page(s) to flyer-print.pdf (755 KB, bleed 3 mm, crop marks); fonts: Helvetica-Bold, Helvetica, Helvetica-Oblique; embedded: Helvetica-Bold (as LiberationSans-Bold), Helvetica (as LiberationSans), Helvetica-Oblique (as LiberationSans-Italic); mixes RGB and CMYK colours
exported 1 page image(s) at 100 dpi: flyer-proof.jpg
```

What preflight checks:

| kind | severity | meaning |
|---|---|---|
| `overset` | error | a story has text that fits in none of its frames |
| `missing_file` | error | a picture or font file can't be read |
| `low_res` | error under half of `min_dpi`, else warning | an image prints below `min_dpi` (default 300) at its placed size |
| `bleed` | warning | a picture or filled shape touches a trim edge without reaching the bleed edge (or the document has no bleed) |
| `off_page` | warning / info | a frame is entirely off the page, or extends past the bleed |
| `margin` | warning / info | a text frame crosses the trim, or sits outside the margins |
| `empty` | warning | a picture frame with nothing in it, an empty text frame |
| `rgb` | info | colours given as RGB; the printer will convert them |
| `fonts` | info | standard fonts used: export with `embed_fonts=true` |

`errors=0` (`ok: true`) means it's ready to export.

The export with **`embed_fonts=true`** embeds the TrueType substitutes, but
keeps the standard fonts' widths, so nothing reflows between your preview
and the printed sheet. `pdffonts flyer-print.pdf` lists them as embedded
TrueType. The "mixes RGB and CMYK" note comes from the RGB photo and the
SVG logo's RGB colours; the printer converts those, which is normal for
photos.

### from the shell

```sh
wetwaresuite batch < printshop.txt
pdfinfo -box flyer-print.pdf     # MediaBox, BleedBox and TrimBox
```

### asking Claude

> Get flyer.wwlay ready for the print shop: they want 3 mm bleed, crop marks,
> embedded fonts and 300 dpi images. Fix anything preflight complains about
> (use hills.jpg for the photo if it's too small) and export flyer-print.pdf.

Claude calls `layout_open`, `layout_preflight`, reads the `low_res` error,
`layout_place_image` with `frame: "photo"`, `layout_preflight` again until
`errors` is 0, then `layout_export` with `bleed`, `marks` and `embed_fonts`.

## 8. text: markup, styles, flow

**Markup** in `text=` and `append=`: a new line (`\\n` in batch) starts a
paragraph; `**bold**`, `*italic*`, `__underline__` and `` `code` `` (Courier)
work inline; `\*` is a literal asterisk, and a lone `*` between spaces stays
as it is. `markdown=true` reads the text as Markdown instead (headings,
lists, quotes), just like `place_text` does for `.md` files.

**Formatting a whole frame**: `frame_set` (and `frame_add`) take `font size
bold italic underline color tracking` and `leading align space_before
space_after indent left_indent break_before`; they apply to all of the
frame's text as local overrides. `style=` sets the paragraph style of every
paragraph.

**Styles**:

```
layout.style_define doc=$d name=lead based_on=body size=13 leading=17 italic=true
layout.style_define doc=$d name=price kind=character bold=true color=brand
layout.style_apply doc=$d frame=f3 style=lead paragraphs=1
layout.style_apply doc=$d frame=f3 style=price match="£15"
layout.style_apply doc=$d frame=f3 style=body paragraphs=2-4 clear=true
```

`match=` applies a character style to every occurrence of a piece of text;
`clear=true` removes local overrides so the style shows through.

**Leading** is baseline-to-baseline in points; `leading=0` is automatic
(1.2 x the largest size on the line), and a leading smaller than the type
switches to automatic so lines never collide.

**Alignment**: `left`, `center`, `right`, `justify` (last line of a paragraph
stays left), `inside`, `outside` (facing pages).

**Columns and frames**: `columns=` and `gutter=` on a frame; `inset=` pads it;
`valign=top|middle|bottom` positions the text vertically. Link frames with
`layout.frame_link from= to=`; `layout.frame_unlink frame=` ends the story in
that frame. `layout.info frame=f3` lists the story's paragraphs with their
styles.

**Masters**:

```
layout.master_set doc=$d master=A header="{title}" footer="Page {page} of {pages}" align=center size=8
layout.frame_add doc=$d master=A type=rect x=0 y=0 w=210 h=4 fill=brand
layout.master_set doc=$d master=B footer="{page}" apply_to=2-20
layout.page_set doc=$d page=1 master=none
```

## 9. pictures, shapes and colour

- `layout.place_image path=` without a size makes a frame at the picture's
  300 dpi size (shrunk to fit the margins), so it prints at 300 dpi. Give `w`
  (or `h`) alone to keep the aspect ratio, both for a fixed frame.
- `frame=` puts a new picture into an existing frame, keeping its size and
  crop mode.
- `layout.frame_set frame=f10 fit=manual image_scale=150 image_x=-20 image_y=0`
  places the picture at 150% of its 300 dpi size, 20 mm left of the frame's
  edge (the frame crops it).
- JPEGs go into the PDF untouched (no recompression); other formats are
  stored losslessly, with transparency kept as a soft mask.
- `.svg` files become `vector` frames: rects, circles, ellipses, lines,
  polylines, polygons, paths and groups with fill, stroke and opacity are
  drawn as vector paths.
- Shapes: `type=rect radius=4` (rounded), `type=ellipse`, `type=line`
  (`w`/`h` are the distance to the end point, `stroke_width` in pt).
- Colours: `layout.color_define name=brand cmyk=0,60,100,0` or
  `color=#ff6600`; then `fill=brand`, `color=brand`, ... CMYK values are
  percent.

## 10. arranging: align, distribute, order, undo

```
layout.align doc=$d frames=f2,f3,f4 edge=left              # to the leftmost
layout.align doc=$d frames=f5 edge=center to=page          # centre on the page
layout.align doc=$d frames=f2,f3 edge=top to=margins
layout.distribute doc=$d frames=f2,f3,f4 axis=horizontal   # equal gaps
layout.distribute doc=$d frames=f2,f3 axis=vertical gap=5  # fixed 5 mm gaps
layout.frame_order doc=$d frame=f1 order=back
layout.frame_duplicate doc=$d frame=f4 dy=60
layout.frame_set doc=$d frame=f4 page=2                    # move to page 2
undo
```

Edges: `left right top bottom center middle`; `to=selection|page|margins`.
Rotated frames align by their visual bounding box.

## 11. command reference

Every edit command also takes `preview=true` (attach a PNG of the page).
Frames are named by id (`f3`) or by `name=`. Pages are 1-based. Lengths are
mm, type sizes and stroke widths pt.

#### `layout.new`: New Layout

Create a print layout and return its id (e.g. lay1). Sizes are mm; pick a preset or size=A4/A5/letter/card/... or width= and height=. Then add frames (layout.frame_add, layout.place_text, layout.place_image) and export with layout.export. Coordinates are mm from the page's top-left trim corner, y down.

| param | type | required | default | meaning |
|---|---|---|---|---|
| `preset` | string |  |  | starting point: poster_a2, poster_a3, flyer_a5, flyer_a4 (landscape, 3 columns), card (85x55 mm), a4_report (page numbers), book_6x9 (facing pages, running head, page numbers), letter [poster_a2\|poster_a3\|flyer_a5\|flyer_a4\|card\|a4_report\|book_6x9\|letter] |
| `size` | string |  |  | paper size: A0, A1, A2, A3, A4, A5, A6, A7, B4, B5, DL, letter, legal, tabloid, half_letter, card, card_us, 6x9, 5x8, square_210 |
| `width` | number |  |  | page width in mm |
| `height` | number |  |  | page height in mm |
| `orientation` | string |  |  | portrait or landscape (swaps width and height if needed) [portrait\|landscape] |
| `pages` | integer |  | `1` | number of pages |
| `margins` | array |  |  | margins in mm: one number, vertical,horizontal, or top,right,bottom,left (with facing pages left = inside, right = outside) |
| `bleed` | number |  |  | bleed beyond the trim on every side, mm (3 is standard) |
| `columns` | integer |  |  | column guides (and autoflow frames) per page |
| `gutter` | number |  |  | space between columns, mm |
| `facing` | boolean |  |  | facing pages (books, magazines): margins become inside/outside |
| `title` | string |  |  | document title (PDF title and the {title} field) |

#### `layout.open`: Open Layout

Open a .wwlay layout file (older .achlay files open too) and return its id.

| param | type | required | default | meaning |
|---|---|---|---|---|
| `path` | string | yes |  | path to a .wwlay file |

#### `layout.save`: Save Layout

Save the layout as .wwlay (JSON: pages, frames, stories, styles, colours; pictures stay linked by path). For print files use layout.export.

| param | type | required | default | meaning |
|---|---|---|---|---|
| `doc` | string | yes |  | id of an open layout, e.g. lay1 |
| `path` | string | yes |  | output .wwlay file |

#### `layout.close`: Close Layout

Close a layout (undo reopens it).

| param | type | required | default | meaning |
|---|---|---|---|---|
| `doc` | string | yes |  | id of an open layout, e.g. lay1 |

#### `layout.info`: Layout Info

Describe a layout: page size, margins, bleed, every page with its frames (position, size, text excerpt, picture dpi, overset), stories, styles, colours, fonts and masters. page= or frame= narrows it.

| param | type | required | default | meaning |
|---|---|---|---|---|
| `doc` | string | yes |  | id of an open layout, e.g. lay1 |
| `page` | integer |  |  | only this page (1-based) |
| `frame` | string |  |  | only this frame, with its full text |

#### `layout.doc_set`: Document Setup

Change the title, margins, bleed, column guides or facing pages.

| param | type | required | default | meaning |
|---|---|---|---|---|
| `doc` | string | yes |  | id of an open layout, e.g. lay1 |
| `title` | string |  |  | document title ({title} field, PDF title) |
| `margins` | array |  |  | margins in mm: one number, vertical,horizontal, or top,right,bottom,left (with facing pages left = inside, right = outside) |
| `bleed` | number |  |  | bleed, mm |
| `columns` | integer |  |  | column guides per page |
| `gutter` | number |  |  | column gutter, mm |
| `facing` | boolean |  |  | facing pages |
| `preview` | boolean |  | `false` | attach a PNG preview of the page |

#### `layout.page_add`: Add Pages

Insert pages (default size: the document's).

| param | type | required | default | meaning |
|---|---|---|---|---|
| `doc` | string | yes |  | id of an open layout, e.g. lay1 |
| `count` | integer |  | `1` | pages to add |
| `after` | integer |  |  | insert after this page (0 = at the start; default: at the end) |
| `master` | string |  |  | master page for the new pages (default: the previous page's; none for none) |
| `size` | string |  |  | paper size: A0, A1, A2, A3, A4, A5, A6, A7, B4, B5, DL, letter, legal, tabloid, half_letter, card, card_us, 6x9, 5x8, square_210 |
| `width` | number |  |  | page width in mm |
| `height` | number |  |  | page height in mm |
| `orientation` | string |  |  | portrait or landscape (swaps width and height if needed) [portrait\|landscape] |
| `preview` | boolean |  | `false` | attach a PNG preview of the page |

#### `layout.page_delete`: Delete Pages

Delete pages and their frames; text in deleted frames flows on into the story's other frames.

| param | type | required | default | meaning |
|---|---|---|---|---|
| `doc` | string | yes |  | id of an open layout, e.g. lay1 |
| `page` | integer | yes |  | first page to delete (1-based) |
| `count` | integer |  | `1` | how many |

#### `layout.page_move`: Move Page

Move a page to another position (stories keep flowing in frame link order).

| param | type | required | default | meaning |
|---|---|---|---|---|
| `doc` | string | yes |  | id of an open layout, e.g. lay1 |
| `page` | integer | yes |  | page to move (1-based) |
| `to` | integer | yes |  | its new position (1-based) |

#### `layout.page_set`: Page Setup

Change a page's size, orientation or master page.

| param | type | required | default | meaning |
|---|---|---|---|---|
| `doc` | string | yes |  | id of an open layout, e.g. lay1 |
| `page` | integer | yes |  | page (1-based); 0 = every page |
| `master` | string |  |  | master page name, or none |
| `size` | string |  |  | paper size: A0, A1, A2, A3, A4, A5, A6, A7, B4, B5, DL, letter, legal, tabloid, half_letter, card, card_us, 6x9, 5x8, square_210 |
| `width` | number |  |  | page width in mm |
| `height` | number |  |  | page height in mm |
| `orientation` | string |  |  | portrait or landscape (swaps width and height if needed) [portrait\|landscape] |
| `preview` | boolean |  | `false` | attach a PNG preview of the page |

#### `layout.master_set`: Master Page

Create or change a master page: a running header and/or footer drawn on every page that uses it. Text may use {page}, {pages} and {title}; align=outside puts page numbers on the outer edge of facing pages. Other frames can be put on a master with layout.frame_add master=A. apply_to picks the pages that use it.

| param | type | required | default | meaning |
|---|---|---|---|---|
| `doc` | string | yes |  | id of an open layout, e.g. lay1 |
| `master` | string |  | `A` | master name |
| `header` | string |  |  | header text, e.g. {title} (empty removes it) |
| `footer` | string |  |  | footer text, e.g. Page {page} of {pages} (empty removes it) |
| `align` | string |  |  | header/footer alignment [left\|center\|right\|inside\|outside] |
| `font` | string |  |  | header/footer font |
| `size` | number |  |  | header/footer size, pt |
| `color` | string |  |  | header/footer colour |
| `apply_to` | string |  |  | pages that use this master: all, none, or a range like 2-20 (default: all when the master is new) |
| `remove` | boolean |  |  | delete the master (pages using it get none) |
| `preview` | boolean |  | `false` | attach a PNG preview of the page |

#### `layout.frame_add`: Add Frame

Add a frame and return its id. Without x/y/w/h it fills the page's margin box. text frames take text= (with **bold**/*italic* markup) and formatting (style, font, size, align, ...); image frames take path= (png, jpg, ...; svg becomes vector art) and fit=; rect/ellipse/line take fill, stroke and stroke_width (pt). A line runs from (x, y) to (x + w, y + h).

| param | type | required | default | meaning |
|---|---|---|---|---|
| `doc` | string | yes |  | id of an open layout, e.g. lay1 |
| `type` | string | yes |  | frame type [text\|image\|rect\|ellipse\|line\|vector] |
| `page` | integer |  | `1` | page (1-based) |
| `master` | string |  |  | put the frame on this master page instead of a page |
| `x` | number |  |  | left edge, mm from the page's left trim edge |
| `y` | number |  |  | top edge, mm from the page's top trim edge |
| `w` | number |  |  | width, mm (lines: x distance to the end point) |
| `h` | number |  |  | height, mm (lines: y distance to the end point) |
| `rotation` | number |  |  | degrees clockwise around the frame's centre |
| `name` | string |  |  | a label to find the frame by |
| `fill` | string |  |  | background colour (none, swatch, #hex, name, cmyk(c,m,y,k)) |
| `stroke` | string |  |  | outline colour |
| `stroke_width` | number |  |  | outline width, pt |
| `opacity` | number |  |  | 0..1 |
| `radius` | number |  |  | rect corner radius, mm |
| `columns` | integer |  |  | text columns in the frame |
| `gutter` | number |  |  | space between columns, mm |
| `inset` | number |  |  | padding inside a text frame, mm |
| `valign` | string |  |  | vertical text position in the frame [top\|middle\|bottom] |
| `text` | string |  |  | text content; new lines (or \n) split paragraphs; **bold**, *italic*, __underline__, `code` |
| `append` | string |  |  | text added as new paragraphs at the end of the story |
| `markdown` | boolean |  |  | read text as Markdown (# headings, - lists, > quotes) |
| `style` | string |  |  | paragraph style for every paragraph (body, h1, h2, h3, quote, bullet, caption, headline, subhead, ...) |
| `path` | string |  |  | image (png, jpg, ...) or SVG file for picture frames |
| `fit` | string |  |  | how a picture fills its frame: fit (whole picture), fill (cover, crops), stretch, manual [fit\|fill\|stretch\|manual] |
| `image_scale` | number |  |  | manual fit: percent of the 300 dpi size |
| `image_x` | number |  |  | manual fit: picture offset in the frame, mm |
| `image_y` | number |  |  | manual fit: picture offset in the frame, mm |
| `mirror` | boolean |  |  | master frames: mirror on left-hand pages of facing documents |
| `font` | string |  |  | font family: helvetica, times, courier (or sans, serif, mono) or a family added with layout.font_add |
| `size` | number |  |  | font size in pt |
| `bold` | boolean |  |  | bold |
| `italic` | boolean |  |  | italic |
| `underline` | boolean |  |  | underline |
| `color` | string |  |  | text colour: swatch name, #hex, a name, or cmyk(c,m,y,k) in percent |
| `tracking` | number |  |  | extra letter spacing in 1/1000 em (e.g. 50; negative tightens) |
| `leading` | number |  |  | line spacing in pt, baseline to baseline (0 = auto, 1.2 x size) |
| `align` | string |  |  | alignment; inside/outside follow the spine on facing pages [left\|center\|right\|justify\|inside\|outside] |
| `space_before` | number |  |  | space above each paragraph, pt |
| `space_after` | number |  |  | space below each paragraph, pt |
| `indent` | number |  |  | first-line indent, mm (negative for hanging bullets) |
| `left_indent` | number |  |  | left indent, mm |
| `break_before` | boolean |  |  | start each paragraph in the next linked frame (chapters on a new page) |
| `preview` | boolean |  | `false` | attach a PNG preview of the page |

#### `layout.frame_set`: Change Frame

Change a frame: position/size (mm), rotation, colours, columns, its text (text= replaces the whole story, append= adds paragraphs), formatting applied to all of its text (font, size, bold, align, leading, style, ...), or its picture (path, fit, image_scale/x/y).

| param | type | required | default | meaning |
|---|---|---|---|---|
| `doc` | string | yes |  | id of an open layout, e.g. lay1 |
| `frame` | string | yes |  | frame id (f3) or name (layout.info lists them) |
| `page` | integer |  |  | move the frame to this page (1-based) |
| `x` | number |  |  | left edge, mm from the page's left trim edge |
| `y` | number |  |  | top edge, mm from the page's top trim edge |
| `w` | number |  |  | width, mm (lines: x distance to the end point) |
| `h` | number |  |  | height, mm (lines: y distance to the end point) |
| `rotation` | number |  |  | degrees clockwise around the frame's centre |
| `name` | string |  |  | a label to find the frame by |
| `fill` | string |  |  | background colour (none, swatch, #hex, name, cmyk(c,m,y,k)) |
| `stroke` | string |  |  | outline colour |
| `stroke_width` | number |  |  | outline width, pt |
| `opacity` | number |  |  | 0..1 |
| `radius` | number |  |  | rect corner radius, mm |
| `columns` | integer |  |  | text columns in the frame |
| `gutter` | number |  |  | space between columns, mm |
| `inset` | number |  |  | padding inside a text frame, mm |
| `valign` | string |  |  | vertical text position in the frame [top\|middle\|bottom] |
| `text` | string |  |  | text content; new lines (or \n) split paragraphs; **bold**, *italic*, __underline__, `code` |
| `append` | string |  |  | text added as new paragraphs at the end of the story |
| `markdown` | boolean |  |  | read text as Markdown (# headings, - lists, > quotes) |
| `style` | string |  |  | paragraph style for every paragraph (body, h1, h2, h3, quote, bullet, caption, headline, subhead, ...) |
| `path` | string |  |  | image (png, jpg, ...) or SVG file for picture frames |
| `fit` | string |  |  | how a picture fills its frame: fit (whole picture), fill (cover, crops), stretch, manual [fit\|fill\|stretch\|manual] |
| `image_scale` | number |  |  | manual fit: percent of the 300 dpi size |
| `image_x` | number |  |  | manual fit: picture offset in the frame, mm |
| `image_y` | number |  |  | manual fit: picture offset in the frame, mm |
| `mirror` | boolean |  |  | master frames: mirror on left-hand pages of facing documents |
| `font` | string |  |  | font family: helvetica, times, courier (or sans, serif, mono) or a family added with layout.font_add |
| `size` | number |  |  | font size in pt |
| `bold` | boolean |  |  | bold |
| `italic` | boolean |  |  | italic |
| `underline` | boolean |  |  | underline |
| `color` | string |  |  | text colour: swatch name, #hex, a name, or cmyk(c,m,y,k) in percent |
| `tracking` | number |  |  | extra letter spacing in 1/1000 em (e.g. 50; negative tightens) |
| `leading` | number |  |  | line spacing in pt, baseline to baseline (0 = auto, 1.2 x size) |
| `align` | string |  |  | alignment; inside/outside follow the spine on facing pages [left\|center\|right\|justify\|inside\|outside] |
| `space_before` | number |  |  | space above each paragraph, pt |
| `space_after` | number |  |  | space below each paragraph, pt |
| `indent` | number |  |  | first-line indent, mm (negative for hanging bullets) |
| `left_indent` | number |  |  | left indent, mm |
| `break_before` | boolean |  |  | start each paragraph in the next linked frame (chapters on a new page) |
| `preview` | boolean |  | `false` | attach a PNG preview of the page |

#### `layout.frame_delete`: Delete Frame

Delete a frame; a linked text frame's text flows on into the rest of its chain.

| param | type | required | default | meaning |
|---|---|---|---|---|
| `doc` | string | yes |  | id of an open layout, e.g. lay1 |
| `frame` | string | yes |  | frame id (f3) or name (layout.info lists them) |
| `preview` | boolean |  | `false` | attach a PNG preview of the page |

#### `layout.frame_duplicate`: Duplicate Frame

Copy a frame (text frames get their own copy of the text) and return the new id; offset with dx/dy (mm) or put it on another page.

| param | type | required | default | meaning |
|---|---|---|---|---|
| `doc` | string | yes |  | id of an open layout, e.g. lay1 |
| `frame` | string | yes |  | frame id (f3) or name (layout.info lists them) |
| `dx` | number |  | `5` | horizontal offset, mm |
| `dy` | number |  | `5` | vertical offset, mm |
| `page` | integer |  |  | put the copy on this page (1-based, same position unless dx/dy given) |
| `preview` | boolean |  | `false` | attach a PNG preview of the page |

#### `layout.frame_order`: Arrange Frame

Bring a frame to the front, send it to the back, or move it one step.

| param | type | required | default | meaning |
|---|---|---|---|---|
| `doc` | string | yes |  | id of an open layout, e.g. lay1 |
| `frame` | string | yes |  | frame id (f3) or name (layout.info lists them) |
| `order` | string | yes |  | where to [front\|back\|forward\|backward] |
| `preview` | boolean |  | `false` | attach a PNG preview of the page |

#### `layout.frame_link`: Link Text Frames

Make a story flow on from one text frame into another (threading). Text already in the target frame is appended to the story. The overset count in the reply says whether it fits now.

| param | type | required | default | meaning |
|---|---|---|---|---|
| `doc` | string | yes |  | id of an open layout, e.g. lay1 |
| `from` | string | yes |  | frame id (f3) or name (layout.info lists them) |
| `to` | string | yes |  | frame id (f3) or name (layout.info lists them) |
| `preview` | boolean |  | `false` | attach a PNG preview of the page |

#### `layout.frame_unlink`: Unlink Text Frame

Cut the link after a text frame: the story ends in it, and the frames that followed become empty.

| param | type | required | default | meaning |
|---|---|---|---|---|
| `doc` | string | yes |  | id of an open layout, e.g. lay1 |
| `frame` | string | yes |  | frame id (f3) or name (layout.info lists them) |
| `preview` | boolean |  | `false` | attach a PNG preview of the page |

#### `layout.style_define`: Define Style

Create or change a paragraph or character style. Only the given fields change; the rest inherit from based_on. Every paragraph using the style reflows. Built in: body, h1, h2, h3, quote, bullet, code, caption, folio, headline, subhead (paragraph) and strong, emphasis (character).

| param | type | required | default | meaning |
|---|---|---|---|---|
| `doc` | string | yes |  | id of an open layout, e.g. lay1 |
| `name` | string | yes |  | style name |
| `kind` | string |  |  | paragraph (default) or character [paragraph\|character] |
| `based_on` | string |  |  | style to inherit from (paragraph styles default to body) |
| `font` | string |  |  | font family: helvetica, times, courier (or sans, serif, mono) or a family added with layout.font_add |
| `size` | number |  |  | font size in pt |
| `bold` | boolean |  |  | bold |
| `italic` | boolean |  |  | italic |
| `underline` | boolean |  |  | underline |
| `color` | string |  |  | text colour: swatch name, #hex, a name, or cmyk(c,m,y,k) in percent |
| `tracking` | number |  |  | extra letter spacing in 1/1000 em (e.g. 50; negative tightens) |
| `leading` | number |  |  | line spacing in pt, baseline to baseline (0 = auto, 1.2 x size) |
| `align` | string |  |  | alignment; inside/outside follow the spine on facing pages [left\|center\|right\|justify\|inside\|outside] |
| `space_before` | number |  |  | space above each paragraph, pt |
| `space_after` | number |  |  | space below each paragraph, pt |
| `indent` | number |  |  | first-line indent, mm (negative for hanging bullets) |
| `left_indent` | number |  |  | left indent, mm |
| `break_before` | boolean |  |  | start each paragraph in the next linked frame (chapters on a new page) |
| `preview` | boolean |  | `false` | attach a PNG preview of the page |

#### `layout.style_apply`: Apply Style

Apply a paragraph style to paragraphs of a frame's story (paragraphs=all or e.g. 1,3-5), or a character style to every occurrence of match= text (or to whole paragraphs). clear=true also drops local formatting.

| param | type | required | default | meaning |
|---|---|---|---|---|
| `doc` | string | yes |  | id of an open layout, e.g. lay1 |
| `style` | string | yes |  | style name |
| `frame` | string | yes |  | frame id (f3) or name (layout.info lists them) |
| `paragraphs` | string |  |  | which paragraphs, 1-based: all (default), 2, or 1,3-5 |
| `match` | string |  |  | character styles: only this text |
| `clear` | boolean |  |  | remove local formatting overrides |
| `preview` | boolean |  | `false` | attach a PNG preview of the page |

#### `layout.color_define`: Define Colour

Create or change a named colour swatch. cmyk= (percent, e.g. 0,60,100,0) makes a print colour exported as DeviceCMYK; color= (#hex or name) makes an RGB one. Use the name anywhere a colour is asked for.

| param | type | required | default | meaning |
|---|---|---|---|---|
| `doc` | string | yes |  | id of an open layout, e.g. lay1 |
| `name` | string | yes |  | swatch name, e.g. brand |
| `cmyk` | array |  |  | cyan, magenta, yellow, black in percent |
| `color` | string |  |  | RGB colour: #hex or a name |
| `preview` | boolean |  | `false` | attach a PNG preview of the page |

#### `layout.font_add`: Add Font

Register a TrueType (.ttf) family under a name usable as font= anywhere. The files are embedded in exported PDFs. Give the regular file and optionally bold, italic and bold_italic files.

| param | type | required | default | meaning |
|---|---|---|---|---|
| `doc` | string | yes |  | id of an open layout, e.g. lay1 |
| `name` | string | yes |  | family name to use, e.g. brand |
| `path` | string | yes |  | regular .ttf file |
| `bold` | string |  |  | bold .ttf |
| `italic` | string |  |  | italic .ttf |
| `bold_italic` | string |  |  | bold italic .ttf |

#### `layout.place_text`: Place Text

Flow a .txt or .md file into the layout. With autoflow (default) it fills a frame in the margins of the start page, then adds pages with linked frames until all the text fits: a whole booklet from one Markdown file. Markdown # ## ### become h1/h2/h3, lists bullets, > quotes, ``` code; **bold** and *italic* work in both.

| param | type | required | default | meaning |
|---|---|---|---|---|
| `doc` | string | yes |  | id of an open layout, e.g. lay1 |
| `path` | string | yes |  | .txt or .md file |
| `page` | integer |  | `1` | start page (1-based) |
| `frame` | string |  |  | existing text frame to fill (its story is replaced) |
| `markdown` | boolean |  |  | read as Markdown (default: true for .md/.markdown) |
| `style` | string |  | `body` | paragraph style for plain paragraphs |
| `autoflow` | boolean |  | `true` | add pages and linked frames until the text fits |
| `columns` | integer |  |  | columns in new frames (default: the document's column guides) |
| `master` | string |  |  | master page for added pages (default: the start page's) |
| `preview` | boolean |  | `false` | attach a PNG preview of the page |

#### `layout.place_image`: Place Image

Place a picture (png, jpg, bmp, gif, tga, psd, ...; .svg becomes vector art) in a new frame, or into an existing frame with frame=. Without w/h the frame takes the picture's 300 dpi size, shrunk to fit the margins. The reply gives the effective dpi at print size and warns under 300.

| param | type | required | default | meaning |
|---|---|---|---|---|
| `doc` | string | yes |  | id of an open layout, e.g. lay1 |
| `path` | string | yes |  | picture file |
| `page` | integer |  | `1` | page (1-based) |
| `frame` | string |  |  | existing image frame to put it in |
| `x` | number |  |  | left, mm (default: left margin) |
| `y` | number |  |  | top, mm (default: top margin) |
| `w` | number |  |  | width, mm |
| `h` | number |  |  | height, mm |
| `fit` | string |  | `fill` | fit (whole picture), fill (cover the frame, cropping), stretch, manual [fit\|fill\|stretch\|manual] |
| `name` | string |  |  | frame label |
| `preview` | boolean |  | `false` | attach a PNG preview of the page |

#### `layout.align`: Align Frames

Line frames up: their left/right/top/bottom edges or centres, relative to each other (selection), the page, or the margins.

| param | type | required | default | meaning |
|---|---|---|---|---|
| `doc` | string | yes |  | id of an open layout, e.g. lay1 |
| `frames` | string | yes |  | comma-separated frame ids, e.g. f1,f2,f3 |
| `edge` | string | yes |  | what to line up [left\|right\|top\|bottom\|center\|middle] |
| `to` | string |  | `selection` | relative to [selection\|page\|margins] |
| `preview` | boolean |  | `false` | attach a PNG preview of the page |

#### `layout.distribute`: Distribute Frames

Space frames evenly along an axis: equal gaps between the first and last (3+ frames), or a fixed gap= in mm starting from the first.

| param | type | required | default | meaning |
|---|---|---|---|---|
| `doc` | string | yes |  | id of an open layout, e.g. lay1 |
| `frames` | string | yes |  | comma-separated frame ids |
| `axis` | string | yes |  | direction [horizontal\|vertical] |
| `gap` | number |  |  | fixed gap between frames, mm |
| `preview` | boolean |  | `false` | attach a PNG preview of the page |

#### `layout.preflight`: Preflight

Check the layout before printing: overset text, missing pictures and fonts, images under min_dpi at print size, artwork touching the page edge without bleed, items off the page, text outside the margins, RGB colours and unembedded fonts. errors=0 means it's ready to export.

| param | type | required | default | meaning |
|---|---|---|---|---|
| `doc` | string | yes |  | id of an open layout, e.g. lay1 |
| `min_dpi` | number |  | `300` | lowest acceptable image resolution |

#### `layout.export`: Export Layout

Write a print PDF (bleed, crop marks, CMYK colours, optional embedded fonts) or PNG/JPEG pages at dpi. Several pages as images write name-1.png, name-2.png, ... (or use {page} in the path).

| param | type | required | default | meaning |
|---|---|---|---|---|
| `doc` | string | yes |  | id of an open layout, e.g. lay1 |
| `path` | string | yes |  | output .pdf, .png, .jpg (or .bmp/.tga) |
| `bleed` | boolean |  | `false` | include the bleed area (PDF: BleedBox; images: extra border) |
| `marks` | boolean |  | `false` | PDF: crop marks and a page label outside the bleed |
| `pages` | string |  |  | pages to export: all (default) or e.g. 1-3,5 |
| `dpi` | number |  | `300` | image exports: resolution |
| `embed_fonts` | boolean |  | `false` | PDF: embed TrueType substitutes for the standard fonts |
| `guides` | boolean |  | `false` | image exports: draw margins, columns and frame edges |
| `quality` | integer |  | `92` | JPEG quality 1-100 |

#### `layout.impose`: Impose N-up

Print several copies (or pages) per sheet: business cards 10-up on A4, labels, tickets. Writes a PDF with the items in a grid, centred, with crop marks on every cut line. gap >= 2 x bleed keeps each item's bleed; gap=0 butts them together (cut on the shared lines).

| param | type | required | default | meaning |
|---|---|---|---|---|
| `doc` | string | yes |  | id of an open layout, e.g. lay1 |
| `path` | string | yes |  | output .pdf |
| `sheet` | string |  | `A4` | sheet size: A0, A1, A2, A3, A4, A5, A6, A7, B4, B5, DL, letter, legal, tabloid, half_letter, card, card_us, 6x9, 5x8, square_210 |
| `orientation` | string |  |  | sheet orientation [portrait\|landscape] |
| `cols` | integer |  |  | columns (default: as many as fit) |
| `rows` | integer |  |  | rows (default: as many as fit) |
| `gap` | number |  | `0` | space between items, mm |
| `marks` | boolean |  | `true` | crop marks |
| `repeat` | boolean |  | `true` | true: fill each sheet with one page (front, then back); false: pages in turn |
| `pages` | string |  |  | pages to impose: all (default) or e.g. 1 |
| `embed_fonts` | boolean |  | `false` | embed TrueType substitutes for the standard fonts |

#### `layout.preview`: Preview Page

Render a page to PNG so you can see it, with margins (magenta), column gutters (violet), frame edges (blue), the bleed (red, with bleed=true) and a red square on frames whose text is overset.

| param | type | required | default | meaning |
|---|---|---|---|---|
| `doc` | string | yes |  | id of an open layout, e.g. lay1 |
| `page` | integer |  | `1` | page (1-based) |
| `dpi` | number |  |  | resolution (default: about 800 px on the long side) |
| `guides` | boolean |  | `true` | draw guides |
| `bleed` | boolean |  | `false` | show the bleed area |


## 12. limitations and troubleshooting

**Limitations** (speed and simplicity were chosen over these):

- Western European text uses the frame's font. Greek and Cyrillic letters
  are set in wetwaresuite's built-in font (Roboto) and embedded, so they
  print and copy correctly but don't match a serif or bold face. Chinese,
  Japanese, Korean, Arabic, Hebrew, Indic scripts and emoji still print as
  `?`. No right-to-left or complex scripts, no kerning pairs, ligatures or
  OpenType features.
- No hyphenation; justified lines stretch word spaces only. Very long words
  are broken where they hit the frame edge.
- No text wrap around pictures, no tab stops, drop caps, footnotes, tables,
  or inline pictures in text.
- Line breaking is greedy (one line at a time), not paragraph-optimal.
- Symbol and ZapfDingbats (two of the 14 standard PDF fonts) aren't offered.
  `font_add` takes TrueType (`.ttf`) files only, not CFF-based `.otf`; fonts
  are embedded whole (not subset), so each adds its file size to the PDF. A
  family without a bold or italic file uses the regular face (no fake bold).
- Previews and PNG exports draw the standard fonts with a metric-compatible
  system font when one is found (Liberation, Arimo, Arial, DejaVu...);
  without any system font text is shown as grey bars ("greeked"), and very
  small text is always greeked. The PDF is unaffected.
- RGB colours (and RGB pictures and SVG colours) are written as DeviceRGB;
  there is no colour management, ICC profile or PDF/X output. CMYK swatches
  preview with a simple conversion, so screen colours are approximate.
- SVG import is the vector tool's subset (rect, circle, ellipse, line,
  polyline, polygon, path, groups, transforms, fill/stroke/opacity); no text,
  gradients, patterns or clip paths in SVGs. Curves are flattened to short
  lines in the PDF.
- Layouts saved before the rename (`.achlay`) still open with `layout.open`.
- Pictures are linked by path, not stored in the `.wwlay` file; moving the
  pictures breaks the links (preflight reports `missing_file`).
- Imposition places whole pages in a grid; there is no booklet (saddle-stitch)
  page reordering or creep. Print the booklet PDF with your printer's or
  print shop's booklet setting.
- Each frame has one story position; frames can't overlap-flow around each
  other, and master text frames don't link.

**Troubleshooting**

- *"N characters overset"*: the text doesn't fit. Make the frame bigger,
  reduce the size, link another frame (`layout.frame_link`), or for long text
  use `layout.place_text` (autoflow adds pages).
- *My `\n` became an `n`*: in batch files and the shell write `\\n` inside
  `text="..."`, because the command reader eats one backslash.
- *What does `-> name` capture?* The document id from `new` and `open`, the
  frame id from every command that reports a frame (`frame_add`,
  `place_text`, `place_image`, `frame_duplicate`, `frame_set`, ...), and the
  document id from everything else.
- *Low-resolution warnings on a poster*: posters seen from a distance are
  fine at 150 dpi: `layout.preflight min_dpi=150`.
- *Text looks grey and blocky in the preview*: no system TrueType font was
  found; set `WETWARESUITE_FONT_DIR` to a folder with Liberation or DejaVu fonts.
  The PDF uses real fonts either way.
- *The print shop says fonts aren't embedded*: export with `embed_fonts=true`
  (needs one of the substitute fonts installed), or use your own `.ttf` via
  `layout.font_add`, which is always embedded.
