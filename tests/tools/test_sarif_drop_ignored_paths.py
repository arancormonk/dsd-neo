#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Keep CodeQL results under the config's paths-ignore out of the uploaded SARIF."""

import contextlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
CONFIG = ROOT / ".github" / "codeql" / "codeql-config.yml"


def module(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools" / f"{name}.py")
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


tool = module("sarif_drop_ignored_paths")


def result(rule, uri, base="%SRCROOT%", line=1):
    location = {"uri": uri}
    if base is not None:
        location["uriBaseId"] = base
    return {
        "ruleId": rule,
        "locations": [{"physicalLocation": {"artifactLocation": location, "region": {"startLine": line}}}],
    }


def patterns(*globs):
    return [tool.compile_pattern(glob) for glob in globs]


def kept_rules(sarif):
    return [entry["ruleId"] for entry in sarif["runs"][0]["results"]]


class ConfigTests(unittest.TestCase):
    def write(self, directory, text):
        path = Path(directory) / "codeql-config.yml"
        path.write_text(text, encoding="utf-8")
        return path

    def test_repository_config_ignores_both_vendored_trees(self):
        globs = [pattern for pattern, _ in tool.read_paths_ignore(CONFIG)]
        self.assertIn("src/third_party/**", globs)
        self.assertIn("android/third_party/**", globs)

    def test_block_lists_with_and_without_indent(self):
        with tempfile.TemporaryDirectory() as directory:
            config = self.write(
                directory,
                "queries:\n  - uses: security-and-quality\n"
                "paths-ignore: # vendored\n  - 'a/**'\n\n  # note\n  - \"b/*.c\"\n"
                "query-filters:\n  - exclude:\n      id: x\n",
            )
            self.assertEqual([p for p, _ in tool.read_paths_ignore(config)], ["a/**", "b/*.c"])
            config = self.write(directory, "paths-ignore:\n- \"c/**\"\n- 'd'\n")
            self.assertEqual([p for p, _ in tool.read_paths_ignore(config)], ["c/**", "d"])

    def test_refuses_shapes_it_cannot_honour(self):
        bad = {
            "flow list": 'paths-ignore: ["a/**"]\n',
            "include list": 'paths:\n  - src\npaths-ignore:\n  - "a/**"\n',
            "quoted include key": 'paths-ignore:\n  - "a/**"\n"paths": ["src/**"]\n',
            "merged include key": 'base: &base\n  paths: ["src/**"]\n<<: *base\npaths-ignore:\n  - "a/**"\n',
            "second document": 'paths-ignore:\n  - "a/**"\n---\npaths: ["src/**"]\n',
            "flow root": '{paths-ignore: ["a/**"]}\n',
            "anchored list": 'paths-ignore: &vendored\n  - "a/**"\n',
            "unquoted item": "paths-ignore:\n  - src/third_party\n",
            "mapping item": "paths-ignore:\n  - vendor:\n",
            "boolean item": "paths-ignore:\n  - false\n",
            "alias item": 'vendored: &v "a/**"\npaths-ignore:\n  - *v\n',
            "single quote escape": "paths-ignore:\n  - 'it''s'\n",
            "duplicate key": 'paths-ignore:\n  - a\npaths-ignore:\n  - b\n',
            "empty list": "paths-ignore:\nqueries: []\n",
            "missing list": "queries: []\n",
            "nested mapping": "paths-ignore:\n  - a\n    b: c\n",
            "partial double star": 'paths-ignore:\n  - "src/a**"\n',
            "question mark": 'paths-ignore:\n  - "src/a?"\n',
            "character class": 'paths-ignore:\n  - "src/[ab]"\n',
            "negation": 'paths-ignore:\n  - "!src"\n',
            "absolute": 'paths-ignore:\n  - "/src"\n',
            "dot segment": 'paths-ignore:\n  - "./src"\n',
            "parent segment": 'paths-ignore:\n  - "src/../x"\n',
            "trailing comment": 'paths-ignore:\n  - "src" # vendored\n',
            "escape": 'paths-ignore:\n  - "src\\\\x"\n',
        }
        with tempfile.TemporaryDirectory() as directory:
            for name, text in bad.items():
                with self.subTest(name):
                    with self.assertRaises(tool.FilterError):
                        tool.read_paths_ignore(self.write(directory, text))


class MatchTests(unittest.TestCase):
    def test_globs_follow_codeql_path_rules(self):
        cases = [
            ("android/third_party/**", "android/third_party/librtlsdr/src/tuner_r82xx.c", True),
            ("android/third_party/**", "android/third_party_x/a.c", False),
            ("android/third_party/**", "tests/io/test_vendored_librtlsdr_async_alloc.c", False),
            ("android/third_party", "android/third_party/a/b.c", True),
            ("android/third_party/", "android/third_party/a.c", True),
            ("src/**/gen", "src/gen/a.c", True),
            ("src/**/gen", "src/a/b/gen/c.c", True),
            ("src/**/gen", "src/a/gen.c", False),
            ("**/vendor", "x/vendor/y.c", True),
            ("tests/*.c", "tests/a.c", True),
            ("tests/*.c", "tests/a/b.c", False),
            ("tests/*", "tests/a/b.c", True),
            ("src/a.c", "src/a.cpp", False),
            ("src/a+b.c", "src/a+b.c", True),
        ]
        for glob, path, expected in cases:
            with self.subTest(glob=glob, path=path):
                self.assertEqual(tool.ignored_by(patterns(glob), path) is not None, expected)


class FilterTests(unittest.TestCase):
    def test_drops_only_results_located_in_ignored_paths(self):
        sarif = {
            "runs": [
                {
                    "originalUriBaseIds": {"VENDOR": {"uri": "file:///work/android/"}},
                    "artifacts": [{"location": {"uri": "android/third_party/librtlsdr/src/librtlsdr.c"}}],
                    "results": [
                        result("relative", "android/third_party/librtlsdr/src/tuner_r82xx.c"),
                        result("no-base", "android/third_party/librtlsdr/src/tuner_e4k.c", base=None),
                        result("file-uri", "file:///work/android/third_party/librtlsdr/src/tuner_fc0012.c", base=None),
                        result("encoded", "android/third%5Fparty/librtlsdr/include/tuner_e4k.h"),
                        result("other-base", "third_party/librtlsdr/src/tuner_fc2580.c", base="VENDOR"),
                        {
                            "ruleId": "artifact-index",
                            "locations": [{"physicalLocation": {"artifactLocation": {"index": 0}}}],
                        },
                        result("project", "src/core/util/a.c"),
                        result("test", "tests/io/test_vendored_librtlsdr_async_alloc.c"),
                        result("outside-root", "file:///usr/include/stdio.h", base=None),
                        result("escapes-root", "../other/android/third_party/x.c"),
                        result("unknown-base", "android/third_party/x.c", base="NOWHERE"),
                        result("remote", "https://example.invalid/android/third_party/x.c", base=None),
                        {"ruleId": "no-location"},
                    ],
                }
            ]
        }
        dropped = tool.filter_sarif(sarif, patterns("android/third_party/**"), "/work")
        self.assertEqual(
            [rule for rule, _, _, _ in dropped],
            ["relative", "no-base", "file-uri", "encoded", "other-base", "artifact-index"],
        )
        self.assertEqual(
            kept_rules(sarif),
            ["project", "test", "outside-root", "escapes-root", "unknown-base", "remote", "no-location"],
        )

    def test_file_uris_resolve_only_to_local_paths(self):
        run = {}
        cases = [
            ("/work", "file:///work/android/third_party/a.c", "android/third_party/a.c"),
            ("/work", "file://localhost/work/android/third_party/a.c", "android/third_party/a.c"),
            ("/work", "file://remote/work/android/third_party/a.c", None),
            ("C:/work", "file:///C:/work/android/third_party/a.c", "android/third_party/a.c"),
            ("/work", "file:///work", None),
            ("/work", "file:///workspace/android/third_party/a.c", None),
        ]
        for root, uri, expected in cases:
            with self.subTest(uri=uri):
                location = result("x", uri, base=None)["locations"][0]
                self.assertEqual(tool.relative_path(location, run, root), expected)

    def test_rejects_sarif_without_runs(self):
        with self.assertRaises(tool.FilterError):
            tool.filter_sarif({}, patterns("a"), "/work")


class CommandTests(unittest.TestCase):
    def run_tool(self, *args):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = tool.main(list(args))
        return rc, out.getvalue(), err.getvalue()

    def test_rewrites_each_sarif_in_a_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            vendored = {
                "version": "2.1.0",
                "runs": [
                    {
                        "tool": {"driver": {"name": "CodeQL", "rules": [{"id": "cpp/commented-out-code"}]}},
                        "automationDetails": {"id": "codeql/c-cpp/"},
                        "results": [
                            result("cpp/commented-out-code", "android/third_party/librtlsdr/src/tuner_r82xx.c", line=836),
                            result("cpp/equality-on-floats", "src/dsp/a.c"),
                        ],
                    }
                ],
            }
            clean = {"version": "2.1.0", "runs": [{"results": [result("py/x", "tools/a.py")]}]}
            (work / "c-cpp.sarif").write_text(json.dumps(vendored), encoding="utf-8")
            clean_text = json.dumps(clean, indent=2)
            (work / "python.sarif").write_text(clean_text, encoding="utf-8")
            (work / "notes.txt").write_text("not sarif", encoding="utf-8")

            rc, out, err = self.run_tool("--config", str(CONFIG), "--source-root", str(work), str(work))
            self.assertEqual((rc, err), (0, ""))
            self.assertIn("dropped cpp/commented-out-code at android/third_party/librtlsdr/src/tuner_r82xx.c:836", out)

            rewritten = json.loads((work / "c-cpp.sarif").read_text(encoding="utf-8"))
            self.assertEqual(kept_rules(rewritten), ["cpp/equality-on-floats"])
            self.assertEqual(rewritten["runs"][0]["automationDetails"], vendored["runs"][0]["automationDetails"])
            self.assertEqual(rewritten["runs"][0]["tool"], vendored["runs"][0]["tool"])
            self.assertEqual((work / "python.sarif").read_text(encoding="utf-8"), clean_text)

    def test_fails_without_sarif_or_on_bad_json(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            rc, _, err = self.run_tool("--config", str(CONFIG), str(work))
            self.assertEqual(rc, 1)
            self.assertIn("no .sarif files", err)
            (work / "c-cpp.sarif").write_text("{", encoding="utf-8")
            rc, _, err = self.run_tool("--config", str(CONFIG), str(work))
            self.assertEqual(rc, 1)
            self.assertIn("invalid JSON", err)
            rc, _, err = self.run_tool("--config", str(work / "missing.yml"), str(work))
            self.assertEqual(rc, 1)


if __name__ == "__main__":
    unittest.main()
