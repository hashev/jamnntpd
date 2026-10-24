# C++23 build

CMake builds the production `jamnntpd` server, `makechs`, and test probes.
C++23 is required without extensions, with a configure-time `std::expected` probe.
All own targets treat compiler warnings as errors.

The dependency manifest pins vcpkg baseline
`b42e8ed051d17a7f95b9d2a3140c1f9296423712`: ICU 78.3, spdlog 1.17.0,
Catch2 3.16.1. Python >=3.10 runs independent fixture and executable tests.
Clone vcpkg at that revision, bootstrap it, then:

```
cmake --preset debug -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build --preset debug --parallel 4
ctest --preset debug
```

Installed CMake packages can also be selected with `CMAKE_PREFIX_PATH`.
Validated local compilers: LLVM 23.1.3, AppleClang 21.0.0, Linux GCC 14.2.0.
Native CI includes Linux GCC/Clang, macOS AppleClang and Windows MSVC.
CI results, sanitizer acceptance and remaining gates are recorded separately in
[verification.md](verification.md); defining a job does not establish a pass.

Presets: debug, release, asan-ubsan, tsan, coverage. Use separate build directories
for ASan/UBSan and TSan. LLVM coverage must run all CTest groups with
`LLVM_PROFILE_FILE=/absolute/build/path/current-%p-%m.profraw`, after deleting old
current profiles. Then run:

```
python3 tools/check_coverage.py --build build/coverage
```

The gate merges all five test/production executables, checks every production
C++ translation unit is represented, includes own headers and error paths, and
requires 90% lines and 85% branches. It does not claim coverage of inactive native
platform branches. Native Windows evidence is an additional acceptance requirement.

Static analysis: LLVM/clang-tidy 23.1.3,
`cmake --build build/debug --target clang-tidy`. The profile enables all agreed
families; reviewed ABI exceptions are local and documented in
[core-guidelines.md](core-guidelines.md). Formatting: `clang-format --dry-run
--Werror` over production and C++ test sources.
