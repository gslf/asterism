"""asterism vault: values come in on stdin and never appear in any output."""
import argparse
import os
import pty
import select
import stat
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("--cli", required=True)
options, remaining = parser.parse_known_args()
CLI = options.cli
SECRET = "xoxb-1234567890-SECRET-value"
ENV_PASS = "ASTERISM_VAULT_PASSPHRASE"


class VaultCli(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="asvault-cli-")
        self.addCleanup(self.temp.cleanup)
        self.dir = Path(self.temp.name) / "root" / "vault"
        self.env = {k: v for k, v in os.environ.items() if k != ENV_PASS}

    def cli(self, *args, input="", env=None):
        return subprocess.run([CLI, str(self.dir), "vault", *args], input=input,
                              text=True, capture_output=True, timeout=60,
                              env=env if env is not None else self.env)

    def fixture(self, *args, env=None):
        return subprocess.run([CLI, str(self.dir), *args], text=True,
                              capture_output=True, timeout=60,
                              env=env if env is not None else self.env)

    def assert_clean(self, result, *values):
        for value in values:
            self.assertNotIn(value, result.stdout)
            self.assertNotIn(value, result.stderr)

    def assert_files_clean(self, *values):
        for path in self.dir.iterdir():
            data = path.read_bytes()
            for value in values:
                self.assertNotIn(value.encode(), data, path.name)

    def test_set_list_check_rm_never_print_the_value(self):
        result = self.cli("set", "slack-bot", "--tool", "slack", "--domain=slack.com",
                          input=SECRET + "\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("{{secret:slack-bot}}", result.stdout)
        self.assert_clean(result, SECRET)
        for args in (["list"], ["check"]):
            result = self.cli(*args)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("slack-bot", result.stdout)
            self.assert_clean(result, SECRET)
        result = self.cli("list")
        self.assertIn("tools: slack", result.stdout)
        self.assertIn("domains: slack.com", result.stdout)
        self.assertIn("ok      slack-bot", self.cli("check").stdout)
        self.assert_files_clean(SECRET)
        self.assertEqual(stat.S_IMODE(self.dir.stat().st_mode), 0o700)
        for name in ("vault.xcdn", "vault.key"):
            self.assertEqual(stat.S_IMODE((self.dir / name).stat().st_mode), 0o600)
        self.assertEqual(len((self.dir / "vault.key").read_bytes()), 32)

        # the stored value is exactly what was piped in (newline dropped)
        result = self.fixture("--resolve", "slack", "x={{secret:slack-bot}}", "slack.com")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "x=" + SECRET)
        result = self.fixture("--resolve", "slack", "{{secret:slack-bot}}", "evil.com")
        self.assertEqual(result.returncode, 1)
        self.assertIn("DENIED", result.stderr)
        result = self.fixture("--redact", "leak: " + SECRET)
        self.assertEqual(result.stdout, "leak: {{secret:slack-bot}}")

        result = self.cli("rm", "slack-bot")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("no secrets", self.cli("list").stdout)
        result = self.cli("rm", "slack-bot")
        self.assertEqual(result.returncode, 1)
        self.assertIn("unknown secret 'slack-bot'", result.stderr)

    def test_multiline_and_crlf_values(self):
        pem = "-----BEGIN KEY-----\nMIIBVgIBADANBg\n-----END KEY-----"
        self.assertEqual(self.cli("set", "pem", "--tool", "ssh", input=pem + "\n").returncode, 0)
        self.assertEqual(self.fixture("--resolve", "ssh", "{{secret:pem}}").stdout, pem)
        result = self.cli("set", "crlf", "--tool", "net", input="windows-value\r\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.fixture("--resolve", "net", "[{{secret:crlf}}]").stdout,
                         "[windows-value]")
        # replacing keeps one entry with the new value and policy
        result = self.cli("set", "crlf", "--tool", "imap", input="second-value")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.fixture("--resolve", "imap", "{{secret:crlf}}").stdout,
                         "second-value")
        self.assertEqual(self.fixture("--resolve", "net", "{{secret:crlf}}").returncode, 1)
        self.assertEqual(self.cli("list").stdout.count("crlf"), 1)

    def test_usage_and_input_errors(self):
        self.assertEqual(subprocess.run([CLI, str(self.dir), "vault"], capture_output=True,
                                        timeout=30).returncode, 2)
        help_result = self.cli("help")
        self.assertEqual(help_result.returncode, 0)
        self.assertIn("asterism vault set", help_result.stdout)
        cases = [
            (["set", "name"], 2, "--tool"),
            (["set", "--tool", "net"], 2, "usage"),
            (["set", "a", "b", "--tool", "net"], 2, "usage"),
            (["set", "a", "--tool"], 2, "usage"),
            (["set", "a", "--bogus", "x", "--tool", "net"], 2, "usage"),
            (["rm"], 2, "usage"),
            (["list", "extra"], 2, "usage"),
            (["frobnicate"], 2, "unknown command"),
            (["set", "Bad_Name", "--tool", "net"], 1, "slug"),
            (["set", "ok", "--tool", "Net"], 1, "slug"),
            (["set", "ok", "--tool", "net", "--domain", "bad domain"], 1, "not a domain"),
        ]
        for args, code, needle in cases:
            result = self.cli(*args, input=SECRET)
            self.assertEqual(result.returncode, code, (args, result.stderr))
            self.assertIn(needle, result.stderr, args)
            self.assert_clean(result, SECRET)
        result = self.cli("set", "empty", "--tool", "net", input="\n")
        self.assertEqual(result.returncode, 1)
        self.assertIn("empty value", result.stderr)
        result = self.cli("set", "nul", "--tool", "net", input="abc\0def")
        self.assertEqual(result.returncode, 1)
        self.assertIn("NUL", result.stderr)
        result = self.cli("set", "big", "--tool", "net", input="x" * 65537)
        self.assertEqual(result.returncode, 1)
        self.assertIn("longer than 65536", result.stderr)
        self.assertEqual(self.cli("set", "max", "--tool", "net", input="x" * 65536).returncode, 0)
        self.assertFalse((self.dir / "vault.xcdn.tmp").exists())

    def test_short_values_are_reported_by_check(self):
        result = self.cli("set", "pin", "--tool", "net", input="q7z")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("not redacted", result.stderr)
        result = self.cli("check")
        self.assertEqual(result.returncode, 0)
        self.assertIn("warning pin", result.stdout)
        self.assertIn("1 warnings", result.stdout)
        self.assert_clean(result, "q7z")

    def test_check_reports_loose_permissions(self):
        self.assertEqual(self.cli("set", "k", "--tool", "net", input=SECRET).returncode, 0)
        os.chmod(self.dir / "vault.key", 0o644)
        os.chmod(self.dir, 0o755)
        result = self.cli("check")
        self.assertEqual(result.returncode, 1)
        self.assertIn("vault.key is mode 0644", result.stdout)
        self.assertIn("warning", result.stdout)

    def test_passphrase_mode(self):
        env = dict(self.env, **{ENV_PASS: "correct horse battery staple"})
        result = self.cli("set", "pw", "--tool", "imap", input=SECRET, env=env)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse((self.dir / "vault.key").exists())
        result = self.cli("check", env=env)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("passphrase key", result.stdout)
        self.assertIn("600000 iterations", result.stdout)
        self.assert_files_clean(SECRET, "correct horse")
        # listing needs no key; anything that decrypts does
        self.assertEqual(self.cli("list").returncode, 0)
        result = self.cli("check")
        self.assertEqual(result.returncode, 1)
        self.assertIn("passphrase-protected", result.stdout)
        wrong = dict(self.env, **{ENV_PASS: "wrong"})
        result = self.cli("check", env=wrong)
        self.assertEqual(result.returncode, 1)
        self.assertIn("wrong vault passphrase", result.stdout)
        result = self.cli("set", "pw2", "--tool", "imap", input="other-value", env=wrong)
        self.assertEqual(result.returncode, 1)
        self.assertIn("wrong vault passphrase", result.stderr)
        self.assertEqual(self.fixture("--resolve", "imap", "{{secret:pw}}", env=env).stdout,
                         SECRET)

    def test_corrupt_vault_is_never_overwritten(self):
        self.assertEqual(self.cli("set", "k", "--tool", "net", input=SECRET).returncode, 0)
        garbage = "#asvault { version: 1, entries: [ {"
        (self.dir / "vault.xcdn").write_text(garbage)
        result = self.cli("set", "k2", "--tool", "net", input="another-value")
        self.assertEqual(result.returncode, 1)
        self.assertIn("corrupt", result.stderr)
        self.assertEqual((self.dir / "vault.xcdn").read_text(), garbage)
        result = self.cli("check")
        self.assertEqual(result.returncode, 1)
        self.assertIn("corrupt", result.stdout)

    def test_terminal_input_is_not_echoed(self):
        master, slave = pty.openpty()
        proc = subprocess.Popen([CLI, str(self.dir), "vault", "set", "tty-secret", "--tool", "net"],
                                stdin=slave, stdout=subprocess.PIPE, stderr=slave,
                                env=self.env, close_fds=True)
        os.close(slave)
        seen = b""
        deadline = time.monotonic() + 20
        try:
            while b"input hidden" not in seen and time.monotonic() < deadline:
                ready, _, _ = select.select([master], [], [], 0.5)
                if ready:
                    seen += os.read(master, 1024)
            self.assertIn(b"input hidden", seen)
            os.write(master, (SECRET + "\n").encode())
            out, _ = proc.communicate(timeout=20)
            while True:
                ready, _, _ = select.select([master], [], [], 0.5)
                if not ready:
                    break
                try:
                    chunk = os.read(master, 1024)
                except OSError:
                    break
                if not chunk:
                    break
                seen += chunk
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            os.close(master)
        self.assertEqual(proc.returncode, 0, seen)
        self.assertNotIn(SECRET.encode(), seen)
        self.assertNotIn(SECRET.encode(), out)
        self.assertEqual(self.fixture("--resolve", "net", "{{secret:tty-secret}}").stdout, SECRET)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0], *remaining])
