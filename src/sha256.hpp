// SHA-256 (FIPS 180-4), for release checksums. libcurl has no public hash API, and the Windows
// build links curl with Schannel, so there is no OpenSSL to borrow one from.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace sha256 {

class Hasher {
public:
    Hasher();
    void update(const void* data, std::size_t size);
    // Lowercase hex. The hasher is spent afterwards.
    std::string hex();

private:
    void block(const std::uint8_t* p);

    std::array<std::uint32_t, 8> h_;
    std::array<std::uint8_t, 64> buf_{};
    std::size_t used_ = 0;
    std::uint64_t total_ = 0;
};

std::string of(const std::string& data);
// Throws std::runtime_error when the file can't be read.
std::string of_file(const std::filesystem::path& file);

}  // namespace sha256
