# Board: whiteboards and diagrams

`board` is a whiteboard and diagram tool that runs without a screen. A board is an infinite canvas holding shapes (rectangles, rounded boxes, ellipses, diamonds, cylinders, clouds, hexagons and more), sticky notes, text, images, frames and groups, plus **connectors that stay attached** to the shapes they join and **freehand ink** with pen pressure. Everything is drawn in layers with a theme, including a hand-drawn "sketchy" look like Excalidraw. You can type a whole diagram as **Mermaid** (flowcharts, sequence diagrams, mind maps) or as an **indented outline**, and the board lays it out for you (layered/Sugiyama, trees, mind maps, radial, force-directed, grids). Boards save as `.board.json`, open and save Excalidraw `.excalidraw` and draw.io `.drawio` files, and export to PNG, SVG and PDF.

Each task below is shown three ways:

- **CLI**: `wetwaresuite <command> key=value` runs one command in a fresh process. A board only lives as long as the process, so one-shot commands are for opening and inspecting files. For edits, pipe a few lines into `wetwaresuite batch` from the shell.
- **Batch**: a script file run with `wetwaresuite batch file.txt`. `-> name` at the end of a line saves the result (the board id) and `$name` reuses it; `#` starts a comment; `undo` and `redo` work as lines (see [headless and batch](headless-and-batch.md)).
- **Claude**: a prompt for Claude over MCP and the tools it calls (see [mcp.md](mcp.md)). `wetwaresuite mcp --tools board` serves just this tool.

Every example was run against the real binary, in an empty folder holding only the input files shown. Outputs are copied from those runs; long ones are shortened where marked with `...`, and the `key: value` data lines that follow each message are left out unless they matter.

## Contents

