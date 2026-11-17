#include "PCH.h"
#include "LE.h"
#include "Papyrus.h"

#include <cmath>

// Snapshot for Skyrim Legendary Edition (SKSE 1.7.3, TESV.exe 1.9.32.0) - hold a key (default L) for 8 seconds in
// game: writes a troubleshooting report (running quests with objectives, aliases and playing scenes, player,
// followers, NPCs around the player with factions and AI packages, every NPC in memory, load order) and copies
// the newest saves and all SKSE / Papyrus logs.
//
// Addresses (TESV.exe 1.9.32.0), each checked in the decrypted game code and in SKSE 1.7.3 / libSkyrim:
//   0x012E59D4  the game's table of every form (pointer); 0x012E59E4 its lock. LookupFormByID (0x00451A30) reads it:
//               read-lock 0x00A4AEE0, unlock 0x00A4B0F0 (thiscall on the lock); the table at +4 is a BSTScatterTable
//               (+4 capacity, +0x18 entries of 12 bytes: key, form, next; next 0 = empty slot)
//   0x01B1160C  Sky* (a pointer: the game reads it with mov eax,[0x01B1160C]); Sky +0x2C current weather
//   0x004D9290  Actor::IsSneaking (libSkyrim)
//   0x006E1340  Actor::IsInCombat (libSkyrim; SKSE declares it virtual without a body)
//   BGSLocation +0x28 parent (the game's Location.IsChild 0x008E9FC0 walks it upward)
//   Actor +0x7C flags1 (0x2 AI enabled, 0x04000000 teammate), +0x90 combat target handle, +0x138 flags2
//   (0x10000 commanded, 0x40000 essential) - libSkyrim Actor.h, the same bits as on SE
//   MagicTarget (Actor +0x54) vtable slot 7 GetActiveEffects (SKSE; 0x006E9180)
//   BGSScene +0x50 owning quest, +0x5C is playing (libSkyrim)

namespace fs = std::filesystem;

namespace
{
	constexpr auto          kVersion = "1.0.0";
	constexpr std::uint32_t kCurrentFollowerFaction = 0x0005C84E;  // Skyrim.esm CurrentFollowerFaction
	constexpr std::uint32_t kGameHour = 0x00000038;                // Skyrim.esm GameHour
	constexpr std::uint32_t kGameDaysPassed = 0x00000039;          // Skyrim.esm GameDaysPassed

