# Using wetwaresuite with Claude (MCP)

Every command in wetwaresuite is also an MCP tool, so Claude can open, edit, preview and save your files for you. You describe what you want ("brighten this photo and make a web copy"), Claude calls the tools, looks at previews to check its work, and saves the result. Nothing is uploaded anywhere: the server runs on your machine and works on your files directly.

This tutorial covers connecting the server, what Claude sees, good prompts, and how to troubleshoot.

## 1. The server

```sh
wetwaresuite mcp
```

starts an MCP server on stdin/stdout (the standard "stdio" transport). You do not normally run it by hand: Claude Code or Claude Desktop starts it for you from the config below. It serves every tool in the suite, plus four session tools (`undo`, `redo`, `history`, `documents`).

### Serving only some tools

Fewer tools means less for Claude to read and fewer chances to pick the wrong one. Pass a comma-separated list of tool groups:

```sh
wetwaresuite mcp --tools photo,paint     # image editing and painting only
wetwaresuite mcp --tools vector          # vector drawing only (see vector.md)
wetwaresuite mcp --tools convert         # file conversion only
```

The groups are `photo`, `paint`, `vector`, `convert` and `toolkit`. The session tools are always included.

Groups bring what they need along: `paint` includes `photo` (opening, previewing and saving live there) and `convert` includes `vector` (it renders SVG). An unknown group name is an error, so typos don't silently serve nothing.

### Big servers list a catalog

The whole suite has over 1,300 tools, and listing them all costs Claude megabytes of schemas before it does anything. So a server with more than 150 tools (`wetwaresuite mcp`, and the office app) lists just three tools plus the session tools:

| tool | what it does |
| --- | --- |
| `find_tools` | search by job (`query="round edges"`) or list a family (`query="photo"`); gives names and one line each |
| `describe_tools` | the full parameters of some tools: `names="cad_fillet,cad_chamfer"` |
| `run_tool` | run one: `name="cad_fillet"`, `arguments={...}` |

Every tool still answers to its own name, so once Claude has found `photo_resize` it can call it directly. The server's instructions tell Claude the families and how many tools each has.

`--all-tools` lists every tool as before; `--catalog` uses the catalog even on a small server:

```sh
wetwaresuite mcp --all-tools
wetwaresuite-image mcp --catalog
```

## 2. Connect Claude Code

Use an absolute path to the binary:

```sh
claude mcp add wetwaresuite -- /path/to/wetwaresuite/build/apps/wetwaresuite/wetwaresuite mcp
```

Everything after `--` is the command Claude Code runs. To add a filtered server, or to make it available in every project rather than just the current one:

```sh
claude mcp add wetwaresuite-art -s user -- /path/to/wetwaresuite/build/apps/wetwaresuite/wetwaresuite mcp --tools photo,paint
```

`-s` (scope) can be `local` (the default: this project, just for you), `project` (shared with your team through a `.mcp.json` file in the repo) or `user` (all your projects).

Check that it connects:

```sh
$ claude mcp list
Checking MCP server health…

wetwaresuite-art: /path/to/wetwaresuite/build/apps/wetwaresuite/wetwaresuite mcp --tools photo,paint - √ Connected
wetwaresuite: /path/to/wetwaresuite/build/apps/wetwaresuite/wetwaresuite mcp - √ Connected
```

Inside a Claude Code session, `/mcp` shows the same status and lets you browse the tools.

## 3. Connect Claude Desktop

Open Claude Desktop's config file (Settings, Developer, Edit Config), which lives at:

- macOS: `~/Library/Application Support/Claude/claude_desktop_config.json`
- Windows: `%APPDATA%\Claude\claude_desktop_config.json`

and add an entry under `mcpServers`:

```json
{
  "mcpServers": {
    "wetwaresuite": {
      "command": "/path/to/wetwaresuite/build/apps/wetwaresuite/wetwaresuite",
      "args": ["mcp"]
    }
  }
}
```

For a filtered server, put the extra arguments in `args`: `"args": ["mcp", "--tools", "photo,paint"]`. On Windows, write paths with doubled backslashes, for example `"C:\\Tools\\wetwaresuite\\wetwaresuite.exe"`. Restart Claude Desktop after saving.

### Let the hub write it for you

The `hub` install manager prints a ready-made config pointing at your binaries:

```sh
cd hub && cargo build --release
./target/release/hub mcp-config --bin-dir /path/to/wetwaresuite/build/apps/wetwaresuite
```

```json
{
  "mcpServers": {
    "wetwaresuite": {
      "command": "/path/to/wetwaresuite/build/apps/wetwaresuite/wetwaresuite",
      "args": ["mcp"]
    }
  }
}
```

