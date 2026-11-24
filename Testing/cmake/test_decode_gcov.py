import os
import shlex
import struct
import sys
import tempfile
import textwrap
import unittest
import zlib

import decode_gcov
from decode_gcov import StreamError, destinations, extract

PAYLOAD = bytes(range(200))


def stream(data=PAYLOAD, length=None, crc=None, width=64):
    hex_lines = [data[i:i + width].hex() for i in range(0, len(data), width)]
    length = len(data) if length is None else length
    crc = zlib.crc32(data) if crc is None else crc
    return "\n".join(["S: p", "", "GCOV-BEGIN", *hex_lines, f"GCOV-END {length:08x} {crc:08x}", "\x04",
                      "EXITTHESIM"]) + "\n"


class Extract(unittest.TestCase):
    def test_complete_stream(self):
        self.assertEqual(extract(stream()), PAYLOAD)

    def test_crlf_console(self):
        self.assertEqual(extract(stream().replace("\n", "\r\n")), PAYLOAD)

    def rejects(self, text, reason):
        with self.assertRaises(StreamError) as caught:
            extract(text)
        self.assertIn(reason, str(caught.exception))

    def test_no_stream(self):
        self.rejects("S: p\nE: HardFault\n", "no GCOV-BEGIN")

    def test_cut_short(self):
        self.rejects(stream().split("GCOV-END")[0], "no GCOV-END")

    def test_cut_inside_a_line(self):
        text = stream()
        for cut in range(1, 18):
            with self.subTest(cut=cut), self.assertRaises(StreamError):
                extract(text[:text.index("GCOV-END") - cut])

    def test_length_mismatch(self):
        self.rejects(stream(length=len(PAYLOAD) + 1), "GCOV-END says")

    def test_crc_mismatch(self):
        self.rejects(stream(crc=zlib.crc32(PAYLOAD) ^ 1), "CRC-32")

    def test_flipped_byte(self):
        corrupted = bytes([PAYLOAD[0] ^ 0x10]) + PAYLOAD[1:]
        self.rejects(stream(corrupted, crc=zlib.crc32(PAYLOAD)), "CRC-32")

    def test_dropped_line(self):
        lines = stream().splitlines()
        del lines[lines.index("GCOV-BEGIN") + 2]
        self.rejects("\n".join(lines), "GCOV-END says")

    def test_target_error(self):
        self.rejects("GCOV-BEGIN\nGCOV-ERROR value-profile counters are not supported\n", "the target reported")

    def test_two_streams(self):
        self.rejects(stream() + stream(), "2 GCOV-BEGIN")

    def test_foreign_line_inside(self):
        lines = stream().splitlines()
        lines.insert(lines.index("GCOV-BEGIN") + 1, "S: 1 0 0 t Y")
        self.rejects("\n".join(lines), "unexpected line")

    def test_odd_hex_line(self):
        lines = stream().splitlines()
        lines[lines.index("GCOV-BEGIN") + 1] += "0"
        self.rejects("\n".join(lines), "unexpected line")

    def test_uppercase_hex(self):
        lines = stream().splitlines()
        lines[lines.index("GCOV-BEGIN") + 1] = lines[lines.index("GCOV-BEGIN") + 1].upper()
        self.rejects("\n".join(lines), "unexpected line")

    def test_empty_stream(self):
        self.rejects(stream(b""), "stream is empty")


VERSION = 0x4234332A  # "B43*", GCC 14.3


def gcfn(*paths, records=((0x01000000, b"\0" * 12), (0x01A10000, -16))):
    """A merge-stream in libgcov's layout: per path, a file name record, a .gcda header and its records. A
    negative record length is an all-zero counter record without payload."""
    data = bytearray()
    for path in paths:
        name = path.encode() + b"\0" * (4 - len(path.encode()) % 4)
        data += struct.pack("<III", decode_gcov.GCFN_MAGIC, VERSION, len(name)) + name
        data += struct.pack("<IIII", decode_gcov.GCDA_MAGIC, VERSION, 0x1234, 0x5678)
        for tag, payload in records:
            if isinstance(payload, int):
                data += struct.pack("<Ii", tag, payload)
            else:
                data += struct.pack("<II", tag, len(payload)) + payload
        data += struct.pack("<I", 0)
    return bytes(data)


class Destinations(unittest.TestCase):
    def test_every_object(self):
        self.assertEqual(destinations(gcfn("/b/x.gcda", "/b/y.gcda")), ["/b/x.gcda", "/b/y.gcda"])

    def rejects(self, data, reason):
        with self.assertRaises(StreamError) as caught:
            destinations(data)
        self.assertIn(reason, str(caught.exception))

    def test_cut_inside_a_record(self):
        data = gcfn("/b/x.gcda")
        for cut in range(1, len(data)):
            with self.subTest(cut=cut), self.assertRaises(StreamError):
                destinations(data[:cut])

    def test_no_file_name_record(self):
        self.rejects(b"\0" * 16, "no file name record")

    def test_name_longer_than_the_stream(self):
        self.rejects(struct.pack("<III", decode_gcov.GCFN_MAGIC, VERSION, 400) + b"/b", "bad length")

    def test_versions_differ(self):
        data = bytearray(gcfn("/b/x.gcda"))
        struct.pack_into("<I", data, 12 + 12 + 4, VERSION + 1)
        self.rejects(bytes(data), "no .gcda header")


