# Type: font design

`type` makes and edits fonts, headless. A **font** document holds metrics (units per em, ascender, descender, x-height, cap height), glyphs (outlines of quadratic and cubic contours, or components that reference other glyphs), kerning pairs and the names and license that go into the file. You can:

- **draw** glyphs from rectangles, ellipses, point lists and SVG path data, or import them from SVG drawings;
- **trace** a scanned (or photographed) sheet of handwriting into a whole font;
- **open** existing fonts (TTF, OTF with CFF outlines, TTC, WOFF, UFO) to look at, proof, subset, convert or change;
- **reshape** them: move, scale, rotate, slant (oblique), embolden, remove overlaps;
- **space and kern**: automatic side bearings, automatic kerning (lite), pairs by hand;
- **look** at the result: a glyph with its points and metric lines, a line of text, a glyph overview, a specimen sheet as PNG or a multi-page PDF, all set in the font itself;
- **export** a TrueType `.ttf` you can install, a `.woff` for the web and an SVG font; and save the source as `.typeface.json` or a UFO folder.

Each task below is shown three ways:

- **CLI**: single `wetwaresuite` commands. A font only stays open inside one process, so the one-shot commands are the ones that read a file and write a result: `info`, `glyph_info`, `kern_list`, `measure`, `preview`, `render`, `proof` and `export` take `font=<file>` instead of `doc=`, and `grid_template` needs no font at all.
- **Batch**: a script for multi-step work with `-> name` captures and `$name` reuse (see [headless and batch](headless-and-batch.md)).
- **Claude**: a prompt for Claude over MCP and the tools it calls (see [mcp.md](mcp.md)). `wetwaresuite mcp --tools type` serves the type tools.

Every example below was run against the real binary in empty folders. Outputs are copied from those runs; lines that repeat the message as `key: value` fields are cut where marked `...`.

## How type works

**Font units, y up.** Coordinates are font units on a grid of `units_per_em` (1000 by default; TrueType fonts often use 2048). The baseline is y = 0, y grows upwards, the glyph's origin is x = 0 and its advance width is how far the pen moves on. A new font has ascender 800, descender -200, x-height 500 and cap height 700. `y_down=true` on drawing commands takes screen-style coordinates instead (y down from the ascender line), handy when copying numbers from a drawing program.

**Glyphs** have a name (`A`, `a`, `zero`, `period`, `Aacute`, `uni00E9`) and code points. Wherever a command asks for a glyph you can give its name, the character itself (`é`) or the code point (`U+00E9`). New glyphs named by a character get the standard name and the code point: `glyph=!` makes `exclam` on U+0021, `glyph=é` makes `uni00E9` on U+00E9.

**Glyph lists** (`glyphs=` on the shape and spacing commands, `glyphs=`/`keep=` on `type.glyph_remove`) are comma-separated names or characters, ranges such as `A-Z` or `U+0400-U+04FF`, and the words `uppercase`, `lowercase`, `digits`, `letters`, `ascii`, `latin1`, `encoded`, `unencoded` and `all` (the default).

**Contours.** Outer contours run clockwise and holes (counters) counter-clockwise, the TrueType rule. You don't have to think about it: drawing commands orient what you draw, `mode=cut` makes holes, and contours that overlap simply fill (nonzero) until `type.remove_overlap` merges them into one clean outline. Quadratic (`Q`) and cubic (`C`) curves are both kept; TTF export converts cubics to quadratics within `tolerance`.

**Components.** A glyph can reference others: `Aacute` = `A` + `acute` moved into place. Edits to `A` show up in `Aacute`, and exported TTFs keep them as composite glyphs. `type.transform`, `type.oblique` and `type.embolden` keep components when the bases change too (so nothing changes twice), and draw composites in to plain contours when only the composite is changed.

**Undo.** Every edit is one undo step. `new`, `open`, `info`, `glyph_info`, `kern_list`, `measure` and `preview` change nothing; `save`, `export`, `render`, `proof` and `grid_template` write files and aren't undoable.

**Previews.** Every edit takes `preview=true` and attaches a PNG: the glyph that changed, large, with its points (filled = on the curve, hollow = control points), handles and the metric lines, or the glyph overview when several changed. `type.preview` shows a glyph, a line of text or the overview without writing anything. This is how Claude checks its work.

## Task 1: a display font from scratch

A small geometric caps-and-lowercase font, *Blocky*, drawn from rectangles, ellipses and path data, spaced, kerned, proofed and exported.

**Batch** (`blocky.txt`):