	const char* kAVNames[] = {
"Aggression",
"Confidence",
"Energy",
"Morality",
"Mood",
"Assistance",
"OneHanded",
"TwoHanded",
"Archery",
"Block",
"Smithing",
"HeavyArmor",
"LightArmor",
"Pickpocket",
"Lockpicking",
"Sneak",
"Alchemy",
"Speech",
"Alteration",
"Conjuration",
"Destruction",
"Illusion",
"Restoration",
"Enchanting",
"Health",
"Magicka",
"Stamina",
"HealRate",
"MagickaRate",
"StaminaRate",
"SpeedMult",
"InventoryWeight",
"CarryWeight",
"CriticalChance",
"MeleeDamage",
"UnarmedDamage",
"Mass",
"VoicePoints",
"VoiceRate",
"DamageResist",
"PoisonResist",
"ResistFire",
"ResistShock",
"ResistFrost",
"ResistMagic",
"ResistDisease",
"PerceptionCondition",
"EnduranceCondition",
"LeftAttackCondition",
"RightAttackCondition",
"LeftMobilityCondition",
"RightMobilityCondition",
"BrainCondition",
"Paralysis",
"Invisibility",
"NightEye",
"DetectLifeRange",
"WaterBreathing",
"WaterWalking",
"IgnoreCrippledLimbs",
"Fame",
"Infamy",
"JumpingBonus",
"WardPower",
"RightItemCharge",
"ArmorPerks",
"ShieldPerks",
"WardDeflection",
"Variable01",
"Variable02",
"Variable03",
"Variable04",
"Variable05",
"Variable06",
"Variable07",
"Variable08",
"Variable09",
"Variable10",
"BowSpeedBonus",
"FavorActive",
"FavorsPerDay",
"FavorsPerDayTimer",
"LeftItemCharge",
"AbsorbChance",
"Blindness",
"WeaponSpeedMult",
"ShoutRecoveryMult",
"BowStaggerBonus",
"Telekinesis",
"FavorPointsBonus",
"LastBribedIntimidated",
"LastFlattered",
"MovementNoiseMult",
"BypassVendorStolenCheck",
"BypassVendorKeywordCheck",
"WaitingForPlayer",
"OneHandedModifier",
"TwoHandedModifier",
"MarksmanModifier",
"BlockModifier",
"SmithingModifier",
"HeavyArmorModifier",
"LightArmorModifier",
"PickpocketModifier",
"LockpickingModifier",
"SneakingModifier",
"AlchemyModifier",
"SpeechcraftModifier",
"AlterationModifier",
"ConjurationModifier",
"DestructionModifier",
"IllusionModifier",
"RestorationModifier",
"EnchantingModifier",
"OneHandedSkillAdvance",
"TwoHandedSkillAdvance",
"MarksmanSkillAdvance",
"BlockSkillAdvance",
"SmithingSkillAdvance",
"HeavyArmorSkillAdvance",
"LightArmorSkillAdvance",
"PickpocketSkillAdvance",
"LockpickingSkillAdvance",
"SneakingSkillAdvance",
"AlchemySkillAdvance",
"SpeechcraftSkillAdvance",
"AlterationSkillAdvance",
"ConjurationSkillAdvance",
"DestructionSkillAdvance",
"IllusionSkillAdvance",
"RestorationSkillAdvance",
"EnchantingSkillAdvance",
"LeftWeaponSpeedMultiply",
"DragonSouls",
"CombatHealthRegenMultiply",
"OneHandedPowerModifier",
"TwoHandedPowerModifier",
"MarksmanPowerModifier",
"BlockPowerModifier",
"SmithingPowerModifier",
"HeavyArmorPowerModifier",
"LightArmorPowerModifier",
"PickpocketPowerModifier",
"LockpickingPowerModifier",
"SneakingPowerModifier",
"AlchemyPowerModifier",
"SpeechcraftPowerModifier",
"AlterationPowerModifier",
"ConjurationPowerModifier",
"DestructionPowerModifier",
"IllusionPowerModifier",
"RestorationPowerModifier",
"EnchantingPowerModifier",
"DragonRend",
"AttackDamageMult",
"HealRateMult",
"MagickaRateMult",
"StaminaRateMult",
"WerewolfPerks",
"VampirePerks",
"GrabActorOffset",
"Grabbed",
"DEPRECATED05",
"ReflectDamage"
	};

	PluginHandle            g_handle = kPluginHandle_Invalid;
	SKSEMessagingInterface* g_messaging = nullptr;
	SKSETaskInterface*      g_task = nullptr;
	std::atomic<bool>       g_inGame{ false };
	std::atomic<bool>       g_busy{ false };
	std::string             g_runtime;
	int                     g_key = 0x4C;  // Windows virtual-key code: L
	float                   g_holdSecs = 8.0f;
	int                     g_saves = 3;
	float                   g_radius = 10000.0f;

	class Task : public TaskDelegate
	{
	public:
		explicit Task(std::function<void()> a_fn) :
			_fn(std::move(a_fn))
		{}
		virtual void Run() override
		{
			try {
				_fn();
			} catch (...) {
				spdlog::error("main thread task failed");
			}
		}
		virtual void Dispose() override { delete this; }

	private:
		std::function<void()> _fn;
	};

	void AddTask(std::function<void()> a_fn)
	{
		if (g_task) {
			g_task->AddTask(new Task(std::move(a_fn)));
		}
	}

	std::string IniString(const fs::path& a_ini, const char* a_key, const char* a_default)
	{
		char buf[256]{};
		GetPrivateProfileStringA("General", a_key, a_default, buf, sizeof(buf), a_ini.string().c_str());
		return buf;
	}

