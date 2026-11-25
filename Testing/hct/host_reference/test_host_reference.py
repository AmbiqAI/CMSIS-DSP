"""The CMSIS-DSP host reference (Testing/hct/host_reference): what it builds and every configuration it refuses.

    python -m unittest discover -s Testing/hct/host_reference -p "test_*.py" -v

With HCT_ROOT set to a helia-core-tester checkout, the manifest is also read with hct's own reader; with
HCT_REQUIRED=1 that check must run.
"""

import json
import os
import platform
import re
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
OFF_OPTIONS = ["HELIUM", "HELIUMEXPERIMENTAL", "MVEF", "MVEI", "MVEFLOAT16", "NEON", "NEONEXPERIMENTAL",
               "AUTOVECTORIZE", "LOOPUNROLL", "DISABLEFLOAT16", "ARMAC5", "FASTBUILD", "ROUNDING", "MATRIXCHECK"]
# The option pinning float16 evaluation, and what 2048 + (1 + 2^-10)(1 - 2^-11) gives with it, by float16_t:
# 2048 when the product is rounded to half on its own, 2050 when it is kept in float.
F16_OPTION = "-fexcess-precision=standard"
F16_PRODUCT_SUM = {"__fp16": 2048, "_Float16": 2050}
PATH_MACROS = ["ARM_MATH_DSP", "ARM_MATH_MVEI", "ARM_MATH_MVEF", "ARM_MATH_HELIUM", "ARM_MATH_MVE_FLOAT16",
               "ARM_MATH_AUTOVECTORIZE", "ARM_MATH_NEON", "ARM_MATH_NEON_EXPERIMENTAL", "ARM_MATH_NEON_FLOAT16",
               "ARM_MATH_LOOPUNROLL", "__ARM_FEATURE_DSP", "__ARM_FEATURE_MVE"]


def configure(build, *args, env=None, generator=None):
    return subprocess.run(["cmake", "-S", str(PROJECT), "-B", str(build), *(generator or GENERATOR), *args],
                          text=True, capture_output=True, check=False,
                          env=None if env is None else {**os.environ, **env})


def launcher(directory, *flags):
    """A compiler launcher that appends `flags` to every compile, past any flag CMake scans."""
    script = Path(directory) / "append-flags.sh"
    script.write_text('#!/bin/sh\nexec "$@" ' + " ".join(flags) + "\n")
    script.chmod(0o755)
    return script


def run(*cmd):
    return subprocess.run([str(c) for c in cmd], text=True, capture_output=True, check=False)


