from conan import ConanFile
from conan.errors import ConanInvalidConfiguration
from conan.tools.build import check_min_cppstd
from conan.tools.cmake import CMakeToolchain, CMakeDeps, CMake, cmake_layout
from conan.tools.files import copy
import os

required_conan_version = ">=2.10.0"


class KickmsgRecipe(ConanFile):
    name = "kickmsg"
    license = "CeCILL-C"
    url = "https://github.com/leducp/kickmsg"
    description = "Lock-free shared-memory MPMC messaging library"
    topics = ("ipc", "shared-memory", "lock-free", "pub-sub", "zero-copy")
    package_type = "library"
    settings = "os", "compiler", "build_type", "arch"
    options = {"shared": [True, False], "fPIC": [True, False]}
    default_options = {"shared": False, "fPIC": True}

    def export_sources(self):
        root = os.path.abspath(os.path.join(self.recipe_folder, "../.."))
        copy(self, "CMakeLists.txt", src=root, dst=self.export_sources_folder)
        copy(self, "include/*", src=root, dst=self.export_sources_folder)
        copy(self, "src/*", src=root, dst=self.export_sources_folder)
        copy(self, "cmake/*", src=root, dst=self.export_sources_folder)
        copy(self, "LICENSE", src=root, dst=self.export_sources_folder)

    def configure(self):
        if self.options.get_safe("shared"):
            self.options.rm_safe("fPIC")

    def layout(self):
        cmake_layout(self)

    def validate(self):
        if self.settings.compiler.get_safe("cppstd"):
            check_min_cppstd(self, 17)

    def generate(self):
        tc = CMakeToolchain(self)
        tc.cache_variables["BUILD_UNIT_TESTS"] = "OFF"
        tc.cache_variables["BUILD_BENCHMARKS"] = "OFF"
        tc.cache_variables["BUILD_EXAMPLES"] = "OFF"
        tc.cache_variables["ENABLE_TSAN"] = "OFF"
        tc.generate()
        deps = CMakeDeps(self)
        deps.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        copy(self, "LICENSE", src=self.source_folder,
             dst=os.path.join(self.package_folder, "licenses"))
        copy(self, "*.h", src=os.path.join(self.source_folder, "include"),
             dst=os.path.join(self.package_folder, "include"))
        # version.h is generated into the build tree (cmake/version.cmake),
        # not committed under include/ -- package it from there.
        copy(self, "version.h", src=os.path.join(self.build_folder, "generated"),
             dst=os.path.join(self.package_folder, "include"))
        copy(self, "*.a", src=self.build_folder,
             dst=os.path.join(self.package_folder, "lib"), keep_path=False)
        copy(self, "*.lib", src=self.build_folder,
             dst=os.path.join(self.package_folder, "lib"), keep_path=False)

    def package_info(self):
        self.cpp_info.libs = ["kickmsg"]
        if self.settings.os == "Linux":
            self.cpp_info.system_libs = ["rt", "pthread"]
