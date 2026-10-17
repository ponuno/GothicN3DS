// Gothic3DS-Baker.exe: a window where the player picks the Gothic folder; then it does everything
// (music too) and puts data.pak, music.pak and speech.pak beside this .exe.
//
// It runs the two command-line tools that lie next to it, and shows what they print:
//   g3ds-bake.exe extract  - the game's music files out of its archives
//   dmrender.exe           - the music to WAV
//   g3ds-bake.exe world    - data.pak (+ music.pak, speech.pak)
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

enum { ID_PATH = 1, ID_BROWSE, ID_MUSIC, ID_GFXPLUS, ID_START, ID_PROGRESS, ID_LOG, ID_STATUS, ID_VOICES, ID_DEBUG, ID_LOD, ID_SIMPLIFY, ID_SIMPLIFY_LARGE, ID_FAR_HQ, ID_TEX256, ID_LIGHTS, ID_NPC_LOD, ID_SUN, ID_OLD_WATER, ID_GRASS_REPORT, ID_GRASS, ID_CLEAR_WATER,
       ID_RECOMMENDED, ID_ADVANCED, ID_GAME, ID_HINT, ID_VOICE_IMA, ID_WATER_GRID };
const UINT WM_LOGLINE = WM_APP + 1; // lParam: new std::wstring*
const UINT WM_STATUS = WM_APP + 2;  // lParam: new std::wstring*
const UINT WM_FINISHED = WM_APP + 3; // wParam: 1 ok

HWND gWnd, gPath, gBrowse, gMusic, gGfxPlus, gLod, gSimplify, gSimplifyLarge, gFarHq, gTex256, gLights, gNpcLod, gSun, gOldWater, gClearWater, gGrassReport, gGrass, gVoices, gVoiceIma, gWaterGrid, gDebug, gStart, gProgress, gLog, gStatus;
HWND gGame, gRecommended, gAdvancedButton, gHint, gGameLabel;
bool gAdvanced = false;
// Every option box with the value the recommended settings give it (filled in wWinMain).
struct OptionBox {
	HWND* box;
	bool recommended;
};
std::vector<OptionBox> gOptions;
// What the player picked, for the worker thread.
struct Choices {
	bool music, gfxPlus, lod, simplify, simplifyLarge, farHq, tex256, lights, npcLod, sun, oldWater, clearWater, grassReport, grass, voices, voiceIma, waterGrid, debug;
};
std::wstring gExeDir; // with a trailing backslash
std::atomic<HANDLE> gChild(nullptr);
std::atomic<bool> gRunning(false), gAbort(false);
std::thread gWorker;
HANDLE gLogFile = INVALID_HANDLE_VALUE;

std::wstring utf8ToWide(const std::string& s) {
	if (s.empty()) return std::wstring();
	int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
	if (n <= 0) n = 0;
	std::wstring w(size_t(n), L'\0');
	if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), &w[0], n);
	return w;
}

std::wstring shortPath(const std::wstring& p) {
	wchar_t buf[MAX_PATH * 2];
	const DWORD n = GetShortPathNameW(p.c_str(), buf, DWORD(sizeof(buf) / sizeof(buf[0])));
	// (The tools read their arguments in the system's ANSI code page: an 8.3 name has only plain letters.)
	if (n == 0 || n >= sizeof(buf) / sizeof(buf[0])) return p;
	return std::wstring(buf, n);
}

bool exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// Extra command-line options for the baker from "bake_args.txt" beside this program (one line or several, as typed
// after g3ds-bake: e.g. --probe -843 11 -1080 --ao-extra 500000). Empty if there is no such file.
std::wstring extraArgs(const std::wstring& dir) {
	FILE* f = _wfopen((dir + L"bake_args.txt").c_str(), L"rb");
	if (!f) return L"";
	std::string text;
	char buf[512];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && text.size() < 4096) text.append(buf, n);
	fclose(f);
	for (char& c : text)
		if (c == '\r' || c == '\n' || c == '\t') c = ' ';
	if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
		text.erase(0, 3);
	return text.empty() ? L"" : L" " + utf8ToWide(text);
}

