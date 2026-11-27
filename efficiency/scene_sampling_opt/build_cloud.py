"""Build an unsigned Scene-only API102 APK on Actions. No device or signing key."""
from __future__ import annotations
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import urllib.request
import xml.etree.ElementTree as ET
import zipfile

HERE = Path(__file__).resolve().parent
ANDROID = "{http://schemas.android.com/apk/res/android}"
API_SHA = "423484a6e1807e7a423c4b88fcd8176d104318259d91791877fed88fe91479d0"


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(*args: object) -> None:
    subprocess.run([str(arg) for arg in args], check=True, timeout=120)


def main() -> None:
    sdk = Path(os.environ["ANDROID_HOME"])
    tools = sdk / "build-tools/36.0.0"
    android = sdk / "platforms/android-36/android.jar"
    out = Path(os.environ["RUNNER_TEMP"]) / "scene-display-build"
    out.mkdir(exist_ok=False)
    manifest = ET.parse(HERE / "AndroidManifest.xml").getroot()
    package = manifest.attrib["package"]
    assert package == "cn.xiaocaiye.scenesamplingopt"
    version_name = manifest.attrib[ANDROID + "versionName"]
    version_code = int(manifest.attrib[ANDROID + "versionCode"])
    aar = out / "api.aar"
    url = "https://repo.maven.apache.org/maven2/io/github/libxposed/api/102.0.0/api-102.0.0.aar"
    with urllib.request.urlopen(url, timeout=30) as response:
        aar.write_bytes(response.read())
    if sha(aar) != API_SHA:
        raise RuntimeError("API102 input digest mismatch")
    api = out / "api.jar"
    with zipfile.ZipFile(aar) as archive:
        api.write_bytes(archive.read("classes.jar"))
    generated = out / "generated"
    info = generated / "cn/xiaocaiye/scenesamplingopt/BuildInfo.java"
    info.parent.mkdir(parents=True)
    info.write_text("package cn.xiaocaiye.scenesamplingopt; public final class BuildInfo {"
                    f" public static final int VERSION_CODE={version_code};"
                    f" public static final String VERSION_NAME={json.dumps(version_name)}; }}\n",
                    encoding="utf8")
    resources = out / "res/values"
    resources.mkdir(parents=True)
    (resources / "strings.xml").write_text(
        '<resources><string name="app_name">SceneSamplingOpt</string>'
        '<string name="app_description">Scene sampling and display persistence; API102.</string></resources>',
        encoding="utf8")
    run(tools / "aapt2", "compile", "--dir", out / "res", "-o", out / "resources.zip")
    run(tools / "aapt2", "link", "-o", out / "resources.apk", "-I", android,
        "--manifest", HERE / "AndroidManifest.xml", "--java", generated, out / "resources.zip")
    classes = out / "classes"
    classes.mkdir()
    production = sorted((HERE / "src").rglob("*.java"))
    sources = production + sorted(generated.rglob("*.java"))
    source_list = out / "sources.txt"
    source_list.write_text("\n".join('"' + str(path) + '"' for path in sources), encoding="utf8")
    run("javac", "-J-Xmx256m", "-encoding", "UTF-8", "-source", "17", "-target", "17",
        "-proc:none", "-classpath", str(android) + os.pathsep + str(api),
        "-d", classes, "@" + str(source_list))
    app_jar = out / "app.jar"
    with zipfile.ZipFile(app_jar, "w", zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(classes.rglob("*.class")):
            if path.relative_to(classes).parts[:2] == ("io", "github"):
                raise RuntimeError("compile-only API entered app classes")
            archive.write(path, path.relative_to(classes).as_posix())
    dex = out / "dex"
    dex.mkdir()
    run("java", "-Xmx384m", "-cp", tools / "lib/d8.jar", "com.android.tools.r8.D8",
        "--release", "--min-api", "28", "--lib", android, "--classpath", api,
        "--output", dex, app_jar)
    unsigned = out / "unsigned.apk"
    shutil.copyfile(out / "resources.apk", unsigned)
    with zipfile.ZipFile(unsigned, "a", zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(dex.glob("classes*.dex")):
            archive.write(path, path.name)
        for path in sorted((HERE / "META-INF/xposed").iterdir()):
            if path.is_file():
                archive.write(path, "META-INF/xposed/" + path.name)
    artifact = Path(os.environ["RUNNER_TEMP"]) / "scene-display-artifact"
    artifact.mkdir(exist_ok=False)
    aligned = artifact / ("SceneSamplingOpt-" + version_name + "-unsigned.apk")
    run(tools / "zipalign", "-f", "-p", "4", unsigned, aligned)
    run(tools / "zipalign", "-c", "4", aligned)
    report = {
        "package": package, "version_name": version_name, "version_code": version_code,
        "artifact_sha256": sha(aligned), "signed": False, "device_deployed": False,
        "api_sha256": API_SHA, "api_bundled": False,
        "source_sha256": {path.relative_to(HERE).as_posix(): sha(path) for path in production},
        "commit": os.environ["GITHUB_SHA"],
    }
    (artifact / "build-report.json").write_text(json.dumps(report, indent=2), encoding="utf8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
