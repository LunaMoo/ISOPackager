// Force-included into maxcso's 7-zip code for UWP builds: CharUpperW / CharLowerW (user32, not in
// the UWP API set) on top of the CRT. 7-zip calls them both on single characters (passed in the
// pointer, as Win32 allows) and on whole strings.
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
#endif
#endif