```text
# blocky.txt: a small geometric display font drawn from rectangles, ellipses and paths
type.new family=Blocky -> f
type.set_info doc=$f designer="Ada Example" version=1.001 license="SIL Open Font License 1.1" license_url=https://openfontlicense.org
# straight letters from rectangles (overlaps are fine; remove_overlap merges them below)
type.glyph_add doc=$f name=I rect=0,0,110,700
type.draw doc=$f glyph=H rect=0,0,110,700
type.draw doc=$f glyph=H rect=370,0,110,700
type.draw doc=$f glyph=H rect=0,300,480,100
type.draw doc=$f glyph=T rect=0,590,560,110
type.draw doc=$f glyph=T rect=225,0,110,600
type.draw doc=$f glyph=L rect=0,0,110,700
type.draw doc=$f glyph=L rect=0,0,440,110
# diagonals from path data (font units, y up from the baseline)
type.draw doc=$f glyph=A path="M0 0H120L300 540L480 0H600L360 700H240Z"
type.draw doc=$f glyph=A rect=130,170,340,95
type.draw doc=$f glyph=V path="M240 0H360L600 700H480L300 160L120 700H0Z"
# round letters: an ellipse, then a smaller one cut out as the counter
type.draw doc=$f glyph=O ellipse=320,350,320,365
type.draw doc=$f glyph=O ellipse=320,350,205,250 mode=cut
type.draw doc=$f glyph=o ellipse=270,250,270,262
type.draw doc=$f glyph=o ellipse=270,250,160,155 mode=cut
# n with quadratic curves, and a round period
type.draw doc=$f glyph=n path="M0 0H110V320Q110 410 210 410Q300 410 300 320V0H410V340Q410 512 240 512Q160 512 110 450V500H0Z"
type.draw doc=$f glyph=. ellipse=60,60,60,60
type.remove_overlap doc=$f
type.metrics_auto doc=$f spacing=60 vertical=false
type.kern_auto doc=$f
type.kern_set doc=$f pairs="L T -110; L V -100"
type.info doc=$f
type.preview doc=$f text="HALT. VOLT on"
type.render doc=$f text="HALT. VOLT on TOTAL" size=90 path=blocky.png
type.proof doc=$f path=blocky-proof.pdf
type.save doc=$f path=blocky.typeface.json
type.export doc=$f path=Blocky-Regular.ttf
type.export doc=$f path=Blocky-Regular.woff
```

```sh
$ wetwaresuite batch blocky.txt
created font1: Blocky Regular, 2 glyphs (1 encoded), 0 kerning pairs, 1000 units/em
...
set version, license, license_url, designer on Blocky Regular
...
added I U+0049 advance 110, lsb 0, rsb 0, 1 contour (4 points)
...
created H U+0048 advance 110, lsb 0, rsb 0, 1 contour (4 points)
...
drew into H U+0048 advance 110, lsb 0, rsb -370, 3 contours (12 points)
...
drew into A U+0041 advance 600, lsb 0, rsb 0, 2 contours (11 points)
...
drew into O U+004F advance 640, lsb 0, rsb 0, 2 contours (24 points)
...
created n U+006E advance 410, lsb 0, rsb 0, 1 contour (16 points)
...
created period U+002E advance 120, lsb 0, rsb 0, 1 contour (12 points)
...
removed overlaps (17 contours -> 13) in 4 glyphs
...
spaced 10 glyphs (lsb 60, rsb 60)
...
auto-kerned 10 glyphs: 71 pairs found, 71 added (target gap 120); strongest: L/T -249, L/V -227, A/T -202, T/A -202, T/o -192, o/T -192, T/period -187, period/T -187
...
set 2 kerning pairs
...
Blocky Regular, 12 glyphs (11 encoded), 71 kerning pairs, 1000 units/em
version 1.001, weight 400, designer Ada Example
license: SIL Open Font License 1.1
metrics: ascender 800, descender -200, x-height 500, cap height 700, line gap 0
missing ASCII (84): !"#$%&'()*+,-/0123456789:;<=>?@BCDEFGJKMNPQRSUWXYZ[\]^_`abcdefghijklmpqrstuvwxyz{|}~
...
preview of text: 504x104
...
rendered 903x122 to blocky.png
...
wrote a PDF proof of Blocky Regular to blocky-proof.pdf (63279 bytes)
...
saved Blocky Regular to blocky.typeface.json
...
exported Blocky Regular to Blocky-Regular.ttf (3084 bytes, 12 glyphs)
...
exported Blocky Regular to Blocky-Regular.woff (2312 bytes, 12 glyphs)
```

Things to notice:

- **Draw first, space later.** The first contour of a new glyph sets its advance (here the width of the first rectangle, which is why `H` shows `rsb -370` until it is spaced). `type.metrics_auto` then gives every glyph the same side bearings (60 units here; round letters such as `O` and `o` get 80 % of that, since they look further apart). `vertical=true` (the default) also reads x-height, cap height, ascender and descender off `x`, `H`, `d` and `p` when the font has them; Blocky keeps the defaults.
- **Overlaps are fine while drawing.** `H` is three rectangles, `A` a path plus a crossbar. They render correctly as they are; `type.remove_overlap` merges them into single contours (H: 3 contours to 1; A: one outline plus the triangular counter) and leaves glyphs that don't overlap exactly as drawn.
- **Counters** are cut with `mode=cut`; with path data you can also draw the counter as a second subpath: `type.draw glyph=U+25A1 path="M0 0H600V600H0Z M150 150H450V450H150Z"` is a square with a square hole, since direction is sorted out by nesting.
- **Kerning.** `type.kern_auto` measured the gap between two `H`s (120 units) and evened out the optical gap of every pair to it. Its first guesses are often strong (L/T -249 here); `type.kern_set` overrides single pairs (`value=0` removes one). Check them on the proof: its kerning section prints the strongest pairs with and without kerning.
- **A proof of a small font.** The proof sets the default sample text and paragraphs only when the font has most of their characters; otherwise it uses upper- or lower-cased versions, or words made of the font's own characters, so even a 10-glyph font gets a useful sheet. Pass `sample=` and `paragraph=` to set your own.

**CLI.** The saved files work one-shot:

```sh
$ wetwaresuite type.export font=blocky.typeface.json path=blocky.svg
exported Blocky Regular to blocky.svg (5526 bytes, 16 glyphs)
path: blocky.svg
format: svg
bytes: 5526
glyphs: 16
```

**Claude.** *"Make me a geometric display font called Blocky with H, I, L, T, A, V, O, o, n and a period: 110-unit stems, round letters from ellipses. Space it, kern it and export a TTF."* Claude calls `type_new`, then `type_draw` for each glyph with `preview=true` (so it sees each glyph with its points and metric lines after drawing it), `type_remove_overlap`, `type_metrics_auto`, `type_kern_auto`, `type_preview text=...` to look at a line, `type_kern_set` for pairs that look off, and `type_export path=Blocky-Regular.ttf`.

## Task 2: an SVG icon, accented letters and hand kerning

Add a heart from an SVG file, an acute accent and two accented capitals built from components, then kern by hand.

`heart.svg` is any SVG drawing; filled paths, rects, circles, ellipses and polygons (with their transforms) become the outline, strokes and text are skipped:

```xml
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100" width="100" height="100">
  <path fill="#c00" d="M50 88 C20 66 6 50 6 32 C6 18 17 8 30 8 C39 8 46 13 50 20 C54 13 61 8 70 8 C83 8 94 18 94 32 C94 50 80 66 50 88 Z"/>
