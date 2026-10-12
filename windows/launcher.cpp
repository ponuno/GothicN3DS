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
#include <string>
#include <thread>
#include <vector>

namespace {

enum { ID_PATH = 1, ID_BROWSE, ID_MUSIC, ID_GFXPLUS, ID_START, ID_PROGRESS, ID_LOG, ID_STATUS, ID_VOICES, ID_DEBUG, ID_LOD, ID_SIMPLIFY };
const UINT WM_LOGLINE = WM_APP + 1; // lParam: new std::wstring*
const UINT WM_STATUS = WM_APP + 2;  // lParam: new std::wstring*
const UINT WM_FINISHED = WM_APP + 3; // wParam: 1 ok

HWND gWnd, gPath, gBrowse, gMusic, gGfxPlus, gLod, gSimplify, gVoices, gDebug, gStart, gProgress, gLog, gStatus;
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

void pipeline(std::wstring game, bool music, bool gfxPlus, bool lod, bool simplify, bool voices, bool debug) {
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
		if (gfxPlus) cmd += L" --ao low --wind --far-lod --vis";
		// Simplified stand-ins for far chunks (menu: Graphics+ -> Far geometry): about half the triangles there.
		if (lod) cmd += L" --lod";
		if (simplify) cmd += L" --simplify";
		if (!voices) cmd += L" --no-voices";
		if (debug) cmd += L" --debug-report \"" + outShort + L"bake_debug.txt\"";
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
		for (const wchar_t* f : {L"data.pak", L"music.pak", L"speech.pak", L"bake_debug.txt"})
			if (exists(out + f)) made += std::wstring(L"\r\n  ") + f;
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
		MessageBoxW(gWnd, L"g3ds-bake.exe is not next to this program. Put all three .exe files in the same folder.", L"Gothic 3DS", MB_OK | MB_ICONERROR);
		return;
	}
	const bool music = SendMessageW(gMusic, BM_GETCHECK, 0, 0) == BST_CHECKED;
	const bool gfxPlus = SendMessageW(gGfxPlus, BM_GETCHECK, 0, 0) == BST_CHECKED;
	const bool lod = SendMessageW(gLod, BM_GETCHECK, 0, 0) == BST_CHECKED;
	const bool simplify = SendMessageW(gSimplify, BM_GETCHECK, 0, 0) == BST_CHECKED;
	const bool voices = SendMessageW(gVoices, BM_GETCHECK, 0, 0) == BST_CHECKED;
	const bool debug = SendMessageW(gDebug, BM_GETCHECK, 0, 0) == BST_CHECKED;
	if (gWorker.joinable()) gWorker.join();
	if (gLogFile != INVALID_HANDLE_VALUE) CloseHandle(gLogFile);
	gLogFile = CreateFileW((gExeDir + L"bake.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	SetWindowTextW(gLog, L"");
	gAbort = false;
	gRunning = true;
	EnableWindow(gStart, FALSE);
	EnableWindow(gBrowse, FALSE);
	EnableWindow(gPath, FALSE);
	EnableWindow(gMusic, FALSE);
	EnableWindow(gGfxPlus, FALSE);
	EnableWindow(gLod, FALSE);
	EnableWindow(gSimplify, FALSE);
	EnableWindow(gVoices, FALSE);
	EnableWindow(gDebug, FALSE);
	SendMessageW(gProgress, PBM_SETMARQUEE, TRUE, 40);
	gWorker = std::thread(pipeline, game, music, gfxPlus, lod, simplify, voices, debug);
}

LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
	switch (m) {
	case WM_COMMAND:
		if (LOWORD(w) == ID_BROWSE) {
			const std::wstring p = browseForFolder();
			if (!p.empty()) SetWindowTextW(gPath, p.c_str());
		} else if (LOWORD(w) == ID_START) {
			startPressed();
		}
		return 0;
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
		EnableWindow(gStart, TRUE);
		EnableWindow(gBrowse, TRUE);
		EnableWindow(gPath, TRUE);
		EnableWindow(gMusic, TRUE);
		EnableWindow(gGfxPlus, TRUE);
		EnableWindow(gLod, TRUE);
		EnableWindow(gSimplify, TRUE);
		EnableWindow(gVoices, TRUE);
		EnableWindow(gDebug, TRUE);
		if (w) MessageBoxW(h, L"Done! data.pak (and music.pak, speech.pak) are next to this program.\r\nCopy them to the SD card, folder 3ds\\gothic3ds\\", L"Gothic 3DS", MB_OK | MB_ICONINFORMATION);
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
	make(L"STATIC", L"Gothic game folder (the one with Data and _work):", 0, 12, 10, 680, 18, 0);
	gPath = make(L"EDIT", guessGame().c_str(), ES_AUTOHSCROLL, 12, 30, 590, 24, ID_PATH, WS_EX_CLIENTEDGE);
	gBrowse = make(L"BUTTON", L"Browse...", BS_PUSHBUTTON, 610, 29, 90, 26, ID_BROWSE);
	gMusic = make(L"BUTTON", L"Also bake the music (10-30 minutes more)", BS_AUTOCHECKBOX, 12, 62, 290, 22, ID_MUSIC);
	SendMessageW(gMusic, BM_SETCHECK, BST_CHECKED, 0);
	gGfxPlus = make(L"BUTTON", L"Graphics+: shading, plants, far layer, visibility", BS_AUTOCHECKBOX, 306, 62, 296, 22, ID_GFXPLUS);
	gVoices = make(L"BUTTON", L"Bake the voices (speech.pak)", BS_AUTOCHECKBOX, 12, 84, 290, 22, ID_VOICES);
	SendMessageW(gVoices, BM_SETCHECK, BST_CHECKED, 0);
	gDebug = make(L"BUTTON", L"Debug report (bake_debug.txt)", BS_AUTOCHECKBOX, 306, 84, 296, 22, ID_DEBUG);
	gLod = make(L"BUTTON", L"Simplified far geometry (about half the triangles far away, +10% size)", BS_AUTOCHECKBOX, 12, 106, 590, 22, ID_LOD);
	SendMessageW(gLod, BM_SETCHECK, BST_CHECKED, 0);
	gSimplify = make(L"BUTTON", L"Simplified all geometry: objects and characters (fewer triangles, faster, less detail)", BS_AUTOCHECKBOX, 12, 128, 590, 22, ID_SIMPLIFY);
	gStart = make(L"BUTTON", L"Bake", BS_DEFPUSHBUTTON, 610, 60, 90, 28, ID_START);
	gProgress = make(PROGRESS_CLASSW, L"", PBS_MARQUEE, 12, 156, 688, 14, ID_PROGRESS);
	gStatus = make(L"STATIC", L"Select the folder and press \"Bake\". The files will appear next to this program.", 0, 12, 176, 688, 18, ID_STATUS);
	gLog = make(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 12, 198, 688, 274, ID_LOG, WS_EX_CLIENTEDGE);
	SendMessageW(gLog, EM_SETLIMITTEXT, 0x7FFFFFF, 0);

	ShowWindow(gWnd, show);
	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
	if (gWorker.joinable()) gWorker.join();
	return 0;
}
