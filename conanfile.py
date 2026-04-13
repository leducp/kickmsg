from conan import ConanFile


class KickmsgDev(ConanFile):
    """Local development conanfile for kickmsg."""
    settings = "os", "compiler", "build_type", "arch"

    options = {
        "unit_tests": [True, False],
        "benchmarks": [True, False],
    }
    default_options = {
        "unit_tests": False,
        "benchmarks": False,
    }

    generators = "CMakeDeps"

    def requirements(self):
        if self.options.unit_tests:
            self.requires("gtest/1.15.0")

        if self.options.benchmarks:
            self.requires("benchmark/1.9.1")

    def configure(self):
        if self.options.unit_tests:
            self.options["gtest"].build_gmock = True
