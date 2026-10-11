// dmrender: renders the game's DirectMusic themes (.sgt, with their styles
// and DLS banks) to WAV files, for the baker's --music-dir (music.pak).
// Uses GothicKit's dmusic (https://github.com/GothicKit/dmusic).
//
//   dmrender MUSIC_DIR OUT_DIR [--rate 44100] [--cycles 1] [--tail 2.0] [THEME.sgt ...]
//
// MUSIC_DIR holds the game's music files (extracted from Music.vdf, or
// _work/Data/Music loose): .sgt, .sty, .dls, in any letter case. Without
// theme names every .sgt there is rendered. Each comes out as OUT_DIR/<stem>.wav
// (16-bit stereo), `cycles` times the segment's length plus `tail` seconds.
#include <dmusic.h>

#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static char gDir[1024];

typedef struct {
	char* name; // file name only
	char* path; // full path
} FileEntry;
static FileEntry* gFiles;
static size_t gFileCount, gFileCap;

static int equalNoCase(const char* a, const char* b) {
	for (; *a && *b; ++a, ++b)
		if (tolower((unsigned char) *a) != tolower((unsigned char) *b)) return 0;
	return *a == *b;
}

// Every file under `dir`, subfolders too: the game keeps its themes in several
// (orchestra, dungeon, ...) and a theme may name a style or bank from another.
static void scan(const char* dir, int depth) {
	DIR* d = opendir(dir);
	if (!d) return;
	for (struct dirent* e; (e = readdir(d));) {
		if (e->d_name[0] == '.') continue;
		char path[2048];
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		struct stat st;
		if (stat(path, &st) != 0) continue;
		if (S_ISDIR(st.st_mode)) {
			if (depth < 8) scan(path, depth + 1);
		} else if (S_ISREG(st.st_mode)) {
			if (gFileCount == gFileCap) {
				gFileCap = gFileCap ? gFileCap * 2 : 256;
				gFiles = realloc(gFiles, gFileCap * sizeof(FileEntry));
			}
			gFiles[gFileCount].name = strdup(e->d_name);
			gFiles[gFileCount].path = strdup(path);
			++gFileCount;
		}
	}
	closedir(d);
}

static void* resolve(void* ctx, char const* name, size_t* len) {
	(void) ctx;
	// (The names may carry a folder: only the file name counts.)
	const char* base = name;
	for (const char* p = name; *p; ++p)
		if (*p == '/' || *p == '\\') base = p + 1;
	for (size_t i = 0; i < gFileCount; ++i) {
		if (!equalNoCase(gFiles[i].name, base)) continue;
		FILE* f = fopen(gFiles[i].path, "rb");
		if (!f) return NULL;
		fseek(f, 0, SEEK_END);
		long size = ftell(f);
		fseek(f, 0, SEEK_SET);
		void* bytes = NULL;
		if (size > 0 && (bytes = malloc((size_t) size))) *len = fread(bytes, 1, (size_t) size, f);
		fclose(f);
		return bytes;
	}
	return NULL;
}

static void put32(FILE* f, uint32_t v) {
	for (int i = 0; i < 4; ++i) fputc((int) ((v >> (8 * i)) & 0xFF), f);
}
static void put16(FILE* f, uint16_t v) {
	fputc(v & 0xFF, f);
	fputc(v >> 8, f);
}

static int writeWav(const char* path, const float* pcm, size_t frames, uint32_t rate) {
	FILE* f = fopen(path, "wb");
	if (!f) return 0;
	fwrite("RIFF", 1, 4, f);
	put32(f, (uint32_t) (36 + frames * 4));
	fwrite("WAVEfmt ", 1, 8, f);
	put32(f, 16);
	put16(f, 1);
	put16(f, 2);
	put32(f, rate);
	put32(f, rate * 4);
	put16(f, 4);
	put16(f, 16);
	fwrite("data", 1, 4, f);
	put32(f, (uint32_t) (frames * 4));
	for (size_t i = 0; i < frames * 2; ++i) {
		float v = pcm[i];
		if (!(v == v)) v = 0.0f;
		v = v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v;
		put16(f, (uint16_t) (int16_t) lrintf(v * 32767.0f));
	}
	fclose(f);
	return 1;
}

