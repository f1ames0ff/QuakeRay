import json
import os
from pathlib import Path
import sys
import unittest

import anyio
from jsonschema import Draft202012Validator
from mcp import Client, MCPError
from mcp.client.stdio import StdioServerParameters

from quakeray_mcp.schemas import INPUTS, output_schema
from quakeray_mcp.server import create_server
from quakeray_mcp.service import EvidenceService


ROOT = Path(__file__).resolve().parents[3]
FIXTURES = Path(__file__).parent / "fixtures"


class ProtocolTests(unittest.TestCase):
    async def exercise(self, target, mode):
        with anyio.fail_after(25):
            async with Client(target, mode=mode, read_timeout_seconds=15) as client:
                self.assertEqual(client.protocol_version, "2025-11-25" if mode == "legacy" else "2026-07-28")
                tools = await client.list_tools()
                self.assertEqual([tool.name for tool in tools.tools], sorted(INPUTS))
                for tool in tools.tools:
                    self.assertFalse(tool.input_schema["additionalProperties"])
                    Draft202012Validator.check_schema(tool.input_schema)
                    Draft202012Validator.check_schema(tool.output_schema)
                result = await client.call_tool("get_architecture", {"document": "entry"})
                self.assertFalse(result.is_error)
                payload = result.structured_content
                Draft202012Validator(output_schema("get_architecture")).validate(payload)
                self.assertEqual(payload["data"]["documents"][0]["path"], "AGENTS.md")
                result = await client.call_tool("read_stats", {"source": {"import_path": str(FIXTURES / "frames.csv")}})
                self.assertFalse(result.is_error)
                self.assertEqual(result.structured_content["data"]["summary"]["fps"], 50)
                error = await client.call_tool("read_stats", {"source": {"import_path": "../private.csv"}})
                self.assertTrue(error.is_error)
                self.assertEqual(error.structured_content["error"]["code"], "PERMISSION_DENIED")
                Draft202012Validator(output_schema("read_stats")).validate(error.structured_content)
                invalid = await client.call_tool("server_status", {"shell": "quit"})
                self.assertTrue(invalid.is_error)
                self.assertEqual(invalid.structured_content["error"]["code"], "INVALID_ARGUMENT")
                both = await client.call_tool("read_stats", {"source": {"import_path": "x", "artifact_id": "x"}})
                self.assertTrue(both.is_error)
                oversized = await client.call_tool("read_stats", {"source": {"import_path": "x"}, "limit": 101})
                self.assertTrue(oversized.is_error)
                missing = await client.call_tool("read_stats", {})
                self.assertTrue(missing.is_error)
                resource = await client.read_resource("quakeray://docs/architecture")
                data = json.loads(resource.contents[0].text)
                self.assertEqual(data["documents"][0]["path"], "ARCHITECTURE.md")
                resources = await client.list_resources()
                self.assertGreaterEqual(len(resources.resources), 8)
                templates = await client.list_resource_templates()
                self.assertEqual(len(templates.resource_templates), 2)
                latest = await client.read_resource("quakeray://runs/latest")
                self.assertTrue(json.loads(latest.contents[0].text)["empty"])
                with self.assertRaises(MCPError):
                    await client.read_resource("quakeray://docs/not-found")
                prompts = await client.list_prompts()
                self.assertEqual(prompts.prompts[0].name, "investigate_slow_scene")
                prompt = await client.get_prompt("investigate_slow_scene")
                self.assertIn("server_status", prompt.messages[0].content.text)

    def test_in_memory_legacy(self):
        anyio.run(self.exercise, create_server(EvidenceService(ROOT, [FIXTURES])), "legacy")

    def test_in_memory_modern(self):
        anyio.run(self.exercise, create_server(EvidenceService(ROOT, [FIXTURES])), "auto")

    def subprocess_target(self):
        return StdioServerParameters(command=sys.executable,
                                     args=["-B", "-m", "quakeray_mcp", "--root", str(ROOT),
                                           "--import-root", str(FIXTURES)],
                                     cwd=str(ROOT), env={**os.environ, "QUAKERAY_IMPORT_ROOTS": ""})

    def test_stdio_legacy(self):
        anyio.run(self.exercise, self.subprocess_target(), "legacy")

    def test_stdio_modern(self):
        anyio.run(self.exercise, self.subprocess_target(), "auto")

    @unittest.skipUnless(os.name == "nt", "Windows job APIs")
    def test_opt_in_job_catalog_and_debug_only_schema(self):
        import tempfile
        async def check():
            with tempfile.TemporaryDirectory() as temp:
                service = EvidenceService(ROOT, enable_jobs=True, state_root=Path(temp) / "jobs")
                try:
                    async with Client(create_server(service), mode="legacy") as client:
                        tools = await client.list_tools()
                        self.assertEqual(len(tools.tools), 13)
                        for tool in tools.tools:
                            Draft202012Validator.check_schema(tool.input_schema)
                            Draft202012Validator.check_schema(tool.output_schema)
                        result = await client.call_tool("start_build", {
                            "runtime_id": "debug", "idempotency_key": "test_request_key_01", "config": "Release"})
                        self.assertTrue(result.is_error)
                        self.assertEqual(result.structured_content["error"]["code"], "INVALID_ARGUMENT")
                        result = await client.call_tool("list_runs", {})
                        self.assertEqual(result.structured_content["data"]["runs"], [])
                finally:
                    service.close()
        anyio.run(check)


if __name__ == "__main__":
    unittest.main()
