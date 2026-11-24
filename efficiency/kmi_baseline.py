"""Extract OEM import CRCs and BTF structure layouts without publishing OEM code."""
import argparse
import gzip
import hashlib
import json
import pathlib
import re
import struct
import tarfile


def elf_sections(blob):
    if blob[:6] != b"\x7fELF\x02\x01":
        raise ValueError("Expected a little-endian ELF64 image")
    shoff = struct.unpack_from("<Q", blob, 40)[0]
    entsize, count, strindex = struct.unpack_from("<HHH", blob, 58)
    headers = [struct.unpack_from("<IIQQQQIIQQ", blob, shoff + i * entsize)
               for i in range(count)]
    names_header = headers[strindex]
    names = blob[names_header[4]:names_header[4] + names_header[5]]
    result = {}
    for h in headers:
        end = names.find(b"\0", h[0])
        name = names[h[0]:end].decode("ascii", "replace")
        if h[1] != 8:  # SHT_NOBITS has no bytes in the file.
            result[name] = blob[h[4]:h[4] + h[5]]
    return result


def module_info(blob):
    sections = elf_sections(blob)
    metadata = {}
    for field in sections.get(".modinfo", b"").split(b"\0"):
        if b"=" in field:
            key, value = field.split(b"=", 1)
            metadata[key.decode()] = value.decode("utf-8", "replace")
    versions = sections.get("__versions", b"")
    if len(versions) % 64:
        raise ValueError("Unexpected module __versions record size")
    crcs = {}
    for offset in range(0, len(versions), 64):
        crc = struct.unpack_from("<Q", versions, offset)[0]
        symbol = versions[offset + 8:offset + 64].split(b"\0", 1)[0].decode()
        crcs[symbol] = f"0x{crc:08x}"
    if "__version_ext_names" in sections:
        names = sections["__version_ext_names"].rstrip(b"\0").split(b"\0")
        values = sections["__version_ext_crcs"]
        if len(values) != len(names) * 4:
            raise ValueError("Unexpected extended modversion data")
        crcs.update({n.decode(): f"0x{struct.unpack_from('<I', values, i * 4)[0]:08x}"
                     for i, n in enumerate(names)})
    return {"name": metadata.get("name"), "vermagic": metadata.get("vermagic"),
            "sha256": hashlib.sha256(blob).hexdigest(), "imports": crcs}


class Btf:
    def __init__(self, blob):
        if blob[:4] == b"\x7fELF":
            blob = elf_sections(blob)[".BTF"]
        magic, version, flags, hdrlen, toff, tlen, soff, slen = struct.unpack_from("<HBBIIIII", blob)
        if magic != 0xEB9F or version != 1:
            raise ValueError("Unsupported BTF format")
        self.strings = blob[hdrlen + soff:hdrlen + soff + slen]
        self.types = [None]
        data = blob[hdrlen + toff:hdrlen + toff + tlen]
        offset = 0
        while offset < len(data):
            nameoff, info, size = struct.unpack_from("<III", data, offset)
            offset += 12
            kind, vlen, kflag = (info >> 24) & 31, info & 65535, info >> 31
            record = {"name": self.string(nameoff), "kind": kind, "size_or_type": size}
            if kind == 1:
                record["encoding"] = struct.unpack_from("<I", data, offset)[0]
                extra = 4
            elif kind == 3:
                record["array"] = struct.unpack_from("<III", data, offset)
                extra = 12
            elif kind in (4, 5):
                record["members"] = []
                for n in range(vlen):
                    member_name, member_type, position = struct.unpack_from("<III", data, offset + n * 12)
                    record["members"].append({
                        "name": self.string(member_name), "type": member_type,
                        "offset_bits": position & 0xFFFFFF if kflag else position,
                        "bitfield_bits": position >> 24 if kflag else 0})
                extra = vlen * 12
            elif kind in (6, 13): extra = vlen * 8
            elif kind in (15, 19): extra = vlen * 12
            elif kind in (14, 17): extra = 4
            elif kind in (2, 7, 8, 9, 10, 11, 12, 16, 18): extra = 0
            else: raise ValueError(f"Unknown BTF kind {kind}")
            offset += extra
            self.types.append(record)
        if offset != len(data):
            raise ValueError("BTF type section bounds mismatch")

    def string(self, offset):
        return self.strings[offset:self.strings.index(0, offset)].decode("utf-8", "replace")

    def describe(self, tid, depth=0):
        if not tid: return "void"
        t = self.types[tid]
        kind, name, size = t["kind"], t["name"], t["size_or_type"]
        if kind in (8, 9, 10, 11, 18): return self.describe(size, depth)
        if kind == 2: return "pointer:" + self.describe(size, depth + 1) if depth < 2 else "pointer"
        if kind == 3:
            elem, index, count = t["array"]
            return f"array[{count}]:" + self.describe(elem, depth + 1)
        if kind in (4, 5): return f"{'struct' if kind == 4 else 'union'}:{name}:{size}"
        if kind == 1: return f"int:{name}:{size}:{t['encoding']}"
        return f"kind{kind}:{name}:{size if kind in (6, 16, 19) else ''}"

    def layouts(self, requested):
        result = {}
        for t in self.types[1:]:
            if t["kind"] == 4 and t["name"] in requested:
                layout = {"size": t["size_or_type"], "members": [
                    {"name": m["name"], "offset_bits": m["offset_bits"],
                     "bitfield_bits": m["bitfield_bits"], "type": self.describe(m["type"])}
                    for m in t["members"]]}
                if t["name"] in result and result[t["name"]] != layout:
                    raise ValueError("Conflicting BTF definitions for " + t["name"])
                result[t["name"]] = layout
        missing = set(requested) - result.keys()
        if missing: raise ValueError("BTF lacks required structures: " + ", ".join(sorted(missing)))
        return result


