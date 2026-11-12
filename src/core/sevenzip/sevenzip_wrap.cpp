// sevenzip_wrap.cpp — runtime wrapper around 7z.dll (Format7zF COM bundle)
//
// Uses 7-Zip's own COM interfaces and helper classes (compiled from the LZMA SDK
// source) to drive IInArchive for 7z / RAR / RAR5. Encryption (AES-256, encrypted
// filenames), solid and multivolume 7z, and password-protected RAR5 are handled by
// 7-Zip itself. We only READ archives here (RAR compression is not permitted by the
// unRAR license, matching the project's design).

// MyInitGuid.h MUST be the first include: it defines INITGUID so that the
// DEFINE_GUID macros in the 7-Zip interface headers emit real GUID definitions
// (instead of extern references) for this translation unit — the single place
// in our build where every IID/CLSID (7z, RAR, stream interfaces...) is defined.
#include "Common/MyInitGuid.h"
#include "StdAfx.h"

#ifndef RINOK
#define RINOK(x) { HRESULT __r = (x); if (__r != S_OK) return __r; }
#endif

#include <vector>
#include <string>

#include "7zip/Archive/IArchive.h"
#include "7zip/IPassword.h"
#include "7zip/Common/FileStreams.h"
#include "Windows/DLL.h"
#include "Windows/FileDir.h"
#include "Windows/FileFind.h"
#include "Windows/FileName.h"
#include "Windows/PropVariant.h"
#include "Windows/PropVariantConv.h"

#include "sevenzip_wrap.h"

// 7-Zip's DLL.cpp references this global but never defines it in the client
// helper set we compile. We provide a definition (NULL is fine: we load 7z.dll
// ourselves via Win32 LoadLibraryW, not through 7-Zip's CLibrary machinery).
HINSTANCE g_hInstance = NULL;

using namespace NWindows;
using namespace NFile;
using namespace NDir;

// 7z format: {23170F69-40C1-278A-1000-000110070000}
// RAR format: {23170F69-40C1-278A-1000-0001100A0000}
#define DEFINE_GUID_ARC(name, id) Z7_DEFINE_GUID(name, \
  0x23170F69, 0x40C1, 0x278A, 0x10, 0x00, 0x00, 0x01, 0x10, id, 0x00, 0x00);

enum { kId_7z = 7, kId_Rar = 0x0A };
DEFINE_GUID_ARC(CLSID_Format7z, kId_7z)
DEFINE_GUID_ARC(CLSID_FormatRar, kId_Rar)

namespace {

struct SZLib {
    HMODULE hmod = nullptr;
    Func_CreateObject createObject = nullptr;
};
SZLib g_lib;

std::wstring HResultToString(HRESULT hr) {
    wchar_t buf[32];
    buf[0] = 0;
    if (hr == E_ABORT) return L"aborted (wrong password?)";
    if (hr == E_OUTOFMEMORY) return L"out of memory";
    if (hr == E_INVALIDARG) return L"invalid argument";
    _snwprintf(buf, 31, L"hresult 0x%08X", (unsigned)hr);
    return buf;
}

// Local mirror of 7-Zip's CArcTime (defined inline in Client7z.cpp). Holds an
// archive entry timestamp plus its precision metadata.
struct CArcTime
{
    FILETIME FT;
    UInt16 Prec;
    Byte Ns100;
    bool Def;