Without `--bin-dir` it uses the folder the `hub` binary itself is in, which is what you want once the suite is installed side by side. Paste the output into Claude Desktop's config, or pass the same command and args to `claude mcp add`.

### Or let the hub do all of it

`hub connect` finds Claude Desktop and Claude Code on your computer and adds the entry to their config files for you, with a backup first. Run it again any time; `hub connect --remove` takes it out. See [ai-connect.md](ai-connect.md), which also covers serving over HTTP (`wetwaresuite mcp --http`) to another computer or a container.

### The standalone toolkit server

`wetwaresuite-toolkit` (built at `build/tools/toolkit/wetwaresuite-toolkit`) has its own tiny MCP server with just the file-identification tools:

```sh
claude mcp add wetwaresuite-toolkit -- /path/to/wetwaresuite/build/tools/toolkit/wetwaresuite-toolkit mcp
```

It offers `toolkit_inspect` and `toolkit_hex` plus the four session tools. The suite binary already includes these, so you only need the standalone one if you want a very small server. See [toolkit.md](toolkit.md).

## 4. What Claude sees

### Instructions

When Claude connects, the server introduces itself with a short workflow guide: open or create a document to get an id like `img1`, pass it as `doc`, look at previews, save; everything is undoable; coordinates are pixels from the top-left; colours accept `#rrggbb`, `#rrggbbaa`, `rgb(r,g,b)` or names; strokes take a flat `[x1,y1,x2,y2,...]` list.

### Tool names

Each command becomes a tool with the dot replaced by an underscore:

| Command | MCP tool |
| --- | --- |
| `photo.open` | `photo_open` |
| `paint.layer_add` | `paint_layer_add` |
| `convert.file` | `convert_file` |
| `vector.add_rect` | `vector_add_rect` |
| `toolkit.inspect` | `toolkit_inspect` |

plus the session tools:

| Tool | What it does |
| --- | --- |
| `undo` | undo the last change to any open document |
| `redo` | redo the last undone change |
| `history` | list the undo history, oldest first (by command title, e.g. `Brush stroke`) |
| `documents` | list open documents with id, kind, size, layers and file path |

### Schemas

Each tool's input schema is generated from the same parameter list `wetwaresuite help` prints: types (`string`, `integer`, `number`, `boolean`, `array` of numbers), descriptions, `enum` choices, defaults and which parameters are required. Here is part of `paint_stroke`:

```json
{
  "name": "paint_stroke",
  "title": "Brush stroke",
  "inputSchema": {
    "type": "object",
    "properties": {
      "doc": { "type": "string", "description": "id of an open image, e.g. img1" },
      "points": { "type": "array", "items": { "type": "number" },
                  "description": "stroke path as x,y pairs in px, e.g. [10,10, 50,40, 90,10]" },
      "pressures": { "type": "array", "items": { "type": "number" },
                     "description": "optional pen pressure per point, 0..1 (from a tablet)" },
      "brush": { "type": "string", "enum": ["airbrush","ink","marker","pen","pencil","round","soft"], "default": "round" },
      "opacity": { "type": "number", "default": 100.0, "description": "stroke opacity 0..100" }
    },
    "required": ["doc", "points"]
  },
  "annotations": { "title": "Brush stroke", "readOnlyHint": false, "openWorldHint": false }
}
```

### Results

A tool result has a text part (the same text the CLI prints) and, where useful, `structuredContent` with the same fields as typed values:

```json
{
  "content": [{ "type": "text", "text": "opened dark.jpg as img1\ndoc: img1\nwidth: 1200\nheight: 800\nlayers: 1" }],
  "isError": false,
  "structuredContent": { "doc": "img1", "width": 1200, "height": 800, "layers": 1 }
}
```

Failures come back as a normal result with `isError: true` and a message Claude can act on, such as `point is outside the image` or `pressures needs one value (0..1) per point: got 2 for 3 points`. Calling a tool name that does not exist is a JSON-RPC error.

### Seeing the picture

Claude can look at its work in three ways:

- **`preview=true`** on almost any editing tool attaches a PNG of the result (longest side 512 px) as an MCP image, right in the same reply. This is the cheapest way for Claude to check a step.
- **`photo_preview`** renders the current image on demand; `max_size` (16..4096, default 512) sets the size.
- **Resources.** Every open document is listed as a resource `wetwaresuite://doc/<id>` (for example `wetwaresuite://doc/img1`). Reading it returns a text summary and a PNG preview. The template `wetwaresuite://doc/{id}` is advertised too.

## 5. Example prompts

Be specific about files (use absolute paths, see troubleshooting) and about what "done" means. Claude chooses the tools; the lists below show what it typically calls.

**Fix an underexposed photo**

