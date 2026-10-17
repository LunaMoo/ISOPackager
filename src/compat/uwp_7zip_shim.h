// Force-included into maxcso's 7-zip code for UWP builds: the few desktop-only Win32 calls 7-zip's
// deflate encoder path makes, on top of the CRT.
//  - CharUpperW / CharLowerW (user32): called both on single characters (passed in the pointer, as
//    Win32 allows) and on whole strings.
//  - AreFileApisANSI: only picks the code page for ANSI file names; UWP file APIs are Unicode.
//  - InitPropVariantFromUInt32: an inline helper from propvarutil.h, hidden from UWP.
#pragma once
#if defined(_WIN32)
#include <windows.h>
#include <wctype.h>
#if defined(WINAPI_FAMILY) && WINAPI_FAMILY == WINAPI_FAMILY_APP
static __inline LPWSTR isopk_CharCase(LPWSTR p, int upper) {
	if (((UINT_PTR)p >> 16) == 0) {
		wint_t c = (wint_t)(UINT_PTR)p;
		return (LPWSTR)(UINT_PTR)(upper ? towupper(c) : towlower(c));
	}
	for (LPWSTR q = p; *q; ++q)
		*q = (WCHAR)(upper ? towupper(*q) : towlower(*q));
	return p;
}
#define CharUpperW(p) isopk_CharCase((p), 1)
#define CharLowerW(p) isopk_CharCase((p), 0)
static __inline BOOL AreFileApisANSI(void) { return TRUE; }  // a function: 7-zip calls ::AreFileApisANSI()
#ifdef __cplusplus
#include <propidl.h>
inline HRESULT InitPropVariantFromUInt32(ULONG value, PROPVARIANT *pv) {
	pv->vt = VT_UI4;
	pv->ulVal = value;
	return S_OK;
}
#endif
#endif
#endif
