# Asset forge: game assets

`forge` gets assets ready for a game engine. It has two halves:

- **Meshes**: open a model (STL, OBJ, PLY, OFF, glTF/GLB, 3MF), fix its up axis and scale, put it on the floor, cut its triangle count, recompute normals, give it UVs, write a chain of LOD files, or do all of that to a whole folder in one command.
- **Animated pixel art**: Aseprite-style sprites with frames, durations, layers, animation tags and a palette (optionally indexed colour). Open and save `.aseprite`/`.ase`, draw pixels, and export sprite sheets with Aseprite JSON, Godot SpriteFrames and animated GIFs.
- **Textures and 2D art**: pack sprite sheets with a JSON atlas engines can read, cut sheets back into frames, make normal maps, make textures tile, reduce pictures to pixel-art palettes, outline sprites, resize to powers of two, pack greyscale maps into one texture's channels (ORM maps) and trim transparent borders.

Sprites and meshes made here go straight into Godot scenes with [scenes](scene.md) (`scene.add type=Sprite2D props="texture=..."`, `mesh=model.glb`).

In the window, an open sprite gets pixel tools on the canvas (Pencil, Eraser, Line, Box, Ellipse, Bucket, Pick) with frames, onion skin, playback, layers and palette swatches beside it; every click runs the same commands shown here. Meshes and textures have no canvas tools yet. Each task below is shown three ways:

