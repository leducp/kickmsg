from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, cmake_layout
from conan.tools.files import copy

import os


class KickMsgConan(ConanFile):
    """Conan package recipe for kickmsg.

    For consumers: add kickmsg as a requirement in your conanfile:
        self.requires("kickmsg/<version>")
    """
    name = "kickmsg"
    license = "CeCILL-C"
    url = "https://github.com/leducp/kickmsg"
    description = "Lock-free shared-memory MPMC messaging library"
    topics = ("ipc", "shared-memory", "lock-free", "pub-sub", "zero-copy")

    settings = "os", "compiler", "build_type", "arch"

    exports_sources = (
        "CMakeLists.txt",
        "include/*",
        "src/*",
        "os/*",
        "LICENSE",
    )

    def layout(self):
        cmake_layout(self)

    def generate(self):
        tc = CMakeToolchain(self)
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        copy(self, "LICENSE", src=self.source_folder,
             dst=os.path.join(self.package_folder, "licenses"))
        copy(self, "*.h", src=os.path.join(self.source_folder, "include"),
             dst=os.path.join(self.package_folder, "include"))
        copy(self, "*.a", src=self.build_folder,
             dst=os.path.join(self.package_folder, "lib"), keep_path=False)
        copy(self, "*.lib", src=self.build_folder,
             dst=os.path.join(self.package_folder, "lib"), keep_path=False)

    def package_info(self):
        self.cpp_info.libs = ["kickmsg"]
        if self.settings.os == "Linux":
            self.cpp_info.system_libs = ["rt", "pthread"]
