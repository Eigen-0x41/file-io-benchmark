# File I/O Benchmark

Cross-platform file I/O benchmark for Ubuntu (`clang++`, `g++`) and Windows (`clang-cl`, MSVC `cl`), built from the same C++ source with CMake.

## Methods

- `fstream`: `std::fstream::write` / `std::fstream::read`
- `stdio-default`: `fwrite` / `fread` using the C runtime's default buffering
- `stdio-custom-buffer`: `fwrite` / `fread` with a user-provided stdio buffer via `setvbuf`

Each method writes the same generated payload to a file, then reads it back and verifies a 64-bit hash. The payload is generated from a deterministic PRNG, not a repeated zero-filled pattern. Each method gets its own file to avoid accidental dependence on data left by another method.

The benchmark reports write and read wall-clock time, MiB/s, bytes processed, and verification status. It is a basic benchmark, not a guarantee of physical-device throughput: operating-system page cache, filesystem cache, storage type, antivirus, and CI host contention can affect results.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

On Windows, use a CMake generator/toolchain appropriate for the compiler being tested. For example, `-G Ninja -DCMAKE_CXX_COMPILER=clang++` on Ubuntu, or a Visual Studio developer environment for MSVC.

## Run

Default payload size is 256 MiB. Set size in MiB explicitly:

```sh
./build/file_io_benchmark --size-mib 1024 --directory benchmark-data --buffer-size 1048576
```

Windows multi-config generators may place the executable under `build/Release/`.

Arguments:

- `--size-mib N`: payload size in MiB (default `256`)
- `--directory PATH`: directory for temporary benchmark files (default `benchmark-data`)
- `--buffer-size N`: custom stdio buffer size in bytes (default `1048576`)
- `--keep-files`: retain generated benchmark files after the run
- `--help`: show usage

The requested size applies to each method. With three methods, peak disk use is approximately three times the payload size while files are present, though each file is removed after its method completes unless `--keep-files` is specified.

## GitHub Actions

`.github/workflows/benchmark.yml` runs Release builds for:

- Ubuntu + GCC
- Ubuntu + Clang
- Windows + MSVC
- Windows + Clang (`clang-cl`)

The workflow uses a 256 MiB payload by default. Change `BENCHMARK_SIZE_MIB` in the workflow to adjust it. Results are printed to the job log; they are not intended to be directly compared across different hosted runner instances as if they were identical hardware.

## Interpretation cautions

- The timers start after opening the file and stop before closing it. Write timing includes `flush`/`fflush`; read timing includes hashing and verification work. Treat the results as operation-loop throughput, not pure storage-device throughput.
- A normal `fread`/`fwrite` stream is already buffered by the C runtime. `stdio-custom-buffer` measures a larger explicit stdio buffer, not unbuffered I/O.
- Read results may be served partly or entirely from the OS page cache. This implementation does not attempt privileged cache eviction or direct I/O because those would reduce portability and reproducibility.
- Repeat runs and compare medians. Do not infer compiler-only differences from a single run: hosted runners and their storage/cache state vary.
