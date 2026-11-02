# 3D view: look at a model from any angle

Every 3D document (a cad part, a mesh in the model app or asset forge, a sculpt, a scan fit, a print in the slicer) can be turned around and looked at from any angle. In the window it's the 3D view you drag around; from the command line, a batch script or Claude it's one command, `app.view`, which draws the same picture.

It's a small software renderer (orthographic, shaded, with outlines on edges), so it runs on any computer without a graphics card.

## How the camera works

- **yaw** turns around the vertical axis, in degrees. 0 looks at the front, 90 from the right, 180 from the back, -90 (or 270) from the left.
- **pitch** tilts up and down: 0 is level, 35 is the classic iso angle, 90 looks straight down (it stops just short of 90).
- **zoom**: 1 fits the whole model, 2 is twice as big.
- **pan**: shifts the model on screen, measured in "model radii" (half the size of the model), so `pan_x=1` moves it right by about half its own width.

The model is fitted by its bounding sphere, so it stays the same size while you turn it.

## Task 1: turn a part around in the window

Open a part in the cad app (`wetwaresuite-cad bracket.wwcad`, or double-click it):

![a cad part in the 3D view](images/gui-cad-3d-view.jpg)

- **Drag** to turn it. Left-right turns around the vertical axis, up-down tilts.
- **Right-drag** or **middle-drag** (or **Shift+drag**) moves it. **Scroll** zooms.
- **Iso**, **Front**, **Right**, **Top** jump to those angles. **Fit** or a **double-click** shows the whole thing.
- In the model app the **shading** combo picks Material, Solid, Clay (matcap), Wireframe or X-ray (see-through, Alt+Z), like Blender's viewport shading. Commands take the same choice as `shading=` for their previews, and `app.view shading=` sets it from a script.
- The bottom-left corner shows the yaw, pitch and zoom, so you can ask for the same picture with `app.view`.

Edits keep the angle, so you can watch a fillet or a hole appear from the side you're working on.

Cad parts and models also take the mouse: click a face of a part to push/pull it with an arrow, move it or sketch on it, and drag the X, Y, Z arrows of a model's selection to move it. See the [cad](cad.md#in-the-window-work-right-on-the-part) and [model](model.md) tutorials.

## Task 2: a picture from a given angle

**CLI.** `app.view` works on an open document, so use a batch script (documents only live inside one process):

```text
cad.open path=bracket.wwcad -> c
app.view doc=$c yaw=30 pitch=10 output=view1.png
app.view doc=$c view=top zoom=1.5 output=view2.png width=400 height=400
```

```sh
$ wetwaresuite batch views.txt
opened cad1 (7 features, 7 params, 1 bodies, mm, 1328 triangles, 658 vertices, size 50 x 40 x 45 at (0, 0, 0), watertight, bracket.wwcad)
...
cad1 from yaw 75, pitch 45
doc: cad1
yaw: 75
pitch: 45.26
zoom: 1
output: view1.png
attachment: view.png (30621 bytes)
cad1 from yaw 0, pitch 90, zoom 1.50
...
```

`view=` picks the starting angle (iso, front, back, left, right, top, bottom; default iso), and `yaw`/`pitch` are added to it. That's why `yaw=30 pitch=10` from iso gives yaw 75, pitch 45. `width`/`height` set the picture size (default 800 x 600).

**Claude.** `app.view` attaches the PNG to its answer, so Claude can look around a model the way you would:

> Show me the bracket from underneath. Are the holes countersunk?

Claude calls `app_view` with `view=bottom` (and maybe `zoom=3 pan_x=...` to get close to a hole), looks at the picture and answers. It's handy for checking a part after an edit without saving anything: "turn it 90 degrees and show me the back".

## Reference

| command | what it does | key parameters |
| --- | --- | --- |
| `app.view` | draw a 3D document from any angle as a PNG | `doc`, `view` (iso), `yaw` (0), `pitch` (0), `zoom` (1), `pan_x` (0), `pan_y` (0), `width` (800), `height` (600), `output` |

Works on: cad parts, meshes (model app, asset forge), sculpts, scan fits and prints in the slicer. Anything else answers that it isn't a 3D document.

## Limitations

- **Orthographic only** (no perspective), and simple shading: it's for seeing shape, not for pretty pictures. Use the render tool for those.
- **Big meshes are slower to turn.** Cad parts (a few thousand triangles) turn instantly. A 330,000-triangle mesh redraws about 15 times a second while you drag (about 60 ms a frame on a 4-core cloud machine), and a million-triangle scan a few times a second. Decimate it in forge or fit first if that bothers you.
- No section views or measuring in the 3D view yet; use the `*.measure*` and section commands of each tool. Cad parts pick faces (`cad.pick`) and models pick objects and elements (`model.pick`); other 3D files only turn around.
- Render scenes (the render tool) and level layouts keep their own previews and camera commands.
