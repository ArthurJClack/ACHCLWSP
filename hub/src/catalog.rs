//! The tools and the apps that group them. Tool sizes are early plan
//! estimates; app sizes are measured release binaries.

#[derive(Debug, Clone, Copy)]
pub struct Tool {
    pub id: &'static str,
    pub name: &'static str,
    pub summary: &'static str,
    pub approx_mb: u32,
}

/// The all-in-one binary: every tool, `wetwaresuite mcp` serves them all.
pub const SUITE: &str = "wetwaresuite";

/// `share` is its own small Rust binary (no C++ core), with its own MCP server.
pub const SHARE: &str = "wetwaresuite-share";

const fn tool(id: &'static str, name: &'static str, summary: &'static str, approx_mb: u32) -> Tool {
    Tool { id, name, summary, approx_mb }
}

pub const TOOLS: &[Tool] = &[
    tool("toolkit", "toolkit", "open anything: inspect, hex, archives, code", 10),
    tool("photo", "photo", "photo editing: adjust, crop, resize, effects, batch", 4),
    tool("paint", "paint", "digital painting with pen pressure and layers", 4),
    tool("convert", "convert", "change any file's type: images, audio, video, docs", 2),
    tool("vector", "vector", "vector design, SVG/AI/EPS", 10),
    tool("share", "share", "share and live-sync folders over LAN or internet", 1),
    tool("cad", "cad", "precision parametric CAD: feature tree, expressions, holes, patterns, STL/3MF", 3),
    tool("blueprint", "blueprint", "technical drafting and floor plans: DXF, PDF to scale", 2),
    tool("slicer", "slicer", "3D printing slicer: STL/3MF to G-code", 20),
    tool("fit", "scan fit", "car parts and mounts that fit phone 3D scans", 3),
    tool("forge", "asset forge", "game assets: meshes, textures, sprites", 25),
    tool("audio", "audio", "multitrack and spectral audio editing", 12),
    tool("cut", "cut", "video editing", 20),
    tool("capture", "capture", "screen recording and screenshots", 5),
    tool("office", "office", "documents, spreadsheets, slides, PDF", 5),
    tool("pdf", "pdf", "PDF editor: mark up, edit text, fill forms, sign, redact, pages", 3),
    tool("layout", "layout", "print layout and publishing", 10),
    tool("library", "library", "asset browser for every format", 3),
    tool("automate", "automate", "Python scripting across tools", 20),
    tool("model", "model", "polygon modeling like Blender: edit mode, modifiers, UVs, materials", 3),
    tool("sculpt", "sculpt", "digital sculpting and retopology", 15),
    tool("render", "render", "3D scenes and path-traced renders", 20),
    tool("animate", "animate", "2D and 3D animation", 15),
    tool("levels", "levels", "game level and tilemap editor", 10),
    tool("scene", "scene", "Godot-style scene editor: node tree, inspector, .tscn", 2),
    tool("beats", "beats", "music making: sequencer, synths, samples", 15),
    tool("circuit", "circuit", "schematics and PCB layout", 25),
    tool("flash", "flash", "flash firmware to Arduino, ESP32 and STM32", 5),
    tool("maps", "maps", "offline maps and GIS", 15),
    tool("board", "board", "whiteboard and diagrams", 5),
    tool("notes", "notes", "markdown notes and knowledge base", 5),
    tool("type", "type", "font design", 8),
    tool("calc", "calc", "calculator: scientific, programmer, units, statistics, solving, graphs", 1),
    tool("code", "code", "code editor for many languages: run, build, search, git", 2),
    tool("explorer", "explorer", "file manager: copy, move, trash with undo, search, zip and tar", 1),
    tool("terminal", "terminal", "terminal: run commands in the system shell", 1),
    tool("monitor", "monitor", "system monitor: processor, memory, disks, processes, network", 1),
    tool("clock", "clock", "clock: world time, timers, stopwatch, alarms, date maths", 1),
];

/// An app: what people install. Each is its own binary `wetwaresuite-<id>` that
/// carries a few related tools; `files` is small and always installed.
#[derive(Debug, Clone, Copy)]
pub struct App {
    pub id: &'static str,
    pub summary: &'static str,
    pub tools: &'static [&'static str],
    /// Size of the release binary (Linux x86-64, rounded up).
    pub approx_mb: u32,
    /// Installed with every other app (opening, converting and finding files).
    pub always: bool,
}

