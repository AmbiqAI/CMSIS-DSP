import json
import os
import sys
import tempfile
import textwrap
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import export_contracts as ec  # noqa: E402

HEADER = """\
#ifndef BASIC_H_
#define BASIC_H_
#include "arm_math_types.h"
#ifdef   __cplusplus
extern "C"
{
#endif
/** @param[in] pSrc doc mention arm_doc_only(x) */
void arm_plain_f32(const float32_t *pSrc, float32_t *pDst, uint32_t blockSize);
#define ARM_MACRO(x) arm_macro_call(x)
#if defined(ARM_MATH_NEON)
void arm_neon_only_f32(float32_t *p);
void arm_split_f32(float32_t *p, uint32_t n, uint32_t neonOnly);
#else
void arm_split_f32(float32_t *p, uint32_t n);
#endif
#if defined(ARM_FLOAT16_SUPPORTED)
void arm_half_f16(const float16_t *pSrc,
                  float16_t *pDst);
#if defined(ARM_MATH_MVEF) && !defined(ARM_MATH_AUTOVECTORIZE)
void arm_mve_init_f16(void);
#endif
#endif
__STATIC_FORCEINLINE int32_t arm_inline_q31(int32_t x)
{
  return arm_called_inside_q31(x);
}
#ifdef   __cplusplus
}
#endif
#endif
"""


def _write(directory, relpath, text):
    path = os.path.join(directory, relpath)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(text)
    return path


def _member(name, params, directions=(), returns="void"):
    params_xml = "".join("<param><type>%s</type><declname>%s</declname></param>" % (t, n) for n, t in params)
    items = "".join("<parameteritem><parameternamelist><parametername direction=\"%s\">%s</parametername>"
                    "</parameternamelist></parameteritem>" % (d, n) for n, d in directions)
    return ("<memberdef kind=\"function\" id=\"%s\"><type>%s</type><name>%s</name>%s<detaileddescription><para>"
            "<parameterlist kind=\"param\">%s</parameterlist></para></detaileddescription></memberdef>"
            % (name, returns, name, params_xml, items))


def _xml(directory, members):
    xml_dir = os.path.join(directory, "xml")
    os.makedirs(xml_dir, exist_ok=True)
    _write(xml_dir, "group.xml", "<doxygen><compounddef>%s</compounddef></doxygen>" % "".join(members))
    _write(xml_dir, "index.xml", "<doxygenindex/>")
    return xml_dir


class FixedValueTest(unittest.TestCase):
    def test_configuration_macros_are_evaluated(self):
        self.assertFalse(ec.fixed_value("defined(ARM_MATH_NEON)"))
        self.assertTrue(ec.fixed_value("!defined(ARM_MATH_NEON)"))
        self.assertFalse(ec.fixed_value("(defined(ARM_MATH_NEON) || defined(DOXYGEN))"))
        self.assertTrue(ec.fixed_value("!defined ARM_MFCC_USE_CFFT && !defined(ARM_MIXED_RADIX_FFT)"))

    def test_any_other_macro_is_a_guard(self):
        self.assertIsNone(ec.fixed_value("defined(ARM_FLOAT16_SUPPORTED)"))
        self.assertIsNone(ec.fixed_value("defined(ARM_MATH_NEON) && defined(ARM_MATH_MVEF)"))
        self.assertIsNone(ec.fixed_value("__ARMCC_VERSION >= 6100100"))
        self.assertIsNone(ec.fixed_value("1"))


class ScanTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def scan(self, text):
        return ec.scan_header(_write(self.tmp.name, "Include/dsp/x_functions.h", text), "Include/dsp/x_functions.h")

    def test_declarations_guards_and_parameters(self):
        found = self.scan(HEADER)
        self.assertEqual(sorted(found), ["arm_half_f16", "arm_inline_q31", "arm_mve_init_f16", "arm_plain_f32",
                                         "arm_split_f32"])
        self.assertEqual(found["arm_plain_f32"], (9, [], ["const float32_t *pSrc", "float32_t *pDst",
                                                          "uint32_t blockSize"], "void"))
        self.assertEqual(found["arm_split_f32"][:2], (15, []))
        self.assertEqual(len(found["arm_split_f32"][2]), 2)
        self.assertEqual(found["arm_half_f16"], (18, ["defined(ARM_FLOAT16_SUPPORTED)"],
                                                 ["const float16_t *pSrc", "float16_t *pDst"], "void"))
        self.assertEqual(found["arm_mve_init_f16"], (21, ["defined(ARM_FLOAT16_SUPPORTED)",
                                                          "defined(ARM_MATH_MVEF) && !defined(ARM_MATH_AUTOVECTORIZE)"],
                                                     [], "void"))
        self.assertEqual(found["arm_inline_q31"][0], 24)
        self.assertEqual(found["arm_inline_q31"][3], "int32_t")

    def test_return_types_drop_storage_qualifiers(self):
        found = self.scan("#include \"x.h\"\nextern arm_cfft_instance_f32 *arm_cfft_get(void);\n"
                          "__STATIC_INLINE float32_t\n  arm_s(float32_t x) { return x; }\n")
        self.assertEqual(found["arm_cfft_get"][3], "arm_cfft_instance_f32 *")
        self.assertEqual(found["arm_s"][3], "float32_t")

    def test_an_unfixed_else_branch_carries_the_negated_guard(self):
        found = self.scan("#if defined(ARM_MATH_DSP)\nvoid arm_a(void);\n#elif defined(X)\nvoid arm_b(void);\n"
                          "#else\nvoid arm_c(void);\n#endif\n")
        self.assertEqual(found["arm_a"][1], ["defined(ARM_MATH_DSP)"])
        self.assertEqual(found["arm_b"][1], ["!(defined(ARM_MATH_DSP)) && defined(X)"])
        self.assertEqual(found["arm_c"][1], ["!(defined(ARM_MATH_DSP)) && !(defined(X))"])

    def test_a_fixed_false_branch_is_dropped_and_its_else_kept_without_a_guard(self):
        found = self.scan("#if !defined(ARM_MATH_NEON)\nvoid arm_m(void);\n#else\nvoid arm_n(void);\n#endif\n")
        self.assertEqual(found, {"arm_m": (2, [], [], "void")})

    def test_a_name_declared_twice_is_refused(self):
        with self.assertRaisesRegex(ec.ExportError, "arm_a is declared twice .*\\(first at line 1\\)"):
            self.scan("void arm_a(void);\nvoid arm_a(int x);\n")

    def test_unbalanced_conditionals_are_refused(self):
        with self.assertRaisesRegex(ec.ExportError, "#endif without #if"):
            self.scan("#endif\n")
        with self.assertRaisesRegex(ec.ExportError, "1 #if level\\(s\\) not closed"):
            self.scan("#if defined(X)\nvoid arm_a(void);\n")
        with self.assertRaisesRegex(ec.ExportError, "#ifdef without a name"):
            self.scan("#ifdef\n#endif\n")

    def test_unbalanced_parentheses_are_refused(self):
        with self.assertRaisesRegex(ec.ExportError, "unbalanced parentheses after arm_a"):
            self.scan("void arm_a(int x;\n")

    def test_an_unreadable_header_is_refused(self):
        path = _write(self.tmp.name, "Include/dsp/bad_functions.h", "")
        with open(path, "wb") as handle:
            handle.write(b"void arm_a(\xff);\n")
        with self.assertRaisesRegex(ec.ExportError, "bad_functions.h: cannot read"):
            ec.scan_header(path, "Include/dsp/bad_functions.h")
        with self.assertRaisesRegex(ec.ExportError, "cannot read"):
            ec.scan_header(os.path.join(self.tmp.name, "missing.h"), "missing.h")

    def test_calls_and_mentions_are_not_declarations(self):
        found = self.scan("static int t = arm_init(3);\nvoid f(void) { arm_called(1); }\n/* arm_doc(x) */\n")
        self.assertEqual(found, {})


class BuildTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = self.tmp.name
        _write(self.root, "Include/dsp/basic_functions.h",
               "void arm_add_f32(const float32_t *pSrcA, float32_t *pDst, uint32_t blockSize);\n")

    def build(self, members, overrides=None):
        return ec.build(self.root, _xml(self.root, members), overrides or {})

    def test_a_record(self):
        document = self.build([_member("arm_add_f32", [("pSrcA", "const float32_t *"), ("pDst", "float32_t *"),
                                                       ("blockSize", "uint32_t")],
                                       [("pSrcA", "in"), ("*pDst", "out")])])
        self.assertEqual(document["schema"], "hct/kernel-contracts/1")
        self.assertEqual(document["library"], "cmsis-dsp")
        self.assertEqual(document["functions"], [{
            "name": "arm_add_f32", "header": "Include/dsp/basic_functions.h", "line": 1, "guards": [],
            "returns": "void", "params": [
                {"direction": "in", "name": "pSrcA", "type": "const float32_t *"},
                {"direction": "out", "name": "pDst", "type": "float32_t *"},
                {"direction": "in", "name": "blockSize", "type": "uint32_t"}]}])

    def test_inout_maps_to_in_out_and_agreeing_blocks_merge(self):
        params = [("pSrcA", "const float32_t *"), ("pDst", "float32_t *"), ("blockSize", "uint32_t")]
        document = self.build([_member("arm_add_f32", params, [("pSrcA", "in"), ("pDst", "inout")]),
                               _member("arm_add_f32", params, [("pDst", "inout"), ("blockSize", "in")])])
        self.assertEqual([p["direction"] for p in document["functions"][0]["params"]], ["in", "in,out", "in"])

    def test_every_problem_is_reported_at_once(self):
        _write(self.root, "Include/dsp/more_functions.h", "void arm_missing_f32(void);\nvoid arm_count_f32(int a);\n")
        with self.assertRaises(ec.ExportError) as caught:
            self.build([_member("arm_add_f32", [("pSrcA", "const float32_t *"), ("pDst", "float32_t *"),
                                                ("blockSize", "uint32_t")], [("pSrcA", "in"), ("pSrcA", "out")]),
                        _member("arm_count_f32", [])])
        message = str(caught.exception)
        self.assertIn("4 problem(s)", message)
        self.assertIn("arm_add_f32.pSrcA: directions ['in', 'out'] conflict", message)
        self.assertIn("arm_add_f32.pDst: pointer parameter without a direction", message)
        self.assertIn("Include/dsp/more_functions.h:1: arm_missing_f32 is not in Doxygen's output", message)
        self.assertIn("arm_count_f32 has 1 parameters in the header and 0 in Doxygen", message)

    def test_overrides_decide_and_must_be_used(self):
        params = [("pSrcA", "const float32_t *"), ("pDst", "float32_t *"), ("blockSize", "uint32_t")]
        document = self.build([_member("arm_add_f32", params, [("pSrcA", "in")])], {"arm_add_f32.pDst": "out"})
        self.assertEqual(document["functions"][0]["params"][1]["direction"], "out")
        with self.assertRaisesRegex(ec.ExportError, "names parameters the export does not have: arm_add_f32.pNope"):
            self.build([_member("arm_add_f32", params, [("pSrcA", "in"), ("pDst", "out")])],
                       {"arm_add_f32.pNope": "in"})

    def test_types_are_compared_by_token(self):
        _write(self.root, "Include/dsp/basic_functions.h",
               "void arm_add_f32(const float32_t*pSrcA, float32_t  * pDst, uint32_t blockSize);\n")
        document = self.build([_member("arm_add_f32", [("pSrcA", "const float32_t *"), ("pDst", "float32_t *"),
                                                       ("blockSize", "uint32_t")], [("pSrcA", "in"), ("pDst", "out")])])
        self.assertEqual(document["functions"][0]["params"][0]["type"], "const float32_t *")

    def test_a_return_type_mismatch_is_refused(self):
        params = [("pSrcA", "const float32_t *"), ("pDst", "float32_t *"), ("blockSize", "uint32_t")]
        with self.assertRaisesRegex(ec.ExportError, "arm_add_f32 returns 'void' in the header and 'arm_status' in "
                                                    "Doxygen"):
            self.build([_member("arm_add_f32", params, [("pSrcA", "in"), ("pDst", "out")], returns="arm_status")])

    def test_a_parameter_type_mismatch_is_refused(self):
        params = [("pSrcA", "const float32_t *"), ("pDst", "float64_t *"), ("blockSize", "uint32_t")]
        with self.assertRaisesRegex(ec.ExportError, "arm_add_f32: parameter types differ: pDst 'float32_t \\*' in "
                                                    "the header, 'float64_t \\*' in Doxygen"):
            self.build([_member("arm_add_f32", params, [("pSrcA", "in"), ("pDst", "out")])])

    def test_a_dropped_const_is_a_mismatch(self):
        params = [("pSrcA", "float32_t *"), ("pDst", "float32_t *"), ("blockSize", "uint32_t")]
        with self.assertRaisesRegex(ec.ExportError, "pSrcA 'const float32_t \\*' in the header, 'float32_t \\*'"):
            self.build([_member("arm_add_f32", params, [("pSrcA", "in"), ("pDst", "out")])])

    def test_an_array_extent_mismatch_is_refused(self):
        _write(self.root, "Include/dsp/basic_functions.h", "void arm_add_f32(float32_t pDst[4]);\n")
        member = _member("arm_add_f32", [("pDst", "float32_t")], [("pDst", "out")]).replace(
            "<declname>pDst</declname>", "<declname>pDst</declname><array>[3]</array>")
        with self.assertRaisesRegex(ec.ExportError, "pDst 'float32_t\\[4\\]' in the header, 'float32_t\\[3\\]'"):
            self.build([member])

    def test_two_doxygen_signatures_are_refused(self):
        with self.assertRaisesRegex(ec.ExportError, "arm_add_f32: Doxygen has 2 different signatures"):
            self.build([_member("arm_add_f32", [("a", "int")]), _member("arm_add_f32", [("a", "float")])])

    def test_a_name_in_two_headers_is_refused(self):
        _write(self.root, "Include/dsp/again_functions.h", "void arm_add_f32(const float32_t *pSrcA, float32_t *pDst,"
                                                           " uint32_t blockSize);\n")
        params = [("pSrcA", "const float32_t *"), ("pDst", "float32_t *"), ("blockSize", "uint32_t")]
        with self.assertRaisesRegex(ec.ExportError, "arm_add_f32 is declared in two public headers"):
            self.build([_member("arm_add_f32", params, [("pSrcA", "in"), ("pDst", "out")])])