# Stands in for gcov-tool: merge-stream records the call and creates each .gcda file the stream names, and
# fails when one of them is named FAIL.gcda.
FAKE_TOOL = textwrap.dedent("""\
    import sys
    sys.path.insert(0, sys.argv[1])
    import decode_gcov
    log, command, path = sys.argv[2], sys.argv[3], sys.argv[4]
    with open(log, "a") as out:
        out.write(command + "\\n")
    names = decode_gcov.destinations(open(path, "rb").read())
    if any(name.endswith("FAIL.gcda") for name in names):
        sys.stderr.write("merge-stream: bad stream\\n")
        sys.exit(1)
    for name in names:
        open(name, "w").close()
""")


class Main(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory(prefix="decode gcov ")
        self.addCleanup(self.dir.cleanup)
        self.build = os.path.join(self.dir.name, "build")
        os.makedirs(os.path.join(self.build, "obj"))
        self.log = os.path.join(self.dir.name, "calls.log")
        tool = os.path.join(self.dir.name, "fake_tool.py")
        with open(tool, "w") as out:
            out.write(FAKE_TOOL)
        here = os.path.dirname(os.path.abspath(__file__))
        self.tool = shlex.join([sys.executable, tool, here, self.log])

    def object(self, name):
        """The .gcda path of an object this build compiled (its .gcno exists)."""
        base = os.path.join(self.build, "obj", name)
        open(base + ".gcno", "w").close()
        return base + ".gcda"

    def transcript(self, name, text):
        path = os.path.join(self.dir.name, name + ".txt")
        with open(path, "w") as out:
            out.write(text)
        return path

    def decode(self, *paths, tool=None):
        return decode_gcov.main(["--gcov-tool", tool or self.tool, "--build-dir", self.build, *paths])

    def calls(self):
        return open(self.log).read().split() if os.path.exists(self.log) else []

    def gcda(self):
        return sorted(name for name in os.listdir(os.path.join(self.build, "obj")) if name.endswith(".gcda"))

    def test_merges_every_transcript(self):
        a = self.transcript("a", stream(gcfn(self.object("a"))))
        b = self.transcript("b", stream(gcfn(self.object("b"), self.object("c"))))
        self.assertEqual(self.decode(a, b), 0)
        self.assertEqual(self.calls(), ["merge-stream", "merge-stream"])
        self.assertEqual(self.gcda(), ["a.gcda", "b.gcda", "c.gcda"])

    def test_one_bad_transcript_merges_nothing(self):
        good = self.transcript("good", stream(gcfn(self.object("a"))))
        bad = self.transcript("bad", stream(gcfn(self.object("b"))).split("GCOV-END")[0])
        self.assertEqual(self.decode(good, bad), 1)
        self.assertEqual(self.calls(), [])
        self.assertEqual(self.gcda(), [])

    def test_missing_transcript_merges_nothing(self):
        good = self.transcript("a", stream(gcfn(self.object("a"))))
        self.assertEqual(self.decode(good, os.path.join(self.dir.name, "none.txt")), 1)
        self.assertEqual(self.calls(), [])

    def test_transcript_of_another_build_merges_nothing(self):
        other = os.path.join(self.dir.name, "other", "x.gcda")
        os.makedirs(os.path.dirname(other))
        open(other[:-5] + ".gcno", "w").close()
        good = self.transcript("good", stream(gcfn(self.object("a"))))
        foreign = self.transcript("foreign", stream(gcfn(self.object("b"), other)))
        self.assertEqual(self.decode(good, foreign), 1)
        self.assertEqual(self.calls(), [])

    def test_path_that_leaves_the_build(self):
        escape = os.path.join(self.build, "obj", "..", "..", "x.gcda")
        open(os.path.join(self.dir.name, "x.gcno"), "w").close()
        self.assertEqual(self.decode(self.transcript("a", stream(gcfn(escape)))), 1)
        self.assertEqual(self.calls(), [])

    def test_object_this_build_did_not_compile(self):
        stale = os.path.join(self.build, "obj", "removed.gcda")
        self.assertEqual(self.decode(self.transcript("a", stream(gcfn(stale)))), 1)
        self.assertEqual(self.calls(), [])

    def test_failed_merge_removes_every_counter_file(self):
        open(os.path.join(self.build, "obj", "earlier.gcda"), "w").close()
        first = self.transcript("first", stream(gcfn(self.object("a"))))
        failing = self.transcript("failing", stream(gcfn(self.object("FAIL"))))
        self.assertEqual(self.decode(first, failing), 1)
        self.assertEqual(self.gcda(), [])

    def test_missing_gcov_tool(self):
        good = self.transcript("a", stream(gcfn(self.object("a"))))
        self.assertEqual(self.decode(good, tool=os.path.join(self.dir.name, "no-gcov-tool")), 1)

    def test_tool_path_with_spaces(self):
        self.assertIn(" ", self.dir.name)
        tool = os.path.join(self.dir.name, "gcov tool")
        with open(tool, "w") as out:
            out.write("#!/bin/sh\nexit 0\n")
        os.chmod(tool, 0o755)
        self.assertEqual(decode_gcov.tool_command(tool), [tool])
        self.assertEqual(decode_gcov.tool_command(self.tool)[0], sys.executable)


if __name__ == "__main__":
    unittest.main()
