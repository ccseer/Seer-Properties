#pragma once

#include <cstdint>
#include <string>
#include <vector>

// One "largest object" row produced by the opt-in contribution analysis.
struct PdbContribution {
    std::string name;      // module name as stored in the PDB
    std::string sizeText;  // human-readable aggregated byte count
};

// Options for one query. They mirror the helper's CLI options.
struct PdbQueryOptions {
    // Internal deadline convention shared by the property packages: a positive
    // value is the budget in milliseconds, 0 means the budget is already
    // exhausted, and hasDeadline == false disables the deadline entirely (the
    // host timeout governs). The helper turns a 0 budget into an immediate
    // abandonment without invoking the query at all.
    bool hasDeadline = false;
    uint32_t deadlineMs = 0;
    bool wantTop = false;
    uint32_t topN = 0;
};

// Result of one PDB query. Every field is presentation-ready text; an empty
// field means the information was not available and the row is omitted.
// Unavailable information is never reworded into a positive claim, so a
// missing count does not become 0.
struct PdbQueryResult {
    // One of: Valid, FastLink, NotAPdb, CorruptPdb, ReadError, QueryError.
    std::string status;
    std::string reason;  // concise explanation for a non-successful status
    std::string formatVersion;
    std::string built;
    std::string age;
    std::string guid;
    std::string machine;
    std::string linkType;  // "Full" or "/DEBUG:FASTLINK"
    std::string streamCount;
    std::string modules;
    std::string publicSymbols;
    std::string globalSymbols;
    std::string typeRecords;
    std::string inlineRecords;
    std::string sourceFiles;
    std::string contributions;
    std::string lineInfo;  // "Yes" or "No"
    std::string fileSize;
    // FASTLINK explanation or partial-result note for an abandoned deadline.
    std::string note;
    std::vector<PdbContribution> topContributions;
};

using PdbQueryFn = PdbQueryResult (*)(const std::wstring &filePath,
                                      const PdbQueryOptions &options);

// Maps and inspects one PDB file. All expected domain outcomes (missing
// file, locked file, not a PDB, corrupt PDB, FASTLINK) are reported as a
// populated result for the caller to publish. An exhausted deadline keeps
// every row gathered so far and marks the result partial instead of
// discarding it.
PdbQueryResult queryPdbFile(const std::wstring &filePath,
                            const PdbQueryOptions &options);