    CArcTime() { Clear(); }
    void Clear()
    {
        FT.dwHighDateTime = FT.dwLowDateTime = 0;
        Prec = 0;
        Ns100 = 0;
        Def = false;
    }
    void Set_From_Prop(const PROPVARIANT& prop)
    {
        FT = prop.filetime;
        unsigned prec = 0;
        unsigned ns100 = 0;
        const unsigned prec_Temp = prop.wReserved1;
        if (prec_Temp != 0
            && prec_Temp <= k_PropVar_TimePrec_1ns
            && prop.wReserved3 == 0) {
            const unsigned ns100_Temp = prop.wReserved2;
            if (ns100_Temp < 100) {
                ns100 = ns100_Temp;
                prec = prec_Temp;
            }
        }
        Prec = (UInt16)prec;
        Ns100 = (Byte)ns100;
        Def = true;
    }
    void Write_To_FiTime(CFiTime& dest) const
    {
        dest = FT;
    }
};

static HRESULT IsArchiveItemFolder(IInArchive* archive, UInt32 index, bool& result) {
    NCOM::CPropVariant prop;
    RINOK(archive->GetProperty(index, kpidIsDir, &prop))
    if (prop.vt == VT_BOOL)
        result = (prop.boolVal != VARIANT_FALSE);
    else if (prop.vt == VT_EMPTY)
        result = false;
    else
        return E_FAIL;
    return S_OK;
}

// ---- Open callback (supplies password when archive requests it) --------------
class CArchiveOpenCallback Z7_final :
    public IArchiveOpenCallback,
    public ICryptoGetTextPassword,
    public CMyUnknownImp
{
    Z7_IFACES_IMP_UNK_2(IArchiveOpenCallback, ICryptoGetTextPassword)
public:
    UString Password;
    CArchiveOpenCallback(const UString& pw) : Password(pw) {}
};

Z7_COM7F_IMF(CArchiveOpenCallback::SetTotal(const UInt64*, const UInt64*)) { return S_OK; }
Z7_COM7F_IMF(CArchiveOpenCallback::SetCompleted(const UInt64*, const UInt64*)) { return S_OK; }
Z7_COM7F_IMF(CArchiveOpenCallback::CryptoGetTextPassword(BSTR* password))
{
    *password = ::SysAllocString(Password);
    return S_OK;
}

// ---- Extract callback (writes files under destDir) ---------------------------
class CArchiveExtractCallback Z7_final :
    public IArchiveExtractCallback,
    public ICryptoGetTextPassword,
    public CMyUnknownImp
{
    Z7_IFACES_IMP_UNK_2(IArchiveExtractCallback, ICryptoGetTextPassword)
    Z7_IFACE_COM7_IMP(IProgress)

    CMyComPtr<IInArchive> _archiveHandler;
    FString _directoryPath;
    UString _filePath;
    FString _diskFilePath;
    bool _extractMode;
    COutFileStream* _outFileStreamSpec = nullptr;
    CMyComPtr<ISequentialOutStream> _outFileStream;
    bool _attribDefined = false;
    UInt32 _attrib = 0;
    bool _mtimeDefined = false;
    CArcTime _mtime;

public:
    UInt64 NumErrors = 0;
    UString Password;

    CArchiveExtractCallback(const UString& pw) : Password(pw) {}
    void Init(IInArchive* archiveHandler, const FString& directoryPath)
    {
        _archiveHandler = archiveHandler;
        _directoryPath = directoryPath;
        NName::NormalizeDirPathPrefix(_directoryPath);
    }
};

Z7_COM7F_IMF(CArchiveExtractCallback::SetTotal(UInt64)) { return S_OK; }
Z7_COM7F_IMF(CArchiveExtractCallback::SetCompleted(const UInt64*)) { return S_OK; }

// sanitize an archive entry name into a relative, containment-safe local path
bool BuildLocalPath(const UString& fullPath, UString& outRel)
{
    UString cur;
    for (unsigned i = 0; i < fullPath.Len(); i++) {
        wchar_t c = fullPath[i];
        if (c == L'/' || c == L'\\') {
            if (cur.IsEqualTo_Ascii_NoCase("..") || cur == L".") {
                if (cur.IsEqualTo_Ascii_NoCase("..")) return false; // escape attempt
                cur.Empty();
                continue;
            }
            if (!cur.IsEmpty()) { outRel += cur; outRel += L'\\'; cur.Empty(); }
        } else {
            cur += c;
        }
    }
    if (cur.IsEqualTo_Ascii_NoCase("..")) return false;
    if (!cur.IsEmpty()) outRel += cur;
    return true;
}

Z7_COM7F_IMF(CArchiveExtractCallback::GetStream(UInt32 index,
    ISequentialOutStream** outStream, Int32 askExtractMode))
{
    *outStream = NULL;
    _outFileStream.Release();

    NCOM::CPropVariant prop;
    RINOK(_archiveHandler->GetProperty(index, kpidPath, &prop))
    UString fullPath;
    if (prop.vt == VT_EMPTY)
        fullPath = L"[Content]";
    else if (prop.vt == VT_BSTR)
        fullPath = prop.bstrVal;
    else
        return E_FAIL;
    _filePath = fullPath;

    if (askExtractMode != NArchive::NExtract::NAskMode::kExtract)
        return S_OK;

    NCOM::CPropVariant propA;
    RINOK(_archiveHandler->GetProperty(index, kpidAttrib, &propA))
    _attribDefined = false;
    if (propA.vt != VT_EMPTY) {
        if (propA.vt != VT_UI4) return E_FAIL;
        _attrib = propA.ulVal; _attribDefined = true;
    }

    bool isDir = false;
    RINOK(IsArchiveItemFolder(_archiveHandler, index, isDir))

    NCOM::CPropVariant propM;
    RINOK(_archiveHandler->GetProperty(index, kpidMTime, &propM))
    _mtime.Clear(); _mtimeDefined = false;
    if (propM.vt == VT_FILETIME) { _mtime.Set_From_Prop(propM); _mtimeDefined = true; }

    UString rel;
    if (!BuildLocalPath(fullPath, rel))
        return E_FAIL; // path traversal -> refuse
    if (rel.IsEmpty())
        return S_OK;

    FString fullProcessedPath = _directoryPath + rel;
    _diskFilePath = fullProcessedPath;

    if (isDir) {
        NDir::CreateComplexDir(fullProcessedPath);
        return S_OK;
    }

    int slashPos = fullProcessedPath.ReverseFind_PathSepar();
    if (slashPos >= 0)
        NDir::CreateComplexDir(fullProcessedPath.Left(slashPos));

    NFind::CFileInfo fi;
    if (fi.Find(fullProcessedPath)) {
        if (!NDir::DeleteFileAlways(fullProcessedPath))
            return E_ABORT;
    }
    _outFileStreamSpec = new COutFileStream;
    CMyComPtr<ISequentialOutStream> outStreamLoc(_outFileStreamSpec);
    if (!_outFileStreamSpec->Create_ALWAYS(fullProcessedPath))
        return E_ABORT;
    _outFileStream = outStreamLoc;
    *outStream = outStreamLoc.Detach();
    return S_OK;
}

Z7_COM7F_IMF(CArchiveExtractCallback::PrepareOperation(Int32 askExtractMode))
{
    _extractMode = (askExtractMode == NArchive::NExtract::NAskMode::kExtract);
    return S_OK;
}

Z7_COM7F_IMF(CArchiveExtractCallback::SetOperationResult(Int32 operationResult))
{
    if (operationResult != NArchive::NExtract::NOperationResult::kOK)
        NumErrors++;
    if (_outFileStream) {
        if (_mtimeDefined) {
            CFiTime ft; _mtime.Write_To_FiTime(ft);
            _outFileStreamSpec->SetMTime(&ft);
        }
        _outFileStreamSpec->Close();
    }
    _outFileStream.Release();
    if (_extractMode && _attribDefined)
        SetFileAttrib_PosixHighDetect(_diskFilePath, _attrib);
    return S_OK;
}

Z7_COM7F_IMF(CArchiveExtractCallback::CryptoGetTextPassword(BSTR* password))
{
    *password = ::SysAllocString(Password);
    return S_OK;
}

// Try to open the archive with the given format CLSID. Returns S_OK on success.
HRESULT TryOpen(IInArchive* archive, const FString& archiveName, const UString& password)
{
    CInFileStream* fileSpec = new CInFileStream;
    CMyComPtr<IInStream> file(fileSpec);
    if (!fileSpec->Open(archiveName))
        return E_FAIL;
    CArchiveOpenCallback* cb = new CArchiveOpenCallback(password);
    CMyComPtr<IArchiveOpenCallback> openCallback(cb);
    cb->Password = password;
    const UInt64 scanSize = (UInt64)1 << 23;
    return archive->Open(file, &scanSize, openCallback);
}

} // namespace

