# Sculpt: digital sculpting and retopology

`sculpt` shapes 3D models like clay, without a screen: brush strokes along a path (draw, inflate, smooth, flatten, pinch, grab, crease) with mirror symmetry and masks, subdivision, voxel remeshing, quadric decimation, quick automatic retopology into quads, heightmap displacement, whole-mesh smoothing, statistics, pictures from any angle, and export to STL, OBJ, PLY and glTF.

Sculpt documents are ordinary `libs/mesh` meshes, so they mix freely with the other 3D tools: `sculpt.copy` takes a cad part or a forge model, `forge.*` and `convert.*` commands work on a sculpt, and `slicer.load mesh=sculpt1` sends one straight to the slicer.

There is no GUI yet; the GUI that comes later will call these same commands. Each task below is shown three ways:

- **CLI**: single `wetwaresuite` commands. Documents only live inside one process, so for one-shots every editing command also takes `input=` (a model file) and `output=` (where to write the result) instead of `doc=`.
- **Batch**: a script of commands with `-> name` captures and `$name` reuse (see [headless and batch](headless-and-batch.md)).
- **Claude**: a prompt for Claude over MCP and the tools it calls (see [mcp.md](mcp.md)). `wetwaresuite mcp --tools sculpt` serves just sculpt (plus photo, for heightmap images).

Every example below was run against the real binary; outputs are copied from those runs, shortened where marked with `...`.

## How sculpt works

**Documents.** `sculpt.new` (a primitive), `sculpt.open` (a file) and `sculpt.copy` (any open mesh) make a sculpt document with an id like `sculpt1`. Every other command takes `doc=sculpt1` and changes it in place; every change is one undo step (`undo`, `redo`). Sculpt commands also accept any other mesh document (`mesh1` from `forge.mesh_open`, `cad1`), but a cad part is rebuilt from its feature tree whenever it changes, so copy it first.

**One vertex per position.** Model files often split vertices along UV seams and hard edges. A stroke would tear such a mesh open, so sculpt welds vertices when it opens or copies a model. Normals are dropped (they're recomputed for pictures and on export), and UVs are dropped too when welding had to merge seams.

**Space.** Z is up, front is -Y (the `front` view looks along +Y), units are whatever the file uses. Brush radii and voxel sizes are in those units.

**Strokes.** A stroke is a path of points. The brush is stamped ("dabs") every `spacing x radius` along it. Each dab moves the vertices within `radius` of its centre, weighted by the `falloff` curve, by `strength`, and by `1 - mask`. Points can be given two ways:

- `space=world` (default): `x,y,z` triples. Each is snapped to the nearest spot on the surface, so a point roughly near the surface is fine.
- `space=screen`: `u,v` pairs on a picture of the model rendered with the same `view`, `yaw` and `pitch` (u from 0 at the left edge to 1 at the right, v from 0 at the top to 1 at the bottom). Each point is cast onto the surface. This is how Claude aims strokes at what it sees in a preview. `sculpt.surface_point` converts one `u,v` into `x,y,z`.
- `space=ray`: `x,y,z,dx,dy,dz` per point, a ray from the eye. Each is cast onto the surface; for `grab` the stroke stays on the plane through where it first hit, so the surface follows the mouse. This is what the window's brush sends.

**Pen pressure.** `pressures=` gives one value from 0 to 1 per point; each dab's strength is scaled by the pressure where it falls (in between points it's blended). A tablet in the window fills it in for you.

**In the window.** Pick a brush on the 3D view's toolbar (Draw, Inflate, Smooth, Flatten, Pinch, Grab, Crease) and paint on the model: a circle shows the brush, `[` and `]` change its size, the stroke previews live and letting go makes it one undo step. Pen pressure scales the strength. Alt+drag still turns the view and right-drag moves it; pick **Off** to go back to plain clicking.

**Symmetry.** `symmetry=x` mirrors each dab across the plane x = 0 (also `y`, `z`, or combinations such as `xz`). The mirror planes go through the origin, so keep the model centred on them. `sculpt.symmetrize` copies one half over the other to make an existing model exactly symmetric.

**Masks.** A mask is a value from 0 (free) to 1 (protected) on every vertex. Masked vertices don't move under strokes, smoothing or displacement, and vertices masked 0.5 or more are locked during decimation.

