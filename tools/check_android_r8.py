#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the shipped DEX/JNI boundary and R8 metadata, without an Android device."""

import argparse
import json
from pathlib import Path
import re
import subprocess
import tempfile
# Only SDK dexdump output is parsed; dex_api rejects DTD/entity declarations.
import xml.etree.ElementTree as ET  # nosemgrep: python.lang.security.use-defused-xml.use-defused-xml
import zipfile


ROOT = Path(__file__).resolve().parents[1]
R8_METADATA = "BUNDLE-METADATA/com.android.tools/r8.json"
R8_MAPPING = "BUNDLE-METADATA/com.android.tools.build.obfuscation/proguard.map"
APP_PACKAGE = "io/github/arancormonk/dsdneo/"
QT_PACKAGE = "org/qtproject/qt/android/"
# Qt 6.11.2 upcalls that include unannotated constructors and teardown/loader
# methods. The annotation/native-method rules cover the wider Qt JNI surface.
QT_CALLS = (
    ("QtNative", "activity", "()Landroid/app/Activity;", True),
    ("QtNative", "service", "()Landroid/app/Service;", True),
    ("QtNative", "getContext", "()Landroid/content/Context;", True),
    ("QtNative", "getQtThread", "()Lorg/qtproject/qt/android/QtThread;", True),
    ("QtNative", "setStarted", "(Z)V", True),
    ("QtThread", "exit", "()V", False),
    ("QtLoader", "isUncompressedNativeLibs", "()Z", True),
    ("QtWindow", "<init>", "(Landroid/content/Context;ZLorg/qtproject/qt/android/QtWindow;"
     "Lorg/qtproject/qt/android/QtInputConnection$QtInputConnectionListener;)V", False),
    ("QtWindow", "setParent", "(Lorg/qtproject/qt/android/QtWindow;)V", False),
    ("QtClipboardManager", "<init>", "(Landroid/content/Context;J)V", False),
    ("QtView", "createWindow", "(J)V", False),
    ("extras/QtAndroidBinder", "<init>", "(J)V", False),
    ("extras/QtAndroidBinder", "setId", "(J)V", False),
    ("extras/QtAndroidServiceConnection", "<init>", "(J)V", False),
    ("extras/QtAndroidServiceConnection", "setId", "(J)V", False),
)


def descriptor(java_type):
    primitives = {
        "void": "V", "boolean": "Z", "byte": "B", "char": "C", "short": "S",
        "int": "I", "long": "J", "float": "F", "double": "D",
    }
    if java_type.endswith("[]"):
        return "[" + descriptor(java_type[:-2])
    return primitives.get(java_type, "L" + java_type.replace(".", "/") + ";")


def dex_api(xml):
    """Use SDK dexdump's definitions, not string/method references in the DEX."""
    if "<!DOCTYPE" in xml or "<!ENTITY" in xml:
        raise ValueError("Unexpected DTD/entity declaration in dexdump output")
    classes, methods = set(), {}
    for package in ET.fromstring(xml).findall("package"):
        for cls in package.findall("class"):
            owner = (package.attrib["name"] + "." + cls.attrib["name"]).strip(".").replace(".", "/")
            classes.add(owner)
            for method in cls.findall("method") + cls.findall("constructor"):
                args = "".join(descriptor(p.attrib["type"]) for p in method.findall("parameter"))
                signature = "(" + args + ")" + descriptor(method.get("return", "void"))
                name = method.attrib["name"]
                if method.tag == "constructor":
                    name = "<clinit>" if method.get("static") == "true" else "<init>"
                methods[(owner, name, signature)] = method.attrib
    return classes, methods


def jni_contract(source_root):
    """Read literal QJniObject upcalls and exported downcalls from production C++."""
    upcalls, downcalls = set(), set()
    call = re.compile(
        r'QJniObject::callStatic(?:ObjectMethod|Method(?:<[^>]+>)?)\s*\(\s*'
        r'(\w+|"[^"\n]+")\s*,\s*"([^"\n]+)"\s*,\s*"([^"\n]+)"'
    )
    for path in (source_root / "android").glob("*.cpp"):
        source = re.sub(r'/\*.*?\*/|//[^\n]*', '', path.read_text(), flags=re.S)
        aliases = dict(re.findall(r'const char\*\s+(\w+)\s*=\s*"([^"\n]+)"', source))
        calls = list(call.finditer(source))
        if len(calls) != source.count("QJniObject::callStatic"):
            raise ValueError(f"Unrecognized JNI call syntax in {path}; extend the artifact check")
        for match in calls:
            owner, name, signature = match.groups()
            owner = owner.strip('"') if owner.startswith('"') else aliases[owner]
            if owner.startswith(APP_PACKAGE):
                upcalls.add((owner, name, signature))
        for name in re.findall(r'Java_io_github_arancormonk_dsdneo_DsdNative_(\w+)\s*\(', source):
            downcalls.add((APP_PACKAGE + "DsdNative", name))
    if not upcalls or not downcalls:
        raise ValueError("No app JNI contract found")
    return upcalls, downcalls


