#pragma once
// Windows calls kept out of the game headers (Windows.h macros clash with CommonLib names)
#include <filesystem>
#include <string>

namespace Win
{
	bool                  KeyHeldInGame(int a_vk);       // a_vk held and this game is the window in front
	std::filesystem::path Documents();                   // Documents folder
	std::string           Stamp(bool a_forFileName);     // local date/time
	std::string           IniString(const std::filesystem::path& a_ini, const char* a_key, const char* a_default);
	void                  Sleep(unsigned a_ms);
}
