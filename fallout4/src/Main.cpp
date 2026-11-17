// Snapshot for Fallout 4 (F4SE) - hold a key (default L) for 8 seconds in game: writes a troubleshooting report
// (running quests, player, companions, nearby NPCs with factions and AI packages, every NPC's factions, load
// order) and copies the newest saves and all F4SE / Papyrus logs.
#include "F4SE/F4SE.h"
#include "RE/Fallout.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <numbers>
#include <string>
#include <thread>
#include <vector>

#include "Win.h"

namespace fs = std::filesystem;

namespace
{
	constexpr auto kVersion = "1.0.0";
	constexpr RE::TESFormID kFollowersQuest = 0x000289E4;  // Fallout4.esm "Followers" (the vanilla companion quest)

	// ---------------------------------------------------------------- settings (Data\F4SE\Plugins\Snapshot.ini)
	int   g_key = 0x4C;  // Windows virtual-key code: 0x4C = L
	float g_holdSecs = 8.0f;
	int   g_saves = 3;
	float g_radius = 10000.0f;
	std::atomic<bool> g_inGame{ false };
	std::string       g_runtime;
	std::atomic<bool> g_busy{ false };

	void LoadSettings()
	{
		const auto ini = fs::absolute("Data\\F4SE\\Plugins\\Snapshot.ini");
		g_key = std::atoi(Win::IniString(ini, "iHotkey", "76").c_str());
		g_holdSecs = std::strtof(Win::IniString(ini, "fHoldSeconds", "8").c_str(), nullptr);
		g_saves = std::atoi(Win::IniString(ini, "iSavesToCopy", "3").c_str());
		g_radius = std::strtof(Win::IniString(ini, "fOutdoorRadius", "10000").c_str(), nullptr);
		REX::INFO("Settings: key {} hold {}s saves {} radius {}", g_key, g_holdSecs, g_saves, g_radius);
	}

	// ---------------------------------------------------------------- console (as the game's console runs a line)
	void Console(const std::string& a_line)
	{
		static RE::Script* script = nullptr;
		if (!script) {
			if (const auto factory = RE::ConcreteFormFactory<RE::Script>::GetFormFactory()) {
				script = factory->Create();
			}
		}
		if (!script) {
			REX::ERROR("console: no script form");
			return;
		}
		script->SetText(a_line);
		RE::ScriptCompiler compiler;
		script->CompileAndRun(&compiler, static_cast<RE::COMPILER_NAME>(1), nullptr);  // 1 = kSystemWindow, as the console
	}

	// ---------------------------------------------------------------- text helpers
	std::string S(const char* a) { return a ? a : ""; }

	std::string PluginOf(const RE::TESForm* a_form)
	{
		if (!a_form) {
			return "";
		}
		if (const auto file = a_form->GetFile(0)) {
			return std::string(file->GetFilename());
		}
		return a_form->IsCreated() ? "created in game" : "?";
	}

	std::string Label(const RE::TESForm* a_form)
	{
		if (!a_form) {
			return "(none)";
		}
		std::string out = "\"" + std::string(RE::TESFullName::GetFullName(*a_form)) + "\"";
		const auto  ed = S(a_form->GetFormEditorID());
		if (!ed.empty()) {
			out += " [" + ed + "]";
		}
		return out + std::format(" {:08X} <{}>", a_form->GetFormID(), PluginOf(a_form));
	}

	std::string RefLabel(RE::TESObjectREFR* a_ref)
	{
		if (!a_ref) {
			return "(none)";
		}
		const auto base = a_ref->GetObjectReference();
		return std::format("\"{}\" ref {:08X} <{}>  base {:08X} <{}>", S(a_ref->GetDisplayFullName()), a_ref->GetFormID(), PluginOf(a_ref),
			base ? base->GetFormID() : 0u, PluginOf(base));
	}

	float Dist(RE::TESObjectREFR* a, RE::TESObjectREFR* b)
	{
		const auto dx = a->data.location.x - b->data.location.x;
		const auto dy = a->data.location.y - b->data.location.y;
		const auto dz = a->data.location.z - b->data.location.z;
		return std::sqrt(dx * dx + dy * dy + dz * dz);
	}