> Open /home/me/Pictures/party.jpg, it came out too dark. Fix the exposure without blowing out the highlights, show me a preview, then save it as party-fixed.jpg next to the original.

`photo_open`, `photo_levels` (`auto=true`, `preview=true`), `photo_adjust` (`kind=exposure` or `gamma`), maybe `photo_pick_color` to check a highlight, `photo_save`.

**Resize a folder for the web**

> Make web copies of everything in /home/me/Pictures/trip: longest side 1600 px, JPEG quality 82, into /home/me/Pictures/trip-web.

`photo_batch` (`max_size=1600`, `quality=82`).

**Paint something**

> Make a 1200x630 social card: a dark teal gradient, a gold circle in the top right, and a white brush stroke underline in the bottom left that tapers like a pen stroke. Save it as /home/me/card.png and keep a layered copy as card.ora.

`paint_new`, `paint_gradient`, `paint_layer_add`, `paint_shape`, `paint_stroke` (with `pressures`), `photo_preview`, `photo_save` twice.

**Convert files**

> Convert /home/me/podcast/ep12.wav to MP3 and /home/me/docs/report.docx to PDF.

`convert_formats` or `convert_check` to make sure ffmpeg and LibreOffice are present, then `convert_file` twice.

**Draw a vector logo**

> Draw a simple 400x400 SVG logo: a navy rounded square with a gold circle in the middle. Save it as /home/me/logo.svg and export a 2x PNG.

`vector_new`, `vector_add_rect`, `vector_add_ellipse`, `vector_preview`, `vector_save`. The [vector tutorial](vector.md) covers this in depth.

**Identify a mystery file**

> What is /home/me/Downloads/file.dat, and which tool should open it?

`toolkit_inspect`, maybe `toolkit_hex`.

**Change your mind**

> Undo the last two changes and show me what it looks like now.

`undo` twice, `photo_preview`.

## 6. Try it by hand

You can talk to the server yourself to see exactly what Claude sees. Each request is one line of JSON:

```sh
cat > session.jsonl <<'EOF'
{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"me","version":"0"}}}
{"jsonrpc":"2.0","method":"notifications/initialized"}
{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"photo_open","arguments":{"path":"dark.jpg"}}}
{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"photo_levels","arguments":{"doc":"img1","auto":true}}}
{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"history","arguments":{}}}
{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"photo_save","arguments":{"doc":"img1","path":"dark-fixed.png"}}}
EOF
wetwaresuite mcp < session.jsonl
```

You get one JSON response per request (notifications get none), for example:

```json
{"id":4,"jsonrpc":"2.0","result":{"content":[{"text":"Levels\n","type":"text"}],"isError":false}}
```

To list the tools: `echo '{"jsonrpc":"2.0","id":1,"method":"tools/list"}' | wetwaresuite mcp`.

## 7. Troubleshooting

**Claude says the server failed to start, or shows no tools.**
Use an absolute path to the binary and check it runs: `/path/to/wetwaresuite --version`. Then pipe in a `tools/list` request as above; if that prints JSON, the server is fine and the problem is in the config (a typo in the path, or `"args"` missing `"mcp"`). In Claude Code, `claude mcp list` shows connection status. Restart Claude Desktop after editing its config.

**"file not found" for a file that exists.**
Relative paths are resolved against the server's working directory, which is not necessarily your project folder (Claude Desktop in particular starts servers elsewhere). Give Claude absolute paths, or ask it to use them.

**Claude edited the wrong thing or lost track of an image.**
Ask it to call `documents`. Ids are never reused within a session, so `img1` always means the first image opened (vector drawings are `vec1`, `vec2`, ...).

**My open images disappeared.**
Documents live in the server process. When the client restarts the server (new conversation, app restart), everything not saved is gone. Ask Claude to save as it goes, and to keep layered work as `.ora`.

**Conversions of audio, video or office files fail.**
Those use optional helper programs (ffmpeg, LibreOffice, pandoc). Ask Claude to run `convert_formats`: each backend shows `ready` or `not installed`. See [convert.md](convert.md).

**Too many tools / Claude picks odd tools.**
Serve only what the task needs with `--tools`, for example `--tools photo,paint` for image work.

**Your client asks permission for every call.**
That is the client's choice. In Claude Code you can allow the server's tools in `/permissions` (for example allow `mcp__wetwaresuite`). Tools that only look (`photo_info`, `photo_preview`, `convert_formats`, ...) are marked `readOnlyHint: true`; tools that write files or discard a document without undo (`photo_save`, `convert_file`, `photo_close`, ...) are marked `readOnlyHint: false` and `destructiveHint: true`, so a client can auto-approve the first kind safely.
