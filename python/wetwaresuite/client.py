"""A small client for the ``wetwaresuite`` suite: it starts ``wetwaresuite mcp`` as a
subprocess and turns every command into a Python method, so

    with Suite() as suite:
        img = suite.photo.open(path="cat.jpg").doc
        suite.photo.resize(doc=img, width=800)
        suite.photo.save(doc=img, path="cat_small.jpg")

does what the same three lines do in a batch script. Pure standard library.
"""

from __future__ import annotations

import base64
import json
import os
import queue
import shutil
import subprocess
import sys
import threading
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Tuple, Union

__all__ = [
    "WetwaresuiteError",
    "CommandError",
    "Result",
    "Suite",
    "BatchResult",
    "find_binary",
    "run_batch",
]

PROTOCOL_VERSION = "2025-06-18"
_BUILTINS = ("undo", "redo", "history", "documents")


class WetwaresuiteError(Exception):
    """The suite couldn't be started, or stopped talking."""


class CommandError(WetwaresuiteError):
    """A command ran and failed. ``.result`` holds the full Result."""

    def __init__(self, command: str, result: "Result") -> None:
        super().__init__(f"{command}: {result.text}")
        self.command = command
        self.result = result


def find_binary(binary: Union[str, os.PathLike, None] = None) -> str:
    """Where the ``wetwaresuite`` executable is: the argument, $WETWARESUITE_BIN,
    ``wetwaresuite`` on PATH, or a build folder next to this package (a checkout)."""
    if binary:
        return os.fspath(binary)
    env = os.environ.get("WETWARESUITE_BIN")
    if env:
        return env
    found = shutil.which("wetwaresuite")
    if found:
        return found
    exe = "wetwaresuite.exe" if os.name == "nt" else "wetwaresuite"
    repo = Path(__file__).resolve().parent.parent.parent
    for candidate in (
        repo / "build" / "apps" / "wetwaresuite" / exe,
        repo / "build" / "apps" / "wetwaresuite" / "Release" / exe,
    ):
        if candidate.is_file():
            return str(candidate)
    raise WetwaresuiteError(
        "can't find the wetwaresuite binary: put it on PATH, set WETWARESUITE_BIN, or pass Suite(binary=...)"
    )


def _decode(value: Any) -> Any:
    # list-valued fields (search results, groups) arrive as JSON text
    if isinstance(value, str) and value[:1] in ("[", "{"):
        try:
            return json.loads(value)
        except ValueError:
            return value
    return value


class Result:
    """What a command returned.

    ``ok`` -- did it succeed; ``text`` -- the message a person would read;
    ``data`` -- the structured fields (``result["width"]`` works too; JSON
    lists such as search results are already decoded); ``images`` -- PNG
    (or other image) previews as bytes; ``attachments`` -- every attached file
    as ``(mime, bytes, name)``."""

    def __init__(self, raw: Dict[str, Any]) -> None:
        self.raw = raw
        self.ok: bool = not raw.get("isError", False)
        texts: List[str] = []
        self.images: List[bytes] = []
        self.attachments: List[Tuple[str, bytes, str]] = []
        for part in raw.get("content", []):
            kind = part.get("type")
            if kind == "text":
                texts.append(part.get("text", ""))
            elif kind == "image":
                data = base64.b64decode(part.get("data", ""))
                self.images.append(data)
                self.attachments.append((part.get("mimeType", "image/png"), data, ""))
            elif kind == "resource":
                res = part.get("resource", {})
                if "blob" in res:
                    name = res.get("uri", "").rsplit("/", 1)[-1]
                    self.attachments.append((res.get("mimeType", ""), base64.b64decode(res["blob"]), name))
        self.text: str = "\n".join(texts)
        self.data: Dict[str, Any] = {k: _decode(v) for k, v in (raw.get("structuredContent") or {}).items()}

    @property
    def message(self) -> str:
        """``text`` without the ``field: value`` and ``attachment:`` lines the
        suite appends after the message (the fields are in ``data``)."""
        prefixes = tuple(f"{k}: " for k in self.data) + ("attachment: ",)
        lines = self.text.splitlines()
        for i, line in enumerate(lines):
            if i > 0 and line.startswith(prefixes):
                return "\n".join(lines[:i])
        return self.text

    @property
    def doc(self) -> Optional[str]:
        """The document id the command made or edited (e.g. ``img1``), if any."""
        value = self.data.get("doc")
        return str(value) if value is not None else None

    @property
    def image(self) -> Optional[bytes]:
        """The first preview image, or None."""
        return self.images[0] if self.images else None

    def save_image(self, path: Union[str, os.PathLike], index: int = 0) -> None:
        """Writes preview image ``index`` to a file."""
        Path(path).write_bytes(self.images[index])

    def get(self, key: str, default: Any = None) -> Any:
        return self.data.get(key, default)

    def __getitem__(self, key: str) -> Any:
        return self.data[key]

    def __contains__(self, key: object) -> bool:
        return key in self.data

    def __bool__(self) -> bool:
        return self.ok

    def __str__(self) -> str:
        return self.text

    def __repr__(self) -> str:
        first = self.text.splitlines()[0] if self.text else ""
        extra = f", {len(self.images)} image(s)" if self.images else ""
        return f"<Result {'ok' if self.ok else 'error'}: {first!r}{extra}>"


