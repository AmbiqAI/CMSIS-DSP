"""The CMSIS-DSP host reference (Testing/hct/host_reference): what it builds and every configuration it refuses.

    python -m unittest discover -s Testing/hct/host_reference -p "test_*.py" -v

With HCT_ROOT set to a helia-core-tester checkout, the manifest is also read with hct's own reader; with
HCT_REQUIRED=1 that check must run.
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

PROJECT = Path(__file__).resolve().parent
ROOT = PROJECT.parents[2]
MANIFEST = "cmsis_dsp_host_reference.json"
GENERATOR = ["-G", "Ninja"] if shutil.which("ninja") else []
REFUSAL = "CMSIS-DSP host reference:"
FP_OPTIONS = ["-O2", "-ffp-contract=off", "-fno-fast-math"]
PATH_MACROS = ["ARM_MATH_DSP", "ARM_MATH_MVEI", "ARM_MATH_MVEF", "ARM_MATH_HELIUM", "ARM_MATH_MVE_FLOAT16",
               "ARM_MATH_AUTOVECTORIZE", "ARM_MATH_NEON", "ARM_MATH_NEON_EXPERIMENTAL", "ARM_MATH_NEON_FLOAT16",
               "ARM_MATH_LOOPUNROLL", "__ARM_FEATURE_DSP", "__ARM_FEATURE_MVE"]


def configure(build, *args):
    return subprocess.run(["cmake", "-S", str(PROJECT), "-B", str(build), *GENERATOR, *args],
                          text=True, capture_output=True, check=False)


def run(*cmd):
    return subprocess.run([str(c) for c in cmd], text=True, capture_output=True, check=False)


class HostReferenceBuildTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls._tmp = tempfile.TemporaryDirectory()
        cls.build_dir = Path(cls._tmp.name) / "host-ref"
        cls.configured = configure(cls.build_dir, "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON")
        cls.built = (run("cmake", "--build", cls.build_dir, "--parallel", os.cpu_count() or 4)
                     if cls.configured.returncode == 0 else None)
        cls.manifest = (json.loads((cls.build_dir / MANIFEST).read_text())
                        if cls.built is not None and cls.built.returncode == 0 else None)

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def setUp(self):
        self.assertEqual(self.configured.returncode, 0, self.configured.stdout + self.configured.stderr)
        self.assertEqual(self.built.returncode, 0, self.built.stdout[-4000:] + self.built.stderr[-4000:])

    def test_manifest(self):
        manifest = self.manifest
        self.assertEqual(manifest["schema"], "hct/host-reference/1")
        self.assertEqual(manifest["library"], "cmsis-dsp")
        self.assertEqual(Path(manifest["source_dir"]), ROOT)
        self.assertTrue(Path(manifest["archive"]).is_file())
        self.assertEqual([Path(d).resolve() for d in manifest["include_dirs"]],
                         [ROOT / "Include", ROOT / "dsppp" / "Include"])
        self.assertEqual(manifest["library_compile_options"], FP_OPTIONS)
        self.assertEqual(manifest["link_libraries"], ["m"])
        self.assertTrue(Path(manifest["c_compiler"]["path"]).is_file())
        self.assertIn(manifest["c_compiler"]["id"], ("GNU", "Clang", "AppleClang"))
        definitions = manifest["compile_definitions"]
        self.assertIn("__GNUC_PYTHON__", definitions)
        # float16_t is the compiler's __fp16 where Arm's header defines it, otherwise _Float16 through two
        # definitions every caller needs too.
        if manifest["float16_type"] == "__fp16":
            self.assertEqual(definitions, ["__GNUC_PYTHON__"])
        else:
            self.assertEqual(manifest["float16_type"], "_Float16")
            self.assertEqual(sorted(definitions), ["ARM_FLOAT16_SUPPORTED=", "__GNUC_PYTHON__", "float16_t=_Float16"])

    def test_every_source_compiles_as_the_plain_c_reference(self):
        commands = json.loads((self.build_dir / "compile_commands.json").read_text())
        sources = [c for c in commands if "/Source/" in c["file"].replace("\\", "/")]
        self.assertGreater(len(sources), 600)
        for command in sources:
            with self.subTest(file=command["file"]):
                text = command["command"]
                for option in FP_OPTIONS:
                    self.assertIn(option, text)
                for forbidden in ("-Ofast", "-ffast-math", "ARM_MATH_LOOPUNROLL", "ARM_MATH_HELIUM", "ARM_MATH_MVE",
                                  "ARM_MATH_NEON", "ARM_MATH_AUTOVECTORIZE", "DISABLEFLOAT16", "ARM_MATH_ROUNDING",
                                  "ARM_MATH_MATRIX_CHECK"):
                    self.assertNotIn(forbidden, text)
        self.assertTrue(any(c["file"].endswith("host_reference_guard.c") for c in commands))

    def test_the_archive_holds_every_family_and_the_f16_kernels(self):
        symbols = run("nm", self.manifest["archive"]).stdout
        defined = {line.split()[-1].lstrip("_") for line in symbols.splitlines()
                   if len(line.split()) == 3 and line.split()[1] in ("T", "S", "R", "D")}
        for name in ("arm_add_f32", "arm_add_f16", "arm_add_q15", "arm_mat_mult_f32", "arm_fir_f32",
                     "arm_rfft_fast_f32", "cmsis_dsp_host_reference"):
            self.assertIn(name, defined)
        # Declared only for the MVE build, so its absence shows no MVE path was compiled.
        self.assertNotIn("arm_biquad_cascade_df1_mve_init_f32", defined)
        self.assertGreater(len([s for s in defined if s.startswith("arm_")]), 700)

    def test_a_caller_built_from_the_manifest_alone_gets_exact_results(self):
        manifest = self.manifest
        executable = self.build_dir / "smoke"
        flags = [f"-D{d}" for d in manifest["compile_definitions"]]
        for directory in manifest["include_dirs"]:
            flags += ["-I", directory]
        compiled = run(manifest["c_compiler"]["path"], *flags, "-O2", "-ffp-contract=off", PROJECT / "smoke.c",
                       manifest["archive"], *[f"-l{lib}" for lib in manifest["link_libraries"]], "-o", executable)
        self.assertEqual(compiled.returncode, 0, compiled.stderr)
        result = run(executable)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS: 0 failure(s)", result.stdout)

    def test_hct_reads_the_manifest(self):
        hct = os.environ.get("HCT_ROOT")
        if not hct:
            if os.environ.get("HCT_REQUIRED") == "1":
                self.fail("HCT_REQUIRED=1 but HCT_ROOT is not set")
            self.skipTest("HCT_ROOT is not set")
        code = ("import sys; from helia_core_tester.core.host_reference import load_manifest; "
                "r = load_manifest(sys.argv[1], library='cmsis-dsp'); print(r.library, r.float16_type)")
        result = subprocess.run(["uv", "run", "--frozen", "--project", hct, "python", "-c", code,
                                 str(self.build_dir / MANIFEST)], cwd=hct, text=True, capture_output=True,
                                check=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(f"cmsis-dsp {self.manifest['float16_type']}", result.stdout)


class HostReferenceRefusalTest(unittest.TestCase):
    """Each configuration that would change the reference fails the configure, naming the cause."""

    def refuses(self, args, message):
        # A fresh directory each time: a cached CMAKE_C_FLAGS would carry into the next configure.
        with tempfile.TemporaryDirectory() as tmp:
            result = configure(Path(tmp) / "build", *args)
        output = result.stdout + result.stderr
        self.assertNotEqual(result.returncode, 0, output)
        self.assertIn(REFUSAL, output)
        self.assertIn(message, " ".join(output.split()))

    def test_a_code_path_option(self):
        for option in ("HELIUM", "MVEI", "MVEF", "NEON", "AUTOVECTORIZE", "LOOPUNROLL", "DISABLEFLOAT16", "FASTBUILD",
                       "ROUNDING", "MATRIXCHECK"):
            with self.subTest(option=option):
                self.refuses([f"-D{option}=ON"], f"{option}=ON changes what the reference computes or builds")

    def test_turning_the_reference_off(self):
        self.refuses(["-DCMSISDSP_HOST_REFERENCE=OFF"], "this project only builds the host reference")

    def test_unsafe_floating_point_flags(self):
        for flag in ("-ffast-math", "-Ofast", "-ffp-contract=fast", "-funsafe-math-optimizations"):
            with self.subTest(flag=flag):
                self.refuses([f"-DCMAKE_C_FLAGS={flag}"], f"CMAKE_C_FLAGS contains '{flag}'")
        self.refuses(["-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_C_FLAGS_RELEASE=-O3 -ffast-math"],
                     "CMAKE_C_FLAGS_RELEASE contains '-ffast-math'")

    def test_a_path_macro_in_the_flags(self):
        for macro in ("ARM_MATH_MVEI", "ARM_MATH_HELIUM", "ARM_MATH_LOOPUNROLL", "ARM_MATH_NEON", "__ARM_FEATURE_DSP"):
            with self.subTest(macro=macro):
                self.refuses([f"-DCMAKE_C_FLAGS=-D{macro}=1"], f"the compile flags select a code path ({macro}")

    def test_a_cross_build(self):
        self.refuses(["-DCMAKE_SYSTEM_NAME=Generic", "-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY"],
                     "it is built for the host CPU")


class GuardTest(unittest.TestCase):
    """The guard compiled into the archive catches what reaches the target past the configure probes."""

    def compile(self, *definitions):
        with tempfile.TemporaryDirectory() as tmp:
            native = run(os.environ.get("CC", "cc"), "-D__GNUC_PYTHON__", "-I", ROOT / "Include",
                         "-c", PROJECT / "host_reference_guard.c", "-o", Path(tmp) / "guard.o", *definitions)
            if native.returncode != 0 and "needs float16_t" in native.stderr:
                native = run(os.environ.get("CC", "cc"), "-D__GNUC_PYTHON__", "-Dfloat16_t=_Float16",
                             "-DARM_FLOAT16_SUPPORTED=", "-I", ROOT / "Include", "-c",
                             PROJECT / "host_reference_guard.c", "-o", Path(tmp) / "guard.o", *definitions)
            return native

    def test_the_plain_build_passes(self):
        result = self.compile()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_every_path_macro_fails(self):
        for macro in PATH_MACROS:
            with self.subTest(macro=macro):
                result = self.compile(f"-D{macro}=1")
                self.assertNotEqual(result.returncode, 0)

    def test_no_float16_fails(self):
        result = run(os.environ.get("CC", "cc"), "-D__GNUC_PYTHON__", "-DDISABLEFLOAT16", "-I", ROOT / "Include",
                     "-fsyntax-only", PROJECT / "host_reference_guard.c")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("needs float16_t", result.stderr)


if __name__ == "__main__":
    sys.exit(unittest.main())
