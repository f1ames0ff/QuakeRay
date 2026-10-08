import hashlib
import json
import os
from pathlib import Path
import queue
import subprocess
import threading
import time
import tempfile

from .errors import EvidenceError
from urllib.parse import unquote, urlparse


class ClangSession:
    def __init__(self, executable, root, database):
        self.temporary = tempfile.TemporaryDirectory()
        commands = json.loads(database.read_text(encoding="utf-8"))
        for item in commands:
            if "arguments" in item:
                item["arguments"].append("/clang:-Wno-error")
            else:
                item["command"] += " /clang:-Wno-error"
        temporary_database = Path(self.temporary.name) / "compile_commands.json"
        temporary_database.write_text(json.dumps(commands), encoding="utf-8")
        self.process = subprocess.Popen([str(executable), "--background-index=false", "--clang-tidy=false",
                                         "--log=error", f"--compile-commands-dir={temporary_database.parent}"],
                                        cwd=root, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        self.messages = queue.Queue()
        self.deadline = time.monotonic() + 7
        self.next_id = 0
        self.diagnostics = []
        threading.Thread(target=self._read, daemon=True).start()
        try:
            self.request("initialize", {"processId": os.getpid(), "rootUri": Path(root).as_uri(),
                                     "capabilities": {"textDocument": {"callHierarchy": {"dynamicRegistration": False},
                                                                      "documentSymbol": {"hierarchicalDocumentSymbolSupport": True}}}})
            self.notify("initialized", {})
        except Exception:
            self.close()
            raise

    def _read(self):
        try:
            while True:
                headers = {}
                while True:
                    line = self.process.stdout.readline()
                    if not line:
                        return
                    if line in (b"\r\n", b"\n"):
                        break
                    key, value = line.decode().split(":", 1)
                    headers[key.lower()] = value.strip()
                size = int(headers["content-length"])
                if size > 16 * 1024 * 1024:
                    return
                self.messages.put(json.loads(self.process.stdout.read(size)))
        except Exception:
            self.messages.put({"reader_failed": True})

    def send(self, body):
        raw = json.dumps(body).encode()
        self.process.stdin.write(f"Content-Length: {len(raw)}\r\n\r\n".encode() + raw)
        self.process.stdin.flush()

    def notify(self, method, params):
        self.send({"jsonrpc": "2.0", "method": method, "params": params})

    def request(self, method, params, timeout=25):
        self.next_id += 1
        identifier = self.next_id
        self.send({"jsonrpc": "2.0", "id": identifier, "method": method, "params": params})
        deadline = min(self.deadline, time.monotonic() + timeout)
        while time.monotonic() < deadline:
            try:
                message = self.messages.get(timeout=max(0.01, deadline - time.monotonic()))
            except queue.Empty:
                break
            if message.get("method") == "textDocument/publishDiagnostics":
                self.diagnostics.extend(message.get("params", {}).get("diagnostics", []))
            if message.get("id") == identifier:
                if "error" in message:
                    raise EvidenceError("INDEX_UNAVAILABLE", message["error"].get("message", "clangd request failed"))
                return message.get("result")
        raise EvidenceError("INDEX_TIMEOUT", "clangd request exceeded its bounded deadline")

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
        self.process.wait(5)
        for stream in (self.process.stdin, self.process.stdout):
            stream.close()
        self.temporary.cleanup()


class CodeIndex:
    def __init__(self, root, executable):
        self.root = Path(root).resolve()
        self.executable = Path(executable)
        self.database = self.root / "build/Debug/compile_commands.json"
        self.symbols = {}
        self.version = subprocess.run([str(self.executable), '--version'], capture_output=True, encoding='utf-8', timeout=5).stdout.splitlines()[0] if self.executable.is_file() else 'unavailable'

    def file(self, value):
        path = (self.root / value).resolve()
        if not path.is_relative_to(self.root) or path.suffix not in {".c", ".cpp", ".h", ".hpp"}:
            raise EvidenceError("PERMISSION_DENIED", "Index file escapes the source allowlist")
        return path

    def identity(self, path):
        headers = hashlib.sha256()
        for folder in ('Quake', 'shared', 'renderer', 'third_party', 'Windows'):
            for header in sorted(path for path in (self.root / folder).rglob('*') if path.suffix in {'.h', '.hpp', '.hxx', '.inl', '.hlsli'}):
                headers.update(str(header.relative_to(self.root)).encode())
                headers.update(header.read_bytes())
        return {"source_sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                "database_sha256": hashlib.sha256(self.database.read_bytes()).hexdigest(),
                'headers_sha256': headers.hexdigest(), 'backend_version': self.version}

    def query(self, file, method, position=None):
        path = self.file(file)
        if not self.executable.is_file() or not self.database.is_file():
            raise EvidenceError("INDEX_UNAVAILABLE", "Configured clangd and Debug compile database are required")
        identity = self.identity(path)
        session = ClangSession(self.executable, self.root, self.database)
        try:
            session.notify("textDocument/didOpen", {"textDocument": {"uri": path.as_uri(), "version": 1,
                           "languageId": "cpp" if path.suffix != ".c" else "c", "text": path.read_text(encoding="utf-8")}})
            params = {"textDocument": {"uri": path.as_uri()}}
            if position is not None:
                params["position"] = position
            if method == "textDocument/references":
                params["context"] = {"includeDeclaration": True}
            value = session.request(method, params)
            return value, identity, session.diagnostics
        finally:
            session.close()

    def find(self, query, file):
        value, identity, diagnostics = self.query(file, "textDocument/documentSymbol")
        found = []
        def visit(nodes):
            for node in nodes or []:
                if query.casefold() in node["name"].casefold():
                    position = node.get("selectionRange", node.get("range", node.get("location", {}).get("range", {}))).get("start")
                    identifier = hashlib.sha256(json.dumps([file, node["name"], position, identity], sort_keys=True).encode()).hexdigest()
                    self.symbols[identifier] = {"file": file, "position": position, "identity": identity, "name": node["name"]}
                    found.append({"symbol_id": identifier, "name": node["name"], "file": file, "position": position})
                visit(node.get("children"))
        visit(value)
        found.sort(key=lambda item: item["name"] != query)
        return {"backend": "clangd_ast", "symbols": found[:50], "identity": identity,
                "diagnostics": diagnostics[:20], "coverage": "selected_translation_unit_partial", "truncated": len(found) > 50,
                "index_only_flags": ["/clang:-Wno-error"]}

    def related(self, identifier, direction):
        symbol = self.symbols.get(identifier)
        if not symbol:
            raise EvidenceError("NOT_FOUND", "Select a symbol from the current index session")
        if self.identity(self.file(symbol["file"])) != symbol["identity"]:
            raise EvidenceError("INDEX_STALE", "Source/database changed; refresh the symbol lookup")
        if direction == "references":
            result, identity, diagnostics = self.query(symbol["file"], "textDocument/references", symbol["position"])
        else:
            path = self.file(symbol["file"])
            session = ClangSession(self.executable, self.root, self.database)
            try:
                session.notify("textDocument/didOpen", {"textDocument": {"uri": path.as_uri(), "version": 1,
                               "languageId": "cpp" if path.suffix != ".c" else "c", "text": path.read_text(encoding="utf-8")}})
                items = session.request("textDocument/prepareCallHierarchy", {"textDocument": {"uri": path.as_uri()}, "position": symbol["position"]})
                result = session.request("callHierarchy/" + direction, {"item": items[0]}) if items else []
                diagnostics = session.diagnostics
                identity = symbol["identity"]
            finally:
                session.close()
        return {"backend": "clangd_ast", "edges": (result or [])[:50], "identity": identity,
                "diagnostics": diagnostics[:20], "coverage": "resolved_edges_only", "truncated": len(result or []) > 50}

    def trace(self, source_id, target_id, depth=4):
        if source_id not in self.symbols or target_id not in self.symbols:
            raise EvidenceError('NOT_FOUND', 'Select both symbols from the current index session')
        target = self.symbols[target_id]
        if self.identity(self.file(target['file'])) != target['identity']:
            raise EvidenceError('INDEX_STALE', 'Target source/database/dependencies changed')
        frontier = [(source_id, [self.symbols[source_id]['name']])]
        seen = set()
        checks = 0
        while frontier and checks < 4:
            identifier, path = frontier.pop(0)
            if identifier in seen or len(path) > depth:
                continue
            seen.add(identifier)
            result = self.related(identifier, 'outgoingCalls')
            checks += 1
            for edge in result['edges']:
                item = edge['to']
                raw = unquote(urlparse(item['uri']).path)
                if os.name == 'nt' and raw.startswith('/'):
                    raw = raw[1:]
                file = Path(raw).resolve()
                if not file.is_relative_to(self.root):
                    continue
                relative = str(file.relative_to(self.root)).replace('\\', '/')
                position = item['selectionRange']['start']
                if item['name'] == target['name'] and relative == target['file'].replace('\\', '/') and position == target['position']:
                    return {'paths': [path + [item['name']]], 'coverage': 'resolved_path_only', 'complete_graph': False}
                identity = self.identity(file)
                key = hashlib.sha256(json.dumps([relative, item['name'], position, identity], sort_keys=True).encode()).hexdigest()
                self.symbols[key] = {'file': relative, 'position': position, 'identity': identity, 'name': item['name']}
                frontier.append((key, path + [item['name']]))
        return {'paths': [], 'coverage': 'bounded_unresolved_frontier', 'complete_graph': False, 'remaining_frontier': len(frontier)}
