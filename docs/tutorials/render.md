# Render: 3D scenes and pictures

`render` turns 3D models into pictures, headless. A **scene** holds objects (primitives such as boxes, spheres and tori, or meshes from model files and from open cad/forge documents) with positions and materials, lights, a sky and a camera. It renders two ways:

- **path** (the default): a CPU path tracer. Soft shadows, reflections, glass, glowing objects, image textures and light bouncing between surfaces, multithreaded, with a denoiser so 16 samples per pixel already look clean. A small scene at 640 x 480 takes a couple of seconds.
- **preview**: a z-buffer rasterizer with ray-traced shadows. Fractions of a second; what you use while placing things, and what Claude looks at after every edit.

Scenes save as JSON (`.scene.json`) that you can read and edit by hand. Turntables write a folder of frames and, when ffmpeg is installed, a GIF or MP4.

Each task below is shown three ways:

- **CLI**: single `wetwaresuite` commands. A scene only lives inside one process, so one-shot commands either work on files (`render.model path=part.stl`) or read a saved scene with `scene=file.scene.json`.
- **Batch**: a script for multi-step work with `-> name` captures and `$name` reuse (see [headless and batch](headless-and-batch.md)).
- **Claude**: a prompt for Claude over MCP and the tools it calls (see [mcp.md](mcp.md)). `wetwaresuite mcp --tools render` serves render plus forge (so Claude can open model files with `forge_mesh_open`) and photo.

Every example below was run against the real binary in an empty folder holding `plate.stl` (a 60 x 40 x 8 mm plate with four holes, made with [cad](cad.md)) and `tiles.png` (a 256 x 256 checkerboard). Outputs are copied from those runs; repeated fields are cut where marked `...`.

## How render works

**Z is up**, as everywhere in the suite. Units are whatever your models use: primitives default to about one unit across, a model in millimetres is tens or hundreds of units, and the camera, ground and default lights adapt to the scene's size.

**Every object's origin is the middle of its base.** A sphere at `position=0,0,0` sits on the ground instead of being half buried; a box at `position=2,0,0` stands on the floor two units to the right. Models from files are moved the same way when added (`center=false` keeps the file's own origin).

**A new scene is ready to render.** `render.new` gives you a sun, a gradient sky and a ground plane that sizes itself to whatever you add. The camera frames everything automatically at render time until you place it yourself with `render.camera`, so you can add objects and render without thinking about the camera at all.

**Names.** Objects and lights have names (`sphere1`, `crate`, `sun`); commands take `name=` to say which one to change. `name=*` in `render.material` changes every object.

**Undo.** Every edit (`add`, `remove`, `transform`, `material`, `light`, `sky`, `camera`, `settings`) is one undo step; `render`, `preview`, `info` and `list` change nothing.

**Colours** are `#rrggbb`, `#rgb`, names such as `red`, `rgb(200,40,40)`, or three linear floats `0.8,0.1,0.1` (those may go above 1 for very bright lights). Scene files store them as hex.

## Task 1: a picture of a model, in one command

You have `plate.stl` and want a presentable image of it.

**CLI.** `render.model` puts the model on a ground plane under a sun and sky, gives it a material, frames it and renders:

```sh
$ wetwaresuite render.model path=plate.stl output=plate.png preset=brushed
rendered plate.stl (640x480 path, 16 spp, 4 bounces) in 2.39s to plate.png
path: plate.png
bytes: 106797
width: 640
height: 480
mode: path
seconds: 2.39
triangles: 1054
threads: 4
samples: 16
bounces: 4
```

