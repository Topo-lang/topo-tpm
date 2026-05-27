#include "tpm/Sha256.h"

#include "picosha2.h"

namespace tpm {

std::string sha256Hex(const std::string& data) {
    return picosha2::hash256_hex_string(data);
}

} // namespace tpm