**Memory limits.** Results are capped at 1 000 000 triangles and voxel grids at 160 cells along the longest side; a command that would pass them fails up front with a message saying what to lower, instead of filling memory.

## Task 1: sculpt a creature head from a sphere

Start from an icosphere: its triangles are all the same size, which is what brushes like. Level 5 has 20 480 triangles, a good density for broad shapes.

**Batch.**

```text
# head.txt: a stylised creature head from a sphere, mirrored left/right
sculpt.new shape=sphere level=5 size=2 -> h
# brow ridge across the front (the front is -Y)
sculpt.stroke doc=$h brush=draw points=0.55,-0.75,0.35,0,-0.92,0.42 radius=0.3 strength=0.8 symmetry=x
# eye sockets
sculpt.stroke doc=$h brush=draw points=0.28,-0.94,0.12,0.42,-0.88,0.12 radius=0.2 strength=1 invert=true symmetry=x
# pull a snout forward
sculpt.stroke doc=$h brush=grab points=0,-1,-0.15,0,-1.45,-0.25 radius=0.45
# mouth line
sculpt.stroke doc=$h brush=crease points=-0.4,-1.05,-0.42,0,-1.2,-0.45,0.4,-1.05,-0.42 radius=0.12 strength=1
# cheeks
sculpt.stroke doc=$h brush=inflate points=0.6,-0.6,-0.25 radius=0.35 strength=0.6 symmetry=x
# ears on top
sculpt.stroke doc=$h brush=grab points=0.5,0.1,0.85,0.75,0.15,1.35 radius=0.3 symmetry=x
sculpt.smooth doc=$h iterations=2 strength=0.3
sculpt.stats doc=$h
sculpt.export doc=$h path=out/head_front.png view=front size=512
sculpt.export doc=$h path=out/head_iso.png view=iso yaw=-30 size=512
sculpt.export doc=$h path=out/head.stl
sculpt.export doc=$h path=out/head.glb
```

```sh
$ wetwaresuite batch head.txt
new sphere sculpt1: 20480 triangles, 10242 vertices, size 2 x 2 x 2 at (-1, -1, -1), watertight
...
draw stroke: 9 dabs moved 773 vertices (up to 0.1689)
...
draw stroke: 4 dabs moved 290 vertices (up to 0.091)
...
grab stroke: 1 dabs moved 503 vertices (up to 0.4709)
...
crease stroke: 25 dabs moved 93 vertices (up to 0.08564)
...
inflate stroke: 1 dabs moved 636 vertices (up to 0.03148)
...
grab stroke: 1 dabs moved 450 vertices (up to 0.5494)
...
smoothed (2 passes, moved up to 0.004631)
...
20480 triangles, 10242 vertices, size 2 x 2.451 x 2.354 at (-1, -1.451, -1), watertight
30720 edges (0 open, 0 non-manifold), edge length 0.02638 / 0.03999 / 0.1235 (min / mean / max)
...
saved out/head_front.png
saved out/head_iso.png
saved out/head.stl
saved out/head.glb
```

Each stroke reports how many dabs it laid, how many vertices moved and the largest move; a stroke that moves nothing (off the model, fully masked) fails, so a typo in a coordinate doesn't pass silently. `grab` is not dabbed: its first point picks up the surface within the radius and the rest of the path is where it's dragged (here 0.45 forward and a little down for the snout). Because the strokes use `symmetry=x`, both eyes, both cheeks and both ears come from one stroke each.

The finished head, front view: a brow ridge, two eye sockets, a snout with a mouth line, and two ears.

**CLI.** With `input=` and `output=`, every edit is a one-shot that reads a file and writes the result (the same file is fine). Make the starting sphere with a two-line batch on stdin, then shape it step by step:

