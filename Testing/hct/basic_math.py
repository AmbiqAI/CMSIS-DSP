"""BasicMath for helia-core-tester: value generators and the descriptors they are run with.

Every case is recorded from the host reference (Testing/hct/host_reference), so a generator gives the
inputs and scalars only. Sizes sweep the Helium lane count of the element type: 1, lanes-1, lanes,
lanes+1, a typical block and a long one; elementwise kernels also take 0. Fixed-point outputs compare
bit-exactly against the host scalar build; FIXED_POINT_EXCEPTIONS lists the ones that may not, with why.

Not covered yet, as hct's pack cases do not take them: float16 (its goldens are recorded on x86-64
only), float64, the uint8 and uint32 bitwise kernels, q63 dot products, and in-place calls.
"""

import numpy as np

from helia_core_tester.pack import CaseSpec, PackError

HCT_PACK_API = "1.0"

FAMILY = "BasicMathFunctions"
DTYPES = {"q7_t": np.int8, "q15_t": np.int16, "q31_t": np.int32, "float32_t": np.float32, "uint16_t": np.uint16}
# Elements per 128-bit Helium vector.
LANES = {"q7_t": 16, "q15_t": 8, "q31_t": 4, "float32_t": 4, "uint16_t": 8}
FRACTIONAL_BITS = {"q7_t": 7, "q15_t": 15, "q31_t": 31}
SUFFIXES = {"q7": "q7_t", "q15": "q15_t", "q31": "q31_t", "f32": "float32_t", "u16": "uint16_t"}

# Fixed-point entries whose Helium build may differ from the host scalar reference, with the reason.
# Empty while one is undecided: Helium arm_mult_q31 (vqdmulhq) keeps the LSB that the scalar
# ((q63_t)a * b >> 32) << 1 clears, so it is 1 LSB above the host reference on about half the elements.
FIXED_POINT_EXCEPTIONS = {}

BINARY = ("add", "sub", "mult")
UNARY = ("abs", "negate")
FIXED = ("q7", "q15", "q31")


def _dtype(case, name):
    element = case.param(name).element
    if element not in DTYPES:
        raise PackError(f"{case.name}: {name} is {element}, which basic_math does not generate")
    return element, DTYPES[element]


def _values(case, element, n):
    """`n` inputs of `element`: full range, edges (the extremes, zero and one LSB around them), or
    unit / wide floats."""
    kind = case.values.get("values", "full")
    dtype = DTYPES[element]
    if np.issubdtype(dtype, np.integer):
        info = np.iinfo(dtype)
        if kind == "full":
            return case.rng.integers(info.min, info.max, n, endpoint=True, dtype=dtype)
        if kind == "edges":
            edges = np.array([info.min, info.min + 1, -1, 0, 1, info.max - 1, info.max], dtype=np.int64)
            edges = edges[(edges >= info.min) & (edges <= info.max)].astype(dtype)
            return case.rng.choice(edges, n)
    else:
        if kind == "unit":
            return case.rng.uniform(-1.0, 1.0, n).astype(dtype)
        if kind == "wide":
            return case.rng.uniform(-1.0e3, 1.0e3, n).astype(dtype)
    raise PackError(f"{case.name}: values {kind!r} do not apply to {element}")


def _n(case):
    n = case.values.get("n")
    if isinstance(n, bool) or not isinstance(n, int) or n < 0:
        raise PackError(f"{case.name}: n must be a non-negative integer, got {n!r}")
    return n


def binary(case):
    n = _n(case)
    element, _ = _dtype(case, "pSrcA")
    return CaseSpec(arrays={"pSrcA": _values(case, element, n), "pSrcB": _values(case, element, n)},
                    output="pDst", output_count=n, scalars={"blockSize": n})


def unary(case):
    n = _n(case)
    element, _ = _dtype(case, "pSrc")
    return CaseSpec(arrays={"pSrc": _values(case, element, n)}, output="pDst", output_count=n,
                    scalars={"blockSize": n})


def offset(case):
    n = _n(case)
    element, dtype = _dtype(case, "pSrc")
    value = case.values["offset"]
    return CaseSpec(arrays={"pSrc": _values(case, element, n)}, output="pDst", output_count=n,
                    scalars={"offset": dtype(value).item(), "blockSize": n})


def shift(case):
    n = _n(case)
    element, _ = _dtype(case, "pSrc")
    return CaseSpec(arrays={"pSrc": _values(case, element, n)}, output="pDst", output_count=n,
                    scalars={"shiftBits": int(case.values["shift_bits"]), "blockSize": n})


def scale(case):
    n = _n(case)
    element, dtype = _dtype(case, "pSrc")
    arrays = {"pSrc": _values(case, element, n)}
    if element == "float32_t":
        return CaseSpec(arrays=arrays, output="pDst", output_count=n,
                        scalars={"scale": float(case.values["scale"]), "blockSize": n})
    return CaseSpec(arrays=arrays, output="pDst", output_count=n,
                    scalars={"scaleFract": dtype(case.values["scale_fract"]).item(),
                             "shift": int(case.values["shift"]), "blockSize": n})


def clip(case):
    n = _n(case)
    element, dtype = _dtype(case, "pSrc")
    low, high = case.values["low"], case.values["high"]
    if not low <= high:
        raise PackError(f"{case.name}: low {low} is above high {high}")
    cast = float if element == "float32_t" else (lambda v: dtype(v).item())
    return CaseSpec(arrays={"pSrc": _values(case, element, n)}, output="pDst", output_count=n,
                    scalars={"low": cast(low), "high": cast(high), "numSamples": n})


