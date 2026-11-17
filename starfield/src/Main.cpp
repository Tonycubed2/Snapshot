// Snapshot for Starfield (SFSE) - hold a key (default L) for 8 seconds in game: writes a troubleshooting report
// (quests, player, companions, nearby NPCs with factions and AI packages, every actor the game is running AI for,
// load order) and copies the newest saves and all SFSE / Papyrus logs.
//
// Starfield is less mapped out than Skyrim/Fallout 4, so some data comes straight from offsets that were read in
// Starfield.exe 1.16.244 for ReSaverHelper (see the quest part below). Quest objectives, quest aliases and
// faction changes made by scripts are not available on Starfield yet.
#include "SFSE/SFSE.h"
#include "RE/Starfield.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <numbers>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "Win.h"

namespace fs = std::filesystem;

namespace
{
	constexpr auto kVersion = "1.0.0";

	int               g_key = 0x4C;  // Windows virtual-key code: 0x4C = L
	float             g_holdSecs = 8.0f;
	int               g_saves = 3;
	float             g_radius = 10000.0f;
	std::atomic<bool> g_busy{ false };
	std::string       g_runtime;

	void LoadSettings()
	{
		const auto ini = fs::absolute("Data\\SFSE\\Plugins\\Snapshot.ini");
		g_key = std::atoi(Win::IniString(ini, "iHotkey", "76").c_str());
		g_holdSecs = std::strtof(Win::IniString(ini, "fHoldSeconds", "8").c_str(), nullptr);
		g_saves = std::atoi(Win::IniString(ini, "iSavesToCopy", "3").c_str());
		g_radius = std::strtof(Win::IniString(ini, "fOutdoorRadius", "10000").c_str(), nullptr);
		REX::INFO("Settings: key {} hold {}s saves {} radius {}", g_key, g_holdSecs, g_saves, g_radius);
	}

	// ---------------------------------------------------------------- console
	// The console's "run this command" (Address Library id 113576) with the console object kept at id 938528, as
	// ReSaverHelper for Starfield runs it (read in Starfield.exe 1.16.244). Main thread only.
	void Console(const std::string& a_line)
	{
		static const auto exec = REL::ID(113576).address();
		static const auto conp = REL::ID(938528).address();
		if (!exec || !conp) {
			REX::ERROR("console: address not found");
			return;
		}
		using func_t = void (*)(void*, const char*, ...);
		reinterpret_cast<func_t>(exec)(*reinterpret_cast<void**>(conp), "%s", a_line.c_str());
	}

	// ---------------------------------------------------------------- quests (offsets read in Starfield.exe 1.16.244
	// for ReSaverHelper: flags +0x114, current stage +0x328, stages +0x138 count / +0x140 list, stage +0x10 index,
	// +0x12 bit 0 done)
	template <class T>
	T Read(const void* a_base, std::size_t a_off)
	{
		T v{};
		std::memcpy(&v, static_cast<const std::uint8_t*>(a_base) + a_off, sizeof(T));
		return v;
	}

	struct QuestState
	{
		std::uint32_t                               flags = 0;
		std::uint16_t                               stage = 0;
		bool                                        running = false, completed = false;
		std::vector<std::pair<std::uint16_t, bool>> stages;
	};

	QuestState ReadQuest(const RE::TESForm* a_quest)
	{
		QuestState q;
		q.flags = Read<std::uint32_t>(a_quest, 0x114);
		q.stage = Read<std::uint16_t>(a_quest, 0x328);
		q.completed = (q.flags & 0x2) != 0;
		q.running = (q.flags & 0x1) && !(q.flags & 0x80) && Read<std::uint64_t>(a_quest, 0x338) == 0 && Read<std::uint8_t>(a_quest, 0x32C) == 0;
		const auto count = Read<std::uint32_t>(a_quest, 0x138);
		const auto list = Read<const std::uint8_t* const*>(a_quest, 0x140);
		for (std::uint32_t i = 0; list && i < count && i < 4096; ++i) {
			if (const auto* st = list[i]) {
				q.stages.emplace_back(Read<std::uint16_t>(st, 0x10), (Read<std::uint8_t>(st, 0x12) & 1) != 0);
			}
		}
		return q;
	}

	// ---------------------------------------------------------------- text helpers
	std::string S(const char* a) { return a ? a : ""; }

	template <class T>
	std::string Label(const T* a_form)
	{
		if (!a_form) {
			return "(none)";
		}
		std::string out;
		if constexpr (std::is_base_of_v<RE::TESFullName, T>) {
			out = "\"" + S(a_form->GetFullName()) + "\"";
		}
		const auto ed = S(a_form->GetFormEditorID());
		if (!ed.empty()) {
			out += (out.empty() ? "[" : " [") + ed + "]";
		}
		return out + std::format(" {:08X}", a_form->GetFormID());
	}

