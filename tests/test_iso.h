// Builds a small but well-formed PSP UMD image (ISO9660 with UMD_DATA.BIN and
// PSP_GAME/PARAM.SFO) for the self-test, so no real game is needed.
#pragma once

#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace testiso {

inline void Put16(std::vector<uint8_t> &b, size_t at, uint16_t v) {
	b[at] = v & 0xFF;
	b[at + 1] = v >> 8;
}
inline void Put32(std::vector<uint8_t> &b, size_t at, uint32_t v) {
	for (int i = 0; i < 4; ++i)
		b[at + i] = (v >> (8 * i)) & 0xFF;
}
inline void Put32Both(std::vector<uint8_t> &b, size_t at, uint32_t v) {
	Put32(b, at, v);
	for (int i = 0; i < 4; ++i)
		b[at + 4 + i] = (v >> (8 * (3 - i))) & 0xFF;
}
inline void Put16Both(std::vector<uint8_t> &b, size_t at, uint16_t v) {
	Put16(b, at, v);
	b[at + 2] = v >> 8;
	b[at + 3] = v & 0xFF;
}

inline std::vector<uint8_t> MakeSfo(const std::string &discId, const std::string &title) {
	struct E {
		std::string key, value;
	};
	std::vector<E> es = { { "CATEGORY", "UG" }, { "DISC_ID", discId }, { "DISC_VERSION", "1.00" }, { "PSP_SYSTEM_VER", "6.60" },
		{ "TITLE", title } };
	std::vector<uint8_t> keys, data;
	std::vector<uint8_t> out(20 + es.size() * 16);
	for (size_t i = 0; i < es.size(); ++i) {
		uint32_t len = (uint32_t)es[i].value.size() + 1, max = (len + 3) & ~3u;
		size_t e = 20 + i * 16;
		Put16(out, e, (uint16_t)keys.size());
		Put16(out, e + 2, 0x0204);
		Put32(out, e + 4, len);
		Put32(out, e + 8, max);
		Put32(out, e + 12, (uint32_t)data.size());
		keys.insert(keys.end(), es[i].key.begin(), es[i].key.end());
		keys.push_back(0);
		size_t d = data.size();
		data.resize(d + max, 0);
		memcpy(&data[d], es[i].value.data(), es[i].value.size());
	}
	while (keys.size() % 4)
		keys.push_back(0);
	memcpy(out.data(), "\0PSF", 4);
	Put32(out, 4, 0x0101);
	Put32(out, 8, (uint32_t)out.size());
	Put32(out, 12, (uint32_t)(out.size() + keys.size()));
	Put32(out, 16, (uint32_t)es.size());
	out.insert(out.end(), keys.begin(), keys.end());
	out.insert(out.end(), data.begin(), data.end());
	return out;
}

struct File {
	std::string name;
	std::vector<uint8_t> data;
	uint32_t lba = 0;
};

// A directory record of `len` bytes at b[at].
inline size_t Record(std::vector<uint8_t> &b, size_t at, uint32_t lba, uint32_t size, bool dir, const std::string &name) {
	size_t nameLen = name.size();
	size_t len = 33 + nameLen + ((nameLen % 2) == 0 ? 1 : 0);
	b[at] = (uint8_t)len;
	Put32Both(b, at + 2, lba);
	Put32Both(b, at + 10, size);
	b[at + 18] = 126;  // 2026
	b[at + 19] = 1;
	b[at + 20] = 1;
	b[at + 25] = dir ? 2 : 0;
	Put16Both(b, at + 28, 1);
	b[at + 32] = (uint8_t)nameLen;
	memcpy(&b[at + 33], name.data(), nameLen);
	return len;
}

