// sevenzip_wrap.h — thin C++ wrapper around 7z.dll (Format7zF COM bundle)
//
// Loads 7z.dll at runtime, instantiates 7-Zip's IInArchive for 7z / RAR / RAR5
// and exposes a small, encoding-safe API used by SevenZipEngine.
// The 7-Zip source is public domain (RAR decoder carries the unRAR restriction:
// decompression only, never compression). We only read archives here.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace yasuo {
namespace sevenzip {

struct SzEntry {
    std::wstring path;     // entry name (may contain sub-paths)
    bool isDir = false;
    bool encrypted = false;
    uint64_t size = 0;
    uint64_t packedSize = 0;
    uint64_t mtime = 0;    // Windows FILETIME as uint64 (0 if undefined)
    bool mtimeDefined = false;
};

// One input item for szCreate7z. relPath uses '/' or '\\' (converted internally);
// absPath is the on-disk source for files (empty for directories).
struct SzCreateItem {
    std::wstring relPath;
    std::wstring absPath;
    bool isDir = false;
    uint64_t size = 0;     // file size in bytes (ignored for directories)
};

// Test-only helper: build a 7z archive with the loaded 7z.dll (IOutArchive).
// Used to generate fixtures so the read-only engine can be validated end-to-end.
// encryptFilenames=true sets the "he" property (encrypt header => names hidden).
bool szCreate7z(const std::vector<SzCreateItem>& items,
                const std::wstring& outPath,
                const std::wstring& password,
                bool encryptFilenames,
                std::wstring& err);

// Load 7z.dll from the given directory (UTF-8 path). Returns true on success.
bool szInit(const std::string& dllDirectoryUtf8);

// List archive contents. password may be empty. Returns true on success.
bool szList(const std::wstring& archivePath,
            const std::wstring& password,
            std::vector<SzEntry>& out,
            std::wstring& err);

// Extract all entries to destDir. Returns true on success.
bool szExtract(const std::wstring& archivePath,
               const std::wstring& destDir,
               const std::wstring& password,
               std::wstring& err);

// Test (verify) archive integrity. Returns true on success.
bool szTest(const std::wstring& archivePath,
            const std::wstring& password,
            std::wstring& err);

} // namespace sevenzip
} // namespace yasuo
