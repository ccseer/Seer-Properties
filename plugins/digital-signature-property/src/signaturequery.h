#pragma once

#include <string>

// Result of one Authenticode trust query.
//
// The record deliberately separates the cryptographic/trust outcome from
// certificate metadata, and records exactly which signature and verification
// policy were examined. An unset optional field means "not available", which is
// never reworded into a positive or negative claim.
struct SignatureQueryResult {
    // One of: Valid, Unsigned, HashMismatch, UntrustedChain, Revoked,
    // RevocationUnavailable, UnsupportedOrMalformed, ReadError, QueryError.
    std::string status;
    // One of: embedded, catalog, none.
    std::string source;
    // Result of the bounded local catalog-signature lookup. "no embedded
    // signature" is never reported as "unsigned" before this lookup has been
    // considered.
    std::string catalogStatus;
    // Which policy/scope was examined, including the revocation limitation.
    std::string scope;
    // How many signers the inspected signature contains, and which of them the
    // reported metadata was taken from. Counts are never generalized silently.
    std::string signersExamined;
    // Embedded signature container kind, e.g. "Authenticode PKCS#7".
    std::string signatureKind;
    std::string publisher;
    std::string issuer;
    std::string thumbprintAlgorithm;
    std::string thumbprint;
    std::string signingDigestAlgorithm;
    std::string validFrom;
    std::string validTo;
    bool hasTimestamp = false;
    std::string timestampStatus;
    // Concise reason/code for a non-successful verification.
    std::string reason;
};

// Runs the real Windows trust query. Returns a populated result and never
// throws. The inspected file is opened read-only and is never loaded or
// executed. Supported baseline: Windows 7 SP1 or later; validated on Windows 10
// and Windows 11 x64.
SignatureQueryResult queryFileSignature(const std::wstring &filePath);