(1054 triangles: the model's 1052 plus the two of the ground plane.)

A quick look from straight above, rasterized, as a JPEG:

```sh
$ wetwaresuite render.model path=plate.stl output=plate_quick.jpg mode=preview view=top width=400
rendered plate.stl (400x300 preview) in 0.1282s to plate_quick.jpg
...
```

`preset` is any material preset (`clay` by default; see [materials](#materials)), `color=` tints it, `view=` is `iso`, `front`, `back`, `left`, `right`, `top`, `bottom` or `low` (a low three-quarter view), `y_up=true` stands up models exported Y-up (most OBJ/STL game assets), `ground=false` floats it in the sky. All the [render parameters](#render-parameters) apply too (`samples`, `width`, `exposure`, ...).

**Batch.** The same thing as a scene you can keep editing:

```text
# model.txt
render.new -> s
render.add doc=$s path=plate.stl preset=brushed
render.render doc=$s output=plate.png
```

**Claude.** *"Render plate.stl as a brushed metal part and show me."* Claude calls `render_model` with `path=plate.stl preset=brushed` and no `output`, so the PNG comes back in the tool result and it can look at it before writing a file.

## Task 2: a still life with different materials

Five objects in five materials, a textured crate, saved as a scene, rendered as a quick preview, a normal render and a high-quality one.

**Batch** (`stilllife.txt`):

```text
# stilllife.txt: a few objects with different materials, saved as a scene and rendered
render.new width=800 height=500 -> s
render.add doc=$s type=sphere name=ball radius=0.6 preset=gold
render.add doc=$s type=box name=crate size=1 position=1.5,0.4,0 rotation=0,0,20 texture=tiles.png color=white roughness=0.6
render.add doc=$s type=sphere name=marble radius=0.35 position=-1.1,-0.6,0 preset=glass
render.add doc=$s type=torus name=ring radius=0.45 radius2=0.12 position=-0.3,-1.3,0 color=#d03030 preset=plastic
render.add doc=$s type=cylinder name=post radius=0.25 height=1.6 position=-1.4,0.9,0 preset=ceramic
render.material doc=$s name=ground color=#9a9a96 roughness=0.8
render.list doc=$s
render.save doc=$s path=stilllife.scene.json
render.render doc=$s output=stilllife_preview.png mode=preview
render.render doc=$s output=stilllife.png
render.render doc=$s output=stilllife_hq.jpg samples=128 bounces=6
```

```sh
$ wetwaresuite batch stilllife.txt
new scene scene1: 1 objects (1 visible), 1 lights, 800x500, camera auto
...
added ball: sphere r0.6 at 0,0,0; colour #ffdb93, roughness 0.2, metallic 1
...
added crate: box 1,1,1 at 1.5,0.4,0 rot 0,0,20; colour #ffffff, roughness 0.6, texture tiles.png
...
ground: plane (auto size) at 0,0,0; colour #9a9a96, roughness 0.8
ball: sphere r0.6 at 0,0,0; colour #ffdb93, roughness 0.2, metallic 1
crate: box 1,1,1 at 1.5,0.4,0 rot 0,0,20; colour #ffffff, roughness 0.6, texture tiles.png
marble: sphere r0.35 at -1.1,-0.6,0; colour #ffffff, roughness 0, glass 1 (ior 1.5)
ring: torus r0.45 at -0.3,-1.3,0; colour #d03030, roughness 0.35
post: cylinder r0.25 h1.6 at -1.4,0.9,0; colour #f3f3f1, roughness 0.15
sun: sun light at -5,-2,6 towards 0,0,0, #fff9f1 x4, 2 deg
doc: scene1
objects: ground,ball,crate,marble,ring,post
lights: sun
saved stilllife.scene.json
...
rendered scene1 (800x500 preview) in 0.5737s to stilllife_preview.png
...
rendered scene1 (800x500 path, 16 spp, 4 bounces) in 4.32s to stilllife.png
...
rendered scene1 (800x500 path, 128 spp, 6 bounces) in 30.06s to stilllife_hq.jpg
```

Things to notice:

- **Presets first, then adjust.** `preset=plastic color=#d03030` takes the plastic look and paints it red. Params given with a preset override it.
- **Textures** multiply into the colour, so use `color=white` to see the image as it is. Boxes and other meshes without UVs get each face mapped from its closest side; spheres get a spherical mapping and cylinders a cylindrical one; models with UVs (OBJ `vt`, glTF) use their own. `texture_scale=4` repeats it four times.
- **The ground is an object called `ground`**: change its material, hide it (`render.transform name=ground visible=false`) or remove it.
- **16 samples** (the default) with denoising is the quick-look quality; 64 to 256 samples is the final-picture quality. Time grows with samples, pixels and bounces roughly linearly.

**CLI.** A saved scene renders in one command, with any setting overridden:

```sh
$ wetwaresuite render.render scene=stilllife.scene.json output=stilllife_bright.jpg exposure=0.5 width=400
rendered stilllife.scene.json (400x250 path, 16 spp, 4 bounces) in 0.4792s to stilllife_bright.jpg
...

$ wetwaresuite render.info scene=stilllife.scene.json
6 objects (6 visible), 1 lights, 800x500, camera auto, stilllife.scene.json
6926 triangles, bounds -1.65,-1.87,0 to 2.141,1.15,1.6
camera at 4.491,-6.021,3.984 looking at 0.2454,-0.36,0.8, fov 40 (auto)
sky gradient, strength 0.5
render path, 16 spp, 4 bounces
...
```

Giving `width` alone keeps the scene's aspect ratio (800 x 500 becomes 400 x 250).

**Claude.** *"Make a still life: a gold ball, a glass marble, a red plastic ring, a white ceramic post and a crate with tiles.png on it. Show me a preview after each object, then render it at 128 samples to stilllife.jpg and save the scene."* Claude calls `render_new`, then `render_add` for each object with `preview=true` (each result carries a rasterized PNG it can look at), `render_material` to fix anything that looks wrong, `render_render` with `samples=128 output=stilllife.jpg`, and `render_save`.

## Task 3: a product shot with studio lights and a transparent background

For a web shop or a slide: the part alone, softly lit, on a see-through background.

**Batch** (`studio.txt`):

```text
# studio.txt: a product shot of plate.stl with soft studio lights and a see-through background
render.new sun=false ground=false width=800 height=600 -> s
render.add doc=$s path=plate.stl name=plate preset=plastic color=#2f6fb5
render.light doc=$s type=area name=key position=-60,-70,80 target=0,0,0 width=60 height=60 intensity=40000
render.light doc=$s type=area name=rim position=70,60,40 target=0,0,0 width=30 height=80 intensity=15000 color=#cfe0ff
render.sky doc=$s type=color color=#ffffff strength=0.3 transparent=true
render.camera doc=$s view=iso fov=30
render.render doc=$s output=plate_studio.png samples=32
```

```sh
$ wetwaresuite batch studio.txt
new scene scene1: 0 objects (0 visible), 0 lights, 800x600, camera auto
...
added plate: mesh (1052 triangles, plate.stl) at 0,0,0; colour #2f6fb5, roughness 0.35
...
added key: area light at -60,-70,80 towards 0,0,0, #ffffff x4e+04, 60x60
...
added rim: area light at 70,60,40 towards 0,0,0, #cfe0ff x1.5e+04, 30x80
...
sky: color #ffffff, strength 0.3, transparent
...
camera at 78.23,-104.3,69.19 looking at 0,0,4, fov 30
...
rendered scene1 (800x600 path, 32 spp, 4 bounces) in 1.663s to plate_studio.png
```

How the light numbers work:

- **Point and area lights fall off with distance squared.** `intensity` is roughly "brightness times distance squared": a light 120 units away needs about 120 x 120 x 2.5 = 36 000 to light a surface as brightly as the default sun. For a scene a few units across, 50 to 200 is typical. `render.light type=point` or `type=area` with no position places the light above and in front of the scene, with an intensity scaled to its size, which is a good starting point.
- **Sun intensity is the same everywhere** (4 is daylight); only its direction (from `position` towards `target`) matters.
- **The sky lights the scene too.** `strength` scales both how bright it looks and how much light it gives; `type=none` leaves only your lights.
- `transparent=true` makes the background see-through in PNGs (JPEGs can't have alpha; they get the background colour). `visible=false background=#ffffff` keeps the sky's light but shows a flat colour behind the objects instead.

**Claude.** *"Make a product shot of plate.stl in blue plastic with a big soft key light and a cool rim light, transparent background, 800 x 600."* Claude calls `render_new` (`sun=false ground=false`), `render_add` with `path=plate.stl`, two `render_light` calls with `type=area`, `render_sky` with `transparent=true`, `render_preview` to check the lighting, then `render_render`.

## Task 4: relight a saved scene as a sunset

Open the still life, move the sun low and orange, darken the sky, add a lamp, make the gold ball glow a little, look from low down, and save it as a second scene.

**Batch** (`sunset.txt`):

```text
# sunset.txt: reopen the still life, relight it as a sunset and save a second scene
render.open path=stilllife.scene.json -> s
render.light doc=$s name=sun position=-8,6,1.2 color=#ff9a50 intensity=5 radius=1
render.sky doc=$s zenith=#304a80 horizon=#f0a070 ground=#3a3230 strength=0.6
render.light doc=$s type=point name=lamp position=1,-2,2.5 color=#ffd8a0 intensity=6 radius=0.1
render.material doc=$s name=ball emission=#ffb060 emission_strength=0.5
render.camera doc=$s view=low
render.save doc=$s path=sunset.scene.json
render.render doc=$s output=sunset.png samples=32
```

```sh
$ wetwaresuite batch sunset.txt
opened stilllife.scene.json as scene1: 6 objects (6 visible), 1 lights, 800x500, camera auto, stilllife.scene.json
...
changed sun: sun light at -8,6,1.2 towards 0,0,0, #ff9a50 x5, 1 deg
...
sky: gradient #304a80 to #f0a070, strength 0.6
...
added lamp: point light at 1,-2,2.5, #ffd8a0 x6, radius 0.1
...
ball: colour #ffdb93, roughness 0.2, metallic 1, glows #ffb060 x0.5
...
camera at 4.85,-6.499,1.951 looking at 0.2454,-0.36,0.8, fov 40
...
saved sunset.scene.json
...
rendered scene1 (800x500 path, 32 spp, 4 bounces) in 4.386s to sunset.png
```

`render.light` edits a light when the name exists (`sun`) and adds one when it doesn't (`lamp`). A sun's `radius` is its size in degrees: bigger means softer shadows. A point light's `radius` is its bulb size, which softens its shadows the same way.

**CLI.** Small changes are quickest in the scene file itself. Its layout:

```json
{
  "format": "wetwaresuite-scene",
  "version": 1,
  "camera": {
    "position": [6.0, -8.0, 5.0],
    "target": [0.0, 0.0, 0.5],
    "up": [0.0, 0.0, 1.0],
    "fov": 40.0,
    "orthographic": false,
    "ortho_size": 5.0,
    "auto": true
  },
  "settings": {"width": 800, "height": 500, "samples": 16, "bounces": 4, "denoise": true, "exposure": 0.0, "mode": "path"},
  "sky": {"type": "gradient", "zenith": "#95b8e7", "horizon": "#e7eff9", "ground": "#817c76", "strength": 0.5,
          "visible": true, "background": "#f9f9fa", "transparent": false},
  "objects": [
    {
      "name": "crate",
      "type": "box",
      "size": [1.0, 1.0, 1.0],
      "position": [1.5, 0.4, 0.0],
      "rotation": [0.0, 0.0, 20.0],
      "scale": [1.0, 1.0, 1.0],
      "material": {"color": "#ffffff", "roughness": 0.6, "metallic": 0.0, "texture": "tiles.png", "texture_scale": 1.0}
    }
  ],
  "lights": [
    {"name": "sun", "type": "sun", "position": [-5.0, -2.0, 6.0], "target": [0.0, 0.0, 0.0],
     "color": "#fff9f1", "intensity": 4.0, "radius": 2.0}
  ]
}
```

(shortened: the real file lists every object, one field per line). Paths to textures and meshes are relative to the scene file. Missing fields take their defaults, a material may say `"preset": "glass"`, a light may give `"direction": [x, y, z]` instead of a target, and `"auto": true` on the camera means "frame everything", ignoring position and target. Edit, then `wetwaresuite render.render scene=sunset.scene.json output=sunset.png`.

**Claude.** *"Open stilllife.scene.json and make it a warm sunset with a low sun and a small lamp; keep the materials. Save it as sunset.scene.json."* Claude calls `render_open`, `render_light` (twice), `render_sky`, `render_camera` with `view=low`, `render_preview` to check, then `render_save` and `render_render`.

## Task 5: render a cad part, with a model from forge

Objects can come straight from documents other tools have open: `mesh=` takes the id of any mesh document, such as a part being designed in [cad](cad.md) or a model opened with [forge](forge.md).

**Batch** (`cadshot.txt`):

```text
# cadshot.txt: render a cad part straight from the cad document, next to a model opened with forge
cad.new -> part
cad.cylinder doc=$part name=body radius=20 height=30
cad.hole doc=$part target=body diameter=24
forge.mesh_open path=plate.stl -> plate
render.new -> s
render.add doc=$s mesh=$part name=bushing preset=copper position=0,0,8
render.add doc=$s mesh=$plate name=plate color=#3a3f47 preset=plastic
render.camera doc=$s view=iso fov=35
render.save doc=$s path=cadshot.scene.json
render.render doc=$s output=cadshot.png
```

```sh
$ wetwaresuite batch cadshot.txt
...
added bushing: mesh (512 triangles) at 0,0,8; colour #f9d1c2, roughness 0.25, metallic 1
...
added plate: mesh (1052 triangles) at 0,0,0; colour #3a3f47, roughness 0.35
...
camera at 75.64,-100.9,82.04 looking at 0,0,19, fov 35
...
saved cadshot.scene.json
...
rendered scene1 (640x480 path, 16 spp, 4 bounces) in 1.017s to cadshot.png
```

The scene keeps a copy of each mesh as it was when added (change the part in cad, then `render.remove` and `render.add` it again). Meshes that came from documents have no file of their own, so `render.save` writes them beside the scene as `cadshot.bushing.glb` and `cadshot.plate.glb`; the scene file points at those.

**Claude.** *"Show me the bracket I'm designing in cad1 as a brushed aluminium render."* Claude calls `render_new`, `render_add` with `mesh=cad1 preset=brushed`, and `render_render` without `output` to see it.

## Task 6: a turntable GIF or MP4

**CLI.** The camera circles the scene about the vertical axis through what it's looking at; frames go to a folder, and `video=` encodes them with ffmpeg:

```sh
$ wetwaresuite render.turntable scene=stilllife.scene.json output_dir=spin frames=24 width=400 video=spin.gif
rendered 24 frames (400x250 preview) to spin in 6.092s, video spin.gif
doc: stilllife.scene.json
output_dir: spin
frames: 24
video: spin.gif
bytes: 262474
...

$ wetwaresuite render.turntable scene=stilllife.scene.json output_dir=spin_hq frames=48 mode=path samples=8 width=480 video=spin.mp4 fps=24
rendered 48 frames (480x300 path, 8 spp, 4 bounces) to spin_hq in 37.53s, video spin.mp4
...
```

The frames are `spin/frame_000.png`, `frame_001.png`, ... (`format=jpg` for JPEGs). Turntables use the fast preview renderer unless you say `mode=path`; a path-traced turntable costs one render per frame, so keep `samples` low (8 is fine for motion) and the size modest. `degrees=90` swings a quarter turn instead of a full one. Video needs ffmpeg on PATH (`.mp4`, `.mov`, `.mkv` as H.264, `.webm` as VP9, `.gif` with an optimised palette); without it, leave out `video=` and you still get the frames.

**Batch.**

```text
render.open path=sunset.scene.json -> s
render.turntable doc=$s output_dir=sunset_spin frames=36 mode=path samples=8 width=480 video=sunset.mp4
```

**Claude.** *"Make a 3-second turntable GIF of the sunset scene."* Claude calls `render_open` then `render_turntable` with `frames=36 fps=12 video=sunset.gif` (`preview=true` returns the first frame so it can check the framing first).

## Placing things

```text
render.transform doc=$s name=crate position=2,0,0          # put it somewhere
render.transform doc=$s name=crate move=0,0.5,0            # nudge it
render.transform doc=$s name=crate rotation=0,0,45         # absolute angles (x, then y, then z)
render.transform doc=$s name=crate turn=0,0,15             # add to the angles
render.transform doc=$s name=crate scale=2                 # or scale=1,1,3
render.transform doc=$s name=crate visible=false           # hide without deleting
render.transform doc=$s name=key position=0,-5,5 target=0,0,1   # lights move too
render.remove doc=$s name=crate
```

Scale and rotation pivot on the object's origin, the middle of its base, so scaled objects stay on the ground. To put one object on another, move it up by the height of the one below (`position=0,0,1` puts something on top of a 1-unit box).

## Cameras

```text
render.camera doc=$s view=front                  # look from the front (-Y), framed
render.camera doc=$s orbit=30                    # swing 30 degrees around the target
render.camera doc=$s position=4,-6,2 target=0,0,0.5 fov=30
render.camera doc=$s fov=20 fit=true             # telephoto, re-framed from the same direction
render.camera doc=$s orthographic=true fit=true  # parallel projection, like a technical drawing
render.camera doc=$s auto=true                   # back to automatic framing
```

Views: `iso` (front-right, above), `front`, `back`, `left`, `right`, `top`, `bottom`, `low` (front-right, close to the ground). A narrow `fov` (20 to 30) flattens perspective like a long lens and suits product shots; a wide one (60 to 90) exaggerates it. `orthographic=true` uses `ortho_size` as the visible height.

## Materials

| preset | looks like | colour kept |
| --- | --- | --- |
| `matte` | flat paint, paper | yes |
| `plastic` | semi-gloss plastic | yes |
| `glossy` | lacquer, glazed ceramic | yes |
| `metal` | metal tinted by the colour | yes |
| `brushed` | brushed aluminium | no |
| `chrome` | polished chrome | no |
| `mirror` | a perfect mirror | no |
| `gold`, `copper` | polished gold, copper | no |
| `glass` | clear glass | no (set `color` after for tinted glass) |
| `frosted` | frosted glass | no |
| `rubber` | black rubber | no |
| `emissive` | glows in its own colour | yes |
| `ceramic` | white porcelain | no |
| `clay` | white clay (a neutral look for shapes) | no |

The material fields, for `render.add`, `render.material` and the scene file:

| param | range | meaning |
| --- | --- | --- |
| `color` | colour | base colour; tints reflections for metals and the light through glass |
| `roughness` | 0..1 | 0 is mirror-smooth, 1 fully rough |
| `metallic` | 0..1 | 1 makes reflections take the colour (metals) |
| `emission`, `emission_strength` | colour, 0.. | glow; strength 1 to 10 is typical, and glowing objects light their surroundings in path mode |
| `transmission` | 0..1 | 1 is glass |
| `ior` | 1..3 | glass refraction: 1.33 water, 1.5 glass, 2.4 diamond |
| `opacity` | 0..1 | below 1 lets light straight through, for cut-outs and ghosts |
| `texture`, `texture_scale` | image file, repeats | image multiplied into the colour; `texture=""` removes it |
| `normal_map`, `normal_strength` | image file, 0..4 (1) | tangent-space normal map (green up, as Blender and glTF bake them) for fine bumps, scratches and weave; uses the same UVs and `texture_scale` |
| `roughness_map` | image file | grey image multiplied into `roughness` (glTF metal-rough maps work too: the green channel is read); white keeps the roughness, black makes that spot mirror-smooth |

**Models with several materials.** `render.add path=chair.obj` reads the OBJ's `.mtl` too: each `usemtl` group becomes a *part* with its own material (`Kd` colour, `Ns`/`Pr` roughness, `Pm` metallic, `Ke` glow, `d` opacity, `Ni` ior, `map_Kd` texture, `norm`/`map_Bump` normal map, `map_Pr` roughness map). `render.info` and the add message list them (`2 materials: 1 red, 2 blue`). `render.material name=chair part=blue roughness=0.7` changes one part (by name or number); without `part=` every part gets the change. Saved scenes keep your edits to each part. Other formats still come in with one material (open them in [model](model.md) to keep theirs).

```text
render.add doc=$s path=two.obj name=box
render.material doc=$s name=box part=blue roughness=0.4
```

```text
added box: mesh (12 triangles, two.obj) at 0,0,0; 2 materials: 1 red, 2 blue
box / blue: colour #1a33e6, roughness 0.4, metallic 1
```

**HDRI skies.** `render.sky hdri=studio.jpg` lights the scene from a 2:1 equirectangular panorama (any image the suite reads) and shows it behind the objects; `rotation=90` turns it about the up axis and `strength` scales it. Bright parts (the sun, windows, softboxes) cast the light and the reflections you see in metals and glass, which is what makes product shots look real. 8-bit panoramas clip the sun, so the brightest pixels are boosted a little to keep its shadows.

```text
render.sky doc=$s hdri=sky.png rotation=30
```

```text
sky: hdri sky.png turned 30 deg, strength 0.5
```

## Command reference

All commands take `doc=` (an open scene id). `render.render`, `render.preview`, `render.turntable`, `render.info` and `render.list` also accept `scene=file.scene.json` instead, which is how they work as one-shot CLI commands. Edits take `preview=true` to attach a quick rasterized PNG of the result.

### render.new

| param | default | meaning |
| --- | --- | --- |
| `ground` | true | add a ground plane (auto-sized) |
| `sun` | true | add a sun |
| `width`, `height` | 640, 480 | render size (also the camera's aspect) |

### render.open, render.save, render.close

`render.open path=` reads a `.scene.json`; `render.save path=` writes one (default: where it came from), plus `.glb` files for meshes that came from documents; `render.close` forgets a scene.

### render.add

| param | default | meaning |
| --- | --- | --- |
| `type` | box (mesh with `path`/`mesh`) | `box`, `sphere`, `cylinder`, `cone`, `torus`, `plane`, `mesh` |
| `name` | type + number | object name |
| `path` | | model file (stl, obj, ply, off, glb, gltf, 3mf, fbx, step) |
| `mesh` | | id of an open mesh document (cad, forge) |
| `center` | true | move a model so the middle of its base is its origin |
| `size` | 1,1,1 | box x,y,z (one number for a cube); plane x,y (omit for the endless ground size) |
| `radius` | 0.5 | sphere, cylinder and cone radius; torus ring radius |
| `radius2` | 0 / radius/4 | cone top radius; torus tube radius |
| `height` | 1 | cylinder and cone height |
| `segments` | 48 | facets around round primitives |
| `position`, `rotation`, `scale` | 0,0,0 / 0,0,0 / 1 | placement (degrees about x, then y, then z) |
| `smooth` | true | smooth shading; false shows facets |
| material params | | as in [materials](#materials), including `preset` |

### render.remove, render.transform, render.material

`render.remove name=` deletes an object or light. `render.transform name=` takes `position`, `rotation`, `scale`, `move`, `turn`, `visible` (objects) or `position`, `move`, `target` (lights). `render.material name=` (or `*`) takes the material params, and `part=` (a name or number) for one material of a multi-material model.

### render.light

| param | default | meaning |
| --- | --- | --- |
| `name` | new light | light to change, or the name for a new one |
| `type` | point | `point`, `sun`, `area` |
| `position`, `target` | placed for the scene | where it is and what it points at (sun and area) |
| `direction` | | sun/area: the way the light travels, instead of `target` |
| `color` | white | light colour |
| `intensity` | 100 (sun 4) | brightness; point and area fall off with distance squared |
| `radius` | 0.1 (sun 2) | point: bulb size; sun: size in degrees; both soften shadows |
| `width`, `height` | 1 | area light size |
| `shadows` | true | cast shadows |

### render.sky

| param | default | meaning |
| --- | --- | --- |
| `type` | gradient | `gradient`, `color`, `none`, `hdri` |
| `hdri` | | 2:1 panorama image; sets type `hdri` and shows it behind the objects |
| `rotation` | 0 | degrees to turn the hdri about the up axis |
| `zenith`, `horizon`, `ground` | blue, pale blue, brown-grey | gradient colours (up, horizon, below) |
| `color` | | one colour (sets type `color`) |
| `strength` | 0.5 | how bright the sky looks and how much it lights the scene |
| `visible` | true | false shows `background` behind the objects instead |
| `background` | near white | the colour behind objects when the sky is hidden (setting it hides the sky) |
| `transparent` | false | transparent background in PNGs |

### render.camera

`position`, `target`, `up` (0,0,1), `fov` (40), `orthographic`, `ortho_size`, `view`, `fit`, `auto`, `orbit` (degrees about the target), `width`, `height`. Setting a position, target, view, orbit or fit turns automatic framing off.

### render.settings

Defaults stored in the scene for `render.render`: `width`, `height`, `samples` (16), `bounces` (4), `denoise` (true), `exposure` (0), `mode` (path).

### render.list, render.info

`render.list` prints every object and light with its type, size, place and material (`objects` and `lights` fields list the names). `render.info` gives counts, triangles, bounds, the camera (including where the automatic camera will be), the sky and the render settings.

### render.preview

`mode` (`preview` or `path`), `size` (longest side, 512), `samples` (8, path mode). Returns a PNG and writes nothing.

### render.render

`output` (png, jpg, bmp, tga, ppm; omit to get the PNG back), `quality` (JPEG, 92), `preview` (attach the PNG as well), and the render parameters.

### render.turntable

`output_dir` (required), `frames` (36), `degrees` (360), `format` (png/jpg), `video` (.mp4, .mov, .mkv, .webm, .gif; needs ffmpeg), `fps` (24), and the render parameters (`mode` defaults to `preview` here).

### render.model

`path` (required), `output`, `preset` (clay), `color`, `view` (iso), `ground` (true), `y_up` (false), `quality`, and the render parameters.

### Render parameters

| param | default | meaning |
| --- | --- | --- |
| `mode` | path | `path` (path tracer) or `preview` (rasterizer) |
| `width`, `height` | scene's (640 x 480) | image size; `width` alone keeps the aspect |
| `samples` | 16 | path: samples per pixel; more is less noise and slower |
| `bounces` | 4 | path: how many times light bounces; glass needs 4 or more, 1 or 2 is faster and darker |
| `denoise` | true | path: smooth the remaining noise, guided by edges, normals and depth |
| `exposure` | 0 | brightness in stops: +1 doubles, -1 halves |
| `shadows` | true | preview: ray-traced shadows |
| `antialias` | true | preview: 2x supersampling |
| `threads` | 0 (all cores) | threads to use |
| `seed` | 1 | path: noise pattern; renders are repeatable for the same seed |

## MCP

`wetwaresuite mcp --tools render` serves the render tools (`render_new`, `render_add`, ..., `render_render`) plus forge and photo. Every edit takes `preview=true`, and `render_preview` returns a picture without writing anything, so Claude can check its work after each step. Open scenes are MCP resources too (`wetwaresuite://doc/scene1`), with a rasterized preview.

Example prompts:

- *"Render every STL in this folder as a white clay picture from the front."* Claude lists the folder and calls `render_model` with `view=front output=<name>.png` for each.
- *"Put a red glossy sphere on a mirror floor under a blue sky and render it at 1280 x 720 with 64 samples."* `render_new`, `render_add` (sphere, `preset=glossy color=red`), `render_material name=ground preset=mirror`, `render_render`.
- *"The shadow is too hard and the picture is too dark."* `render_light name=sun radius=5`, then `render_render exposure=0.7`.

## Limitations

- **Speed first.** The path tracer is a straightforward CPU one: no GPU, no adaptive sampling. Big images at hundreds of samples take minutes; the denoiser is a simple edge-aware blur, so very low sample counts look painterly and fine shadows can soften.
- **Lights are invisible.** Point, sun and area lights light the scene but don't appear in the picture or in mirrors; for a visible light source, add an object with an `emissive` material (glowing objects light the scene in path mode, but they are noisier than real lights).
- **No caustics.** Shadows of glass are lighter and tinted but don't focus light; the light through glass onto surfaces behind it is approximate.
- **The preview is a sketch.** It shows materials roughly (no refraction, simple reflections of the sky, glass drawn see-through), and emissive objects don't light anything.
- **Materials from files.** OBJ files keep their `.mtl` materials (one part each); glTF, FBX and other formats come in with one material, and vertex colours are ignored. Texture alpha isn't used for cut-outs (use `opacity`), and there are no metallic, emission or displacement maps.
- **No animation** beyond turntables (camera orbits). Objects can't move between frames yet.
- **Meshes are copied in.** A scene keeps the geometry from when it was added; reopen the file or re-add the document after changing it.
- **No depth of field, motion blur, volumes (fog) or subsurface scattering.** HDRI skies read 8-bit images (PNG, JPEG ...); `.hdr`/`.exr` floating-point panoramas aren't read yet, so convert them first.
