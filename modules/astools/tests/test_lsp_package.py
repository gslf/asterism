"""Validate the generated contract and query a packaged clangd via asterism tools invoke."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from lsp_contract import manifest, render
from package_clangd import assemble


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", required=True)
    parser.add_argument("--clangd", required=True, type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="astools-lsp-package-") as directory:
        root = Path(directory)
        package = assemble(args.clangd, root / "registry" / "clangd")
        subprocess.run([args.cli, "check", str(package)], check=True, timeout=10)
        original = (package / "bin" / "clangd").stat().st_size
        try:
            assemble(args.clangd, package)
        except FileExistsError:
            pass
        else:
            raise AssertionError("Existing package was replaced")
        assert (package / "bin" / "clangd").stat().st_size == original
        contract = manifest("linux", "x86_64")
        for case in ("protocol", "oneshot", "command", "path", "hash", "effect", "cache"):
            bad = copy.deepcopy(contract)
            cmd = bad["commands"][0]
            if case == "protocol": bad["runtime"]["protocol"] = "unknown"
            elif case == "oneshot": bad["runtime"]["mode"] = "oneshot"
            elif case == "command": cmd["name"] = "execute"
            elif case == "path": cmd["params"][0]["type"]["kind"] = "string"
            elif case == "hash": cmd["params"][1]["required"] = False
            elif case == "effect": cmd["annotations"]["read_only"] = False
            elif case == "cache": cmd["annotations"]["idempotent"] = True
            negative = root / "negative.xcdn"
            negative.write_text(render(bad), encoding="utf-8")
            result = subprocess.run([args.cli, "check", str(negative)], capture_output=True, timeout=10)
            assert result.returncode == 2, (case, result.stdout, result.stderr)
        workspace = root / "workspace"
        workspace.mkdir()
        source = "#ifndef ASTOOLS_HIDE_SYMBOL\nint identity(int x) { return x; }\n#endif\n"
        (workspace / ".clangd").write_text("CompileFlags:\n  Add: [-DASTOOLS_HIDE_SYMBOL]\n", encoding="utf-8")
        (workspace / "example.cpp").write_text(source, encoding="utf-8")
        config = root / "config.xcdn"
        config.write_text('#astools_config ' + json.dumps({
            "registry": {"watch": "off", "pinning": "off"},
            "workspace": {"root": str(workspace)},
            "sandbox": {"default_level": "basic", "strict_fallback": "reject"},
        }), encoding="utf-8")
        host = ["--root", str(package.parent), "--config", str(config)]
        catalog = subprocess.run([args.cli, "catalog", "--level", "index", *host], text=True,
                                 capture_output=True, timeout=30)
        assert catalog.returncode == 0, catalog.stderr
        for command in ("symbols", "definition", "references", "diagnostics"):
            assert command in catalog.stdout, catalog.stdout
        digest = hashlib.sha256(source.encode()).hexdigest()
        run = subprocess.run([args.cli, "invoke", "clangd.symbols",
                              '{path: "example.cpp", sha256: "%s"}' % digest, *host],
                             text=True, capture_output=True, timeout=30)
        if "ASTOOLS_ERR_UNSUPPORTED" in run.stderr:
            # an older clangd (Xcode 15) cannot negotiate UTF-8 positions
            print(f"SKIP: {args.clangd} does not negotiate UTF-8 positions", file=sys.stderr)
            raise SystemExit(77)
        assert run.returncode == 0, (run.stdout, run.stderr)
        observation = json.loads(run.stdout)
        assert observation["items"][0]["name"].startswith("identity"), observation
        assert observation["source_sha256"] == digest
        assert "verification" not in observation
        stale = subprocess.run([args.cli, "invoke", "clangd.symbols",
                                '{path: "example.cpp", sha256: "%s"}' % ("0" * 64), *host],
                               text=True, capture_output=True, timeout=30)
        assert stale.returncode == 2 and "reopen the file" in stale.stderr, (stale.stdout, stale.stderr)
        print("Packaged clangd: schema, negative contracts, catalog and query passed")


if __name__ == "__main__":
    main()