class Command:
    """One command, callable with keyword arguments: ``suite.photo.resize(doc="img1", width=800)``."""

    def __init__(self, suite: "Suite", spec: Dict[str, Any]) -> None:
        self._suite = suite
        self.spec = spec
        self.tool_name: str = spec["name"]
        self.id: str = self.tool_name.replace("_", ".", 1) if "_" in self.tool_name else self.tool_name
        self.params: Dict[str, Any] = spec.get("inputSchema", {}).get("properties", {})
        self.required: List[str] = list(spec.get("inputSchema", {}).get("required", []))
        self.__doc__ = self._doc()

    def _doc(self) -> str:
        lines = [self.spec.get("description", "")]
        for name, schema in self.params.items():
            req = "required" if name in self.required else "optional"
            line = f"  {name} ({schema.get('type', 'string')}, {req}): {schema.get('description', '')}"
            if "enum" in schema:
                line += " [" + "|".join(map(str, schema["enum"])) + "]"
            if "default" in schema:
                line += f" (default {schema['default']})"
            lines.append(line)
        return "\n".join(lines)

    def __call__(self, *args: Any, **kwargs: Any) -> Result:
        if args:
            # a single positional argument fills `doc` (or the only required param)
            if len(args) > 1:
                raise TypeError(f"{self.id} takes keyword arguments (one positional at most, for doc)")
            target = "doc" if "doc" in self.params else (self.required[0] if len(self.required) == 1 else None)
            if target is None or target in kwargs:
                raise TypeError(f"{self.id} takes keyword arguments")
            kwargs[target] = args[0]
        return self._suite.call(self.tool_name, **kwargs)

    def __repr__(self) -> str:
        return f"<command {self.id}>"


class Tool:
    """The commands of one tool: ``suite.photo``."""

    def __init__(self, name: str, commands: Dict[str, Command]) -> None:
        self.name = name
        self._commands = commands

    def __getattr__(self, item: str) -> Command:
        try:
            return self._commands[item]
        except KeyError:
            raise AttributeError(f"{self.name} has no command {item!r} (has: {', '.join(sorted(self._commands))})") from None

    def __dir__(self) -> Iterable[str]:
        return sorted(self._commands)

    def __iter__(self):
        return iter(self._commands.values())

    def __repr__(self) -> str:
        return f"<tool {self.name}: {len(self._commands)} commands>"


def _to_arg(value: Any, schema: Dict[str, Any]) -> Any:
    if isinstance(value, os.PathLike):
        return os.fspath(value)
    kind = schema.get("type")
    if isinstance(value, (list, tuple)):
        if kind == "array":
            return [float(v) for v in value]
        return ",".join(os.fspath(v) if isinstance(v, os.PathLike) else str(v) for v in value)
    if isinstance(value, dict):
        if kind == "string":
            # vars= takes name=value pairs; a JSON object works too
            return json.dumps(value)
        raise TypeError("only string parameters take a dict")
    return value


