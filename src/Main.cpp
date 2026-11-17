// Snapshot - hold a key (default L) for 8 seconds in game and it writes a full troubleshooting
// snapshot: running quests, player stats, every NPC's factions, nearby NPCs with their AI packages,
// followers, load order, SKSE plugins, plus copies of the last 3 saves and all logs.
#include <filesystem>
#include <map>
#include <numbers>
#include <set>

SKSEPluginInfo(
	.Version = REL::Version{ 1, 0, 0, 0 },
	.Name = "Snapshot"sv,
	.Author = "tonycubed2"sv,
	.StructCompatibility = SKSE::StructCompatibility::Independent,
	.RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary)

namespace fs = std::filesystem;

namespace
{
	// ---------------------------------------------------------------- settings (Data\SKSE\Plugins\Snapshot.ini)
	std::uint32_t g_key = 0x26;     // DirectX scan code: 0x26 = L
	float         g_holdSecs = 8.0f;
	int           g_saves = 3;
	float         g_radius = 10000.0f;

	void LoadSettings()
	{
		const auto ini = fs::absolute("Data\\SKSE\\Plugins\\Snapshot.ini").string();
		char       buf[32]{};
		GetPrivateProfileStringA("General", "iHotkey", "38", buf, sizeof(buf), ini.c_str());
		g_key = static_cast<std::uint32_t>(std::strtoul(buf, nullptr, 10));
		GetPrivateProfileStringA("General", "fHoldSeconds", "8", buf, sizeof(buf), ini.c_str());
		g_holdSecs = std::strtof(buf, nullptr);
		GetPrivateProfileStringA("General", "iSavesToCopy", "3", buf, sizeof(buf), ini.c_str());
		g_saves = std::atoi(buf);
		GetPrivateProfileStringA("General", "fOutdoorRadius", "10000", buf, sizeof(buf), ini.c_str());
		g_radius = std::strtof(buf, nullptr);
		logger::info("Settings: key {} hold {}s saves {} radius {}", g_key, g_holdSecs, g_saves, g_radius);
	}

	void ShowNotification(const char* a_text)
	{
		using func_t = void (*)(const char*, const char*, bool);
		static REL::Relocation<func_t> func{ RELOCATION_ID(52050, 52933) };
		func(a_text, nullptr, false);
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
		return a_form->IsDynamicForm() ? "created in game" : "?";
	}

	std::string Label(const RE::TESForm* a_form)
	{
		if (!a_form) {
			return "(none)";
		}
		std::string out = "\"" + S(a_form->GetName()) + "\"";
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
		const auto base = a_ref->GetBaseObject();
		return std::format("\"{}\" ref {:08X} <{}>  base {:08X} <{}>", S(a_ref->GetDisplayFullName()), a_ref->GetFormID(), PluginOf(a_ref),
			base ? base->GetFormID() : 0u, PluginOf(base));
	}

	std::string Pos(RE::TESObjectREFR* a_ref)
	{
		const auto p = a_ref->GetPosition();
		const auto r = a_ref->GetAngle();
		return std::format("x {:.1f}  y {:.1f}  z {:.1f}  facing {:.0f} deg", p.x, p.y, p.z, r.z * 180.0f / std::numbers::pi_v<float>);
	}