```sh
$ printf 'sculpt.new shape=sphere level=5 -> h\nsculpt.export doc=$h path=cli/ball.stl\n' | wetwaresuite batch
new sphere sculpt1: 20480 triangles, 10242 vertices, size 2 x 2 x 2 at (-1, -1, -1), watertight
...
saved cli/ball.stl

$ wetwaresuite sculpt.stroke input=cli/ball.stl output=cli/head.stl brush=draw points=0.55,-0.75,0.35,0,-0.92,0.42 radius=0.3 strength=0.8 symmetry=x
draw stroke: 9 dabs moved 773 vertices (up to 0.1689)
saved cli/head.stl
...

$ wetwaresuite sculpt.stroke input=cli/head.stl output=cli/head.stl brush=grab points=0,-1,-0.15,0,-1.45,-0.25 radius=0.45
grab stroke: 1 dabs moved 505 vertices (up to 0.4709)
saved cli/head.stl
...

$ wetwaresuite sculpt.smooth input=cli/head.stl output=cli/head.stl iterations=2 strength=0.3
smoothed (2 passes, moved up to 0.0007428)
saved cli/head.stl

$ wetwaresuite convert.file input=cli/head.stl output=cli/head.png width=512 view=front
rendered cli/head.stl to cli/head.png
```

`sculpt.open path=cli/head.stl` on its own is a quick read-only inspector. Each one-shot re-reads and re-writes the file, so for many strokes a batch script is faster, and only a batch (or MCP) session has undo.

**Claude.**

> Sculpt me a cartoon creature head from a sphere: a brow, eye sockets, a snout with a mouth, puffy cheeks and pointy ears, symmetric left and right. Save it as head.stl and head.glb.

Claude calls `sculpt_new` (shape sphere, level 5, `preview=true`), then `sculpt_stroke` once per feature with `symmetry=x`, usually with `space=screen` and `view=front` so it can aim at what it saw in the last preview, checking each result picture (`preview=true`). It uses `undo` when a stroke goes wrong, `sculpt_preview` with `yaw`/`pitch` to look from other angles, and `sculpt_export` twice at the end.

## Task 2: from a cad part to an organic print

Mechanical parts make good starting blocks, but their meshes are a few long, thin triangles that can't bend. Remeshing turns them into an even grid that sculpts well. Here a cad knob gets rounded edges and thumb dents, keeps its flat bottom, and goes to the slicer.

**Batch.**

```text
# knob.txt: a cad part made organic, then lightened for printing
cad.new -> c
cad.cylinder doc=$c diameter=30 height=12
cad.box doc=$c width=8 depth=36 height=6 center=true z=12
# copy the part's mesh into a sculpt (the cad part stays editable)
sculpt.copy source=$c -> k
# even quads everywhere: cad meshes have long thin triangles that sculpt badly
sculpt.remesh doc=$k voxel_size=0.4
# protect the flat bottom (it sits on the print bed), then soften every other edge
sculpt.mask doc=$k shape=plane point=0,0,1 normal=0,0,-1 softness=2
sculpt.smooth doc=$k iterations=40 method=laplacian
sculpt.mask doc=$k shape=clear
# dent a thumb grip into each end of the bar (symmetry=y mirrors to the other end)
sculpt.stroke doc=$k brush=draw points=0,15,15 radius=4 strength=1 invert=true symmetry=y
sculpt.stroke doc=$k brush=draw points=0,15,15,0,13,15 radius=4 strength=1 invert=true symmetry=y
sculpt.stats doc=$k
sculpt.decimate doc=$k triangles=20000
sculpt.export doc=$k path=out/knob.stl
sculpt.export doc=$k path=out/knob.png view=iso size=512
# straight to the slicer: sculpt documents are mesh documents
slicer.load mesh=$k printer=ender3 filament=pla -> job
slicer.export_gcode doc=$job path=out/knob.gcode
```

```sh
$ wetwaresuite batch knob.txt
created cad1 (0 features, 0 params, 0 bodies, mm)
...
added cylinder cylinder1 (1 body, volume 8468.68 mm^3)
...
added box box1 (2 bodies, volume 10196.7 mm^3)
...
copied cad1 into sculpt1: 264 triangles, 136 vertices, size 30 x 36 x 15 at (-15, -18, 0), watertight
...
remeshed at 90 cells: 264 -> 40500 triangles (20250 quads)
...
mask plane: 5165 of 20252 vertices masked
...
smoothed (40 passes, moved up to 1.269)
...
mask clear: 0 of 20252 vertices masked
...
draw stroke: 1 dabs moved 861 vertices (up to 0.5957)
...
draw stroke: 3 dabs moved 1204 vertices (up to 1.502)
...
40500 triangles, 20252 vertices, size 29.85 x 35.94 x 15 at (-14.92, -17.97, 2.623e-09), watertight
60750 edges (0 open, 0 non-manifold), edge length 0.07395 / 0.4045 / 0.7469 (min / mean / max)
...
decimated 40500 -> 20000 triangles
...
saved out/knob.stl
saved out/knob.png
loaded sculpt1 as print1 (20000 triangles) on the ender3
...
wrote out/knob.gcode: 75 layers, about 16m 35s, 1.53 m / 4.6 g of filament
...
```