void say(const std::wstring& line) { PostMessageW(gWnd, WM_LOGLINE, 0, LPARAM(new std::wstring(line))); }
void status(const std::wstring& text) { PostMessageW(gWnd, WM_STATUS, 0, LPARAM(new std::wstring(text))); }

void logRaw(const std::string& bytes) {
	if (gLogFile == INVALID_HANDLE_VALUE) return;
	DWORD done;
	WriteFile(gLogFile, bytes.data(), DWORD(bytes.size()), &done, nullptr);
}

bool anyFile(const std::wstring& dir, const wchar_t* pattern) {
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW((dir + pattern).c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return false;
	FindClose(h);
	return true;
}

void removeTree(const std::wstring& dir) {
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			const std::wstring name = fd.cFileName;
			if (name == L"." || name == L"..") continue;
			const std::wstring full = dir + L"\\" + name;
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) removeTree(full);
			else {
				SetFileAttributesW(full.c_str(), FILE_ATTRIBUTE_NORMAL);
				DeleteFileW(full.c_str());
			}
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}
	RemoveDirectoryW(dir.c_str());
}

// Runs `cmd` with its output into the log window; true if it ended with 0.
bool run(const std::wstring& cmd) {
	SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
	HANDLE rd = nullptr, wr = nullptr;
	if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
	SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
	STARTUPINFOW si = {};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;
	si.hStdOutput = wr;
	si.hStdError = wr;
	si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
	PROCESS_INFORMATION pi = {};
	std::vector<wchar_t> line(cmd.begin(), cmd.end());
	line.push_back(0);
	say(L"> " + cmd);
	const BOOL ok = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, gExeDir.c_str(), &si, &pi);
	CloseHandle(wr);
	if (!ok) {
		CloseHandle(rd);
		say(L"Could not start the program (error " + std::to_wstring(GetLastError()) + L")");
		return false;
	}
	gChild = pi.hProcess;
	std::string pending;
	char buf[4096];
	DWORD got;
	while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got > 0) {
		logRaw(std::string(buf, got));
		pending.append(buf, got);
		size_t cut;
		while ((cut = pending.find_first_of("\r\n")) != std::string::npos) {
			const std::string one = pending.substr(0, cut);
			pending.erase(0, cut + 1);
			if (!one.empty()) say(utf8ToWide(one));
		}
	}
	if (!pending.empty()) say(utf8ToWide(pending));
	CloseHandle(rd);
	WaitForSingleObject(pi.hProcess, INFINITE);
	DWORD code = 1;
	GetExitCodeProcess(pi.hProcess, &code);
	gChild = nullptr;
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return code == 0;
}