</svg>
```

**Batch** (`extend.txt`):

```text
# extend.txt: add an SVG icon, accented letters and kerning to Blocky
type.open path=blocky.typeface.json -> f
# an icon from an SVG drawing, scaled to the cap height and put on U+2665
type.import_svg doc=$f glyph=U+2665 path=heart.svg fit=cap
# an acute accent, then Aacute and Oacute built from components
type.draw doc=$f glyph=acute points=60,760,160,760,260,880,150,880 advance=320
type.glyph_copy doc=$f from=A to=Á component=true
type.component_add doc=$f glyph=Á base=acute center=true dy=20
type.glyph_copy doc=$f from=O to=Ó component=true
type.component_add doc=$f glyph=Ó base=acute center=true dy=40
type.glyph_info doc=$f glyph=Á
# a space a little narrower, and kerning by hand on top of kern_auto
type.glyph_set doc=$f glyph=space advance=220
type.kern_set doc=$f pairs="T Ó -150; Á V -150; V Á -150"
type.kern_list doc=$f glyph=T limit=6
type.measure doc=$f text="VOTÁ" size=48
type.measure doc=$f text="VOTÁ" size=48 kerning=false
type.render doc=$f text="ÁLTO ♥ VOLT" size=90 path=extended.png
type.save doc=$f path=blocky.typeface.json
type.export doc=$f path=Blocky-Regular.ttf
```

```sh
$ wetwaresuite batch extend.txt
opened blocky.typeface.json as font1: Blocky Regular, 12 glyphs (11 encoded), 71 kerning pairs, 1000 units/em, blocky.typeface.json
...
imported 1 contour into uni2665 U+2665 advance 890, lsb 60, rsb 60, 1 contour (18 points)
...
created acute advance 320, lsb 60, rsb 60, 1 contour (4 points)
...
copied A to uni00C1 U+00C1 advance 720, lsb 60, rsb 60, 0 contours (0 points), components: A@0,0
...
updated uni00C1 U+00C1 advance 720, lsb 60, rsb 60, 0 contours (0 points), components: A@0,0 acute@200,20
...
updated uni00D3 U+00D3 advance 736, lsb 48, rsb 48, 0 contours (0 points), components: O@0,0 acute@208,40
...
updated space U+0020 advance 220, 0 contours (0 points)
...
set 3 kerning pairs
...
20 kerning pairs with T
  A T -202
  T A -202
  T o -192
  o T -192
  T period -187
  period T -187
  ... 14 more
...
"VOTÁ" is 2631 units wide (126.29 px at 48 px)
...
"VOTÁ" is 2856 units wide (137.09 px at 48 px)
...
rendered 596x122 to extended.png
...
exported Blocky Regular to Blocky-Regular.ttf (3428 bytes, 16 glyphs)
```

- **`fit=`** decides how the SVG maps onto the em. `canvas` (the default) treats the SVG's height as the space from descender to ascender, as if the SVG were the glyph's box (so drawings made on a template keep their position); `cap`, `xheight` and `ascender` scale the ink to that height standing on the baseline, with even side bearings; `none` takes SVG units as font units. The SVG's y-down coordinates are flipped for you.
- **`center=true`** on `type.component_add` centres the accent over the glyph's current outline; `dx`/`dy` move it from there (or from the origin without `center`). `scale_x=-1` mirrors a component.
- **`glyph=Á`** names the glyph by its character; it got the standard name `uni00C1` and the code point U+00C1. `type.glyph_set glyph=Á name=Aacute` renames it (components and kerning follow).
- **`type.measure`** says how wide a line is with and without kerning, and which characters are missing.

**Claude.** *"Add a heart on U+2665 from heart.svg at cap height, and Á and Ó built from an acute accent."* Claude calls `type_import_svg` (`glyph=U+2665 path=heart.svg fit=cap preview=true`), `type_draw` for the accent, `type_glyph_copy component=true` and `type_component_add center=true` for each accented letter, then `type_preview text="ÁÓ"` to check the accents sit right.

## Task 3: a font from your handwriting

Print a template, write one letter per cell, scan it, and trace the scan into a font.

**1. The template.** One cell per character with light guides for the baseline, x-height and cap height (light enough for the tracer to ignore) and a faint label in the corner:

```sh
$ wetwaresuite type.grid_template path=sheet.png chars=HAVOTnoi cols=4
drew a template with 8 cells (960x480) to sheet.png; trace it with cols=4
path: sheet.png
cells: 8
cols: 4
width: 960
height: 480
```

`chars=` is the characters in reading order (a space leaves a cell empty) or `uppercase`, `lowercase`, `digits`, `letters`, `basic` (letters, digits and punctuation) or `ascii`. The default cell is 240 px, about 2 cm when printed at 300 dpi; `chars=uppercase cols=7` makes a 1680 x 960 sheet. Pass `doc=` to place the guides at an existing font's metrics.

**2. Write and scan.** Write each letter in its cell with a dark pen, standing on the baseline guide, and scan the sheet (or photograph it flat and crop it to the grid). Any size works as long as the grid fills the image; `margin=`, `margin_x=` and `margin_y=` skip a border around the grid.

For this tutorial the "scan" was painted onto the template with the paint tool (`scan.txt`: `photo.open path=sheet.png`, a few `paint.stroke brush=ink size=20 points=...` lines per letter, `photo.save path=scan.png`).

**3. Trace.** **Batch** (`hand.txt`):

```text
# hand.txt: a font from a scanned sheet of handwriting
type.new family="My Hand" -> f
type.trace_grid doc=$f image=scan.png chars=HAVOTnoi cols=4
type.metrics_auto doc=$f spacing=50
type.kern_auto doc=$f
type.kern_list doc=$f limit=5
type.info doc=$f
type.render doc=$f text="TOTAH in HAVOT" size=96 path=hand.png
type.save doc=$f path=hand.typeface.json
type.export doc=$f path=MyHand-Regular.ttf
```

```sh
$ wetwaresuite batch hand.txt
created font1: My Hand Regular, 2 glyphs (1 encoded), 0 kerning pairs, 1000 units/em
...
traced 8 glyphs from a 2x4 grid: HAVOTnoi
doc: font1
traced: 8
glyph_chars: HAVOTnoi
empty: 
spaced 8 glyphs (lsb 50, rsb 50); set cap_height=725
...
auto-kerned 8 glyphs: 44 pairs found, 44 added (target gap 122); strongest: o/T -264, T/A -260, T/o -260, n/T -260, A/T -258, T/n -249, V/A -198, A/V -195
...
44 kerning pairs
  o T -264
  T A -260
  T o -260
  n T -260
  A T -258
  ... 39 more
