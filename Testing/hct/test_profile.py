"""Testing/hct/hct-library.yaml against the headers and files it names.

    python -m unittest discover -s Testing/hct -p "test_*.py" -v

Needs PyYAML. helia-core-tester checks the profile's form (`contract validate`); these tests check what only
this repository can: that its status codes are arm_status and that its paths exist.
"""

import os
import re
import unittest

import yaml

HERE = os.path.dirname(os.path.abspath(__file__))
PROFILE = os.path.join(HERE, "hct-library.yaml")

ENUM = re.compile(r"typedef\s+enum\s*\{(?P<body>[^}]*)\}\s*(?P<name>[A-Za-z_]\w*)\s*;", re.S)
MEMBER = re.compile(r"([A-Za-z_]\w*)\s*=\s*(-?\s*\d+)")


def _unique_mapping(loader, node, deep=False):
    keys = [loader.construct_object(key, deep=deep) for key, _ in node.value]
    duplicates = sorted({key for key in keys if keys.count(key) > 1})
    if duplicates:
        raise yaml.constructor.ConstructorError(None, None, f"duplicate keys {duplicates}", node.start_mark)
    return loader.construct_mapping(node, deep=deep)


class _UniqueLoader(yaml.SafeLoader):
    pass


_UniqueLoader.add_constructor(yaml.resolver.BaseResolver.DEFAULT_MAPPING_TAG, _unique_mapping)


def load_profile(path=PROFILE):
    with open(path, encoding="utf-8") as handle:
        return yaml.load(handle, Loader=_UniqueLoader)


def header_enum(text, name):
    """The members of `typedef enum {...} name;` by value; every member must have an explicit value."""
    found = [m for m in ENUM.finditer(text) if m.group("name") == name]
    if len(found) != 1:
        raise ValueError(f"{len(found)} definitions of enum {name}")
    body = re.sub(r"/\*.*?\*/|//[^\n]*", "", found[0].group("body"), flags=re.S)
    entries = [entry.strip() for entry in body.split(",") if entry.strip()]
    members = {}
    for entry in entries:
        match = MEMBER.fullmatch(entry)
        if match is None:
            raise ValueError(f"enum {name}: member {entry!r} is not NAME = <decimal integer>")
        if match.group(1) in members:
            raise ValueError(f"enum {name}: {match.group(1)} appears twice")
        members[match.group(1)] = int(match.group(2).replace(" ", ""))
    return members


class HeaderEnumTest(unittest.TestCase):
    def test_reads_members_and_comments(self):
        text = "typedef enum\n{\n  A = 0, /**< ok, fine */\n  B = -1 // no\n} st;\n"
        self.assertEqual(header_enum(text, "st"), {"A": 0, "B": -1})

    def test_refuses_an_implicit_value(self):
        with self.assertRaisesRegex(ValueError, "is not NAME = <decimal integer>"):
            header_enum("typedef enum { A = 0, B } st;", "st")

    def test_refuses_a_value_it_would_read_only_in_part(self):
        for value in ("-1 + 1", "0x10", "1u", "(1)", "1 << 2"):
            with self.subTest(value=value), self.assertRaisesRegex(ValueError, "is not NAME = <decimal integer>"):
                header_enum(f"typedef enum {{ A = {value} }} st;", "st")

    def test_refuses_a_missing_or_repeated_enum(self):
        with self.assertRaisesRegex(ValueError, "0 definitions"):
            header_enum("typedef enum { A = 0 } other;", "st")
        with self.assertRaisesRegex(ValueError, "2 definitions"):
            header_enum("typedef enum { A = 0 } st;\ntypedef enum { A = 0 } st;", "st")

    def test_refuses_a_repeated_member(self):
        with self.assertRaisesRegex(ValueError, "appears twice"):
            header_enum("typedef enum { A = 0, A = 1 } st;", "st")

    def test_profile_loader_refuses_duplicate_keys(self):
        with self.assertRaisesRegex(yaml.constructor.ConstructorError, "duplicate keys"):
            yaml.load("a: 1\na: 2\n", Loader=_UniqueLoader)


class ProfileTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.profile = load_profile()
        cls.root = os.path.normpath(os.path.join(HERE, cls.profile["source_root"]))

    def test_source_root_is_this_checkout(self):
        self.assertTrue(os.path.isfile(os.path.join(self.root, "Include", "arm_math.h")), self.root)

    def test_status_codes_are_arm_status(self):
        status = self.profile["harness"]["status"]
        with open(os.path.join(self.root, "Include", "arm_math_types.h"), encoding="utf-8") as handle:
            header = header_enum(handle.read(), status["type"])
        self.assertEqual(status["codes"], header)
        self.assertEqual(header[status["success"]], 0)
        self.assertIn(status["argument_error"], header)

    def test_harness_headers_exist(self):
        harness = self.profile["harness"]
        for name in harness["includes"] + harness["header_includes"]:
            self.assertTrue(os.path.isfile(os.path.join(self.root, "Include", name)), name)

    def test_contracts_exist(self):
        self.assertTrue(os.path.isfile(os.path.join(HERE, self.profile["contracts"]["signatures"])))

    def test_host_reference_names_its_project_option_and_manifest(self):
        reference = self.profile["host_reference"]
        project = os.path.join(self.root, reference["project"])
        with open(os.path.join(project, "CMakeLists.txt"), encoding="utf-8") as handle:
            cmake = handle.read()
        self.assertRegex(cmake, rf"option\({re.escape(reference['option'])}\b")
        self.assertIn(f"/{reference['manifest']}\"", cmake)
        for relpath in reference["build_inputs"]:
            self.assertTrue(os.path.isdir(os.path.join(self.root, relpath)), relpath)

    def test_build_inputs_hold_every_file_the_host_reference_build_includes(self):
        # hct reuses a cached host-reference build until a build input changes, so a CMake file the
        # build includes from outside them would go stale unseen.
        inputs = [os.path.join(self.root, relpath) for relpath in self.profile["host_reference"]["build_inputs"]]
        include = re.compile(r"include\(\s*\$\{CMAKE_CURRENT_LIST_DIR\}/([^)\s]+)\s*\)")
        found = 0
        for directory, _, names in os.walk(os.path.join(self.root, "Source")):
            for name in names:
                if name != "CMakeLists.txt" and not name.endswith(".cmake"):
                    continue
                with open(os.path.join(directory, name), encoding="utf-8") as handle:
                    for relpath in include.findall(handle.read()):
                        target = os.path.normpath(os.path.join(directory, relpath))
                        found += 1
                        self.assertTrue(any(os.path.commonpath([target, root]) == root for root in inputs), target)
        self.assertGreater(found, 0)


if __name__ == "__main__":
    unittest.main()
