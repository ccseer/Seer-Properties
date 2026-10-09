#include "pdbquery.h"

#include "pdbformat.h"

#include "PDB.h"
#include "PDB_DBIStream.h"
#include "PDB_IPIStream.h"
#include "PDB_InfoStream.h"
#include "PDB_RawFile.h"
#include "PDB_TPIStream.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

namespace {

using pdbformat::formatBytes;

// A read-only file mapping that stays alive for the whole query. The file is
// opened with read and write sharing so a debugger or linker can hold it open
// while it is inspected.
class MappedFile {
public:
    MappedFile() = default;
    ~MappedFile()
    {
        if (m_view != nullptr) {
            UnmapViewOfFile(m_view);
        }
        if (m_mapping != nullptr) {
            CloseHandle(m_mapping);
        }
        if (m_file != INVALID_HANDLE_VALUE) {
            CloseHandle(m_file);
        }
    }

    MappedFile(const MappedFile &) = delete;
    MappedFile &operator=(const MappedFile &) = delete;

    bool open(const std::wstring &path, std::string *error)
    {
        m_file = CreateFileW(
            path.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (m_file == INVALID_HANDLE_VALUE) {
            *error = describeCreateError(GetLastError());
            return false;
        }
        LARGE_INTEGER size {};
        if (!GetFileSizeEx(m_file, &size) || size.QuadPart < 0) {
            *error = "the file size could not be determined";
            return false;
        }
        if (size.QuadPart == 0) {
            *error = "the file is empty";
            return false;
        }
        m_size = static_cast<uint64_t>(size.QuadPart);
        m_mapping = CreateFileMappingW(m_file, nullptr, PAGE_READONLY, 0, 0,
                                       nullptr);
        if (m_mapping == nullptr) {
            *error = "the file could not be mapped (Win32 error "
                     + std::to_string(GetLastError()) + ")";
            return false;
        }
        m_view = MapViewOfFile(m_mapping, FILE_MAP_READ, 0, 0, 0);
        if (m_view == nullptr) {
            *error = "the file could not be mapped into memory (Win32 error "
                     + std::to_string(GetLastError()) + ")";
            return false;
        }
        return true;
    }

    const void *data() const
    {
        return m_view;
    }

    uint64_t size() const
    {
        return m_size;
    }

private:
    static std::string describeCreateError(DWORD error)
    {
        switch (error) {
            case ERROR_FILE_NOT_FOUND:
            case ERROR_PATH_NOT_FOUND:
                return "the file does not exist";
            case ERROR_ACCESS_DENIED:
                return "the file cannot be opened (access denied)";
            case ERROR_SHARING_VIOLATION:
            case ERROR_LOCK_VIOLATION:
                return "the file is locked by another process";
            default:
                return "the file cannot be opened (Win32 error "
                       + std::to_string(error) + ")";
        }
    }

    HANDLE m_file = INVALID_HANDLE_VALUE;
    HANDLE m_mapping = nullptr;
    const void *m_view = nullptr;
    uint64_t m_size = 0;
};

// Deadline checkpoints shared by the property packages. The budget is checked
// between the bounded steps of the query; an exhausted budget keeps every row
// gathered so far and marks the result partial instead of discarding it.
class Deadline {
public:
    Deadline(bool hasDeadline, uint32_t budgetMs)
        : m_enabled(hasDeadline),
          m_budgetMs(budgetMs),
          m_start(std::chrono::steady_clock::now())
    {
    }

    // Returns false once the budget is used up. The caller then skips the
    // remaining work; the checkpoint name lands in the published note.
    bool running(const char *checkpoint)
    {
        if (m_abandoned) {
            return false;
        }
        if (!m_enabled) {
            return true;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - m_start);
        if (elapsed.count()
            < static_cast<std::chrono::milliseconds::rep>(m_budgetMs)) {
            return true;
        }
        m_abandoned = true;
        m_checkpoint = checkpoint;
        return false;
    }