def capture(args):
    result = {"modules": {}, "structures": Btf(args.btf.read_bytes()).layouts(
        ["task_struct", "rq", "tcp_sock", "inet_connection_sock"])}
    result["gki_exports"] = sorted(set(args.gki_exports.read_text(encoding="utf-8").splitlines()))
    if not result["gki_exports"]:
        raise ValueError("Baseline kernel export inventory is empty")
    loaded = set(args.loaded.read_text(encoding="utf-8").splitlines()) if args.loaded else None
    for archive in args.modules:
        with tarfile.open(archive) as tf:
            for entry in tf:
                if not entry.isfile() or not entry.name.endswith(".ko"): continue
                info = module_info(tf.extractfile(entry).read())
                name = info["name"] or pathlib.PurePosixPath(entry.name).stem.replace("-", "_")
                info["loaded_at_baseline"] = name in loaded if loaded is not None else None
                result["modules"][name] = info
    args.output.write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps({"modules": len(result["modules"]),
                      "loaded_modules": sum(m["loaded_at_baseline"] is True for m in result["modules"].values()),
                      "structure_sizes": {k: v["size"] for k, v in result["structures"].items()}}, indent=2))


def verify(args):
    raw = gzip.decompress(args.baseline.read_bytes()) if args.baseline.suffix == ".gz" else args.baseline.read_bytes()
    baseline = json.loads(raw)
    exports = {}
    for line in args.symvers.read_text(encoding="utf-8").splitlines():
        fields = line.split()
        if len(fields) >= 2: exports[fields[1]] = fields[0].lower()
    failures = []
    checked = 0
    baseline_exports = set(baseline["gki_exports"])
    for name, module in baseline["modules"].items():
        if module["loaded_at_baseline"] is not True: continue
        for symbol, crc in module["imports"].items():
            # Other vendor modules supply these symbols; they are checked on-device.
            if symbol not in baseline_exports: continue
            checked += 1
            if symbol not in exports or int(exports[symbol], 16) != int(crc, 16):
                failures.append({"module": name, "symbol": symbol, "expected": crc, "actual": exports.get(symbol)})
    btf = Btf(args.vmlinux.read_bytes())
    expected_layouts = baseline["structures"].copy()
    scan_control = None
    if args.kernel_version == "6.6.118":
        path = pathlib.Path(__file__).with_name("kmi") / "plq110-6.6.118-layouts.json.gz"
        snapshot = path.read_bytes()
        if hashlib.sha256(snapshot).hexdigest() != "6d61bb7c0bd20530ddfe15c6158aecfe4cf207a8bb0b5d33682862901459ec09":
            raise RuntimeError("Unreviewed 6.6.118 KMI layout baseline")
        scan_control = json.loads(gzip.decompress(snapshot))["structs"]["scan_control"]
        # The pinned OnePlus source keeps the 6.6.89 OEM scheduler layout
        # with SCHED_CLASS_EXT enabled, adding dmabuf_info in task KABI slot 3.
        # Keep every other task field and the entire OEM rq layout exact.
        task = expected_layouts["task_struct"]
        members = list(task["members"])
        reserved3 = {"name": "android_kabi_reserved3", "offset_bits": 28736,
                     "bitfield_bits": 0, "type": "int:unsigned long long:8:64"}
        members[members.index(reserved3)] = {**reserved3, "name": "", "type": "union::8"}
        expected_layouts["task_struct"] = {**task, "members": members}
    actual = btf.layouts(expected_layouts)
    from memory_layout import verify_memory_layouts
    memory_failures, memory_actual = verify_memory_layouts(btf, scan_control)
    failures.extend(memory_failures)
    for name, expected in expected_layouts.items():
        compatible = actual[name] == expected
        if name in ("tcp_sock", "inet_connection_sock"):
            # BBRv3 can consume an existing KABI reserve or unused bits. Every
            # previously public field, its offset and type must remain identical.
            def reserved(member):
                return bool(re.fullmatch(r"unused\d*|android_kabi_reserved\d+", member["name"]))
            protected = [m for m in expected["members"] if not reserved(m)]
            compatible = actual[name]["size"] == expected["size"] and all(m in actual[name]["members"] for m in protected)
            reserves = []
            for member in expected["members"]:
                if reserved(member):
                    size = member["bitfield_bits"] or (64 if member["name"].startswith("android_kabi_reserved") else 0)
                    if size: reserves.append((member["offset_bits"], member["offset_bits"] + size))
            for member in actual[name]["members"]:
                if member in protected: continue
                size = member["bitfield_bits"]
                if not size:
                    match = re.fullmatch(r"(?:int|struct|union):[^:]*:(\d+)(?::.*)?", member["type"])
                    size = int(match.group(1)) * 8 if match else (64 if member["type"].startswith("pointer:") else 0)
                start = member["offset_bits"]
                if not size or not any(low <= start and start + size <= high for low, high in reserves):
                    compatible = False
        if not compatible:
            failures.append({"structure": name, "reason": "BTF layout or member type changed",
                             "expected_size": expected["size"], "actual_size": actual[name]["size"]})
    if checked == 0: failures.append({"reason": "No baseline loaded OEM imports were checked"})
    report = {"checked_imports": checked, "failures": failures, "passed": not failures,
              "actual_structures": actual,
              "memory_structures": memory_actual,
              "scope": "GKI CRCs referenced by baseline loaded OEM modules and six explicit BTF structure layouts; runtime module loading remains required."}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps({"checked_imports": checked, "passed": not failures,
                      "failure_count": len(failures), "first_failures": failures[:10]}, indent=2))
    if failures: raise SystemExit(1)