Things to notice:

- The cylinder and the bar overlap as two separate bodies in the cad part. The remesh fuses them into one closed surface, which is what printers and smoothing need.
- The plane mask masks everything below z = 1 (the side `normal=0,0,-1` points to), fading over 2 mm so smoothing doesn't leave a step at the mask's edge. Laplacian smoothing rounds edges quickly but also shrinks; `method=taubin` (the default) keeps the volume and works more gently.
- Decimating at the end halves the file without visible change: quadric decimation removes triangles from flat areas first.

**CLI.** The same remesh and decimate steps work on files:

```sh
$ wetwaresuite sculpt.remesh input=out/knob.stl output=cli/knob_even.stl voxel_size=0.5
remeshed at 72 cells: 20000 -> 26144 triangles (13072 quads)
saved cli/knob_even.stl
...
```

**Claude.**

> Take the knob part I made in cad (cad1), round off all its edges except the bottom so it feels nice in the hand, add a thumb dent at both ends of the bar, and slice it for my Ender 3 in PLA.

Claude calls `sculpt_copy` (source cad1), `sculpt_remesh` with a `voxel_size` about 1/80 of the part, `sculpt_mask` (plane under the base), `sculpt_smooth`, `sculpt_mask shape=clear`, `sculpt_stroke` with `symmetry=y` and `invert=true`, checking previews, then `slicer_load mesh=sculpt1` and `slicer_export_gcode`.

## Task 3: retopology and LODs for a game

A sculpt is dense and messy; a game wants a light, even mesh. `sculpt.retopo` rebuilds the surface as about the number of quads you ask for, relaxed and projected back onto the sculpt; `sculpt.decimate` cuts triangles while keeping the silhouette, for LODs.

**Batch.**

```text
# game.txt: the sculpted head as a light, quad-based game model with LODs
sculpt.open path=out/head.stl -> hi
sculpt.copy source=$hi -> lo
sculpt.retopo doc=$lo quads=1500
sculpt.stats doc=$lo
# quads survive in OBJ; glb gets each quad as two triangles
sculpt.export doc=$lo path=game/head_lo.obj
sculpt.export doc=$lo path=game/head_lo.glb
sculpt.export doc=$lo path=game/head_lo.png view=front size=512
# LODs straight from the high-res sculpt: quadric decimation keeps the silhouette
sculpt.copy source=$hi -> lod1
sculpt.decimate doc=$lod1 triangles=1000
sculpt.export doc=$lod1 path=game/head_lod1.glb
sculpt.decimate doc=$lod1 triangles=300
sculpt.export doc=$lod1 path=game/head_lod2.glb
sculpt.export doc=$lod1 path=game/head_lod2.png view=front size=512
```

```sh
$ wetwaresuite batch game.txt
opened out/head.stl as sculpt1: 20480 triangles, 10242 vertices, size 2 x 2.451 x 2.354 at (-1, -1.451, -1), watertight
...
copied sculpt1 into sculpt2: 20480 triangles, 10242 vertices, size 2 x 2.451 x 2.354 at (-1, -1.451, -1), watertight
...
retopologized 20480 triangles into 1542 quads
...
3084 triangles, 1544 vertices, size 1.998 x 2.437 x 2.332 at (-0.999, -1.44, -0.9989), watertight
4626 edges (0 open, 0 non-manifold), edge length 0.03431 / 0.1027 / 0.1656 (min / mean / max)
...
saved game/head_lo.obj with quads
saved game/head_lo.glb
saved game/head_lo.png
...
decimated 20480 -> 1000 triangles
...
saved game/head_lod1.glb
decimated 1000 -> 300 triangles
...
saved game/head_lod2.glb
saved game/head_lod2.png
```