    std::string note() const
    {
        if (!m_abandoned) {
            return {};
        }
        return std::string("Partial result: the internal deadline (")
               + std::to_string(m_budgetMs)
               + " ms) was reached at the \"" + m_checkpoint
               + "\" step; the remaining fields were skipped.";
    }

private:
    bool m_enabled = false;
    bool m_abandoned = false;
    uint32_t m_budgetMs = 0;
    const char *m_checkpoint = "";
    std::chrono::steady_clock::time_point m_start;
};

// Aggregates section contributions per OBJ module and reports the top N by
// bytes. Ties keep the lower module index so the output is deterministic.
std::vector<PdbContribution> topContributionRows(
    const PDB::ArrayView<PDB::DBI::SectionContribution> &rows,
    const PDB::ModuleInfoStream &moduleInfo, std::size_t moduleCount,
    uint32_t topN)
{
    std::vector<uint64_t> sizes(moduleCount, 0);
    for (const PDB::DBI::SectionContribution &row : rows) {
        if (row.moduleIndex < moduleCount) {
            sizes[row.moduleIndex] += row.size;
        }
    }

    std::vector<uint32_t> order(static_cast<std::size_t>(moduleCount));
    for (std::size_t i = 0; i < moduleCount; ++i) {
        order[i] = static_cast<uint32_t>(i);
    }
    const std::size_t wanted
        = std::min<std::size_t>(topN, moduleCount);
    const auto bySizeDesc = [&sizes](uint32_t lhs, uint32_t rhs) {
        if (sizes[lhs] != sizes[rhs]) {
            return sizes[lhs] > sizes[rhs];
        }
        return lhs < rhs;
    };
    std::partial_sort(order.begin(), order.begin()
                                             + static_cast<std::ptrdiff_t>(wanted),
                      order.end(), bySizeDesc);

    std::vector<PdbContribution> result;
    result.reserve(wanted);
    for (std::size_t i = 0; i < wanted; ++i) {
        const uint32_t moduleIndex = order[i];
        if (sizes[moduleIndex] == 0) {
            break;  // fewer modules hold contribution data than requested
        }
        const PDB::ModuleInfoStream::Module &module
            = moduleInfo.GetModule(moduleIndex);
        PdbContribution row;
        // Module names are length-delimited and not guaranteed to be
        // null-terminated, so the view length bounds the copy.
        const char *rawName = module.GetName().Decay();
        const std::size_t rawLength = module.GetName().GetLength();
        if (rawName != nullptr && rawLength > 0) {
            row.name = pdbformat::sanitizeUtf8(std::string(rawName, rawLength));
            const std::size_t terminator = row.name.find('\0');
            if (terminator != std::string::npos) {
                row.name.resize(terminator);
            }
        }
        if (row.name.empty()) {
            row.name = "(no name)";
        }
        row.sizeText = formatBytes(sizes[moduleIndex]);
        result.push_back(std::move(row));
    }
    return result;
}

// The full query. Every step is bounded, and each optional step checks the
// deadline first so an exhausted budget skips the rest without losing the
// rows already collected. The result travels through an out reference so the
// SEH-guarded frame below never materializes a C++ object that would need
// unwinding.
void runQuery(const std::wstring &path, const PdbQueryOptions &options,
              PdbQueryResult &result)
{
    Deadline deadline(options.hasDeadline, options.deadlineMs);

    MappedFile mapped;
    {
        std::string openError;
        if (!mapped.open(path, &openError)) {
            if (openError == "the file is empty") {
                result.status = "NotAPdb";
                result.reason = openError;
                result.fileSize = "0 B";
                return;
            }
            result.status = "ReadError";
            result.reason = openError;
            return;
        }
    }
    result.fileSize = formatBytes(mapped.size());

    // raw_pdb collapses "too small for the super block" and "truncated" into
    // one error code, but they read differently: a file that cannot even hold
    // the magic carries no evidence of ever being a PDB, so it is reported as
    // NotAPdb instead of a corrupt PDB.
    if (mapped.size() < sizeof(PDB::SuperBlock)) {
        result.status = "NotAPdb";
        result.reason = "the file is too small to carry the PDB magic";
        return;
    }

    const PDB::ErrorCode validation
        = PDB::ValidateFile(mapped.data(),
                            static_cast<std::size_t>(mapped.size()));
    switch (validation) {
        case PDB::ErrorCode::Success:
            break;
        case PDB::ErrorCode::InvalidSuperBlock:
            result.status = "NotAPdb";
            result.reason = "the file does not carry the PDB magic";
            return;
        case PDB::ErrorCode::InvalidDataSize:
            result.status = "CorruptPdb";
            result.reason
                = "the file is truncated: it is smaller than its MSF header "
                  "claims";
            return;
        case PDB::ErrorCode::InvalidFreeBlockMap:
            result.status = "CorruptPdb";
            result.reason = "the MSF free block map index is invalid";
            return;
        default:
            result.status = "CorruptPdb";
            result.reason = "the MSF structure failed validation";
            return;
    }

    const auto *superBlock
        = static_cast<const PDB::SuperBlock *>(mapped.data());
    if (superBlock->blockSize == 0) {
        result.status = "CorruptPdb";
        result.reason = "the MSF block size is invalid";
        return;
    }

    if (!deadline.running("MSF directory")) {
        result.status = "QueryError";
        result.reason
            = "the query was abandoned because the internal deadline ("
              + std::to_string(options.deadlineMs)
              + " ms) was reached before the stream directory could be read";
        return;
    }

    const PDB::RawFile raw = PDB::CreateRawFile(mapped.data());
    result.streamCount = std::to_string(raw.GetStreamCount());

    // The info stream always lives at index 1. raw_pdb asserts instead of
    // checking in release builds, so the preconditions are verified here.
    if (raw.GetStreamCount() < 2
        || raw.GetStreamSize(1) < sizeof(PDB::Header)) {
        result.status = "CorruptPdb";
        result.reason = "the stream directory has no usable info stream";
        return;
    }

    if (!deadline.running("info stream")) {
        result.status = "QueryError";
        result.reason
            = "the query was abandoned because the internal deadline ("
              + std::to_string(options.deadlineMs)
              + " ms) was reached before the info stream could be read";
        return;
    }

    const PDB::InfoStream info(raw);
    const PDB::Header *header = info.GetHeader();
    result.formatVersion = pdbformat::formatFormatVersion(
        static_cast<uint32_t>(header->version));
    result.built = pdbformat::formatUnixTimeUtc(header->signature);
    result.age = std::to_string(header->age);
    result.guid = pdbformat::formatGuid(header->guid.Data1, header->guid.Data2,
                                        header->guid.Data3,
                                        header->guid.Data4);

    if (info.UsesDebugFastLink()) {
        result.status = "FastLink";
        result.linkType = "/DEBUG:FASTLINK";
        result.note
            = "The executable was linked with /DEBUG:FASTLINK: detailed "
              "private records are stored in the linked OBJ files, not in "
              "this PDB.";
    }
    else {
        result.status = "Valid";
        result.linkType = "Full";
    }

    // DBI-dependent rows. The DBI stream lives at index 3; without it the
    // machine and the per-module counts stay unknown instead of becoming 0.
    if (raw.GetStreamCount() >= 4
        && PDB::HasValidDBIStream(raw) == PDB::ErrorCode::Success
        && deadline.running("DBI stream")) {
        const PDB::DBIStream dbi = PDB::CreateDBIStream(raw);
        result.machine = pdbformat::formatMachine(dbi.GetHeader().machine);

        const PDB::ModuleInfoStream moduleInfo
            = dbi.CreateModuleInfoStream(raw);
        const std::size_t moduleCount
            = moduleInfo.GetModules().GetLength();
        result.modules = std::to_string(moduleCount);

        bool anyLines = false;
        for (const PDB::ModuleInfoStream::Module &module :
             moduleInfo.GetModules()) {
            if (module.HasLineStream()) {
                anyLines = true;
                break;
            }
        }
        result.lineInfo = anyLines ? "Yes" : "No";

        if (dbi.HasValidPublicSymbolStream(raw)
            == PDB::ErrorCode::Success) {
            result.publicSymbols = std::to_string(
                dbi.CreatePublicSymbolStream(raw).GetRecords().GetLength());
        }
        if (dbi.HasValidGlobalSymbolStream(raw)
            == PDB::ErrorCode::Success) {
            result.globalSymbols = std::to_string(
                dbi.CreateGlobalSymbolStream(raw).GetRecords().GetLength());
        }

        if (deadline.running("section contributions")
            && dbi.HasValidSectionContributionStream(raw)
                   == PDB::ErrorCode::Success) {
            const PDB::SectionContributionStream contributions
                = dbi.CreateSectionContributionStream(raw);
            const PDB::ArrayView<PDB::DBI::SectionContribution> rows
                = contributions.GetContributions();
            result.contributions = std::to_string(rows.GetLength());
            if (options.wantTop && options.topN > 0 && rows.GetLength() > 0) {
                result.topContributions = topContributionRows(
                    rows, moduleInfo, moduleCount, options.topN);
            }
        }

        if (deadline.running("source files")
            && dbi.GetHeader().sourceInfoSize >= sizeof(uint16_t) * 2) {
            const PDB::SourceFileStream sources
                = dbi.CreateSourceFileStream(raw);
            uint64_t files = 0;
            for (std::size_t i = 0; i < sources.GetModuleCount(); ++i) {
                files += sources.GetModuleFilenameOffsets(i).GetLength();
            }
            result.sourceFiles = std::to_string(files);
        }
    }

    // The type streams live at fixed indices 2 (TPI) and 4 (IPI) and only
    // their headers are touched, so the counts stay cheap even for huge PDBs.
    if (deadline.running("type streams") && raw.GetStreamCount() >= 3
        && PDB::HasValidTPIStream(raw) == PDB::ErrorCode::Success) {
        result.typeRecords = std::to_string(
            PDB::CreateTPIStream(raw).GetTypeRecordCount());
    }
    if (info.HasIPIStream() && raw.GetStreamCount() >= 5
        && PDB::HasValidIPIStream(raw) == PDB::ErrorCode::Success) {
        result.inlineRecords = std::to_string(
            PDB::CreateIPIStream(raw).GetTypeRecords().GetLength());
    }

    const std::string deadlineNote = deadline.note();
    if (!deadlineNote.empty()) {
        result.note = result.note.empty()
                          ? deadlineNote
                          : result.note + " " + deadlineNote;
    }
}

struct QueryContext {
    const std::wstring *path = nullptr;
    const PdbQueryOptions *options = nullptr;
    PdbQueryResult result;
    bool faulted = false;
    unsigned faultCode = 0;
};

// raw_pdb reports malformed stream data through PDB_ASSERT, which compiles to
// a no-op in release builds, so a crafted file could cause an access violation
// inside the parser. The query therefore runs behind a structured-exception
// guard: a fault is published as a QueryError result instead of crashing the
// helper, which would make the host report a plugin failure and wipe the
// output directory. The guarded frame must stay free of C++ objects with
// destructors, so the state travels through a plain struct pointer.
BOOL runGuarded(QueryContext *context)
{
    __try {
        runQuery(*context->path, *context->options, context->result);
        return TRUE;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        context->faulted = true;
        context->faultCode = static_cast<unsigned>(GetExceptionCode());
        return FALSE;
    }
}

}  // namespace

PdbQueryResult queryPdbFile(const std::wstring &filePath,
                            const PdbQueryOptions &options)
{
    QueryContext context;
    context.path = &filePath;
    context.options = &options;
    if (runGuarded(&context)) {
        return std::move(context.result);
    }

    PdbQueryResult result;
    result.status = "QueryError";
    result.reason
        = "the PDB parser hit an internal fault (exception "
          + std::to_string(context.faultCode)
          + "); the file is not reported as parsed";
    return result;
}
