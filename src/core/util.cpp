#include "core/util.h"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <ctime>

namespace isopk {

fs::path U8Path(const std::string &utf8) {
	return fs::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string PathU8(const fs::path &p) {
	std::u8string s = p.u8string();
	return std::string(s.begin(), s.end());
}

std::string ToLower(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return s;
}

std::vector<std::string> Split(const std::string &s, char sep) {
	std::vector<std::string> out;
	size_t p = 0;
	while (p <= s.size()) {
		size_t q = s.find(sep, p);
		if (q == std::string::npos)
			q = s.size();
		std::string part = Trim(s.substr(p, q - p));
		if (!part.empty())
			out.push_back(part);
		p = q + 1;
	}
	return out;
}

std::string Trim(const std::string &s) {
	size_t a = 0, b = s.size();
	while (a < b && std::isspace((unsigned char)s[a]))
		++a;
	while (b > a && std::isspace((unsigned char)s[b - 1]))
		--b;
	return s.substr(a, b - a);
}

bool ParseSize(const std::string &in, uint64_t *out) {
	std::string s = ToLower(Trim(in));
	if (s.empty())
		return false;
	uint64_t mul = 1;
	if (s.size() > 1 && s.back() == 'b')
		s.pop_back();
	switch (s.back()) {
	case 'k': mul = 1ull << 10; s.pop_back(); break;
	case 'm': mul = 1ull << 20; s.pop_back(); break;
	case 'g': mul = 1ull << 30; s.pop_back(); break;
	default: break;
	}
	if (s.empty())
		return false;
	char *end = nullptr;
	double v = strtod(s.c_str(), &end);
	if (!end || *end != '\0' || v < 0)
		return false;
	*out = (uint64_t)(v * (double)mul + 0.5);
	return true;
}

std::string Format(const char *fmt, ...) {
	char buf[1024];
	va_list va;
	va_start(va, fmt);
	int n = vsnprintf(buf, sizeof(buf), fmt, va);
	va_end(va);
	if (n < (int)sizeof(buf))
		return std::string(buf, n < 0 ? 0 : n);
	std::string big(n + 1, '\0');
	va_start(va, fmt);
	vsnprintf(big.data(), big.size(), fmt, va);
	va_end(va);
	big.resize(n);
	return big;
}

std::string HumanBytes(uint64_t bytes) {
	const char *units[] = { "B", "KB", "MB", "GB", "TB" };
	double v = (double)bytes;
	int u = 0;
	while (v >= 1024.0 && u < 4) {
		v /= 1024.0;
		++u;
	}
	if (u == 0)
		return Format("%llu B", (unsigned long long)bytes);
	return Format("%.2f %s", v, units[u]);
}

std::string HumanDuration(double s) {
	if (s < 0)
		return "-";
	if (s < 60)
		return Format("%.1f s", s);
	uint64_t t = (uint64_t)(s + 0.5);
	if (t < 3600)
		return Format("%u:%02u", (unsigned)(t / 60), (unsigned)(t % 60));
	return Format("%u:%02u:%02u", (unsigned)(t / 3600), (unsigned)(t / 60 % 60), (unsigned)(t % 60));
}

std::string Timestamp() {
	std::time_t t = std::time(nullptr);
	std::tm tm{};
#ifdef _WIN32
	localtime_s(&tm, &t);
#else
	localtime_r(&t, &tm);
#endif
	char buf[32];
	std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
	return buf;
}

uint32_t FourCC(const char *s) {
	char c[4] = { ' ', ' ', ' ', ' ' };
	for (int i = 0; i < 4 && s[i]; ++i)
		c[i] = s[i];
	return ((uint32_t)(uint8_t)c[0] << 24) | ((uint32_t)(uint8_t)c[1] << 16) | ((uint32_t)(uint8_t)c[2] << 8) | (uint32_t)(uint8_t)c[3];
}

std::string FourCCName(uint32_t v) {
	std::string s;
	for (int i = 3; i >= 0; --i) {
		char c = (char)((v >> (i * 8)) & 0xFF);
		if (c != ' ' && c != 0)
			s += c;
	}
	return s;
}

}  // namespace isopk
