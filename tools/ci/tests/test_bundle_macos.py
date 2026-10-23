"""Native Mach-O packaging regressions, with no SDK or game data required."""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


BUNDLER = Path(__file__).resolve().parents[1] / "bundle_macos.sh"


@unittest.skipUnless(sys.platform == "darwin", "requires Apple's Mach-O tools")
class BundleMacOSTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="project8 macos ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "original libraries"
        self.out = self.root / "portable install"
        self.source.mkdir()
        self.out.mkdir()

    def run_tool(self, *args):
        return subprocess.run([str(arg) for arg in args], check=True,
                              text=True, capture_output=True)

    def library(self, name, code, install_id, *flags, directory=None):
        directory = directory or self.source
        directory.mkdir(parents=True, exist_ok=True)
        source = directory / (name + ".c")
        binary = directory / (name + ".dylib")
        source.write_text(code)
        self.run_tool("xcrun", "clang", "-dynamiclib", source,
                      "-Wl,-headerpad_max_install_names", "-install_name", install_id,
                      *flags, "-o", binary)
        return binary

    def executable(self, library, *flags):
        source = self.root / "main.c"
        source.write_text('#include <stdio.h>\nint codec(void);\n'
                          'int main(void) { printf("%d\\n", codec()); return 0; }\n')
        binary = self.out / "launcher"
        self.run_tool("xcrun", "clang", source, library,
                      "-Wl,-headerpad_max_install_names", *flags, "-o", binary)
        return binary

    def package(self, binary, success=True):
        result = subprocess.run(["bash", str(BUNDLER), str(self.out), str(binary)],
                                text=True, capture_output=True)
        if success:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def launch_without_sources(self):
        # A builder's libraries must not mask an incomplete package. Remove the
        # source tree and relocate the result before asking dyld to load it.
        shutil.rmtree(self.source)
        moved = self.root / "moved install"
        self.out.rename(moved)
        for binary in moved.iterdir():
            self.run_tool("codesign", "--force", "--sign", "-", binary)
        env = {key: value for key, value in os.environ.items()
               if not key.startswith("DYLD_")}
        result = subprocess.run([str(moved / "launcher")], env=env,
                                text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "42\n")

    def test_transitive_rpath_and_loader_relative_dependencies(self):
        cms_dir = self.source / "cms"
        helper = self.library("helper", "int helper(void) { return 42; }",
                              "@loader_path/helper.dylib", directory=cms_dir)
        cms = self.library("cms", "int helper(void); int cms(void) { return helper(); }",
                           "@rpath/cms.dylib", helper, directory=cms_dir)
        codec_path = self.source / "codec.dylib"
        codec = self.library("codec", "int cms(void); int codec(void) { return cms(); }",
                             str(codec_path), cms, "-Wl,-rpath,@loader_path/cms")
        binary = self.executable(codec)
        self.package(binary)
        for name in ("codec", "cms", "helper"):
            self.assertTrue((self.out / (name + ".dylib")).is_file())
        self.package(binary)  # Re-running on an already normalised stage works.
        self.launch_without_sources()

    def test_rpath_inherited_from_executable(self):
        cms_dir = self.source / "cms"
        cms = self.library("cms", "int cms(void) { return 42; }",
                           "@rpath/cms.dylib", directory=cms_dir)
        codec = self.library("codec", "int cms(void); int codec(void) { return cms(); }",
                             str(self.source / "codec.dylib"), cms)
        binary = self.executable(codec, "-Wl,-rpath," + str(cms_dir))
        self.package(binary)
        self.launch_without_sources()

    def test_pre_staged_plugin_is_also_traversed(self):
        codec = self.library("codec", "int codec(void) { return 42; }",
                             str(self.source / "codec.dylib"))
        binary = self.executable(codec)
        helper = self.library("helper", "int helper(void) { return 0; }",
                              str(self.source / "helper.dylib"))
        plugin = self.library("plugin", "int helper(void); int plugin(void) { return helper(); }",
                              "@rpath/plugin.dylib", helper)
        shutil.copy2(plugin, self.out / plugin.name)
        self.package(binary)
        self.assertTrue((self.out / helper.name).is_file())
        self.launch_without_sources()

    def test_missing_import_fails_instead_of_silently_skipping(self):
        for token in ("@rpath/", "@loader_path/", "@executable_path/", "/missing/"):
            with self.subTest(token=token):
                codec = self.library("codec", "int codec(void) { return 42; }",
                                     token + "codec.dylib")
                binary = self.executable(codec)
                result = self.package(binary, success=False)
                self.assertIn("cannot resolve " + token + "codec.dylib", result.stderr)

    def test_dependency_copy_failure_is_fatal(self):
        codec = self.library("codec", "int codec(void) { return 42; }",
                             str(self.source / "codec.dylib"))
        binary = self.executable(codec)
        (self.out / codec.name).mkdir()
        result = self.package(binary, success=False)
        self.assertIn("macOS bundling failed:", result.stderr)


if __name__ == "__main__":
    unittest.main()