	RE::TESWorldSpace* WorldOf(RE::TESObjectREFR* a_ref)
	{
		const auto cell = a_ref->GetParentCell();
		return cell && cell->IsExterior() ? cell->worldSpace : nullptr;
	}

	std::string Pos(RE::TESObjectREFR* a_ref)
	{
		const auto& p = a_ref->data.location;
		return std::format("x {:.1f}  y {:.1f}  z {:.1f}  facing {:.0f} deg", p.x, p.y, p.z, a_ref->data.angle.z * 180.0f / std::numbers::pi_v<float>);
	}

	std::string Where(RE::TESObjectREFR* a_ref)
	{
		std::string out;
		const auto  cell = a_ref->GetParentCell();
		if (cell) {
			out += "cell " + Label(cell);
			if (cell->IsInterior()) {
				out += " (interior)";
			} else {
				out += std::format(" (exterior grid {},{})", cell->GetDataX(), cell->GetDataY());
				if (cell->worldSpace) {
					out += " | worldspace " + Label(cell->worldSpace);
				}
			}
		} else {
			out += "cell (not loaded)";
		}
		std::string chain;
		for (auto loc = a_ref->GetCurrentLocation(); loc; loc = loc->parentLoc) {
			chain += (chain.empty() ? "" : " > ") + Label(loc);
		}
		if (!chain.empty()) {
			out += " | location " + chain;
		}
		return out;
	}

	const char* ObjState(int a_s)
	{
		switch (a_s) {
		case 0: return "dormant";
		case 1: return "DISPLAYED";
		case 2: return "completed";
		case 3: return "completed (shown)";
		case 4: return "failed";
		case 5: return "failed (shown)";
		default: return "?";
		}
	}

	// ---------------------------------------------------------------- report
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

