#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using clock_type = std::chrono::steady_clock;
constexpr std::size_t default_chunk_size = 1U << 20;
constexpr std::uint64_t hash_offset = 14695981039346656037ULL;
constexpr std::uint64_t hash_prime = 1099511628211ULL;

constexpr std::uint64_t kib_size = 1024;
constexpr std::uint64_t mib_size = kib_size * kib_size;

struct options {
   std::uint64_t size_mib = 256;
   std::filesystem::path directory = "benchmark-data";
   std::size_t buffer_size = default_chunk_size;
   bool keep_files = false;
};

struct file_closer {
   void operator()(std::FILE *file) const noexcept {
      if (file != nullptr) {
         std::fclose(file);
      }
   }
};

struct measurement {
   std::string method;
   double write_seconds = 0.0;
   double read_seconds = 0.0;
   double write_mib_s = 0.0;
   double read_mib_s = 0.0;
   std::uint64_t expected_hash = 0;
   std::uint64_t actual_hash = 0;
   bool verified = false;
};

auto hash_bytes(std::uint64_t hash, std::span<const std::byte> bytes)
    -> std::uint64_t {
   for (const auto value : bytes) {
      hash ^= std::to_integer<unsigned char>(value);
      hash *= hash_prime;
   }
   return hash;
}

