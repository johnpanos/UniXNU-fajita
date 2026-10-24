// apfswalk IMG OFFSET VOLID APFSDIR OUTDIR : extract an APFS directory tree (read-only), decompressing files. Build: see build-apfswalk.sh
#include <ApfsLib/ApfsContainer.h>
#include <ApfsLib/ApfsVolume.h>
#include <ApfsLib/ApfsDir.h>
#include <ApfsLib/Device.h>
#include <ApfsLib/Decmpfs.h>
#include <ApfsLib/Global.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sstream>
#include <sys/stat.h>
static ApfsDir *D; static long nf, nd;
static void walk(uint64_t ino, const std::string &out) {
	mkdir(out.c_str(), 0755); nd++;
	std::vector<ApfsDir::DirRec> list; if (!D->ListDirectory(list, ino)) return;
	for (auto &r : list) {
		ApfsDir::Inode in; if (!D->GetInode(in, r.file_id)) continue;
		std::string p = out + "/" + r.name;
		switch (in.mode & 0170000) {
		case 0040000: walk(r.file_id, p); break;
		case 0100000: {
			std::vector<uint8_t> o;
			if (in.bsd_flags & APFS_UF_COMPRESSED) { std::vector<uint8_t> a; D->GetAttribute(a, r.file_id, "com.apple.decmpfs"); if (!DecompressFile(*D, r.file_id, o, a)) { fprintf(stderr, "decomp %s\n", p.c_str()); continue; } }
			else { o.resize(in.ds_size); if (o.size() && !D->ReadFile(o.data(), r.file_id, 0, o.size())) { fprintf(stderr, "read %s\n", p.c_str()); continue; } }
			FILE *f = fopen(p.c_str(), "wb"); if (f) { fwrite(o.data(), 1, o.size(), f); fclose(f); chmod(p.c_str(), in.mode & 0777); nf++; } break; }
		default: fprintf(stderr, "skip %s mode %o\n", p.c_str(), in.mode);
		}
	}
}
int main(int argc, char **argv) {
	if (argc < 6) return 2;
	Device *d = Device::OpenDevice(argv[1]); if (!d) return 1;
	uint64_t off = strtoull(argv[2], 0, 0);
	ApfsContainer *c = new ApfsContainer(d, off, d->GetSize() - off, nullptr, 0, 0); if (!c->Init(0)) return 1;
	ApfsVolume *v = c->GetVolume(atoi(argv[3])); if (!v) return 1;
	ApfsDir dir(*v); D = &dir; uint64_t ino = 2; std::stringstream ss(argv[4]); std::string comp;
	while (std::getline(ss, comp, '/')) { if (comp.empty()) continue; ApfsDir::DirRec r; if (!dir.LookupName(r, ino, comp.c_str())) { fprintf(stderr, "no %s\n", comp.c_str()); return 1; } ino = r.file_id; }
	{ ApfsDir::Inode in; if (dir.GetInode(in, ino) && (in.mode & 0170000) == 0100000) { // a single file: argv[5] is the output file
		std::vector<uint8_t> o; o.resize(in.ds_size); if (o.size() && !dir.ReadFile(o.data(), ino, 0, o.size())) return 1;
		FILE *f = fopen(argv[5], "wb"); fwrite(o.data(), 1, o.size(), f); fclose(f); printf("1 file\n"); return 0; } }
	walk(ino, argv[5]); printf("%ld files %ld dirs\n", nf, nd); return 0;
}