	std::string RefLabel(RE::TESObjectREFR* a_ref)
	{
		if (!a_ref) {
			return "(none)";
		}
		const auto base = a_ref->GetBaseObject();
		return std::format("\"{}\" ref {:08X}  base {:08X}", S(a_ref->GetDisplayFullName()), a_ref->GetFormID(), base ? base->GetFormID() : 0u);
	}

	float Dist(RE::TESObjectREFR* a, RE::TESObjectREFR* b)
	{
		const auto p = a->GetPosition();
		const auto q = b->GetPosition();
		return std::sqrt((p.x - q.x) * (p.x - q.x) + (p.y - q.y) * (p.y - q.y) + (p.z - q.z) * (p.z - q.z));
	}

	std::string Pos(RE::TESObjectREFR* a_ref)
	{
		const auto p = a_ref->GetPosition();
		return std::format("x {:.1f}  y {:.1f}  z {:.1f}  facing {:.0f} deg", p.x, p.y, p.z, a_ref->GetAngle().z * 180.0f / std::numbers::pi_v<float>);
	}

	std::string Where(RE::TESObjectREFR* a_ref)
	{
		std::string out;
		const auto  cell = a_ref->parentCell;
		out += cell ? "cell " + Label(cell) + (cell->IsInterior() ? " (interior)" : " (exterior)") : std::string("cell (not loaded)");
		std::string chain;
		for (auto loc = a_ref->GetCurrentLocation(); loc; loc = loc->parentLocation.get()) {
			chain += (chain.empty() ? "" : " > ") + Label(loc);
		}
		if (!chain.empty()) {
			out += " | location " + chain;
		}
		return out;
	}

	struct Out
	{
		std::ofstream f;
		void          L(const std::string& a_line) { f << a_line << '\n'; }
		void          H(const std::string& a_title)
		{
			f << "\n================================================================================\n"
			  << a_title << "\n================================================================================\n";
		}
	};

	template <class T>
	std::vector<T*> FormsOfType(RE::FormType a_type)
	{
		std::vector<T*> out;
		auto&           arr = RE::TESDataHandler::GetSingleton()->formArrays[std::to_underlying(a_type)];
		for (auto& f : arr.formArray) {
			if (f && f->Is(a_type)) {
				out.push_back(static_cast<T*>(f.get()));
			}
		}
		return out;
	}

	// every actor the game is running AI for (Starfield keeps no list of every actor that CommonLibSF exposes)
	std::vector<RE::Actor*> ProcessActors()
	{
		std::vector<RE::Actor*> out;
		const auto              pl = RE::ProcessLists::GetSingleton();
		if (!pl) {
			return out;
		}
		for (auto list : { &pl->highActorHandles, &pl->middleHighActorHandles, &pl->middleLowActorHandles, &pl->lowActorHandles }) {
			for (auto& h : *list) {
				if (const auto a = h.get(); a && std::find(out.begin(), out.end(), a.get()) == out.end()) {
					out.push_back(a.get());
				}
			}
		}
		std::sort(out.begin(), out.end(), [](auto x, auto y) { return x->GetFormID() < y->GetFormID(); });
		return out;
	}

	std::string Factions(RE::Actor* a_actor)
	{
		std::string out;
		if (const auto npc = a_actor->GetNPC()) {
			for (auto& f : npc->factions) {
				if (f.faction) {
					out += std::format("    {} rank {}\n", Label(f.faction), f.rank);
				}
			}
		}
		return out;
	}

	void ActorDetail(Out& o, RE::Actor* a, RE::Actor* a_player, bool a_isPlayer)
	{
		const auto npc = a->GetNPC();
		o.L("--------------------------------------------------------------------------------");
		o.L(RefLabel(a));
		o.L("  base NPC   " + Label(npc));
		o.L("  race       " + Label(a->race));
		o.L("  position   " + Pos(a));
		o.L("  where      " + Where(a));
		if (!a_isPlayer) {
			o.L(std::format("  distance to player {:.0f} units", Dist(a, a_player)));
		}
		o.L(std::format("  state      dead {} | in combat {} | hostile to player {} | teammate {}", a->IsDead(), a->IsInCombat(),
			a_isPlayer ? false : a->IsHostileToActor(a_player), a->boolBits.all(RE::Actor::BOOL_BITS::kPlayerTeammate)));
		if (!a_isPlayer) {
			const auto pkg = a->currentProcess ? a->currentProcess->GetActorPackageRunning()->package : nullptr;
			o.L("  CURRENT PACKAGE " + Label(pkg));
		}
		o.L("  factions (from the NPC record; changes made by scripts are not readable on Starfield yet):");
		o.f << Factions(a);
		o.L("  stats (current / base / permanent):");
		const auto owner = static_cast<RE::ActorValueOwner*>(a);
		for (auto av : FormsOfType<RE::ActorValueInfo>(RE::FormType::kAVIF)) {
			o.L(std::format("    {:<48} {:>10.2f} {:>10.2f} {:>10.2f}", Label(av), owner->GetActorValue(*av), owner->GetBaseActorValue(*av), owner->GetPermanentActorValue(*av)));
		}
	}