- **CLI**: single `wetwaresuite` commands that read files and write files.
- **Batch**: a script for multi-step work with `-> name` captures and `$name` reuse (see [headless and batch](headless-and-batch.md)).
- **Claude**: a prompt for Claude over MCP and the tools it calls (see [mcp.md](mcp.md)). `wetwaresuite mcp --tools forge` serves just forge plus photo (forge's image commands edit images that `photo_open` opens).

Every example below was run against the real binary; the outputs are copied from those runs (long ones are shortened where marked).

## How forge works

**Mesh documents.** `forge.mesh_open` opens a model as a document with an id such as `mesh1`. The other `forge.mesh_*` commands take `doc=mesh1` and change it in place; every change is one undo step. `forge.mesh_save` writes it out, and the extension picks the format: `.glb` for most engines, `.obj`, `.stl`, `.ply`, `.off`, `.gltf`, `.3mf`, or `.png`/`.jpg` for a rendered picture.

**Up axes.** Inside the suite Z is up (like Blender, CAD and 3D printing). glTF files are Y-up by definition, so they are rotated on the way in and out automatically. OBJ, STL and PLY files have no fixed convention: most game models are exported Y-up, so a downloaded OBJ arrives lying on its back. `axes=y_up_to_z_up` stands it up (do this before saving `.glb`); `axes=z_up_to_y_up` lays a model down for a Y-up engine that reads OBJ/STL.

**Image commands take a document or a file.** Commands that change one image (`forge.seamless`, `forge.palette`, `forge.upscale`, `forge.outline`, `forge.resize_pot`, `forge.trim`) work either way:

- `doc=img1` edits an image opened with `photo.open`, in place and undoable (layers are flattened into one). Add `output=` to also write the result.
- `input=a.png output=b.png` reads a file and writes the result, without opening anything: the one-shot CLI form.

Commands that make a new image (`forge.normal_map`, `forge.tile_preview`, `forge.channel_pack`, `forge.sprite_sheet`) read a `doc` or an `input` and open the result as a new document (returned as `doc`, so `-> name` captures it); `output=` also writes it.

## Task 1: prepare a downloaded model for a game engine

You downloaded `downloads/statue.obj`: 38 000 triangles, modelled Y-up in centimetres, standing 20 cm below the origin. The engine wants a `.glb` in metres, standing on the floor at the origin, around 10 000 triangles, with UVs and a set of LODs.

First, look at it. `forge.mesh_open` is read-only, so it works as a one-shot inspector:

```sh
$ wetwaresuite forge.mesh_open path=downloads/statue.obj
opened downloads/statue.obj as mesh1: 38080 triangles, 19360 vertices, size 73.06 x 160 x 72.41 at (-38.5, -20, -36.21), watertight
doc: mesh1
triangles: 38080
vertices: 19360
size: 73.0616,160,72.4134
min: -38.5,-20,-36.2067
max: 34.5616,140,36.2067
watertight: true
open_edges: 0
normals: false
uvs: false
area: 30170.1
volume: 411036
```

The size is 160 along Y: it's Y-up, and in centimetres.

**CLI.** `forge.mesh_batch` does the whole preparation for every model in a folder, then `forge.mesh_lods` writes the LOD chain:

```sh
$ wetwaresuite forge.mesh_batch input_dir=downloads output_dir=game axes=y_up_to_z_up scale=0.01 center=true floor=true triangles=10000 uv=cylindrical
processed 1 of 1 models into game
  game/statue.glb (38080 -> 9966 triangles)
processed: 1
skipped: 0
failed: 0
triangles_in: 38080
triangles_out: 9966

$ wetwaresuite forge.mesh_lods input=game/statue.glb output=game/lods/statue.glb ratios=1,0.5,0.25,0.1 uv=cylindrical
wrote 4 LODs:
  game/lods/statue_lod0.glb (9966 triangles)
  game/lods/statue_lod1.glb (4836 triangles)
  game/lods/statue_lod2.glb (2308 triangles)
  game/lods/statue_lod3.glb (888 triangles)
files: game/lods/statue_lod0.glb, game/lods/statue_lod1.glb, game/lods/statue_lod2.glb, game/lods/statue_lod3.glb
triangles: 9966,4836,2308,888
levels: 4

$ wetwaresuite convert.file input=game/statue.glb output=game/statue.png width=512 view=front
rendered game/statue.glb to game/statue.png
triangles: 9966
output: game/statue.png
backend: mesh
bytes: 85463
```

- `mesh_batch` runs its steps in a fixed order: weld (if `weld=true`), decimate, axes, scale, center, floor, normals, UVs. Decimating before centering means the result sits exactly on the floor.
- Decimation always recomputes normals (with `crease=60` degrees: sharper edges stay hard) and drops UVs, which is why `uv=` projects new ones.
- Existing results are skipped; add `overwrite=true` to redo them. Originals are never replaced.
- Each LOD is decimated from the original, not from the previous level, so errors don't pile up. Ratio 1 copies the original.
- The last command renders a picture to check the result: upright, with the front view showing its full 1.6 m height.

**Batch.** A script gives you control over each step and lets you inspect between them:

```text
# statue.txt: prepare a downloaded Y-up OBJ (in centimetres) for a game engine
forge.mesh_open path=downloads/statue.obj -> m
forge.mesh_decimate doc=$m triangles=10000
forge.mesh_transform doc=$m axes=y_up_to_z_up scale=0.01 center=true floor=true
forge.mesh_uv doc=$m projection=cylindrical axis=z
forge.mesh_info doc=$m
forge.mesh_save doc=$m path=game/statue.glb
forge.mesh_lods doc=$m output=game/lods/statue.glb ratios=1,0.5,0.25,0.1 uv=cylindrical
forge.mesh_save doc=$m path=game/statue.png width=512 view=front
```

```sh
$ wetwaresuite batch statue.txt
opened downloads/statue.obj as mesh1: 38080 triangles, 19360 vertices, size 73.06 x 160 x 72.41 at (-38.5, -20, -36.21), watertight
...
decimated 38080 -> 9966 triangles
...
transformed: 9966 triangles, 4987 vertices, size 0.7296 x 0.7241 x 1.6 at (-0.3648, -0.362, 0), open
...
cylindrical UVs generated
...
9966 triangles, 5067 vertices, size 0.7296 x 0.7241 x 1.6 at (-0.3648, -0.362, 0), open
normals: yes, uvs: yes, no holes but some edges are shared by more than two triangles or wound inconsistently
...
saved game/statue.glb
...
wrote 4 LODs:
  game/lods/statue_lod0.glb (9966 triangles)
  game/lods/statue_lod1.glb (4836 triangles)
  game/lods/statue_lod2.glb (2308 triangles)
  game/lods/statue_lod3.glb (888 triangles)
...
saved game/statue.png
```

(shortened: each command also prints its `key: value` fields.) Notice the mesh went from `watertight` to `open` after decimation: vertex clustering is fast but can leave a few non-manifold edges. That is harmless for rendering in an engine; for 3D printing or booleans, decimate less or not at all.

Other useful steps for a model like this:

```text
# more.txt
forge.mesh_open path=game/statue.glb -> m
# face a different way (degrees about x, y, z)
forge.mesh_transform doc=$m rotate=0,0,90
# harder edges; crease=180 smooths everything
forge.mesh_normals doc=$m crease=30
# only if it renders inside out, flip winding and normals:
# forge.mesh_normals doc=$m mode=flip
# close cracks between nearly touching vertices (this also merges UV seams)
forge.mesh_weld doc=$m tolerance=0.0001
# stand it on a 1 x 1 x 0.1 m plinth from another file
forge.mesh_merge doc=$m other=parts/plinth.obj move=0,0,-0.1
forge.mesh_normals doc=$m
forge.mesh_save doc=$m path=game/statue_on_plinth.png width=512
```

```sh
$ wetwaresuite batch more.txt
opened game/statue.glb as mesh1: 9966 triangles, 5067 vertices, size 0.7296 x 0.7241 x 1.6 at (-0.3648, -0.362, 0), open
...
merged 12 triangles: 9978 triangles, 4992 vertices, size 1 x 1 x 1.7 at (-0.5, -0.5, -0.1), open
...
```

The plinth file has no normals, so the merged mesh loses them (normals and UVs survive a merge only when both sides have them); the `forge.mesh_normals` after it puts them back.

**Claude.**

> I downloaded /home/me/assets/statue.obj. Get it ready for Godot: glb, metres, standing on the floor, about 10k triangles, with UVs and four LODs in /home/me/game/models. Show me what it looks like.

Claude calls `forge_mesh_open` (sees the 160-unit Y extent and suggests it's Y-up in centimetres), `forge_mesh_decimate` with `triangles=10000`, `forge_mesh_transform` with `axes=y_up_to_z_up`, `scale=[0.01]`, `center=true`, `floor=true`, `forge_mesh_uv` with `projection=cylindrical`, `forge_mesh_preview` with `view=front` to look at it, then `forge_mesh_save` to `statue.glb` and `forge_mesh_lods` with `ratios=[1,0.5,0.25,0.1]` and `uv=cylindrical`. Any mesh command accepts `preview=true` to return a render with its result, so Claude can check each step as it goes, and `undo` steps back.

## Task 2: a sprite sheet and atlas for a character animation

You have eight frames of a run cycle, `hero/run_1.png` to `hero/run_8.png`, each 48 x 48 with a transparent background, and want one sheet image plus an atlas for Phaser, PixiJS, Godot or Unity.

**CLI.** Three common layouts:

```sh
# tight: shelf-packed, transparent borders trimmed, power-of-two sheet
$ wetwaresuite forge.sprite_sheet input_dir=hero output=sheets/hero_tight.png trim=true pot=true
packed 8 frames into 128x128 sheets/hero_tight.png + sheets/hero_tight.json (opened as img1)
frames: 8
atlas: sheets/hero_tight.json
output: sheets/hero_tight.png
doc: img1
width: 128
height: 128

# untrimmed, packed with the default 2 px padding
$ wetwaresuite forge.sprite_sheet input_dir=hero output=sheets/hero.png
packed 8 frames into 202x102 sheets/hero.png + sheets/hero.json (opened as img1)
...

# a classic strip: one row of equal cells, no padding
$ wetwaresuite forge.sprite_sheet input_dir=hero output=sheets/hero_strip.png layout=grid columns=8
packed 8 frames into 384x48 sheets/hero_strip.png + sheets/hero_strip.json (opened as img1)
...
```

Frames are sorted by name the way a person would (`run_2` before `run_10`). The atlas is a TexturePacker-style "JSON hash", which most engines and plugins import directly:

```json
{
  "frames": {
    "run_1.png": {
      "frame": { "x": 2, "y": 45, "w": 13, "h": 39 },
      "rotated": false,
      "trimmed": true,
      "spriteSourceSize": { "x": 18, "y": 6, "w": 13, "h": 39 },
      "sourceSize": { "w": 48, "h": 48 }
    },
    ...
  },
  "meta": { "app": "wetwaresuite forge", "version": "1.0", "image": "hero_tight.png", "format": "RGBA8888",
            "size": { "w": 128, "h": 128 }, "scale": "1" }
}
```

`frame` is where the frame is on the sheet. With `trim=true`, `spriteSourceSize` says where that trimmed piece sat in the original 48 x 48 frame, so the engine draws it in the right place and the character doesn't jitter. Frames are never rotated.

Going the other way, `forge.sprite_split` cuts a sheet into numbered files, by a grid or by an atlas:

```sh
$ wetwaresuite forge.sprite_split input=sheets/hero_strip.png output_dir=frames cell_width=48 prefix=run
wrote 8 frames to frames
frames: 8
skipped: 0
output_dir: frames
first: frames/run_00.png
last: frames/run_07.png

$ wetwaresuite forge.sprite_split input=sheets/hero_tight.png atlas=sheets/hero_tight.json output_dir=restored
wrote 8 frames to restored
...
first: restored/run_1.png
last: restored/run_8.png
```

By atlas, frames get their original names and their untrimmed 48 x 48 size back. By grid, give `cols`/`rows` or `cell_width`/`cell_height`, and `padding=` if the sheet has gaps between cells (and around the edge). Fully transparent cells are skipped unless `skip_empty=false`.

To outline every frame first, loop in the shell, then pack the outlined copies:

```sh
for f in hero/*.png; do wetwaresuite forge.outline input="$f" output="outlined/$(basename "$f")" color=#1a1a2e; done
wetwaresuite forge.sprite_sheet input_dir=outlined output=sheets/hero_outlined.png trim=true pot=true
```

**Batch.**

```text
# hero.txt: outline every frame of the run cycle, then pack them into a sheet + atlas
forge.outline input=hero/run_1.png output=build/hero/run_1.png color=#1a1a2e
forge.outline input=hero/run_2.png output=build/hero/run_2.png color=#1a1a2e
forge.outline input=hero/run_3.png output=build/hero/run_3.png color=#1a1a2e
forge.outline input=hero/run_4.png output=build/hero/run_4.png color=#1a1a2e
forge.outline input=hero/run_5.png output=build/hero/run_5.png color=#1a1a2e
forge.outline input=hero/run_6.png output=build/hero/run_6.png color=#1a1a2e
forge.outline input=hero/run_7.png output=build/hero/run_7.png color=#1a1a2e
forge.outline input=hero/run_8.png output=build/hero/run_8.png color=#1a1a2e
forge.sprite_sheet input_dir=build/hero output=sheets/hero_run.png trim=true pot=true -> sheet
# a 2x copy for high-DPI screens, and a check that the atlas cuts cleanly
forge.upscale doc=$sheet factor=2 output=sheets/hero_run@2x.png
forge.sprite_split input=sheets/hero_run.png atlas=sheets/hero_run.json output_dir=build/check
```

```sh
$ wetwaresuite batch hero.txt
1 px outline added
...
packed 8 frames into 128x128 sheets/hero_run.png + sheets/hero_run.json (opened as img1)
...
upscaled 2x to 256x256
...
wrote 8 frames to build/check
...
```

`-> sheet` captures the new sheet document, so later lines can edit it. (The 2x sheet needs its atlas numbers doubled; most engines let you set the scale instead.) A sheet can also be built from open images, which is handy when an earlier step made them: `files=$a,$b,$c` takes image ids or file paths, named after their files.

**Claude.**

> Pack the frames in /home/me/game/art/hero into a power-of-two sprite sheet with a JSON atlas for Phaser, trimmed, into /home/me/game/public/hero.png. Show me the sheet.

Claude calls `forge_sprite_sheet` with `input_dir`, `output`, `trim=true`, `pot=true` and `preview=true` (the sheet comes back as an image). For "give them a dark outline first" it calls `forge_outline` with `input`/`output` for each frame, then packs the outlined folder.

## Task 3: a tileable texture and its normal map

`textures/stone.png` is a 256 x 256 stone photo. Repeated across a floor, its edges show as a grid of seams, and the engine also wants a normal map so the stone catches the light.

**CLI.** Check the seams, fix them, check again, then make the normal map:

```sh
$ wetwaresuite forge.tile_preview input=textures/stone.png output=textures/stone_before_3x3.png
3x3 tile preview (edge difference 33.1) (opened as img1)
seam: 33.1198
output: textures/stone_before_3x3.png
doc: img1
width: 768
height: 768
attachment: img1.png (572972 bytes)

$ wetwaresuite forge.seamless input=textures/stone.png output=textures/stone_tile.png
made tileable (edge difference 33.1 -> 3.95)
seam_before: 33.1198
seam_after: 3.94661
width: 256
height: 256
output: textures/stone_tile.png

$ wetwaresuite forge.tile_preview input=textures/stone_tile.png output=textures/stone_tile_3x3.png
3x3 tile preview (edge difference 3.95) (opened as img1)
...

$ wetwaresuite forge.normal_map input=textures/stone_tile.png output=textures/stone_tile_n.png strength=3
normal map made (OpenGL, strength 3) (opened as img1)
output: textures/stone_tile_n.png
doc: img1
width: 256
height: 256
```

- The edge difference is the average colour step across the wrap-around edges; neighbouring pixels inside a stone texture differ by a few levels, so single digits means no visible seam.
- `forge.seamless` blends the texture with a copy shifted by half its size, first across, then down. `blend` (default 0.25) is how far in from each edge the blend reaches: wider hides seams better but softens and ghosts more of the picture.
- The normal map treats brightness as height (white is raised). It's in OpenGL convention (green up), which Blender, Godot and Unity use; for Unreal or other DirectX-convention engines add `invert_y=true`. `wrap=true` (the default) keeps a tileable texture's normal map tileable too.
- For a dedicated height map, pass that instead; `channel=red` (or `green`, `blue`, `alpha`) reads one channel.

An ORM texture (ambient occlusion in red, roughness in green, metallic in blue) comes from `forge.channel_pack`. Here the stone's brightness stands in for occlusion, roughness is a constant 200 and metallic is 0:

```sh
$ wetwaresuite forge.channel_pack red=textures/stone_tile.png green_value=200 output=textures/stone_orm.png
packed red into one 256x256 texture (opened as img1)
output: textures/stone_orm.png
doc: img1
width: 256
height: 256
```

**Batch.**

```text
# stone.txt: tileable stone: colour map, normal maps for both conventions, ORM map, preview
photo.open path=textures/stone.png -> stone
forge.seamless doc=$stone blend=0.2
forge.resize_pot doc=$stone mode=up
photo.save doc=$stone path=game/stone_albedo.png
forge.normal_map doc=$stone strength=3 output=game/stone_normal_gl.png
forge.normal_map doc=$stone strength=3 invert_y=true output=game/stone_normal_dx.png
forge.channel_pack red=$stone green_value=200 blue_value=0 output=game/stone_orm.png
forge.tile_preview doc=$stone tiles=3 output=game/stone_check.png
```

```sh
$ wetwaresuite batch stone.txt
opened textures/stone.png as img1
...
made tileable (edge difference 33.1 -> 4.16)
...
already 256x256, powers of two
...
saved game/stone_albedo.png
...
normal map made (OpenGL, strength 3) (opened as img2)
...
normal map made (DirectX, strength 3) (opened as img3)
...
packed red into one 256x256 texture (opened as img4)
...
3x3 tile preview (edge difference 4.16) (opened as img5)
...
```

`forge.resize_pot` is there for textures that aren't already powers of two (a 640 x 480 photo becomes 512 x 512 with the default `mode=nearest`); `channel_pack` reads the open document `$stone` as easily as a file.

**Claude.**

> Make /home/me/tex/stone.png tile seamlessly, show me a 3x3 preview, then make a DirectX normal map for Unreal next to it.

Claude calls `forge_seamless` (with `input` and `output`, or `photo_open` and then `doc`), `forge_tile_preview` (which returns the 3x3 picture), and `forge_normal_map` with `invert_y=true` and `preview=true`. If the seams still show, it can `undo` and try a wider `blend`.

## Task 4: turn a picture into 16-colour pixel art

`photo.jpg` is a 640 x 480 landscape. Pixel art needs three things: few pixels, few colours, and big crisp pixels when it's shown.

**CLI.**

```sh
$ wetwaresuite convert.file input=photo.jpg output=pixel/small.png width=160
converted photo.jpg to pixel/small.png
output: pixel/small.png
backend: raster
bytes: 8091

$ wetwaresuite forge.palette input=pixel/small.png output=pixel/small16.png colors=16 method=kmeans
reduced 1190 colours to 16
colors_before: 1190
colors: 16
palette: #336a31,#71a889,#70a3df,#3e8842,#87b0d7,#479645,#5d9c6e,#9abbd0,#5e98e4,#8ab6ad,#327937,#659ce2,#fbe57b,#7ca9da,#91b6d3,#a4c0cc
width: 160
height: 120
output: pixel/small16.png

$ wetwaresuite forge.upscale input=pixel/small16.png output=pixel/art.png factor=4
upscaled 4x to 640x480
width: 640
height: 480
output: pixel/art.png
```

- Shrinking first (160 px wide here) does most of the work; `convert.file` averages the pixels it drops, so edges stay clean.
- `method=median_cut` (the default) is fast and good; `kmeans` refines those colours and often matches the picture more closely. `dither=true` adds Floyd-Steinberg dithering: smoother gradients in skies, busier pixels.
- `forge.upscale` uses nearest-neighbour sampling, so each art pixel becomes a crisp 4 x 4 block. Keep the small version as the real asset and let the engine scale it with nearest filtering if it can.
- `palette` returns the colours it chose, ready to reuse with other pictures (`palette=#336a31,#71a889,...`) so a set of sprites shares one palette.

**Batch.** The same, then a second version in the fixed 16-colour PICO-8 palette, using `undo` to go back to the small full-colour image in between:

```text
# pixel.txt: picture -> 16-colour pixel art, two ways: its own best 16 colours, and the PICO-8 palette
photo.open path=photo.jpg -> pic
photo.resize doc=$pic width=160
forge.palette doc=$pic colors=16 method=kmeans
forge.upscale doc=$pic factor=4 output=pixel/art_kmeans.png
undo
undo
forge.palette doc=$pic dither=true palette=#000000,#1d2b53,#7e2553,#008751,#ab5236,#5f574f,#c2c3c7,#fff1e8,#ff004d,#ffa300,#ffec27,#00e436,#29adff,#83769c,#ff77a8,#ffccaa
forge.upscale doc=$pic factor=4
photo.save doc=$pic path=pixel/art_pico8.png
```

```sh
$ wetwaresuite batch pixel.txt
opened photo.jpg as img1
...
resized to 160x120
...
reduced 1190 colours to 16
...
upscaled 4x to 640x480
...
undone
undone
reduced 1190 colours to 14
...
upscaled 4x to 640x480
...
saved pixel/art_pico8.png
```

With a fixed palette, `colors` reports how many of its colours the picture actually used (14 of PICO-8's 16 here).

**Claude.**

> Turn /home/me/Pictures/castle.jpg into 16-colour pixel art about 160 pixels wide, shown at 4x, and save it as castle_pixel.png. Try it with and without dithering and show me both.

Claude calls `photo_open`, `photo_resize` with `width=160`, `forge_palette` with `colors=16` and `preview=true`, `forge_upscale` with `factor=4` and `preview=true`, then `undo` twice and repeats with `dither=true`, and saves the one you pick with `photo_save`.

## Task 5: an animated pixel-art sprite, Aseprite style

A sprite document (`spr1`) holds what an Aseprite file does: a canvas size, frames with their own durations in ms, layers (bottom first) with a picture (a *cel*) per frame, animation tags such as `idle` and `hit`, and a palette. With `color_mode=indexed` every pixel stays on the palette, so swapping the palette recolours the whole animation. Every edit is one undo step.

**Batch.** A 16 x 16 slime on the PICO-8 palette: draw one frame, copy it into a four-frame bounce, add a two-frame hit flash, tag both, then save the `.aseprite`, a GIF of the idle loop, and a sheet with its JSON and a Godot SpriteFrames resource:

```text
# slime.txt: a 16x16 slime with a bouncing idle loop and a hit flash, on the PICO-8 palette
forge.sprite_new width=16 height=16 duration=120 color_mode=indexed palette=pico8 -> s
forge.draw doc=$s tool=fill_ellipse points=2,6,13,15 color=#00e436
forge.draw doc=$s tool=ellipse points=2,6,13,15 color=#008751
forge.draw doc=$s tool=line points=5,9,5,10 color=#000000
forge.draw doc=$s tool=line points=10,9,10,10 color=#000000
# three more frames: down 1 px, down 2 px, down 1 px
forge.frame_add doc=$s count=3 copy=0
forge.sprite_transform doc=$s frame=1 dy=1
forge.sprite_transform doc=$s frame=2 dy=2
forge.sprite_transform doc=$s frame=3 dy=1
# two pale frames for the hit flash
forge.frame_add doc=$s count=2 copy=0
forge.draw doc=$s frame=4 tool=replace from_color=#00e436 color=#fff1e8
forge.draw doc=$s frame=5 tool=replace from_color=#00e436 color=#fff1e8
forge.tag_add doc=$s name=idle from=0 to=3
forge.tag_add doc=$s name=hit from=4 to=5 color=#ff004d
forge.frame_duration doc=$s tag=hit ms=60
forge.sprite_info doc=$s
forge.sprite_save doc=$s path=slime/slime.aseprite
forge.sprite_save doc=$s path=slime/idle.gif tag=idle scale=4
forge.sprite_export doc=$s output=slime/slime_sheet.png godot=slime/slime.tres
# a recoloured variant from the DawnBringer 16 palette, then back
forge.sprite_palette doc=$s action=preset name=db16 remap=true
forge.sprite_save doc=$s path=slime/slime_db16.gif scale=4
undo
```

```sh
$ wetwaresuite batch slime.txt
created spr1: 16x16 sprite, 1 frame, 1 layer, indexed 17 colours
...
fill_ellipse on Layer 1 frame 0: 96 pixels changed
...
added 3 frames at 1 (4 frames now)
...
replace on Layer 1 frame 4: 64 pixels changed
...
2 frames now last 60 ms
doc: spr1
16x16 sprite, 6 frames, 1 layer, tags idle,hit, indexed 17 colours
frames: 6 (120,120,120,120,60,60 ms; 600 ms in all)
layers (bottom first):
  0: Layer 1 (6 cels)
tags:
  idle: frames 0..3, forward
  hit: frames 4..5, forward
palette: #00000000,#000000,#1d2b53,#7e2553,#008751,#ab5236,#5f574f,#c2c3c7,#fff1e8,#ff004d,#ffa300,#ffec27,#00e436,#29adff,#83769c,#ff77a8,#ffccaa
...
saved slime/slime.aseprite
...
exported 6 frames to 96x16 slime/slime_sheet.png + slime/slime_sheet.json + slime/slime.tres
...
palette: #00000000,#140c1c,#442434,#30346d,#4e4a4e,#854c30,#346524,#d04648,#757161,#597dce,#d27d2c,#8595a1,#6daa2c,#d2aa99,#6dc2ca,#dad45e,#deeed6
colors: 17
saved slime/slime_db16.gif
...
undone
```

- Indexed palettes keep entry 0 as "transparent" (the `#00000000` above), as Aseprite does. Colours you draw with snap to the nearest palette entry; `index=12` picks an entry by number instead of `color=`.
- `forge.draw` tools: `pencil` (dots joined by lines, `points=x1,y1,x2,y2,...`, like a mouse drag), `line`, `rect`, `fill_rect`, `ellipse`, `fill_ellipse` (two corners), `bucket` (fills the touching pixels of one colour), `erase`, and `replace` (one colour for another; `all_frames=true` does the whole animation). `frame=` and `layer=` pick the cel; the default is frame 0 on the top layer.
- `slime_sheet.json` is in Aseprite's own export format (frames with `frame`, `spriteSourceSize`, `sourceSize` and `duration`, plus `meta.frameTags` and `meta.layers`), which Phaser, PixiJS, Godot importers and Unity's Aseprite tooling read. `format=array` writes the frames as an array; `layout=grid`, `vertical` or `packed trim=true` change how they sit on the sheet.
- `slime.tres` is a Godot 4 `SpriteFrames` resource with one animation per tag (looping at the frames' speed): keep the sheet next to it and give it to an `AnimatedSprite2D`.
- `sprite_palette action=preset ... remap=true` keeps each pixel's palette *index* and swaps the colours, so the same art comes out in a new palette. `action=set_color index=12 color=#...` changes one entry, `action=load path=my.gpl` reads a GIMP/JASC palette, `action=save path=my.gpl` writes one.
- `forge.sprite_preview doc=$s frame=2 onion_before=1 onion_after=1` attaches a picture of a frame with its neighbours faintly in red (before) and blue (after), the way Aseprite's onion skin shows motion.

**CLI.** Each `wetwaresuite` run is a new process, so feed short scripts to `batch` on stdin. Re-export the idle loop as a trimmed, packed sheet with an array JSON, and the hit flash as a big GIF:

```sh
$ printf 'forge.sprite_open path=slime/slime.aseprite -> s\nforge.sprite_export doc=$s output=export/slime.png layout=packed trim=true format=array tag=idle\nforge.sprite_save doc=$s path=export/hit.gif tag=hit scale=8\n' | wetwaresuite batch
opened spr1: 16x16 sprite, 6 frames, 1 layer, tags idle,hit, indexed 17 colours, slime/slime.aseprite
...
exported 4 frames to 12x36 export/slime.png + export/slime.json
...
saved export/hit.gif
...
```

`forge.sprite_open` also reads a sprite sheet (`path=sheet.png columns=6`, or `cell_width=`/`cell_height=`), a folder of frame images (`input_dir=`) or a list (`files=`), so loose frames from another tool become one animation to tag and export.

**Claude.**

> Make me a 32x32 pixel-art coin on the DB16 palette that spins in 6 frames at 80 ms, save it as coin.aseprite and a GIF, and give me a sheet with Godot SpriteFrames.

Claude calls `forge_sprite_new` with `palette=db16`, draws with `forge_draw` (checking each step with `preview=true`), adds frames with `forge_frame_add`, tags them with `forge_tag_add`, and saves with `forge_sprite_save` and `forge_sprite_export godot=coin.tres`.

## More texture helpers

```sh
# crop a sprite to its visible pixels and get the offset to place it back
$ wetwaresuite forge.trim input=hero/run_1.png output=trimmed/run_1.png
trimmed 48x48 to 13x39 at (18, 6)
x: 18
y: 6
original_width: 48
original_height: 48
width: 13
height: 39
output: trimmed/run_1.png

# power-of-two size for older engines, mobile GPUs and mipmaps
$ wetwaresuite forge.resize_pot input=photo.jpg output=pot.png
resized 640x480 -> 512x512
width: 512
height: 512
output: pot.png
```

## Command reference

`preview=true` attaches a 512 px PNG of the result, which Claude sees as an image. Mesh commands also take `view` (iso, front, back, left, right, top, bottom) for that preview.

### Meshes

#### forge.mesh_open
Open a model as a mesh document. Read-only (changes nothing outside the session). Returns `doc`, `triangles`, `vertices`, `size`, `min`, `max`, `watertight`, `open_edges`, `normals`, `uvs`, `area`, and `volume` when watertight.

| Parameter | Type | Required | Default | Description |
| --- | --- | --- | --- | --- |
| `path` | string | yes | | `.stl` `.obj` `.ply` `.off` `.glb` `.gltf` `.3mf` `.fbx` `.step` |

#### forge.mesh_save
Write the mesh, or a picture of it. Not undoable.

| Parameter | Type | Required | Default | Description |
| --- | --- | --- | --- | --- |
| `doc` | string | yes | | mesh id |
| `path` | string | yes | | `.glb` `.gltf` `.obj` `.stl` `.ply` `.off` `.3mf`, or `.png` `.jpg` `.bmp` `.tga` `.ppm` for a render |
| `ascii` | boolean | no | false | STL/PLY as text |
| `width` | integer | no | 1024 | render width (16..8192) |
| `height` | integer | no | = width | render height |
| `view` | string | no | `iso` | render camera |
| `outline` | boolean | no | true | dark lines on edges and silhouettes |

#### forge.mesh_info
Read-only. Same fields as `mesh_open`, plus a text summary that says whether the mesh has holes.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `doc` | string | yes | mesh id |

#### forge.mesh_preview
Render a PNG to look at. Read-only.

| Parameter | Type | Required | Default | Description |
| --- | --- | --- | --- | --- |
| `doc` | string | yes | | mesh id |
| `view` | string | no | `iso` | camera |
| `size` | integer | no | 512 | image size (16..4096) |
| `outline` | boolean | no | true | edge and silhouette lines |
| `smooth` | boolean | no | true | false shows flat facets |

#### forge.mesh_close
Close a mesh; unsaved changes are lost. `doc` (required).

#### forge.mesh_transform
Undoable. Steps run in this order: axes, scale, rotate, center, floor, move. Returns `size`, `min`, `max`.

| Parameter | Type | Required | Default | Description |
| --- | --- | --- | --- | --- |
| `doc` | string | yes | | mesh id |
| `axes` | `none`, `y_up_to_z_up`, `z_up_to_y_up` | no | `none` | up-axis conversion |
| `scale` | 1 or 3 numbers | no | | `0.01` (cm to m), `2,2,1` |
| `rotate` | 3 numbers | no | | degrees about x, y, z |
| `center` | boolean | no | false | bounding-box centre to the origin |
| `floor` | boolean | no | false | lowest point to z = 0 |
| `move` | 3 numbers | no | | offset |

#### forge.mesh_decimate
Undoable. Vertex clustering: fast, but may leave non-manifold edges and drops UVs.

| Parameter | Type | Required | Default | Description |
| --- | --- | --- | --- | --- |
| `doc` | string | yes | | mesh id |
| `triangles` | integer | one of | | target count (the result is at most this, usually 80-100% of it) |
| `ratio` | number | one of | | fraction of the current count, (0, 1] |
| `normals` | boolean | no | true | recompute normals |
| `crease` | number | no | 60 | degrees: sharper edges stay hard |

#### forge.mesh_normals
Undoable.

| Parameter | Type | Required | Default | Description |
| --- | --- | --- | --- | --- |
| `doc` | string | yes | | mesh id |
| `mode` | `recompute`, `flip`, `clear` | no | `recompute` | flip reverses winding and normals (inside-out meshes) |
| `crease` | number | no | 60 | 180 = smooth everywhere, 0 = flat facets |

Recomputing matches vertices by position, so it smooths across UV seams without merging them (UVs survive).

#### forge.mesh_weld
Undoable. Merges vertices closer than `tolerance` (default 0 = identical positions), drops collapsed triangles. Also merges UV and normal seams. Returns `removed`.

#### forge.mesh_merge
Undoable. Appends `other` (an open mesh id or a model file), optionally offset by `move`. Normals and UVs are kept only if both meshes have them.

#### forge.mesh_uv
Undoable. Replaces UVs with a projection.

| Parameter | Type | Required | Default | Description |
| --- | --- | --- | --- | --- |
| `doc` | string | yes | | mesh id |
| `projection` | `box`, `planar`, `cylindrical`, `spherical` | no | `box` | box: each face from its closest side (crates, rocks, walls); planar: flat along `axis` (floors, terrain, decals); cylindrical/spherical: around `axis` (barrels, trees, planets) |
| `axis` | `x`, `y`, `z` | no | `z` | projection direction or pole axis |
| `scale` | number | no | 1 | 2 repeats the texture twice |

UVs fill 0..1 on the longest side (box keeps one texel density on every side). Cylindrical and spherical UVs run past 1 on the triangles across the seam, so use a texture set to repeat.

#### forge.mesh_lods
Not undoable (writes files). Writes `<name>_lod0.<ext>` .. `_lodN`.

| Parameter | Type | Required | Default | Description |
| --- | --- | --- | --- | --- |
| `doc` / `input` | string | one of | | open mesh, or a model file |
| `output` | string | yes | | base name, e.g. `lods/rock.glb` |
| `ratios` | numbers | no | `1,0.5,0.25,0.125` | one file per ratio, each from the original |
| `normals` | boolean | no | true | recompute normals |
| `crease` | number | no | 60 | crease angle |
| `uv` | `none` or a projection | no | `none` | project UVs on every level |
| `uv_axis` | `x`, `y`, `z` | no | `z` | axis for `uv` |
| `ascii` | boolean | no | false | STL/PLY as text |

Returns `files`, `triangles` (per level), `levels`.

#### forge.mesh_batch
Not undoable. Processes every model in a folder: weld, decimate, axes, scale, center, floor, normals, UVs, save.

| Parameter | Type | Required | Default | Description |
| --- | --- | --- | --- | --- |
| `input_dir` | string | yes | | source folder |
| `output_dir` | string | yes | | results (created if missing; never replaces sources) |
| `format` | `glb` `gltf` `obj` `stl` `ply` `off` `3mf` | no | `glb` | output format |
| `axes` | as mesh_transform | no | `none` | up-axis conversion |
| `scale` | number | no | 1 | uniform scale |
| `center`, `floor` | boolean | no | false | as mesh_transform |
| `ratio` / `triangles` | number / integer | no | | decimate |
| `normals` | boolean | no | false | recompute normals (decimation always does) |
| `crease` | number | no | 60 | crease angle |
| `uv`, `uv_axis` | | no | `none`, `z` | project UVs |
| `weld` | boolean | no | false | weld identical vertices first |
| `recursive` | boolean | no | false | include subfolders |
| `overwrite` | boolean | no | false | redo existing results |

Returns `processed`, `skipped`, `failed`, `triangles_in`, `triangles_out`.

### Sprites

#### forge.sprite_sheet
Not undoable (writes files). The sheet opens as a new image.

| Parameter | Type | Required | Default | Description |
| --- | --- | --- | --- | --- |
| `input_dir` | string | one of | | folder of frames (sorted naturally by name) |
| `files` | string | one of | | comma-separated files or open image ids, in this order |
| `output` | string | yes | | sheet `.png` |
| `atlas` | string | no | output with `.json` | atlas file |
| `layout` | `packed`, `grid` | no | `packed` | shelf-packed or equal cells |
| `columns` | integer | no | 0 (about square) | grid: cells per row |
| `padding` | integer | no | 2 packed, 0 grid | px between frames and around the edge |
| `pot` | boolean | no | false | power-of-two sheet size |
| `trim` | boolean | no | false | packed: crop each frame's transparent border |

Returns `frames`, `atlas`, `output`, `doc`, `width`, `height`. Grid cells are the size of the largest frame; smaller frames sit centred in their cell and the atlas gives their exact rectangle.

#### forge.sprite_split
Not undoable (writes files).

| Parameter | Type | Required | Default | Description |
| --- | --- | --- | --- | --- |
| `doc` / `input` | string | one of | | sheet |
| `output_dir` | string | yes | | folder for frames |
| `cols` / `cell_width` | integer | one of (grid) | | frames across, or frame width |
| `rows` / `cell_height` | integer | no | 1 row | frames down, or frame height |
| `padding` | integer | no | 0 | px between cells and around the edge |
| `atlas` | string | no | | cut by a JSON atlas (hash or array) instead of a grid |
| `prefix` | string | no | sheet name | grid frames are `<prefix>_00.png`, `_01`, ... |
| `skip_empty` | boolean | no | true | skip fully transparent frames |
| `format` | `png`, `tga`, `bmp` | no | `png` | frame format |

### Animated sprites

Sprite documents (`spr1`, ...) are Aseprite-style animations. Edits are undoable; `frame` is 0-based and `layer` is a name or index (0 = bottom). Editing commands take `preview=true` to attach a strip of the frames.

| Command | Parameters (defaults) | What it does |
| --- | --- | --- |
| `forge.sprite_new` | `width`, `height`, `frames` (1), `duration` (100 ms), `color_mode` (`rgba` or `indexed`), `palette` (preset, `.gpl`/`.pal` file or `#a,#b,...`), `background` | new sprite with one layer; not undoable |
| `forge.sprite_open` | `path` (`.aseprite`/`.ase` or an image), `cell_width`/`cell_height` or `columns`/`rows`, `count`, `input_dir`, `files`, `duration` (100) | open a sprite, a sheet cut into frames, or loose frames |
| `forge.sprite_save` | `path` (`.aseprite`, `.ase`, `.gif`, `.png`...), `frame` (png), `tag` (gif), `scale` (1) | write an Aseprite file, an animated GIF or one flattened frame |
| `forge.sprite_export` | `output` (sheet png), `data` (json; `none` skips), `format` (`hash`/`array`), `layout` (`horizontal`, `vertical`, `grid`, `packed`), `columns`, `padding` (0), `trim` (false), `scale` (1), `tag`, `godot` (.tres), `frames_dir` | sheet + Aseprite JSON, optional Godot SpriteFrames and one PNG per frame |
| `forge.sprite_info` | | size, frames and durations, layers, tags, palette; also returns `layer_names`, `layer_visible`, `tag_ranges` |
| `forge.sprite_preview` | `frame` (0), `onion_before` (0), `onion_after` (0), `onion_opacity` (96), `scale` (about 256 px) | one frame as a picture, with onion skin |
| `forge.sprite_close` | | close it |
| `forge.frame_add` | `at` (end), `count` (1), `copy` (frame before), `empty` (false), `duration` | add copies of a frame or empty frames; tags spanning `at` grow |
| `forge.frame_remove` | `frame`, `count` (1) | delete frames; tags shrink |
| `forge.frame_move` | `frame`, `to` | move a frame |
| `forge.frame_duration` | `ms`, `frames` (all) or `tag` | set frame durations |
| `forge.sprite_layer_add` | `name`, `index` (top), `copy` | add a layer, empty or a copy |
| `forge.sprite_layer_set` | `layer`, `name`, `visible`, `opacity` (0..255), `blend` (`normal`, `multiply`, `screen`, `overlay`, `darken`, `lighten`, `difference`, `addition`), `index` | layer settings |
| `forge.sprite_layer_remove` | `layer` | delete a layer |
| `forge.tag_add` | `name`, `from`, `to`, `direction` (`forward`, `reverse`, `pingpong`, `pingpong_reverse`), `repeat` (0 = loop), `color` | name an animation; re-adding a name changes it |
| `forge.tag_remove` | `name` | delete a tag (frames stay) |
| `forge.draw` | `frame`, `layer`, `tool` (`pencil`, `line`, `rect`, `fill_rect`, `ellipse`, `fill_ellipse`, `bucket`, `erase`, `replace`), `points`, `color` or `index`, `from_color`, `all_frames` | draw pixels |
| `forge.cel_paste` | `frame`, `layer`, `input` (image file or id) or `from_frame`/`from_layer`, `x`, `y`, `replace` | paste a picture or another cel |
| `forge.cel_clear` | `frame`, `layer`, `all_frames` | empty a cel |
| `forge.sprite_transform` | `frame`, `layer` (both optional), `flip_h`, `flip_v`, `rotate` (90/180/270), `dx`, `dy`, `wrap` | flip, turn or shift cels |
| `forge.sprite_resize` | `scale`, or `width`/`height` with `anchor` (`center`, `top_left`, ...) | enlarge pixels or change the canvas |
| `forge.sprite_palette` | `action` (`preset`, `load`, `set`, `from_sprite`, `quantize`, `set_color`, `add`, `remove`, `save`), `name`, `path`, `colors`, `index`, `color`, `count`, `remap` | edit the palette; presets `pico8`, `db16`, `db32`, `gameboy`, `grayscale4` |
| `forge.sprite_color_mode` | `mode` (`rgba`/`indexed`), `count` (32) | switch colour mode; going indexed snaps pixels to the palette |

### Textures

The six editing commands take `doc` (edit an open image in place, undoable) or `input` + `output` (files); `output` with `doc` also writes the result. They return `width` and `height` (and `doc` when editing one).

| Command | Parameters (defaults) | What it does |
| --- | --- | --- |
| `forge.seamless` | `blend` (0.25, range 0.02..0.5) | make it tile; returns `seam_before`, `seam_after` |
| `forge.palette` | `colors` (16, 2..256), `method` (`median_cut` or `kmeans`), `dither` (false), `palette` (comma-separated colours, instead of picking) | reduce colours, keep transparency; returns `colors_before`, `colors`, `palette` |
| `forge.upscale` | `factor` (2, 1..16) | nearest-neighbour enlargement |
| `forge.outline` | `color` (#000000), `thickness` (1, 1..32), `diagonal` (false), `threshold` (128: alpha that counts as solid), `expand` (false: grow the canvas so the outline fits) | outline around visible pixels |
| `forge.resize_pot` | `mode` (`nearest`, `up`, `down`), `square` (false), `filter` (`bilinear` or `nearest`) | resize both sides to powers of two |
| `forge.trim` | `threshold` (0: alpha at or below is empty), `padding` (0) | crop to visible pixels; returns `x`, `y`, `original_width`, `original_height` |

These make a new image (opened as a new document, written to `output` when given), and are not undoable:

| Command | Parameters (defaults) | What it does |
| --- | --- | --- |
| `forge.normal_map` | `doc`/`input`, `output`, `strength` (2), `invert_y` (false = OpenGL), `wrap` (true), `channel` (`luma`, `red`, `green`, `blue`, `alpha`) | normal map from a height image; flat gives (128, 128, 255) |
| `forge.tile_preview` | `doc`/`input`, `output`, `tiles` (3, 1..16); `preview` defaults to true | the texture repeated `tiles` x `tiles`; returns `seam` |
| `forge.channel_pack` | `red`, `green`, `blue`, `alpha` (each an image id or file, brightness used), `red_value`/`green_value`/`blue_value` (0), `alpha_value` (255), `output` | combine greyscale maps into one texture; size follows the first source |

## Limits

- FBX (binary or ASCII, any version) is read and written: every mesh in the file, placed where its scene puts it, with normals and the first UV set; written files are Y-up in millimetres. Animation, skinning and cameras are dropped. DAE, USD and .blend aren't read yet; export glTF/GLB, FBX or OBJ from the source program. Materials, textures and node names in files are not kept (geometry, normals and UVs are).
- Decimation is vertex clustering: quick and fine for LODs and props, but rougher than edge-collapse tools, it can leave non-manifold edges, and it drops UVs (`uv=` regenerates them).
- UV projections are simple; they don't unwrap. For characters and hero props, unwrap in a modelling program.
- `forge.seamless` blends; it doesn't synthesise new texture, so strong features near the edges ghost.
- Sprite sheets don't rotate frames to pack tighter. `forge.sprite_sheet` writes TexturePacker JSON hash; `forge.sprite_export` writes Aseprite's JSON (hash or array).
- `.aseprite` files: tilemap layers open empty, linked cels become copies, cel opacity is multiplied into the pixels, and slices and user data are not kept. Sprites are written as 32-bit RGBA or 8-bit indexed (never greyscale) with zlib-compressed cels.
- Blend modes other than normal, multiply, screen, overlay, darken, lighten, difference and addition are kept in the file but draw as normal. GIFs use one palette for the whole animation (up to 255 colours plus transparent).