auto make_payload(std::size_t size) -> std::vector<std::byte> {
   std::vector<std::byte> payload(size);
   std::mt19937_64 engine{0x8f3d'9b71'24c6'a50dULL};
   std::uniform_int_distribution<unsigned int> distribution(0, 255);
   for (auto &value : payload) {
      value = static_cast<std::byte>(distribution(engine));
   }
   return payload;
}

auto hash_payload(std::span<const std::byte> payload) -> std::uint64_t {
   return hash_bytes(hash_offset, payload);
}

void print_usage() {
   std::cout << "Usage: file_io_benchmark [options]\n"
             << "  --size-mib N       Payload size in MiB (default: 256)\n"
             << "  --directory PATH   Directory for benchmark files (default: "
                "benchmark-data)\n"
             << "  --buffer-size N    Custom stdio buffer size in bytes "
                "(default: 1048576)\n"
             << "  --keep-files       Keep generated files after each method\n"
             << "  --help             Show this help\n";
}

auto parse_u64(std::string_view text, std::uint64_t &value) -> bool {
   if (text.empty())
      return false;
   std::uint64_t result = 0;
   for (const char c : text) {
      if (c < '0' || c > '9') {
         return false;
      }
      const auto digit = static_cast<std::uint64_t>(c - '0');
      if (result > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
         return false;
      }
      result = (result * 10) + digit;
   }
   value = result;
   return true;
}

auto parse_options(int argc, char **argv) -> options {
   options result;
   for (int i = 1; i < argc; ++i) {
      const std::string_view arg{argv[i]};
      if (arg == "--help") {
         print_usage();
         std::exit(EXIT_SUCCESS);
      }
      if (arg == "--keep-files") {
         result.keep_files = true;
      } else if (arg == "--size-mib" || arg == "--buffer-size" ||
                 arg == "--directory") {
         if (++i >= argc) {
            throw std::runtime_error("Missing value for " + std::string(arg));
         }
         const std::string_view value{argv[i]};
         if (arg == "--directory") {
            result.directory = std::filesystem::path(value);
         } else {
            std::uint64_t parsed = 0;
            if (!parse_u64(value, parsed) || parsed == 0) {
               throw std::runtime_error("Expected a positive integer for " +
                                        std::string(arg));
            }
            if (arg == "--size-mib") {
               result.size_mib = parsed;
            } else {
               if (parsed > std::numeric_limits<std::size_t>::max()) {
                  throw std::runtime_error("--buffer-size is too large");
               }
               result.buffer_size = static_cast<std::size_t>(parsed);
            }
         }
      } else {
         throw std::runtime_error("Unknown argument: " + std::string(arg));
      }
   }
   if (result.size_mib > std::numeric_limits<std::size_t>::max() / mib_size) {
      throw std::runtime_error("--size-mib is too large for this platform");
   }
   return result;
}

auto seconds_since(clock_type::time_point start) -> double {
   return std::chrono::duration<double>(clock_type::now() - start).count();
}

auto throughput(std::uint64_t bytes, double seconds) -> double {
   if (seconds <= 0.0) {
      return 0.0;
   }
   constexpr double mib = mib_size;
   return (static_cast<double>(bytes) / mib) / seconds;
}

void ensure_io(bool condition, std::string_view message) {
   if (!condition) {
      throw std::runtime_error(std::string(message));
   }
}

auto run_fstream(const std::filesystem::path &path,
                 std::span<const std::byte> payload, std::size_t chunk_size,
                 std::optional<std::size_t> buffer_size, bool keep_files)
    -> measurement {

   measurement result;
   result.method =
       std::format("fstream-{}", (buffer_size) ? "custom-buffer" : "default");
   result.expected_hash = hash_payload(payload);

   std::vector<std::byte> chunk_array(std::min(chunk_size, payload.size()));
   std::vector<char> buffer_array;
   if (buffer_size) {
      buffer_array.resize(*buffer_size);
   }

   {
      std::filebuf fbuf{};
      if (buffer_size) {
         fbuf.pubsetbuf(buffer_array.data(),
                        static_cast<std::streamsize>(*buffer_size));
      }
      fbuf.open(path, std::ios::out | std::ios::binary | std::ios::trunc);
      ensure_io(fbuf.is_open(), "fstream: could not open output file");
      std::ostream out(&fbuf);

      const auto start = clock_type::now();
      std::size_t offset = 0;
      while (offset < payload.size()) {
         const auto count =
             std::min(chunk_array.size(), payload.size() - offset);
         out.write(reinterpret_cast<const char *>(payload.data() + offset),
                   static_cast<std::streamsize>(count));
         ensure_io(static_cast<bool>(out), "fstream: write failed");
         offset += count;
      }
      out.flush();
      ensure_io(static_cast<bool>(out), "fstream: flush failed");
      result.write_seconds = seconds_since(start);
   }

   {
      std::filebuf fbuf{};
      if (buffer_size) {
         fbuf.pubsetbuf(buffer_array.data(),
                        static_cast<std::streamsize>(*buffer_size));
      }
      fbuf.open(path, std::ios::in | std::ios::binary);
      ensure_io(fbuf.is_open(), "fstream: could not open output file");
      std::istream in(&fbuf);

      auto start = clock_type::now();
      std::uint64_t hash = hash_offset;
      std::size_t total = 0;
      while (total < payload.size()) {
         const auto count =
             std::min(chunk_array.size(), payload.size() - total);
         in.read(reinterpret_cast<char *>(chunk_array.data()),
                 static_cast<std::streamsize>(count));
         const auto got = in.gcount();
         ensure_io(got > 0, "fstream: unexpected end of file or read failure");
         hash = hash_bytes(
             hash, std::span<const std::byte>(chunk_array.data(),
                                              static_cast<std::size_t>(got)));
         total += static_cast<std::size_t>(got);
      }
      result.read_seconds = seconds_since(start);
      result.actual_hash = hash;
      result.verified = total == payload.size() && hash == result.expected_hash;
   }

   if (!keep_files) {
      std::error_code ec;
      std::filesystem::remove(path, ec);
      ensure_io(!ec, "Could not remove temporary file: " + path.string());
   }
   result.write_mib_s = throughput(payload.size(), result.write_seconds);
   result.read_mib_s = throughput(payload.size(), result.read_seconds);
   return result;
}

auto run_stdio(const std::filesystem::path &path,
               std::span<const std::byte> payload, std::size_t chunk_size,
               std::optional<std::size_t> buffer_size, bool keep_files)
    -> measurement {

   measurement result;
   result.method =
       std::format("stdio-{}", (buffer_size) ? "custom-buffer" : "default");
   result.expected_hash = hash_payload(payload);

   std::vector<std::byte> chunk_array(std::min(chunk_size, payload.size()));
   std::vector<char> buffer_array;
   if (buffer_size) {
      buffer_array.resize(*buffer_size);
   }

   {
      std::FILE *file = std::fopen(path.string().c_str(), "wb");
      ensure_io(file != nullptr, "fopen: could not open output file");
      std::unique_ptr<std::FILE, file_closer> handle(file);
      if (buffer_size) {
         ensure_io(std::setvbuf(file, buffer_array.data(), _IOFBF,
                                buffer_array.size()) == 0,
                   "setvbuf failed for output file");
      }

      const auto start = clock_type::now();
      std::size_t offset = 0;
      while (offset < payload.size()) {
         const auto count =
             std::min(chunk_array.size(), payload.size() - offset);
         const auto written =
             std::fwrite(payload.data() + offset, 1, count, file);
         ensure_io(written == count, "fwrite failed");
         offset += written;
      }
      ensure_io(std::fflush(file) == 0, "fflush failed");
      result.write_seconds = seconds_since(start);
      ensure_io(std::fclose(handle.release()) == 0,
                "fclose failed after write");
   }

   {
      std::FILE *file = std::fopen(path.string().c_str(), "rb");
      ensure_io(file != nullptr, "fopen: could not open input file");
      std::unique_ptr<std::FILE, file_closer> handle(file);
      if (buffer_size) {
         ensure_io(std::setvbuf(file, buffer_array.data(), _IOFBF,
                                buffer_array.size()) == 0,
                   "setvbuf failed for input file");
      }

      const auto start = clock_type::now();
      std::uint64_t hash = hash_offset;
      std::size_t total = 0;
      while (total < payload.size()) {
         const auto count =
             std::min(chunk_array.size(), payload.size() - total);
         const auto got = std::fread(chunk_array.data(), 1, count, file);
         ensure_io(got > 0, "fread failed or reached unexpected end of file");
         hash = hash_bytes(hash,
                           std::span<const std::byte>(chunk_array.data(), got));
         total += got;
      }
      result.read_seconds = seconds_since(start);
      result.actual_hash = hash;
      result.verified = total == payload.size() && hash == result.expected_hash;
      ensure_io(std::fclose(handle.release()) == 0, "fclose failed after read");
   }

   if (!keep_files) {
      std::error_code ec;
      std::filesystem::remove(path, ec);
      ensure_io(!ec, "Could not remove temporary file: " + path.string());
   }
   result.write_mib_s = throughput(payload.size(), result.write_seconds);
   result.read_mib_s = throughput(payload.size(), result.read_seconds);
   return result;
}

void print_result(const measurement &result, std::uint64_t bytes) {
   std::cout << std::format("{:<22} "
                            "write={:>9.2f} MiB/s ({:0<9}s) "
                            "read={:>9.2f} MiB/s ({:0<9}s) "
                            "bytes={} "
                            "verified={}\n",
                            result.method, result.write_mib_s,
                            result.write_seconds, result.read_mib_s,
                            result.read_seconds, bytes,
                            (result.verified) ? "yes" : "NO");
}

} // namespace