def dot_prod(case):
    n = _n(case)
    element, _ = _dtype(case, "pSrcA")
    return CaseSpec(arrays={"pSrcA": _values(case, element, n), "pSrcB": _values(case, element, n)},
                    output="result", output_count=1, scalars={"blockSize": n})


def bitwise(case):
    n = _n(case)
    names = [p.name for p in case.params if p.pointer and p.direction == "in"]
    element, _ = _dtype(case, names[0])
    return CaseSpec(arrays={name: _values(case, element, n) for name in names}, output="pDst",
                    output_count=n, scalars={"blockSize": n})


def _sizes(element, elementwise=True):
    lanes = LANES[element]
    sizes = [1, lanes - 1, lanes, lanes + 1, 4 * lanes + 3, 255]
    return sorted({s for s in sizes if s > 0} | ({0} if elementwise else set()))


def _doc(name, entry, **params):
    return {"schema": "hct/descriptor/1", "name": name, "entry": entry, "params": params}


def _kinds(suffix):
    return ("full", "edges") if suffix in FIXED or suffix == "u16" else ("unit", "wide")


def _sweep(op, suffix, extra=None, elementwise=True):
    element = SUFFIXES[suffix]
    docs = []
    for kind in _kinds(suffix):
        for n in _sizes(element, elementwise):
            for tag, params in (extra or {"": {}}).items():
                name = f"{op}_{suffix}_n{n}_{kind}{('_' + tag) if tag else ''}"
                docs.append(_doc(name, f"arm_{op}_{suffix}", n=n, values=kind, **params))
    return docs


def _offsets(suffix):
    if suffix == "f32":
        return {"pos": {"offset": 0.75}, "neg": {"offset": -12.5}}
    info = np.iinfo(DTYPES[SUFFIXES[suffix]])
    return {"pos": {"offset": int(info.max // 3)}, "neg": {"offset": int(info.min // 5)}, "max": {"offset": int(info.max)}}


def _shifts(suffix):
    bits = FRACTIONAL_BITS[SUFFIXES[suffix]]
    return {f"s{'m' if s < 0 else ''}{abs(s)}": {"shift_bits": s} for s in (-bits, -1, 0, 1, bits)}


def _scales(suffix):
    if suffix == "f32":
        return {"half": {"scale": 0.5}, "neg": {"scale": -3.25}}
    info = np.iinfo(DTYPES[SUFFIXES[suffix]])
    return {"half_s0": {"scale_fract": int(info.max // 2), "shift": 0},
            "max_s1": {"scale_fract": int(info.max), "shift": 1},
            "neg_s3": {"scale_fract": int(info.min // 3), "shift": 3}}


def _clips(suffix):
    if suffix == "f32":
        return {"narrow": {"low": -0.25, "high": 0.5}}
    info = np.iinfo(DTYPES[SUFFIXES[suffix]])
    return {"narrow": {"low": int(info.min // 4), "high": int(info.max // 2)},
            "point": {"low": 0, "high": 0}}


def descriptors(contracts):
    """Every BasicMath case, one file per operation; entries the contract lacks are left out."""
    functions = getattr(contracts, "functions", {}) or {}
    files = {}

    def add(op, docs):
        kept = [doc for doc in docs if doc["entry"] in functions]
        if kept:
            files.setdefault(f"{FAMILY}/{op}.yaml", []).extend(kept)

    for op in BINARY:
        for suffix in FIXED + ("f32",):
            add(op, _sweep(op, suffix))
    for op in UNARY:
        for suffix in FIXED + ("f32",):
            add(op, _sweep(op, suffix))
    for suffix in FIXED + ("f32",):
        add("offset", _sweep("offset", suffix, _offsets(suffix)))
        add("scale", _sweep("scale", suffix, _scales(suffix)))
        add("clip", _sweep("clip", suffix, _clips(suffix)))
    for suffix in FIXED:
        add("shift", _sweep("shift", suffix, _shifts(suffix)))
    for suffix in ("q7", "f32"):
        add("dot_prod", _sweep("dot_prod", suffix, elementwise=False))
    for op in ("and", "or", "xor", "not"):
        add("bitwise", _sweep(op, "u16"))
    return files


def register(pack):
    fixed_and_f32 = [f"arm_{{}}_{s}" for s in FIXED + ("f32",)]
    pack.generator("basic_math.binary", binary,
                   entries=[p.format(op) for op in BINARY for p in fixed_and_f32])
    pack.generator("basic_math.unary", unary, entries=[p.format(op) for op in UNARY for p in fixed_and_f32])
    pack.generator("basic_math.offset", offset, entries=[p.format("offset") for p in fixed_and_f32])
    pack.generator("basic_math.scale", scale, entries=[p.format("scale") for p in fixed_and_f32])
    pack.generator("basic_math.clip", clip, entries=[p.format("clip") for p in fixed_and_f32])
    pack.generator("basic_math.shift", shift, entries=[f"arm_shift_{s}" for s in FIXED])
    pack.generator("basic_math.dot_prod", dot_prod, entries=["arm_dot_prod_q7", "arm_dot_prod_f32"])
    pack.generator("basic_math.bitwise", bitwise, entries=[f"arm_{op}_u16" for op in ("and", "or", "xor", "not")])
    pack.descriptor_source(descriptors)