def quick_crc(args):
    raw = gzip.decompress(args.baseline.read_bytes()) if args.baseline.suffix == ".gz" else args.baseline.read_bytes()
    baseline = json.loads(raw)
    expected = {int(m["imports"]["module_layout"], 16)
                for m in baseline["modules"].values()
                if m["loaded_at_baseline"] and "module_layout" in m["imports"]}
    if len(expected) != 1:
        raise RuntimeError("OEM baseline does not agree on module_layout CRC")
    matches = re.findall(r"^#SYMVER\s+module_layout\s+(0x[0-9a-fA-F]+)\s*$",
                         args.cmd_file.read_text(encoding="utf-8"), re.M)
    actual = {int(value, 16) for value in matches}
    result = {"symbol": "module_layout", "expected": [f"0x{x:08x}" for x in expected],
              "actual": [f"0x{x:08x}" for x in actual], "passed": actual == expected,
              "scope": "Early common-type CRC check; full post-link OEM import and BTF checks remain mandatory."}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))
    if not result["passed"]:
        raise SystemExit(1)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    commands = parser.add_subparsers(dest="command", required=True)
    cap = commands.add_parser("capture")
    cap.add_argument("--btf", type=pathlib.Path, required=True)
    cap.add_argument("--modules", type=pathlib.Path, nargs="+", required=True)
    cap.add_argument("--loaded", type=pathlib.Path)
    cap.add_argument("--gki-exports", type=pathlib.Path, required=True)
    cap.add_argument("--output", type=pathlib.Path, required=True)
    check = commands.add_parser("verify")
    check.add_argument("--baseline", type=pathlib.Path, required=True)
    check.add_argument("--symvers", type=pathlib.Path, required=True)
    check.add_argument("--vmlinux", type=pathlib.Path, required=True)
    check.add_argument("--kernel-version", choices=["6.6.89", "6.6.118"], default="6.6.89")
    check.add_argument("--output", type=pathlib.Path, required=True)
    quick = commands.add_parser("quick-crc")
    quick.add_argument("--baseline", type=pathlib.Path, required=True)
    quick.add_argument("--cmd-file", type=pathlib.Path, required=True)
    quick.add_argument("--output", type=pathlib.Path, required=True)
    opts = parser.parse_args()
    {"capture": capture, "verify": verify, "quick-crc": quick_crc}[opts.command](opts)