	std::vector<RE::Actor*> AllActors()
	{
		std::vector<RE::Actor*> out;
		const auto& [map, lock] = RE::TESForm::GetAllForms();
		RE::BSAutoReadLock l{ lock.get() };
		if (map) {
			for (auto& [id, form] : *map) {
				if (form && form->Is(RE::ENUM_FORM_ID::kACHR) && !form->IsDeleted()) {
					if (const auto a = form->As<RE::Actor>()) {
						out.push_back(a);
					}
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
					out += std::format("    (base)    {} rank {}\n", Label(f.faction), f.rank);
				}
			}
		}
		if (a_actor->extraList) {
			if (const auto changes = a_actor->extraList->GetByType<RE::ExtraFactionChanges>()) {
				for (auto& f : changes->factionChanges) {
					if (f.faction) {
						out += std::format("    (changed) {} rank {}{}\n", Label(f.faction), f.rank, f.rank < 0 ? " (removed)" : "");
					}
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
		o.L("  race       " + Label(a->race) + std::format("   sex {}   level {}", a->GetSex() == RE::SEX::kFemale ? "female" : "male", a->GetLevel()));
		o.L("  position   " + Pos(a));
		o.L("  where      " + Where(a));
		const bool loaded = a->Get3D() != nullptr;
		if (!a_isPlayer) {
			o.L(std::format("  distance to player {:.0f} units   3D loaded {}", Dist(a, a_player), loaded));
		}
		const auto flags = a->boolFlags;
		o.L(std::format("  state      dead {} | disabled {} | in combat {} | hostile to player {} | essential {} | protected {} | commanded {} | sneaking {} | weapon drawn {}",
			a->IsDead(false), a->IsDisabled(), a->IsInCombat(), (a_isPlayer || !loaded) ? false : a->GetHostileToActor(a_player),
			flags.all(RE::Actor::BOOL_FLAGS::kEssential), flags.all(RE::Actor::BOOL_FLAGS::kProtected), flags.all(RE::Actor::BOOL_FLAGS::kIsCommandedActor),
			a->IsSneaking(), a->GetWeaponMagicDrawn()));
		if (const auto target = a->currentCombatTarget.get()) {
			o.L("  combat target " + RefLabel(target.get()));
		}
		if (!a_isPlayer) {
			const auto pkg = a->currentProcess ? a->currentProcess->currentPackage.package : nullptr;
			o.L("  CURRENT PACKAGE " + Label(pkg));
			o.L("  packages on NPC (in priority order):");
			if (npc) {
				for (auto p : npc->aiPackList.listPackages) {
					if (p) {
						o.L("    " + Label(p));
					}
				}
			}
		}
		o.L("  factions:");
		o.f << Factions(a);
		o.L("  stats (current / base / permanent):");
		const auto owner = static_cast<RE::ActorValueOwner*>(a);
		for (auto av : RE::TESDataHandler::GetSingleton()->GetFormArray<RE::ActorValueInfo>()) {
			if (av) {
				o.L(std::format("    {:<44} {:>10.2f} {:>10.2f} {:>10.2f}", Label(av), owner->GetActorValue(*av), owner->GetBaseActorValue(*av), owner->GetPermanentActorValue(*av)));
			}
		}
		o.L("  spells:");
		if (npc && npc->spellData) {
			for (std::uint32_t i = 0; i < npc->spellData->numSpells; ++i) {
				if (const auto sp = npc->spellData->spells[i]) {
					o.L("    (base) " + Label(sp));
				}
			}
		}
		for (auto sp : a->addedSpells) {
			if (sp) {
				o.L("    (added) " + Label(sp));
			}
		}
		o.L("  active magic effects:");
		if (const auto list = static_cast<RE::MagicTarget*>(a)->GetActiveEffectList()) {
			for (auto& sp : list->data) {
				const auto ae = sp.get();
				if (!ae) {
					continue;
				}
				o.L(std::format("    {} from {}  magnitude {:.1f}  {:.0f}/{:.0f} s{}", Label(ae->effect ? ae->effect->effectSetting : nullptr), Label(ae->spell),
					ae->magnitude, ae->elapsedSeconds, ae->duration, ae->flags.any(RE::ActiveEffect::Flags::kInactive) ? "  (inactive)" : ""));
			}
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

		o.L(std::format("Snapshot {} troubleshooting report - Fallout 4", kVersion));
		o.L(std::format("Taken {}   game runtime {}", Win::Stamp(false), g_runtime));
		if (const auto cal = RE::Calendar::GetSingleton()) {
			o.L(std::format("Game days passed {:.2f}   hour {:.2f}", cal->gameDaysPassed ? cal->gameDaysPassed->GetValue() : 0.0f, cal->gameHour ? cal->gameHour->GetValue() : 0.0f));
		}
		if (const auto sky = RE::Sky::GetSingleton()) {
			o.L("Weather: " + Label(sky->currentWeather));
		}
		if (const auto ui = RE::UI::GetSingleton()) {
			std::string menus;
			for (auto m : { "Console", "DialogueMenu", "PipboyMenu", "ContainerMenu", "BarterMenu", "LooksMenu", "WorkshopMenu", "LoadingMenu", "MessageBoxMenu",
					 "SleepWaitMenu", "TerminalMenu", "LockpickingMenu", "ExamineMenu", "CookingMenu", "PauseMenu", "FaderMenu" }) {
				if (ui->GetMenuOpen(RE::BSFixedString(m))) {
					menus += std::string(menus.empty() ? "" : ", ") + m;
				}
			}
			o.L("Open menus: " + (menus.empty() ? std::string("none") : menus));
		}

		o.H("PLAYER");
		ActorDetail(o, player, player, true);
		o.L("  perks (rank):");
		for (auto perk : dh->GetFormArray<RE::BGSPerk>()) {
			if (perk) {
				if (const auto rank = player->GetPerkRank(perk); rank > 0) {
					o.L(std::format("    {}  rank {}", Label(perk), rank));
				}
			}
		}
		o.L("  inventory (count, equipped):");
		if (const auto inv = player->inventoryList) {
			RE::BSAutoReadLock l{ inv->rwLock };
			for (auto& item : inv->data) {
				std::uint32_t count = 0;
				bool          equipped = false;
				for (auto s = item.stackData.get(); s; s = s->nextStack.get()) {
					count += s->count;
					equipped = equipped || s->IsEquipped();
				}
				o.L(std::format("    {:>6}  {}{}", count, Label(item.object), equipped ? "  [EQUIPPED]" : ""));
			}
		}

		const auto actors = AllActors();
		o.H("COMPANIONS (actors held by the Followers quest's aliases, and commanded actors)");
		std::vector<RE::Actor*> companions;
		if (const auto fq = RE::TESForm::GetFormByID<RE::TESQuest>(kFollowersQuest)) {
			for (auto al : fq->aliases) {
				if (!al) {
					continue;
				}
				if (const auto r = fq->GetAliasedRef(al->aliasID).get()) {
					if (const auto a = r->As<RE::Actor>(); a && a != player && std::find(companions.begin(), companions.end(), a) == companions.end()) {
						o.L(std::format("  (Followers alias \"{}\")", S(al->aliasName.c_str())));
						companions.push_back(a);
						ActorDetail(o, a, player, false);
					}
				}
			}
		}
		for (auto a : actors) {
			if (a != player && a->boolFlags.all(RE::Actor::BOOL_FLAGS::kIsCommandedActor) && std::find(companions.begin(), companions.end(), a) == companions.end()) {
				o.L("  (commanded actor)");
				ActorDetail(o, a, player, false);
			}
		}

		const auto pcell = player->GetParentCell();
		const bool inside = pcell && pcell->IsInterior();
		o.H(inside ? "NPCs IN THE SAME INTERIOR AS THE PLAYER" : std::format("NPCs WITHIN {:.0f} UNITS OF THE PLAYER (outdoors)", g_radius));
		int nearby = 0;
		for (auto a : actors) {
			if (a == player || a->IsDisabled()) {
				continue;
			}
			const auto cell = a->GetParentCell();
			const bool hit = inside ? cell == pcell : (cell && cell->IsExterior() && WorldOf(a) == WorldOf(player) && Dist(a, player) <= g_radius);
			if (hit) {
				ActorDetail(o, a, player, false);
				++nearby;
			}
		}
		o.L(std::format("{} NPCs listed", nearby));

		o.H("RUNNING QUESTS (full detail)");
		std::vector<RE::TESQuest*> completed;
		for (auto q : dh->GetFormArray<RE::TESQuest>()) {
			if (!q) {
				continue;
			}
			const std::uint16_t fl = q->data.flags;
			const bool          transit = fl == 0xFFFF;  // caught mid start/stop
			const bool          running = !transit && (fl & 0x0001);
			if (!transit && (fl & 0x0002)) {
				completed.push_back(q);
			}
			if (!running && !transit) {
				continue;
			}
			o.L("--------------------------------------------------------------------------------");
			o.L("QUEST " + Label(q));
			o.L(std::format("  stage {} | running {} | starting/stopping {} | completed {} | failed {} | active in journal {} | already run {} | type {} | flags {:04X}",
				q->currentStage, running, transit, !transit && (fl & 0x0002), !transit && (fl & 0x0040), !transit && (fl & 0x0800), q->alreadyRun, q->data.questType, fl));
			for (auto obj : q->objectives) {
				if (obj) {
					o.L(std::format("  objective {:>4}  {:<18} {}", obj->index, ObjState(obj->state), S(obj->displayText.c_str())));
				}
			}
			for (auto al : q->aliases) {
				if (!al) {
					continue;
				}
				std::string filled = "(empty / not a reference alias)";
				if (const auto r = q->GetAliasedRef(al->aliasID).get()) {
					filled = RefLabel(r.get());
					if (r->GetParentCell()) {
						filled += "  @ " + Where(r.get());
					}
				}
				o.L(std::format("  alias {:>3} {:<32} {}", al->aliasID, S(al->aliasName.c_str()), filled));
			}
		}
		o.H("COMPLETED QUESTS");
		for (auto q : completed) {
			o.L(std::format("  stage {:>4}  {}", q->currentStage, Label(q)));
		}

		o.H(std::format("EVERY NPC IN MEMORY ({} actors): location and factions", actors.size()));
		for (auto a : actors) {
			o.L(RefLabel(a) + std::format("  | dead {} disabled {}", a->IsDead(false), a->IsDisabled()));
			o.L("    " + Where(a));
			o.f << Factions(a);
		}

		o.H("LOAD ORDER");
		for (auto f : dh->compiledFileCollection.files) {
			if (f) {
				o.L(std::format("  {:02X}     {}", f->GetCompileIndex(), f->GetFilename()));
			}
		}
		for (auto f : dh->compiledFileCollection.smallFiles) {
			if (f) {
				o.L(std::format("  FE {:03X} {}", f->GetSmallFileCompileIndex(), f->GetFilename()));
			}
		}
		o.H("F4SE PLUGINS (Data\\F4SE\\Plugins\\*.dll)");
		std::error_code ec;
		for (auto& e : fs::directory_iterator("Data\\F4SE\\Plugins", ec)) {
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
		CopyTree(a_myGames / "F4SE", a_outDir / "Logs" / "F4SE", a_outRoot, errors);
		CopyTree(a_myGames / "Logs", a_outDir / "Logs" / "Logs", a_outRoot, errors);
		std::vector<fs::directory_entry> saves;
		for (auto& e : fs::directory_iterator(a_myGames / "Saves", ec)) {
			if (e.is_regular_file(ec) && e.path().extension() == ".fos") {
				saves.push_back(e);
			}
		}
		std::sort(saves.begin(), saves.end(), [](auto& x, auto& y) {
			std::error_code e1;
			return x.last_write_time(e1) > y.last_write_time(e1);
		});
		fs::create_directories(a_outDir / "Saves", ec);
		for (int i = 0; i < static_cast<int>(saves.size()) && i < g_saves; ++i) {
			for (auto ext : { ".fos", ".f4se" }) {
				auto src = saves[i].path();
				src.replace_extension(ext);
				if (fs::exists(src, ec) && !fs::copy_file(src, a_outDir / "Saves" / src.filename(), fs::copy_options::overwrite_existing, ec)) {
					errors.push_back(src.string() + ": " + ec.message());
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
		F4SE::GetTaskInterface()->AddTask([]() { Console("cgf \"Debug.MessageBox\" \"Data Collected\""); });
		g_busy = false;
	}

	void TakeSnapshot()
	{
		const fs::path myGames = Win::Documents() / "My Games" / std::string(F4SE::GetSaveFolderName().empty() ? "Fallout4" : F4SE::GetSaveFolderName());
		const fs::path outRoot = myGames / "F4SE" / "Snapshot";
		const fs::path outDir = outRoot / ("Snapshot_" + Win::Stamp(true));
		std::error_code ec;
		fs::create_directories(outDir, ec);
		REX::INFO("Snapshot: writing report to {}", outDir.string());
		Console("cgf \"Debug.Notification\" \"Snapshot: collecting data...\"");
		WriteReport(outDir / "Snapshot report.txt");
		std::thread([=]() { CopyFiles(myGames, outRoot, outDir); }).detach();
	}

	void HotkeyThread()
	{
		bool  fired = false;
		float held = 0.0f;
		while (true) {
			Win::Sleep(100);
			if (!g_inGame || !Win::KeyHeldInGame(g_key)) {
				held = 0.0f;
				fired = false;
				continue;
			}
			held += 0.1f;
			if (!fired && held >= g_holdSecs) {
				fired = true;
				if (!g_busy.exchange(true)) {
					F4SE::GetTaskInterface()->AddTask([]() { TakeSnapshot(); });
				}
			}
		}
	}

	void OnMessage(F4SE::MessagingInterface::Message* a_msg)
	{
		switch (a_msg->type) {
		case F4SE::MessagingInterface::kPostLoadGame:
		case F4SE::MessagingInterface::kNewGame:
			g_inGame = true;
			break;
		default:
			break;
		}
	}
}

F4SE_PLUGIN_LOAD(const F4SE::LoadInterface* a_f4se)
{
	F4SE::Init(a_f4se);
	g_runtime = a_f4se->RuntimeVersion().string();
	REX::INFO("Snapshot {} loading", kVersion);
	LoadSettings();
	F4SE::GetMessagingInterface()->RegisterListener(OnMessage);
	std::thread(HotkeyThread).detach();
	return true;
}