class OverridesTest(unittest.TestCase):
    def load(self, document):
        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as handle:
            json.dump(document, handle)
        self.addCleanup(os.unlink, handle.name)
        return ec.load_overrides(handle.name)

    def test_a_valid_entry(self):
        self.assertEqual(self.load({"directions": {"arm_a.p": {"direction": "in,out", "reason": "read"}}}),
                         {"arm_a.p": "in,out"})
        self.assertEqual(ec.load_overrides(os.path.join(tempfile.gettempdir(), "no-such-overrides.json")), {})

    def test_entries_without_a_reason_or_a_known_direction_are_refused(self):
        for entry in ({"direction": "in"}, {"direction": "in", "reason": " "}, {"direction": "inout", "reason": "x"},
                      "in"):
            with self.assertRaisesRegex(ec.ExportError, "needs a direction"):
                self.load({"directions": {"arm_a.p": entry}})

    def test_the_committed_overrides_load(self):
        overrides = ec.load_overrides()
        self.assertTrue(overrides)
        self.assertTrue(all(key.startswith("arm_") and "." in key for key in overrides))


class DoxygenTest(unittest.TestCase):
    def test_another_version_is_refused(self):
        with mock.patch.object(ec, "doxygen_version", return_value="1.18.0"):
            with self.assertRaisesRegex(ec.ExportError, "is Doxygen 1.18.0; the export is pinned to 1.17.0"):
                ec.run_doxygen("doxygen", ".", tempfile.gettempdir())

    def test_a_missing_doxygen_is_refused(self):
        with self.assertRaisesRegex(ec.ExportError, "cannot run"):
            ec.doxygen_version(os.path.join(tempfile.gettempdir(), "no-such-doxygen"))


class MainTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.output = os.path.join(self.tmp.name, "kernel_contracts.json")
        self.text = ec.render({"schema": ec.SCHEMA, "library": ec.LIBRARY, "functions": [{"header": "h"}]})

    def run_main(self, command):
        with mock.patch.object(ec, "generate", return_value=self.text):
            return ec.main([command, "--output", self.output])

    def test_export_then_check(self):
        self.assertEqual(self.run_main("export"), 0)
        self.assertEqual(self.run_main("check"), 0)

    def test_check_fails_on_drift_and_on_a_missing_file(self):
        self.assertEqual(self.run_main("check"), 1)
        with open(self.output, "w", encoding="utf-8") as handle:
            handle.write(self.text.replace('"h"', '"g"'))
        self.assertEqual(self.run_main("check"), 1)

    def test_an_export_error_exits_2(self):
        with mock.patch.object(ec, "generate", side_effect=ec.ExportError("boom")):
            self.assertEqual(ec.main(["export", "--output", self.output]), 2)
        self.assertFalse(os.path.exists(self.output))


class CommittedExportTest(unittest.TestCase):
    """What the committed export must hold, without Doxygen."""

    def test_shape(self):
        with open(ec.DEFAULT_OUTPUT, encoding="utf-8") as handle:
            document = json.load(handle)
        self.assertEqual((document["schema"], document["library"]), (ec.SCHEMA, ec.LIBRARY))
        names = [record["name"] for record in document["functions"]]
        self.assertEqual(len(names), len(set(names)))
        for record in document["functions"]:
            for param in record["params"]:
                self.assertIn(param["direction"], ("in", "out", "in,out"), (record["name"], param))

    def test_every_header_function_is_exported(self):
        with open(ec.DEFAULT_OUTPUT, encoding="utf-8") as handle:
            exported = {record["name"] for record in json.load(handle)["functions"]}
        scanned = set()
        for pattern in ec.HEADER_PATTERNS:
            for path in __import__("glob").glob(os.path.join(ec.DEFAULT_ROOT, pattern)):
                scanned |= set(ec.scan_header(path, os.path.relpath(path, ec.DEFAULT_ROOT)))
        self.assertEqual(scanned, exported)


if __name__ == "__main__":
    unittest.main()