	void LoadSettings()
	{
		const auto ini = fs::absolute("Data\\SKSE\\Plugins\\Snapshot.ini");
		g_key = std::atoi(IniString(ini, "iHotkey", "76").c_str());
		g_holdSecs = std::strtof(IniString(ini, "fHoldSeconds", "8").c_str(), nullptr);
		g_saves = std::atoi(IniString(ini, "iSavesToCopy", "3").c_str());
		g_radius = std::strtof(IniString(ini, "fOutdoorRadius", "10000").c_str(), nullptr);
		spdlog::info("Settings: key {} hold {}s saves {} radius {}", g_key, g_holdSecs, g_saves, g_radius);
	}

	std::string Stamp(bool a_file)
	{
		SYSTEMTIME t{};
		GetLocalTime(&t);
		return a_file ? std::format("{:04}-{:02}-{:02}_{:02}-{:02}-{:02}", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond)
		              : std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
	}

	template <class T>
	T At(const void* a_base, std::size_t a_off)
	{
		return *reinterpret_cast<const T*>(reinterpret_cast<const std::uint8_t*>(a_base) + a_off);
	}

	// ---------------------------------------------------------------- game functions (see the top of the file)
	std::vector<TESForm*> AllForms(UInt8 a_type)
	{
		std::vector<TESForm*> out;
		const auto map = *reinterpret_cast<std::uint8_t**>(0x012E59D4);
		if (!map) {
			return out;
		}
		using lock_t = void(__thiscall*)(void*);
		void* lock = reinterpret_cast<void*>(0x012E59E4);
		reinterpret_cast<lock_t>(0x00A4AEE0)(lock);
		try {
			const auto* table = map + 4;
			const auto  capacity = At<UInt32>(table, 0x04);
			const auto* entries = At<const std::uint8_t*>(table, 0x18);
			for (UInt32 i = 0; entries && i < capacity; ++i) {
				const auto* e = entries + i * 12;
				if (At<UInt32>(e, 8) == 0) {
					continue;  // empty slot
				}
				auto* form = At<TESForm*>(e, 4);
				if (form && form->formType == a_type) {
					out.push_back(form);
				}
			}
		} catch (...) {
			spdlog::error("reading the form table failed");
		}
		reinterpret_cast<lock_t>(0x00A4B0F0)(lock);
		std::sort(out.begin(), out.end(), [](auto a, auto b) { return a->formID < b->formID; });
		return out;
	}

	bool IsSneaking(Actor* a) { return reinterpret_cast<bool(__thiscall*)(Actor*)>(0x004D9290)(a); }
	bool InCombat(Actor* a) { return reinterpret_cast<bool(__thiscall*)(Actor*)>(0x006E1340)(a); }
	UInt32 Flags1(Actor* a) { return At<UInt32>(a, 0x7C); }
	UInt32 Flags2(Actor* a) { return At<UInt32>(a, 0x138); }
	bool   Loaded(TESObjectREFR* r) { return r && r->loadedState != nullptr; }
	bool   Disabled(TESForm* f) { return f && (f->flags & 0x800) != 0; }

	TESObjectREFR* CombatTarget(Actor* a)
	{
		UInt32         handle = At<UInt32>(a, 0x90);
		TESObjectREFR* ref = nullptr;
		if (handle && handle != *g_invalidRefHandle) {
			LookupREFRByHandle(&handle, &ref);
		}
		return ref;
	}

	tList<ActiveEffect>* ActiveEffects(Actor* a)
	{
		auto* mt = &a->magicTarget;
		using F = tList<ActiveEffect>*(__thiscall*)(MagicTarget*);
		return reinterpret_cast<F>((*reinterpret_cast<void***>(mt))[7])(mt);
	}

	// ActorValueOwner through the game's vtable (01 current, 02 maximum, 03 base; as ReSaverHelper LE calls 03)
	float AV(Actor* a_actor, int a_slot, UInt32 a_av)
	{
		auto* owner = &a_actor->actorValueOwner;
		using F = float(__thiscall*)(ActorValueOwner*, UInt32);
		return reinterpret_cast<F>((*reinterpret_cast<void***>(owner))[a_slot])(owner, a_av);
	}

	// ---------------------------------------------------------------- text helpers
	std::string Label(TESForm* a_form)
	{
		if (!a_form) {
			return "(none)";
		}
		auto plugin = le::PluginOf(a_form);
		return std::format("\"{}\" {:08X} <{}>", le::NameOf(a_form), a_form->formID, plugin.empty() ? std::string("created in game") : plugin);
	}