def verify_jni(classes, methods, upcalls, downcalls, qt_classes, qt_calls=QT_CALLS):
    errors = []
    for key in sorted(upcalls):
        if key not in methods or methods[key].get("static") != "true":
            errors.append(f"Missing static JNI entry point: {key}")
    native_methods = {(owner, name) for (owner, name, _), attrs in methods.items()
                      if attrs.get("native") == "true"}
    for key in sorted(downcalls - native_methods):
        errors.append(f"Missing native JNI binding: {key}")
    for owner in sorted(qt_classes - classes):
        errors.append(f"Missing Qt class referenced by a native library: {owner}")
    for owner, name, signature, static in qt_calls:
        key = (QT_PACKAGE + owner, name, signature)
        if key not in methods or methods[key].get("static") != str(static).lower():
            errors.append(f"Missing Qt JNI entry point: {key}")
    if errors:
        raise ValueError("\n".join(errors))


def verify_mapping(mapping):
    if "# compiler: R8" not in mapping or not re.search(r"(?m)^\S+ -> \S+:$", mapping):
        raise ValueError("Missing or empty R8 mapping (retain the mapping from this exact build)")


def verify_metadata(metadata):
    if not isinstance(metadata, dict):
        raise ValueError("Invalid R8 bundle metadata")
    options = metadata.get("options", {})
    if not isinstance(options, dict):
        raise ValueError("Invalid R8 options metadata")
    for option in ("isObfuscationEnabled", "isOptimizationsEnabled", "isShrinkingEnabled"):
        if options.get(option) is not True:
            raise ValueError(f"R8 bundle metadata does not enable {option}")
    resources = metadata.get("resourceOptimization", {})
    if not isinstance(resources, dict) or resources.get("isOptimizedShrinkingEnabled") is not True:
        raise ValueError("R8 resource shrinking is disabled")


def inspect_artifact(artifact, dexdump, source_root, mapping_path=None):
    classes, methods, qt_classes = set(), {}, set()
    dex_bytes = 0
    with zipfile.ZipFile(artifact) as archive, tempfile.TemporaryDirectory(prefix="dsd-r8-") as temp:
        entries = archive.namelist()
        dex_names = [name for name in entries if re.search(r"(?:^|/)classes\d*\.dex$", name)]
        if not dex_names:
            raise ValueError("Artifact contains no DEX files")
        for index, name in enumerate(dex_names):
            data = archive.read(name)
            dex_bytes += len(data)
            if b"~~R8{" not in data:
                raise ValueError(f"{name} has no R8 compiler marker")
            path = Path(temp) / f"classes{index}.dex"
            path.write_bytes(data)
            result = subprocess.run([str(dexdump), "-l", "xml", str(path)],
                                    check=True, capture_output=True, text=True)
            found_classes, found_methods = dex_api(result.stdout)
            classes.update(found_classes)
            methods.update(found_methods)
        for name in entries:
            if name.endswith(".so"):
                qt_classes.update(s.decode("ascii") for s in re.findall(
                    rb"org/qtproject/qt/android/[\w/$]+", archive.read(name)))
        if not qt_classes:
            raise ValueError("Artifact contains no Qt native JNI references")
        metadata = None
        if "base/manifest/AndroidManifest.xml" in entries:
            metadata = json.loads(archive.read(R8_METADATA))
            verify_metadata(metadata)
            verify_mapping(archive.read(R8_MAPPING).decode("utf-8"))
        elif mapping_path is None:
            raise ValueError("APK validation needs --mapping from the same build")
        if mapping_path is not None:
            verify_mapping(mapping_path.read_text())
    upcalls, downcalls = jni_contract(source_root)
    verify_jni(classes, methods, upcalls, downcalls, qt_classes)
    print(f"R8 OK: {len(upcalls)} app upcalls, {len(downcalls)} native bindings, "
          f"{len(QT_CALLS)} Qt upcalls, {len(qt_classes)} Qt JNI classes; "
          f"{dex_bytes:,} uncompressed DEX bytes "
          f"({dex_bytes / 1048576:.2f} MiB)")
    if metadata is not None:
        print("R8 bundle metadata: optimization, obfuscation, code/resource shrinking enabled; "
              "stats=" + json.dumps(metadata.get("stats", {}), sort_keys=True))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("artifact", type=Path, help="Release APK or AAB")
    parser.add_argument("--dexdump", type=Path, required=True, help="SDK build-tools dexdump")
    parser.add_argument("--mapping", type=Path, help="APK build/outputs/mapping/release/mapping.txt")
    parser.add_argument("--source-root", type=Path, default=ROOT)
    args = parser.parse_args()
    try:
        inspect_artifact(args.artifact, args.dexdump, args.source_root, args.mapping)
    except (ValueError, KeyError, OSError, zipfile.BadZipFile, ET.ParseError,
            subprocess.CalledProcessError) as error:
        parser.exit(1, f"R8 verification failed: {error}\n")


if __name__ == "__main__":
    main()