// sizeMB of payload: a third random, a third compressible text, a third zeros.
inline std::vector<uint8_t> MakeIso(uint32_t sizeMB, const std::string &discId = "ULUS99999", const std::string &title = "ISOPackager Test") {
	const uint32_t S = 2048;
	std::vector<File> files;
	std::string umd = discId.substr(0, 4) + "-" + discId.substr(4) + "|0000000000000001|0001|G";
	files.push_back({ "UMD_DATA.BIN", std::vector<uint8_t>(umd.begin(), umd.end()) });
	std::vector<uint8_t> sfo = MakeSfo(discId, title);

	std::mt19937 rng(42);
	uint32_t third = sizeMB * (1u << 20) / 3;
	File rnd{ "RANDOM.BIN" }, txt{ "TEXT.BIN" }, zero{ "ZERO.BIN" };
	rnd.data.resize(third);
	for (auto &c : rnd.data)
		c = (uint8_t)rng();
	const char *words[] = { "psp ", "umd ", "game ", "level ", "sprite ", "sound ", "texture ", "model ", "\n" };
	while (txt.data.size() < third) {
		const char *w = words[rng() % 9];
		txt.data.insert(txt.data.end(), w, w + strlen(w));
	}
	txt.data.resize(third);
	zero.data.assign(third, 0);
	files.push_back(std::move(rnd));
	files.push_back(std::move(txt));
	files.push_back(std::move(zero));

	// Layout: 0-15 system area, 16 PVD, 17 terminator, 18 root dir, 19 PSP_GAME dir, 20 SFO, then files.
	uint32_t next = 21;
	for (auto &f : files) {
		f.lba = next;
		next += (uint32_t)((f.data.size() + S - 1) / S);
	}
	const uint32_t total = next;
	std::vector<uint8_t> iso((size_t)total * S, 0);

	// Root directory.
	std::vector<uint8_t> root(S, 0);
	size_t p = 0;
	p += Record(root, p, 18, S, true, std::string(1, '\0'));
	p += Record(root, p, 18, S, true, std::string(1, '\1'));
	p += Record(root, p, 19, S, true, "PSP_GAME");
	for (auto &f : files)
		p += Record(root, p, f.lba, (uint32_t)f.data.size(), false, f.name + ";1");
	memcpy(&iso[18 * S], root.data(), S);

	std::vector<uint8_t> game(S, 0);
	p = 0;
	p += Record(game, p, 19, S, true, std::string(1, '\0'));
	p += Record(game, p, 18, S, true, std::string(1, '\1'));
	p += Record(game, p, 20, (uint32_t)sfo.size(), false, "PARAM.SFO;1");
	memcpy(&iso[19 * S], game.data(), S);
	memcpy(&iso[20 * S], sfo.data(), sfo.size());
	for (auto &f : files)
		memcpy(&iso[(size_t)f.lba * S], f.data.data(), f.data.size());

	// Primary volume descriptor.
	std::vector<uint8_t> pvd(S, ' ');
	pvd[0] = 1;
	memcpy(&pvd[1], "CD001", 5);
	pvd[6] = 1;
	pvd[7] = 0;
	memcpy(&pvd[8], "PSP GAME", 8);
	memcpy(&pvd[40], "UMD VIDEO", 9);
	for (int i = 72; i < 80; ++i)
		pvd[i] = 0;
	Put32Both(pvd, 80, total);
	for (int i = 88; i < 120; ++i)
		pvd[i] = 0;
	Put16Both(pvd, 120, 1);
	Put16Both(pvd, 124, 1);
	Put16Both(pvd, 128, (uint16_t)S);
	for (int i = 132; i < 190; ++i)
		pvd[i] = 0;
	Record(pvd, 156, 18, S, true, std::string(1, '\0'));
	pvd[881] = 1;
	for (int i = 882; i < S; ++i)
		pvd[i] = 0;
	memcpy(&iso[16 * S], pvd.data(), S);
	iso[17 * S] = 255;
	memcpy(&iso[17 * S + 1], "CD001", 5);
	iso[17 * S + 6] = 1;
	return iso;
}

// A small GameCube disc: header (game ID, magic, title), an FST with a few files (random, zeros,
// text) and zeros elsewhere. sizeMB rounded to whole 2 KiB sectors.
inline void PutBE32(std::vector<uint8_t> &b, size_t at, uint32_t v) {
	for (int i = 0; i < 4; ++i)
		b[at + i] = (uint8_t)(v >> (24 - 8 * i));
}

inline std::vector<uint8_t> MakeGameCubeDisc(uint32_t sizeMB, const std::string &gameId = "GTST01", const std::string &title = "ISOPackager GC Test") {
	std::vector<uint8_t> d((size_t)sizeMB << 20, 0);
	memcpy(d.data(), gameId.data(), 6);
	PutBE32(d, 0x1C, 0xC2339F3D);
	memcpy(d.data() + 0x20, title.data(), title.size());
	struct F {
		uint32_t offset, size;
		int kind;
	} files[] = { { 0x20000, 0x123457, 0 }, { 0x200000, 0x300000, 1 }, { 0x500000, 0x180001, 2 } };
	const size_t entries = 1 + 3, fst = 0x10000, fstSize = entries * 12 + entries * 8;
	PutBE32(d, 0x424, (uint32_t)fst);
	PutBE32(d, 0x428, (uint32_t)fstSize);
	PutBE32(d, 0x42C, (uint32_t)fstSize);
	PutBE32(d, fst, 0x01000000);
	PutBE32(d, fst + 8, (uint32_t)entries);
	std::mt19937 rng(7);
	for (size_t i = 0; i < 3; ++i) {
		PutBE32(d, fst + (i + 1) * 12, (uint32_t)(i * 8));
		PutBE32(d, fst + (i + 1) * 12 + 4, files[i].offset);
		PutBE32(d, fst + (i + 1) * 12 + 8, files[i].size);
		snprintf((char *)d.data() + fst + entries * 12 + i * 8, 8, "f%zu", i);
		uint8_t *p = d.data() + files[i].offset;
		for (uint32_t k = 0; k < files[i].size; ++k)
			p[k] = files[i].kind == 0 ? (uint8_t)rng() : files[i].kind == 1 ? 0 : (uint8_t)"gamecube wii disc "[k % 18];
	}
	return d;
}