`head_lo.obj` has 1542 four-sided `f` lines, ready for UV unwrapping and rigging in another program; the `.glb` stores the same quads as triangle pairs because glTF has no quads. The 300-triangle LOD still reads as the same head from a distance.

**CLI.**

```sh
$ wetwaresuite sculpt.retopo input=out/head.stl output=cli/head_quads.obj quads=1500
retopologized 20480 triangles into 1542 quads
saved cli/head_quads.obj
...

$ wetwaresuite sculpt.decimate input=out/head.stl output=cli/head_2k.glb triangles=2000
decimated 20480 -> 2000 triangles
saved cli/head_2k.glb
...
```

`forge.mesh_lods` and `forge.mesh_uv` take it from there (UVs, LOD chains with the same mapping).

**Claude.**

> Make a game-ready version of head.stl: about 1500 quads as an OBJ, plus 1000- and 300-triangle LODs as glb.

Claude calls `sculpt_open`, `sculpt_copy`, `sculpt_retopo` (quads 1500, `preview=true` to check it), `sculpt_export` to `.obj`, then `sculpt_copy` again from the original and `sculpt_decimate` twice with an export after each.

## Task 4: a relief tile from a heightmap

`sculpt.displace` pushes the surface along its normals by an image's brightness. A plane from `sculpt.new` maps the image straight on: the image's top edge is the plane's +Y side. The heightmap can be any image file, or one painted in the same session with paint and photo commands.

**Batch.**

```text
# relief.txt: a heightmap painted on the spot, pressed into a tile
paint.new width=256 height=256 background=black -> hm
paint.gradient doc=$hm kind=radial x1=128 y1=128 x2=128 y2=20 from=#808080 to=#000000
paint.shape doc=$hm kind=ellipse x=78 y=78 width=100 height=100 color=white filled=true
paint.shape doc=$hm kind=rect x=118 y=20 width=20 height=216 color=#c0c0c0 filled=true
photo.effect doc=$hm kind=blur amount=4
photo.save doc=$hm path=out/height.png
# a 100 x 100 mm plane, 2 x 4^7 = 32768 triangles: enough for the detail to show
sculpt.new shape=plane size=100 level=7 -> tile
# white rises 6 mm, black stays flat
sculpt.displace doc=$tile image=$hm amount=6 midlevel=0
# take the stair-steps of 8-bit grey out of the slopes
sculpt.smooth doc=$tile iterations=2
sculpt.stats doc=$tile
sculpt.export doc=$tile path=out/relief.png view=iso size=512
sculpt.export doc=$tile path=out/relief.obj
```

```sh
$ wetwaresuite batch relief.txt
created img1 with layers Background and Layer 1
...
saved out/height.png
...
new plane sculpt1: 32768 triangles, 16641 vertices, size 100 x 100 x 0 at (-50, -50, 0), open
...
displaced by img1 (up to 6)
...
smoothed (2 passes, moved up to 0.05157)
...
32768 triangles, 16641 vertices, size 100 x 100 x 6.033 at (-50, -50, -0.0122), open
49408 edges (512 open, 0 non-manifold), edge length 0.7812 / 0.9075 / 2.08 (min / mean / max)
...
saved out/relief.png
saved out/relief.obj
```

A disc rises 6 mm in the middle with a ridge across it, over a gentle mound. The plane is a single sheet (the 512 open edges are its border), fine for rendering, CNC and as a texture source; to print it, give it thickness in cad or another tool first. The image's resolution matters less than the mesh's: detail finer than the triangles can't show, so subdivide (or use a higher `level`) before displacing.

On other shapes pick a projection: `mapping=spherical` for heads and rocks, `cylindrical` for bottles and trunks (`axis=` is the pole), `uv` for a model with its own UVs, `tile=4` to repeat a texture.

**CLI.** With a plane saved first:

```sh
$ printf 'sculpt.new shape=plane size=100 level=7 -> t\nsculpt.export doc=$t path=cli/tile.obj\n' | wetwaresuite batch
new plane sculpt1: 32768 triangles, 16641 vertices, size 100 x 100 x 0 at (-50, -50, 0), open
...
saved cli/tile.obj

$ wetwaresuite sculpt.displace input=cli/tile.obj output=cli/tile_relief.stl image=out/height.png amount=6 midlevel=0
displaced by out/height.png (up to 6)
saved cli/tile_relief.stl
...
```

