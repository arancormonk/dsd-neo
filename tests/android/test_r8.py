#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Regressions for validation of the optimized Android artifacts."""

from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import check_android_r8 as r8


DEX_XML = """<api><package name="io.github.arancormonk.dsdneo">
<class name="DecoderService">
  <method name="startDecoder" return="void" static="true" native="false">
    <parameter type="android.content.Context"/><parameter type="java.lang.String[]"/><parameter type="long"/>
  </method>
  <method name="lifecycleStatus" return="java.lang.String" static="true" native="false"/>
</class>
<class name="DsdNative">
  <method name="nativeRun" return="int" static="false" native="true"/>
</class>
</package><package name="org.qtproject.qt.android">
<class name="QtWindow">
  <constructor name="QtWindow" static="false"><parameter type="long"/></constructor>
</class>
<class name="QtApkFileEngine$JFileInfo">
  <field name="relativePath" type="java.lang.String" static="false"/>
  <field name="size" type="long" static="false"/>
  <field name="isDir" type="boolean" static="false"/>
</class>
</package></api>"""
START = (r8.APP_PACKAGE + "DecoderService", "startDecoder",
         "(Landroid/content/Context;[Ljava/lang/String;J)V")
NATIVE = (r8.APP_PACKAGE + "DsdNative", "nativeRun")


class R8ArtifactTests(unittest.TestCase):
    def check_api(self, xml=DEX_XML, upcalls=None, downcalls=None, qt_classes=None, qt_calls=()):
        classes, methods, fields = r8.dex_api(xml)
        r8.verify_jni(classes, methods, fields, upcalls if upcalls is not None else {START},
                      downcalls if downcalls is not None else {NATIVE},
                      qt_classes or {r8.QT_PACKAGE + "QtWindow"}, qt_calls)

    def test_preserved_jni_contract(self):
        self.check_api(qt_calls=(("QtWindow", "<init>", "(J)V", False),))

    def test_removed_or_renamed_upcall_fails(self):
        with self.assertRaisesRegex(ValueError, "Missing static JNI entry point"):
            self.check_api(DEX_XML.replace('name="startDecoder"', 'name="a"'))

    def test_changed_signature_fails(self):
        with self.assertRaisesRegex(ValueError, "Missing static JNI entry point"):
            self.check_api(DEX_XML.replace("java.lang.String[]", "java.lang.Object[]"))

    def test_instance_method_cannot_replace_static_bridge(self):
        with self.assertRaisesRegex(ValueError, "Missing static JNI entry point"):
            self.check_api(DEX_XML.replace('static="true"', 'static="false"'))

    def test_native_binding_must_still_be_native(self):
        with self.assertRaisesRegex(ValueError, "Missing native JNI binding"):
            self.check_api(DEX_XML.replace('native="true"', 'native="false"'))

    def test_native_binding_class_name_must_survive(self):
        with self.assertRaisesRegex(ValueError, "Missing native JNI binding"):
            self.check_api(DEX_XML.replace('name="DsdNative"', 'name="a"'))

    def test_native_qt_class_cannot_disappear(self):
        with self.assertRaisesRegex(ValueError, "Missing Qt class"):
            self.check_api(qt_classes={r8.QT_PACKAGE + "QtServiceBase"})

    def test_unannotated_qt_method_cannot_disappear(self):
        with self.assertRaisesRegex(ValueError, "Missing Qt JNI entry point"):
            self.check_api(qt_calls=(("QtWindow", "setParent", "(Lorg/qtproject/qt/android/QtWindow;)V", False),))

    def test_apk_file_info_field_cannot_be_removed_or_renamed(self):
        for name in ("relativePath", "size", "isDir"):
            with self.subTest(field=name), self.assertRaisesRegex(ValueError, "Missing Qt JNI field"):
                self.check_api(DEX_XML.replace(f'name="{name}"', 'name="a"'))
        for field in ('<field name="size" type="long" static="false"/>',
                      '<field name="isDir" type="boolean" static="false"/>'):
            with self.subTest(removed=field), self.assertRaisesRegex(ValueError, "Missing Qt JNI field"):
                self.check_api(DEX_XML.replace(field, ""))

    def test_apk_file_info_field_descriptor_and_staticness_must_match(self):
        for old, new in (('type="long"', 'type="int"'),
                         ('name="size" type="long" static="false"',
                          'name="size" type="long" static="true"')):
            with self.subTest(change=new), self.assertRaisesRegex(ValueError, "Missing Qt JNI field"):
                self.check_api(DEX_XML.replace(old, new))

    def test_arrays_and_primitive_descriptors(self):
        self.assertEqual(r8.descriptor("long[][]"), "[[J")
        self.assertEqual(r8.descriptor("org.qtproject.qt.android.QtInputConnection$QtInputConnectionListener"),
                         "Lorg/qtproject/qt/android/QtInputConnection$QtInputConnectionListener;")

    def test_dtd_and_entity_declarations_are_refused(self):
        for declaration in ('<!DOCTYPE api [<!ENTITY value "expanded">]>', '<!ENTITY value "expanded">'):
            with self.subTest(declaration=declaration), self.assertRaisesRegex(ValueError, "DTD/entity"):
                r8.dex_api(declaration + DEX_XML)

    def test_production_contract_reads_descriptors_and_all_native_exports(self):
        upcalls, downcalls = r8.jni_contract(ROOT)
        self.assertIn(START, upcalls)
        self.assertIn((r8.APP_PACKAGE + "DecoderService", "lifecycleStatus", "()Ljava/lang/String;"), upcalls)
        self.assertIn((r8.APP_PACKAGE + "DecoderService", "retireAttempts", "(J)Z"), upcalls)
        self.assertIn((r8.APP_PACKAGE + "DocumentCopies", "copyDocumentToFile",
                       "(Landroid/content/ContentResolver;Ljava/lang/String;Ljava/lang/String;J)I"), upcalls)
        self.assertIn(NATIVE, downcalls)
        self.assertIn((r8.APP_PACKAGE + "DsdNative", "nativeHostDiagnostic"), downcalls)
        self.assertIn((r8.APP_PACKAGE + "DsdNative", "nativeQuitUi"), downcalls)

    def test_mapping_requires_r8_and_class_mappings(self):
        r8.verify_mapping("# compiler: R8\nexample.Policy -> a.b:\n")
        for invalid in ("", "# compiler: D8\nexample.Policy -> a.b:\n", "# compiler: R8\n"):
            with self.subTest(mapping=invalid), self.assertRaises(ValueError):
                r8.verify_mapping(invalid)

    def test_metadata_requires_each_optimization(self):
        options = dict.fromkeys(("isObfuscationEnabled", "isOptimizationsEnabled", "isShrinkingEnabled"), True)
        metadata = {"options": options, "resourceOptimization": {"isOptimizedShrinkingEnabled": True}}
        r8.verify_metadata(metadata)
        for option in options:
            with self.subTest(option=option), self.assertRaises(ValueError):
                r8.verify_metadata({**metadata, "options": {**options, option: False}})
        with self.assertRaisesRegex(ValueError, "resource shrinking"):
            r8.verify_metadata({"options": options})
        for invalid in ({}, [], {"options": []}, {"options": options, "resourceOptimization": []}):
            with self.subTest(metadata=invalid), self.assertRaises(ValueError):
                r8.verify_metadata(invalid)


if __name__ == "__main__":
    unittest.main()