/// One row per app, in the order `hub list` shows them. Keep in step with
/// `apps/apps.hpp` on the C++ side (a test there checks the same grouping).
pub const APPS: &[App] = &[
    App { id: "image", summary: "edit photos, paint pictures and make textures", tools: &["photo", "paint"], approx_mb: 2, always: false },
    App { id: "draw", summary: "vector art and logos, fonts, whiteboards and diagrams", tools: &["vector", "type", "board"], approx_mb: 5, always: false },
    App { id: "model", summary: "3D modeling and sculpting, rendered pictures, animation", tools: &["model", "sculpt", "render", "animate"], approx_mb: 5, always: false },
    App { id: "game", summary: "game scenes (Godot .tscn), assets (meshes, sprites, textures) and levels", tools: &["scene", "forge", "levels"], approx_mb: 4, always: false },
    App { id: "cad", summary: "precise parts, technical drawings and floor plans, 3D printing, parts that fit 3D scans", tools: &["cad", "blueprint", "slicer", "fit"], approx_mb: 5, always: false },
    App { id: "electronics", summary: "circuit diagrams, circuit boards and flashing microcontrollers", tools: &["circuit", "flash"], approx_mb: 4, always: false },
    App { id: "sound", summary: "record and clean up audio, make music", tools: &["audio", "beats"], approx_mb: 4, always: false },
    App { id: "video", summary: "edit videos, take screenshots and record the screen", tools: &["cut", "capture"], approx_mb: 3, always: false },
    App { id: "office", summary: "documents, spreadsheets, slides, a PDF editor, print layouts and notes", tools: &["office", "pdf", "layout", "notes"], approx_mb: 7, always: false },
    App { id: "maps", summary: "offline maps, GPS tracks and map data", tools: &["maps"], approx_mb: 4, always: false },
    App { id: "system", summary: "calculator, code editor, file manager, terminal, system monitor and clock", tools: &["calc", "code", "explorer", "terminal", "monitor", "clock"], approx_mb: 6, always: false },
    App { id: "files", summary: "open, convert, find and batch-process any file (always installed)", tools: &["toolkit", "convert", "library", "automate"], approx_mb: 4, always: true },
    App { id: "share", summary: "share and live-sync folders with other devices", tools: &["share"], approx_mb: 1, always: false },
];

impl App {
    /// The executable name, without `.exe`.
    pub fn binary(&self) -> String {
        if self.id == "share" {
            SHARE.to_string()
        } else {
            format!("{SUITE}-{}", self.id)
        }
    }
}

/// Finds an app by its id, or by the id of a tool it contains ("photo" -> image).
pub fn find_app(id: &str) -> Option<&'static App> {
    let id = id.to_ascii_lowercase();
    if let Some(app) = APPS.iter().find(|a| a.id == id) {
        return Some(app);
    }
    let tool = find(&id)?;
    APPS.iter().find(|a| a.tools.contains(&tool.id))
}

pub fn find(id: &str) -> Option<&'static Tool> {
    let id = id.to_ascii_lowercase();
    TOOLS.iter().find(|t| t.id == id || t.name == id)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ids_are_unique() {
        for (i, a) in TOOLS.iter().enumerate() {
            for b in &TOOLS[i + 1..] {
                assert_ne!(a.id, b.id);
            }
        }
    }

    #[test]
    fn find_by_id_or_name() {
        assert_eq!(find("TOOLKIT").unwrap().id, "toolkit");
        assert_eq!(find("asset forge").unwrap().id, "forge");
        assert!(find("photoshop").is_none());
    }

    #[test]
    fn every_tool_is_in_exactly_one_app() {
        for t in TOOLS {
            let n = APPS.iter().filter(|a| a.tools.contains(&t.id)).count();
            assert_eq!(n, 1, "{} is in {n} apps", t.id);
        }
        for a in APPS {
            for t in a.tools {
                assert!(find(t).is_some(), "app {} lists unknown tool {t}", a.id);
            }
        }
    }

    #[test]
    fn apps_by_id_or_tool_alias() {
        assert_eq!(find_app("image").unwrap().id, "image");
        assert_eq!(find_app("Paint").unwrap().id, "image");
        assert_eq!(find_app("slicer").unwrap().id, "cad");
        assert_eq!(find_app("cad").unwrap().id, "cad");
        assert_eq!(find_app("asset forge").unwrap().id, "game");
        assert!(find_app("photoshop").is_none());
        assert_eq!(find_app("model").unwrap().binary(), "wetwaresuite-model");
        assert_eq!(find_app("share").unwrap().binary(), "wetwaresuite-share");
        assert!(APPS.iter().all(|a| a.approx_mb > 0));
    }
}