**Claude.**

> Make a 100 mm square relief tile from logo.png, 4 mm high where it's white, and give me an OBJ.

Claude calls `sculpt_new` (shape plane, size 100, level 7), `sculpt_displace` with `image=logo.png`, `amount=4`, `midlevel=0` and `preview=true`, maybe `sculpt_smooth` with 1 or 2 iterations, then `sculpt_export`.

## Task 5: aim strokes on a picture, then mirror

This is how Claude sculpts: it looks at a preview, picks spots on the picture, and strokes there with `space=screen`. `sculpt.surface_point` shows what model point a picture spot lands on.

**Batch.**

```text
# screen.txt: strokes aimed on a picture, then the left half copied to the right
sculpt.open path=out/head.stl -> h
# the front picture Claude would look at
sculpt.export doc=$h path=out/screen_before.png view=front size=512
# what's under the middle of the left eye socket?
sculpt.surface_point doc=$h view=front u=0.36 v=0.47
# fine lines need fine triangles: 4x the detail, same shape
sculpt.subdivide doc=$h method=midpoint
# a scar across the left cheek, drawn on the picture (u,v from the top left)
sculpt.stroke doc=$h brush=crease space=screen view=front points=0.25,0.55,0.32,0.62,0.38,0.66 radius=0.08 strength=0.7
# a bump on the left of the forehead, seen from 30 degrees round
sculpt.stroke doc=$h brush=inflate space=screen view=front yaw=30 points=0.3,0.3,0.4,0.25 radius=0.2 strength=1
sculpt.export doc=$h path=out/screen_after.png view=front size=512
# the picture's left is the model's -X side: copy it over +X
sculpt.symmetrize doc=$h axis=x direction=negative
sculpt.export doc=$h path=out/screen_mirrored.png view=front size=512
sculpt.export doc=$h path=out/head_mirrored.stl
```

```sh
$ wetwaresuite batch screen.txt
opened out/head.stl as sculpt1: 20480 triangles, 10242 vertices, size 2 x 2.451 x 2.354 at (-1, -1.451, -1), watertight
...
saved out/screen_before.png
...
surface at -0.3745, -0.9213, 0.2572
point: -0.37448,-0.921336,0.257169
subdivided 20480 -> 81920 triangles
...
crease stroke: 27 dabs moved 316 vertices (up to 0.03474)
...
start: -0.668714,-0.742469,0.0431804
end: -0.320983,-0.980448,-0.251054
inflate stroke: 7 dabs moved 725 vertices (up to 0.116)
...
saved out/screen_after.png
mirrored the -x half: 82400 triangles, 41202 vertices, size 2 x 2.451 x 2.354 at (-1, -1.451, -1), watertight
...
saved out/screen_mirrored.png
saved out/head_mirrored.stl
```

The screen points are densified along the picture and each one is cast onto the surface, so the stroke follows the face's curve. The `yaw=30` stroke is aimed on a picture with the model turned 30 degrees about Z; preview with the same `view`/`yaw`/`pitch` you stroke with. `start` and `end` give the stroke's ends in model space. After `sculpt.symmetrize`, the scar and the bump appear on both sides; the seam down the middle is welded, so the result is still watertight.

**CLI.**

```sh
$ wetwaresuite sculpt.symmetrize input=out/head.stl output=cli/head_sym.stl axis=x direction=positive
mirrored the +x half: 20708 triangles, 10356 vertices, size 2 x 2.451 x 2.354 at (-1, -1.451, -1), watertight
saved cli/head_sym.stl
...
```

**Claude.**

> Open head.stl. Add a scar across its left cheek and a small horn on the left of the forehead, then make the right side match.

Claude calls `sculpt_open` with `preview=true` and `view=front`, reads where the cheek is in the returned picture, calls `sculpt_stroke` with `space=screen`, `view=front` and `u,v` points on that picture (`preview=true` again to check), and finishes with `sculpt_symmetrize axis=x direction=negative`. If it wants exact coordinates for a mask or a world-space stroke, it calls `sculpt_surface_point`.

