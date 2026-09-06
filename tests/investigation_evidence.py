#!/usr/bin/env python3
"""Behavioral acceptance for investigation source retrieval and hard limits."""

import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


BIN = str(Path(os.environ.get("HPRSCRIPT_BIN", "./hprscript")).resolve())


class InvestigationEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="hpr-evidence-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def write(self, name, content):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")
        return path

    def fixture(self):
        self.write("auth.go", "package auth\n\nfunc validateToken(token string) bool {\n    return verifySignature(token)\n}\n")
        self.write("crypto.go", "package auth\n\nfunc verifySignature(token string) bool {\n    return len(token) > 12\n}\n")
        self.write("auth_test.go", 'package auth\nimport "testing"\nfunc TestRejectShort(t *testing.T) {\n    if verifySignature("short") {\n        t.Fatal("short signature accepted")\n    }\n}\n')

    def run_tool(self, *args, seed="validateToken", paths=None, env=None, code=0, cwd=None):
        command = [BIN, "investigate", "-F", seed, "-profile", "symbol", *map(str, args)]
        command += list(map(str, paths if paths is not None else [self.root]))
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                env={**os.environ, **(env or {})}, cwd=cwd)
        self.assertEqual(result.returncode, code, result.stderr.decode() + result.stdout.decode())
        result.stdout.decode("utf-8", errors="strict")
        return result

    def records(self, *args, **kwargs):
        result = self.run_tool(*args, **kwargs)
        return [json.loads(line) for line in result.stdout.splitlines()]

    @staticmethod
    def evidence(records):
        return [r for r in records if r["type"] == "investigation-evidence"]

    @staticmethod
    def sources(records):
        return [r for r in records if r["type"] == "investigation-source"]

    def test_a1_helper_and_associated_test(self):
        self.fixture()
        records = self.records("-evidence-budget", 16000)
        summary = records[0]
        self.assertEqual((summary["scan_stages"], summary["tests"], summary["related_tests"]), (2, 0, 1))
        rows = self.evidence(records)
        helper = next(r for r in rows if r["file"].endswith("crypto.go") and r["classification"] == "probable_definition")
        test = next(r for r in rows if r["file"].endswith("auth_test.go"))
        self.assertEqual(helper["derived_value"], "verifySignature")
        self.assertEqual(helper["derived_source_rows"], test["derived_source_rows"])
        self.assertEqual(test["category"], "associated_test")
        source = "\n".join(line["text"] for chunk in self.sources(records) for line in chunk["lines"])
        self.assertIn("return len(token) > 12", source)
        self.assertIn('t.Fatal("short signature accepted")', source)
        llm = self.run_tool("-llm", "-evidence-budget", 16000).stdout.decode()
        self.assertIn("derived=verifySignature", llm)
        self.assertIn("return len(token) > 12", llm)

    def test_a2_cpp_definition_declaration_call_and_signature(self):
        self.write("a.cpp", "struct Result {};\nResult *target(int value);\n\nResult *\ntarget(\n    int value\n) {\n    return helper(value);\n}\nvoid caller() { target(1); }\n")
        records = self.records("-followup-scan", "never", seed="target")
        roles = {r["line"]: r["classification"] for r in self.evidence(records)}
        self.assertEqual(roles[2], "probable_declaration")
        self.assertEqual(roles[5], "probable_definition")
        self.assertEqual(roles[10], "probable_call_or_reference")
        target = next(r for r in self.evidence(records) if r["classification"] == "probable_definition")
        body = next(s for s in self.sources(records) if s["source_chunk_id"] == target["source_chunk_id"])
        self.assertTrue(body["body_complete"])
        self.assertEqual(body["line_start"], 4)
        self.assertTrue(body["signature_complete"])
        self.assertIn("Result *", [line["text"] for line in body["lines"]])

    def test_a3_complete_small_body_once(self):
        self.write("a.go", "func target() {\n    target()\n    helper()\n}\n")
        records = self.records("-followup-scan", "never", seed="target")
        self.assertEqual(len(self.sources(records)), 1)
        chunk = self.sources(records)[0]
        self.assertTrue(chunk["body_complete"])
        self.assertEqual([line["line"] for line in chunk["lines"]], [1, 2, 3, 4])

    def test_multiline_prototype_and_tag_type_mentions(self):
        self.write("a.cpp", "Result *\ntarget(\n    int value\n);\nstruct Token;\nstruct Token *use(struct Token *value);\nstruct Token { int field; };\n")
        rows = self.evidence(self.records("-followup-scan", "never", seed="target"))
        self.assertEqual(rows[0]["classification"], "probable_declaration")
        rows = self.evidence(self.records("-followup-scan", "never", seed="Token"))
        self.assertEqual([r["line"] for r in rows if r["classification"] == "probable_definition"], [7])
        self.assertEqual([r["line"] for r in rows if r["classification"] == "probable_declaration"], [5])

    def test_a4_large_caller_has_guard_and_omitted_ranges(self):
        self.write("a.go", "func caller() {\n" + "    work()\n" * 45 + "    if ready {\n        target()\n    }\n" + "    work()\n" * 45 + "}\n")
        records = self.records("-followup-scan", "never", seed="target")
        chunk = self.sources(records)[0]
        self.assertFalse(chunk["body_complete"])
        self.assertTrue(chunk["omitted_ranges"])
        self.assertIn("    if ready {", [line["text"] for line in chunk["lines"]])

    def test_a5_unrelated_caller_noise_does_not_displace_helper(self):
        self.write("seed.go", "func target() {\n    directHelper()\n}\n")
        self.write("helper.go", "func directHelper() {\n    meaningfulWork()\n}\n")
        self.write("caller.go", "func caller() {\n    target()\n" + "    work()\n" * 6 + "    cli.stats.size()\n" * 100 + "}\n")
        records = self.records("-max-related-patterns", 1, seed="target")
        self.assertTrue(any(r.get("derived_value") == "directHelper" and r["classification"] == "probable_definition" for r in self.evidence(records)))

    def test_a6_comment_string_parameter_roles_and_formatter(self):
        self.write("a.go", '// func Token() { fake() }\nvar example = "func Token() { fake() }"\nfunc accepts(value Token) {\n}\nfunc Token() {\n    text := "}"\n    /* } */\n}\n')
        records = self.records("-followup-scan", "never", seed="Token")
        roles = {r["line"]: r["classification"] for r in self.evidence(records)}
        self.assertEqual(roles[1], "comment")
        self.assertEqual(roles[2], "string")
        self.assertEqual(roles[3], "probable_call_or_reference")
        self.assertEqual(roles[5], "probable_definition")
        chunk = next(c for c in self.sources(records) if c["line_start"] == 5)
        self.assertEqual(chunk["line_end"], 8)
        result = subprocess.run([BIN, "-F", "Token", "-scope", "auto", str(self.root)], capture_output=True, check=True)
        rows = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertEqual([r["line"] for r in rows if r.get("role") == "def"], [5])

    def test_a7_shared_helper_keeps_all_origins_and_one_body(self):
        self.write("a.go", "func firstSeed() { sharedHelper() }\nfunc secondSeed() { sharedHelper() }\n")
        self.write("b.go", "func sharedHelper() { work() }\n")
        records = self.records("-F", "secondSeed", "-examples", 20, seed="firstSeed")
        helper = next(r for r in self.evidence(records) if r["file"].endswith("b.go") and r["classification"] == "probable_definition")
        self.assertEqual(len(helper["derived_source_rows"]), 2)
        self.assertEqual(len([c for c in self.sources(records) if c["file"].endswith("b.go")]), 1)

    def test_cpp_raw_string_cannot_invent_definition_or_body_boundary(self):
        self.write("a.cpp", 'const char *sample = R"tag("\nint target() { fake(); }\n)tag";\nint target() {\n    auto text = R"({ " })";\n    return 1;\n}\n')
        records = self.records("-followup-scan", "never", seed="target")
        roles = {r["line"]: r["classification"] for r in self.evidence(records)}
        self.assertEqual(roles[2], "string")
        self.assertEqual(roles[4], "probable_definition")
        body = next(c for c in self.sources(records) if c["body_complete"])
        self.assertEqual(body["line_end"], 7)

    def test_return_type_and_declared_name_share_multiline_source(self):
        self.write("a.cpp", "Token\ntarget() { return helper(); }\n")
        records = self.records("-F", "target", "-followup-scan", "never", seed="Token")
        self.assertEqual(len(self.sources(records)), 1)
        self.assertEqual(len({r["source_chunk_id"] for r in self.evidence(records)}), 1)

    def test_late_definition_survives_earlier_prototypes(self):
        self.write("a.cpp", "void target() { directHelper(); }\n" + "int directHelper();\n" * 20)
        self.write("z.cpp", "int directHelper() { return 42; }\n")
        records = self.records("-examples", 2, seed="target")
        self.assertTrue(any(r["file"].endswith("z.cpp") and r["classification"] == "probable_definition" for r in self.evidence(records)))

    def test_a8_context_and_expand_refs(self):
        self.write("notes.txt", "one\ntwo\nthree\nfour\nvalidateToken\nsix\nseven\neight\nnine\n")
        base = self.records("-followup-scan", "never")
        wider = self.records("-followup-scan", "never", "-C", 3)
        self.assertGreater(len(self.sources(wider)[0]["lines"]), len(self.sources(base)[0]["lines"]))
        zero = self.records("-followup-scan", "never", "-C", 0)
        self.assertEqual([line["line"] for line in self.sources(zero)[0]["lines"]], [5])
        after = self.records("-followup-scan", "never", "-A", 1)
        self.assertEqual([line["line"] for line in self.sources(after)[0]["lines"]], [5, 6])
        before = self.records("-followup-scan", "never", "-B", 1)
        self.assertEqual([line["line"] for line in self.sources(before)[0]["lines"]], [4, 5])
        self.run_tool("-C", -1, code=2)
        self.fixture()
        records = self.records("-refs", paths=[self.root / "auth.go"])
        ref = self.evidence(records)[0]["ref"]
        result = subprocess.run([BIN, "expand", ref], capture_output=True, check=True)
        self.assertIn(b"return verifySignature(token)", result.stdout)

    def test_a9_source_precedes_rankings_under_pressure(self):
        self.fixture()
        result = self.run_tool("-evidence-budget", 3500)
        self.assertLessEqual(len(result.stdout), 3500)
        rows = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertTrue(self.sources(rows))
        first_source = next(i for i, r in enumerate(rows) if r["type"] == "investigation-source")
        self.assertTrue(all(i > first_source for i, r in enumerate(rows) if r["type"] == "investigation-file"))

    def test_a10_encoded_bytes_unicode_summary_and_output_cap(self):
        self.write("a.go", 'func validateToken() {\n    text := "שלום \\\"quoted\\\" 😀"\n}\n')
        for llm in ([], ["-llm"]):
            for cap in (2000, 3000, 7000):
                result = self.run_tool(*llm, "-summary", "-evidence-budget", 9000, "-max-output-bytes", cap)
                self.assertLessEqual(len(result.stdout), cap)
                if not llm:
                    records = [json.loads(line) for line in result.stdout.splitlines()]
                    self.assertEqual(records[-1]["type"], "summary")
                    self.assertEqual(records[0]["complete"], records[-1]["complete"])

    def test_long_single_line_degrades_before_dropping_seed(self):
        self.write("a.go", 'func validateToken() { text := "' + "😀" * 2500 + '" }\n')
        records = self.records("-evidence-budget", 3500, "-examples", 1)
        row = self.evidence(records)[0]
        self.assertEqual(row["classification"], "probable_definition")
        self.assertTrue(row["context_truncated"])
        source = self.sources(records)[0]
        self.assertFalse(source["body_complete"])
        self.assertTrue(source["lines"][0]["omitted_bytes"])

    def test_a11_too_small_budget_has_no_partial_stdout(self):
        self.fixture()
        for flags in ([], ["-llm"], ["-summary"], ["-explain-plan"]):
            result = self.run_tool(*flags, "-evidence-budget", 1, code=2)
            self.assertEqual(result.stdout, b"")
            self.assertIn(b"minimum", result.stderr)

    def test_a12_disabled_or_empty_expansion(self):
        self.fixture()
        records = self.records("-followup-scan", "never")
        self.assertEqual(records[0]["scan_stages"], 1)
        self.assertEqual(records[0]["related_tests"], 0)
        self.assertEqual(records[0]["coverage"]["associated_test"], "not_searched_for_related_identifiers")
        self.assertTrue(all(r["file"].endswith("auth.go") for r in self.evidence(records)))
        self.write("empty.go", "func loneSeed() {}\n")
        empty = self.records("-followup-scan", "always", seed="loneSeed", paths=[self.root / "empty.go"])
        self.assertEqual(empty[0]["scan_stages"], 1)
        self.assertEqual(empty[0]["followup"], "no_candidates")

    def test_a13_independent_candidate_memory_and_output_limits(self):
        self.fixture()
        records = self.records("-max-related-patterns", 1, "-evidence-budget", 3000, "-summary")
        self.assertFalse(records[0]["expansion_complete"])
        reasons = records[-2]["omission_reasons"]
        self.assertIn("adaptive_pattern_cap", reasons)
        self.assertIn("evidence_budget", reasons)
        limited = self.records("-max-memory-bytes", 12000, "-summary")
        self.assertLessEqual(limited[-1]["buffered_bytes_peak"], 12000)
        tiny = self.records("-max-memory-bytes", 1, "-summary")
        self.assertFalse(tiny[0]["output_complete"])
        self.assertLessEqual(tiny[-1]["buffered_bytes_peak"], 1)
        self.assertIn("memory_cap", tiny[-2]["omission_reasons"])

    def test_a14_followup_read_failure_is_counted(self):
        self.fixture()
        env = {"HPRSCRIPT_ENABLE_FAULT_INJECTION": "1", "HPRSCRIPT_TEST_FAIL_FOLLOWUP_READ_N": "1"}
        records = self.records("-summary", "-diagnostics", env=env)
        self.assertFalse(records[0]["scan_complete"])
        self.assertFalse(records[0]["expansion_complete"])
        self.assertEqual(records[-2]["followup_files_failed"], 1)
        self.assertEqual(records[-1]["files_failed"], 1)
        self.run_tool("-require-complete", env=env, code=2)
        result = self.run_tool("-diagnostics", "-summary", "-evidence-budget", 1, env=env, code=2)
        self.assertEqual(result.stdout, b"")

    def test_a15_same_name_targets_are_ambiguous(self):
        self.write("auth.go", "func validateToken() { sharedHelper() }\n")
        self.write("one/helper.go", "func sharedHelper() { first() }\n")
        self.write("two/helper.go", "func sharedHelper() { second() }\n")
        rows = self.evidence(self.records())
        defs = [r for r in rows if r.get("derived_value") == "sharedHelper" and r["classification"] == "probable_definition"]
        self.assertEqual(len(defs), 2)
        self.assertTrue(all(r["ambiguous"] for r in defs))

    def test_a16_unsupported_scope_has_lexical_fallback(self):
        self.write("a.py", "def validateToken(token):\n    return check(token)\n")
        chunk = self.sources(self.records("-followup-scan", "never"))[0]
        self.assertEqual(chunk["mode"], "lexical_window")
        self.assertFalse(chunk["body_complete"])
        self.assertFalse(chunk["signature_complete"])

    def test_a17_noisy_seed_does_not_starve_quiet_seed(self):
        self.write("a.go", "func noisySeed() {\n" + "".join(f"    helper{i}()\n" for i in range(25)) + "}\nfunc quietSeed() { quietHelper() }\n")
        self.write("b.go", "func quietHelper() { useful() }\n")
        records = self.records("-F", "quietSeed", "-max-related-patterns", 2, seed="noisySeed")
        self.assertTrue(any(r.get("derived_value") == "quietHelper" and r["classification"] == "probable_definition" for r in self.evidence(records)))
        self.assertTrue(any(r["category"] == "seed_implementation" and "quietSeed" in r["context"] for r in self.evidence(records)))

    def test_a18_exclusion_scope_and_seed_predicate_contract(self):
        self.fixture()
        excluded = self.records("-exclude", "crypto.go")
        self.assertFalse(any(r["file"].endswith("crypto.go") for r in self.evidence(excluded)))
        scoped = self.records("-in-scope", "^validateToken$")
        self.assertTrue(all(r["file"].endswith("auth.go") for r in self.evidence(scoped)))
        predicate = self.records("-file-where", "p0")
        self.assertTrue(any(r["file"].endswith("crypto.go") for r in self.evidence(predicate)))
        self.assertEqual(predicate[0]["filters"]["seed_file_where"], "p0")
        restricted = self.records("-lines", "3:3")
        self.assertTrue(all(r["line"] == 3 for r in self.evidence(restricted)))

    def test_missing_literal_input_is_incomplete(self):
        records = self.records("-summary", paths=[self.root / "missing.go"], code=1)
        self.assertFalse(records[0]["scan_complete"])
        self.assertEqual(records[-1]["files_failed"], 1)
        self.run_tool("-require-complete", paths=[self.root / "missing.go"], code=2)

    def test_a18_git_selection_applies_to_both_stages(self):
        self.fixture()
        def git(*args):
            subprocess.run(["git", *args], cwd=self.root, capture_output=True, check=True)
        git("init", "-q")
        git("add", ".")
        git("-c", "user.name=Test", "-c", "user.email=test@example.invalid", "-c", "commit.gpgsign=false", "commit", "-qm", "fixture")
        self.write("auth.go", "package auth\n\nfunc validateToken(token string) bool { // changed\n    return verifySignature(token)\n}\n")
        self.write("crypto.go", "package auth\n\nfunc verifySignature(token string) bool { // changed\n    return len(token) > 12\n}\n")
        records = self.records("-git-changed", "-git-added-lines", paths=[], cwd=self.root)
        self.assertEqual(records[0]["related_tests"], 0)
        self.assertTrue(all(r["line"] == 3 for r in self.evidence(records)))
        self.assertTrue(any(r.get("derived_value") == "verifySignature" for r in self.evidence(records)))

    def test_a19_repeated_and_permuted_input_order(self):
        self.fixture()
        paths = sorted(self.root.iterdir())
        forward = self.run_tool(paths=paths).stdout
        backward = self.run_tool(paths=list(reversed(paths))).stdout
        # Input provenance preserves the user spelling/order; evidence does not.
        a = [json.loads(line) for line in forward.splitlines()]
        b = [json.loads(line) for line in backward.splitlines()]
        a[0].pop("inputs")
        b[0].pop("inputs")
        self.assertEqual(a, b)
        self.assertEqual(forward, self.run_tool(paths=paths).stdout)

    def test_selected_caller_chunk_does_not_include_unselected_matches(self):
        self.write("a.go", "func validateToken() { helper() }\n")
        self.write("b.go", "func caller() {\n    validateToken()\n" + "    unrelated()\n" * 10 + "    helper()\n" * 100 + "}\n")
        rows = self.records("-examples", 2)
        caller = next(c for c in self.sources(rows) if c["file"].endswith("b.go"))
        self.assertLess(len(caller["lines"]), 10)


if __name__ == "__main__":
    unittest.main(verbosity=2)