	void WriteReport(const fs::path& a_file)
	{
		Out o;
		o.f.open(a_file, std::ios::out | std::ios::trunc);
		if (!o.f) {
			REX::ERROR("cannot write {}", a_file.string());
			return;
		}
		const auto player = RE::PlayerCharacter::GetSingleton();
		const auto dh = RE::TESDataHandler::GetSingleton();
		o.L(std::format("Snapshot {} troubleshooting report - Starfield", kVersion));
		o.L(std::format("Taken {}   game runtime {}", Win::Stamp(false), g_runtime));

		o.H("PLAYER");
		ActorDetail(o, player, player, true);

		const auto actors = ProcessActors();
		o.H("COMPANIONS (player teammates)");
		for (auto a : actors) {
			if (a != player && a->boolBits.all(RE::Actor::BOOL_BITS::kPlayerTeammate)) {
				ActorDetail(o, a, player, false);
			}
		}

		const auto pcell = player->parentCell;
		const bool inside = pcell && pcell->IsInterior();
		o.H(inside ? "NPCs IN THE SAME INTERIOR AS THE PLAYER" : std::format("NPCs WITHIN {:.0f} UNITS OF THE PLAYER (outdoors)", g_radius));
		int nearby = 0;
		for (auto a : actors) {
			if (a == player) {
				continue;
			}
			const bool hit = inside ? a->parentCell == pcell : (a->parentCell && a->parentCell->IsExterior() && Dist(a, player) <= g_radius);
			if (hit) {
				ActorDetail(o, a, player, false);
				++nearby;
			}
		}
		o.L(std::format("{} NPCs listed", nearby));

		o.H("RUNNING QUESTS (stage and stages done; objectives and aliases are not readable on Starfield yet)");
		std::vector<std::pair<RE::TESForm*, QuestState>> completed;
		for (auto q : FormsOfType<RE::TESForm>(RE::FormType::kQUST)) {
			const auto st = ReadQuest(q);
			if (st.completed) {
				completed.emplace_back(q, st);
			}
			if (!st.running) {
				continue;
			}
			o.L("--------------------------------------------------------------------------------");
			o.L(std::format("QUEST {}  stage {} | completed {} | flags {:08X}", Label(q), st.stage, st.completed, st.flags));
			std::string done;
			for (auto& [i, d] : st.stages) {
				if (d) {
					done += std::format("{} ", i);
				}
			}
			o.L("  stages done: " + (done.empty() ? std::string("none") : done));
		}
		o.H("COMPLETED QUESTS");
		for (auto& [q, st] : completed) {
			o.L(std::format("  stage {:>4}  {}", st.stage, Label(q)));
		}

		o.H(std::format("EVERY ACTOR THE GAME IS RUNNING AI FOR ({}): location and factions", actors.size()));
		for (auto a : actors) {
			o.L(RefLabel(a) + std::format("  | dead {}", a->IsDead()));
			o.L("    " + Where(a));
			o.f << Factions(a);
		}

		o.H("LOAD ORDER (full, small and medium plugins)");
		for (auto list : { &dh->compiledFileCollection.files, &dh->compiledFileCollection.smallFiles, &dh->compiledFileCollection.mediumFiles }) {
			for (auto f : *list) {
				if (f) {
					o.L(std::format("  {:02X}  {}", f->compileIndex, f->fileName));
				}
			}
		}
		o.H("SFSE PLUGINS (Data\\SFSE\\Plugins\\*.dll)");
		std::error_code ec;
		for (auto& e : fs::directory_iterator("Data\\SFSE\\Plugins", ec)) {
			if (e.is_regular_file(ec) && e.path().extension() == ".dll") {
				o.L(std::format("  {:<48} {:>10} bytes", e.path().filename().string(), e.file_size(ec)));
			}
		}
		o.L("\n(end of report)");
	}