void pipeline(std::wstring game, Choices c) {
	const bool music = c.music, gfxPlus = c.gfxPlus, lod = c.lod, simplify = c.simplify, simplifyLarge = c.simplifyLarge, farHq = c.farHq, tex256 = c.tex256, lights = c.lights, npcLod = c.npcLod, sun = c.sun,
	           oldWater = c.oldWater, clearWater = c.clearWater, grassReport = c.grassReport, grass = c.grass, voices = c.voices, voiceIma = c.voiceIma, waterGrid = c.waterGrid, debug = c.debug;
	const std::wstring bake = L"\"" + gExeDir + L"g3ds-bake.exe\"";
	const std::wstring render = L"\"" + gExeDir + L"dmrender.exe\"";
	const std::wstring out = gExeDir; // everything lands beside the program
	const std::wstring outShort = shortPath(out);
	const std::wstring gameShort = shortPath(game);
	const std::wstring musicSrc = out + L"_music_src", wavs = out + L"_music_wavs";
	bool ok = true;
	bool haveMusic = false;
	if (music && exists(gExeDir + L"dmrender.exe")) {
		status(L"Music: extracting the files from the game archives...");
		CreateDirectoryW(musicSrc.c_str(), nullptr);
		CreateDirectoryW(wavs.c_str(), nullptr);
		if (run(bake + L" extract --game \"" + gameShort + L"\" -o \"" + shortPath(musicSrc) + L"\"") && !gAbort) {
			status(L"Music: rendering to WAV (the longest step, 10-30 minutes)...");
			if (!run(render + L" \"" + shortPath(musicSrc) + L"\" \"" + shortPath(wavs) + L"\"") && !gAbort)
				say(L"Music rendering failed, continuing without music.");
			haveMusic = anyFile(wavs, L"\\*.wav");
		} else if (!gAbort) {
			say(L"No music files found, continuing without music.");
		}
	} else if (music) {
		say(L"dmrender.exe is not next to this program: no music.");
	}
	if (!gAbort) {
		status(L"Baking the game data (data.pak)... a few minutes, the window may look idle.");
		std::wstring cmd = bake + L" world --game \"" + gameShort + L"\" -o \"" + outShort + L"data.pak\" --no-script-check";
		if (haveMusic) cmd += L" --music-dir \"" + shortPath(wavs) + L"\"";
		// Graphics+ in the data: corner shading, swaying plants, far layer and visibility table (switched on in the game's menu, Graphics -> Graphics+).
		// Swaying plants are always baked (the game's menu switches them on: Graphics+ -> Foliage sway), costing about 10 % more vertices.
		cmd += L" --wind";
		if (gfxPlus) cmd += L" --ao low --far-lod --vis";
		// A finer far view (Graphics+ -> Far layer): more triangles and finer cells, about 2.5 times the memory of the default.
		// Sharper textures: the longest edge 256 instead of 128 (about 4 times the texture memory and a bigger data.pak).
		if (tex256) cmd += L" --max-tex 256";
		if (lights) cmd += L" --lights";
		// The sun's shadow on the ground: a 4-bit mask per grid cell (menu: Graphics+ -> Sun shadows), no extra triangles.
		if (sun) cmd += L" --sun-shadow";
		if (grass) cmd += L" --grass"; // (the new grass: in the game's menu Graphics+ -> Grass, off until chosen)
		// The game's own water texture instead of the one made by the baker (wind ripples).
		if (oldWater) cmd += L" --water-texture original";
		// Clearer water: the bottom and the shore show through the baker's water (no effect on the game's own texture).
		else if (clearWater) cmd += L" --water-clear";
		// For the grass tests: where new grass could grow (grass_<world>.txt, grass_<world>.png beside the program).
		// It only looks; data.pak is the same with or without it.
		if (grassReport) cmd += L" --grass-report \"" + outShort + L".\""; // (".": a backslash before the quote would escape it)
		// A coarse body for each character, shown far away and swapped for the full one when near.
		if (npcLod) cmd += L" --npc-lod";
		if (gfxPlus && farHq) cmd += L" --far-tris 200 --far-cell 2";
		// Simplified stand-ins for far chunks (menu: Graphics+ -> Far geometry): about half the triangles there.
		if (lod) cmd += L" --lod";
		if (simplify || simplifyLarge) cmd += L" --simplify";
		// A second, harder pass over the large objects (the heaviest ones in the busiest scenes).
		if (simplifyLarge) cmd += L" --simplify-ratio-large 0.4";
		if (!voices) cmd += L" --no-voices";
		// The voices as IMA ADPCM: a quarter the size of the recordings' 16-bit samples (the game plays both).
		else if (voiceIma) cmd += L" --voice-codec ima";
		// The water cut along the shores into a 2 m grid: waves lift it, the shore is coloured by depth (the recommended setting for Gothic 1).
		if (waterGrid) cmd += L" --water-grid 2";
		if (debug) cmd += L" --debug-report \"" + outShort + L"bake_debug.txt\"";
		const std::wstring extra = extraArgs(gExeDir);
		if (!extra.empty()) {
			say(L"Extra options from bake_args.txt:" + extra);
			cmd += extra;
		}
		ok = run(cmd);
	}
	// (The temporary music files are big and useless afterwards.)
	removeTree(musicSrc);
	removeTree(wavs);
	if (gAbort) {
		PostMessageW(gWnd, WM_FINISHED, 0, 0);
		return;
	}
	if (ok) {
		std::wstring made;
		for (const wchar_t* f : {L"data.pak", L"music.pak", L"speech.pak", L"video.pak", L"bake_debug.txt"})
			if (exists(out + f)) made += std::wstring(L"\r\n  ") + f;
		if (grassReport && anyFile(out, L"grass_*.png")) made += L"\r\n  grass_<world>.txt, grass_<world>.png, grass_<world>_mask.png";
		say(L"Done. Created:" + made);
		status(L"Done! The files are next to this program. Copy them to the SD card, folder 3ds\\gothic3ds\\");
	} else {
		status(L"Failed. See the log below and bake.log next to this program.");
	}
	PostMessageW(gWnd, WM_FINISHED, ok ? 1 : 0, 0);
}