	std::string RefLabel(TESObjectREFR* a_ref)
	{
		if (!a_ref) {
			return "(none)";
		}
		auto name = le::NameOf(a_ref);
		if (name.empty()) {
			name = le::NameOf(a_ref->baseForm);
		}
		return std::format("\"{}\" ref {:08X} <{}>  base {:08X} <{}>", name, a_ref->formID, le::PluginOf(a_ref), a_ref->baseForm ? a_ref->baseForm->formID : 0u,
			le::PluginOf(a_ref->baseForm));
	}

	bool Interior(TESObjectCELL* a_cell) { return a_cell && (a_cell->unk2C & 1) != 0; }  // TESObjectCELL flags: 1 = interior
	TESWorldSpace* WorldOf(TESObjectREFR* r) { return r->parentCell && !Interior(r->parentCell) ? r->parentCell->unk84 : nullptr; }

	float Dist(TESObjectREFR* a, TESObjectREFR* b)
	{
		const float dx = a->pos.x - b->pos.x, dy = a->pos.y - b->pos.y, dz = a->pos.z - b->pos.z;
		return std::sqrt(dx * dx + dy * dy + dz * dz);
	}

	std::string Where(TESObjectREFR* a_ref)
	{
		auto*       cell = a_ref->parentCell;
		std::string out = cell ? "cell " + Label(cell) + (Interior(cell) ? " (interior)" : " (exterior)") : std::string("cell (not loaded)");
		if (cell && !Interior(cell) && cell->unk84) {
			out += " | worldspace " + Label(cell->unk84);
		}
		std::string chain;
		for (auto* loc = a_ref->GetCurrentLocation(); loc; loc = At<BGSLocation*>(loc, 0x28)) {
			chain += (chain.empty() ? "" : " > ") + Label(loc);
		}
		if (!chain.empty()) {
			out += " | location " + chain;
		}
		return out;
	}

	std::string Pos(TESObjectREFR* a_ref)
	{
		return std::format("x {:.1f}  y {:.1f}  z {:.1f}  facing {:.0f} deg", a_ref->pos.x, a_ref->pos.y, a_ref->pos.z, a_ref->rot.z * 180.0f / 3.14159265f);
	}

