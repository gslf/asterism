"""Exercise reviewed local MCP-server packages through asterism tools invoke."""
import argparse
import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from package_mcp import assemble


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", required=True)
    parser.add_argument("--server", required=True, type=Path)
    args = parser.parse_args()
    reviewed = {"id": "remote", "version": "1.0.0", "summary": "Reviewed local echo",
        "description": "A local fixture for the MCP client contract.", "arguments": ["normal"],
        "permissions": {"fs": [{"path": "${workspace}", "access": "read-write"}], "net": False, "proc": False, "env": []},
        "commands": [{"name": "echo", "summary": "Echo one message", "annotations": {"read_only": False},
            "params": [{"name": "msg", "required": True, "type": {"kind": "string"}}],
            "mcp": {"name": "remote.echo", "input_schema": {"type": "object", "properties": {"msg": {"type": "string"}},
                "required": ["msg"], "additionalProperties": False}}}]}
    with tempfile.TemporaryDirectory(prefix="astools-mcp-package-") as directory:
        root = Path(directory)
        for level, mode in (("basic", "normal"), ("strict", "normal"), ("basic", "environment"), ("strict", "environment"), ("basic", "environment-denied"), ("basic", "failed"), ("basic", "schema")):
            work = root / (level + "-" + mode); work.mkdir()
            contract = work / "contract.json"
            spec = copy.deepcopy(reviewed); spec["arguments"] = [mode]
            if mode.startswith("environment"):
                spec["permissions"]["env"] = ["MCP_TEST_ALLOWED"]
            contract.write_text(json.dumps(spec), encoding="utf-8")
            package = assemble(args.server, contract, work / "registry" / "remote")
            assert not (work / "called").exists(), "Assembly executed the server"
            subprocess.run([args.cli, "check", str(package)], check=True, capture_output=True, timeout=10)
            try:
                assemble(args.server, contract, package)
            except FileExistsError:
                pass
            else:
                raise AssertionError("Existing package was overwritten")
            config = work / "config.xcdn"
            config.write_text("#astools_config " + json.dumps({
                "registry": {"watch": "off", "pinning": "off"},
                "grants": {"tools": [{"tool": "remote", "env": ["MCP_TEST_ALLOWED"]}] if mode == "environment" else []},
                "workspace": {"root": str(work)}, "sandbox": {"default_level": level, "strict_fallback": "reject"}}))
            host = ["--root", str(package.parent), "--config", str(config)]
            catalog = subprocess.run([args.cli, "catalog", *host], text=True, capture_output=True, timeout=30)
            assert catalog.returncode == 0 and "echo" in catalog.stdout, (catalog.stdout, catalog.stderr)
            run = subprocess.run([args.cli, "invoke", "remote.echo", '{msg: "tè 🍵"}', *host],
                                 text=True, capture_output=True, timeout=30, env={**os.environ,
                                     "MCP_TEST_ALLOWED": "synthetic-test-credential",
                                     "MCP_TEST_DENIED": "must-not-reach-child"})
            if mode == "schema":
                assert run.returncode == 2 and not (work / "called").exists(), (run.stdout, run.stderr)
            else:
                assert run.returncode == (1 if mode == "failed" else 0), (level, mode, run.stdout, run.stderr)
                nested = json.loads(run.stdout)
                assert nested["structuredContent"]["arguments"]["msg"] == "tè 🍵", nested
                assert nested["content"][0]["text"] == "tè 🍵", nested
        negative = root / "negative.json"
        negative.write_text('{"id":"one","id":"two"}')
        try:
            assemble(args.server, negative, root / "invalid")
        except ValueError:
            pass
        else:
            raise AssertionError("Duplicate contract fields were accepted")
        assert not (root / "invalid").exists()
    print("MCP package: basic/strict invocation, catalog, failed-result retention and schema rejection passed")


if __name__ == "__main__":
    main()