auto main(int argc, char **argv) -> int {
   try {
      const auto config = parse_options(argc, argv);
      const auto bytes_u64 = config.size_mib * mib_size;
      const auto bytes = static_cast<std::size_t>(bytes_u64);
      std::filesystem::create_directories(config.directory);

      std::cout << std::format("Payload: {} MiB ({} bytes)\n"
                               "Chunk size: {} bytes\n"
                               "Buffer size: {} bytes\n\n",
                               config.size_mib, bytes_u64,
                               std::min(default_chunk_size, bytes),
                               config.buffer_size);
      const auto payload = make_payload(bytes);
      const auto expected_hash = hash_payload(payload);
      std::cout << std::format("Expected hash: {:#x}\n\n", expected_hash);

      std::vector<measurement> results;
      const auto chunk_size = std::min(default_chunk_size, payload.size());
      results.push_back(run_fstream(config.directory / "fstream-default.bin",
                                    payload, chunk_size, std::nullopt,
                                    config.keep_files));
      results.push_back(
          run_fstream(config.directory / "fstream-custom-buffer.bin", payload,
                      chunk_size, config.buffer_size, config.keep_files));
      results.push_back(run_stdio(config.directory / "stdio-default.bin",
                                  payload, chunk_size, std::nullopt,
                                  config.keep_files));
      results.push_back(run_stdio(config.directory / "stdio-custom-buffer.bin",
                                  payload, chunk_size, config.buffer_size,
                                  config.keep_files));

      std::cout << std::format("{:<22} write MiB/s / seconds | read MiB/s / "
                               "seconds | verification\n",
                               "Method");
      for (const auto &result : results) {
         print_result(result, bytes_u64);
      }
      const bool all_verified = std::ranges::all_of(
          results, [](const measurement &value) { return value.verified; });
      return all_verified ? EXIT_SUCCESS : EXIT_FAILURE;
   } catch (const std::exception &e) {
      std::cerr << "error: " << e.what() << '\n';
      return EXIT_FAILURE;
   }
}