1. [The ideas you need](#1-the-ideas-you-need)
2. [Task: a flowchart from Mermaid](#2-task-a-flowchart-from-mermaid)
3. [Task: a hand-drawn architecture sketch, shape by shape](#3-task-a-hand-drawn-architecture-sketch-shape-by-shape)
4. [Task: a sequence diagram](#4-task-a-sequence-diagram)
5. [Task: a mind map from an outline](#5-task-a-mind-map-from-an-outline)
6. [Task: a retro wall of sticky notes](#6-task-a-retro-wall-of-sticky-notes)
7. [Task: Excalidraw and draw.io in and out](#7-task-excalidraw-and-drawio-in-and-out)
8. [Shapes, connectors and styles](#8-shapes-connectors-and-styles)
9. [Auto-layout](#9-auto-layout)
10. [What Mermaid syntax is understood](#10-what-mermaid-syntax-is-understood)
11. [File formats](#11-file-formats)
12. [Command reference](#12-command-reference)
13. [Limitations](#13-limitations)
14. [In the window](#14-in-the-window)

## 1. The ideas you need

**The canvas.** Coordinates are canvas units (one unit is one pixel at `scale=1`), x to the right and y down, with no edges: shapes can sit at negative positions. Exports and previews frame whatever is on the board plus a margin (`padding=32`), or a `region=x,y,w,h` you give, or just some elements (`only=e12`, handy for one frame).

**Elements and ids.** Every element gets an id, `e1`, `e2`, ... in the order they're made. Give one a `name=` and commands accept the name wherever they take an id (`ids=api`, `from=web`). Diagrams made from Mermaid name each node after its Mermaid id, so after `A --> B` you can write `board.set ids=B fill=#b2f2bb`. Commands that take several elements use comma lists: `ids=e1,e2,api`, or `ids=all`.

**Shapes size themselves.** Leave out `w` and `h` and a shape grows to fit its text (sticky notes stay about square and wrap). Leave out `x` and `y` and it goes to the right of everything; `near=api side=below gap=80` puts it next to another element.

**Connectors stay attached.** `board.connect from=a to=b` stores which shapes it joins, not coordinates, so moving, resizing or laying out shapes redraws the line. A connector can also have free ends (`start=x,y`, `end=x,y`). Routes: `straight`, `elbow` (right angles, found by a search that goes around the other shapes) and `curved`.

**Frames and groups.** A frame is a titled box that holds other elements (like a Miro frame or a Mermaid subgraph); a group is invisible. Moving either moves what's inside; frames are always drawn underneath their layer's other elements. Make them with `board.group kind=frame|group`, or `parent=` on `board.add`.

**Styles and themes.** Each element can have its own fill, stroke, dash, opacity, font and so on (the same params on `board.add`, `board.set` and `board.connect`); anything it doesn't set comes from the board's theme: `default`, `sketchy`, `dark`, `blueprint`, `pastel` or `mono`. `sketch=true` and `fill_style=hachure` give one element the hand-drawn look on any theme.

**Layers.** Layers draw bottom first. Hidden layers aren't drawn or exported; locked layers refuse edits. Elements go on a layer with `layer=`.

**Undo and previews.** Every edit is one undo step (`undo` in a batch script or the `undo` MCP tool), and an edit that fails changes nothing. Every edit takes `preview=true`, which attaches a PNG of the board to the result so Claude sees what it did; `board.preview` renders without changing anything. Even making a board from Mermaid is undoable: undo closes the new board.

## 2. Task: a flowchart from Mermaid

The goal: an order-handling flowchart from a few lines of Mermaid, with one box relabelled and a sticky note added, exported as a high-resolution PNG and a PDF, and handed to a colleague as a draw.io file.

`flow.mmd`:

```text
flowchart TD
    A([Start]) --> B[Read order]
    B --> C{In stock?}
    C -->|yes| D[Pack items]
    C -->|no| E[Back-order]
    E -.-> B
    D --> F[(Orders DB)]
    D ==> G((Ship))
    subgraph Warehouse
      D
      F
    end
    classDef hot fill:#ffe3e3,stroke:#c92a2a
    class E hot
```

### Batch script

```text
# flowchart.txt: an order flow from Mermaid, touched up and exported
board.from_mermaid path=flow.mmd title="Order flow" -> f
board.set doc=$f ids=E text="Back-order from supplier" fill=#ffe8cc
board.add doc=$f type=note text="Stock is synced every hour" near=B side=left gap=80
board.find doc=$f text=order
board.export doc=$f path=out/flow.png scale=2
board.export doc=$f path=out/flow.pdf
board.save doc=$f path=out/flow.board.json
board.save doc=$f path=out/flow.drawio
```

```sh
$ wetwaresuite batch flowchart.txt
made a flowchart with 7 nodes, 7 connectors, 1 frame on board1
...
changed e8 rect 'E' "Back-order from supplier" at 255,430 195x50
...
added e16 note "Stock is synced every hour" at -39,105 160x160
...
3 matches
  e2 rect 'B' "Read order" at 201,160 120x50
  e8 rect 'E' "Back-order from supplier" at 255,430 195x50
  e11 cylinder 'F' "Orders DB" at 0,583 120x54 in e15
...
exported out/flow.png
doc: board1
path: out/flow.png
width: 1146
height: 1388
exported out/flow.pdf
...
saved out/flow.board.json
saved out/flow.drawio
```

What happened:

- The Mermaid shapes became board shapes: `([ ])` a stadium, `{ }` a diamond, `[( )]` a cylinder, `(( ))` a circle. `-->|yes|` became a labelled connector, `-.->` a dashed one, `==>` a thick one. The `subgraph` became a frame named `Warehouse` holding `D` and `F`, and `classDef`/`class` coloured `E`.
- The layered layout put the nodes in ranks from top to bottom. Forward links leave from the bottom of a box and arrive at the top; the loop back from `E` to `B` goes round the side so it doesn't run over the others. A node outside the subgraph that the frame would cover (`Ship`) is moved out of the frame's way.
- Changing the text of `E` grew the box to fit. Its connectors followed.
- `scale=2` doubled the pixels of the PNG. The PDF is vector, one page sized to the drawing, with real text you can select.

### CLI

Every edit needs the board to stay alive between commands, so pipe the lines into `wetwaresuite batch`:

```sh
$ printf 'board.from_mermaid path=flow.mmd -> f\nboard.export doc=$f path=out/flow.svg\n' | wetwaresuite batch
made a flowchart with 7 nodes, 7 connectors, 1 frame on board1
board1
doc: board1
kind: flowchart
nodes: 7
edges: 7
frames: 1
ids: e1,e2,e3,e4,e5,e6,e7,e8,e9,e10,e11,e12,e13,e14,e15
exported out/flow.svg
doc: board1
path: out/flow.svg
width: 483
height: 694
```

`text=` takes the Mermaid inline instead of a file. In a shell, write the line breaks as `\n`: `board.from_mermaid text="graph LR\nA --> B\nB --> C"`.

### Claude

> Draw me a flowchart of our order handling: start, read the order, check stock; if it's in stock pack it (store in the orders DB) and ship it, otherwise back-order it and re-read. Put packing and the DB in a "Warehouse" box. Then export it as PNG and draw.io into ~/docs.

Claude writes the Mermaid itself and calls `board_from_mermaid` with `text` and `preview: true`, looks at the attached picture, adjusts with `board_set` (`ids: "E"`, `text`, `fill`) and `board_add` (`type: "note"`, `near: "B"`), then `board_export` (`path: ".../flow.png", scale: 2`) and `board_save` (`path: ".../flow.drawio"`).

## 3. Task: a hand-drawn architecture sketch, shape by shape

The goal: a whiteboard-style sketch of a checkout system built shape by shape, with straight, elbow and curved connectors, a frame around the backend, a sticky note and a pen mark, in the sketchy theme.

### Batch script

```text
# system.txt: a hand-drawn architecture sketch
board.new title="Checkout service" theme=sketchy -> b
board.add doc=$b type=rounded text="Web shop" name=web x=0 y=0
board.add doc=$b type=rect text="API gateway" name=api near=web side=right gap=120
board.add doc=$b type=hexagon text="Payments" name=pay near=api side=right gap=120
board.add doc=$b type=cylinder text="Orders DB" name=db near=api side=below gap=140
board.add doc=$b type=cloud text="Card network" name=cards near=pay side=below gap=120
board.add doc=$b type=rect text="Cache" name=cache x=240 y=-150
board.connect doc=$b from=web to=api label=HTTPS end_arrow=triangle
board.connect doc=$b from=api to=pay end_arrow=triangle
board.connect doc=$b from=api to=db route=elbow end_arrow=triangle start_arrow=circle
board.connect doc=$b from=pay to=cards route=curved dash=dashed label=async
board.connect doc=$b from=web to=db route=elbow label="nightly export" stroke=#e03131
board.connect doc=$b from=api to=cache route=elbow end_arrow=arrow dash=dotted
board.group doc=$b ids=api,pay,db,cache kind=frame label="Backend (eu-west)" name=backend
board.add doc=$b type=note text="Retry payments 3x before failing the order" near=cards side=right gap=60 fill=#ffec99
board.ink doc=$b points=640,170,660,150,700,140,740,150,750,175 pressures=0.3,0.7,1,0.8,0.4 width=5 color=#e03131
board.info doc=$b
board.export doc=$b path=out/system.png scale=2
board.export doc=$b path=out/system.svg
board.export doc=$b path=out/system.pdf
board.save doc=$b path=out/system.board.json
board.save doc=$b path=out/system.excalidraw
board.save doc=$b path=out/system.drawio
```

```sh
$ wetwaresuite batch system.txt
created board1: "Checkout service": 0 shapes, 0 connectors, 0 ink strokes, 1 layer, theme sketchy
...
added e1 rounded 'web' "Web shop" at 0,0 120x60
...
added e2 rect 'api' "API gateway" at 240,0 121x60
...
connected e9 connector e2 -> e4 elbow
...
framed 4 elements as e13
...
drew ink e15 with 20 points
...
"Checkout service": 8 shapes, 6 connectors, 1 ink stroke, 1 layer, theme sketchy
layers:
  default (15 elements)
elements:
  e13 frame 'backend' "Backend (eu-west)" at 216,-205 419x489
  e1 rounded 'web' "Web shop" at 0,0 120x60
  e2 rect 'api' "API gateway" at 240,0 121x60 in e13
  e3 hexagon 'pay' "Payments" at 481,0 130x60 in e13
  e4 cylinder 'db' "Orders DB" at 241,200 120x60 in e13
  e5 cloud 'cards' "Card network" at 462,180 168x100
  e6 rect 'cache' "Cache" at 240,-150 120x60 in e13
  e7 connector "HTTPS" e1 -> e2 straight
  e8 connector e2 -> e3 straight
  e9 connector e2 -> e4 elbow
  e10 connector "async" e3 -> e5 curved
  e11 connector "nightly export" e1 -> e4 elbow
  e12 connector e2 -> e6 elbow
  e14 note "Retry payments 3x before failing the ..." at 690,150 160x160
  e15 ink at 638,149 115x28
...
exported out/system.png
exported out/system.svg
exported out/system.pdf
saved out/system.board.json
saved out/system.excalidraw
saved out/system.drawio
```

Things to notice:

- `near=` and `side=` placed each box relative to the last, centred on it, without working out coordinates.
- The sketchy theme draws every outline twice with a wobble and overshooting corners, and fills with diagonal hachure lines. The wobble is seeded from each element's id, so the same board always draws the same way (diffs and tests stay stable).
- The elbow connector from `web` to `db` goes around `api` rather than through it. `start_arrow=circle` puts a dot where the API connector starts.
- `board.group kind=frame` sized the frame around its four members and put it underneath them; `ids=backend` now moves the whole backend at once: `board.set doc=$b ids=backend dx=40`.
- The ink stroke is smoothed (`smoothing=0.5` by default) and thicker where the pressure is higher.

### CLI

```sh
$ printf 'board.new theme=sketchy -> b\nboard.add doc=$b text=Client name=c\nboard.add doc=$b text=Server near=c\nboard.connect doc=$b from=c to=s label=HTTP\n' | wetwaresuite batch
```

fails on the last line with `no element 's'` because the server box has no name. Names make scripts readable; ids (`e2`) always work.

### Claude

> Sketch our checkout architecture in the hand-drawn style: web shop → API gateway → payments → card network (async), the gateway also talks to an orders DB and a cache. Box the backend parts as "Backend (eu-west)" and add a sticky note that payments retry 3 times.

Claude calls `board_new` (`theme: "sketchy"`), `board_add` for each shape with `name` and `near`, `board_connect` with `route` and `end_arrow`, `board_group` (`kind: "frame"`), `board_add` (`type: "note"`) and finally `board_preview` to check, then `board_export`.

## 4. Task: a sequence diagram

`seq.mmd`:

```text
sequenceDiagram
    autonumber
    actor U as User
    participant W as Web app
    participant A as Auth API
    U->>W: Open login page
    W->>A: POST /login
    alt valid password
        A-->>W: 200 + token
        W-->>U: Dashboard
    else wrong password
        A-->>W: 401
        W-->>U: Error message
    end
    Note over W,A: Tokens expire after 1 hour
    W->>W: cache token
```

### Batch script

```text
# sequence.txt
board.from_mermaid path=seq.mmd title=Login -> s
board.info doc=$s limit=8
board.export doc=$s path=out/login.png
board.export doc=$s path=out/login.svg
```

```sh
$ wetwaresuite batch sequence.txt
made a sequence with 4 nodes, 7 messages, 1 frame on board1
...
"Login": 16 shapes, 11 connectors, 0 ink strokes, 1 layer, theme default
content: -14,0 458x684
layers:
  default (27 elements)
elements:
  e27 frame "alt  [valid password]" at -14,178 458x272
  e26 connector 'A_lifeline' e3 -> e25 straight
  e24 connector 'W_lifeline' e2 -> e23 straight
  e22 connector 'U_lifeline' e1 -> e21 straight
  e1 stadium 'U' "User" at 0,0 110x46
  e2 rect 'W' "Web app" at 160,0 110x46
  e3 rect 'A' "Auth API" at 320,0 110x46
  e4 connector (55,108) -> (215,108) straight
  ... 19 more
...
exported out/login.png
exported out/login.svg
```

Participants are boxes at the top and bottom joined by dashed lifelines (named `<id>_lifeline`; the bottom boxes are `<id>_end`). Messages are connectors with free ends at fixed heights: `->>` solid with a filled head, `-->>` dashed, `-)` an open head, `-x` a cross, and a message to itself loops out to the right. `autonumber` numbers the labels. `alt`/`else`, `loop`, `opt`, `par`, `critical` and `break` become frames with a dashed divider per `else`. `Note over W,A` is a sticky note spanning both lifelines. Columns are spaced so the longest label between two participants fits.

The layout of a sequence diagram is fixed by the order of the messages, so `board.layout` isn't meant for it; move single elements with `board.set`.

### Claude

> Make a sequence diagram of our login: the user opens the login page, the web app posts to the auth API, which answers 200 with a token or 401; show both cases and note that tokens expire after an hour.

Claude calls `board_from_mermaid` with a `sequenceDiagram` text and `preview: true`, then `board_export`.

## 5. Task: a mind map from an outline

Any indented list works: Markdown bullets (`-`, `*`, `+`, `1.`), task boxes, `#` headings, or plain indented lines. `plan.md`:

```text
# Launch plan
## Product
- Pricing page
- Onboarding emails
  - Welcome
  - Day 3 tips
## Marketing
- Blog post
- Social
  - Twitter
  - LinkedIn
## Support
- FAQ
- Help desk rota
```

### Batch script

```text
# mindmap.txt
board.from_outline path=plan.md -> m
board.export doc=$m path=out/plan.png
board.layout doc=$m algorithm=force seed=3
board.export doc=$m path=out/plan-force.png
board.layout doc=$m algorithm=tree direction=TB route=elbow
board.export doc=$m path=out/plan-tree.png
```

```sh
$ wetwaresuite batch mindmap.txt
made a mind map from the outline with 14 nodes, 13 connectors on board1
board1
...
exported out/plan.png
laid out 14 shapes with force
exported out/plan-force.png
laid out 14 shapes with tree, 13 connectors now elbow
exported out/plan-tree.png
```

The first heading is the centre (an ellipse); each `##` branch gets its own colour from the theme's palette, carried down to its leaves, and connectors are curved. The default `mindmap` layout balances branches on both sides of the root. `force` gives an organic spread (try a few `seed=` values), `radial` puts each level on a ring, and `tree` with `direction=TB` and `route=elbow` turns the same content into an org chart. When the outline has several top-level items, `root="Topic"` names the centre (default "Mind map"). `colors=false` keeps the theme's single colour. Mermaid's own `mindmap` syntax (indented, with `((round))`, `[square]`, `{{hex}}`, `)cloud(` node shapes) goes through `board.from_mermaid` and gives the same kind of board.

### CLI

```sh
$ printf 'board.from_outline text="- Trip\\n  - Packing\\n  - Route" -> m\nboard.export doc=$m path=out/trip.png\n' | wetwaresuite batch
made a mind map from the outline with 3 nodes, 2 connectors on board1
...
```

### Claude

> Here are my notes for the launch (pasted outline). Turn them into a mind map, dark theme, and give me a PNG.

Claude calls `board_from_outline` with `text`, `theme: "dark"` and `preview: true`, perhaps `board_layout` with `algorithm: "force"` if you ask for something less regular, then `board_export`.

## 6. Task: a retro wall of sticky notes

The goal: three columns of sticky notes, lined up and evenly spaced, two notes linked to actions, and the facilitator's pen marks on their own layer that can be hidden for the final export.

### Batch script

```text
# retro.txt: a sprint retro wall of sticky notes
board.new title="Sprint 14 retro" theme=pastel -> r
board.add doc=$r type=text text="Went well" font_size=28 bold=true x=0 y=0 name=h1
board.add doc=$r type=text text="To improve" font_size=28 bold=true x=420 y=0 name=h2
board.add doc=$r type=text text="Actions" font_size=28 bold=true x=840 y=0 name=h3
board.add doc=$r type=note text="Release went out on time" x=0 y=70 name=w1
board.add doc=$r type=note text="Pairing on the parser" x=180 y=60 fill=#d3f9d8 name=w2
board.add doc=$r type=note text="Flaky CI on Windows" x=420 y=80 fill=#ffc9c9 name=i1
board.add doc=$r type=note text="Too many meetings" x=600 y=65 fill=#ffc9c9 name=i2
board.add doc=$r type=note text="Long review queue" x=430 y=260 fill=#ffc9c9 name=i3
board.add doc=$r type=note text="Fix the CI cache (Ana)" x=840 y=70 fill=#d0ebff name=a1
board.add doc=$r type=note text="No-meeting Wednesdays" x=850 y=250 fill=#d0ebff name=a2
board.align doc=$r ids=w1,w2,i1,i2,a1 edge=top
board.distribute doc=$r ids=w1,w2 axis=horizontal gap=20
board.distribute doc=$r ids=i1,i2 axis=horizontal gap=20
board.set doc=$r ids=i3 x=420 y=260
board.align doc=$r ids=a1,a2 edge=left
board.connect doc=$r from=i1 to=a1 route=elbow dash=dashed end_arrow=arrow
board.connect doc=$r from=i2 to=a2 route=curved dash=dashed end_arrow=arrow
board.layer doc=$r op=add name=facilitator
board.ink doc=$r layer=facilitator points=410,60,470,52,560,50,650,55,760,60 width=6 color=#e8590c pressures=0.4,0.9,1,0.9,0.3
board.add doc=$r layer=facilitator type=text text="vote: top issue" x=640 y=250 text_color=#e8590c
board.find doc=$r text=meeting
board.export doc=$r path=out/retro.png
board.layer doc=$r op=set name=facilitator visible=false
board.export doc=$r path=out/retro-clean.png
```

```sh
$ wetwaresuite batch retro.txt
created board1: "Sprint 14 retro": 0 shapes, 0 connectors, 0 ink strokes, 1 layer, theme pastel
...
added e4 note 'w1' "Release went out on time" at 0,70 160x160
...
aligned 5 elements top
distributed 2 elements horizontally with gap 20
distributed 2 elements horizontally with gap 20
...
added layer facilitator
drew ink e13 with 20 points
added e14 text "vote: top issue" at 640,250 104x20 [facilitator]
2 matches
  e7 note 'i2' "Too many meetings" at 600,60 160x160
  e10 note 'a2' "No-meeting Wednesdays" at 840,250 160x160
...
exported out/retro.png
changed layer facilitator
exported out/retro-clean.png
```

`board.align` lines up edges (`left`, `center`, `right`, `top`, `middle`, `bottom`) against the selection's bounds, or against one element with `to=`. `board.distribute` spaces elements evenly between the outermost two, or packs them with a fixed `gap=`. The elbow connector from `i1` threads above its neighbour instead of crossing it. `retro-clean.png` leaves the hidden `facilitator` layer out; `layers=default,facilitator` on `board.export` picks layers explicitly whatever their visibility.

### Claude

> Set up a retro board with "Went well", "To improve" and "Actions" columns and put these notes on it: (list). Link the CI note to the CI action. Circle the top issue on a separate layer.

Claude calls `board_new`, `board_add` per heading and note, `board_align`/`board_distribute`, `board_connect`, `board_layer` (`op: "add"`) and `board_ink` with `layer`, then `board_preview`.

## 7. Task: Excalidraw and draw.io in and out

The goal: take the Excalidraw file from task 3, give it a clean look, relabel the cache, bring the flowchart from task 2 onto the same board, and save it for both Excalidraw and draw.io users.

### CLI: look inside a file

```sh
$ wetwaresuite board.open path=out/system.excalidraw
opened out/system.excalidraw as board1: "Checkout service": 8 shapes, 6 connectors, 1 ink stroke, 1 layer, theme sketchy, out/system.excalidraw
board1
doc: board1
elements: 15
theme: sketchy
$ wetwaresuite board.open path=out/system.drawio
opened out/system.drawio as board1: "Checkout service": 8 shapes, 7 connectors, 0 ink strokes, 1 layer, theme sketchy, out/system.drawio
...
```

(draw.io has no freehand strokes, so the ink came back as a curved line: 7 connectors.)

### Batch script

```text
# interop.txt: open an Excalidraw scene, change it, hand it on as draw.io
board.open path=out/system.excalidraw -> x
board.theme doc=$x theme=default clear_styles=true
board.set doc=$x ids=cache text="Redis cache" fill=#ffd8a8
board.import doc=$x path=flow.mmd
board.info doc=$x limit=3
board.save doc=$x path=out/system-v2.drawio
board.save doc=$x path=out/system-v2.excalidraw
board.export doc=$x path=out/system-v2.png
```

```sh
$ wetwaresuite batch interop.txt
opened out/system.excalidraw as board1: "Checkout service": 8 shapes, 6 connectors, 1 ink stroke, 1 layer, theme sketchy, out/system.excalidraw
board1
theme is now default (14 elements back to theme colours)
changed e7 rect 'cache' "Redis cache" at 240,-150 120x60 in e1
imported 15 elements from flow.mmd
"Checkout service": 16 shapes, 13 connectors, 1 ink stroke, 1 layer, theme default, out/system.excalidraw
layers:
  default (30 elements)
elements:
  e1 frame "Backend (eu-west)" at 216,-205 419x489
  e2 rounded 'web' "Web shop" at 0,0 120x60
  e3 rect 'api' "API gateway" at 240,0 121x60 in e1
  ... 27 more
saved out/system-v2.drawio
saved out/system-v2.excalidraw
exported out/system-v2.png
```

An Excalidraw scene stores colours and roughness on every element, so after opening one the colours belong to the elements, not the theme; `clear_styles=true` hands them back to the theme (it also clears the ink's colour, so do it before your own restyling). Ids are renumbered on open, but the names written by `board.save` to `.excalidraw` (`web`, `api`, `cache`, ...) come back. `board.import` merged another file (a `.board.json`, `.excalidraw`, `.drawio`, a Mermaid `.mmd` or an outline `.md`/`.txt`) to the right of what was there.

### Claude

> Open ~/Downloads/architecture.excalidraw, rename "Cache" to "Redis cache", tidy it into the clean theme and save a draw.io copy next to it.

Claude calls `board_open`, `board_info` (to find the ids), `board_theme` (`clear_styles: true`), `board_set`, `board_save` with a `.drawio` path.

## 8. Shapes, connectors and styles

### Shape types (`board.add type=`)

| type | looks like | notes |
| --- | --- | --- |
| `rect` | rectangle | the default |
| `rounded` | rounded rectangle | `radius=` (default 12) |
| `stadium` | pill | Mermaid `([ ])`; actors in sequence diagrams |
| `ellipse`, `circle` | ellipse / circle | `circle` keeps width = height |
| `diamond` | decision rhombus | |
| `hexagon`, `parallelogram`, `trapezoid`, `triangle` | flowchart shapes | |
| `cylinder` | database drum | aliases `database`, `db` |
| `cloud` | cloud | |
| `note` | sticky note with a folded corner and a soft shadow | alias `sticky`; theme note colour |
| `subroutine` | box with double sides | Mermaid `[[ ]]` |
| `text` | free text, no box | sizes to the text; `w=` wraps it |
| `image` | a picture | `src=` file path or `data:` URL; size from the image (max 400) |
| `frame` | titled area | holds elements (`parent=`); drawn under the rest |

Groups (`board.group kind=group`), connectors (`board.connect`) and ink (`board.ink`) have their own commands.

### Connectors

| param | values | default |
| --- | --- | --- |
| `from`, `to` | shape ids or names (comma lists connect every pair) | |
| `start`, `end` | `x,y` free end instead | |
| `route` | `straight`, `elbow`, `curved` | `straight` |
| `end_arrow`, `start_arrow` | `none`, `arrow` (open V), `triangle` (filled), `diamond`, `circle`, `bar`, `cross` | `arrow` / `none` |
| `from_anchor`, `to_anchor` | `auto`, `top`, `right`, `bottom`, `left`, `center` | `auto` |
| `waypoints` | `x1,y1,x2,y2,...` points a straight or curved line passes through | |
| `label` | text at the middle, on a background patch | |

A straight connector aims at the other shape's centre and stops at the outline (diamonds, ellipses and clouds included). Elbow and curved connectors leave through a side: `auto` picks the side facing the other shape. Elbow routes are searched on a grid around the other shapes with a penalty per bend, so they take few turns and keep clear of boxes in the way. A connector from a shape to itself makes a loop on its top-right corner.

### Style params (on `board.add`, `board.set`, `board.connect`)

| param | meaning |
| --- | --- |
| `fill`, `stroke`, `text_color` | colours: `#rgb`, `#rrggbb`, `#rrggbbaa`, `rgb(...)`, CSS names, or `none`; `theme` resets to the theme's |
| `stroke_width` | outline / line width |
| `dash` | `solid`, `dashed`, `dotted` |
| `opacity` | 0..1 for the whole element |
| `font_size`, `font` (`sans`, `serif`, `mono`), `bold`, `italic` | label text |
| `align` (`left`, `center`, `right`), `valign` (`top`, `middle`, `bottom`) | label position |
| `fill_style` | `solid`, `hachure` (diagonal lines), `cross` (cross-hatch), `none` |
| `sketch`, `roughness` | hand-drawn strokes, wobble 0..3 |
| `radius` | corner radius of rounded shapes |
| `shadow` | drop shadow |

`board.set clear_style=true` drops all of an element's own style before applying new values.

### Themes (`board.new theme=`, `board.theme`)

| theme | look |
| --- | --- |
| `default` | white canvas, blue outlines on pale blue, dashed grey frames |
| `sketchy` | hand-drawn: wobbly double strokes, blue hachure fills, like Excalidraw |
| `dark` | dark canvas, light lines, pastel branch colours |
| `blueprint` | white lines on blueprint blue, monospace text |
| `pastel` | soft purple on cream |
| `mono` | black on white, serif text, for print |

## 9. Auto-layout

`board.layout` moves shapes using the connectors between them; connectors follow and frames refit around their contents.

| algorithm | what it does | good for |
| --- | --- | --- |
| `layered` | Sugiyama: cycles broken, ranks by longest path, crossings reduced by barycentre sweeps, nodes pulled towards their neighbours; `direction=TB/BT/LR/RL` | flowcharts, pipelines, dependency graphs |
| `tree` | each subtree gets the width its leaves need, parents centred over children | org charts, file trees |
| `mindmap` | root in the middle, branches alternating right and left by size | mind maps |
| `radial` | levels on rings around the root, wedges sized by leaves | big mind maps, taxonomies |
| `force` | force-directed (springs and repulsion, `seed=`, `iterations=`) then overlaps removed | networks, organic mind maps |
| `grid` | rows and columns in reading order (`columns=`); no connectors needed | tidying sticky notes, galleries |

`gap_x` is the space between neighbours and `gap_y` between ranks. `ids=` lays out only some shapes; without it every connected shape is laid out (or all shapes when nothing is connected). Shapes in a group move with the group; shapes on locked layers stay put. `route=elbow` also switches every connector's route, which suits trees. The result keeps its top-left corner where the shapes were, unless `x=`/`y=` say otherwise.

## 10. What Mermaid syntax is understood

| diagram | supported |
| --- | --- |
| `flowchart` / `graph` with `TB`, `TD`, `BT`, `LR`, `RL` | nodes `id`, `id[text]`, `(text)`, `([text])`, `[[text]]`, `[(text)]`, `((text))`, `(((text)))`, `{text}`, `{{text}}`, `[/text/]`, `[\text\]`, `[/text\]`, `[\text/]`, `>text]`; quoted labels with `<br>` and `#35;` codes; links `-->`, `---`, `-.->`, `-.-`, `==>`, `===`, `--o`, `--x`, `<-->`, longer `--->`; labels `-->\|text\|`, `-- text -->`, `-. text .->`, `== text ==>`; chains `A --> B --> C` and `A & B --> C & D`; `subgraph id [title]` ... `end` (nested) as frames; `classDef`, `class`, `:::class`, `style` (fill, stroke, stroke-width, color, stroke-dasharray, font-weight, font-size); `%%` comments; `;` separators |
| `sequenceDiagram` | `participant X as Name`, `actor`, messages `->`, `-->`, `->>`, `-->>`, `-x`, `--x`, `-)`, `--)`, `<<->>`, activation `+`/`-` (ignored), `Note left of / right of / over A,B`, `loop`, `alt`/`else`, `opt`, `par`/`and`, `critical`/`option`, `break`, `rect`, `autonumber`, `title` |
| `mindmap` | indentation, node shapes `((round))`, `(rounded)`, `[square]`, `{{hex}}`, `)cloud(`, `))bang((`; `::icon()` and `:::class` are dropped |

A ```` ```mermaid ```` fence and `---` front matter around the text are skipped. Lines it can't read are skipped with a `warning:` line in the result; text without a header is read as a flowchart. Mermaid ids become element names.

## 11. File formats

| extension | read | write | notes |
| --- | --- | --- | --- |
| `.board.json` (also `.json`) | yes | yes | native: everything, including names, layers, styles, anchors and ink pressure |
| `.excalidraw` | yes | yes | rectangles, ellipses, diamonds, text (free and bound to shapes), arrows and lines with bindings, elbow and curved arrows, freehand with pressures, images (embedded files), frames, groups, roughness, hachure/cross-hatch, dashes, opacity. Shapes Excalidraw lacks (cylinder, cloud, hexagon, note, ...) are saved as rectangles/ellipses with the real type in `customData`, so they come back intact |
| `.drawio`, `.dio`, `.xml` | yes (first page; compressed or plain) | yes (plain XML) | vertices with the matching draw.io shapes (rhombus, cylinder3, cloud, hexagon, parallelogram, trapezoid, note, process, text, image, swimlane/containers, groups), edges with source/target, exit/entry anchors, waypoints, orthogonal/curved styles and arrowheads, labels (HTML stripped), layers, `sketch=1` |
| `.svg` | no | yes | vector; text stays text; one `<g id="eN">` per element; images embedded |
| `.pdf` | no | yes | vector, one page sized to the content (1 unit = 1 pt at `scale=1`), standard fonts |
| `.png`, `.jpg`, `.bmp`, `.tga`, `.ppm` | no | yes | rendered at `scale`; `transparent=true` for PNG without the background |

`board.save` picks the format from the extension; `board.export` does the same and adds `scale`, `padding`, `region`, `only`, `layers` and `transparent`.

## 12. Command reference

| command | what it does | key params |
| --- | --- | --- |
| `board.new` | new empty board | `title`, `theme`, `background` |
| `board.open` | open `.board.json`, `.excalidraw`, `.drawio` | `path` |
| `board.save` | save by extension | `path`, `format` |
| `board.export` | picture or file of the board or part of it | `path`, `format`, `scale`, `padding`, `region`, `only`, `layers`, `transparent` |
| `board.import` | merge another file into this board | `path`, `dx`, `dy` |
| `board.add` | add a shape | `type`, `text`, `x`, `y`, `w`, `h`, `near`, `side`, `gap`, `name`, `layer`, `parent`, `src`, styles |
| `board.set` | move, resize, relabel, restyle, re-route | `ids`, `x`, `y`, `dx`, `dy`, `w`, `h`, `fit`, `text`, `type`, `layer`, `parent`, `from`, `to`, `route`, arrows, anchors, styles |
| `board.remove` | delete elements (frames with contents, attached connectors too) | `ids` |
| `board.order` | stacking order | `ids`, `to` (front, back, forward, backward) |
| `board.duplicate` | copy elements | `ids`, `dx`, `dy` |
| `board.connect` | attached connector | `from`, `to`, `start`, `end`, `route`, `label`, arrows, anchors, `waypoints`, styles |
| `board.ink` | freehand stroke | `points`, `pressures`, `width`, `color`, `smoothing` |
| `board.group` / `board.ungroup` | groups and frames | `ids`, `kind`, `label`, `padding` / `keep_frame` |
| `board.align` | line up edges or centres | `ids`, `edge`, `to` |
| `board.distribute` | even spacing | `ids`, `axis`, `gap` |
| `board.layout` | automatic layout | `algorithm`, `direction`, `ids`, `gap_x`, `gap_y`, `columns`, `seed`, `iterations`, `route`, `x`, `y` |
| `board.layer` | add, change, move, remove layers | `op`, `name`, `new_name`, `visible`, `locked`, `opacity`, `index`, `delete_elements` |
| `board.theme` | switch theme | `theme`, `background`, `clear_styles` |
| `board.from_mermaid` | Mermaid to diagram (new board or `doc=`) | `text` or `path`, `theme`, `title`, `direction`, `route`, `layout`, `x`, `y`, `layer` |
| `board.from_outline` | indented list to mind map | `text` or `path`, `root`, `layout`, `route`, `colors`, ... |
| `board.find` | search | `text`, `type`, `layer`, `connected`, `inside`, `limit` |
| `board.info` | describe the board or one element | `id`, `limit` |
| `board.preview` | PNG without changing anything | `scale` or `max_size`, `region`, `only`, `layers` |
| `board.view` | the area the window's picture shows, and its scale (read-only) | |
| `board.hit` | the element at a point (read-only) | `x`, `y`, `tolerance` |

`wetwaresuite help board.<command>` prints every parameter with its type and default. Over MCP the tools are named `board_new`, `board_add`, ... and also `undo`, `redo`, `history` and `documents`; open boards are MCP resources with live previews.

## 13. Limitations

- No rotation: shapes are axis-aligned. Excalidraw elements with an angle open unrotated.
- Text uses the standard PDF font metrics (Helvetica, Times, Courier) with FreeSans/FreeSerif/FreeMono glyphs in pictures. There is no handwriting font, so the sketchy theme's text is clean; Excalidraw files use its hand font (Virgil) for sketchy boards, so text there is a little wider. Greek and Cyrillic are drawn and embedded with the built-in font (Roboto); other scripts (CJK, Arabic, ...) and emoji come out as `?`.
- Layout doesn't know about frames: nodes in a Mermaid subgraph are laid out with everything else and the frame is fitted around them afterwards; outside nodes that end up under a frame are pushed aside, but two subgraphs can still overlap on dense charts.
- Elbow routes avoid shapes but not other connectors; parallel links can share segments. Connectors don't route around frames.
- Sequence diagrams are placed once: moving a participant doesn't move its messages, and activation bars, `create`/`destroy` and `box` groups are ignored.
- Mermaid class diagrams, state diagrams, ER diagrams, Gantt charts and the new `A@{ shape: ... }` syntax aren't supported.
- Excalidraw: closed `line` polygons come back as open polylines; arrow labels that Excalidraw stores as separate text become connector labels only when bound. draw.io: only the first page is read; stencils without an equivalent (UML, cloud icons, ...) open as rectangles with their label; freehand ink is written as a curved line; HTML formatting in labels is dropped.
- PDF export is one page; there is no tiling of very large boards onto paper sizes (use `region=` per page).
- Very large boards: elbow routing looks at shapes within a few hundred units of each connector, and PNG exports are capped at 12000 px on the longest side (the scale drops to fit).

## 14. In the window

Open a board in `wetwaresuite-draw` (Whiteboard tab) and draw on it directly, like Miro or Excalidraw. The rail on the left:

- **Select** (V): click a shape, note, connector or ink stroke to pick it. Drag to move it (connectors attached to it follow), drag the dot at its bottom-right corner to resize. **Double-click** a shape or note (or press Enter) to type on it: Enter keeps the words, Ctrl+Enter starts a new line, Esc cancels. Arrow keys nudge (Shift: further), Delete removes. The bar has Edit text, Front, Back, Duplicate, a colour (fill for shapes, line colour for connectors and ink) and Delete.
- **Note** (N), **Box** (R), **Circle** (O): click to drop one at a default size, or drag to make it the size you want. You're typing on it straight away; afterwards the tool goes back to Select.
- **Words** (T): free text; click and type.
- **Connect** (C): drag from one shape to another. The arrow is attached, so it follows when either shape moves. Let go on empty board (or start on empty board) for a loose end.
- **Ink** (P): draw freehand with the mouse or a pen. The bar sets the width.

New things use the board's theme colours until you pick a colour on the bar. Middle or right drag pans, the wheel zooms, Space+drag pans too. Every action is one board command (`board.add`, `board.set`, `board.connect`, `board.ink`, `board.remove`), so Ctrl+Z takes it back and Claude sees the same board.
