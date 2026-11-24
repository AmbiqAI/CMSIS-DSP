"""Merge the coverage streams that target images print at exit into the build's .gcda files.

usage: decode_gcov.py --gcov-tool COMMAND --build-dir DIR TRANSCRIPT...

target/gcov_dump.c writes each image's counters to the console as a gcov-tool merge-stream in hex:
    GCOV-BEGIN
    <hex lines>
    GCOV-END <length> <crc32>
Each transcript (suite_status.py --transcript) must hold exactly one complete stream whose length and CRC-32
match. All of them are checked before any is merged, so a run that stopped early, a stream cut short or
corrupted, or a target that reported GCOV-ERROR leaves the counters as they were and fails the decode. If a
merge itself fails, every counter file under the build directory is removed, so no partial report follows.
"""
import argparse
import os
import re
import shlex
import subprocess
import sys
import tempfile
import zlib

from host_coverage import counter_files

BEGIN = "GCOV-BEGIN"
END = re.compile(r"^GCOV-END ([0-9a-f]{8}) ([0-9a-f]{8})$")
ERROR = "GCOV-ERROR"
HEX = re.compile(r"^(?:[0-9a-f]{2})+$")


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


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--gcov-tool", required=True, help="gcov-tool of the compiler that built the images")
    parser.add_argument("--build-dir", required=True, help="the build whose .gcda files the streams name")
    parser.add_argument("transcripts", nargs="+")
    args = parser.parse_args(argv)

    streams = []
    errors = []
    for path in args.transcripts:
        try:
            with open(path, errors="replace") as transcript:
                streams.append((path, extract(transcript.read())))
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

    if failure := merge(shlex.split(args.gcov_tool), streams):
        partial = list(counter_files(args.build_dir))
        for path in partial:
            os.remove(path)
        print(f"decode_gcov.py: {failure}; removed {len(partial)} counter file(s)", file=sys.stderr)
        return 1
    if not any(True for _ in counter_files(args.build_dir)):
        print(f"decode_gcov.py: the streams wrote no counter file under {args.build_dir}: "
              "the images come from another build", file=sys.stderr)
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