## Brushes

| Brush | What it does | Notes |
| --- | --- | --- |
| `draw` | Raises the surface along the average normal under the brush (clay buildup). | `invert=true` digs. |
| `inflate` | Pushes each vertex out along its own normal: swells, rounds. | `invert=true` deflates. |
| `smooth` | Moves vertices toward their neighbours' average. | Good after other strokes; strength 0.5 to 1. |
| `flatten` | Pulls vertices onto the plane through the area under the brush. | Planes, cut faces. |
| `pinch` | Pulls vertices sideways toward the dab centre. | Sharpens ridges; `invert=true` spreads. |
| `grab` | Picks up the surface at the first point and drags it along the path. | Moves by `strength x falloff` of the path offset (strength defaults to 1). Not dabbed. |
| `crease` | Pinches and pushes in at once: a sharp groove. | `invert=true` makes a sharp ridge. Keep the radius at least 2 to 3 edge lengths. |

Falloffs: `smooth` (default, soft S-curve), `linear`, `sharp` (concentrated in the middle), `constant` (full strength to the edge).

Per dab, `draw`, `inflate` and `crease` move up to `radius x strength x 0.15`; with the default spacing of 0.25 about four dabs overlap at each spot, so a stroke raises roughly `radius x strength x 0.5`. Run a stroke again (or along the same points twice) to build more.

## Command reference

All editing commands take `doc=` (or `input=`/`output=`) plus `preview=true` to attach a picture of the result, with `view`, `yaw` and `pitch` choosing the angle. They are undoable in batch and MCP sessions.

### Documents

| Command | Parameters | What it does |
| --- | --- | --- |
| `sculpt.new` | `shape` sphere/cube/plane (sphere), `level` (4), `size` (2) | New sculpt from a primitive. Sphere: icosphere, 20 x 4^level triangles, `size` is the diameter. Cube: 12 x 4^level, a grid on each face. Plane: 2 x 4^level in XY facing +Z, with UVs. Read-only (makes a new document). |
| `sculpt.open` | `path` | Opens a model (stl, obj, ply, off, glb, gltf, 3mf, fbx, step) welded for sculpting. Read-only. |
| `sculpt.copy` | `source` | New sculpt from any open mesh (forge, cad, another sculpt), leaving the source alone. Read-only. |
| `sculpt.close` | `doc` | Closes a document. |

### Brushes and masks