	std::string Where(RE::TESObjectREFR* a_ref)
	{
		std::string out;
		const auto  cell = a_ref->GetParentCell();
		if (cell) {
			out += "cell " + Label(cell);
			if (cell->IsInteriorCell()) {
				out += " (interior)";
			} else if (const auto c = cell->GetCoordinates()) {
				out += std::format(" (exterior grid {},{})", c->cellX, c->cellY);
			}
		} else {
			out += "cell (not loaded)";
		}
		if (const auto ws = a_ref->GetWorldspace()) {
			out += " | worldspace " + Label(ws);
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

	const char* ObjState(RE::QUEST_OBJECTIVE_STATE a_s)
	{
		switch (a_s) {
		case RE::QUEST_OBJECTIVE_STATE::kDormant:
			return "dormant";
		case RE::QUEST_OBJECTIVE_STATE::kDisplayed:
			return "DISPLAYED";
		case RE::QUEST_OBJECTIVE_STATE::kCompleted:
			return "completed";
		case RE::QUEST_OBJECTIVE_STATE::kCompletedDisplayed:
			return "completed (shown)";
		case RE::QUEST_OBJECTIVE_STATE::kFailed:
			return "failed";
		case RE::QUEST_OBJECTIVE_STATE::kFailedDisplayed:
			return "failed (shown)";
		default:
			return "?";
		}
	}

	// ---------------------------------------------------------------- report writer
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
		RE::BSReadLockGuard l{ lock };
		if (map) {
			for (auto& [id, form] : *map) {
				if (form && form->Is(RE::FormType::ActorCharacter) && !form->IsDeleted()) {
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
		a_actor->VisitFactions([&](RE::TESFaction* a_faction, std::int8_t a_rank) {
			if (a_faction) {
				out += std::format("{}    {} rank {}{}\n", "", Label(a_faction), a_rank, a_rank < 0 ? " (removed)" : "");
			}
			return false;
		});
		return out;
	}

	void ActorDetail(Out& o, RE::Actor* a, RE::Actor* a_player, bool a_isPlayer)
	{
		auto base = a->GetActorBase();
		o.L("--------------------------------------------------------------------------------");
		o.L(RefLabel(a));
		o.L("  base NPC   " + Label(base));
		o.L("  race       " + Label(a->GetRace()) + std::format("   sex {}   level {}", base && base->GetSex() == RE::SEX::kFemale ? "female" : "male", a->GetLevel()));
		o.L("  position   " + Pos(a));
		o.L("  where      " + Where(a));
		if (!a_isPlayer) {
			o.L(std::format("  distance to player {:.0f} units   3D loaded {}", a->GetDistance(a_player), a->Is3DLoaded()));
		}
		o.L(std::format("  state      dead {} | disabled {} | in combat {} | hostile to player {} | essential {} | protected {} | AI enabled {} | teammate {} | summoned {} | sneaking {} | mounted {} | weapon drawn {} | ghost {}",
			a->IsDead(false), a->IsDisabled(), a->IsInCombat(), (a_isPlayer || !a->Is3DLoaded()) ? false : a->IsHostileToActor(a_player), a->IsEssential(), a->IsProtected(),
			a->IsAIEnabled(), a->IsPlayerTeammate(), a->IsSummoned(), a->IsSneaking(), a->IsOnMount(),
			a->AsActorState() ? a->AsActorState()->IsWeaponDrawn() : false, a->IsGhost()));
		if (const auto target = a->GetActorRuntimeData().currentCombatTarget.get()) {
			o.L("  combat target " + RefLabel(target.get()));
		}
		if (!a_isPlayer) {
			const auto pkg = a->GetCurrentPackage();
			o.L("  CURRENT PACKAGE " + (pkg ? Label(pkg) + "  type " + S(pkg->GetObjectTypeName()) : std::string("(none)")));
			o.L("  packages on NPC (in priority order):");
			if (base) {
				for (auto p : base->aiPackages.packages) {
					if (p) {
						o.L("    " + Label(p) + "  type " + S(p->GetObjectTypeName()));
					}
				}
			}
		}
		o.L("  factions (base, then script changes):");
		o.f << Factions(a);
		o.L("  stats (current / base / permanent):");
		if (const auto av = a->AsActorValueOwner()) {
			for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(RE::ActorValue::kTotal); ++i) {
				const auto v = static_cast<RE::ActorValue>(i);
				o.L(std::format("    {:<28} {:>10.2f} {:>10.2f} {:>10.2f}", S(RE::ActorValueList::GetActorValueName(v)), av->GetActorValue(v), av->GetBaseActorValue(v), av->GetPermanentActorValue(v)));
			}
		}
		o.L("  equipped: right " + Label(a->GetEquippedObject(false)) + " | left " + Label(a->GetEquippedObject(true)));
		o.L("  spells:");
		if (base && base->actorEffects) {
			for (std::uint32_t i = 0; i < base->actorEffects->numSpells; ++i) {
				if (const auto sp = base->actorEffects->spells[i]) {
					o.L("    (base) " + Label(sp));
				}
			}
		}
		for (auto sp : a->GetActorRuntimeData().addedSpells) {
			if (sp) {
				o.L("    (added) " + Label(sp));
			}
		}
		o.L("  active magic effects:");
		if (const auto mt = a->AsMagicTarget()) {
			if (const auto list = mt->GetActiveEffectList()) {
				for (auto ae : *list) {
					if (!ae) {
						continue;
					}
					o.L(std::format("    {} from {}  magnitude {:.1f}  {:.0f}/{:.0f} s{}", Label(ae->GetBaseObject()), Label(ae->spell), ae->magnitude, ae->elapsedSeconds, ae->duration,
						ae->flags.any(RE::ActiveEffect::Flag::kInactive) ? "  (inactive)" : ""));
				}
			}
		}
	}

	// ---------------------------------------------------------------- the report
	void WriteReport(const fs::path& a_file)
	{
		Out o;
		o.f.open(a_file, std::ios::out | std::ios::trunc);
		if (!o.f) {
			logger::error("cannot write {}", a_file.string());
			return;
		}
		const auto player = RE::PlayerCharacter::GetSingleton();
		const auto dh = RE::TESDataHandler::GetSingleton();

		SYSTEMTIME t{};
		GetLocalTime(&t);
		o.L("Snapshot 1.0.0 troubleshooting report");
		o.L(std::format("Taken {:04}-{:02}-{:02} {:02}:{:02}:{:02}   game runtime {}", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, REL::Module::get().version().string()));
		if (const auto cal = RE::Calendar::GetSingleton()) {
			char buf[256]{};
			cal->GetTimeDateString(buf, sizeof(buf), true);
			o.L(std::format("Game date: {}   days passed {:.2f}   hour {:.2f}   timescale {:.0f}", buf, cal->GetDaysPassed(), cal->GetHour(), cal->GetTimescale()));
		}
		if (const auto sky = RE::Sky::GetSingleton()) {
			o.L("Weather: " + Label(sky->currentWeather));
		}
		if (const auto ui = RE::UI::GetSingleton()) {
			std::string menus;
			for (auto m : { "Console", "Dialogue Menu", "InventoryMenu", "ContainerMenu", "BarterMenu", "MapMenu", "Journal Menu", "Sleep/Wait Menu", "MessageBoxMenu",
					 "Lockpicking Menu", "Crafting Menu", "Book Menu", "TweenMenu", "StatsMenu", "MagicMenu", "FavoritesMenu", "Loading Menu", "Training Menu", "GiftMenu" }) {
				if (ui->IsMenuOpen(m)) {
					menus += std::string(menus.empty() ? "" : ", ") + m;
				}
			}
			o.L("Open menus: " + (menus.empty() ? std::string("none") : menus));
		}

		// ---- player
		o.H("PLAYER");
		ActorDetail(o, player, player, true);
		o.L("  perks:");
		for (auto perk : dh->GetFormArray<RE::BGSPerk>()) {
			if (perk && player->HasPerk(perk)) {
				o.L("    " + Label(perk));
			}
		}
		o.L("  bounties (crime gold per crime faction):");
		for (auto fac : dh->GetFormArray<RE::TESFaction>()) {
			if (fac && fac->TracksCrimes()) {
				if (const auto gold = player->GetCrimeGoldValue(fac); gold > 0) {
					o.L(std::format("    {}  {} gold", Label(fac), gold));
				}
			}
		}
		o.L("  inventory (count, worn):");
		for (auto& [obj, data] : player->GetInventory()) {
			o.L(std::format("    {:>6}  {}{}", data.first, Label(obj), data.second && data.second->IsWorn() ? "  [WORN]" : ""));
		}

		// ---- followers
		const auto actors = AllActors();
		static const auto followerFaction = RE::TESForm::LookupByID<RE::TESFaction>(0x0005C84E);  // CurrentFollowerFaction
		o.H("FOLLOWERS (player teammates, CurrentFollowerFaction members, summons)");
		for (auto a : actors) {
			if (a == player) {
				continue;
			}
			if (a->IsPlayerTeammate() || a->IsSummonedByPlayer() || (followerFaction && a->IsInFaction(followerFaction))) {
				ActorDetail(o, a, player, false);
			}
		}

		// ---- nearby
		const auto pcell = player->GetParentCell();
		const bool inside = pcell && pcell->IsInteriorCell();
		o.H(inside ? "NPCs IN THE SAME INTERIOR (dungeon / building) AS THE PLAYER" : std::format("NPCs WITHIN {:.0f} UNITS OF THE PLAYER (outdoors)", g_radius));
		int nearby = 0;
		for (auto a : actors) {
			if (a == player || a->IsDisabled()) {
				continue;
			}
			const bool hit = inside ? a->GetParentCell() == pcell :
			                          (a->GetParentCell() && !a->GetParentCell()->IsInteriorCell() && a->GetWorldspace() == player->GetWorldspace() && a->GetDistance(player) <= g_radius);
			if (hit) {
				ActorDetail(o, a, player, false);
				++nearby;
			}
		}
		o.L(std::format("{} NPCs listed", nearby));

		// ---- quests
		o.H("RUNNING QUESTS (full detail)");
		std::vector<RE::TESQuest*> completed;
		for (auto q : dh->GetFormArray<RE::TESQuest>()) {
			if (!q) {
				continue;
			}
			if (q->IsCompleted()) {
				completed.push_back(q);
			}
			if (!(q->IsRunning() || q->IsStarting() || q->IsStopping())) {
				continue;
			}
			o.L("--------------------------------------------------------------------------------");
			o.L("QUEST " + Label(q));
			o.L(std::format("  stage {} | running {} | starting {} | stopping {} | completed {} | active in journal {} | already run {} | type {}", q->GetCurrentStageID(),
				q->IsRunning(), q->IsStarting(), q->IsStopping(), q->IsCompleted(), q->IsActive(), q->alreadyRun, static_cast<int>(q->GetType())));
			for (auto obj : q->objectives) {
				if (obj) {
					o.L(std::format("  objective {:>4}  {:<18} {}", obj->index, ObjState(obj->state.get()), obj->displayText.c_str() ? obj->displayText.c_str() : ""));
				}
			}
			for (auto sc : q->scenes) {
				if (sc && sc->isPlaying) {
					o.L("  scene PLAYING: " + Label(sc));
				}
			}
			RE::BSReadLockGuard l{ q->aliasAccessLock };
			for (auto al : q->aliases) {
				if (!al) {
					continue;
				}
				std::string filled = "(empty / not a reference alias)";
				if (const auto it = q->refAliasMap.find(al->aliasID); it != q->refAliasMap.end()) {
					if (const auto r = it->second.get()) {
						filled = RefLabel(r.get());
						if (r->GetParentCell()) {
							filled += "  @ " + Where(r.get());
						}
					}
				}
				o.L(std::format("  alias {:>3} {:<32} {}", al->aliasID, al->aliasName.c_str() ? al->aliasName.c_str() : "", filled));
			}
		}
		o.H("COMPLETED QUESTS");
		for (auto q : completed) {
			o.L(std::format("  stage {:>4}  {}", q->GetCurrentStageID(), Label(q)));
		}

		// ---- every NPC's factions
		o.H(std::format("EVERY NPC IN MEMORY ({} actors): location and factions", actors.size()));
		for (auto a : actors) {
			o.L(RefLabel(a) + std::format("  | dead {} disabled {}", a->IsDead(false), a->IsDisabled()));
			o.L("    " + Where(a));
			o.f << Factions(a);
		}

		// ---- load order and SKSE plugins
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
		o.H("SKSE PLUGINS (Data\\SKSE\\Plugins\\*.dll)");
		std::error_code ec;
		for (auto& e : fs::directory_iterator("Data\\SKSE\\Plugins", ec)) {
			if (e.is_regular_file(ec) && e.path().extension() == ".dll") {
				o.L(std::format("  {:<48} {:>10} bytes", e.path().filename().string(), e.file_size(ec)));
			}
		}
		o.L("\n(end of report)");
	}

	// ---------------------------------------------------------------- file copies (background thread)
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

	void CopyFiles(fs::path a_myGames, fs::path a_skseDir, fs::path a_outRoot, fs::path a_outDir, fs::path a_savesDir)
	{
		std::vector<std::string> errors;
		std::error_code          ec;
		CopyTree(a_skseDir, a_outDir / "Logs" / "SKSE", a_outRoot, errors);
		CopyTree(a_myGames / "Logs", a_outDir / "Logs" / "Logs", a_outRoot, errors);

		std::vector<fs::directory_entry> saves;
		for (auto& e : fs::directory_iterator(a_savesDir, ec)) {
			if (e.is_regular_file(ec) && e.path().extension() == ".ess") {
				saves.push_back(e);
			}
		}
		std::sort(saves.begin(), saves.end(), [](auto& x, auto& y) {
			std::error_code e1;
			return x.last_write_time(e1) > y.last_write_time(e1);
		});
		fs::create_directories(a_outDir / "Saves", ec);
		for (int i = 0; i < static_cast<int>(saves.size()) && i < g_saves; ++i) {
			for (auto ext : { ".ess", ".skse" }) {
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
		logger::info("Snapshot files copied to {} ({} errors)", a_outDir.string(), errors.size());
		const auto msg = "Data Collected\n\n" + a_outDir.string();
		if (const auto tasks = SKSE::GetTaskInterface()) {
			tasks->AddTask([msg]() { RE::DebugMessageBox(msg.c_str()); });
		}
	}

	std::atomic<bool> g_busy{ false };

	void TakeSnapshot()
	{
		if (g_busy.exchange(true)) {
			return;
		}
		auto skseDir = SKSE::log::log_directory();
		if (!skseDir) {
			g_busy = false;
			return;
		}
		const fs::path myGames = skseDir->parent_path();
		const fs::path outRoot = *skseDir / "Snapshot";
		SYSTEMTIME     t{};
		GetLocalTime(&t);
		const fs::path outDir = outRoot / std::format("Snapshot_{:04}-{:02}-{:02}_{:02}-{:02}-{:02}", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
		std::error_code ec;
		fs::create_directories(outDir, ec);

		std::string savePath = "Saves\\";
		if (const auto ini = RE::INISettingCollection::GetSingleton()) {
			if (const auto s = ini->GetSetting("sLocalSavePath:General"); s && s->GetString() && *s->GetString()) {
				savePath = s->GetString();
			}
		}
		const fs::path savesDir = myGames / savePath;

		logger::info("Snapshot: writing report to {}", outDir.string());
		ShowNotification("Snapshot: collecting data...");
		WriteReport(outDir / "Snapshot report.txt");  // main thread: reads live game data
		std::thread([=]() {
			CopyFiles(myGames, *skseDir, outRoot, outDir, savesDir);
			g_busy = false;
		}).detach();
	}

	// ---------------------------------------------------------------- hotkey
	class InputSink : public RE::BSTEventSink<RE::InputEvent*>
	{
	public:
		RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_event, RE::BSTEventSource<RE::InputEvent*>*) override
		{
			for (auto e = a_event ? *a_event : nullptr; e; e = e->next) {
				if (e->GetEventType() != RE::INPUT_EVENT_TYPE::kButton || e->GetDevice() != RE::INPUT_DEVICES::kKeyboard) {
					continue;
				}
				const auto b = e->AsButtonEvent();
				if (!b || b->GetIDCode() != g_key) {
					continue;
				}
				if (!b->IsPressed()) {
					_fired = false;
				} else if (!_fired && b->HeldDuration() >= g_holdSecs) {
					_fired = true;
					TakeSnapshot();
				}
			}
			return RE::BSEventNotifyControl::kContinue;
		}

	private:
		bool _fired{ false };
	};

	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		if (a_msg->type == SKSE::MessagingInterface::kInputLoaded) {
			static InputSink sink;
			if (const auto mgr = RE::BSInputDeviceManager::GetSingleton()) {
				mgr->AddEventSink(&sink);
				logger::info("Hotkey listener installed");
			}
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);
	logger::info("Snapshot 1.0.0 loading; game runtime {}", REL::Module::get().version().string());
	LoadSettings();
	SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
	return true;
}
