#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <ShlObj.h>

#include <cstdio>

#include "Win.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")

namespace Win
{
	bool KeyHeldInGame(int a_vk)
	{
		DWORD pid = 0;
		if (HWND w = GetForegroundWindow()) {
			GetWindowThreadProcessId(w, &pid);
		}
		return pid == GetCurrentProcessId() && (GetAsyncKeyState(a_vk) & 0x8000) != 0;
	}

	std::filesystem::path Documents()
	{
		PWSTR p = nullptr;
		std::filesystem::path out;
		if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &p)) && p) {
			out = p;
		}
		CoTaskMemFree(p);
		return out;
	}

	std::string Stamp(bool a_forFileName)
	{
		SYSTEMTIME t{};
		GetLocalTime(&t);
		char buf[64]{};
		if (a_forFileName) {
			std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u_%02u-%02u-%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
		} else {
			std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
		}
		return buf;
	}

	std::string IniString(const std::filesystem::path& a_ini, const char* a_key, const char* a_default)
	{
		char buf[256]{};
		GetPrivateProfileStringA("General", a_key, a_default, buf, sizeof(buf), a_ini.string().c_str());
		return buf;
	}

	void Sleep(unsigned a_ms) { ::Sleep(a_ms); }
}