	const char* ObjState(UInt32 a_s)
	{
		switch (a_s & 0xFF) {
		case 0: return "dormant";
		case 1: return "DISPLAYED";
		case 2: return "completed";
		case 3: return "completed (shown)";
		case 4: return "failed";
		case 5: return "failed (shown)";
		default: return "?";
		}
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

	// collected on the main thread before writing; current packages and crime gold come back from Papyrus
	struct Collected
	{
		std::vector<Actor*>         all;
		std::vector<Actor*>         followers;
		std::vector<std::string>    followerWhy;
		std::vector<Actor*>         nearby;
		std::map<UInt32, TESForm*>  currentPackage;
		std::map<UInt32, SInt32>    crimeGold;
		std::mutex                  lock;
	};

	std::string Factions(Actor* a)
	{
		std::string out;
		for (const auto& [f, r] : le::Factions(a)) {
			out += std::format("    {} rank {}\n", Label(f), static_cast<int>(r));
		}
		return out;
	}

	void ActorDetail(Out& o, Actor* a, Actor* a_player, bool a_isPlayer, Collected& c)
	{
		auto* npc = le::BaseNPC(a);
		o.L("--------------------------------------------------------------------------------");
		o.L(RefLabel(a));
		o.L("  base NPC   " + Label(npc));
		const bool female = npc && CALL_MEMBER_FN(npc, GetSex)() == 1;
		o.L("  race       " + Label(a->race) + std::format("   sex {}   level {}", female ? "female" : "male", le::GetLevel(a)));
		o.L("  position   " + Pos(a));
		o.L("  where      " + Where(a));
		const bool loaded = Loaded(a);
		if (!a_isPlayer) {
			o.L(std::format("  distance to player {:.0f} units   3D loaded {}", Dist(a, a_player), loaded));
		}
		const UInt32 f1 = Flags1(a), f2 = Flags2(a);
		o.L(std::format("  state      dead {} | disabled {} | in combat {} | hostile to player {} | essential {} | AI enabled {} | teammate {} | commanded {} | sneaking {} | weapon drawn {}",
			a->IsDead(1), Disabled(a), InCombat(a), (a_isPlayer || !loaded) ? false : CALL_MEMBER_FN(a, IsHostileToActor)(a_player), (f2 & 0x40000) != 0,
			(f1 & 0x2) != 0, (f1 & 0x04000000) != 0, (f2 & 0x10000) != 0, IsSneaking(a), a->actorState.IsWeaponDrawn()));
		if (auto* t = CombatTarget(a)) {
			o.L("  combat target " + RefLabel(t));
		}
		if (!a_isPlayer) {
			TESForm* pkg = nullptr;
			{
				std::scoped_lock l(c.lock);
				if (auto it = c.currentPackage.find(a->formID); it != c.currentPackage.end()) {
					pkg = it->second;
				}
			}
			o.L("  CURRENT PACKAGE " + Label(pkg));
			o.L("  packages on NPC (in priority order):");
			if (npc) {
				for (auto* d = &npc->aiForm.unk10; d; d = d->next) {
					if (d->unk0) {
						o.L("    " + Label(reinterpret_cast<TESForm*>(d->unk0)));
					}
				}
			}
		}
		o.L("  factions:");
		o.f << Factions(a);
		o.L("  stats (current / maximum / base):");
		for (UInt32 i = 0; i < std::size(kAVNames); ++i) {
			o.L(std::format("    {:<28} {:>10.2f} {:>10.2f} {:>10.2f}", kAVNames[i], AV(a, 1, i), AV(a, 2, i), AV(a, 3, i)));
		}
		o.L("  equipped: right " + Label(a->GetEquippedObject(false)) + " | left " + Label(a->GetEquippedObject(true)));
		o.L("  spells:");
		if (npc && npc->spellList.unk04) {
			for (UInt32 i = 0; i < npc->spellList.unk04->numSpells; ++i) {
				if (auto* sp = npc->spellList.unk04->spells[i]) {
					o.L("    (base) " + Label(sp));
				}
			}
		}
		for (UInt32 i = 0; i < a->addedSpells.Length(); ++i) {
			if (auto* sp = a->addedSpells.Get(i)) {
				o.L("    (added) " + Label(sp));
			}
		}
		o.L("  active magic effects:");
		if (auto* list = ActiveEffects(a)) {
			for (auto it = list->Begin(); !it.End(); ++it) {
				auto* ae = it.Get();
				if (!ae) {
					continue;
				}
				o.L(std::format("    {} from {}  magnitude {:.1f}  {:.0f}/{:.0f} s{}", Label(ae->effect ? ae->effect->mgef : nullptr), Label(ae->item), ae->magnitude,
					ae->elapsed, ae->duration, (ae->flags & 0x8000) ? "  (inactive)" : ""));
			}
		}
	}

	void Collect(Collected& c)
	{
		auto* player = le::Player();
		auto* faction = le::LookupAs<TESFaction>(kCurrentFollowerFaction);
		const bool inside = Interior(player->parentCell);
		for (auto* f : AllForms(kFormType_Character)) {
			auto* a = static_cast<Actor*>(f);
			if (a == player) {
				continue;
			}
			c.all.push_back(a);
			std::string why;
			if (Flags1(a) & 0x04000000) {
				why = "teammate";
			} else if (faction && le::FactionRank(a, faction) >= 0) {
				why = "follower faction";
			} else if (Flags2(a) & 0x10000) {
				why = "commanded (summon)";
			}
			if (!why.empty() && !a->IsDead(1)) {
				c.followers.push_back(a);
				c.followerWhy.push_back(why);
			}
			if (Disabled(a)) {
				continue;
			}
			if (inside ? a->parentCell == player->parentCell : (WorldOf(a) && WorldOf(a) == WorldOf(player) && Dist(a, player) <= g_radius)) {
				c.nearby.push_back(a);
			}
		}
	}

	void WriteReport(const fs::path& a_file, Collected& c)
	{
		Out o;
		o.f.open(a_file, std::ios::out | std::ios::trunc);
		if (!o.f) {
			spdlog::error("cannot write {}", a_file.string());
			return;
		}
		auto* player = le::Player();
		auto* dh = le::Data();
		o.L(std::format("Snapshot {} troubleshooting report - Skyrim Legendary Edition", kVersion));
		o.L(std::format("Taken {}   game runtime {}", Stamp(false), g_runtime));
		auto* hour = le::LookupAs<TESGlobal>(kGameHour);
		auto* days = le::LookupAs<TESGlobal>(kGameDaysPassed);
		o.L(std::format("Game days passed {:.2f}   hour {:.2f}", days ? days->value : 0.0f, hour ? hour->value : 0.0f));
		if (auto* sky = *reinterpret_cast<std::uint8_t**>(0x01B1160C)) {
			o.L("Weather: " + Label(At<TESForm*>(sky, 0x2C)));
		}
		if (auto* mm = MenuManager::GetSingleton()) {
			std::string menus;
			for (auto* m : { "Console", "Dialogue Menu", "InventoryMenu", "ContainerMenu", "BarterMenu", "MapMenu", "Journal Menu", "Sleep/Wait Menu", "MessageBoxMenu",
					 "Lockpicking Menu", "Crafting Menu", "Book Menu", "TweenMenu", "StatsMenu", "MagicMenu", "FavoritesMenu", "Loading Menu", "Training Menu", "GiftMenu" }) {
				BSFixedString name(m);
				if (mm->IsMenuOpen(&name)) {
					menus += std::string(menus.empty() ? "" : ", ") + m;
				}
			}
			o.L("Open menus: " + (menus.empty() ? std::string("none") : menus));
		}

		o.H("PLAYER");
		ActorDetail(o, player, player, true, c);
		o.L("  perks:");
		for (UInt32 i = 0; i < dh->perks.count; ++i) {
			if (auto* perk = dh->perks.arr.entries[i]; perk && le::HasPerk(player, perk)) {
				o.L("    " + Label(perk));
			}
		}
		o.L("  bounties (crime gold per crime faction):");
		{
			std::scoped_lock l(c.lock);
			for (const auto& [id, gold] : c.crimeGold) {
				if (gold > 0) {
					o.L(std::format("    {}  {} gold", Label(LookupFormByID(id)), gold));
				}
			}
		}
		o.L("  inventory (count, worn):");
		for (const auto& it : le::Inventory(player)) {
			if (it.count == 0) {
				continue;
			}
			bool worn = false;
			for (auto* xl : it.extras) {
				worn = worn || (xl && (xl->HasType(kExtraData_Worn) || xl->HasType(kExtraData_WornLeft)));
			}
			o.L(std::format("    {:>6}  {}{}", it.count, Label(it.form), worn ? "  [WORN]" : ""));
		}

		o.H("FOLLOWERS (teammates, CurrentFollowerFaction members, summons)");
		for (std::size_t i = 0; i < c.followers.size(); ++i) {
			o.L("  (" + c.followerWhy[i] + ")");
			ActorDetail(o, c.followers[i], player, false, c);
		}

		const bool inside = Interior(player->parentCell);
		o.H(inside ? "NPCs IN THE SAME INTERIOR AS THE PLAYER" : std::format("NPCs WITHIN {:.0f} UNITS OF THE PLAYER (outdoors)", g_radius));
		for (auto* a : c.nearby) {
			ActorDetail(o, a, player, false, c);
		}
		o.L(std::format("{} NPCs listed", c.nearby.size()));

		// playing scenes, by quest
		std::map<TESForm*, std::vector<TESForm*>> scenes;
		for (auto* s : AllForms(kFormType_Scene)) {
			if (At<bool>(s, 0x5C)) {
				scenes[At<TESForm*>(s, 0x50)].push_back(s);
			}
		}

		o.H("RUNNING QUESTS (full detail)");
		std::vector<TESQuest*> completed;
		for (UInt32 qi = 0; qi < dh->quests.count; ++qi) {
			auto* q = dh->quests.arr.entries[qi];
			if (!q) {
				continue;
			}
			if (le::Completed(q)) {
				completed.push_back(q);
			}
			if (!le::Running(q)) {
				continue;
			}
			const UInt16 fl = q->unk07C.flags;
			o.L("--------------------------------------------------------------------------------");
			o.L(std::format("QUEST \"{}\" [{}] {:08X} <{}>", le::NameOf(q), le::EditorID(q), q->formID, le::PluginOf(q)));
			o.L(std::format("  stage {} | completed {} | failed {} | active in journal {} | flags {:04X}", q->currentStageID, le::Completed(q), (fl & 0x40) != 0,
				(fl & 0x800) != 0, fl));
			for (auto it = q->objectives.Begin(); !it.End(); ++it) {
				if (auto* ob = it.Get()) {
					o.L(std::format("  objective {:>4}  {:<18} {}", ob->index, ObjState(ob->unk14), ob->displayText.data ? ob->displayText.data : ""));
				}
			}
			if (auto sit = scenes.find(q); sit != scenes.end()) {
				for (auto* s : sit->second) {
					o.L("  scene PLAYING: " + Label(s));
				}
			}
			for (UInt32 ai = 0; ai < q->aliases.count; ++ai) {
				auto* alias = q->aliases.arr.entries[ai];
				if (!alias) {
					continue;
				}
				auto*       refAlias = le::Cast<BGSRefAlias>(alias, RTTI_BGSBaseAlias, RTTI_BGSRefAlias);
				std::string filled = "(empty / not a reference alias)";
				if (auto* r = refAlias ? refAlias->GetReference() : nullptr) {
					filled = RefLabel(r) + "  @ " + Where(r);
				}
				o.L(std::format("  alias {:>3} {:<32} {}", alias->aliasId, alias->name.data ? alias->name.data : "", filled));
			}
		}
		o.H("COMPLETED QUESTS");
		for (auto* q : completed) {
			o.L(std::format("  stage {:>4}  \"{}\" [{}] {:08X} <{}>", q->currentStageID, le::NameOf(q), le::EditorID(q), q->formID, le::PluginOf(q)));
		}

		o.H(std::format("EVERY NPC IN MEMORY ({} actors): location and factions", c.all.size() + 1));
		for (auto* a : c.all) {
			o.L(RefLabel(a) + std::format("  | dead {} disabled {}", a->IsDead(1), Disabled(a)));
			o.L("    " + Where(a));
			o.f << Factions(a);
		}

		o.H("LOAD ORDER");
		for (UInt32 i = 0; i < dh->modList.loadedModCount && i < 0xFF; ++i) {
			if (auto* mod = dh->modList.loadedMods[i]) {
				o.L(std::format("  {:02X}  {}", i, mod->name));
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
		CopyTree(a_myGames / "SKSE", a_outDir / "Logs" / "SKSE", a_outRoot, errors);
		CopyTree(a_myGames / "Logs", a_outDir / "Logs" / "Logs", a_outRoot, errors);
		std::vector<fs::directory_entry> saves;
		for (auto& e : fs::directory_iterator(a_myGames / "Saves", ec)) {
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
		spdlog::info("Snapshot files copied to {} ({} errors)", a_outDir.string(), errors.size());
		AddTask([]() { papyrus::CallStatic("Debug", "MessageBox", { papyrus::Arg::String("Data Collected") }); });
		g_busy = false;
	}

	void TakeSnapshot()
	{
		const fs::path skseDir = rshlog::Dir();  // Documents\My Games\Skyrim\SKSE
		if (skseDir.empty()) {
			g_busy = false;
			return;
		}
		const fs::path myGames = skseDir.parent_path();
		const fs::path outRoot = skseDir / "Snapshot";
		const fs::path outDir = outRoot / ("Snapshot_" + Stamp(true));
		std::error_code ec;
		fs::create_directories(outDir, ec);
		spdlog::info("Snapshot: collecting into {}", outDir.string());
		papyrus::CallStatic("Debug", "Notification", { papyrus::Arg::String("Snapshot: collecting data...") });

		auto c = std::make_shared<Collected>();
		Collect(*c);
		// answers from Papyrus arrive on later frames: each listed NPC's current AI package, and the player's crime gold
		std::vector<Actor*> ask = c->followers;
		ask.insert(ask.end(), c->nearby.begin(), c->nearby.end());
		for (auto* a : ask) {
			const UInt32 id = a->formID;
			papyrus::CallMethod(a, "Actor", "GetCurrentPackage", {}, [c, id](const papyrus::Result& r) {
				std::scoped_lock l(c->lock);
				c->currentPackage[id] = r.form;
			});
		}
		auto* dh = le::Data();
		for (UInt32 i = 0; i < dh->factions.count; ++i) {
			auto* f = dh->factions.arr.entries[i];
			if (f && (f->factionFlags & TESFaction::kFlag_TrackCrime)) {
				const UInt32 id = f->formID;
				papyrus::CallMethod(f, "Faction", "GetCrimeGold", {}, [c, id](const papyrus::Result& r) {
					if (r.IsInt()) {
						std::scoped_lock l(c->lock);
						c->crimeGold[id] = r.GetInt();
					}
				});
			}
		}
		std::thread([c, myGames, outRoot, outDir]() {
			std::this_thread::sleep_for(2s);
			AddTask([c, myGames, outRoot, outDir]() {
				WriteReport(outDir / "Snapshot report.txt", *c);
				std::thread([myGames, outRoot, outDir]() { CopyFiles(myGames, outRoot, outDir); }).detach();
			});
		}).detach();
	}

	void HotkeyThread()
	{
		bool  fired = false;
		float held = 0.0f;
		while (true) {
			std::this_thread::sleep_for(100ms);
			DWORD pid = 0;
			if (HWND w = GetForegroundWindow()) {
				GetWindowThreadProcessId(w, &pid);
			}
			const bool down = g_inGame && pid == GetCurrentProcessId() && (GetAsyncKeyState(g_key) & 0x8000) != 0;
			if (!down) {
				held = 0.0f;
				fired = false;
				continue;
			}
			held += 0.1f;
			if (!fired && held >= g_holdSecs) {
				fired = true;
				if (!g_busy.exchange(true)) {
					AddTask([]() { TakeSnapshot(); });
				}
			}
		}
	}

	void OnMessage(SKSEMessagingInterface::Message* a_msg)
	{
		if (!a_msg) {
			return;
		}
		switch (a_msg->type) {
		case SKSEMessagingInterface::kMessage_PreLoadGame:
			g_inGame = false;
			break;
		case SKSEMessagingInterface::kMessage_PostLoadGame:
			g_inGame = a_msg->data != nullptr;
			break;
		case SKSEMessagingInterface::kMessage_NewGame:
			g_inGame = true;
			break;
		default:
			break;
		}
	}
}

extern "C"
{
	__declspec(dllexport) bool SKSEPlugin_Query(const SKSEInterface* a_skse, PluginInfo* a_info)
	{
		rshlog::Open();
		a_info->infoVersion = PluginInfo::kInfoVersion;
		a_info->name = "Snapshot";
		a_info->version = 1;
		g_runtime = std::format("1.{}.{}.{}", (a_skse->runtimeVersion >> 24) & 0xFF, (a_skse->runtimeVersion >> 16) & 0xFF, (a_skse->runtimeVersion >> 4) & 0xFFF);
		spdlog::info("Snapshot {} (Legendary Edition) - game {}", kVersion, g_runtime);
		if (a_skse->isEditor) {
			return false;
		}
		if (a_skse->runtimeVersion != RUNTIME_VERSION_1_9_32_0) {
			spdlog::error("Snapshot is for Skyrim 1.9.32.0 (the last Legendary Edition patch); not loaded");
			return false;
		}
		g_handle = a_skse->GetPluginHandle();
		return true;
	}

	__declspec(dllexport) bool SKSEPlugin_Load(const SKSEInterface* a_skse)
	{
		LoadSettings();
		g_task = static_cast<SKSETaskInterface*>(a_skse->QueryInterface(kInterface_Task));
		g_messaging = static_cast<SKSEMessagingInterface*>(a_skse->QueryInterface(kInterface_Messaging));
		if (!g_task || !g_messaging || !g_messaging->RegisterListener(g_handle, "SKSE", OnMessage)) {
			spdlog::error("SKSE task or messaging interface unavailable; Snapshot cannot start");
			return false;
		}
		std::thread(HotkeyThread).detach();
		return true;
	}
}