namespace yasuo {
namespace sevenzip {

bool szInit(const std::string& dllDirectoryUtf8)
{
    if (g_lib.hmod && g_lib.createObject) return true;
    std::wstring wdir;
    {
        int n = MultiByteToWideChar(CP_UTF8, 0, dllDirectoryUtf8.c_str(), -1, NULL, 0);
        if (n > 0) {
            wdir.resize(n - 1);
            MultiByteToWideChar(CP_UTF8, 0, dllDirectoryUtf8.c_str(), -1, &wdir[0], n);
        }
        if (!wdir.empty() && wdir.back() != L'\\' && wdir.back() != L'/')
            wdir += L'\\';
    }
    std::wstring dllPath = wdir + L"7z.dll";
    g_lib.hmod = ::LoadLibraryW(dllPath.c_str());
    if (!g_lib.hmod) return false;
    g_lib.createObject = (Func_CreateObject)::GetProcAddress(g_lib.hmod, "CreateObject");
    return g_lib.createObject != nullptr;
}

bool szList(const std::wstring& archivePath,
            const std::wstring& password,
            std::vector<SzEntry>& out,
            std::wstring& err)
{
    out.clear(); err.clear();
    if (!g_lib.createObject) { err = L"7z.dll not loaded"; return false; }

    CMyComPtr<IInArchive> archive;
    if (g_lib.createObject(&CLSID_Format7z, &IID_IInArchive, (void**)&archive) != S_OK)
    { err = L"cannot create 7z handler"; return false; }

    FString name(archivePath.c_str());
    HRESULT hr = TryOpen(archive, name, UString(password.c_str()));
    if (hr != S_OK) {
        CMyComPtr<IInArchive> rar;
        if (g_lib.createObject(&CLSID_FormatRar, &IID_IInArchive, (void**)&rar) == S_OK) {
            archive = rar;
            hr = TryOpen(archive, name, UString(password.c_str()));
        }
    }
    if (hr != S_OK) { err = L"cannot open archive: " + HResultToString(hr); return false; }

    UInt32 numItems = 0;
    archive->GetNumberOfItems(&numItems);
    for (UInt32 i = 0; i < numItems; i++) {
        SzEntry e;
        NCOM::CPropVariant prop;
        archive->GetProperty(i, kpidPath, &prop);
        if (prop.vt == VT_BSTR) e.path = prop.bstrVal;
        else if (prop.vt == VT_EMPTY) e.path = L"[Content]";

        NCOM::CPropVariant pSize;
        archive->GetProperty(i, kpidSize, &pSize);
        if (pSize.vt == VT_UI4) e.size = pSize.ulVal;
        else if (pSize.vt == VT_UI8) e.size = (uint64_t)pSize.uhVal.QuadPart;

        NCOM::CPropVariant pPack;
        archive->GetProperty(i, kpidPackSize, &pPack);
        if (pPack.vt == VT_UI4) e.packedSize = pPack.ulVal;
        else if (pPack.vt == VT_UI8) e.packedSize = (uint64_t)pPack.uhVal.QuadPart;

        NCOM::CPropVariant pEnc;
        archive->GetProperty(i, kpidEncrypted, &pEnc);
        if (pEnc.vt == VT_BOOL) e.encrypted = (pEnc.boolVal != VARIANT_FALSE);

        bool isDir = false;
        IsArchiveItemFolder(archive, i, isDir);
        e.isDir = isDir;

        NCOM::CPropVariant pM;
        archive->GetProperty(i, kpidMTime, &pM);
        if (pM.vt == VT_FILETIME) {
            e.mtimeDefined = true;
            e.mtime = ((uint64_t)pM.filetime.dwHighDateTime << 32) | pM.filetime.dwLowDateTime;
        }
        out.push_back(e);
    }
    return true;
}

bool szExtract(const std::wstring& archivePath,
               const std::wstring& destDir,
               const std::wstring& password,
               std::wstring& err)
{
    err.clear();
    if (!g_lib.createObject) { err = L"7z.dll not loaded"; return false; }

    CMyComPtr<IInArchive> archive;
    if (g_lib.createObject(&CLSID_Format7z, &IID_IInArchive, (void**)&archive) != S_OK)
    { err = L"cannot create 7z handler"; return false; }
    FString name(archivePath.c_str());
    HRESULT hr = TryOpen(archive, name, UString(password.c_str()));
    if (hr != S_OK) {
        CMyComPtr<IInArchive> rar;
        if (g_lib.createObject(&CLSID_FormatRar, &IID_IInArchive, (void**)&rar) == S_OK) {
            archive = rar;
            hr = TryOpen(archive, name, UString(password.c_str()));
        }
    }
    if (hr != S_OK) { err = L"cannot open archive: " + HResultToString(hr); return false; }

    CArchiveExtractCallback* cb = new CArchiveExtractCallback(UString(password.c_str()));
    CMyComPtr<IArchiveExtractCallback> extractCallback(cb);
    cb->Init(archive, FString(destDir.c_str()));

    hr = archive->Extract(NULL, (UInt32)(Int32)-1, false, extractCallback);
    if (hr != S_OK || cb->NumErrors != 0) {
        err = L"extract failed: " + HResultToString(hr) +
              (cb->NumErrors ? L" (data errors)" : L"");
        return false;
    }
    return true;
}

bool szTest(const std::wstring& archivePath,
            const std::wstring& password,
            std::wstring& err)
{
    err.clear();
    if (!g_lib.createObject) { err = L"7z.dll not loaded"; return false; }

    CMyComPtr<IInArchive> archive;
    if (g_lib.createObject(&CLSID_Format7z, &IID_IInArchive, (void**)&archive) != S_OK)
    { err = L"cannot create 7z handler"; return false; }
    FString name(archivePath.c_str());
    HRESULT hr = TryOpen(archive, name, UString(password.c_str()));
    if (hr != S_OK) {
        CMyComPtr<IInArchive> rar;
        if (g_lib.createObject(&CLSID_FormatRar, &IID_IInArchive, (void**)&rar) == S_OK) {
            archive = rar;
            hr = TryOpen(archive, name, UString(password.c_str()));
        }
    }
    if (hr != S_OK) { err = L"cannot open archive: " + HResultToString(hr); return false; }

    CArchiveExtractCallback* cb = new CArchiveExtractCallback(UString(password.c_str()));
    CMyComPtr<IArchiveExtractCallback> extractCallback(cb);
    cb->Init(archive, FString()); // no output dir needed for test

    hr = archive->Extract(NULL, (UInt32)(Int32)-1, true, extractCallback); // testMode = true
    if (hr != S_OK || cb->NumErrors != 0) {
        err = L"test failed: " + HResultToString(hr) +
              (cb->NumErrors ? L" (data errors)" : L"");
        return false;
    }
    return true;
}

// ---------------- Test-only 7z creator (IOutArchive) -------------------------
// Generates a 7z archive through 7z.dll so the read-only engine can be exercised
// against a real Format7zF-produced file. Not used by the product UI.

namespace {

class CArchiveUpdateCallback Z7_final :
    public IArchiveUpdateCallback,
    public ICryptoGetTextPassword2,
    public CMyUnknownImp
{
    Z7_IFACES_IMP_UNK_2(IArchiveUpdateCallback, ICryptoGetTextPassword2)
    Z7_IFACE_COM7_IMP(IProgress)
    const std::vector<SzCreateItem>* _items;
    UString _password;

public:
    CArchiveUpdateCallback(const std::vector<SzCreateItem>& items, const UString& pw)
        : _items(&items), _password(pw) {}
};

Z7_COM7F_IMF(CArchiveUpdateCallback::SetTotal(UInt64)) { return S_OK; }
Z7_COM7F_IMF(CArchiveUpdateCallback::SetCompleted(const UInt64*)) { return S_OK; }

Z7_COM7F_IMF(CArchiveUpdateCallback::GetUpdateItemInfo(UInt32, Int32* newData, Int32* newProps, UInt32* indexInArchive))
{
    *newData = 1; *newProps = 1; *indexInArchive = 0xFFFFFFFF; // brand-new item
    return S_OK;
}

Z7_COM7F_IMF(CArchiveUpdateCallback::GetProperty(UInt32 index, PROPID propID, PROPVARIANT* value))
{
    NCOM::CPropVariant prop;
    const SzCreateItem& it = (*_items)[index];
    switch (propID) {
    case kpidPath: {
        UString p;
        for (wchar_t c : it.relPath)
            p += (c == L'/' ? L'\\' : c);
        prop = p;
        break;
    }
    case kpidIsDir:
        prop.vt = VT_BOOL;
        prop.boolVal = it.isDir ? VARIANT_TRUE : VARIANT_FALSE;
        break;
    case kpidSize:
        if (!it.isDir) { prop.vt = VT_UI8; prop.uhVal.QuadPart = (LONG64)it.size; }
        break;
    case kpidAttrib:
        prop.vt = VT_UI4;
        prop.ulVal = it.isDir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        break;
    default:
        break;
    }
    prop.Detach(value);
    return S_OK;
}

Z7_COM7F_IMF(CArchiveUpdateCallback::GetStream(UInt32 index, ISequentialInStream** inStream))
{
    *inStream = NULL;
    const SzCreateItem& it = (*_items)[index];
    if (it.isDir) return S_OK;
    CInFileStream* inSpec = new CInFileStream;
    CMyComPtr<ISequentialInStream> inStreamLoc(inSpec);
    if (!inSpec->Open(FString(it.absPath.c_str())))
        return E_ABORT;
    *inStream = inStreamLoc.Detach();
    return S_OK;
}

Z7_COM7F_IMF(CArchiveUpdateCallback::SetOperationResult(Int32)) { return S_OK; }

Z7_COM7F_IMF(CArchiveUpdateCallback::CryptoGetTextPassword2(Int32* passwordIsDefined, BSTR* password))
{
    *passwordIsDefined = _password.IsEmpty() ? 0 : 1;
    if (*passwordIsDefined)
        *password = ::SysAllocString(_password);
    return S_OK;
}

} // namespace (anon inside yasuo::sevenzip)

bool szCreate7z(const std::vector<SzCreateItem>& items,
                const std::wstring& outPath,
                const std::wstring& password,
                bool encryptFilenames,
                std::wstring& err)
{
    err.clear();
    if (!g_lib.createObject) { err = L"7z.dll not loaded"; return false; }

    CMyComPtr<IOutArchive> outArchive;
    if (g_lib.createObject(&CLSID_Format7z, &IID_IOutArchive, (void**)&outArchive) != S_OK) {
        err = L"cannot create 7z writer"; return false;
    }

    // Compression level + optional header encryption
    std::vector<std::wstring> names;
    std::vector<NCOM::CPropVariant> values;
    NCOM::CPropVariant vLevel; vLevel.vt = VT_UI4; vLevel.ulVal = 5;
    names.push_back(L"x"); values.push_back(vLevel);
    if (encryptFilenames) {
        NCOM::CPropVariant vHe; vHe.vt = VT_BOOL; vHe.boolVal = VARIANT_TRUE;
        names.push_back(L"he"); values.push_back(vHe);
    }
    std::vector<const wchar_t*> namePtrs;
    for (const auto& n : names) namePtrs.push_back(n.c_str());
    // SetProperties lives on ISetProperties, a separate interface from IOutArchive.
    CMyComPtr<ISetProperties> setProps;
    if (outArchive->QueryInterface(IID_ISetProperties, (void**)&setProps) == S_OK && setProps)
        setProps->SetProperties(namePtrs.data(), values.data(), (UInt32)values.size());

    COutFileStream* outSpec = new COutFileStream;
    CMyComPtr<ISequentialOutStream> outStream(outSpec);
    if (!outSpec->Create_ALWAYS(FString(outPath.c_str()))) {
        err = L"cannot create output 7z"; return false;
    }

    CArchiveUpdateCallback* cb = new CArchiveUpdateCallback(items, UString(password.c_str()));
    CMyComPtr<IArchiveUpdateCallback> updateCallback(cb);
    HRESULT hr = outArchive->UpdateItems(outStream, (UInt32)items.size(), updateCallback);
    if (hr != S_OK) { err = L"7z update failed: " + HResultToString(hr); return false; }
    return true;
}

} // namespace sevenzip
} // namespace yasuo
