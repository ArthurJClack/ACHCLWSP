# Circuit: schematics, PCBs and Gerbers

`circuit` designs small electronics boards, headless, from schematic to the files a PCB fab needs. One **circuit** document holds both halves:

- the **schematic**: parts from a built-in symbol library (resistors, capacitors, LEDs, diodes, Zeners, transistors, op-amps, regulators, the 555, switches, connectors, ATmega328P, ATtiny85, RP2040 and ESP32 chips, generic ICs and dev boards such as the Arduino Nano and Raspberry Pi Pico), wires, net labels and power symbols. It checks itself (ERC), exports a netlist and a bill of materials, and draws to SVG, PDF and PNG;
- the **board**: an outline, footprints placed from the schematic, a ratsnest, traces and vias on two or four copper layers drawn by hand or by the autorouter, copper pours and planes with thermal reliefs, plane fan-out, mounting holes, design rule checks (DRC), Gerber RS-274X plus Excellon drill files and the assembly files (pick-and-place and BOM), as a zip ready to upload or as a folder, and a 3D picture of the assembled board.

There is also a small simulator (DC operating point and transient) for the analog parts. KiCad 6/7/8 schematics (`.kicad_sch`, sub-sheets included), boards (`.kicad_pcb`), projects and netlists open directly, and designs save back to KiCad as well as to `.circuit.json`. In the electronics window you can click on the schematic and the board to place parts, draw wires and traces and move things.

Each task below is shown three ways:

- **CLI**: single `wetwaresuite` commands. A circuit only lives inside one process, so one-shot commands look things up (`circuit.symbols`, `circuit.footprints`, `circuit.symbol_info`) or open a file and report on it. Anything longer goes through a batch script, which can also come from a pipe (`... | wetwaresuite batch`).
- **Batch**: a script with `-> name` captures and `$name` reuse (see [headless and batch](headless-and-batch.md)). This is how you will normally drive circuit from a shell.
- **Claude**: a prompt for Claude over MCP and the tools it calls (see [mcp.md](mcp.md)). `wetwaresuite mcp --tools circuit` serves just these tools. Every editing tool takes `preview=true`, which attaches a PNG of the schematic (or of the board, for board commands), so Claude can look at what it drew.