| Command | Parameters | What it does |
| --- | --- | --- |
| `sculpt.stroke` | `brush` (draw), `points`, `space` world/screen/ray, `pressures` (none), `radius` (a tenth of the model's diagonal), `strength` 0..1 (0.5; grab 1), `falloff` (smooth), `spacing` (0.25 of the radius), `invert` (false), `symmetry` none/x/y/z/xy/... (none), `view`/`yaw`/`pitch` for screen points | One brush stroke. Returns `dabs`, `moved`, `max_offset`, `radius`, `start`, `end`. At most 4000 dabs. |
| `sculpt.surface_point` | `u`, `v`, `view`, `yaw`, `pitch` | The model point under a spot on a picture. Read-only. |
| `sculpt.mask` | `shape` sphere/plane/all/clear/invert/grow/shrink/blur (sphere), `center`, `radius`, `point` (0,0,0), `normal` (0,0,1), `softness` (0), `mode` set/add/subtract/intersect (set), `steps` (1) | Sets the mask. Sphere: within `radius` of `center`. Plane: the side `normal` points to. Grow/shrink by `steps` edge rings; blur softens. Returns `masked`. Sculpt documents only. |

### Whole mesh

| Command | Parameters | What it does |
| --- | --- | --- |
| `sculpt.smooth` | `iterations` (5, up to 500), `strength` (0.5), `method` taubin/laplacian (taubin) | Smooths everything not masked. Open edges stay put. |
| `sculpt.subdivide` | `method` loop/midpoint (loop), `levels` 1..4 (1) | 4x triangles per level. Loop rounds the shape; midpoint keeps it. Mask carries over. |
| `sculpt.remesh` | `resolution` 4..160 (64), or `voxel_size` | Even quad surface from a voxel grid; fuses overlapping parts. Open meshes become a thin shell. Clears the mask. |
| `sculpt.decimate` | `triangles` or `ratio` | Quadric edge collapse. Keeps open edges and the silhouette, never flips faces, locks masked vertices. |
| `sculpt.retopo` | `quads` (2000), `relax` 0..100 (8) | Automatic quad retopology: sized voxel remesh, relaxed and projected back onto the original. Returns `quads`. |
| `sculpt.symmetrize` | `axis` x/y/z (x), `direction` positive/negative (positive) | Copies one half over the other across the plane through the origin. Clears the mask. |
| `sculpt.displace` | `image` (file or open image id), `amount` (0.1), `midlevel` (0.5), `mapping` auto/uv/planar/cylindrical/spherical (auto), `axis` (z), `tile` (1) | Moves vertices along their normals by `amount x (brightness - midlevel)`. Auto uses UVs if the mesh has them, else planar. |

### Looking and saving

| Command | Parameters | What it does |
| --- | --- | --- |
| `sculpt.stats` | `doc` | Triangles, vertices, edges, open and non-manifold edges, edge lengths, size, area, volume (closed meshes), Euler characteristic, masked vertices, quads. Read-only. |
| `sculpt.preview` | `view` (iso), `yaw`, `pitch`, `size` (512), `outline` (false), `flat` (false) | A PNG of the model from any angle (attached; over MCP Claude sees it). Read-only. |
| `sculpt.export` | `path`, `normals` (true), `ascii` (false), `size` (1024, pictures), `view`/`yaw`/`pitch` | Writes stl, obj, ply, glb, gltf, off or 3mf by extension, or a png/jpg picture. OBJ keeps quads after remesh or retopo. |

Views: `front` looks along +Y at the -Y side, `back` the opposite, `left` looks along +X, `right` along -X, `top` down, `bottom` up, `iso` from the front-right-top. `yaw` turns the model counter-clockwise (seen from above) about Z before the view; `pitch` tilts it toward the camera.

## Over MCP

Every command is an MCP tool with `.` replaced by `_` (`sculpt_stroke`, `sculpt_retopo`, ...), plus `undo`, `redo`, `history` and `documents`. Calling any editing tool with `preview=true` returns the picture alongside the result, so Claude sees each stroke land. `sculpt_new`, `sculpt_open`, `sculpt_copy`, `sculpt_stats`, `sculpt_preview` and `sculpt_surface_point` are marked read-only; `sculpt_export` and the file forms (`input=`/`output=`) write files.

A good loop for Claude: preview from the angle it will work in, stroke with `space=screen` and the same view, check the returned picture, undo if needed.

## Limitations

- **Sizes are capped.** Results stop at 1 000 000 triangles (`sculpt.new`, `sculpt.subdivide`, `sculpt.remesh`, `sculpt.retopo`) and voxel grids at 160 cells along the longest side. For more detail, remesh at 160 and subdivide once.
- **Brushes are simple.** No dynamic topology (triangles stretch under big grabs: remesh afterwards), no textured alphas, no layers. Every dab scans all vertices, so very dense meshes (several hundred thousand vertices) stroke slowly; a typical sculpt at 20 000 to 80 000 triangles is instant.
- **Symmetry is about the origin planes**, not the model's centre: centre the model first (`forge.mesh_transform center=true`). A dab closer to the mirror plane than its radius overlaps its own mirror image, so the area along the plane gets a double dose: use a lower strength there.
- **Remesh and retopo lose detail smaller than a cell**, and features thinner than about two cells (ear tips, thin creases) come out lumpy. Retopology is automatic: its edge loops follow the voxel grid, not the face's features like hand retopology would. Open meshes remesh into a thin closed shell, and retopo on them is not projected back.
- **Masks aren't shown in pictures**; `sculpt.stats` and `sculpt.mask` report how many vertices are masked. Topology changes (remesh, retopo, symmetrize) clear the mask.
- **UVs and normals.** Opening a model drops its normals, and its UVs when welding had to merge seams; subdivision keeps UVs, every other topology change drops them. Make new UVs with `forge.mesh_uv` when you're done.
- **No colour, vertex paint or textures** are kept or exported; exports are geometry only.
