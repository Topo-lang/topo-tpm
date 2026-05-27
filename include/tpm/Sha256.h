#ifndef TPM_SHA256_H
#define TPM_SHA256_H

#include <string>

namespace tpm {

/// Compute the lowercase-hex SHA-256 digest of a byte string.
std::string sha256Hex(const std::string& data);

} // namespace tpm

#endif // TPM_SHA256_H