...
My Hand Regular, 10 glyphs (9 encoded), 44 kerning pairs, 1000 units/em
...
rendered 785x128 to hand.png
...
exported My Hand Regular to MyHand-Regular.ttf (3260 bytes, 10 glyphs)
```

How the tracer works: the sheet is cut into `rows` x `cols` cells, `inset` (6 %) of each cell's edge is ignored (grid lines and labels), pixels darker than `threshold` (0.5) count as ink, specks under `min_area` pixels are dropped, the ink's outline is followed (marching squares) and fitted with cubic Bezier curves to within `tolerance` pixels, keeping turns sharper than `corner` degrees as corners. The cell's top is the ascender and its bottom the descender, so what you write on the baseline guide sits on the font's baseline. `empty:` lists cells with no ink. Here `metrics_auto` also read the cap height (725) off the traced `H`, since handwriting rarely hits the guide exactly.

**One more letter on its own.** `type.trace_image` traces one image into one glyph (a letter drawn later, a logo mark, a brush stroke). `fit=cell` (the default) treats the image like a grid cell; `cap`, `xheight` and `ascender` scale the ink to that height on the baseline (`fit=cap` also for letters with descenders: they end up standing on the baseline, so move them down with `type.transform dy=`).

```text
# letter.txt: one more letter, drawn on its own, traced into the handwriting font
paint.new width=300 height=300 -> p
paint.stroke doc=$p brush=ink size=22 points=90,30,92,150,90,270
paint.stroke doc=$p brush=ink size=22 points=210,120,150,180,96,206
paint.stroke doc=$p brush=ink size=22 points=140,186,180,230,215,270
photo.save doc=$p path=k.png
type.open path=hand.typeface.json -> f
type.trace_image doc=$f image=k.png glyph=k fit=ascender
type.glyph_info doc=$f glyph=k
type.render doc=$f text="ok kin TOVA" size=96 path=hand2.png
type.save doc=$f path=hand.typeface.json
```

```sh
$ wetwaresuite batch letter.txt
...
traced 1 contour into k U+006B advance 571, lsb 60, rsb 59.5, 1 contour (35 points)
...
rendered 584x128 to hand2.png
...
```

**CLI.** The template is a one-shot (above); the result can be checked one-shot too:

```sh
$ wetwaresuite type.info font=MyHand-Regular.ttf
My Hand Regular, 10 glyphs (9 encoded), 44 kerning pairs, 1000 units/em, MyHand-Regular.ttf
version 1.000, weight 400
metrics: ascender 800, descender -200, x-height 500, cap height 725, line gap 0
missing ASCII (86): !"#$%&'()*+,-./0123456789:;<=>?@BCDEFGIJKLMNPQRSUWXYZ[\]^_`abcdefghjklmpqrstuvwxyz{|}~
...
```

**Claude.** *"Print me a handwriting template for the alphabet in upper and lower case."* `type_grid_template path=sheet.png chars=letters cols=10`. Then: *"Here's the scan (scan.jpg). Make it into a font called My Hand and show me."* Claude calls `type_new family="My Hand"`, `type_trace_grid image=scan.jpg chars=letters cols=10 preview=true` (the overview shows every traced glyph, so it can spot a letter that came out wrong, an `empty:` cell, or ink from a neighbouring cell), `type_metrics_auto`, `type_kern_auto`, `type_proof path=myhand.png` and `type_export path=MyHand-Regular.ttf`. If the ink is faint it retries with a lower `threshold=0.3` (less darkness counts as ink); if letters come out lumpy, `tolerance=2`.

## Task 4: open, proof, subset and convert an existing font

You have an OpenType font with CFF outlines (`InterDisplay-Light.otf`) and a TrueType one (`CrimsonPro-Regular.ttf`).

**CLI.** Everything that only reads the font works one-shot with `font=`:

```sh
$ wetwaresuite type.info font=CrimsonPro-Regular.ttf
Crimson Pro Regular, 823 glyphs (689 encoded), 76980 kerning pairs, 1024 units/em, CrimsonPro-Regular.ttf
version 1.003, weight 400, designer Jacques Le Bailly
license: This Font Software is licensed under the SIL Open Font License, Version 1.1. This license is available with a FAQ at: https://scripts.sil.org/OFL
copyright: Copyright 2018 The Crimson Pro Project Authors (https://github.com/Fonthausen/CrimsonPro)
metrics: ascender 918, descender -220, x-height 430, cap height 587, line gap 0
all printable ASCII is present
...

$ wetwaresuite type.export font=CrimsonPro-Regular.ttf path=CrimsonPro-Regular.woff
exported Crimson Pro Regular to CrimsonPro-Regular.woff (136064 bytes, 823 glyphs)
...

$ wetwaresuite type.proof font=CrimsonPro-Regular.ttf path=crimson-proof.png
wrote a 1200x7243 proof of Crimson Pro Regular to crimson-proof.png
...

$ wetwaresuite type.render font=CrimsonPro-Regular.ttf text="Hamburgefonstiv" size=96 path=crimson.png
rendered 704x139 to crimson.png
...

$ wetwaresuite type.measure font=CrimsonPro-Regular.ttf text="AVATAR Wave" size=24
"AVATAR Wave" is 5795 units wide (135.82 px at 24 px)
...

$ wetwaresuite type.kern_list font=CrimsonPro-Regular.ttf glyph=V limit=5
654 kerning pairs with V
  V four.osf -105
  V AE -74
  V AEacute -74
  V slash -70
  backslash V -70
  ... 649 more
...

$ wetwaresuite type.glyph_info font=CrimsonPro-Regular.ttf glyph=l points=true
l U+006C advance 269, lsb 16, rsb 26, 1 contour (27 points)
path: M28 0L26 37L91 51Q93 61 94 84Q95 107 95.5 145Q96 183 96 238L96 363Q96 401 95.5 441Q95 481 94.5 516.5Q94 552 93.5 577.5Q93 603 92 611L18 621L16 653L159 694L178 680Q177 638 176 596Q175 554 174.5 503Q174 452 174 380L173 219Q173 160 174.5 112.5Q176 65 178 51L243 37L241 0Z
contour 0: 28,0 26,37 91,51 93,61q 95,107q 96,183q 96,238 96,363 96,401q 95,481q 94,552q 93,603q 92,611 18,621 16,653 159,694 178,680 177,638q 175,554q 174,452q 174,380 173,219 173,160q 176,65q 178,51 243,37 241,0
...
```

In point lists, `q` marks a quadratic control point and `c` a cubic one; in TrueType outlines two `q` points in a row have an implied on-curve point halfway between them, which the `path:` (SVG path data, y up) spells out.

**Batch** (`convert.txt`): CFF to TrueType, a PDF proof, and a small web font with only ASCII:

```text
# convert.txt: an OpenType (CFF) font to TTF and WOFF, with a proof, and a small web subset
type.open path=InterDisplay-Light.otf -> f
type.export doc=$f path=InterDisplay-Light.ttf
type.export doc=$f path=InterDisplay-Light.woff
type.proof doc=$f path=inter-proof.pdf
# a subset with just the basic Latin letters, digits and punctuation for a headline font
type.glyph_remove doc=$f keep=ascii
type.info doc=$f
type.export doc=$f path=InterDisplay-Light-latin.woff
```

```sh
$ wetwaresuite batch convert.txt
opened InterDisplay-Light.otf as font1: Inter Display Light, 2926 glyphs (2838 encoded), 300000 kerning pairs, 2048 units/em, InterDisplay-Light.otf
warning: kerning has more than 300000 pairs; the rest were skipped
...
exported Inter Display Light to InterDisplay-Light.ttf (1761824 bytes, 2926 glyphs)
...
exported Inter Display Light to InterDisplay-Light.woff (367940 bytes, 2926 glyphs)
...
wrote a PDF proof of Inter Display Light to inter-proof.pdf (1903773 bytes)
...
removed 2830 glyphs: uni0000, uni00C4, uni00C3, uni00C0, uni00C1, uni00C2, uni00C5, uni01FA, uniEE02, uni0100, uni0102, uni0104, ... 2818 more (96 glyphs left)
...
Inter Display Light, 96 glyphs (95 encoded), 846 kerning pairs, 2048 units/em, InterDisplay-Light.otf
version 4.000, weight 300, designer Rasmus Andersson
...
all printable ASCII is present
...
exported Inter Display Light to InterDisplay-Light-latin.woff (15668 bytes, 96 glyphs)
```

- **Kerning comes along.** The `kern` table and GPOS pair kerning (glyph pairs and class pairs) are read, class kerning is expanded into glyph pairs (up to 300 000, hence the warning for Inter's huge class tables), and both are written back on export.
- **Subsetting.** `keep=` removes every glyph except those (plus `.notdef` and the bases of kept composites), and their kerning with them: 368 KB down to 16 KB here. `glyphs=` removes just the listed ones (`glyphs=U+0100-U+017F`).
- **The PDF proof** is A4 pages with vector outlines: the family name large, the character set, a waterfall of sizes, paragraphs at 18 and 12 pt, the strongest kerning pairs with and without kerning, and a grid of every glyph with its name and code point. The PNG proof is the same as one long image.
- **Names.** Glyph names come from the font's `post` table. Fonts without names there (CFF fonts, `post` format 3) get `uniXXXX` names from their code points (standard names such as `A` and `period` for ASCII) and `glyphN` for unencoded glyphs.

**Claude.** *"What's in CrimsonPro-Regular.ttf? Show me the lowercase g and how it kerns with V."* Claude calls `type_open path=CrimsonPro-Regular.ttf`, `type_info`, `type_preview glyph=g` (a PNG of the glyph with its points and metric lines comes back in the result) and `type_kern_list glyph=V`. *"Make a WOFF of it with only Latin-1."* `type_glyph_remove keep=latin1`, `type_export path=crimson-latin1.woff`.

## Task 5: bold and oblique styles from a regular

Make Bold, Oblique and Bold Oblique from the Blocky source.

**Batch** (`variants.txt`):

```text
# variants.txt: Bold, Oblique and Bold Oblique from the Regular source
type.open path=blocky.typeface.json -> r
type.embolden doc=$r amount=28
type.remove_overlap doc=$r
type.set_info doc=$r style=Bold weight=700
type.export doc=$r path=Blocky-Bold.ttf
type.oblique doc=$r angle=10 style="Bold Oblique"
type.export doc=$r path=Blocky-BoldOblique.ttf
type.open path=blocky.typeface.json -> o
type.oblique doc=$o angle=10 style=Oblique
type.export doc=$o path=Blocky-Oblique.ttf
type.render doc=$o text="VOLT ÁLTO" size=72 path=oblique.png
type.render doc=$r text="VOLT ÁLTO" size=72 path=boldoblique.png
```

```sh
$ wetwaresuite batch variants.txt
opened blocky.typeface.json as font1: Blocky Regular, 16 glyphs (14 encoded), 74 kerning pairs, 1000 units/em, blocky.typeface.json
...
emboldened by 28 units: 15 glyphs
...
removed overlaps (15 contours -> 15) in 0 glyphs
...
set style, weight on Blocky Bold
...
exported Blocky Bold to Blocky-Bold.ttf (3392 bytes, 16 glyphs)
...
slanted 10 degrees: 15 glyphs
...
exported Blocky Bold Oblique to Blocky-BoldOblique.ttf (3648 bytes, 16 glyphs)
...
exported Blocky Oblique to Blocky-Oblique.ttf (3568 bytes, 16 glyphs)
...
```

- **`amount`** is per side: stems grow by twice it (110 to 166 units here), and the advance grows by 2 x amount so the spacing stays the same (`keep_advance=true` keeps the widths, for a tabular font). `amount_y=0` keeps heights; a negative amount makes a lighter style.
- **`type.oblique`** skews around half the x-height so letters lean without drifting sideways, sets the font's italic angle (-10 here) and, with `style=`, the style name. Exported fonts get the right style bits: "Bold Oblique" is written as family "Blocky Bold Oblique", subfamily "Bold Italic", so apps group it with the others.
- **Composites stay composites**: Á and Ó were not slanted or emboldened twice, since their bases A, O and acute changed with them.
- After emboldening, run `type.remove_overlap` when strokes merge into each other (for Blocky, nothing overlapped).

**Claude.** *"Make a bold of Blocky about 50 % heavier and save it as Blocky-Bold.ttf."* Claude reads the stem width from `type_glyph_info glyph=I` (110), calls `type_embolden amount=28 preview=true`, looks at the overview, `type_remove_overlap`, `type_set_info style=Bold weight=700` and `type_export`.

## In the window

Open a font in `wetwaresuite-draw` (Fonts tab). The canvas shows the glyph overview; pick a tool on the rail to work on one letter:

- **Edit glyph**: the letter big, with the baseline, x-height, cap height, ascender, descender and the advance width drawn as lines. Type the letter (or a glyph name like `zero`, or `U+00E9`) in the **Glyph** box on the bar. Squares are on-curve points, dots are curve handles. Drag a point to reshape the letter; its handles come along. Drag a handle to bend the curve next to it.
- **Draw glyph**: click corners to draw a new contour into the letter, and click the first corner again to close it. Shift keeps the next edge straight across or up. Tick **Cut a hole** to punch a counter (the hole in an O) instead of adding ink. Esc drops the contour you're drawing.

A letter that doesn't exist yet is made by the first contour you draw. Every change is a `type.draw` command (moves send the whole outline with `mode=replace`), so Ctrl+Z takes it back and it's the same as doing it headless.

## File formats

| format | open | save / export | notes |
| --- | --- | --- | --- |
| `.typeface.json` | yes | `type.save` | this tool's own source format: everything, readable JSON |
| `.ufo` folder | yes (UFO 2 and 3) | `type.save` (UFO 3) | fontinfo, glyphs (.glif), kerning with groups (expanded); for Glyphs, FontForge, RoboFont, fontmake |
| `.ttf` | yes | `type.export` | TrueType outlines, composite glyphs, `kern` and GPOS kerning; hinting is dropped on open and not written |
| `.otf` | yes (CFF outlines) | no | open and export as `.ttf` or `.woff` |
| `.ttc` | yes (the first font) | no | |
| `.woff` | yes (WOFF 1.0) | `type.export` | the TTF, zlib-compressed per table |
| `.svg` | `type.import_svg` (one glyph) | `type.export` (SVG font) | SVG fonts are for tools that still read them (some cutting machines, old toolchains) |
| images | `type.trace_grid`, `type.trace_image` | `type.render`, `type.proof`, `type.grid_template` | PNG, JPEG, BMP, TGA |
| `.pdf` | no | `type.proof` | A4 proof pages with vector outlines |

Exported TTFs contain `head`, `hhea`, `maxp`, `OS/2` (version 4), `name`, `cmap` (format 4, plus format 12 when there are code points above U+FFFF), `loca`, `glyf`, `hmtx`, `post` (format 2, with glyph names), and kerning as a `kern` table and a GPOS pair-adjustment lookup (`kerning=both`, the default; `gpos`, `kern` or `none` to choose). The checksums are filled in; fontTools decompiles every table and `fc-scan` reads the files.

## Command reference

`doc=` is the id of an open font (`font1`). `type.info`, `type.glyph_info`, `type.kern_list`, `type.measure`, `type.preview`, `type.render`, `type.proof` and `type.export` take `font=<file>` instead, for one-shot use. Every edit takes `preview=true`.

### Files and font info

| command | params | what it does |
| --- | --- | --- |
| `type.new` | `family` (Untitled), `style` (Regular), `units_per_em` (1000), `ascender`, `descender`, `x_height`, `cap_height` (0.8, -0.2, 0.5, 0.7 em) | an empty font with `.notdef` and `space` |
| `type.open` | `path` | opens `.typeface.json`, `.ttf`, `.otf`, `.ttc`, `.woff` or a `.ufo` folder; warnings say what was skipped |
| `type.save` | `path`, `format` (auto, json, ufo) | writes the source |
| `type.export` | `path`, `format` (auto, ttf, woff, svg), `kerning` (both, gpos, kern, none), `tolerance` (0.5 units, cubic to quadratic) | writes a font file |
| `type.close` | | closes the font (undo reopens it) |
| `type.info` | `glyphs` (false: one line per glyph) | names, version, license, metrics, counts, missing ASCII |
| `type.set_info` | `family`, `style`, `version`, `copyright`, `license`, `license_url`, `designer`, `manufacturer`, `description`, `vendor` (4 letters), `weight` (100..900) | names and legal info for the `name` and `OS/2` tables |
| `type.set_metrics` | `units_per_em` (with `scale`, true: scales outlines, advances and kerning), `ascender`, `descender`, `x_height`, `cap_height`, `line_gap`, `italic_angle`, `underline_position`, `underline_thickness` | vertical metrics |

### Glyphs

| command | params | what it does |
| --- | --- | --- |
| `type.glyph_add` | `name` (a name or character), `unicode` (A, U+0041, 0x41, comma-separated, `none`), `advance`, a shape (below), `replace` | adds a glyph; without `advance`, the outline's width plus equal bearings |
| `type.draw` | `glyph`, a shape, `mode` (add, replace, cut), `advance` | draws into a glyph (created when missing) |
| `type.glyph_set` | `glyph`, `name`, `unicode` (replaces), `add_unicode`, `advance`, `lsb`, `rsb` | renames (components and kerning follow), re-encodes, respaces |
| `type.glyph_remove` | `glyphs` or `keep` | removes glyphs, or everything except `keep` (subsetting) |
| `type.glyph_copy` | `from`, `to`, `unicode`, `from_doc` (another open font), `component` (false), `replace` | copies a glyph, or makes a component reference to it |
| `type.glyph_info` | `glyph`, `points` (false) | unicodes, advance, bounds, bearings, components, SVG path data, points |
| `type.component_add` | `glyph`, `base`, `dx`, `dy`, `scale`, `scale_x`, `scale_y`, `center` (false) | places a glyph inside another as a reference |
| `type.decompose` | `glyphs` | turns components into plain contours |
| `type.import_svg` | `glyph`, `path` or `svg` (inline text), `fit` (canvas, cap, xheight, ascender, none), `lsb`, `rsb` (6 % of the em, or kept for canvas/none), `advance`, `mode` (replace, add) | an SVG drawing as a glyph |

**Shapes** for `type.glyph_add` and `type.draw` (one or more per call):

| param | example | meaning |
| --- | --- | --- |
| `path` | `"M100 0L300 700L500 0Z"` | SVG path data: M L H V C S Q T A Z, absolute or relative; curves are kept (arcs become cubics) |
| `points` | `0,0,100,0,50,80` | a closed polygon x1,y1,x2,y2,... |
| `curve` | `smooth` | with `points`: a smooth closed curve through them instead of straight edges |
| `rect` | `0,0,110,700` | x,y,width,height |
| `ellipse` | `300,350,250,360` | cx,cy,rx,ry |
| `y_down` | `true` | coordinates are y down from the ascender line |

### Outlines

| command | params | what it does |
| --- | --- | --- |
| `type.transform` | `glyphs`, `dx`, `dy`, `scale`, `scale_x`, `scale_y`, `rotate` (degrees, counter-clockwise), `skew` (degrees, leans right), `origin_x`, `origin_y` (0,0), `center` (false), `keep_advance` (false) | moves, scales, rotates, skews; horizontal scaling scales the advance too |
| `type.oblique` | `angle` (12), `glyphs`, `style` | slants around half the x-height, sets the italic angle |
| `type.embolden` | `amount` (per side), `amount_y` (= amount), `glyphs`, `keep_advance` (false) | heavier (or lighter) strokes |
| `type.remove_overlap` | `glyphs`, `tolerance` (em/1000) | merges overlapping contours (union); untouched contours stay exact |
| `type.fix_directions` | `glyphs` | outer contours clockwise, holes counter-clockwise, by nesting |

### Spacing and kerning

| command | params | what it does |
| --- | --- | --- |
| `type.metrics_auto` | `glyphs`, `spacing` (6 % of the em), `lsb`, `rsb`, `round_factor` (0.8), `sidebearings` (true), `vertical` (true) | even side bearings; reads x-height, cap height, ascender and descender off x, H, d and p |
| `type.kern_set` | `left`, `right`, `value`, or `pairs="A V -80; T o -60"` | sets pairs (0 removes) |
| `type.kern_auto` | `glyphs` (letters and . , - ' "), `strength` (0.8), `target` (the HH gap), `min` (1.5 % of the em), `max` (30 % of the em), `replace` (false) | automatic kerning from glyph profiles; keeps existing pairs |
| `type.kern_list` | `glyph`, `limit` (100) | pairs, strongest first |
| `type.kern_clear` | `glyph` | removes all pairs, or those with one glyph |

### Looking and output

| command | params | what it does |
| --- | --- | --- |
| `type.preview` | `glyph`, or `text`, or neither (the overview); `size`, `points` (true), `metrics` (true), `cell` (96), `columns` (10), `max_glyphs` (160) | a PNG attachment, nothing written |
| `type.render` | `path`, `text` (lines split by `\n`) or `glyph`, `size` (72 px per em), `color` (#000000), `background` (#ffffff, or `transparent`), `max_width` (wrap, px), `padding` (16), `kerning` (true), `tracking` (units), `points` | text or a glyph view as an image |
| `type.proof` | `path` (.png or .pdf), `title`, `sample`, `paragraph`, `sizes` (72,48,36,24,18,14,12), `charset`, `grid`, `kerning` (all true), `width` (1200 px) | a specimen sheet set in the font |
| `type.measure` | `text`, `size` (16), `kerning` (true), `tracking` (0) | width in units and px, missing characters |

### Handwriting

| command | params | what it does |
| --- | --- | --- |
| `type.grid_template` | `path`, `chars` (or uppercase, lowercase, digits, letters, basic, ascii), `cols` (10), `cell` (240 px), `doc` (metrics for the guides) | a sheet to write on |
| `type.trace_grid` | `image`, `chars`, `cols` (10), `rows` (from chars), `margin`, `margin_x`, `margin_y`, `inset` (0.06), `baseline` (fraction from the cell top; from the metrics), plus the tracing params | one glyph per cell |
| `type.trace_image` | `image`, `glyph`, `fit` (cell, cap, xheight, ascender), `baseline`, plus the tracing params | one image into one glyph |

Tracing params: `threshold` (0.5, darkness that counts as ink), `invert` (false: light ink on dark), `tolerance` (1 px, bigger is smoother with fewer points), `min_area` (12 px), `corner` (70 degrees), `spacing` (side bearings, 6 % of the em), `mode` (replace, add).

## MCP

`wetwaresuite mcp --tools type` serves the type tools (`type_new`, `type_draw`, ..., `type_export`) plus `undo`, `redo`, `history` and `documents`. Open fonts are MCP resources too (`wetwaresuite://doc/font1`) with the glyph overview as their preview. Every edit takes `preview=true`, and `type_preview` returns a glyph, a line of text or the overview without writing anything, so Claude can look after each step: points and handles show where a curve kinks, the metric lines show whether a letter sits on the baseline and reaches the x-height.

Example prompts:

- *"Draw a lowercase a for Blocky that matches the o and n."* Claude reads the `o` and `n` with `type_glyph_info` (stems, curves, the x-height overshoot), draws with `type_draw` and `preview=true`, and compares `type_preview text="anoa"`.
- *"The kerning between T and o is too tight."* `type_kern_list glyph=T`, then `type_kern_set left=T right=o value=-120` and `type_preview text="To Tor"`.
- *"Convert every OTF in this folder to WOFF."* For each file: `type_export font=<file>.otf path=<file>.woff`.

## Limitations

- **TrueType out only.** Export writes TrueType (glyf) outlines; CFF/OpenType-PS, CFF2 (variable OTF) and WOFF2 aren't written, and CFF2 and WOFF2 files can't be opened either. Variable fonts open as their default instance.
- **No hinting.** Hinting instructions are dropped on open and none are written; text looks fine on high-resolution screens and in print, softer at small sizes on low-resolution Windows screens.
- **No OpenType layout beyond kerning.** Ligatures, alternates, small caps and mark positioning (GSUB, GPOS mark lookups) are dropped on open and not written. Kerning is pairs only: class kerning from fonts and UFO groups is expanded into glyph pairs (at most 300 000 on open), and the `kern` table holds at most 10 920 pairs (GPOS holds them all; some old apps only read `kern`).
- **Components.** Point-matched component placement (TrueType's anchor-point form) is read as placed at the origin. Exported components are offsets plus scale/2x2 transforms.
- **Overlap removal** rebuilds only the contours that cross other contours: they are flattened, merged and refitted with curves within `tolerance`, so their points are new (and usually more of them) rather than the ones you drew. Contours that don't cross keep their points exactly.
- **Auto-kerning is lite.** It compares the glyphs' side profiles in horizontal bands, without knowing about optical rules for specific shapes; treat its result as a first pass and check it on a proof. Auto-spacing gives every glyph the same bearings (round shapes a little less), no more.
- **Tracing needs clean scans:** dark ink on light paper, one letter per cell, no letter crossing into a neighbouring cell. Light guide lines and the template's labels are ignored; faint ink needs a lower `threshold=`, shadows and coloured paper a higher one, and photos taken at an angle need straightening first (the [photo](photo.md) tool can crop and rotate). `type.trace_image fit=cap`, `xheight` and `ascender` put the lowest ink on the baseline, so descenders need moving down afterwards.
- **The glyph editor is simple.** It moves points and draws straight-edged contours; curves come from moving the handles of existing points, from SVG, from tracing or from `type.draw curve=smooth`. There's no point adding or deleting in the window yet (`type.draw mode=replace path=...` does anything).
