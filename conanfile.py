from conan import ConanFile
from conan.tools.cmake import CMakeDeps, CMakeToolchain
from conan.tools.env import VirtualRunEnv


class AjbDependencies(ConanFile):
    required_conan_version = ">=2.28 <3"
    settings = "os", "arch", "compiler", "build_type"
    options = {
        "with_tui": [True, False],
        "with_tests": [True, False],
        "with_media": [True, False],
    }
    default_options = {
        "with_tui": True,
        "with_tests": True,
        "with_media": True,
    }

    def configure(self):
        if not self.options.with_media:
            self.options.with_tui = False

    def requirements(self):
        if self.options.with_tui:
            self.requires("ftxui/7.0.3")

    def build_requirements(self):
        if self.options.with_tests:
            self.test_requires("gtest/1.18.0")

    def layout(self):
        self.folders.generators = "generators"

    def generate(self):
        CMakeDeps(self).generate()
        toolchain = CMakeToolchain(self)
        toolchain.user_presets_path = False
        toolchain.generate()
        VirtualRunEnv(self).generate()