// A small PS2 disc: ISO9660 with SYSTEM.CNF (BOOT2) and a data file, plus a UDF volume
// recognition sequence after the ISO descriptors when udf (PS2 DVDs have one, PS2 CDs don't).
inline std::vector<uint8_t> MakePs2Iso(uint32_t sizeKB, bool udf, const std::string &boot = "SLUS_203.12") {
	const uint32_t S = 2048;
	// 16 PVD, 17 terminator, 18-20 BEA01 / NSR02 / TEA01 (or zeros), 21 root, 22 SYSTEM.CNF, 23.. DATA.BIN.
	std::string cnf = "BOOT2 = cdrom0:\\" + boot + ";1\r\nVER = 1.00\r\nVMODE = NTSC\r\n";
	const uint32_t dataBytes = sizeKB * 1024;
	const uint32_t total = 23 + (dataBytes + S - 1) / S;
	std::vector<uint8_t> iso((size_t)total * S, 0);
	std::mt19937 rng(7);
	for (uint32_t i = 0; i < dataBytes; ++i)
		iso[23 * S + i] = i % 3 ? (uint8_t)rng() : 0;
	memcpy(&iso[22 * S], cnf.data(), cnf.size());
	std::vector<uint8_t> root(S, 0);
	size_t p = 0;
	p += Record(root, p, 21, S, true, std::string(1, '\0'));
	p += Record(root, p, 21, S, true, std::string(1, '\1'));
	p += Record(root, p, 23, dataBytes, false, "DATA.BIN;1");
	p += Record(root, p, 22, (uint32_t)cnf.size(), false, "SYSTEM.CNF;1");
	memcpy(&iso[21 * S], root.data(), S);
	std::vector<uint8_t> pvd(S, 0);
	pvd[0] = 1;
	memcpy(&pvd[1], "CD001", 5);
	pvd[6] = 1;
	memcpy(&pvd[8], "PLAYSTATION", 11);
	Put32Both(pvd, 80, total);
	Put16Both(pvd, 120, 1);
	Put16Both(pvd, 124, 1);
	Put16Both(pvd, 128, (uint16_t)S);
	Record(pvd, 156, 21, S, true, std::string(1, '\0'));
	pvd[881] = 1;
	memcpy(&iso[16 * S], pvd.data(), S);
	iso[17 * S] = 255;
	memcpy(&iso[17 * S + 1], "CD001", 5);
	iso[17 * S + 6] = 1;
	if (udf) {
		const char *vrs[] = { "BEA01", "NSR02", "TEA01" };
		for (int i = 0; i < 3; ++i) {
			memcpy(&iso[(18 + i) * S + 1], vrs[i], 5);
			iso[(18 + i) * S + 6] = 1;
		}
	}
	return iso;
}

// 2048-byte sectors as raw 2352-byte CD sectors: MODE1 (sync, header, data) or MODE2 form 1
// (sync, header, XA subheader, data). EDC/ECC left zero: nothing here checks them.
inline std::vector<uint8_t> RawSectors(const std::vector<uint8_t> &iso, int mode) {
	const size_t n = iso.size() / 2048;
	std::vector<uint8_t> raw(n * 2352, 0);
	for (size_t i = 0; i < n; ++i) {
		uint8_t *s = &raw[i * 2352];
		memset(s + 1, 0xff, 10);
		const uint32_t f = (uint32_t)i + 150;
		auto bcd = [](uint32_t v) { return (uint8_t)((v / 10) << 4 | (v % 10)); };
		s[12] = bcd(f / 4500);
		s[13] = bcd((f / 75) % 60);
		s[14] = bcd(f % 75);
		s[15] = (uint8_t)mode;
		memcpy(s + (mode == 1 ? 16 : 24), &iso[i * 2048], 2048);
	}
	return raw;
}

}  // namespace testiso
