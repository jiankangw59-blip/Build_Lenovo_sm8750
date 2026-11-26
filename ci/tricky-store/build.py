"""Rebuild the Java DEX for AUTO-mode virtual-key recovery from pinned sources."""
import argparse
import hashlib
from pathlib import Path
import subprocess
from zipfile import ZipFile, ZIP_DEFLATED


HERE = Path(__file__).resolve().parent
SOURCE = HERE / "TrickyStoreOSS-maple2-public-source.zip"
BASELINE = HERE / "TSOSS-maple2-native-template.zip"
SEAM = """                when {
                    PkgConfig.needGenerate(callingUid) -> {"""
FIX = """                // A persisted virtual key remains owned by this caller when its
                // current policy resolves to AUTO/leaf hacking. The real keystore
                // has no matching key to serve. Authorization was checked above.
                authorized?.info?.response?.let { response ->
                    return@runCatching safeTypedObjectReply(response, "persisted")
                }
                when {
                    PkgConfig.needGenerate(callingUid) -> {"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("workspace", type=Path)
    parser.add_argument("--prepare-only", action="store_true")
    args = parser.parse_args()
    if hashlib.sha256(SOURCE.read_bytes()).hexdigest() != "8751a8498c6d10448b36900fcc38365126d4b8727b21f8c2ed14bdca120541ad":
        raise ValueError("Pinned source archive hash differs")
    if hashlib.sha256(BASELINE.read_bytes()).hexdigest() != "bf8f70dc4657cdc9e10617dae48615625d9633df9315ff5263371397edd18730":
        raise ValueError("Pinned native baseline archive hash differs")
    root = args.workspace / "TrickyStoreOSS"
    if args.prepare_only:
        with ZipFile(SOURCE) as archive:
            archive.extractall(args.workspace)
        source = root / "app/src/main/java/io/github/beakthoven/TrickyStoreOSS/interceptors/Keystore2Interceptor.kt"
        text = source.read_text(encoding="utf-8")
        if text.count(SEAM) != 1:
            raise ValueError("Pinned getKeyEntry source seam differs")
        source.write_text(text.replace(SEAM, FIX), encoding="utf-8", newline="\n")
        (root / "gradlew").chmod(0o755)
        return
    dex = root / "app/build/intermediates/dex/release/minifyReleaseWithR8/classes.dex"
    payload = dex.read_bytes()
    if not payload.startswith(b"dex\n"):
        raise ValueError("Missing compiled release DEX")
    output = args.workspace / "TSOSS-3.1.0-201-compat10-maple.3-auto1-arm64.zip"
    with ZipFile(BASELINE) as baseline, ZipFile(output, "w", ZIP_DEFLATED) as package:
        for name in baseline.namelist():
            data = baseline.read(name)
            if name == "classes.dex":
                data = payload
            if name == "module.prop":
                data = data.replace(b"maple.2", b"maple.3-auto1").replace(b"versionCode=211", b"versionCode=212")
            package.writestr(name, data)
    with ZipFile(SOURCE) as source, ZipFile(BASELINE) as baseline:
        print("source_sha256=" + hashlib.sha256(SOURCE.read_bytes()).hexdigest())
        print("baseline_sha256=" + hashlib.sha256(BASELINE.read_bytes()).hexdigest())
    print("dex_sha256=" + hashlib.sha256(payload).hexdigest())
    print("module_sha256=" + hashlib.sha256(output.read_bytes()).hexdigest())


if __name__ == "__main__":
    main()