	// ---------------------------------------------------------------- copies (background thread)
	void CopyTree(const fs::path& a_from, const fs::path& a_to, const fs::path& a_skip, std::vector<std::string>& a_errors)
	{
		std::error_code ec;
		for (auto it = fs::recursive_directory_iterator(a_from, fs::directory_options::skip_permission_denied, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
			if (ec) {
				break;
			}
			const auto& p = it->path();
			if (fs::equivalent(p, a_skip, ec)) {
				it.disable_recursion_pending();
				continue;
			}
			if (!it->is_regular_file(ec)) {
				continue;
			}
			const auto dest = a_to / fs::relative(p, a_from, ec);
			fs::create_directories(dest.parent_path(), ec);
			if (!fs::copy_file(p, dest, fs::copy_options::overwrite_existing, ec)) {
				a_errors.push_back(p.string() + ": " + ec.message());
			}
		}
	}

	void CopyFiles(fs::path a_myGames, fs::path a_outRoot, fs::path a_outDir)
	{
		std::vector<std::string> errors;
		std::error_code          ec;
		CopyTree(a_myGames / "SFSE", a_outDir / "Logs" / "SFSE", a_outRoot, errors);
		CopyTree(a_myGames / "Logs", a_outDir / "Logs" / "Logs", a_outRoot, errors);
		std::vector<fs::directory_entry> saves;
		for (auto& e : fs::directory_iterator(a_myGames / "Saves", ec)) {
			if (e.is_regular_file(ec) && e.path().extension() == ".sfs") {
				saves.push_back(e);
			}
		}
		std::sort(saves.begin(), saves.end(), [](auto& x, auto& y) {
			std::error_code e1;
			return x.last_write_time(e1) > y.last_write_time(e1);
		});
		fs::create_directories(a_outDir / "Saves", ec);
		for (int i = 0; i < static_cast<int>(saves.size()) && i < g_saves; ++i) {
			// the save and every file next to it with the same name (SFSE co-save and any others)
			const auto stem = saves[i].path().stem();
			for (auto& e : fs::directory_iterator(a_myGames / "Saves", ec)) {
				if (e.is_regular_file(ec) && e.path().stem() == stem &&
					!fs::copy_file(e.path(), a_outDir / "Saves" / e.path().filename(), fs::copy_options::overwrite_existing, ec)) {
					errors.push_back(e.path().string() + ": " + ec.message());
				}
			}
		}
		if (!errors.empty()) {
			std::ofstream f(a_outDir / "copy_errors.txt");
			for (auto& e : errors) {
				f << e << '\n';
			}
		}
		REX::INFO("Snapshot files copied to {} ({} errors)", a_outDir.string(), errors.size());
		SFSE::GetTaskInterface()->AddTask([]() { Console("cgf \"Debug.MessageBox\" \"Data Collected\""); });
		g_busy = false;
	}

	void TakeSnapshot()
	{
		const fs::path myGames = Win::Documents() / "My Games" / "Starfield";
		const fs::path outRoot = myGames / "SFSE" / "Snapshot";
		const fs::path outDir = outRoot / ("Snapshot_" + Win::Stamp(true));
		std::error_code ec;
		fs::create_directories(outDir, ec);
		REX::INFO("Snapshot: writing report to {}", outDir.string());
		Console("cgf \"Debug.Notification\" \"Snapshot: collecting data...\"");
		WriteReport(outDir / "Snapshot report.txt");
		std::thread([=]() { CopyFiles(myGames, outRoot, outDir); }).detach();
	}

	bool InGame()
	{
		const auto p = RE::PlayerCharacter::GetSingleton();
		return p && p->parentCell;
	}

	void HotkeyThread()
	{
		bool  fired = false;
		float held = 0.0f;
		while (true) {
			Win::Sleep(100);
			if (!Win::KeyHeldInGame(g_key) || !InGame()) {
				held = 0.0f;
				fired = false;
				continue;
			}
			held += 0.1f;
			if (!fired && held >= g_holdSecs) {
				fired = true;
				if (!g_busy.exchange(true)) {
					SFSE::GetTaskInterface()->AddTask([]() { TakeSnapshot(); });
				}
			}
		}
	}
}

SFSE_PLUGIN_LOAD(const SFSE::LoadInterface* a_sfse)
{
	SFSE::Init(a_sfse);
	g_runtime = a_sfse->RuntimeVersion().string();
	REX::INFO("Snapshot {} loading", kVersion);
	LoadSettings();
	std::thread(HotkeyThread).detach();
	return true;
}