def accepts(flag):
    """True when the host C compiler compiles a trivial file with `flag`."""
    with tempfile.TemporaryDirectory() as tmp:
        source = Path(tmp) / "empty.c"
        source.write_text("int main(void) { return 0; }\n")
        return run(os.environ.get("CC", "cc"), flag, "-c", source, "-o", Path(tmp) / "empty.o").returncode == 0


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
        self.assertEqual(manifest["library_compile_options"], FP_OPTIONS + [F16_OPTION])
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
                for option in self.manifest["library_compile_options"]:
                    self.assertIn(option, text)
                for forbidden in ("-Ofast", "-ffast-math", "ARM_MATH_LOOPUNROLL", "ARM_MATH_HELIUM", "ARM_MATH_MVE",
                                  "ARM_MATH_NEON", "ARM_MATH_AUTOVECTORIZE", "DISABLEFLOAT16", "ARM_MATH_ROUNDING",
                                  "ARM_MATH_MATRIX_CHECK"):
                    self.assertNotIn(forbidden, text)
        self.assertTrue(any(c["file"].endswith("host_reference_guard.c") for c in commands))

    def test_the_archive_holds_every_family_and_the_f16_kernels(self):
        symbols = run("nm", self.manifest["archive"]).stdout
        # The type and name are the last two fields, whatever nm prints before them (address, member name).
        defined = {line.split()[-1].lstrip("_") for line in symbols.splitlines()
                   if len(line.split()) >= 2 and line.split()[-2] in ("T", "S", "R", "D")}
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
        flags.append(f"-DHOST_REFERENCE_F16_PRODUCT_SUM={F16_PRODUCT_SUM[manifest['float16_type']]}")
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

    def refuses(self, args, message, env=None, generator=None):
        # A fresh directory each time: a cached CMAKE_C_FLAGS would carry into the next configure.
        with tempfile.TemporaryDirectory() as tmp:
            result = configure(Path(tmp) / "build", *args, env=env, generator=generator)
        output = result.stdout + result.stderr
        self.assertNotEqual(result.returncode, 0, output)
        self.assertIn(REFUSAL, output)
        self.assertIn(message, " ".join(output.split()))

    def test_every_refused_option_is_tested(self):
        text = (PROJECT / "CMakeLists.txt").read_text()
        listed = re.search(r"set\(HOST_REFERENCE_OFF_OPTIONS([^)]*)\)", text).group(1).split()
        self.assertEqual(sorted(listed), sorted(OFF_OPTIONS))

    def test_a_code_path_option(self):
        for option in OFF_OPTIONS:
            with self.subTest(option=option):
                self.refuses([f"-D{option}=ON"], f"{option}=ON changes what the reference computes or builds")

    def test_turning_the_reference_off(self):
        self.refuses(["-DCMSISDSP_HOST_REFERENCE=OFF"], "this project only builds the host reference")

    def test_unsafe_floating_point_flags(self):
        for flag in ("-ffast-math", "-Ofast", "-ffp-contract=fast", "-ffp-contract=on", "-funsafe-math-optimizations",
                     "-fassociative-math", "-freciprocal-math", "-ffinite-math-only", "-fno-signed-zeros",
                     "-fno-honor-nans", "-fno-honor-infinities", "-fexcess-precision=fast", "-fcx-limited-range",
                     "-fcx-fortran-rules", "-fapprox-func", "-ffp-model=fast", "-ffp-model=aggressive", "-mrecip",
                     "-mrecip=all", "-mfpmath=387", "-mfpmath=sse+387", "-fsingle-precision-constant",
                     "-ffp-eval-method=double", "-ffp-eval-method=extended"):
            with self.subTest(flag=flag):
                if accepts(flag):
                    self.refuses([f"-DCMAKE_C_FLAGS={flag}"], f"CMAKE_C_FLAGS contains '{flag}'")
                else:
                    # A flag this compiler rejects already fails CMake's compiler check.
                    with tempfile.TemporaryDirectory() as tmp:
                        self.assertNotEqual(configure(Path(tmp) / "build", f"-DCMAKE_C_FLAGS={flag}").returncode, 0)
        self.refuses(["-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_C_FLAGS_RELEASE=-O3 -ffast-math"],
                     "CMAKE_C_FLAGS_RELEASE contains '-ffast-math'")

    def test_an_unsafe_flag_in_cc(self):
        compiler = os.environ.get("CC", "cc")
        self.refuses([], "CMAKE_C_COMPILER_ARG1 contains '-ffast-math'", env={"CC": f"{compiler} -ffast-math"})

    def test_float16_evaluated_otherwise_than_pinned(self):
        # A flag added past every scan (here by a launcher) still meets the probe: with _Float16 as float the
        # product is kept in float on every host.
        with tempfile.TemporaryDirectory() as tmp:
            self.refuses([f"-DCMAKE_C_COMPILER_LAUNCHER={launcher(tmp, '-D_Float16=float')}"],
                         "is not evaluated")

    @unittest.skipUnless(platform.machine() in ("x86_64", "AMD64"), "x87 exists only on x86")
    def test_x87_intermediates(self):
        with tempfile.TemporaryDirectory() as tmp:
            probe = Path(tmp) / "x87.c"
            probe.write_text("int main(void) { volatile float x = 1.0f, y = 5.9604644775390625e-08f; "
                             "float d = (x + y) - x; return d != 0.0f; }\n")
            built = run(os.environ.get("CC", "cc"), "-O2", "-mfpmath=387", probe, "-o", Path(tmp) / "x87")
            if built.returncode != 0 or run(Path(tmp) / "x87").returncode == 0:
                self.skipTest("this compiler does not evaluate float in x87 precision with -mfpmath=387")
            self.refuses([f"-DCMAKE_C_COMPILER_LAUNCHER={launcher(tmp, '-mfpmath=387')}"],
                         "float intermediates are not rounded to float")

    @unittest.skipUnless(shutil.which("ninja"), "needs Ninja for a multi-config generator")
    def test_a_multi_config_generator(self):
        self.refuses([], "needs a single-configuration generator", generator=["-G", "Ninja Multi-Config"])

    def test_flags_that_do_not_compile(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.refuses([f"-DCMAKE_C_COMPILER_LAUNCHER={launcher(tmp, '-fno-such-flag-for-the-reference')}"],
                         "does not compile an empty program")

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
                self.assertIn("plain C code only", result.stderr)

    def test_no_float16_fails(self):
        result = run(os.environ.get("CC", "cc"), "-D__GNUC_PYTHON__", "-DDISABLEFLOAT16", "-I", ROOT / "Include",
                     "-fsyntax-only", PROJECT / "host_reference_guard.c")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("needs float16_t", result.stderr)


if __name__ == "__main__":
    sys.exit(unittest.main())