static int renderOne(DmLoader* loader, const char* theme, const char* outDir, uint32_t rate, double cycles, double tail) {
	DmSegment* segment = NULL;
	if (DmLoader_getSegment(loader, theme, &segment) != DmResult_SUCCESS) {
		fprintf(stderr, "%s: cannot load (the segment, its style or its DLS bank is missing?)\n", theme);
		return 0;
	}
	DmPerformance* perf = NULL;
	if (DmPerformance_create(&perf, rate) != DmResult_SUCCESS) {
		DmSegment_release(segment);
		return 0;
	}
	int ok = 0;
	if (DmPerformance_playSegment(perf, segment, DmTiming_INSTANT) == DmResult_SUCCESS) {
		double seconds = DmSegment_getLength(segment) * cycles + tail;
		if (seconds < 1.0) seconds = 1.0;
		if (seconds > 600.0) seconds = 600.0;
		const size_t frames = (size_t) (seconds * rate);
		float* pcm = calloc(frames * 2, sizeof(float));
		if (pcm) {
			// In pieces: the performance advances its clock by what is asked.
			for (size_t at = 0; at < frames;) {
				size_t n = frames - at < 4096 ? frames - at : 4096;
				DmPerformance_renderPcm(perf, pcm + at * 2, n * 2, DmRender_FLOAT | DmRender_STEREO);
				at += n;
			}
			char stem[256];
			snprintf(stem, sizeof(stem), "%s", theme);
			char* dot = strrchr(stem, '.');
			if (dot) *dot = 0;
			char path[2048];
			snprintf(path, sizeof(path), "%s/%s.wav", outDir, stem);
			ok = writeWav(path, pcm, frames, rate);
			if (ok) printf("%s -> %s (%.1f s)\n", theme, path, seconds);
			else fprintf(stderr, "%s: cannot write %s\n", theme, path);
			free(pcm);
		}
	}
	DmPerformance_release(perf);
	DmSegment_release(segment);
	return ok;
}

int main(int argc, char** argv) {
	if (argc < 3) {
		fprintf(stderr, "usage: dmrender MUSIC_DIR OUT_DIR [--rate HZ] [--cycles N] [--tail SECONDS] [THEME.sgt ...]\n");
		return 2;
	}
	snprintf(gDir, sizeof(gDir), "%s", argv[1]);
	const char* outDir = argv[2];
	uint32_t rate = 44100;
	double cycles = 1.0, tail = 2.0;
	const char* names[4096];
	int count = 0;
	for (int i = 3; i < argc; ++i) {
		if (!strcmp(argv[i], "--rate") && i + 1 < argc) rate = (uint32_t) strtoul(argv[++i], NULL, 10);
		else if (!strcmp(argv[i], "--cycles") && i + 1 < argc) cycles = atof(argv[++i]);
		else if (!strcmp(argv[i], "--tail") && i + 1 < argc) tail = atof(argv[++i]);
		else if (count < 4096) names[count++] = argv[i];
	}
	scan(gDir, 0);
	if (gFileCount == 0) {
		fprintf(stderr, "no files in %s\n", gDir);
		return 1;
	}
	mkdir(outDir, 0755);
	Dm_setLoggerDefault(DmLogLevel_WARN);

	DmLoader* loader = NULL;
	if (DmLoader_create(&loader, DmLoader_DEFAULT | DmLoader_DOWNLOAD) != DmResult_SUCCESS) return 1;
	DmLoader_addResolver(loader, resolve, NULL);

	int done = 0, failed = 0;
	if (count == 0) {
		for (size_t i = 0; i < gFileCount && count < 4096; ++i) {
			const size_t n = strlen(gFiles[i].name);
			if (n > 4 && equalNoCase(gFiles[i].name + n - 4, ".sgt")) names[count++] = gFiles[i].name;
		}
		for (int i = 0; i < count; ++i) (renderOne(loader, names[i], outDir, rate, cycles, tail) ? ++done : ++failed);
	} else {
		for (int i = 0; i < count; ++i) (renderOne(loader, names[i], outDir, rate, cycles, tail) ? ++done : ++failed);
	}
	printf("dmrender: %d rendered, %d failed\n", done, failed);
	DmLoader_release(loader);
	return failed ? 1 : 0;
}