void appendLog(const std::wstring& line) {
	std::wstring text = line + L"\r\n";
	SendMessageW(gLog, EM_SETSEL, WPARAM(-1), LPARAM(-1));
	SendMessageW(gLog, EM_REPLACESEL, FALSE, LPARAM(text.c_str()));
}

std::wstring browseForFolder() {
	std::wstring result;
	BROWSEINFOW bi = {};
	bi.hwndOwner = gWnd;
	bi.lpszTitle = L"Select the Gothic game folder (the one with Data and _work)";
	bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
	LPITEMIDLIST id = SHBrowseForFolderW(&bi);
	if (id) {
		wchar_t path[MAX_PATH * 2];
		if (SHGetPathFromIDListW(id, path)) result = path;
		CoTaskMemFree(id);
	}
	return result;
}

std::wstring guessGame() {
	const wchar_t* tries[] = {L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Gothic", L"C:\\Program Files\\Steam\\steamapps\\common\\Gothic",
	                          L"C:\\GOG Games\\Gothic", L"C:\\Program Files (x86)\\GOG Galaxy\\Games\\Gothic", L"C:\\Program Files (x86)\\JoWooD\\Gothic",
	                          L"D:\\SteamLibrary\\steamapps\\common\\Gothic", L"D:\\Games\\Gothic"};
	for (const wchar_t* t : tries)
		if (exists(std::wstring(t) + L"\\Data")) return t;
	return L"";
}

std::wstring editText(HWND h) {
	const int n = GetWindowTextLengthW(h);
	std::wstring s(size_t(n) + 1, L'\0');
	GetWindowTextW(h, &s[0], n + 1);
	s.resize(size_t(n));
	return s;
}

void setAllEnabled(bool on) {
	EnableWindow(gStart, on);
	EnableWindow(gBrowse, on);
	EnableWindow(gPath, on);
	EnableWindow(gGame, on);
	EnableWindow(gRecommended, on);
	for (const OptionBox& o : gOptions) EnableWindow(*o.box, on);
}

bool checked(HWND h) { return SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED; }

void setHint(bool recommended) {
	SetWindowTextW(gHint, recommended ? L"Recommended settings for Gothic are selected. Everything can be changed in Advanced settings."
	                                  : L"Custom settings. \"Recommended settings\" puts the best ones back.");
}

// Puts every option to its recommended value and points the baker at the Gothic folder if it can be found.
void applyRecommended() {
	for (const OptionBox& o : gOptions) SendMessageW(*o.box, BM_SETCHECK, o.recommended ? BST_CHECKED : BST_UNCHECKED, 0);
	SendMessageW(gGame, CB_SETCURSEL, 0, 0);
	std::wstring game = editText(gPath);
	if (game.empty() || !exists(game + L"\\Data")) {
		const std::wstring found = guessGame();
		if (!found.empty()) SetWindowTextW(gPath, found.c_str());
		else MessageBoxW(gWnd, L"Gothic was not found in the usual places. Press Browse and select the folder where Gothic is installed (it contains Data and _work).", L"Gothic 3DS", MB_OK | MB_ICONINFORMATION);
	}
	setHint(true);
}