Every example below was run against the real binary in an empty folder; the KiCad task also uses `timer.net`, whose full text is in [task 5](#task-5-open-a-kicad-netlist-and-make-a-board). Outputs are copied from those runs; repeated fields are cut where marked `...`.

## How circuit works

**Two coordinate systems.** The schematic is in **grid units of 0.1 in** (2.54 mm), the spacing between pins, with y pointing **down**. The board is in **millimetres**, also with y down (Gerber files flip it, as the format expects). You rarely type board coordinates: `circuit.pcb_place` places footprints and `circuit.route` routes, and `circuit.pads` tells you where the pads are when you do want to draw by hand.

**Parts, refs and pins.** `circuit.place` puts a library symbol on the sheet and gives it the next free reference (`R1`, `R2`, `D1`, `U1`, `J1`, `A1` for modules) unless you pass `ref=`. Pins are named `REF.NUMBER` or `REF.NAME`: `R1.2`, `D1.A`, `U1.VCC`, `A1.D13`. Pins of dev boards have aliases, so `A1.TX` and `A1.D1` both reach the Nano's `D1/TX` pin. The reply to `place` lists every pin with its position, so you (or Claude) know where it ended up.

**Connections are geometry.** A wire end or vertex on a pin connects to it; a pin, wire end or label lying on another wire connects there (a T-junction, drawn with a dot). Labels with the same name are one net anywhere on the sheet, and so are power symbols with the same value (`GND`, `+5V`, `VCC`, `+3V3`). You can draw wires point by point (`circuit.wire`), but three commands do almost all the work:

| command | what it draws | use it for |
|---|---|---|
| `circuit.connect from=R1.2 to=D1.A` | a wire with right-angle bends that avoids part bodies | neighbours |
| `circuit.net name=SDA pins=U1.5,R3.1,J1.3` | a short stub and a label on each pin | pins far apart, buses, anything that would make a messy drawing |
| `circuit.power symbol=gnd pins=C1.1,C2.1` | a ground or power symbol under each pin | power rails |

If `connect` can't find a clean path (every route would cross other wires or pins), it joins the pins with net labels instead and says so.

**Net names.** A net takes the name of its label or power symbol; unnamed nets are called `Net-(R1-1)` after their first pin, the way KiCad does it.

**Footprints.** Every symbol has a default footprint (`R_0805`, `LED_0805`, `SOT-23`, `DIP-8`, `PinHeader_1x02` ...); `footprint=` on `place` or `edit` picks another from `circuit.footprints`. KiCad names such as `Resistor_SMD:R_0805_2012Metric` are understood.

**Undo.** Every edit (place, wire, route, pour, rules ...) is one undo step (`undo` and `redo` lines in a batch script, Claude's `undo` and `redo` tools). Checks, lists, previews and exports change nothing.

**Values** take SI suffixes: `4k7`, `4.7k`, `100n`, `10u`, `2M2` or `2.2meg` (mega; a lower-case `m` is milli, as in SPICE), optionally with a unit (`10uF`, `5V`). The simulator reads them; the BOM keeps what you typed.

## Task 1: your first schematic and board, an LED on a header

Two header pins bring in 5 V; a resistor and an LED light up. From nothing to a routed board with a ground pour and a fab zip.

**Batch** (`blinky.txt`):

```text
circuit.new title=blinky -> c
circuit.place doc=$c symbol=header pins=2 ref=J1 x=0 y=0
circuit.place doc=$c symbol=resistor value=330 x=6 y=-2
circuit.place doc=$c symbol=led value=red x=12 y=-2
circuit.connect doc=$c from=J1.1 to=R1.1
circuit.connect doc=$c from=R1.2 to=D1.2
circuit.power doc=$c symbol=+5v pins=J1.1
circuit.power doc=$c symbol=gnd pins=J1.2,D1.1
circuit.erc doc=$c
circuit.nets doc=$c
circuit.export doc=$c path=blinky.png
circuit.export doc=$c path=blinky.svg
circuit.export doc=$c path=blinky.pdf
circuit.sim_op doc=$c
circuit.pcb_place doc=$c
circuit.route doc=$c
circuit.drc doc=$c
circuit.pour doc=$c net=GND layer=B
circuit.drc doc=$c
circuit.export doc=$c view=pcb path=board.png
circuit.export doc=$c view=pcb path=board.svg
circuit.gerber doc=$c path=fab.zip
circuit.save doc=$c path=blinky.circuit.json
circuit.bom doc=$c
```

```sh
$ wetwaresuite batch blinky.txt
created ckt1: 0 parts, 0 nets, 0 wires
...
placed J1 (header Conn_01x02) at 0,0
pins: 1=Pin_1@-3,0 2=Pin_2@-3,1
...
placed R1 (resistor 330) at 6,-2
pins: 1@4,-2 2@8,-2
...
placed D1 (led red) at 12,-2
pins: 1=K@14,-2 2=A@10,-2
...
connected 2 pins on net Net-(J1-1)
connected 2 pins on net Net-(D1-2)
added 1 +5V symbol
added 2 GND symbols
ERC: 0 errors, 0 warnings
3 nets:
+5V: J1.1 R1.1
GND: J1.2 D1.1
Net-(D1-2): R1.2 D1.2
wrote blinky.png (38047 bytes)
wrote blinky.svg (13219 bytes)
wrote blinky.pdf (2602 bytes)
operating point (14 iterations):
  V(+5V) = 5V
  V(Net-(D1-2)) = 2V
  I(+5V) = 9.11mA delivered
note: J1 (header) doesn't conduct in the simulation; left out
...
placed 3 footprints; board 21 x 12 mm
routed 3 of 3 connections (12 segments, 1 vias)
DRC: 0 errors, 0 warnings
poured GND on B.Cu, 210.27 mm2 of copper
DRC: 0 errors, 0 warnings
wrote board.png (35281 bytes)
wrote board.svg (9519 bytes)
wrote 10 files to fab.zip
  blinky-F_Cu.gtl (920 bytes)
  blinky-F_Mask.gts (372 bytes)
  blinky-F_Paste.gtp (266 bytes)
  blinky-F_Silkscreen.gto (1647 bytes)
  blinky-B_Cu.gbl (3202 bytes)
  blinky-B_Mask.gbs (248 bytes)
  blinky-B_Paste.gbp (142 bytes)
  blinky-B_Silkscreen.gbo (143 bytes)
  blinky-Edge_Cuts.gm1 (299 bytes)
  blinky.drl (196 bytes)
saved blinky.circuit.json
Item,Qty,References,Value,Footprint,Description
1,1,D1,red,LED_0805,"LED (1 = K, 2 = A)"
2,1,J1,Conn_01x02,PinHeader_1x02,Pin header 1x2
3,1,R1,330,R_0805,Resistor
```

What happened, line by line:

- The LED's pins are `1 = K` (cathode) and `2 = A` (anode), as in KiCad, so `R1.2 to D1.2` feeds the anode; `D1.A` would say the same thing more readably.
- `circuit.power symbol=+5v pins=J1.1` names the net `+5V`; the ground symbols make `GND`.
- `sim_op` treats `+5V` as a 5 V source and finds 2 V across the red LED and 9.1 mA, so 330 ohms is about right.
- `pcb_place` made a 21 x 12 mm board because there was no outline yet (see [task 3](#task-3-a-shield-for-an-arduino-nano-with-copper-on-both-sides) for a board of your own size); `route` routed all three connections; the pour fills the bottom with ground.
- `fab.zip` is what you upload to a fab: copper, mask, paste and silkscreen for both sides, the outline and the drill file, with the usual extensions (`.gtl`, `.gbl`, `.gm1` ...), so fabs recognize the layers on their own.

The schematic PNG shows the header on the left, the resistor and LED to its right and the ground and +5V symbols on their pins; the board PNG shows top copper in red, bottom copper in blue, gold pads, white silkscreen and the yellow outline.

**CLI.** The design is saved, so you can come back to it in one command. `circuit.open` reports what is in the file:

```sh
$ wetwaresuite circuit.open path=blinky.circuit.json
opened blinky.circuit.json as ckt1: 3 parts, 3 nets, 5 wires; board 21.0 x 12.0 mm, 12 traces, 1 vias (blinky.circuit.json)
ckt1
doc: ckt1
parts: 6
```

(`parts: 6` counts the three power symbols too; the summary line counts real parts.) To do one thing to a saved design, pipe a two-line script into `batch`, which reads standard input when given no file:

```sh
$ printf 'circuit.open path=blinky.circuit.json -> c\ncircuit.export doc=$c path=blinky-again.pdf\n' | wetwaresuite batch
opened blinky.circuit.json as ckt1: ...
wrote blinky-again.pdf (2602 bytes)
```

**Claude.** *"Make a board with a 2-pin power header, a 330 ohm resistor and a red LED, check it, route it with a ground pour on the bottom and give me Gerbers in fab.zip."* Claude calls `circuit_new`, three `circuit_place` (reading each reply's pin list), `circuit_connect` twice, `circuit_power` for +5V and GND, `circuit_erc`, `circuit_preview` to look at the sheet, then `circuit_pcb_place`, `circuit_route`, `circuit_pour`, `circuit_drc` with `preview=true`, and `circuit_gerber path=fab.zip`. If you ask *"is 330 ohms right for a red LED at 5 V?"* it runs `circuit_sim_op` and reads off the current.

## Task 2: a 555 LED blinker, schematic to Gerber

The classic astable 555: R1 and R2 charge C1, the 555 discharges it through R2, and the output drives an LED. This shows a generic IC with named pins, `circuit.net` for the timing nodes, and power on several pins at once.

**Batch** (`blinker.txt`):

```text
# blinker.txt: a 555 astable LED blinker, from schematic to Gerber
circuit.new title=blinker -> c
circuit.place doc=$c symbol=ic ref=U1 value=NE555 pins=8 pin_names=GND,TRIG,OUT,RESET,CTRL,THR,DIS,VCC x=0 y=0
circuit.place doc=$c symbol=resistor ref=R1 value=10k x=-12 y=-6 rot=90
circuit.place doc=$c symbol=resistor ref=R2 value=47k x=-12 y=0 rot=90
circuit.place doc=$c symbol=capacitor ref=C1 value=10u x=-12 y=6 rot=90 footprint=C_1206
circuit.place doc=$c symbol=capacitor ref=C2 value=10n x=10 y=6 rot=90
circuit.place doc=$c symbol=resistor ref=R3 value=470 x=12 y=-3
circuit.place doc=$c symbol=led ref=D1 value=red x=18 y=-3
circuit.place doc=$c symbol=header ref=J1 pins=2 value=PWR x=-24 y=0
circuit.connect doc=$c from=R1.1 to=R2.2
circuit.connect doc=$c from=R2.1 to=C1.2
circuit.net doc=$c name=DIS pins=U1.DIS,R1.1
circuit.net doc=$c name=THR pins=U1.THR,U1.TRIG,C1.2
circuit.connect doc=$c from=U1.OUT to=R3.1
circuit.connect doc=$c from=R3.2 to=D1.A
circuit.net doc=$c name=CTRL pins=U1.CTRL,C2.2
circuit.power doc=$c symbol=+5v pins=R1.2,U1.VCC,U1.RESET,J1.1
circuit.power doc=$c symbol=gnd pins=C1.1,C2.1,D1.K,U1.GND,J1.2
circuit.erc doc=$c
circuit.export doc=$c path=blinker.png
circuit.pcb_place doc=$c
circuit.route doc=$c
circuit.pour doc=$c net=GND layer=B
circuit.drc doc=$c
circuit.export doc=$c view=pcb path=blinker-board.png
circuit.gerber doc=$c path=blinker-gerbers.zip
circuit.bom doc=$c path=blinker-bom.csv
circuit.save doc=$c path=blinker.circuit.json
```

```sh
$ wetwaresuite batch blinker.txt
created ckt1: 0 parts, 0 nets, 0 wires
...
placed U1 (ic NE555) at 0,0
pins: 1=GND@-5,-1 2=TRIG@-5,0 3=OUT@-5,1 4=RESET@-5,2 5=CTRL@5,2 6=THR@5,1 7=DIS@5,0 8=VCC@5,-1
...
placed R1 (resistor 10k) at -12,-6
pins: 1@-12,-4 2@-12,-8
...
connected 2 pins on net Net-(R1-1)
connected 2 pins on net Net-(C1-2)
net DIS now has 3 pins
net THR now has 4 pins
connected 2 pins on net Net-(R3-1)
connected 2 pins on net Net-(D1-2)
net CTRL now has 2 pins
added 4 +5V symbols
added 5 GND symbols
ERC: 0 errors, 0 warnings
wrote blinker.png (113843 bytes)
placed 8 footprints; board 34 x 25 mm
routed 15 of 15 connections (65 segments, 2 vias)
poured GND on B.Cu, 737.41 mm2 of copper
DRC: 0 errors, 0 warnings
wrote blinker-board.png (115954 bytes)
wrote 10 files to blinker-gerbers.zip
  blinker-F_Cu.gtl (3573 bytes)
  ...
  blinker.drl (347 bytes)
wrote blinker-bom.csv (8 rows)
saved blinker.circuit.json
```

Things to notice:

- `symbol=ic pins=8 pin_names=...` makes a generic IC whose pins you can then use by name (`U1.DIS`, `U1.VCC`). Pins 1 to N/2 go down the left side and the rest come back up the right, like a DIP package, and the default footprint is `DIP-8` to match.
- `rot=90` stands the resistors and capacitors upright (rotation is counter-clockwise; a vertical resistor has pin 1 at the bottom). The `place` reply shows where each pin went.
- `connect from=R1.1 to=R2.2` and then `net name=DIS pins=U1.DIS,R1.1` put a label on a pin that already had a wire: the net is the union, so DIS has three pins (U1.7, R1.1, R2.2). `circuit.nets` lists them.
- Routing goes before pouring. The pour flows around the traces and keeps 0.3 mm from other nets; islands touching no GND pad are dropped.

The schematic (`blinker.png`) has the timing chain R1, R2, C1 down the left, the 555 in the middle with DIS, THR and CTRL labels on its right-hand pins, and R3 and the LED at the top right.

**CLI.** Check the result:

```sh
$ wetwaresuite circuit.open path=blinker.circuit.json
opened blinker.circuit.json as ckt1: 8 parts, 7 nets, 20 wires; board 34.0 x 25.0 mm, 65 traces, 2 vias (blinker.circuit.json)
...
```

**Claude.** *"Design a 555 astable that blinks an LED about once a second from 5 V, with an SMD board and Gerbers."* Claude picks values (10k, 47k, 10 uF gives f = 1.44 / ((R1 + 2 R2) C1), about 1.4 Hz), calls `circuit_symbols category=ic` to see what exists, `circuit_place symbol=ic pins=8 pin_names=...` for the 555 and the other parts, `circuit_net` for DIS, THR and CTRL, `circuit_connect`, `circuit_power`, `circuit_erc`, `circuit_preview` to look, then `circuit_pcb_place`, `circuit_route`, `circuit_pour`, `circuit_drc preview=true`, `circuit_gerber`, `circuit_bom` and `circuit_save`.

## Task 3: a shield for an Arduino Nano, with copper on both sides

A dev-board footprint with three LEDs and a button, on a board you size yourself, poured with ground on both layers.

**Batch** (`nano.txt`):

```text
circuit.new title=nano-leds -> c
circuit.place doc=$c symbol=arduino_nano x=0 y=0
circuit.place doc=$c symbol=resistor value=220 x=20 y=-4
circuit.place doc=$c symbol=resistor value=220 x=20 y=-1
circuit.place doc=$c symbol=resistor value=220 x=20 y=2
circuit.place doc=$c symbol=led value=red x=28 y=-4
circuit.place doc=$c symbol=led value=green x=28 y=-1
circuit.place doc=$c symbol=led value=blue x=28 y=2
circuit.place doc=$c symbol=switch x=20 y=8
circuit.place doc=$c symbol=capacitor value=100n x=-14 y=8 rot=90
circuit.net doc=$c name=LED1 pins=A1.D2,R1.1
circuit.net doc=$c name=LED2 pins=A1.D3,R2.1
circuit.net doc=$c name=LED3 pins=A1.D4,R3.1
circuit.connect doc=$c from=R1.2 to=D1.A
circuit.connect doc=$c from=R2.2 to=D2.A
circuit.connect doc=$c from=R3.2 to=D3.A
circuit.net doc=$c name=BTN pins=A1.D5,SW1.1
circuit.power doc=$c symbol=gnd pins=D1.K,D2.K,D3.K,SW1.2,C1.2,A1.GND
circuit.power doc=$c symbol=+5v pins=C1.1,A1.5V
circuit.erc doc=$c
circuit.export doc=$c path=nano.png
circuit.pcb_place doc=$c
circuit.route doc=$c
circuit.drc doc=$c
circuit.pour doc=$c net=GND layer=both
circuit.drc doc=$c
circuit.export doc=$c view=pcb path=nano-board.png
circuit.gerber doc=$c path=nano-fab
```

```sh
$ wetwaresuite batch nano.txt
...
placed A1 (arduino_nano Arduino_Nano) at 0,0
pins: 1=D1/TX@-6,-7 2=D0/RX@-6,-6 3=RESET@-6,-5 4=GND@-6,-4 5=D2@-6,-3 6=D3@-6,-2 7=D4@-6,-1 8=D5@-6,0 9=D6@-6,1 10=D7@-6,2 11=D8@-6,3 12=D9@-6,4 13=D10@-6,5 14=D11/MOSI@-6,6 15=D12/MISO@-6,7 16=D13/SCK@6,-7 17=3V3@6,-6 18=AREF@6,-5 19=A0@6,-4 20=A1@6,-3 21=A2@6,-2 22=A3@6,-1 23=A4/SDA@6,0 24=A5/SCL@6,1 25=A6@6,2 26=A7@6,3 27=5V@6,4 28=RESET@6,5 29=GND@6,6 30=VIN@6,7
...
added 6 GND symbols
added 2 +5V symbols
ERC: 0 errors, 0 warnings
info [module_unused] A1: 21 pins unused (normal for a dev board)
wrote nano.png (159022 bytes)
placed 9 footprints; board 47 x 59 mm
routed 17 of 17 connections (72 segments, 7 vias)
DRC: 0 errors, 0 warnings
poured GND on both layers, 4741.23 mm2 of copper
DRC: 0 errors, 0 warnings
wrote nano-board.png (297737 bytes)
wrote 10 files to nano-fab
  nano-leds-F_Cu.gtl (194652 bytes)
  ...
  nano-leds.drl (800 bytes)
```

- The Nano's two GND pins (4 and 29) are joined inside the module, so `A1.GND` (the first one) is enough; the router and DRC know both pads are ground.
- Unused module pins are fine: ERC prints an `info` line, not an error. On an ordinary IC an unconnected pin is an error until you mark it with `circuit.no_connect`.
- `path=nano-fab` (no `.zip`) writes the same ten files into a folder.

For a board of a given size, set the outline before placing: `circuit.board doc=$c width=50 height=60` (top-left corner at 0,0) or any polygon with `points=0,0,50,0,50,60,0,60`. Then `pcb_place` packs the footprints inside it, and `circuit.footprint_move doc=$c ref=SW1 x=40 y=50 rot=90` puts a part exactly where you want it (centre in mm, rotation counter-clockwise; `side=bottom` flips it to the back).

The other dev boards work the same way: `symbol=arduino_uno` (the R3 shield header positions), `symbol=esp32_devkit` (the 30-pin DOIT DevKit V1, rows 25.4 mm apart) and `symbol=bluepill` (STM32F103, 2 x 20). `circuit.symbol_info` lists their pins:

```sh
$ wetwaresuite circuit.symbol_info symbol=esp32
esp32_devkit: ESP32 DevKit V1 (30 pins, DOIT layout)
prefix A, default value ESP32_DevKit, footprint ESP32_DevKit
pins:
  1 EN (passive) at -7,-7
  2 GPIO36/VP (passive) at -7,-6
  ...
  29 GND (power_in) at 7,6
  30 3V3 (power_in) at 7,7
pins: 30
footprint: ESP32_DevKit
```

**Claude.** *"I have an Arduino Nano. Make me a carrier board with LEDs on D2, D3 and D4 through 220 ohm resistors, a button on D5 to ground, and a 100 nF decoupling cap; ground pour on both sides."* Claude calls `circuit_symbol_info symbol=nano` to learn the pin names, places the parts, joins the GPIOs with `circuit_net`, adds power with `circuit_power`, runs `circuit_erc`, then `circuit_board` if you gave a size, `circuit_pcb_place`, `circuit_preview view=pcb` to see the placement, `circuit_footprint_move` for anything awkward, `circuit_route`, `circuit_pour layer=both` and `circuit_drc`.

## Task 4: route a board by hand

The autorouter is fine for small boards, but sometimes you want a trace exactly here. This reopens the blinky board from task 1, removes the autorouted copper and draws it again.

`circuit.pads` gives every pad's centre in mm, its size and its net:

```sh
$ printf 'circuit.open path=blinky.circuit.json -> c\ncircuit.pads doc=$c\n' | wetwaresuite batch
...
6 pads:
J1.1 at 5.08,5.08 size 1.7x1.7 drill 1 (F+B) net +5V
J1.2 at 5.08,7.62 size 1.7x1.7 drill 1 (F+B) net GND
R1.1 at 14.29,3.81 size 1x1.3 (F) net +5V
R1.2 at 16.19,3.81 size 1x1.3 (F) net Net-(D1-2)
D1.1 at 9.21,3.81 size 1x1.3 (F) net GND
D1.2 at 11.11,3.81 size 1x1.3 (F) net Net-(D1-2)
```

**Batch** (`hand.txt`):

```text
# hand.txt: route the blinky board by hand
circuit.open path=blinky.circuit.json -> c
circuit.unroute doc=$c
circuit.unpour doc=$c
circuit.pads doc=$c
# LED anode to resistor: over the top of R1.1 on the top layer
circuit.trace doc=$c layer=F points=16.19,3.81,16.19,1.9,11.11,1.9,11.11,3.81
# +5V under the LED, 0.5 mm wide because +5V is a power net
circuit.trace doc=$c layer=F points=5.08,5.08,7.62,5.6,13.5,5.6,14.29,4.8,14.29,3.81
# GND: up from the LED cathode, through a via, along the bottom to J1.2
circuit.trace doc=$c layer=F points=9.21,3.81,9.21,2.4,7.5,2.4
circuit.via doc=$c x=7.5 y=2.4
circuit.trace doc=$c layer=B points=7.5,2.4,7.5,7.62,5.08,7.62
circuit.ratsnest doc=$c
circuit.drc doc=$c
circuit.export doc=$c view=pcb path=hand.png
```

```sh
$ wetwaresuite batch hand.txt
opened blinky.circuit.json as ckt1: 3 parts, 3 nets, 5 wires; board 21.0 x 12.0 mm, 12 traces, 1 vias (blinky.circuit.json)
...
removed 12 traces and 1 vias
removed 1 pours
6 pads:
...
3 trace segments on F.Cu net Net-(D1-2)
4 trace segments on F.Cu net +5V
2 trace segments on F.Cu net GND
via at 7.5,2.4 on GND
2 trace segments on B.Cu net GND
0 unrouted connections
DRC: 0 errors, 0 warnings
wrote hand.png (35287 bytes)
```

- A trace's net comes from the pad (or trace) under its first point, and its width from the rules: 0.25 mm for signals, 0.5 mm for power nets (`GND`, `VCC`, `+5V`, anything named like a rail). `width=` and `net=` override.
- A via takes its net from the copper at its position.
- `circuit.ratsnest` lists what is still unrouted, `circuit.drc` catches traces that come too close to other nets or touch them. Mix freely: route the critical nets by hand, then `circuit.route` does the rest (it leaves existing copper alone), or `circuit.route nets=GND,+5V` / `skip=GND` limits it.
- `circuit.unroute net=GND` or `layer=B` removes just part of the copper.

**Claude.** *"On blinky, route +5V by hand along the bottom edge and let the autorouter do the rest."* Claude calls `circuit_open`, `circuit_pads net=+5V`, `circuit_unroute net=+5V`, `circuit_trace` with points it computes from the pad list, `circuit_route` for the remaining nets and `circuit_drc preview=true` to look at the result.

## Task 5: open a KiCad netlist and make a board

`circuit.open` reads KiCad netlists (`.net`, the s-expression format Eeschema exports with File > Export > Netlist). Parts are mapped to library symbols (R, C, L, LED, D, Q, Conn_01xNN ...) and KiCad footprint names to built-in footprints; anything else becomes a generic IC with the right pin count. The parts are laid out on a grid and joined with net labels, so you get a readable schematic, an ERC and a board straight away.

`timer.net` (a 555 with its timing parts, as Eeschema 8 writes it):

```text
(export (version "E")
  (design (source "timer.kicad_sch") (tool "Eeschema 8.0"))
  (components
    (comp (ref "U1") (value "NE555P") (footprint "Package_DIP:DIP-8_W7.62mm")
      (libsource (lib "Timer") (part "NE555P") (description "Precision Timers, 555 compatible")))
    (comp (ref "R1") (value "1k") (footprint "Resistor_SMD:R_0805_2012Metric")
      (libsource (lib "Device") (part "R")))
    (comp (ref "R2") (value "10k") (footprint "Resistor_SMD:R_0805_2012Metric")
      (libsource (lib "Device") (part "R")))
    (comp (ref "C1") (value "10u") (footprint "Capacitor_SMD:C_1206_3216Metric")
      (libsource (lib "Device") (part "C")))
    (comp (ref "J1") (value "PWR") (footprint "Connector_PinHeader_2.54mm:PinHeader_1x02_P2.54mm_Vertical")
      (libsource (lib "Connector") (part "Conn_01x02_Pin"))))
  (nets
    (net (code "1") (name "+5V")
      (node (ref "U1") (pin "8") (pinfunction "VCC")) (node (ref "U1") (pin "4") (pinfunction "R"))
      (node (ref "R1") (pin "1")) (node (ref "J1") (pin "1")))
    (net (code "2") (name "GND")
      (node (ref "U1") (pin "1") (pinfunction "GND")) (node (ref "C1") (pin "2")) (node (ref "J1") (pin "2")))
    (net (code "3") (name "/DIS")
      (node (ref "U1") (pin "7") (pinfunction "DIS")) (node (ref "R1") (pin "2")) (node (ref "R2") (pin "1")))
    (net (code "4") (name "/THR")
      (node (ref "U1") (pin "6") (pinfunction "THR")) (node (ref "U1") (pin "2") (pinfunction "TR"))
      (node (ref "R2") (pin "2")) (node (ref "C1") (pin "1")))
    (net (code "5") (name "unconnected-(U1-Pad3)")
      (node (ref "U1") (pin "3") (pinfunction "Q")))
    (net (code "6") (name "unconnected-(U1-Pad5)")
      (node (ref "U1") (pin "5") (pinfunction "CV")))))
```

**CLI.** Open it to see what came across:

```sh
$ wetwaresuite circuit.open path=timer.net
opened timer.net as ckt1: 5 parts, 6 nets, 14 wires (timer.net)
warning: U1: 'NE555P' has no library symbol; using a generic 8-pin IC
ckt1
doc: ckt1
parts: 5
```

The generic IC keeps the pin names from `pinfunction` (`U1.VCC`, `U1.DIS` ...). The hierarchical `/` is dropped from net names.

**Batch** (`kicad.txt`):

```text
# kicad.txt: a KiCad netlist to a routed board
circuit.open path=timer.net -> t
circuit.parts doc=$t
circuit.nets doc=$t
circuit.erc doc=$t
circuit.no_connect doc=$t pins=U1.3,U1.5
circuit.erc doc=$t
circuit.export doc=$t path=timer.svg
circuit.board doc=$t width=30 height=25
circuit.pcb_place doc=$t
circuit.route doc=$t
circuit.drc doc=$t
circuit.export doc=$t view=pcb path=timer-board.svg
circuit.gerber doc=$t path=timer-fab name=timer
circuit.export_netlist doc=$t path=timer-out.net
circuit.save doc=$t path=timer.circuit.json
```

```sh
$ wetwaresuite batch kicad.txt
opened timer.net as ckt1: 5 parts, 6 nets, 14 wires (timer.net)
warning: U1: 'NE555P' has no library symbol; using a generic 8-pin IC
...
5 parts:
U1: ic NE555P [DIP-8] at 7,4
R1: resistor 1k [R_0805] at 19,2
R2: resistor 10k [R_0805] at 29,2
C1: capacitor 10u [C_1206] at 39,3
J1: header PWR [PinHeader_1x02] at 50,3
...
6 nets:
GND: U1.1 C1.2 J1.2
THR: U1.2 U1.6 R2.2 C1.1
Net-(U1-3): U1.3
+5V: U1.4 U1.8 R1.1 J1.1
Net-(U1-5): U1.5
DIS: U1.7 R1.2 R2.1
unconnected pins: 2
...
ERC: 2 errors, 0 warnings
error [unconnected_pin] U1.3 (Q) is not connected (wire it or mark it with circuit.no_connect)
error [unconnected_pin] U1.5 (CV) is not connected (wire it or mark it with circuit.no_connect)
...
marked 2 pins not connected
ERC: 0 errors, 0 warnings
wrote timer.svg (52225 bytes)
board 30 x 25 mm
placed 5 footprints; board 30 x 25 mm
routed 10 of 10 connections (30 segments, 1 vias)
DRC: 0 errors, 0 warnings
wrote timer-board.svg (12700 bytes)
wrote 10 files to timer-fab
  timer-F_Cu.gtl (1869 bytes)
  ...
  timer.drl (330 bytes)
wrote timer-out.net (kicad, 6 nets)
saved timer.circuit.json
```

The ERC errors are real: this netlist leaves the 555's output and control pins unconnected (KiCad would have shown no-connect flags for them). `circuit.no_connect` says that's deliberate.

`circuit.export_netlist` goes the other way: `.net` writes a KiCad netlist (which `circuit.open` reads back), `.json` a machine-readable one, `.txt` one net per line:

```text
GND: U1.1 C1.1 C2.1 D1.1 J1.2
THR: U1.2 U1.6 R2.1 C1.2
...
```

**Claude.** *"Here's timer.net from KiCad. Make me a 30 x 25 mm board and Gerbers."* Claude calls `circuit_open path=timer.net`, `circuit_erc`, asks whether U1.3 and U1.5 are meant to be unconnected (or reads the `unconnected-` net names and decides), `circuit_no_connect`, `circuit_board width=30 height=25`, `circuit_pcb_place`, `circuit_route`, `circuit_drc` and `circuit_gerber`.

## Task 6: simulate an RC filter and an LED

The simulator solves the circuit's nodal equations with Newton iteration for diodes: a DC operating point, and a transient with a fixed time step. Sources are `vsource` parts (a number, `SIN(offset amplitude freq)` or `PULSE(v1 v2 delay rise fall width period)`), `isource`, `battery`, and every power symbol named like a voltage (`+5V`, `+3V3`, `+12V`, `-12V`, `12V`; `VCC` has no voltage in its name, so the simulator asks for `fields=voltage=5` on its symbol, or use `+5V`). Ground is `GND`.

A 10k / 100n low-pass (tau = 1 ms) driven by a 5 V square wave, and an LED on +5V:

**Batch** (`rc.txt`):

```text
# rc.txt: a voltage divider, an LED and an RC low-pass, simulated
circuit.new title=rc-lab -> c
circuit.place doc=$c symbol=vsource ref=V1 value="PULSE(0 5 1m 10u 10u 5m 10m)" x=0 y=0
circuit.place doc=$c symbol=resistor ref=R1 value=10k x=5 y=-2
circuit.place doc=$c symbol=capacitor ref=C1 value=100n x=9 y=1 rot=90
circuit.connect doc=$c from=V1.1 to=R1.1
circuit.net doc=$c name=OUT pins=R1.2,C1.2
circuit.power doc=$c symbol=gnd pins=V1.2,C1.1
circuit.place doc=$c symbol=+5v ref=#PWR9 x=16 y=-4
circuit.place doc=$c symbol=resistor ref=R2 value=330 x=16 y=-1 rot=90
circuit.place doc=$c symbol=led ref=D1 value=red x=16 y=4 rot=270
circuit.connect doc=$c from=#PWR9.1 to=R2.2
circuit.connect doc=$c from=R2.1 to=D1.A
circuit.power doc=$c symbol=gnd pins=D1.K
circuit.erc doc=$c
circuit.sim_op doc=$c
circuit.sim_tran doc=$c tstop=20m step=20u probes=OUT path=rc.csv preview=true
circuit.export doc=$c path=rc.png
```

The `PULSE(...)` value has spaces, so it is in double quotes.

```sh
$ wetwaresuite batch rc.txt
...
placed V1 (vsource PULSE(0 5 1m 10u 10u 5m 10m)) at 0,0
pins: 1=+@0,-2 2=-@0,2
...
ERC: 0 errors, 0 warnings
operating point (14 iterations):
  V(Net-(R1-1)) = 0V
  V(OUT) = 0V
  V(Net-(D1-2)) = 2V
  V(+5V) = 5V
  I(V1) = 0A delivered
  I(+5V) = 9.11mA delivered
...
transient to 20ms, 1001 points:
  V(OUT): min 0V, max 4.96V, final 94.6mV
wrote rc.csv
doc: ckt1
V(OUT): 0.0945959
attachment: transient.png (39631 bytes)
wrote rc.png (58035 bytes)
```

- The operating point uses each source's value at t = 0, so the pulse source is at 0 V.
- In the transient, OUT charges to 4.96 V during each 5 ms pulse (five time constants) and discharges in between. `rc.csv` has `time,V(OUT)` rows to plot elsewhere; `preview=true` attaches the plot as a PNG (Claude sees it; batch just reports it).
- Switches conduct when their field says so: `circuit.edit doc=$c ref=SW1 fields=state=on`. A potentiometer's wiper is `fields=wiper=0.25`.
- Regulators and generic ICs are not simulated (transistors and MOSFETs are, see task 11): `sim_op` and `sim_tran` refuse to run with them on the sheet unless `ignore_unsupported=true`, which leaves them out. Headers, crystals and dev boards are always left out with a note (as the header was in task 1). Op-amps are ideal amplifiers with a gain of 100000 and no supply limits, so a comparator circuit will swing past its rails.

**Claude.** *"What's the -3 dB point of a 10k and 100 nF low-pass? Simulate a 5 V square wave through it and show me."* Claude builds the circuit, runs `circuit_sim_tran preview=true` and looks at the plot; for the corner frequency it can run a `SIN` source at 159 Hz and read the amplitude from the `max` it reports.

## Task 7: revise a design

Designs change. This takes the blinker from task 2, slows it down, swaps the LED for a through-hole one, adds a test point on the output and tightens the clearance rule.

**Batch** (`tweak.txt`):

```text
# tweak.txt: revise the blinker: slower blink, THT LED, a test point, tighter rules
circuit.open path=blinker.circuit.json -> c
circuit.pins doc=$c ref=U1
circuit.edit doc=$c ref=R2 value=100k fields=mpn=RC0805FR-07100KL
circuit.edit doc=$c ref=D1 footprint=LED_THT
circuit.place doc=$c symbol=header ref=TP1 pins=1 value=OUT x=6 y=-8
circuit.net doc=$c name=OUT pins=TP1.1,U1.OUT
circuit.erc doc=$c
circuit.pcb_place doc=$c
circuit.footprint_move doc=$c ref=TP1 x=30 y=21
circuit.rules doc=$c clearance=0.25
circuit.drc doc=$c
circuit.unpour doc=$c
circuit.unroute doc=$c
circuit.route doc=$c
circuit.pour doc=$c net=GND layer=B
circuit.drc doc=$c
circuit.bom doc=$c
circuit.save doc=$c path=blinker-v2.circuit.json
```

```sh
$ wetwaresuite batch tweak.txt
...
U1 (ic, 8 pins):
  1 GND [power_in] at -5,-1 -> GND
  2 TRIG [passive] at -5,0 -> THR
  3 OUT [passive] at -5,1 -> Net-(R3-1)
  4 RESET [passive] at -5,2 -> +5V
  5 CTRL [passive] at 5,2 -> CTRL
  6 THR [passive] at 5,1 -> THR
  7 DIS [passive] at 5,0 -> DIS
  8 VCC [power_in] at 5,-1 -> +5V
...
changed R2: value, fields
changed D1: footprint
placed TP1 (header OUT) at 6,-8
...
net OUT now has 3 pins
ERC: 0 errors, 0 warnings
placed 1 footprints; board 34 x 25 mm
TP1 at 30,21 mm, rot 0, top
rules (mm): trace 0.25, power_trace 0.5, clearance 0.25, min_trace 0.15, via 0.4/0.8, min_drill 0.3, edge_clearance 0.3, grid 0.127
DRC: 7 errors, 0 warnings
error [short] pad U1.3 touches trace (Net-(R3-1)) (3.81, 10.16)-(4.57, 10.29) (OUT / Net-(R3-1))
error [short] pad U1.3 touches trace (Net-(R3-1)) (4.57, 10.29)-(10.41, 16.13) (OUT / Net-(R3-1))
error [clearance] pad R2.2 is 0.22 mm from via at (16.76, 15.24) (DIS / THR), minimum 0.25
error [short] via at (18.92, 16.13) touches pad R3.1 (Net-(R3-1) / OUT)
error [short] trace (Net-(R3-1)) (18.92, 16.13)-(19.37, 16.51) touches pad R3.1 (Net-(R3-1) / OUT)
error [unrouted] OUT: U1.3 to R3.1 is not routed
error [unrouted] OUT: R3.1 to TP1.1 is not routed
...
removed 1 pours
removed 65 traces and 2 vias
routed 16 of 16 connections (69 segments, 2 vias)
poured GND on B.Cu, 733.13 mm2 of copper
DRC: 0 errors, 0 warnings
Item,Qty,References,Value,Footprint,Description,mpn
1,1,C1,10u,C_1206,Capacitor,
2,1,C2,10n,C_0805,Capacitor,
3,1,D1,red,LED_THT,"LED (1 = K, 2 = A)",
4,1,J1,PWR,PinHeader_1x02,Pin header 1x2,
5,1,R1,10k,R_0805,Resistor,
6,1,R2,100k,R_0805,Resistor,RC0805FR-07100KL
7,1,R3,470,R_0805,Resistor,
8,1,TP1,OUT,PinHeader_1x01,Pin header 1x1,
9,1,U1,NE555,DIP-8,"Generic IC, 8 pins",
saved blinker-v2.circuit.json
```

The first DRC is worth reading:

- Naming the output net `OUT` renamed the net the old traces were drawn on (`Net-(R3-1)`), so DRC sees them as a different net touching OUT's pads. **Traces keep the net name they were drawn with**: after renaming a net on the schematic, unroute it (`circuit.unroute net=Net-(R3-1)`) and route it again.
- The tighter clearance catches a via that was legal at 0.2 mm.
- `pcb_place` only placed the new part (TP1); `all=true` re-places everything.

Removing the pour and every trace and routing again fixes all of it. `fields=` adds BOM columns (any name; `dnp=yes` leaves a part off the BOM). `circuit.edit ref=R2 new_ref=R10` renames a part, `circuit.move ref=R3 dx=2` moves it on the sheet with its wires following, `circuit.remove ref=TP1` deletes it with its wires and footprint, `circuit.unwire pin=R3.1` removes the wiring on one pin, and `circuit.annotate` fixes duplicate or missing references (`renumber=true` renumbers everything top-left to bottom-right).

**Claude.** *"Make the blinker blink slower and add a test point on the 555 output, then re-route."* Claude calls `circuit_open`, `circuit_pins ref=U1`, `circuit_edit` on R2 (it may run the 555 formula to pick the value), `circuit_place` and `circuit_net` for the test point, `circuit_erc`, `circuit_pcb_place`, `circuit_drc`, sees the stale-net shorts, `circuit_unroute`, `circuit_route`, `circuit_pour` and `circuit_drc` again.

## Task 8: a four-layer board with ground and power planes

Four layers put whole copper planes inside the board: `In1.Cu` for ground and `In2.Cu` for power is the usual stack. Through-hole pads reach the planes on their own; SMD pads need a short trace and a via each, which `circuit.fanout` adds. The planes connect pads through **thermal reliefs** (four spokes) so they stay easy to solder; `thermal=false` connects them solidly. A `mounting_hole` is a non-plated hole: `circuit.pcb_place` puts holes in the board's corners.

**Batch**

```text
# four.txt: an ATtiny85 board on four layers, with ground and power planes
circuit.new title=tiny4 -> c
circuit.place doc=$c symbol=attiny85 x=0 y=0
circuit.place doc=$c symbol=capacitor value=100n x=-12 y=-4
circuit.place doc=$c symbol=resistor value=330 x=12 y=-4
circuit.place doc=$c symbol=led x=18 y=-4
circuit.place doc=$c symbol=header pins=3 ref=J1 x=20 y=6
circuit.place doc=$c symbol=mounting_hole x=30 y=0
circuit.net doc=$c name=LED pins=U1.PB0,R1.1
circuit.connect doc=$c from=R1.2 to=D1.A
circuit.net doc=$c name=IN pins=U1.PB3,J1.3
circuit.power doc=$c symbol=gnd pins=U1.GND,C1.2,D1.K,J1.1
circuit.power doc=$c symbol=+5v pins=U1.VCC,C1.1,J1.2
circuit.no_connect doc=$c pins=U1.PB5,U1.PB1,U1.PB2,U1.PB4
circuit.erc doc=$c
circuit.board doc=$c width=40 height=30 layers=4
circuit.pcb_place doc=$c
circuit.pour doc=$c net=GND layer=In1
circuit.pour doc=$c net=+5V layer=In2
circuit.fanout doc=$c
circuit.route doc=$c skip=GND,+5V
circuit.drc doc=$c
circuit.export doc=$c view=3d path=tiny4-3d.png
circuit.gerber doc=$c path=tiny4.zip
circuit.save doc=$c path=tiny4.kicad_pro
```

```sh
$ wetwaresuite batch four.txt
...
board 40 x 30 mm, 4 copper layers
placed 6 footprints; board 40 x 30 mm
poured GND on In1.Cu, 1086.66 mm2 of copper
...
added 3 vias
routed 3 of 3 connections (12 segments, 0 vias)
DRC: 0 errors, 0 warnings
wrote tiny4-3d.png (239518 bytes)
wrote 15 files to tiny4.zip
saved tiny4.kicad_pro, tiny4.kicad_sch, tiny4.kicad_pcb
```

The zip holds four copper layers (`tiny4-F_Cu.gtl`, `tiny4-In1_Cu.g2`, `tiny4-In2_Cu.g3`, `tiny4-B_Cu.gbl`), mask, paste, silkscreen, the outline, `tiny4.drl` (plated holes), `tiny4-NPTH.drl` (the mounting hole), `tiny4-pos.csv` (pick-and-place) and `tiny4-bom.csv`, which is what an assembly service asks for. `circuit.placement path=pos.csv` writes the placement file on its own. The 3D picture turns with `angle=` and `tilt=`, `from_below=true` shows the underside and `mask=black` (or blue, red, white, purple) changes the solder mask colour.

**Claude**

> Make the ATtiny LED board on four layers with a ground plane and a 5 V plane, fan out the SMD parts, route the rest and show me the board in 3D.

Claude calls `circuit_board` with `layers=4`, `circuit_pour` twice (`layer=In1`, `layer=In2`), `circuit_fanout`, `circuit_route` with `skip=GND,+5V`, `circuit_drc` and `circuit_preview` with `view=3d`.

## Task 9: open a KiCad schematic, lay out its board and give it back to KiCad

`circuit.open` reads KiCad 6, 7 and 8 schematics. Parts keep KiCad's own symbol drawings; the ones it knows (R, C, LED, Conn_01x04 ...) also become library parts, so the simulator and the 3D view understand them. Sub-sheets are flattened onto one sheet, to the right of their parent: the sheet's pins become labels named `Sheet/PIN`, and local labels inside a sub-sheet are prefixed with its name. Multi-unit parts (a dual op-amp) keep one reference with their units drawn `U1A`, `U1B`; hidden power pins join their nets (`VCC`) as KiCad does. A `.kicad_pcb` next to the schematic is loaded too (`board=false` skips it), and `.kicad_pro` opens both.

**Batch** (`demo.kicad_sch` has a resistor, an LED, a dual gate and a sub-sheet `sub.kicad_sch` with one more resistor)

```text
# kicad2.txt: a KiCad schematic with a sub-sheet, checked, laid out and handed back to KiCad
circuit.open path=demo.kicad_sch -> k
circuit.parts doc=$k
circuit.nets doc=$k
circuit.erc doc=$k
circuit.board doc=$k width=40 height=25
circuit.pcb_place doc=$k
circuit.route doc=$k
circuit.save doc=$k path=demo-out.kicad_pro
circuit.open path=demo-out.kicad_pcb -> b
circuit.info doc=$b
circuit.unroute doc=$k
circuit.import_board doc=$k path=demo-out.kicad_pcb
```

```sh
$ wetwaresuite batch kicad2.txt
opened demo.kicad_sch as ckt1: 5 parts, 8 nets, 4 wires (demo.kicad_sch)
warning: hierarchical sheets were flattened onto one sheet (local labels are prefixed with the sheet name)
8 parts:
#PWR01: power:+5V +5V at 40,17
R1: resistor 330 [R_0805] at 40,20; MPN=RC0805FR-07330RL
D1: led red [LED_0805] at 40,25 rot 90
#PWR02: power:GND GND at 40,27.5
U1: test:DUAL DUAL [SOIC-8] at 60,20
U1: test:DUAL DUAL at 60,30
R2: resistor 10k [R_0805] at 75,17 rot 90
#PWR03: power:GND GND at 76.5,17
8 nets:
+5V: R1.1
Sub/IN: R1.2 D1.2 R2.1
GND: D1.1 R2.2
...
VCC: U1.8
ERC: 0 errors, 1 warnings
warning [multiple_names] one net has several names: Sub/IN, LED_A (it is called Sub/IN)
...
placed 4 footprints; board 40 x 25 mm
routed 3 of 3 connections (12 segments, 0 vias)
saved demo-out.kicad_pro, demo-out.kicad_sch, demo-out.kicad_pcb
opened demo-out.kicad_pcb as ckt2: 4 parts, 11 nets, 7 wires; board 40.0 x 25.0 mm, 12 traces, 0 vias (demo-out.kicad_pcb)
...
removed 12 traces and 0 vias
imported demo-out.kicad_pcb: 4 footprints, 12 track segments, 0 vias, 0 pours, 2 copper layers
```

- **Saving** to `.kicad_sch` writes one flat KiCad 7 sheet with every symbol embedded (so KiCad needs no library to show it), junctions where wires meet, and the footprint names; `.kicad_pcb` writes footprints with their pads and nets, tracks, vias, pours (as zones over the whole board) and the outline; `.kicad_pro` writes a project holding both. Symbols and footprints are linked the way KiCad links them, so its "Update PCB from Schematic" pairs them up.
- **A board on its own** (`circuit.open path=x.kicad_pcb`) keeps KiCad's footprints and copper; each part gets a boxed symbol whose pins are labelled with the pads' nets (pads with no net get a no-connect mark), so ERC, DRC, routing and Gerbers all work on it.
- **`circuit.import_board`** puts a KiCad board onto the schematic you already have: footprints go to the parts with the same reference, the outline, tracks, vias and pours replace this board's, and KiCad's net names are matched to the schematic's through the pads. It is one undo step.

**Claude**

> Open ~/kicad/sensor/sensor.kicad_pro, tell me what ERC finds, route whatever isn't routed and save it back for KiCad.

Claude calls `circuit_open` (schematic and board together), `circuit_erc`, `circuit_ratsnest`, `circuit_route`, `circuit_drc` and `circuit_save` with `path=~/kicad/sensor/sensor-routed.kicad_pro`.

## Task 10: draw in the window

Open a `.circuit.json`, `.kicad_sch`, `.kicad_pcb` or `.kicad_pro` in the electronics app (or make one with **New Circuit**). Above the picture:

- **Show: Schematic, Board, 3D** switches the picture (`circuit.view`). In 3D, drag to move around and scroll to zoom.
- **Tool** (schematic): **Select** says what you clicked (a pin and its net, a wire, a label, a part); **Place** puts the part chosen in the list next to it (type a value too) where you click; **Wire** draws from one click to the next, across then down, and keeps going until you click a pin or press Esc (or right-click); **Move** picks a part up with one click and puts it down with the next; **Delete** removes the part or wire you click. **Check** runs ERC.
- **Tool** (board): **Select**, **Move** (footprints), **Trace** (on the layer you pick; clicks on a pad land on its centre) and **Via**. **Place parts**, **Route** and **Check** (DRC) are one click each.

Every click asks `circuit.hit` what is under the mouse and then runs the ordinary command (`circuit.place`, `circuit.wire`, `circuit.move`, `circuit.remove`, `circuit.unwire`, `circuit.footprint_move`, `circuit.trace`, `circuit.via`), so each one shows in the history, Undo takes it back, and a batch script or Claude can do exactly the same. Scroll zooms, a right or middle drag (or Space and drag) moves around, and **Fit** shows it all.

## Task 11: a transistor amplifier and its frequency response

The simulator models NPN and PNP transistors (Ebers-Moll: beta 100 by default, `fields=beta=250` to change it) and N- and P-channel MOSFETs (square law: `fields=vth=2;kp=0.1`). `circuit.sim_ac` sweeps frequency and reports gain and phase: the source with an `ac=` field drives the sweep (otherwise the first voltage source, at 1 V).

A common-emitter stage on 12 V, biased by a 100k/22k divider:

**Batch** (`amp.txt`):

```text
circuit.new title=amp -> c
circuit.place doc=$c symbol=npn ref=Q1
circuit.place doc=$c symbol=resistor ref=R1 value=100k
circuit.place doc=$c symbol=resistor ref=R2 value=22k
circuit.place doc=$c symbol=resistor ref=RC value=4.7k
circuit.place doc=$c symbol=resistor ref=RE value=1k
circuit.place doc=$c symbol=capacitor ref=CIN value=10u
circuit.place doc=$c symbol=capacitor ref=CE value=100u
circuit.place doc=$c symbol=vsource ref=V1 value=0 fields=ac=1
circuit.power doc=$c symbol=+12v pins=R1.1,RC.1
circuit.net doc=$c name=IN pins=V1.1,CIN.1
circuit.net doc=$c name=B pins=CIN.2,R1.2,R2.1,Q1.1
circuit.net doc=$c name=OUT pins=RC.2,Q1.3
circuit.net doc=$c name=E pins=Q1.2,RE.1,CE.1
circuit.power doc=$c symbol=gnd pins=R2.2,RE.2,CE.2,V1.2
circuit.sim_op doc=$c
circuit.sim_ac doc=$c start=1 stop=10meg probes=OUT path=amp.csv preview=true
```

The operating point puts the collector at 6.06 V (about half the supply, as you want), and the sweep:

```text
ac sweep 1Hz to 10MHz, 141 points:
  OUT: 11.1 dB at 1Hz, 47.2 dB at 10MHz, -3 dB at 88.1Hz
f3db(OUT): 88.0653
gain_db(OUT): 47.2186
```

47 dB is a gain of about 230, which is gm x RC for 1.27 mA. The low corner comes from the coupling and bypass capacitors. The transistor model has no junction capacitance, so the gain stays flat at the top end where a real 2N3904 would roll off in the tens of MHz. `amp.csv` has `freq,dB(OUT),deg(OUT)`.

**Claude.** *"Design me a common-emitter amp with about 40 dB of gain from 100 Hz up."* Claude places the parts, runs `circuit_sim_op` to check the bias and `circuit_sim_ac` to read `gain_db` and `f3db`, and adjusts RC, RE or the capacitors until both numbers are right.

## Task 12: multi-unit parts and buses

Quad and dual parts come as separate units you can put anywhere on the sheet: `opamp_dual` (units A, B and the power unit), `opamp_quad`, `nand_quad` (a 74HC00) and `inverter_hex` (a 74HC04). Placing one puts every unit side by side; `unit=` places one unit on its own, and `ref=` joins it to a part already there. Pins are numbered as on the package, so `U1.1` is unit A's output wherever unit A is drawn, and the footprint goes on the first unit only.

```text
circuit.place doc=$c symbol=opamp_quad
circuit.place doc=$c symbol=nand_quad ref=U2 unit=1 x=40 y=10
circuit.place doc=$c symbol=nand_quad ref=U2 unit=2 x=40 y=20
```

A **bus** is one thick line carrying many nets. Name it like KiCad does, `D[0..7]` for D0 to D7 or `{SDA,SCL}` for a group, and tap pins onto it; each tap gets a short entry and a label, so the nets join exactly as if you had wired them:

```text
circuit.bus doc=$c name=D[0..7] from=30,5 to=30,40
circuit.bus_tap doc=$c bus=D pins=U1.2..9
circuit.bus_tap doc=$c bus=D pins=J1.1..8
```

`U1.2..9` is pins 2 to 9 in order. KiCad buses and bus entries open and save both ways.

## Task 13: push and shove, length tuning and any angle

`circuit.trace shove=true` pushes other nets' traces aside instead of overlapping them: they bend around the new trace with 45 degree jogs and stay connected, and traces they bump push the next ones (three levels deep). Pads and vias never move, so a push that would hit one is left as it was and reported. The window's Trace tool shoves by default.

```text
circuit.trace doc=$c points=12,10.9,28,10.9 layer=F shove=true
1 trace segments on F.Cu; pushed 1 trace aside
```

`circuit.length` prints the copper length of each net, and `circuit.tune` stretches one with meanders (trombone bumps) until it is a given length or as long as another net, the way memory buses and USB pairs are matched:

```text
circuit.length doc=$c nets=D0,D1
circuit.tune doc=$c net=D1 match=D0
circuit.tune doc=$c net=CLK length=40 amplitude=1.2 spacing=0.6
Net-(J1-2): 32 mm -> 40 mm (target 40)
```

Bumps go on the net's longest straight traces, on whichever side has room; meanders only add length, so the reply says when a net is already longer or there was no room.

Footprints turn to any whole degree (`circuit.footprint_move ref=U1 rot=30`). Turned rectangular pads are drawn, checked and written to Gerber as their true slanted shape; KiCad files keep the angle.

## Task 14: make your own footprints

When a part isn't in the built-in list, make its footprint from the land pattern numbers on the datasheet. It goes in your footprint library, a folder of KiCad `.kicad_mod` files (`footprints` next to the suite's settings, or the folder in `WETWARESUITE_FOOTPRINTS`), so later circuits and KiCad itself can use it:

```text
circuit.footprint_make name=SOP-16_W7.5 kind=dual pins=16 pitch=1.27 span=9.4 pad_w=0.6 pad_h=2
circuit.footprint_make name=VQFN-24_4x4 kind=quad pins=24 pitch=0.5 span=3.9 pad_w=0.25 pad_h=0.8 exposed=2.6
circuit.footprint_make name=BGA-36_0.8 kind=grid pins=36 pitch=0.8 pad_w=0.4
circuit.place doc=$c symbol=ic pins=24 footprint=VQFN-24_4x4
```

Kinds: `smd2` (two-pad chips; pitch is pad centre to pad centre), `row` (headers and connectors), `dual` (SOIC/SOP/DIP; `span` is row centre to row centre), `quad` (QFP/QFN; `span` is across the pads, `exposed` the centre pad), `grid` (BGA; balls are named A1, A2 ... B1, skipping I, O, Q, S, X and Z) and `blank`. `pad_w` is a pad's width along its row, `pad_h` its length out from the body; `drill=` makes through-hole pads.

`circuit.footprint_pad` edits the footprint on one part: move a pad (`x`, `y` or `dx`, `dy`), resize it, change its shape, hole or number, add one (a number the footprint doesn't have) or remove one. Without changes it lists the pads. The part gets its own copy, which the circuit file keeps, and `circuit.footprint_save` puts it in your library (or `path=` copies in a `.kicad_mod` from anywhere; `list=true` lists the library).

```text
circuit.footprint_pad doc=$c ref=U2 number=17 x=0 y=0 w=2.4 h=6 name=SOP-16_EP
circuit.footprint_save doc=$c ref=U2
```

**Claude.** *"Here's the datasheet land pattern for a TPS63020 (VSON-14, 0.5 mm pitch, 3x4 mm): make the footprint and put it on U3."* Claude calls `circuit_footprint_make` with `kind=dual`, the pitch, span and pad sizes, `preview=true` to compare it with the drawing, then `ref=U3`, and `circuit_footprint_pad` for the thermal pad.

## Task 15: the board in 3D, out to cad

`circuit.export path=board.step` (or `.stl`, `.obj`, `.glb`, `.3mf`) writes the board as a solid: the outline with its holes (mounting holes and anything 2 mm or bigger) and a body for each part, at its height. Open it in cad to design an enclosure around it, or in model to render it.

## Looking things up

```sh
$ wetwaresuite circuit.symbols category=module
4 symbols:
arduino_uno [module]: Arduino Uno R3 shield headers (aliases: uno)
arduino_nano [module]: Arduino Nano 2x15 (aliases: nano)
esp32_devkit [module]: ESP32 DevKit V1, 30 pins (aliases: esp32)
bluepill [module]: STM32F103 Blue Pill 2x20 (aliases: stm32, blue_pill)
count: 4
```

```sh
$ wetwaresuite circuit.symbol_info symbol=opamp
opamp: Op-amp (2 = -, 3 = +, 6 = out, 7 = V+, 4 = V-)
prefix U, default value TL071, footprint SOIC-8
pins:
  2 - (input) at -3,-1
  3 + (input) at -3,1
  6 OUT (output) at 3,0
  7 V+ (power_in) at 0,-3
  4 V- (power_in) at 0,3
pins: 5
footprint: SOIC-8
```

### Symbols

`circuit.symbols` lists all 47:

| symbol | aliases | pins | default footprint |
|---|---|---|---|
| `resistor` | r, res | 1, 2 | R_0805 |
| `capacitor` | c, cap | 1, 2 | C_0805 |
| `capacitor_polarized` | cp, electrolytic | 1 = +, 2 | CP_THT |
| `inductor` | l | 1, 2 | L_0805 |
| `potentiometer` | pot, rv | 1, 2 = wiper, 3 | Pot_THT |
| `crystal` | xtal, y | 1, 2 | HC49 |
| `diode` | d | 1 = K, 2 = A | D_SOD-123 |
| `zener` | dz | 1 = K, 2 = A; value = breakdown voltage | D_SOD-123 |
| `schottky` | d_schottky | 1 = K, 2 = A | D_SOD-123 |
| `led` | | 1 = K, 2 = A | LED_0805 |
| `fuse` | f, polyfuse | 1, 2 | Fuse_1206 |
| `ferrite_bead` | fb, ferrite | 1, 2 | L_0805 |
| `buzzer` | bz, piezo | 1 = +, 2 | Buzzer_12mm |
| `npn`, `pnp` | transistor, q_npn, q_pnp | 1 = B, 2 = E, 3 = C | SOT-23 |
| `nmos`, `pmos` | mosfet, q_nmos, q_pmos | 1 = G, 2 = S, 3 = D | SOT-23 |
| `opamp` | op-amp | 2 = -, 3 = +, 6 = OUT, 7 = V+, 4 = V- | SOIC-8 |
| `regulator` | vreg, 7805 | 1 = IN, 2 = GND, 3 = OUT | TO-220 |
| `ams1117` | ldo_sot223, lm1117 | 1 = GND/ADJ, 2 = OUT, 3 = IN | SOT-223 |
| `lm317` | | 1 = ADJ, 2 = OUT, 3 = IN | TO-220 |
| `ne555` | 555, timer | 1 GND, 2 TR, 3 Q, 4 R, 5 CV, 6 THR, 7 DIS, 8 VCC | DIP-8 |
| `lm358` | dual_opamp | 1 OUTA, 2 -A, 3 +A, 4 V-, 5 +B, 6 -B, 7 OUTB, 8 V+ | SOIC-8 |
| `atmega328p` | atmega328, mega328 | 28 pins, also named D0..D13, A0..A5, RX, TX | DIP-28 |
| `atmega328p_au` | atmega328p-au | 32 pins, TQFP | TQFP-32 |
| `attiny85` | tiny85 | 8 pins (PB0..PB5, VCC, GND) | DIP-8 |
| `rp2040` | | 57 pins (GPIO0..29, QSPI, USB, power; pad 57 = ground pad) | QFN-56 |
| `ic` | chip | `pins=N`, names from `pin_names=` | DIP-N |
| `switch` | button, sw, pushbutton | 1, 2 (normally open) | SW_PUSH_6mm |
| `switch_spdt` | spdt, toggle | 1, 2 = common, 3 | SW_SPDT |
| `header` | conn, connector, j | `pins=N`, one row | PinHeader_1xNN |
| `header2` | conn2 | `pins=N` total, two rows | PinHeader_2xNN |
| `screw_terminal` | terminal | `pins=N`, 5.08 mm | TerminalBlock_1xNN |
| `usb_micro` | usb | VBUS, D-, D+, ID, GND, shield | USB_Micro-B |
| `test_point` | tp | 1 (not on the BOM) | TestPoint_Pad_1.5mm |
| `mounting_hole` | hole | no pins (not on the BOM) | MountingHole_3.2mm |
| `arduino_uno` | uno | 32: power header, A0..A5, D0..D13, AREF, SDA, SCL | Arduino_Uno_Shield |
| `arduino_nano` | nano | 30 pins as on the board | Arduino_Nano |
| `esp32_devkit` | esp32 | 30 pins, DOIT V1 | ESP32_DevKit |
| `bluepill` | stm32, blue_pill | 40 pins | BluePill |
| `esp32_wroom` | wroom, esp32-wroom-32 | 38 pads + ground pad | ESP32-WROOM-32 |
| `rpi_pico` | pico | 40 pins as on the board | RPi_Pico |
| `gnd` | ground | net GND | none |
| `power` | vcc, +5v, +3v3, +12v, vdd | net named by the value | none |
| `vsource` | v | 1 = +, 2 = - | none |
| `isource` | i | 1, 2 | none |
| `battery` | bt, bat | 1 = +, 2 | PinHeader_1x02 |

### Footprints

`circuit.footprints` lists the 38 families: `R/C/L/LED_0402/0603/0805/1206` (LED pad 1 = cathode), `Fuse_0805/1206`, `D_SOD-123`, `D_SOD-323`, `D_SMA`, `D_0805`, `R_THT`/`L_THT` (axial, 10.16 mm), `D_THT` (DO-41), `C_THT` (disc, 5.08 mm), `CP_THT` (radial, 2.54 mm), `LED_THT` (5 mm), `HC49`, `SOT-23`, `SOT-23-5`, `SOT-23-6`, `SOT-223`, `TO-92`, `TO-220`, `SOIC-N`, `TSSOP-N` (0.65 mm), `TQFP-N` (32/44 at 0.8 mm, 48/64/100 at 0.5 mm; `LQFP-N` too), `QFN-N` (with the exposed pad as pad N+1), `DIP-N`, `PinHeader_1xNN`, `PinHeader_2xNN`, `TerminalBlock_1xNN`, `USB_Micro-B`, `TestPoint_Pad_1.5mm`, `MountingHole_2.2/2.7/3.2/4.3mm` (non-plated, for M2 to M4), `Buzzer_12mm`, `SW_PUSH_6mm`, `SW_SPDT`, `Pot_THT`, `Arduino_Uno_Shield`, `Arduino_Nano`, `ESP32_DevKit`, `BluePill`, `RPi_Pico`, `ESP32-WROOM-32`. KiCad library names such as `Package_SO:SOIC-8_3.9x4.9mm_P1.27mm` map onto them. Footprints you make (task 14) are listed after them.

## Command reference

`doc` (the id from `circuit.new` or `circuit.open`, e.g. `ckt1`) is required by every command that works on a circuit and is left out of the tables. Every editing command also takes `preview=true`.

### Files

| command | parameters | notes |
|---|---|---|
| `circuit.new` | `title` | an empty circuit; prints its id |
| `circuit.open` | `path` (required), `board` (default true) | `.circuit.json` / `.json`, KiCad `.kicad_sch` (with the `.kicad_pcb` beside it unless `board=false`), `.kicad_pcb`, `.kicad_pro` or `.net` |
| `circuit.save` | `path` (required) | `.circuit.json`, or KiCad `.kicad_sch`, `.kicad_pcb`, `.kicad_pro` (all three files) |
| `circuit.import_board` | `path` (required) | a KiCad `.kicad_pcb` onto this circuit's parts (undoable) |
| `circuit.view` | `view` (required: schematic, pcb, 3d) | what the window shows |
| `circuit.hit` | `x`, `y` (required, pixels of the window's picture), `view` | what is there: pin, label, wire, part; pad, via, trace, footprint; with the point in grid units or mm |
| `circuit.close` | | |
| `circuit.info` | | parts, nets, wires, board, traces, vias, pours |
| `circuit.symbols` | `category`: passive, semiconductor, ic, switch, connector, module, power, source | |
| `circuit.symbol_info` | `symbol` (required), `pins`, `pin_names` | pin positions relative to the part |
| `circuit.footprints` | | |
| `circuit.preview` | `view` (schematic, pcb, 3d), `grid`, `highlight` (a net), `layers` (both, top, bottom, inner), `from_below`, `ratsnest` (default true), `net_names`, `angle`, `tilt`, `mask` (3d), `size` (default 1400 px) | a PNG for Claude; writes nothing |
| `circuit.export` | `path` (required), `view`, `grid`, `highlight`, `layers`, `from_below`, `ratsnest`, `net_names`, `angle`, `tilt`, `mask`, `scale` (px per grid unit or per mm; the width for 3d) | `.svg`, `.png`, or `.pdf` (schematic only); 3d is `.png`; `.step`, `.stl`, `.obj`, `.glb`, `.3mf` write the board as a solid |

### Schematic

| command | parameters | notes |
|---|---|---|
| `circuit.place` | `symbol` (required), `ref`, `value`, `unit` (multi-unit parts), `x`, `y` (grid units, default right of what is there), `rot` (0/90/180/270, counter-clockwise), `mirror`, `footprint`, `pins` (ic, header, header2), `pin_names` (ic), `fields` (`k=v;k2=v2`) | replies with every pin's position |
| `circuit.edit` | `ref` (required), `value`, `footprint`, `new_ref`, `pins`, `pin_names`, `fields` (`k=` removes k) | |
| `circuit.move` | `ref` (required), `x`, `y` or `dx`, `dy`, `rot`, `mirror` | wires on its pins follow |
| `circuit.remove` | `ref` (required) | with the wires and labels only it used, and its footprint |
| `circuit.connect` | `from`, `to`, or `pins` (a chain), `vertical_first` | wires around part bodies; falls back to labels |
| `circuit.wire` | `points` (required, `x1,y1,x2,y2,...` grid units) | |
| `circuit.net` | `name` (required), `pins` (required) | stub + label on each pin |
| `circuit.label` | `name` (required), `x`, `y` or `pin` | |
| `circuit.power` | `symbol` (gnd, vcc, +5v, +3v3, +12v, power), `pins`, `value` (for symbol=power, e.g. +9V), or `x`, `y` | |
| `circuit.no_connect` | `pins` (required) | |
| `circuit.unwire` | `pin`, `x`+`y`, `net`, or `all=true` | removes wires and labels |
| `circuit.annotate` | `renumber` (default false) | |
| `circuit.bus` | `name` (required: `D[0..7]` or `{A,B}`), `points` or `from`+`to`, `remove` | |
| `circuit.bus_tap` | `bus`, `pins` (required; `U1.2..9` ranges), `members` | an entry and a label per pin |

### Checks and outputs

| command | parameters | notes |
|---|---|---|
| `circuit.parts` | | refs, symbols, values, footprints, positions |
| `circuit.pins` | `ref` (required) | numbers, names, types, positions, nets |
| `circuit.nets` | `net` | |
| `circuit.erc` | | see [ERC checks](#erc-and-drc-checks) |
| `circuit.bom` | `path`, `grouped` (default true) | CSV: Item, Qty, References, Value, Footprint, Description, then any fields |
| `circuit.export_netlist` | `path` (required), `format` (auto, kicad, json, text) | |
| `circuit.sim_op` | `ignore_unsupported` | |
| `circuit.sim_tran` | `tstop` (required, e.g. `10m`), `step` (default tstop/500), `probes` (nets; default up to 8), `path` (CSV), `ignore_unsupported` | |
| `circuit.sim_ac` | `start` (default 10), `stop` (default 1meg), `per_decade` (default 20), `probes`, `path` (CSV), `ignore_unsupported` | gain, phase, -3 dB corners; driven by the source with `ac=` |

### Board

| command | parameters | notes |
|---|---|---|
| `circuit.board` | `width`, `height`, `x`, `y` (top-left, default 0,0) or `points` (polygon), `layers` (2 or 4) | mm; `layers` alone keeps the outline |
| `circuit.pcb_place` | `all` (default false), `spacing` (default 2 mm) | packs footprints by connection, mounting holes to the corners; makes an outline if there is none |
| `circuit.footprint_move` | `ref` (required), `x`, `y` (centre) or `dx`, `dy`, `rot` (any degree), `side` (top, bottom) | |
| `circuit.pads` | `ref`, `net` | positions in mm, sizes, layers, nets |
| `circuit.trace` | `points` (required, mm), `layer` (F, B, In1, In2; default F), `width`, `net`, `shove` | `shove=true` pushes other traces aside |
| `circuit.length` | `nets` | copper length per net, mm |
| `circuit.tune` | `net` (required), `length` or `match` (another net), `amplitude` (1 mm), `spacing` (0.5 mm) | meanders |
| `circuit.footprint_make` | `name`, `kind` (required: smd2, row, dual, quad, grid, blank), `pins`, `pitch`, `span`, `pad_w`, `pad_h`, `drill`, `shape`, `columns`, `exposed`, `ref`, `save` (default true) | into your library |
| `circuit.footprint_pad` | `ref` (required), `number`, `x`, `y`, `dx`, `dy`, `w`, `h`, `shape`, `drill`, `plated`, `new_number`, `remove`, `name` | edits that part's copy |
| `circuit.footprint_save` | `ref`, `name`, `path` (a .kicad_mod), `list` | |
| `circuit.via` | `x`, `y` (required), `net`, `drill`, `diameter` | |
| `circuit.route` | `nets`, `skip`, `layers` (1, 2 or 4; default 2), `via_cost` (default 8), `time_limit` (default 60 s) | keeps existing copper |
| `circuit.unroute` | `net`, `layer` (F, B, In1, In2) | all copper when neither is given |
| `circuit.pour` | `net` (default GND), `layer` (F, B, both, In1, In2; default B), `clearance` (default 0.3 mm), `thermal` (default true), `spoke` (default 0.5 mm) | refilled whenever the board changes |
| `circuit.fanout` | `nets` (default: nets with a pour) | a trace and a via from each SMD pad to its plane |
| `circuit.unpour` | `net` | |
| `circuit.rules` | `trace` (0.25), `power_trace` (0.5), `clearance` (0.2), `min_trace` (0.15), `via_drill` (0.4), `via_diameter` (0.8), `min_drill` (0.3), `edge_clearance` (0.3), `grid` (0.127) | mm; defaults in brackets suit cheap 2-layer fabs |
| `circuit.ratsnest` | | unrouted connections |
| `circuit.drc` | | see below |
| `circuit.gerber` | `path` (required; `.zip` or a folder), `name` (file prefix, default the title), `assembly` (default true: also the pick-and-place and BOM CSVs) | |
| `circuit.placement` | `path` | pick-and-place CSV (Designator, Val, Package, Mid X, Mid Y, Rotation, Layer; mm, y up from the bottom-left corner); printed when no path |

### ERC and DRC checks

**ERC** errors: unconnected pins (not on a module, not marked no-connect), two power names on one net (`+5V` shorted to `GND`), duplicate or missing references, outputs driving each other, a footprint with fewer pads than the symbol has pins. Warnings: labels that reach only one pin, dangling wire ends, power input pins with nothing driving the net.

**DRC** errors: shorts and clearance between different nets (pads, traces, vias, pours), traces under `min_trace`, drills under `min_drill`, annular ring (vias and through-hole pads) under `min_annular`, copper closer than `edge_clearance` to or outside the outline, copper on a layer the board doesn't have, overlapping footprints, parts not on the board, unrouted connections.

### Gerber files

| file | layer |
|---|---|
| `NAME-F_Cu.gtl`, `NAME-B_Cu.gbl` | copper, top and bottom |
| `NAME-In1_Cu.g2`, `NAME-In2_Cu.g3` | inner copper (4-layer boards) |
| `NAME-F_Mask.gts`, `NAME-B_Mask.gbs` | solder mask openings |
| `NAME-F_Paste.gtp`, `NAME-B_Paste.gbp` | paste (SMD pads) |
| `NAME-F_Silkscreen.gto`, `NAME-B_Silkscreen.gbo` | silkscreen: outlines and references |
| `NAME-Edge_Cuts.gm1` | board outline |
| `NAME.drl` | Excellon drill file, metric, plated holes |
| `NAME-NPTH.drl` | non-plated holes (mounting holes), when there are any |
| `NAME-pos.csv`, `NAME-bom.csv` | pick-and-place and bill of materials, for assembly (`assembly=false` leaves them out) |

RS-274X with X2 file attributes, coordinates in mm (4.6 format). Pours are written as regions.

## Limitations

- **Positions of the Arduino Uno and ESP32 DevKit headers are close but not guaranteed**: check them against your actual board before ordering (`circuit.pads ref=A1` prints them). The Nano and Blue Pill follow 2.54 mm grids and are exact for the common clones.
- **BJT pinout:** `npn`/`pnp` pins are 1 = B, 2 = E, 3 = C to match the SOT-23 footprint. Through-hole parts such as the 2N3904 in TO-92 are E-B-C; with `footprint=TO-92` the TO-92 pads are numbered 1-2-3 left to right, so with `footprint=TO-92` pin 1 (B) lands on the emitter. Use SOT-23 transistors, or check the pad order against the datasheet before ordering.
- Schematic rotations are multiples of 90 degrees; footprints take any whole degree. Turned round and oval pads are exact; DRC treats a turned rectangular pad as its upright bounding box, so it is a little cautious near one. Text is always horizontal.
- Multi-unit built-ins are `opamp_dual`, `opamp_quad`, `nand_quad` and `inverter_hex` (`lm358` is still one symbol with both halves). Hierarchical sheets are flattened on import and saved back as one flat sheet.
- A pin, label or wire end lying on the middle of a wire connects to it, as in KiCad. `circuit.connect` avoids this, but hand-drawn `circuit.wire` paths can make junctions you didn't mean: check `circuit.nets`.
- Power and ground symbols can crowd labels next to tightly packed module pins. The connection is right; for a cleaner drawing use `circuit.net name=GND pins=...` there instead.
- KiCad files: pours are whole-board here, so a KiCad zone with its own outline grows to the board's (with a warning); board cut-outs, blind and micro vias, more than four copper layers, curved board edges (kept as short straight pieces) and KiCad's design rules (they live in the `.kicad_pro`) are not carried over. Saved KiCad files embed every symbol and footprint and name built-in footprints `wetwaresuite:R_0805`, so KiCad shows them without a library but "Update PCB from Schematic" will want a library footprint for parts that never had one. A netlist (`.net`) has no drawing, so it is laid out on a grid with net labels.
- Traces, vias and pours keep the net name they were made with; after renaming a net on the schematic, unroute and re-route it.
- The autorouter is a grid maze router with a time limit, made for boards with tens of parts. Dense boards may leave connections unrouted; route those by hand or loosen the grid (`circuit.rules grid=0.25`). It doesn't route differential pairs or control impedance; match lengths afterwards with `circuit.tune`. Shoving moves traces only (never vias or footprints) and keeps them at 45 degrees.
- Pours fill the whole board on a 0.1 mm raster (no pour outlines of your own); pads join them through four thermal spokes, or solidly with `thermal=false`.
- Two or four copper layers. The autorouter uses inner layers only with `layers=4`; planes are usually better there.
- The 3D view and the STEP/STL export are boxes and cylinders by package, good for enclosures and clearances, not photo-real parts.
- The simulator handles R, C, L, diodes and LEDs (Shockley model), BJTs (Ebers-Moll), MOSFETs (square law with channel-length modulation), switches, potentiometers, ideal op-amps and sources. Transistors have no junction capacitance, so AC sweeps don't roll off at high frequency; no regulators or other ICs, no noise, no temperature; transient is backward Euler with a fixed step, so make `step` small compared with your time constants.
