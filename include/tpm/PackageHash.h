#ifndef TPM_PACKAGE_HASH_H
#define TPM_PACKAGE_HASH_H

#include <string>

namespace tpm {

/// Compute the SHA-256 content hash over a package directory's normalized
/// file set.
///
/// Normalization: every regular file under `packageDir` is enumerated with a
/// path relative to the package root, using '/' separators; the `.git/`
/// directory and `tpm.lock` are excluded (the lock is generated downstream
/// and a checkout's `.git` is not package content). Entries are sorted by
/// relative path, and each contributes "<relpath>\0<filebytes>\0" to the
/// hashed stream. The result is deterministic across platforms.
///
/// Returns the lowercase-hex digest, or an empty string when `packageDir`
/// does not exist.
std::string computePackageContentHash(const std::string& packageDir);

} // namespace tpm

#endif // TPM_PACKAGE_HASH_H