// The window's height follows the advanced settings: shown, they push the progress bar and the log down.
void layout() {
	const int kPanelTop = 160, kPanelHeight = 9 * 22 + 8;
	for (const OptionBox& o : gOptions) ShowWindow(*o.box, gAdvanced ? SW_SHOW : SW_HIDE);
	SetWindowTextW(gAdvancedButton, gAdvanced ? L"Hide advanced settings" : L"Advanced settings...");
	const int y = kPanelTop + (gAdvanced ? kPanelHeight : 0);
	SetWindowPos(gProgress, nullptr, 12, y, 688, 14, SWP_NOZORDER);
	SetWindowPos(gStatus, nullptr, 12, y + 20, 688, 18, SWP_NOZORDER);
	SetWindowPos(gLog, nullptr, 12, y + 42, 688, 250, SWP_NOZORDER);
	RECT r = {0, 0, 712, y + 42 + 250 + 12};
	AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
	SetWindowPos(gWnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
}

void startPressed() {
	if (gRunning) return;
	std::wstring game = editText(gPath);
	while (!game.empty() && (game.back() == L'\\' || game.back() == L'/' || game.back() == L' ')) game.pop_back();
	if (game.empty() || !exists(game + L"\\Data")) {
		MessageBoxW(gWnd, L"This folder has no Data subfolder. Select the folder where Gothic is installed (it contains Data and _work).", L"Gothic 3DS",
		            MB_OK | MB_ICONWARNING);
		return;
	}
	if (!exists(gExeDir + L"g3ds-bake.exe")) {
		MessageBoxW(gWnd, L"g3ds-bake.exe is not next to this program. Put all the .exe files in the same folder.", L"Gothic 3DS", MB_OK | MB_ICONERROR);
		return;
	}
	Choices c = {};
	c.music = checked(gMusic);
	c.gfxPlus = checked(gGfxPlus);
	c.lod = checked(gLod);
	c.simplify = checked(gSimplify);
	c.simplifyLarge = checked(gSimplifyLarge);
	c.farHq = checked(gFarHq);
	c.tex256 = checked(gTex256);
	c.lights = checked(gLights);
	c.npcLod = checked(gNpcLod);
	c.sun = checked(gSun);
	c.oldWater = checked(gOldWater);
	c.clearWater = checked(gClearWater);
	c.grassReport = checked(gGrassReport);
	c.grass = checked(gGrass);
	c.voices = checked(gVoices);
	c.voiceIma = checked(gVoiceIma);
	c.waterGrid = checked(gWaterGrid);
	c.debug = checked(gDebug);
	if (gWorker.joinable()) gWorker.join();
	if (gLogFile != INVALID_HANDLE_VALUE) CloseHandle(gLogFile);
	gLogFile = CreateFileW((gExeDir + L"bake.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	SetWindowTextW(gLog, L"");
	gAbort = false;
	gRunning = true;
	setAllEnabled(false);
	SendMessageW(gProgress, PBM_SETMARQUEE, TRUE, 40);
	gWorker = std::thread(pipeline, game, c);
}

LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
	switch (m) {
	case WM_COMMAND: {
		const int id = LOWORD(w), code = HIWORD(w);
		if (id == ID_BROWSE) {
			const std::wstring p = browseForFolder();
			if (!p.empty()) SetWindowTextW(gPath, p.c_str());
		} else if (id == ID_START) {
			startPressed();
		} else if (id == ID_RECOMMENDED) {
			applyRecommended();
		} else if (id == ID_ADVANCED) {
			gAdvanced = !gAdvanced;
			layout();
		} else if (id == ID_GAME && code == CBN_SELCHANGE) {
			// Only Gothic 1 so far; Gothic II and Archolos come later.
			if (SendMessageW(gGame, CB_GETCURSEL, 0, 0) != 0) {
				SendMessageW(gGame, CB_SETCURSEL, 0, 0);
				MessageBoxW(h, L"Only Gothic (1) can be baked for now. Gothic II and Archolos are coming later.", L"Gothic 3DS", MB_OK | MB_ICONINFORMATION);
			}
		} else if (code == BN_CLICKED) {
			for (const OptionBox& o : gOptions)
				if (GetDlgCtrlID(*o.box) == id) {
					bool all = true;
					for (const OptionBox& p : gOptions) all = all && checked(*p.box) == p.recommended;
					setHint(all);
				}
		}
		return 0;
	}
	case WM_LOGLINE: {
		std::wstring* s = reinterpret_cast<std::wstring*>(l);
		appendLog(*s);
		delete s;
		return 0;
	}
	case WM_STATUS: {
		std::wstring* s = reinterpret_cast<std::wstring*>(l);
		SetWindowTextW(gStatus, s->c_str());
		delete s;
		return 0;
	}
	case WM_FINISHED:
		gRunning = false;
		SendMessageW(gProgress, PBM_SETMARQUEE, FALSE, 0);
		setAllEnabled(true);
		if (w) MessageBoxW(h, L"Done! data.pak (and music.pak, speech.pak, video.pak) are next to this program.\r\nCopy them to the SD card, folder 3ds\\gothic3ds\\", L"Gothic 3DS", MB_OK | MB_ICONINFORMATION);
		return 0;
	case WM_CLOSE:
		if (gRunning && MessageBoxW(h, L"Baking is still running. Cancel it?", L"Gothic 3DS", MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
		gAbort = true;
		if (HANDLE c = gChild.load()) TerminateProcess(c, 1);
		DestroyWindow(h);
		return 0;
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(h, m, w, l);
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
	CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_PROGRESS_CLASS};
	InitCommonControlsEx(&icc);
	wchar_t exe[MAX_PATH * 2];
	GetModuleFileNameW(nullptr, exe, DWORD(sizeof(exe) / sizeof(exe[0])));
	gExeDir = exe;
	gExeDir.resize(gExeDir.find_last_of(L'\\') + 1);

	WNDCLASSW wc = {};
	wc.lpfnWndProc = proc;
	wc.hInstance = inst;
	wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
	wc.lpszClassName = L"G3dsBaker";
	RegisterClassW(&wc);
	gWnd = CreateWindowExW(0, wc.lpszClassName, L"Gothic 3DS: data baker", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
	                       CW_USEDEFAULT, CW_USEDEFAULT, 720, 520, nullptr, nullptr, inst, nullptr);
	const HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
	const auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, DWORD ex = 0) {
		HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, gWnd, reinterpret_cast<HMENU>(INT_PTR(id)), inst, nullptr);
		SendMessageW(c, WM_SETFONT, WPARAM(font), TRUE);
		return c;
	};
	// The game (only Gothic 1 so far), its folder, the three buttons.
	gGameLabel = make(L"STATIC", L"Game:", 0, 12, 14, 44, 18, 0);
	gGame = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 58, 10, 300, 120, ID_GAME);
	SendMessageW(gGame, CB_ADDSTRING, 0, LPARAM(L"Gothic"));
	SendMessageW(gGame, CB_ADDSTRING, 0, LPARAM(L"Gothic II (coming later)"));
	SendMessageW(gGame, CB_ADDSTRING, 0, LPARAM(L"Archolos (coming later)"));
	SendMessageW(gGame, CB_SETCURSEL, 0, 0);
	make(L"STATIC", L"Gothic game folder (the one with Data and _work):", 0, 12, 42, 680, 18, 0);
	gPath = make(L"EDIT", guessGame().c_str(), ES_AUTOHSCROLL, 12, 62, 590, 24, ID_PATH, WS_EX_CLIENTEDGE);
	gBrowse = make(L"BUTTON", L"Browse...", BS_PUSHBUTTON, 610, 61, 90, 26, ID_BROWSE);
	gRecommended = make(L"BUTTON", L"Recommended settings", BS_PUSHBUTTON, 12, 98, 190, 28, ID_RECOMMENDED);
	gAdvancedButton = make(L"BUTTON", L"Advanced settings...", BS_PUSHBUTTON, 210, 98, 190, 28, ID_ADVANCED);
	gStart = make(L"BUTTON", L"Bake", BS_DEFPUSHBUTTON, 610, 98, 90, 28, ID_START);
	gHint = make(L"STATIC", L"", 0, 12, 134, 688, 18, ID_HINT);

	// The advanced settings: two columns of nine, hidden until asked for. The values are the recommended ones.
	const int kLeft = 12, kRight = 362, kWidth = 340, kTop = 160, kRow = 22;
	int row[2] = {0, 0};
	const auto option = [&](HWND& box, int column, const wchar_t* text, int id, bool recommended) {
		box = make(L"BUTTON", text, BS_AUTOCHECKBOX, column ? kRight : kLeft, kTop + kRow * row[column]++, kWidth, kRow, id);
		SendMessageW(box, BM_SETCHECK, recommended ? BST_CHECKED : BST_UNCHECKED, 0);
		gOptions.push_back({&box, recommended});
	};
	option(gMusic, 0, L"Music (10-30 minutes more)", ID_MUSIC, true);
	option(gVoices, 0, L"Voices (speech.pak)", ID_VOICES, true);
	option(gVoiceIma, 0, L"Compact voices: IMA ADPCM, a quarter the size", ID_VOICE_IMA, true);
	option(gGfxPlus, 0, L"Graphics+: shading, far layer, visibility", ID_GFXPLUS, true);
	option(gFarHq, 0, L"Finer far view (needs Graphics+; ~2.5x memory)", ID_FAR_HQ, true);
	option(gTex256, 0, L"Sharper textures: 256 px (bigger data.pak)", ID_TEX256, true);
	option(gLights, 0, L"Baked light from torches and fires", ID_LIGHTS, true);
	option(gNpcLod, 0, L"Simplified far characters (coarse body)", ID_NPC_LOD, true);
	option(gSun, 0, L"Sun shadows on the ground", ID_SUN, true);
	option(gWaterGrid, 1, L"Finer water: waves, shore colour (water grid 2)", ID_WATER_GRID, true);
	option(gClearWater, 1, L"Clearer water: see the bottom and the shore", ID_CLEAR_WATER, true);
	option(gOldWater, 1, L"Original water texture, not the baker's ripples", ID_OLD_WATER, false);
	option(gGrass, 1, L"New grass on the meadows (about 2.5 MB)", ID_GRASS, true);
	option(gLod, 1, L"Simplified far geometry (+10% size)", ID_LOD, false);
	option(gSimplify, 1, L"Simplify all geometry (faster, less detail)", ID_SIMPLIFY, false);
	option(gSimplifyLarge, 1, L"Large objects simplified harder", ID_SIMPLIFY_LARGE, false);
	option(gGrassReport, 1, L"Grass report for testing (grass_<world>.txt)", ID_GRASS_REPORT, false);
	option(gDebug, 1, L"Debug report (bake_debug.txt)", ID_DEBUG, false);

	gProgress = make(PROGRESS_CLASSW, L"", PBS_MARQUEE, 12, 160, 688, 14, ID_PROGRESS);
	gStatus = make(L"STATIC", L"Select the folder and press \"Bake\". The files will appear next to this program.", 0, 12, 180, 688, 18, ID_STATUS);
	gLog = make(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 12, 202, 688, 250, ID_LOG, WS_EX_CLIENTEDGE);
	SendMessageW(gLog, EM_SETLIMITTEXT, 0x7FFFFFF, 0);
	setHint(true);
	layout();

	ShowWindow(gWnd, show);
	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
	if (gWorker.joinable()) gWorker.join();
	return 0;
}