class Suite:
    """A running ``wetwaresuite mcp`` process. Use it as a context manager, or call close().

    ``tools`` narrows it (e.g. ``["photo", "convert"]``); ``raise_errors``
    makes failed commands raise CommandError (default) instead of returning a
    Result with ``ok == False``; ``timeout`` is seconds per command (None waits
    for ever)."""

    def __init__(
        self,
        binary: Union[str, os.PathLike, None] = None,
        tools: Optional[Iterable[str]] = None,
        raise_errors: bool = True,
        timeout: Optional[float] = None,
        cwd: Union[str, os.PathLike, None] = None,
        env: Optional[Dict[str, str]] = None,
        stderr: Any = None,
    ) -> None:
        self.binary = find_binary(binary)
        self.raise_errors = raise_errors
        self.timeout = timeout
        # every command as a method needs every schema, not the catalog big servers list
        args = [self.binary, "mcp", "--all-tools"]
        if tools:
            args += ["--tools", ",".join(tools)]
        try:
            self._proc = subprocess.Popen(
                args,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=stderr,
                cwd=os.fspath(cwd) if cwd is not None else None,
                env=env,
                bufsize=0,
            )
        except OSError as e:
            raise WetwaresuiteError(f"can't start {self.binary}: {e}") from e
        self._lines: "queue.Queue[Optional[bytes]]" = queue.Queue()
        self._reader = threading.Thread(target=self._read, daemon=True)
        self._reader.start()
        self._next_id = 0
        self._lock = threading.Lock()
        init = self._request(
            "initialize",
            {"protocolVersion": PROTOCOL_VERSION, "capabilities": {}, "clientInfo": {"name": "wetwaresuite-python", "version": "0.1"}},
        )
        self.server_info: Dict[str, Any] = init.get("serverInfo", {})
        self.instructions: str = init.get("instructions", "")
        self._send({"jsonrpc": "2.0", "method": "notifications/initialized"})
        self._commands: Dict[str, Command] = {}
        self._tools: Dict[str, Tool] = {}
        self.refresh()

    # ---- protocol ----

    def _read(self) -> None:
        assert self._proc.stdout is not None
        for line in self._proc.stdout:
            self._lines.put(line)
        self._lines.put(None)

    def _send(self, message: Dict[str, Any]) -> None:
        if self._proc.poll() is not None:
            raise WetwaresuiteError(f"wetwaresuite exited (code {self._proc.returncode})")
        assert self._proc.stdin is not None
        try:
            self._proc.stdin.write((json.dumps(message) + "\n").encode("utf-8"))
            self._proc.stdin.flush()
        except (BrokenPipeError, OSError) as e:
            raise WetwaresuiteError(f"wetwaresuite stopped: {e}") from e

    def _request(self, method: str, params: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
        with self._lock:
            self._next_id += 1
            rid = self._next_id
            self._send({"jsonrpc": "2.0", "id": rid, "method": method, "params": params or {}})
            while True:
                try:
                    line = self._lines.get(timeout=self.timeout)
                except queue.Empty:
                    raise WetwaresuiteError(f"{method} timed out after {self.timeout} s") from None
                if line is None:
                    raise WetwaresuiteError(f"wetwaresuite exited (code {self._proc.wait()})")
                try:
                    msg = json.loads(line)
                except ValueError:
                    continue
                if msg.get("id") != rid:
                    continue
                if "error" in msg:
                    raise WetwaresuiteError(f"{method}: {msg['error'].get('message', msg['error'])}")
                return msg.get("result", {})

    # ---- commands ----

    def refresh(self) -> None:
        """Re-reads the command list (tools/list)."""
        specs = self._request("tools/list").get("tools", [])
        self._commands = {}
        grouped: Dict[str, Dict[str, Command]] = {}
        for spec in specs:
            name = spec["name"]
            cmd = Command(self, spec)
            self._commands[name] = cmd
            if "_" in name:
                tool, method = name.split("_", 1)
                grouped.setdefault(tool, {})[method] = cmd
        self._tools = {name: Tool(name, cmds) for name, cmds in grouped.items()}

    @property
    def tools(self) -> Dict[str, Tool]:
        """Tools by name: ``suite.tools["photo"]``."""
        return dict(self._tools)

    def commands(self) -> List[str]:
        """Every command id, e.g. ``photo.resize``."""
        return sorted(c.id for c in self._commands.values() if c.tool_name not in _BUILTINS)

    def command(self, command_id: str) -> Command:
        """A command by id (``photo.resize``) or MCP name (``photo_resize``)."""
        name = command_id.replace(".", "_")
        if name not in self._commands:
            raise AttributeError(f"no command {command_id!r}")
        return self._commands[name]

    def call(self, command_id: str, **kwargs: Any) -> Result:
        """Runs a command by id: ``suite.call("photo.resize", doc="img1", width=800)``."""
        cmd = self.command(command_id)
        if "json" in cmd.params and "json" not in kwargs:
            kwargs["json"] = True  # commands with a json switch return their lists as data
        args = {}
        for key, value in kwargs.items():
            if value is None:
                continue
            args[key] = _to_arg(value, cmd.params.get(key, {}))
        result = Result(self._request("tools/call", {"name": cmd.tool_name, "arguments": args}))
        if self.raise_errors and not result.ok:
            raise CommandError(cmd.id, result)
        return result

    run = call

    def undo(self) -> bool:
        return Result(self._request("tools/call", {"name": "undo"})).ok

    def redo(self) -> bool:
        return Result(self._request("tools/call", {"name": "redo"})).ok

    def history(self) -> List[str]:
        text = Result(self._request("tools/call", {"name": "history"})).text
        return [] if text == "history is empty" else [line for line in text.splitlines() if line]

    def documents(self) -> List[Dict[str, Any]]:
        """Open documents: ``[{"id": "img1", "kind": "image", "summary": ..., "path": ...}]``."""
        return Result(self._request("tools/call", {"name": "documents"})).data.get("documents", [])

    def __getattr__(self, item: str) -> Tool:
        if item.startswith("_"):
            raise AttributeError(item)
        tools = self.__dict__.get("_tools", {})
        if item in tools:
            return tools[item]
        raise AttributeError(f"no tool {item!r} (tools: {', '.join(sorted(tools))})")

    def __dir__(self) -> Iterable[str]:
        return sorted(set(super().__dir__()) | set(self.__dict__.get("_tools", {})))

    # ---- lifetime ----

    def close(self) -> None:
        """Ends the wetwaresuite process (documents that weren't saved are lost)."""
        proc = self.__dict__.get("_proc")
        if proc is None or proc.poll() is not None:
            return
        try:
            if proc.stdin:
                proc.stdin.close()
            proc.wait(timeout=10)
        except (OSError, subprocess.TimeoutExpired):
            proc.kill()
            proc.wait()

    def __enter__(self) -> "Suite":
        return self

    def __exit__(self, *exc: Any) -> None:
        self.close()

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass

    def __repr__(self) -> str:
        return f"<Suite {self.binary}: {len(self._tools)} tools, {len(self._commands)} commands>"


class BatchResult:
    """The outcome of run_batch: ``ok``, ``returncode``, ``output`` (stdout) and ``errors`` (stderr)."""

    def __init__(self, returncode: int, output: str, errors: str) -> None:
        self.returncode = returncode
        self.ok = returncode == 0
        self.output = output
        self.errors = errors

    def __bool__(self) -> bool:
        return self.ok

    def __str__(self) -> str:
        return self.output

    def __repr__(self) -> str:
        return f"<BatchResult {'ok' if self.ok else 'failed'} ({self.returncode})>"


def run_batch(
    text: str,
    binary: Union[str, os.PathLike, None] = None,
    keep_going: bool = False,
    check: bool = True,
    cwd: Union[str, os.PathLike, None] = None,
    timeout: Optional[float] = None,
) -> BatchResult:
    """Runs a batch script (one command per line, ``-> name`` captures,
    ``$name`` reuse) in a fresh ``wetwaresuite batch`` process. Raises
    WetwaresuiteError when a line fails, unless ``check`` is False."""
    args = [find_binary(binary), "batch", "-"]
    if keep_going:
        args.append("--keep-going")
    proc = subprocess.run(
        args,
        input=text.encode("utf-8"),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        cwd=os.fspath(cwd) if cwd is not None else None,
        timeout=timeout,
    )
    result = BatchResult(proc.returncode, proc.stdout.decode("utf-8", "replace"), proc.stderr.decode("utf-8", "replace"))
    if check and not result.ok:
        raise WetwaresuiteError(f"batch failed ({proc.returncode}): {result.errors.strip() or result.output.strip()}")
    return result


if __name__ == "__main__":  # python -m wetwaresuite.client: list what's there
    with Suite() as s:
        for cid in s.commands():
            print(cid)
        sys.exit(0)
