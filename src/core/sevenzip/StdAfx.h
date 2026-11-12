// StdAfx.h — precompiled-header shim for YasuoCompressor's 7-Zip client wrapper.
// Pulls in 7-Zip's Common.h (which sets up UNICODE, the COM helpers and Windows
// type mappings) so the wrapper source compiles against the LZMA SDK headers
// exactly the way 7-Zip's own Client7z sample does.
#ifndef YASUO_SEVENZIP_STDAFX_H
#define YASUO_SEVENZIP_STDAFX_H
#include "Common/Common.h"
#endif
