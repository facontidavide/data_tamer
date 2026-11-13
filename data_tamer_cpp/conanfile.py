# -*- coding: utf-8 -*-
from conan import ConanFile
from conan.tools.cmake import CMakeToolchain, CMake, cmake_layout, CMakeDeps
from conan.tools.files import rmdir
import os


class DataTamerConan(ConanFile):
    name = "data_tamer"
    version = "2.1.0"
    package_type = "library"
    url = "https://github.com/facontidavide/data_tamer"
    license = "MIT"
    description = "library for data profiling"
    settings = "os", "compiler", "build_type", "arch"
    options = {
        "shared": [True, False],
        "fPIC": [True, False],
        "tests": [True, False],
        "examples": [True, False],
        "benchmarks": [True, False]
    }
    default_options = {
        "shared": False,
        "fPIC": True,
        "tests": True,
        "examples": True,
        "benchmarks": False
    }
    # MCAP comes from the mcap package (DATA_TAMER_USE_SYSTEM_MCAP), not from FetchContent.
    exports_sources = (
        "cmake/*",
        "include/*",
        "src/*",
        "examples/*",
        "benchmarks/*",
        "tests/*",
        "CMakeLists.txt"
    )

    def requirements(self):

        # mcap is header-only: libdata_tamer compiles its implementation, and
        # data_tamer/sinks/mcap_encoding.hpp includes its headers.
        self.requires("mcap/2.1.3", transitive_headers=True)
        if self.options.tests:
            self.requires("gtest/1.14.0")
        if self.options.benchmarks:
            self.requires("benchmark/1.8.3")

    def build_requirements(self):
        self.tool_requires("cmake/3.26.4")

    def configure(self):
        if self.options.shared:
            self.options.rm_safe("fPIC")

    def layout(self):
        cmake_layout(self)

    def generate(self):
        tc = CMakeToolchain(self)
        tc.generate()

        deps = CMakeDeps(self)
        deps.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure(
            {
                "DATA_TAMER_BUILD_ROS": False,
                "DATA_TAMER_USE_SYSTEM_MCAP": True,
                "DATA_TAMER_BUILD_TESTS": self.options.tests,
                "DATA_TAMER_BUILD_EXAMPLES": self.options.examples,
                "DATA_TAMER_BUILD_BENCHMARKS": self.options.benchmarks
            }
        )
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()
        # CMakeDeps generates the CMake package of consumers.
        rmdir(self, os.path.join(self.package_folder, "lib", "cmake"))

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "data_tamer")
        self.cpp_info.set_property("cmake_target_name", "data_tamer::data_tamer")
        self.cpp_info.libs = ["data_tamer"]
        self.cpp_info.defines = [f'DATA_TAMER_VERSION="{self.version}"']
        if self.settings.os in ["Linux", "FreeBSD"]:
            self.cpp_info.system_libs = ["pthread"]
