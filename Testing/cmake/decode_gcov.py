"""Merge the coverage streams that target images print at exit into the build's .gcda files.

usage: decode_gcov.py --gcov-tool COMMAND --build-dir DIR TRANSCRIPT...

target/gcov_dump.c writes each image's counters to the console as a gcov-tool merge-stream in hex:
    GCOV-BEGIN
    <hex lines>
    GCOV-END <length> <crc32>
Each transcript (suite_status.py --transcript) must hold exactly one complete stream whose length and CRC-32
match, and every object in it must name a .gcda file of this build (one with its .gcno beside it). All of
them are checked before any is merged, so a run that stopped early, a stream cut short or corrupted, a
target that reported GCOV-ERROR, or a transcript from another build leaves the counters as they were and
fails the decode. If a merge itself fails, or leaves a named .gcda file missing, every counter file under
the build directory is removed, so no partial report follows.
"""
import argparse
import os
import re
import shlex
import struct
import subprocess
import sys
import tempfile
import zlib

from host_coverage import counter_files

BEGIN = "GCOV-BEGIN"
END = re.compile(r"^GCOV-END ([0-9a-f]{8}) ([0-9a-f]{8})$")
ERROR = "GCOV-ERROR"
HEX = re.compile(r"^(?:[0-9a-f]{2})+$")
# libgcov's merge-stream layout (gcov-io.h): per object, a "gcfn" record naming the .gcda file, then the
# .gcda data: a header and tag/length records, ended by a zero tag. A negative length marks counters that
# are all zero and carry no payload.
GCFN_MAGIC = 0x6763666E
GCDA_MAGIC = 0x67636461


class StreamError(Exception):
    pass


def extract(text):
    """Return the stream bytes of one transcript, or raise StreamError."""
    lines = [line.strip() for line in text.splitlines()]
    for line in lines:
        if line.startswith(ERROR):
            raise StreamError(f"the target reported: {line}")
    begins = [index for index, line in enumerate(lines) if line == BEGIN]
    if not begins:
        raise StreamError("no GCOV-BEGIN: the run ended before the counters were exported")
    if len(begins) > 1:
        raise StreamError(f"{len(begins)} GCOV-BEGIN lines: one run must export once")
    data = bytearray()
    for line in lines[begins[0] + 1:]:
        if end := END.match(line):
            length, crc = int(end.group(1), 16), int(end.group(2), 16)
            if len(data) != length:
                raise StreamError(f"stream has {len(data)} bytes, GCOV-END says {length}")
            if zlib.crc32(data) != crc:
                raise StreamError(f"stream CRC-32 is {zlib.crc32(data):08x}, GCOV-END says {crc:08x}")
            if not data:
                raise StreamError("stream is empty: the image holds no instrumented code")
            return bytes(data)
        if not HEX.match(line):
            raise StreamError(f"unexpected line inside the stream: {line[:60]!r}")
        data += bytes.fromhex(line)
    raise StreamError("no GCOV-END: the stream was cut short")


def destinations(data):
    """Return the .gcda path of each object in a stream, or raise StreamError if the layout is broken."""
    paths = []
    offset = 0

    def words(count, what):
        nonlocal offset
        if offset + 4 * count > len(data):
            raise StreamError(f"stream ends inside {what} at byte {offset}")
        values = struct.unpack_from(f"<{count}I", data, offset)
        offset += 4 * count
        return values

    while offset < len(data):
        magic, version, length = words(3, "a file name record")
        if magic != GCFN_MAGIC:
            raise StreamError(f"no file name record at byte {offset - 12}")
        if offset + length > len(data) or length == 0:
            raise StreamError(f"file name record at byte {offset - 12} has a bad length {length}")
        paths.append(data[offset:offset + length].rstrip(b"\0").decode(errors="replace"))
        offset += length
        magic, gcda_version, _, _ = words(4, "a .gcda header")
        if magic != GCDA_MAGIC or gcda_version != version:
            raise StreamError(f"no .gcda header for {paths[-1]}")
        while True:
            (tag,) = words(1, f"the records of {paths[-1]}")
            if tag == 0:
                break
            (size,) = words(1, f"the records of {paths[-1]}")
            if size < 0x80000000:
                if offset + size > len(data):
                    raise StreamError(f"stream ends inside a record of {paths[-1]}")
                offset += size
    return paths


def check_destinations(paths, build_dir):
    """Return why the paths do not all belong to this build, or None."""
    build = os.path.realpath(build_dir) + os.sep
    for path in paths:
        if not path.endswith(".gcda") or not os.path.realpath(path).startswith(build):
            return f"names {path}, which is not a counter file of {build_dir}"
        if not os.path.isfile(path[:-len(".gcda")] + ".gcno"):
            return f"names {path}, whose object this build did not compile"
    return None


def tool_command(spec):
    """An existing path is one argument, spaces and all; anything else is parsed as a command line."""
    return [spec] if os.path.isfile(spec) else shlex.split(spec)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--gcov-tool", required=True, help="gcov-tool of the compiler that built the images")
    parser.add_argument("--build-dir", required=True, help="the build whose .gcda files the streams name")
    parser.add_argument("transcripts", nargs="+")
    args = parser.parse_args(argv)

    streams = []
    expected = []
    errors = []
    for path in args.transcripts:
        try:
            with open(path, errors="replace") as transcript:
                data = extract(transcript.read())
            named = destinations(data)
            if problem := check_destinations(named, args.build_dir):
                raise StreamError(problem)
            streams.append((path, data))
            expected.extend(named)
        except OSError as exc:
            errors.append(f"{path}: cannot read: {exc.strerror}")
        except StreamError as exc:
            errors.append(f"{path}: {exc}")
    if errors:
        for error in errors:
            print(f"decode_gcov.py: {error}", file=sys.stderr)
        print(f"decode_gcov.py: {len(errors)} of {len(args.transcripts)} transcript(s) rejected; nothing merged",
              file=sys.stderr)
        return 1

    failure = merge(tool_command(args.gcov_tool), streams)
    if not failure and (missing := [path for path in expected if not os.path.isfile(path)]):
        failure = f"gcov-tool wrote no {missing[0]} ({len(missing)} of {len(expected)} counter files missing)"
    if failure:
        partial = list(counter_files(args.build_dir))
        for path in partial:
            os.remove(path)
        print(f"decode_gcov.py: {failure}; removed {len(partial)} counter file(s)", file=sys.stderr)
        return 1
    return 0


def merge(tool, streams):
    """Merge each stream with gcov-tool; return a description of the first failure, or None."""
    with tempfile.TemporaryDirectory() as scratch:
        for index, (path, data) in enumerate(streams):
            stream = os.path.join(scratch, f"{index}.gcfn")
            with open(stream, "wb") as out:
                out.write(data)
            try:
                run = subprocess.run([*tool, "merge-stream", stream], capture_output=True, text=True)
            except OSError as exc:
                return f"cannot run {tool[0]}: {exc.strerror}"
            if run.returncode != 0 or run.stderr.strip():
                return f"{path}: gcov-tool merge-stream failed (status {run.returncode}): {run.stderr.strip()}"
            print(f"decode_gcov.py: merged {len(data)} bytes from {path}")
    return None


if __name__ == "__main__":
    sys.exit(main())
