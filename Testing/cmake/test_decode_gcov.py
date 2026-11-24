import os
import sys
import tempfile
import textwrap
import unittest
import zlib

import decode_gcov
from decode_gcov import StreamError, extract

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


# Stands in for gcov-tool: merge-stream writes <build>/<name>.gcda for each stream it is given, records the
# call, and fails when the stream holds the bytes "FAIL".
FAKE_TOOL = textwrap.dedent("""\
    import os, sys
    build, log = sys.argv[1], sys.argv[2]
    command, path = sys.argv[3], sys.argv[4]
    data = open(path, "rb").read()
    with open(log, "a") as out:
        out.write(command + "\\n")
    if data == b"FAIL":
        sys.stderr.write("merge-stream: bad stream\\n")
        sys.exit(1)
    open(os.path.join(build, data.decode() + ".gcda"), "w").close()
""")


class Main(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        self.build = os.path.join(self.dir.name, "build")
        os.makedirs(self.build)
        self.log = os.path.join(self.dir.name, "calls.log")
        tool = os.path.join(self.dir.name, "fake_tool.py")
        with open(tool, "w") as out:
            out.write(FAKE_TOOL)
        self.tool = f"{sys.executable} {tool} {self.build} {self.log}"

    def transcript(self, name, text):
        path = os.path.join(self.dir.name, name + ".txt")
        with open(path, "w") as out:
            out.write(text)
        return path

    def decode(self, *paths):
        return decode_gcov.main(["--gcov-tool", self.tool, "--build-dir", self.build, *paths])

    def calls(self):
        return open(self.log).read().split() if os.path.exists(self.log) else []

    def gcda(self):
        return sorted(name for name in os.listdir(self.build) if name.endswith(".gcda"))

    def test_merges_every_transcript(self):
        self.assertEqual(self.decode(self.transcript("a", stream(b"a")), self.transcript("b", stream(b"b"))), 0)
        self.assertEqual(self.calls(), ["merge-stream", "merge-stream"])
        self.assertEqual(self.gcda(), ["a.gcda", "b.gcda"])

    def test_one_bad_transcript_merges_nothing(self):
        good = self.transcript("good", stream(b"a"))
        bad = self.transcript("bad", stream(b"b").split("GCOV-END")[0])
        self.assertEqual(self.decode(good, bad), 1)
        self.assertEqual(self.calls(), [])
        self.assertEqual(self.gcda(), [])

    def test_missing_transcript_merges_nothing(self):
        self.assertEqual(self.decode(self.transcript("a", stream(b"a")), os.path.join(self.dir.name, "none.txt")), 1)
        self.assertEqual(self.calls(), [])

    def test_failed_merge_removes_every_counter_file(self):
        open(os.path.join(self.build, "earlier.gcda"), "w").close()
        first = self.transcript("first", stream(b"a"))
        failing = self.transcript("failing", stream(b"FAIL"))
        self.assertEqual(self.decode(first, failing), 1)
        self.assertEqual(self.gcda(), [])

    def test_missing_gcov_tool(self):
        result = decode_gcov.main(["--gcov-tool", os.path.join(self.dir.name, "no-gcov-tool"),
                                   "--build-dir", self.build, self.transcript("a", stream(b"a"))])
        self.assertEqual(result, 1)

    def test_streams_of_another_build(self):
        other = os.path.join(self.dir.name, "other")
        os.makedirs(other)
        result = decode_gcov.main(["--gcov-tool", self.tool, "--build-dir", other,
                                   self.transcript("a", stream(b"a"))])
        self.assertEqual(result, 1)


if __name__ == "__main__":
    unittest.main()
