// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 WarcraftXL
//
// wxl.equip-plus: 3D models on every armour slot, for the recompiled x64 client.
//
// A port of wxl-equip-plus 1.3.0 (TheDaveCalaz/wxl-equip-plus, itself a continuation of
// Morfium-G/wxl-equip-extension and Atraxian123's 1.1 port) from the 32-bit extension ABI, which
// detoured the client's code, to the x64 mod ABI, which does not:
//
//   - wxl.m2 hands over every item an appearance puts on and takes off (players, dressed NPCs,
//     character select, the dressing room), with the character model to hang models on;
//   - for each, the model list is resolved exactly as upstream resolves it -- WXLItemDisplayModels
//     CSV rows first, else ItemDisplayInfo's ModelName_1/2 with the Icon2 attachment and flag
//     column, the same race/gender suffix rules, folders and stale-row guards;
//   - each model is made with wxl.m2, textured (ModelTexture, then WXLItemDisplayModelMaterials
//     rows by texture type), filtered to its geosets, and attached. A collection piece on the root
//     attachment follows the body's bones, which is what the old BuildBonePalette detour did.
//
// Every model the mod makes it holds a reference to, per wearer and slot, and lets go of when the
// item comes off, the wearer's character model is replaced, or the wearer is freed.
//
// Payload: every archive in <mod>\data\ (a .MPQ file or an open .MPQ folder, such as the
// Backport Helper's patch-W.MPQ) is mounted when storage comes up, so the pieces and the CSVs
// travel with the mod instead of being copied into the client's Data folder.

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "wxl/sdk.hpp"
#include "wxl/services/m2.h"
#include "wxl/services/script.hpp"
#include "wxl/services/storage.hpp"

#include "PhysicsLive.hpp"

namespace
{
    constexpr const char* kTag = "equip-plus";

    wxl::Service<wxl_m2_api> g_m2(WXL_M2_SERVICE, WXL_M2_SERVICE_VERSION);
    const wxl::StorageService g_storage;

    // --- Settings: <mod>\WXLExtendedEquipment.ini --------------------------------------------------

    bool g_verbose = false; // [Log] Verbose=1: a line per piece resolved, attached or skipped

    std::string ModDirectory()
    {
        HMODULE self = nullptr;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCSTR>(&ModDirectory), &self))
            return {};
        char path[MAX_PATH];
        const DWORD n = GetModuleFileNameA(self, path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return {};
        std::string dir(path, n);
        // <mod>\bin\x64\wxl-equip-plus.dll -> <mod>
        for (int up = 0; up < 3; ++up)
        {
            const size_t slash = dir.find_last_of("\\/");
            if (slash == std::string::npos) return {};
            dir.resize(slash);
        }
        return dir;
    }

    void Info(const std::string& line) { wxl::Log(WXL_LOG_INFO, kTag, line); }
    void Warn(const std::string& line) { wxl::Log(WXL_LOG_WARN, kTag, line); }
    void Verbose(const std::string& line)
    {
        if (g_verbose) wxl::Log(WXL_LOG_INFO, kTag, line);
    }

    template <class... A>
    std::string Format(const char* fmt, A... args)
    {
        char line[1024];
        std::snprintf(line, sizeof line, fmt, args...);
        return line;
    }

    // --- Strings, as upstream compares them ------------------------------------------------------

    bool StartsWithCI(const char* s, const char* prefix)
    {
        if (!s || !prefix) return false;
        while (*prefix)
        {
            if (std::tolower(static_cast<unsigned char>(*s++)) != std::tolower(static_cast<unsigned char>(*prefix++)))
                return false;
        }
        return true;
    }

    bool ContainsCI(const char* s, const char* needle)
    {
        if (!s || !needle || !*needle) return false;
        for (const char* p = s; *p; ++p)
        {
            const char* a = p;
            const char* b = needle;
            while (*a && *b && std::tolower(static_cast<unsigned char>(*a)) == std::tolower(static_cast<unsigned char>(*b)))
            {
                ++a;
                ++b;
            }
            if (!*b) return true;
        }
        return false;
    }

    std::string Trim(std::string value)
    {
        size_t first = 0;
        while (first < value.size() && (value[first] == ' ' || value[first] == '\t')) ++first;
        size_t last = value.size();
        while (last > first && std::strchr(" \t\r\n", value[last - 1])) --last;
        return value.substr(first, last - first);
    }

    std::string Lower(std::string value)
    {
        for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return value;
    }

    bool ParseU32(const std::string& text, uint32_t& out)
    {
        if (text.empty()) return false;
        char*               end   = nullptr;
        const unsigned long value = std::strtoul(text.c_str(), &end, 10);
        if (end == text.c_str()) return false;
        out = static_cast<uint32_t>(value);
        return true;
    }

    /// "1301,2301" -> {1301, 2301}; at most 16, as upstream.
    std::vector<uint16_t> ParseGeosets(const char* spec)
    {
        std::vector<uint16_t> ids;
        const char*           p = spec;
        while (p && *p && ids.size() < 16)
        {
            uint32_t v = 0;
            while (*p >= '0' && *p <= '9') v = v * 10 + static_cast<uint32_t>(*p++ - '0');
            ids.push_back(static_cast<uint16_t>(v));
            if (*p == ',') ++p;
            else break;
        }
        return ids;
    }

    /// Part `index` of a ';'-separated list, trimmed. False past the end.
    bool ListPart(const char* list, uint32_t index, std::string& out)
    {
        out.clear();
        if (!list) return false;
        const char* part = list;
        for (uint32_t cur = 0;; ++cur)
        {
            const char* end = part;
            while (*end && *end != ';') ++end;
            if (cur == index)
            {
                out = Trim(std::string(part, end));
                return true;
            }
            if (!*end) return false;
            part = end + 1;
        }
    }

    // --- Slots ----------------------------------------------------------------------------------

    struct SlotConfig
    {
        const char* folder;
        uint32_t    defAttach1;
        uint32_t    defAttach2;
        bool        defUseRace;
        bool        defUseGender;
    };

    // Indexed by the client's item slot (wxl_appearance_args::item_slot), as upstream's model slot.
    constexpr SlotConfig kSlots[11] = {
        {"Head", 11, 55, true, true},        // 0  head
        {"Shoulder", 6, 5, false, false},    // 1  shoulder (Model1 left/6, Model2 right/5)
        {"Shirt", 34, 34, false, false},     // 2  shirt
        {"Chest", 34, 34, false, false},     // 3  chest
        {"Waist", 53, 53, false, false},     // 4  belt
        {"Leg", 9, 10, false, false},        // 5  legs
        {"Foot", 47, 48, false, false},      // 6  feet
        {"Bracer", 3, 4, false, false},      // 7  wrists
        {"Glove", 1, 2, false, false},       // 8  hands
        {"Tabard", 34, 34, false, false},    // 9  tabard
        {"Cape", 12, 12, false, false},      // 10 cloak
    };
    constexpr uint32_t kNumSlots         = 11;
    constexpr uint32_t kCollectionAttach = 19; // Ground_Base: the character's root
    constexpr uint32_t kNoAttach         = UINT32_MAX;

    // Icon2 flags.
    constexpr uint32_t kFlagNoGender          = 0x2;
    constexpr uint32_t kFlagNoRace            = 0x4;
    constexpr uint32_t kFlagTexRace           = 0x8;
    constexpr uint32_t kFlagTexGender         = 0x10;
    constexpr uint32_t kFlagSubfolder         = 0x20;
    constexpr uint32_t kFlagNewSuffix         = 0x40;
    constexpr uint32_t kFlagForceModelRaceSex = 0x80;
    constexpr uint32_t kFlagsFromSlot         = UINT32_MAX; // a CSV policy of DBC/Slot

    uint32_t ParseSidecarSlot(const std::string& slot)
    {
        if (slot.empty()) return kNoAttach;
        uint32_t numeric = 0;
        if (ParseU32(slot, numeric)) return numeric < kNumSlots ? numeric : kNoAttach;
        const char* s = slot.c_str();
        if (StartsWithCI(s, "Head")) return 0;
        if (StartsWithCI(s, "Shoulder")) return 1;
        if (StartsWithCI(s, "Shirt")) return 2;
        if (StartsWithCI(s, "Chest") || StartsWithCI(s, "Robe")) return 3;
        if (StartsWithCI(s, "Waist") || StartsWithCI(s, "Belt")) return 4;
        if (StartsWithCI(s, "Leg") || StartsWithCI(s, "Pant")) return 5;
        if (StartsWithCI(s, "Foot") || StartsWithCI(s, "Feet") || StartsWithCI(s, "Boot")) return 6;
        if (StartsWithCI(s, "Bracer") || StartsWithCI(s, "Wrist")) return 7;
        if (StartsWithCI(s, "Glove") || StartsWithCI(s, "Hand")) return 8;
        if (StartsWithCI(s, "Cape") || StartsWithCI(s, "Back") || StartsWithCI(s, "Cloak")) return 10;
        if (StartsWithCI(s, "Tabard")) return 9;
        return kNoAttach; // weapons and anything else: any slot
    }

    uint32_t ModelFlagsForPolicy(const std::string& policy)
    {
        const char* p = policy.c_str();
        if (StartsWithCI(p, "DBC") || StartsWithCI(p, "Slot")) return kFlagsFromSlot;
        if (policy.empty() || StartsWithCI(p, "Exact") || StartsWithCI(p, "None")) return kFlagNoGender | kFlagNoRace;
        if (StartsWithCI(p, "Retail") || StartsWithCI(p, "New")) return kFlagNewSuffix;
        if (StartsWithCI(p, "Legacy") || StartsWithCI(p, "Race")) return 0;
        return kFlagNoGender | kFlagNoRace;
    }

    uint32_t TextureFlagsForPolicy(const std::string& policy)
    {
        const char* p = policy.c_str();
        if (StartsWithCI(p, "DBC") || StartsWithCI(p, "Slot")) return kFlagsFromSlot;
        if (StartsWithCI(p, "Retail") || StartsWithCI(p, "New")) return kFlagTexRace | kFlagTexGender | kFlagNewSuffix;
        if (StartsWithCI(p, "Legacy") || StartsWithCI(p, "Race")) return kFlagTexRace | kFlagTexGender;
        return 0;
    }

    void ParseAttachField(const char* start, const char* end, uint32_t& left)
    {
        uint32_t a = 0;
        const char* p = start;
        while (p < end && *p >= '0' && *p <= '9') a = a * 10 + static_cast<uint32_t>(*p++ - '0');
        left = a; // "a|b" names a right-side point too; upstream reads only the left one
    }

    /// Icon2: "<attach1>:<attach2>:<flags>:<folder>", every field optional.
    struct Icon2
    {
        bool        configured = false;
        bool        attachAExplicit = false;
        bool        attachBExplicit = false;
        uint32_t    attachA = 0;
        uint32_t    attachB = 0;
        uint32_t    flags = 0;
        std::string folder;
    };

    Icon2 ParseIcon2(const char* text, const SlotConfig& slot)
    {
        Icon2 icon;
        icon.attachA = slot.defAttach1;
        icon.attachB = slot.defAttach2;
        if (!text || !*text || !(*text == ':' || (*text >= '0' && *text <= '9'))) return icon;
        icon.configured = true;

        std::vector<std::string> fields;
        const char*              start = text;
        for (const char* p = text;; ++p)
        {
            if (*p == ':' || !*p)
            {
                fields.emplace_back(start, p);
                if (!*p || fields.size() == 3)
                {
                    if (*p) fields.emplace_back(p + 1); // the folder takes the rest
                    break;
                }
                start = p + 1;
            }
        }
        if (fields.size() > 0 && !fields[0].empty())
        {
            icon.attachAExplicit = true;
            ParseAttachField(fields[0].data(), fields[0].data() + fields[0].size(), icon.attachA);
        }
        if (fields.size() > 1 && !fields[1].empty())
        {
            icon.attachBExplicit = true;
            ParseAttachField(fields[1].data(), fields[1].data() + fields[1].size(), icon.attachB);
        }
        if (fields.size() > 2 && !fields[2].empty()) ParseU32(fields[2], icon.flags);
        if (fields.size() > 3) icon.folder = fields[3];
        return icon;
    }

    bool IsKnownComponentFolder(std::string_view folder)
    {
        static const char* kFolders[] = {"Head",   "Shoulder", "Shirt", "Chest",       "Waist",  "Leg", "Foot",
                                         "Bracer", "Glove",    "Cape",  "Tabard",      "Collections", "Weapon"};
        for (const char* known : kFolders)
        {
            if (std::strlen(known) == folder.size() && _strnicmp(known, folder.data(), folder.size()) == 0)
                return true;
        }
        return false;
    }

    const char* InferFolder(const char* name, bool collection, const char* custom, const char* slotFolder)
    {
        if (custom && *custom) return custom;
        if (collection) return "Collections";
        if (StartsWithCI(name, "lshoulder_") || StartsWithCI(name, "rshoulder_") || StartsWithCI(name, "shoulder_") ||
            ContainsCI(name, "_shoulder_l") || ContainsCI(name, "_shoulder_r"))
            return "Shoulder";
        if (StartsWithCI(name, "cape_")) return "Cape";
        if (StartsWithCI(name, "tabard_")) return "Tabard";
        return slotFolder;
    }

    uint32_t InferAttach(const char* name, bool collection, bool explicitAttach, uint32_t attach)
    {
        if (explicitAttach || collection) return attach;
        if (ContainsCI(name, "lshoulder_") || ContainsCI(name, "_shoulder_l")) return 6;
        if (ContainsCI(name, "rshoulder_") || ContainsCI(name, "_shoulder_r")) return 5;
        if (ContainsCI(name, "shoulder"))
        {
            if (ContainsCI(name, "_l.") || ContainsCI(name, "_l_")) return 6;
            if (ContainsCI(name, "_r.") || ContainsCI(name, "_r_")) return 5;
        }
        if (ContainsCI(name, "collections_"))
        {
            if (ContainsCI(name, "collections_belt_") || ContainsCI(name, "_belt")) return 53;
        }
        if (ContainsCI(name, "cape_")) return 12;
        if (ContainsCI(name, "tabard_")) return 34;
        return attach;
    }

    /// A plain (non-collection) model is accepted on the head, shoulders, waist, tabard and cloak.
    bool SlotAllowsPlainModel(uint32_t slot) { return slot == 0 || slot == 1 || slot == 4 || slot == 9 || slot == 10; }

    /// A stock helm or shoulder model name, copied between rows and never cleaned out of the DBC.
    bool IsStrayLegacyName(const char* stem)
    {
        const char* base = stem;
        for (const char* s = stem; *s; ++s)
            if (*s == '\\' || *s == '/') base = s + 1;
        return StartsWithCI(base, "lshoulder_") || StartsWithCI(base, "rshoulder_") || StartsWithCI(base, "shoulder_") ||
               StartsWithCI(base, "helm_");
    }

    bool IsCollectionGloveBody(uint32_t slot, const char* stem) { return slot == 8 && ContainsCI(stem, "_glove"); }

    // --- Paths ----------------------------------------------------------------------------------

    struct Wearer
    {
        std::string race;   // ChrRaces client prefix
        std::string gender; // "M" or "F"
    };

    std::string Suffix(const Wearer& w, bool race, bool gender, bool newSuffix)
    {
        std::string out = "_";
        if (race) out += w.race;
        if (newSuffix && race && gender) out += '_';
        if (gender) out += w.gender;
        return out;
    }

    /// Item\ObjectComponents\<folder>\[<stem>\]<stem>[_<race>[_]<gender>].mdx
    std::string BuildModelPath(const std::string& modelName, const Wearer& w, uint32_t flags, const char* slotFolder,
                               bool collection, const char* custom)
    {
        const bool race   = !(flags & kFlagNoRace);
        const bool gender = !(flags & kFlagNoGender);
        const size_t dot  = modelName.find_last_of('.');
        const std::string stem = dot == std::string::npos ? modelName : modelName.substr(0, dot);

        std::string path = "Item\\ObjectComponents\\";
        path += InferFolder(modelName.c_str(), collection, custom, slotFolder);
        path += '\\';
        if (flags & kFlagSubfolder) path += stem + '\\';
        path += stem;
        if (race || gender) path += Suffix(w, race, gender, (flags & kFlagNewSuffix) != 0);
        return path + ".mdx";
    }

    /// Item\ObjectComponents\<folder>\[<model stem>\]<texture>[_<race>[_]<gender>].blp
    std::string BuildTexturePath(const std::string& texName, const Wearer& w, uint32_t flags, const char* slotFolder,
                                 bool collection, const char* custom, const std::string& modelStem)
    {
        std::string explicitFolder;
        std::string texStem = texName;
        const size_t slash  = texName.find_first_of("\\/");
        if (slash != std::string::npos && slash > 0 && IsKnownComponentFolder(std::string_view(texName).substr(0, slash)))
        {
            explicitFolder = texName.substr(0, slash);
            texStem        = texName.substr(slash + 1);
        }

        const std::string& subBase = modelStem.empty() ? texStem : modelStem;
        std::string folder = InferFolder(subBase.c_str(), collection, custom, slotFolder);
        if (!explicitFolder.empty()) folder = explicitFolder;

        std::string path = "Item\\ObjectComponents\\" + folder + '\\';
        if (flags & kFlagSubfolder) path += subBase.substr(0, subBase.find('.')) + '\\';
        path += texStem;
        const bool race   = (flags & kFlagTexRace) != 0;
        const bool gender = (flags & kFlagTexGender) != 0;
        if (race || gender) path += Suffix(w, race, gender, (flags & kFlagNewSuffix) != 0);
        return path + ".blp";
    }

    // --- Files ----------------------------------------------------------------------------------

    std::unordered_map<std::string, bool> g_exists;

    struct Stats
    {
        uint32_t attached = 0;       // pieces attached this session
        uint32_t staleIgnored = 0;   // stock helm/shoulder names ignored on other slots
        uint32_t missingSkipped = 0; // pieces skipped because their model file is missing
        uint32_t attachFailed = 0;   // pieces whose attachment point the character lacks
    };
    Stats g_stats;

    /// The client loads models as .m2 whatever the data says, so .mdx is looked for as .m2.
    bool ModelExists(const std::string& path)
    {
        std::string key = Lower(path);
        if (key.size() > 4 && key.compare(key.size() - 4, 4, ".mdx") == 0) key.replace(key.size() - 4, 4, ".m2");
        auto it = g_exists.find(key);
        if (it != g_exists.end()) return it->second;
        const bool exists = g_storage.Exists(key);
        g_exists.emplace(key, exists);
        if (!exists) Verbose("model not found, skipped: " + key);
        return exists;
    }

    // --- CSV sidecars -----------------------------------------------------------------------------

    std::vector<std::string> ParseCsvLine(const std::string& line)
    {
        std::vector<std::string> fields;
        std::string              field;
        bool                     quoted = false;
        for (size_t i = 0; i < line.size(); ++i)
        {
            const char c = line[i];
            if ((c == '\r' || c == '\n') && !quoted) break;
            if (quoted)
            {
                if (c == '"')
                {
                    if (i + 1 < line.size() && line[i + 1] == '"')
                    {
                        field.push_back('"');
                        ++i;
                    }
                    else quoted = false;
                }
                else field.push_back(c);
            }
            else if (c == '"') quoted = true;
            else if (c == ',')
            {
                fields.push_back(Trim(field));
                field.clear();
            }
            else field.push_back(c);
        }
        fields.push_back(Trim(field));
        return fields;
    }

    std::string NormalizeColumn(const std::string& value)
    {
        std::string out;
        for (char c : value)
        {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out.push_back(c);
        }
        return out;
    }

    struct Csv
    {
        std::vector<std::string>              header;
        std::vector<std::vector<std::string>> rows;

        int Column(const char* name) const
        {
            const std::string wanted = NormalizeColumn(name);
            for (size_t i = 0; i < header.size(); ++i)
                if (NormalizeColumn(header[i]) == wanted) return static_cast<int>(i);
            return -1;
        }

        static const std::string& Field(const std::vector<std::string>& row, int column)
        {
            static const std::string kEmpty;
            return column < 0 || static_cast<size_t>(column) >= row.size() ? kEmpty : row[static_cast<size_t>(column)];
        }
    };

    bool ReadCsv(const char* path, Csv& out)
    {
        if (!g_storage.Exists(path)) return false;
        const wxl::Expected<std::string> text = g_storage.ReadText(path);
        if (!text || text->empty()) return false;
        size_t start = 0;
        bool   first = true;
        for (size_t i = 0; i <= text->size(); ++i)
        {
            if (i != text->size() && (*text)[i] != '\n') continue;
            std::string line = text->substr(start, i - start);
            start            = i + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (first)
            {
                if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF) line.erase(0, 3); // UTF-8 BOM
                out.header = ParseCsvLine(line);
                first      = false;
            }
            else if (!Trim(line).empty())
            {
                out.rows.push_back(ParseCsvLine(line));
            }
        }
        return !out.header.empty();
    }

    struct SidecarModel
    {
        uint32_t              slot         = kNoAttach;
        uint32_t              attach       = kNoAttach;
        uint32_t              modelFlags   = kFlagNoGender | kFlagNoRace;
        uint32_t              textureFlags = 0;
        std::string           folder;
        std::string           model;
        std::string           texture;
        std::vector<uint16_t> geosets;

        bool operator==(const SidecarModel& o) const
        {
            return slot == o.slot && attach == o.attach && modelFlags == o.modelFlags && textureFlags == o.textureFlags &&
                   geosets == o.geosets && Lower(folder) == Lower(o.folder) && Lower(model) == Lower(o.model) &&
                   Lower(texture) == Lower(o.texture);
        }
    };

    struct SidecarMaterial
    {
        uint32_t    modelIndex  = kNoAttach;
        uint32_t    modelColumn = kNoAttach;
        uint32_t    textureType = kNoAttach;
        std::string folder;
        std::string model;
        std::string texture;
        std::string targetMode;
    };

    std::unordered_map<uint32_t, std::vector<SidecarModel>>    g_sidecarModels;
    std::unordered_map<uint32_t, std::vector<SidecarMaterial>> g_sidecarMaterials;
    bool                                                       g_sidecarsLoaded = false;

    uint32_t ParseModelColumn(const std::string& value)
    {
        if (value.empty()) return kNoAttach;
        uint32_t numeric = 0;
        if (ParseU32(value, numeric)) return numeric == 0 || numeric == 1 ? numeric : numeric == 2 ? 1 : kNoAttach;
        const char* v = value.c_str();
        if (ContainsCI(v, "ModelName_1") || ContainsCI(v, "ModelTexture_1") || ContainsCI(v, "Model_1") || ContainsCI(v, "Texture_1"))
            return 0;
        if (ContainsCI(v, "ModelName_2") || ContainsCI(v, "ModelTexture_2") || ContainsCI(v, "Model_2") || ContainsCI(v, "Texture_2"))
            return 1;
        return kNoAttach;
    }

    void LoadModelSidecar(const char* path)
    {
        Csv csv;
        if (!ReadCsv(path, csv)) return;
        const int cDisplay = csv.Column("DisplayID");
        const int cSlot    = csv.Column("Slot");
        int       cModel   = csv.Column("Model");
        if (cModel < 0) cModel = csv.Column("ModelStem");
        if (cModel < 0) cModel = csv.Column("ModelName");
        int cTexture = csv.Column("Texture");
        if (cTexture < 0) cTexture = csv.Column("TextureStem");
        if (cTexture < 0) cTexture = csv.Column("ModelTexture");
        const int cFolder        = csv.Column("Folder");
        const int cGeosets       = csv.Column("Geosets");
        const int cAttach        = csv.Column("Attach");
        const int cSuffixPolicy  = csv.Column("SuffixPolicy");
        const int cTexturePolicy = csv.Column("TexturePolicy");
        const int cFlags         = csv.Column("Flags");
        if (cDisplay < 0 || cModel < 0)
        {
            Warn(Format("%s: no DisplayID or Model column, ignored", path));
            return;
        }

        uint32_t loaded = 0;
        for (const auto& row : csv.rows)
        {
            uint32_t display = 0;
            if (!ParseU32(Csv::Field(row, cDisplay), display) || display == 0) continue;

            SidecarModel e;
            e.slot         = ParseSidecarSlot(Csv::Field(row, cSlot));
            e.modelFlags   = ModelFlagsForPolicy(Csv::Field(row, cSuffixPolicy));
            e.textureFlags = TextureFlagsForPolicy(Csv::Field(row, cTexturePolicy));
            uint32_t value = 0;
            if (ParseU32(Csv::Field(row, cAttach), value)) e.attach = value;
            if (ParseU32(Csv::Field(row, cFlags), value) && e.modelFlags != kFlagsFromSlot) e.modelFlags |= value;
            e.folder  = Csv::Field(row, cFolder);
            e.model   = Csv::Field(row, cModel);
            e.texture = Csv::Field(row, cTexture);
            const size_t colon = e.model.find(':');
            if (colon != std::string::npos)
            {
                e.geosets = ParseGeosets(e.model.c_str() + colon + 1);
                e.model.resize(colon);
            }
            const std::string& geosets = Csv::Field(row, cGeosets);
            if (!geosets.empty()) e.geosets = ParseGeosets(geosets.c_str());
            if (e.model.empty()) continue;

            // The same CSV is often reachable by two names; an identical row adds nothing.
            std::vector<SidecarModel>& list = g_sidecarModels[display];
            if (std::find(list.begin(), list.end(), e) != list.end()) continue;
            list.push_back(std::move(e));
            ++loaded;
        }
        if (loaded) Info(Format("%s: %u model row(s)", path, loaded));
    }

    void LoadMaterialSidecar(const char* path)
    {
        Csv csv;
        if (!ReadCsv(path, csv)) return;
        const int cDisplay     = csv.Column("DisplayID");
        const int cModelIndex  = csv.Column("ModelIndex");
        const int cModelColumn = csv.Column("ModelColumn");
        const int cModel       = csv.Column("Model");
        const int cTextureType = csv.Column("TextureType");
        const int cFolder      = csv.Column("Folder");
        const int cTexture     = csv.Column("Texture");
        const int cTargetMode  = csv.Column("TargetMode");
        if (cDisplay < 0 || cTexture < 0)
        {
            Warn(Format("%s: no DisplayID or Texture column, ignored", path));
            return;
        }

        uint32_t loaded = 0;
        for (const auto& row : csv.rows)
        {
            uint32_t display = 0;
            if (!ParseU32(Csv::Field(row, cDisplay), display) || display == 0) continue;
            SidecarMaterial e;
            ParseU32(Csv::Field(row, cModelIndex), e.modelIndex);
            e.modelColumn = ParseModelColumn(Csv::Field(row, cModelColumn));
            if (e.modelColumn == kNoAttach) e.modelColumn = ParseModelColumn(Csv::Field(row, cModelIndex));
            ParseU32(Csv::Field(row, cTextureType), e.textureType);
            e.folder     = Csv::Field(row, cFolder);
            e.model      = Csv::Field(row, cModel);
            e.texture    = Csv::Field(row, cTexture);
            e.targetMode = Csv::Field(row, cTargetMode);
            if (e.texture.empty()) continue;
            g_sidecarMaterials[display].push_back(std::move(e));
            ++loaded;
        }
        if (loaded) Info(Format("%s: %u material row(s)", path, loaded));
    }

    void LoadSidecars()
    {
        if (g_sidecarsLoaded) return;
        g_sidecarsLoaded = true;
        LoadModelSidecar("WXLItemDisplayModels.csv");
        LoadModelSidecar("DBFilesClient\\WXLItemDisplayModels.csv");
        LoadMaterialSidecar("WXLItemDisplayModelMaterials.csv");
        LoadMaterialSidecar("DBFilesClient\\WXLItemDisplayModelMaterials.csv");
        Info(Format("sidecars ready: %zu display(s) with models, %zu with materials", g_sidecarModels.size(),
                    g_sidecarMaterials.size()));
    }

    // --- Materials --------------------------------------------------------------------------------

    std::string StemKey(const std::string& value)
    {
        size_t base = value.find_last_of("\\/");
        base        = base == std::string::npos ? 0 : base + 1;
        std::string out;
        for (size_t i = base; i < value.size() && value[i] != ':' && value[i] != '.'; ++i)
        {
            const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(value[i])));
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') out.push_back(c);
        }
        return out;
    }

    bool MaterialMatches(const SidecarMaterial& e, uint32_t modelColumn, uint32_t partIndex, const std::string& modelName)
    {
        const bool hasModel = !e.model.empty();
        if (hasModel)
        {
            const std::string a = StemKey(e.model), b = StemKey(modelName);
            if (a.empty() || a != b) return false;
            if (modelColumn != kNoAttach && e.modelColumn != kNoAttach && e.modelColumn != modelColumn) return false;
        }
        else
        {
            if (e.modelColumn != kNoAttach && e.modelColumn != modelColumn) return false;
            if (e.modelColumn == kNoAttach && e.modelIndex != kNoAttach)
            {
                const bool columnMatch = modelColumn != kNoAttach && (e.modelIndex == modelColumn || e.modelIndex == modelColumn + 1);
                const bool partMatch   = e.modelIndex == partIndex || e.modelIndex == partIndex + 1;
                if (!columnMatch && !partMatch) return false;
            }
        }
        return !e.texture.empty();
    }

    // --- Pieces: what one item display hangs on a character -------------------------------------------

    struct Piece
    {
        std::string                                 model;   // full model path
        std::string                                 texture; // full texture path for type 2, or empty
        std::vector<std::pair<uint32_t, std::string>> materials;
        std::vector<uint16_t>                       geosets; // empty: all
        uint32_t                                    attach = kNoAttach;
        bool                                        collection = false;
    };

    void AddMaterials(Piece& piece, uint32_t display, uint32_t modelColumn, uint32_t partIndex, const std::string& modelName,
                      const Wearer& w, const char* slotFolder, const char* customFolder)
    {
        auto it = g_sidecarMaterials.find(display);
        if (it == g_sidecarMaterials.end()) return;
        for (const SidecarMaterial& m : it->second)
        {
            if (!MaterialMatches(m, modelColumn, partIndex, modelName) || m.textureType == kNoAttach) continue;
            // Hidden materials need the batches a row hides, which the CSV no longer names.
            if (ContainsCI(m.targetMode.c_str(), "Hide") || StartsWithCI(m.texture.c_str(), "__hide__") ||
                StartsWithCI(m.texture.c_str(), "hide"))
                continue;
            const char* folder = m.folder.empty() ? customFolder : m.folder.c_str();
            piece.materials.emplace_back(
                m.textureType, BuildTexturePath(m.texture, w, 0, slotFolder, piece.collection, folder, modelName));
        }
    }

    /// The pieces of one display on one slot, resolved as upstream resolves them.
    std::vector<Piece> ResolvePieces(uint32_t slot, uint32_t display, const Wearer& w)
    {
        std::vector<Piece> pieces;
        const SlotConfig&  cfg = kSlots[slot];

        wxl_m2_item_display row{};
        row.struct_size = sizeof row;
        if (g_m2->item_display(display, &row) != WXL_OK)
        {
            Verbose(Format("display %u: no ItemDisplayInfo row", display));
            return pieces;
        }

        const Icon2 icon = ParseIcon2(row.icon[1], cfg);

        bool rowHasCollection = false, rowHasNormal = false;
        for (const char* list : {row.model[0], row.model[1]})
        {
            std::string part;
            for (uint32_t i = 0; i < 16 && ListPart(list, i, part); ++i)
            {
                if (part.empty()) continue;
                (part.find(':') != std::string::npos ? rowHasCollection : rowHasNormal) = true;
            }
        }
        const bool mixedRow = rowHasCollection && rowHasNormal;

        // Collections always take race and gender; plain models take the slot's defaults.
        auto effectiveFlags = [&](bool collection) {
            uint32_t ef = icon.flags;
            if (collection && !(ef & kFlagNewSuffix) && !(ef & kFlagNoRace) && !(ef & kFlagNoGender)) ef |= kFlagNewSuffix;
            if (!collection)
            {
                const bool force = (ef & kFlagForceModelRaceSex) || ((ef & kFlagNewSuffix) && !mixedRow);
                if (!force)
                {
                    if (!cfg.defUseRace) ef |= kFlagNoRace;
                    if (!cfg.defUseGender) ef |= kFlagNoGender;
                }
            }
            return ef;
        };

        // --- CSV rows first: one row for a display replaces the DBC model lists --------------------
        LoadSidecars();
        auto sidecar = g_sidecarModels.find(display);
        if (sidecar != g_sidecarModels.end())
        {
            uint32_t index = 0;
            for (const SidecarModel& sc : sidecar->second)
            {
                if (sc.slot != kNoAttach && sc.slot != slot) continue;
                const uint32_t partIndex = index++;
                const bool collection = (!sc.folder.empty() && StartsWithCI(sc.folder.c_str(), "Collections")) || !sc.geosets.empty() ||
                                        StartsWithCI(sc.model.c_str(), "collections_");
                uint32_t attach = sc.attach != kNoAttach ? sc.attach : cfg.defAttach1;
                if (collection && sc.attach == kNoAttach) attach = kCollectionAttach;
                if (collection && IsCollectionGloveBody(slot, sc.model.c_str())) attach = kCollectionAttach;
                if (!collection && !SlotAllowsPlainModel(slot))
                {
                    Verbose(Format("display %u: plain model %s ignored on texture-only slot %u", display, sc.model.c_str(), slot));
                    continue;
                }
                const uint32_t modelFlags   = sc.modelFlags == kFlagsFromSlot ? effectiveFlags(collection) : sc.modelFlags;
                const uint32_t textureFlags = sc.textureFlags == kFlagsFromSlot ? effectiveFlags(collection) : sc.textureFlags;
                const char*    folder       = sc.folder.empty() ? nullptr : sc.folder.c_str();

                Piece piece;
                piece.collection = collection;
                piece.attach     = attach;
                piece.geosets    = sc.geosets;
                piece.model      = BuildModelPath(sc.model, w, modelFlags, cfg.folder, collection, folder);
                if (!ModelExists(piece.model))
                {
                    ++g_stats.missingSkipped;
                    continue;
                }
                if (!sc.texture.empty())
                    piece.texture = BuildTexturePath(sc.texture, w, textureFlags, cfg.folder, collection, folder, sc.model);
                AddMaterials(piece, display, kNoAttach, partIndex, sc.model, w, cfg.folder, folder);
                pieces.push_back(std::move(piece));
            }
            if (!pieces.empty()) return pieces;
        }

        // --- Else ModelName_1 and ModelName_2, each a ';' list of "<model>[:<geosets>]" -----------
        // Head and shoulders: the client attaches ModelName_1 (and the right pauldron) itself, so
        // only what it does not -- the head's ModelName_2 -- is added here.
        for (uint32_t column = 0; column < 2; ++column)
        {
            if (slot == 1 || (slot == 0 && column == 0)) continue;
            const char*    list           = row.model[column];
            const uint32_t columnAttach   = column == 0 ? icon.attachA : icon.attachB;
            const uint32_t defaultAttach  = column == 0 ? cfg.defAttach1 : cfg.defAttach2;
            const bool     explicitAttach = column == 0 ? icon.attachAExplicit : icon.attachBExplicit;

            std::string part;
            for (uint32_t index = 0; index < 16 && ListPart(list, index, part); ++index)
            {
                if (part.empty()) continue;
                std::string texPart;
                if (!ListPart(row.model_texture[column], index, texPart)) ListPart(row.model_texture[column], 0, texPart);

                std::vector<uint16_t> geosets;
                std::string           stem  = part;
                const size_t          colon = part.find(':');
                if (colon != std::string::npos)
                {
                    stem    = part.substr(0, colon);
                    geosets = ParseGeosets(part.c_str() + colon + 1);
                }
                if (stem.empty()) continue;

                const bool collection = !geosets.empty();
                if (!collection && !SlotAllowsPlainModel(slot))
                {
                    Verbose(Format("display %u: plain model %s ignored on texture-only slot %u", display, stem.c_str(), slot));
                    continue;
                }
                if (!collection && slot > 1 && !icon.configured && IsStrayLegacyName(stem.c_str()))
                {
                    ++g_stats.staleIgnored;
                    Verbose(Format("display %u: stale stock model %s on slot %u ignored; set Icon2 to use it", display, stem.c_str(), slot));
                    continue;
                }

                uint32_t attach = mixedRow ? (collection ? kCollectionAttach : defaultAttach)
                                           : (collection && !explicitAttach ? kCollectionAttach : columnAttach);
                attach          = InferAttach(stem.c_str(), collection, explicitAttach, attach);
                if (collection && IsCollectionGloveBody(slot, stem.c_str())) attach = kCollectionAttach;

                const uint32_t ef     = effectiveFlags(collection);
                const char*    custom = icon.folder.empty() ? nullptr : icon.folder.c_str();
                Piece          piece;
                piece.collection = collection;
                piece.attach     = attach;
                piece.geosets    = geosets;
                piece.model      = BuildModelPath(stem, w, ef, cfg.folder, collection, custom);
                if (!ModelExists(piece.model))
                {
                    ++g_stats.missingSkipped;
                    continue;
                }
                if (!texPart.empty()) piece.texture = BuildTexturePath(texPart, w, ef, cfg.folder, collection, custom, stem);
                AddMaterials(piece, display, column, index, stem, w, cfg.folder, custom);
                pieces.push_back(std::move(piece));
            }
        }
        return pieces;
    }

    // --- Wearers: what the mod has hung on whom ---------------------------------------------------

    struct SlotModels
    {
        uint32_t           display = 0;
        std::vector<void*> models; // each one reference the mod holds
    };

    struct Dressed
    {
        void*      model = nullptr; // the character model the slots hang on
        SlotModels slots[kNumSlots];
    };

    std::unordered_map<void*, Dressed> g_wearers;

    constexpr uint32_t kBeltBuckle  = 53; // retail's point; 3.3.5 bodies have none
    constexpr uint32_t kKeyBoneWaist = 5;

    /// Where a body without a BeltBuckle point would have one: centred between its hip weapon
    /// points (32, 33), at their height, brought forward by 0.6 of the hip half-width -- the
    /// belly's front, measured across the stock 3.3.5 bodies.
    bool StandInBuckle(void* body, wxl_vec3& out)
    {
        wxl_vec3 left{}, right{};
        if (g_m2->attachment_point(body, 32, &left) != WXL_OK || g_m2->attachment_point(body, 33, &right) != WXL_OK)
            return false;
        const float halfWidth = std::abs(left.y - right.y) * 0.5f;
        out.x = (left.x + right.x) * 0.5f + 0.6f * halfWidth;
        out.y = (left.y + right.y) * 0.5f;
        out.z = (left.z + right.z) * 0.5f;
        return true;
    }

    void Clear(SlotModels& slot)
    {
        for (void* model : slot.models) g_m2->release_model(model); // detaches it too
        slot.models.clear();
        slot.display = 0;
    }

    void ClearAll(Dressed& dressed)
    {
        for (SlotModels& slot : dressed.slots) Clear(slot);
    }

    /// Whether what the slot holds still hangs, whole, on `parent`.
    bool StillDressed(const SlotModels& slot, uint32_t display, void* parent)
    {
        if (slot.display != display || slot.models.empty()) return false;
        for (void* model : slot.models)
            if (g_m2->model_parent(model) != parent) return false;
        return true;
    }

    void Dress(const wxl_appearance_args& change)
    {
        const uint32_t slot = change.item_slot;
        if (slot >= kNumSlots) return; // the quiver, and weapons, are the client's own

        Dressed& dressed = g_wearers[change.wearer];
        if (dressed.model != change.model)
        {
            // A new character model: everything hung on the old one goes with it.
            ClearAll(dressed);
            dressed.model = change.model;
        }
        if (!change.model) return;

        SlotModels& held = dressed.slots[slot];
        if (StillDressed(held, change.display_id, change.model)) return; // the same item put on again
        Clear(held);

        Wearer     w;
        wxl_str    prefix{};
        if (g_m2->race_prefix(change.race_id, &prefix) != WXL_OK) return;
        w.race   = std::string(prefix.data, prefix.size);
        w.gender = change.sex_id == 1 ? "F" : "M";

        const std::vector<Piece> pieces = ResolvePieces(slot, change.display_id, w);
        held.display                    = change.display_id;
        for (const Piece& piece : pieces)
        {
            void* model = g_m2->create_model(change.model, wxl::Str(piece.model));
            if (!model) continue;

            if (!piece.texture.empty() && g_m2->replace_texture(model, 2, wxl::Str(piece.texture)) != WXL_OK)
                Verbose("texture not loaded: " + piece.texture);
            for (const auto& [type, path] : piece.materials)
                if (g_m2->replace_texture(model, type, wxl::Str(path)) != WXL_OK) Verbose("material not loaded: " + path);
            if (!piece.geosets.empty())
            {
                g_m2->set_geosets(model, 0, 0xFFFF, WXL_FALSE);
                for (uint16_t id : piece.geosets) g_m2->set_geosets(model, id, id, WXL_TRUE);
            }

            // A collection on the root is skinned to the body: it hangs on the root and follows the
            // body's bones. Everything else hangs on its attachment point and animates itself.
            const bool rootSkinned = piece.attach == kCollectionAttach && (piece.collection || slot == 10);
            const uint32_t flags = rootSkinned ? (WXL_M2_ATTACH_ROOT | WXL_M2_ATTACH_FOLLOW_BONES)
                                               : (piece.collection ? WXL_M2_ATTACH_ROOT : 0u);
            wxl_result attached = g_m2->attach_model(model, change.model, piece.attach, flags);
            wxl_vec3   buckle{};
            if (attached == WXL_E_NOT_FOUND && piece.attach == kBeltBuckle && !rootSkinned && StandInBuckle(change.model, buckle))
            {
                attached = g_m2->attach_to_bone(model, change.model, kBeltBuckle, kKeyBoneWaist, buckle);
                if (attached == WXL_OK)
                    Verbose(Format("%s: no BeltBuckle on this body, pinned to its waist at (%.3f, %.3f, %.3f)",
                                   piece.model.c_str(), buckle.x, buckle.y, buckle.z));
            }
            if (attached != WXL_OK)
            {
                ++g_stats.attachFailed;
                Verbose(Format("%s: the character has no attachment %u", piece.model.c_str(), piece.attach));
                g_m2->release_model(model);
                continue;
            }
            held.models.push_back(model);
            ++g_stats.attached;
            Verbose(Format("display %u slot %u: %s at %u%s%s, %zu material(s), %zu geoset(s)", change.display_id, slot,
                           piece.model.c_str(), piece.attach, rootSkinned ? " following the body" : "",
                           piece.texture.empty() ? "" : (", texture " + piece.texture).c_str(), piece.materials.size(),
                           piece.geosets.size()));
        }
    }

    /// AppearanceChanged, subscribed raw: the args as the runtime raised them.
    void WXL_CALL OnEquipment(void*, const void* args)
    {
        const wxl_appearance_args* change = static_cast<const wxl_appearance_args*>(args);
        if (!change || !change->wearer) return;
        switch (change->change)
        {
            case WXL_APPEARANCE_WORN:
                Dress(*change);
                break;
            case WXL_APPEARANCE_REMOVED: {
                auto it = g_wearers.find(change->wearer);
                if (it != g_wearers.end() && change->item_slot < kNumSlots) Clear(it->second.slots[change->item_slot]);
                break;
            }
            case WXL_APPEARANCE_FREED: {
                auto it = g_wearers.find(change->wearer);
                if (it == g_wearers.end()) break;
                ClearAll(it->second);
                g_wearers.erase(it);
                break;
            }
            default:
                break;
        }
    }

    void ReleaseEverything()
    {
        if (!g_m2) return;
        for (auto& [wearer, dressed] : g_wearers) ClearAll(dressed);
        g_wearers.clear();
    }

    // --- Settings and payload -----------------------------------------------------------------------

    void PhysicsLog(const char* line) { wxl::Log(WXL_LOG_INFO, kTag, line); }

    /// The client's folder: where upstream (and the Workbench) keep WXLExtendedEquipment.ini.
    std::string ClientDirectory()
    {
        char path[MAX_PATH];
        const DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return ".";
        std::string dir(path, n);
        const size_t slash = dir.find_last_of("\\/");
        return slash == std::string::npos ? "." : dir.substr(0, slash);
    }

    void LoadSettings(const std::string& dir)
    {
        const std::string ini = dir + "\\WXLExtendedEquipment.ini";
        g_verbose = GetPrivateProfileIntA("Log", "Verbose", 0, ini.c_str()) != 0;
        if (GetEnvironmentVariableA("WXL_EQUIP_LOG", nullptr, 0) != 0) g_verbose = true;
        equip_plus::physics_live::SetLogger(&PhysicsLog);
        equip_plus::physics_live::Init(ini, ClientDirectory() + "\\WXLExtendedEquipment.ini");
    }

    /// Mounts every archive in <mod>\data: .MPQ files and open .MPQ folders alike.
    void MountPayload(const std::string& dir)
    {
        WIN32_FIND_DATAA found{};
        const std::string pattern = dir + "\\data\\*.MPQ";
        HANDLE            find    = FindFirstFileA(pattern.c_str(), &found);
        if (find == INVALID_HANDLE_VALUE) return;
        do
        {
            const std::string path = dir + "\\data\\" + found.cFileName;
            const wxl_result  r    = g_storage.MountArchive(path, 100);
            if (r == WXL_OK) Info("mounted " + path);
            else Warn(Format("could not mount %s (%d)", path.c_str(), static_cast<int>(r)));
        } while (FindNextFileA(find, &found));
        FindClose(find);
    }

    // --- The F9 panel ---------------------------------------------------------------------------------
    //
    // Upstream drew its panel in WarcraftXL's F9 overlay (UiAddPanel). The x64 runtime publishes no
    // overlay (wxl.ui), so the same panel is an interface frame: the mod gives the game's script a
    // few functions, and builds the frame once the interface has loaded. F9 (or /equipplus) shows
    // and hides it.

    uint32_t g_csvReloads = 0;

    void ReloadSidecars()
    {
        g_sidecarModels.clear();
        g_sidecarMaterials.clear();
        g_exists.clear();
        g_sidecarsLoaded = false;
        LoadSidecars();
        equip_plus::physics_live::LoadTable();
        ++g_csvReloads;
        Info(Format("panel: CSV sidecars and physics table reloaded (%zu displays with models, %zu with materials)",
                    g_sidecarModels.size(), g_sidecarMaterials.size()));
    }

    /// EquipPlus_Stats() -> displays with models, with materials, models attached now, characters,
    /// stale names ignored, missing files skipped, attachments lacked, physics on, table rows,
    /// files matched, simulating now, last ms, worst ms, faults, log on, reloads
    void ScriptStats(wxl::ScriptCall& call)
    {
        LoadSidecars();
        size_t characters = 0, models = 0;
        for (const auto& [wearer, dressed] : g_wearers)
        {
            size_t here = 0;
            for (const SlotModels& slot : dressed.slots) here += slot.models.size();
            if (here) ++characters;
            models += here;
        }
        const auto ps = equip_plus::physics_live::GetStats();
        call.PushNumber(double(g_sidecarModels.size()));
        call.PushNumber(double(g_sidecarMaterials.size()));
        call.PushNumber(double(models));
        call.PushNumber(double(characters));
        call.PushNumber(g_stats.staleIgnored);
        call.PushNumber(g_stats.missingSkipped);
        call.PushNumber(g_stats.attachFailed);
        call.PushBool(equip_plus::physics_live::Enabled());
        call.PushNumber(ps.tableRows);
        call.PushNumber(ps.files);
        call.PushNumber(ps.live);
        call.PushNumber(ps.lastFrameMs);
        call.PushNumber(ps.worstFrameMs);
        call.PushNumber(ps.faults);
        call.PushBool(g_verbose);
        call.PushNumber(g_csvReloads);
    }

    void ScriptSetPhysics(wxl::ScriptCall& call)
    {
        const bool on = call.Bool(0).value_or(false);
        equip_plus::physics_live::SetEnabled(on);
        Info(Format("panel: live physics %s", on ? "on" : "off"));
    }

    void ScriptSetLog(wxl::ScriptCall& call) { g_verbose = call.Bool(0).value_or(false); }

    void ScriptReload(wxl::ScriptCall&) { ReloadSidecars(); }

    constexpr const char* kPanelLua = R"lua(
if EquipPlusPanel or not EquipPlus_Stats or not UIParent then return end
local f = CreateFrame("Frame", "EquipPlusPanel", UIParent)
f:SetWidth(400); f:SetHeight(300); f:SetPoint("CENTER")
f:SetFrameStrata("DIALOG"); f:SetToplevel(true); f:SetClampedToScreen(true)
f:SetBackdrop({bgFile = "Interface\\DialogFrame\\UI-DialogBox-Background",
               edgeFile = "Interface\\DialogFrame\\UI-DialogBox-Border",
               tile = true, tileSize = 32, edgeSize = 32,
               insets = {left = 11, right = 12, top = 12, bottom = 11}})
f:EnableMouse(true); f:SetMovable(true); f:RegisterForDrag("LeftButton")
f:SetScript("OnDragStart", f.StartMoving); f:SetScript("OnDragStop", f.StopMovingOrSizing)
f:Hide()
tinsert(UISpecialFrames, "EquipPlusPanel")

local title = f:CreateFontString(nil, "ARTWORK", "GameFontNormalLarge")
title:SetPoint("TOP", 0, -18); title:SetText("Equip Plus")
local close = CreateFrame("Button", nil, f, "UIPanelCloseButton")
close:SetPoint("TOPRIGHT", -6, -6)

local lines, y = {}, -46
local function line(indent)
  local t = f:CreateFontString(nil, "ARTWORK", "GameFontHighlightSmall")
  t:SetPoint("TOPLEFT", 22 + (indent or 0), y); t:SetJustifyH("LEFT")
  y = y - 15
  lines[#lines + 1] = t
  return t
end
local csv, attached = line(), line()
y = y - 6
local prot = line(); prot:SetText("|cffffd100Protection this session|r")
local stale, missing = line(10), line(10)
y = y - 6

local phys = CreateFrame("CheckButton", "EquipPlusPhysicsCheck", f, "UICheckButtonTemplate")
phys:SetPoint("TOPLEFT", 16, y + 4); y = y - 26
EquipPlusPhysicsCheckText:SetText("Live physics (plumes, ribbons, chains, cloth)")
phys:SetScript("OnClick", function(self) EquipPlus_SetPhysics(self:GetChecked() and true or false) end)
local table_, cost = line(10), line(10)
y = y - 4

local log = CreateFrame("CheckButton", "EquipPlusLogCheck", f, "UICheckButtonTemplate")
log:SetPoint("TOPLEFT", 16, y + 4); y = y - 30
EquipPlusLogCheckText:SetText("Verbose log (Logs\\WarcraftXL.log)")
log:SetScript("OnClick", function(self) EquipPlus_SetLog(self:GetChecked() and true or false) end)

local reload = CreateFrame("Button", nil, f, "UIPanelButtonTemplate")
reload:SetWidth(170); reload:SetHeight(22); reload:SetPoint("TOPLEFT", 22, y)
reload:SetText("Reload CSV and physics")
reload:SetScript("OnClick", function() EquipPlus_Reload() end)
y = y - 28
local hint = line()

local function refresh()
  local models, mats, now, chars, staleN, missingN, lacked, physOn, rows, files, live, last, worst, faults, logOn, reloads = EquipPlus_Stats()
  csv:SetText(format("CSV sidecars: %d displays with models, %d with materials", models, mats))
  attached:SetText(format("Attached now: %d models on %d characters", now, chars))
  stale:SetText(format("Stale stock helm/shoulder names ignored: %d", staleN))
  missing:SetText(format("Missing model files skipped: %d; attachment points lacking: %d", missingN, lacked))
  phys:SetChecked(physOn)
  table_:SetText(format("Physics table: %d pieces (%d seen); simulating %d now", rows, files, live))
  cost:SetText(format("Cost: %.3f ms last, %.3f ms worst; faults %d", last, worst, faults))
  log:SetChecked(logOn)
  if reloads > 0 then hint:SetText(format("Reloaded %d time(s). Re-equip an item to see a change on it.", reloads))
  else hint:SetText("Edit a CSV, reload, then re-equip the item to see the change.") end
end
local wait = 0
f:SetScript("OnShow", refresh)
f:SetScript("OnUpdate", function(self, elapsed)
  wait = wait - elapsed
  if wait <= 0 then wait = 0.5; refresh() end
end)

local toggle = CreateFrame("Button", "EquipPlusToggleButton", UIParent)
toggle:SetScript("OnClick", function() if f:IsShown() then f:Hide() else f:Show() end end)
local function bind()
  if not InCombatLockdown() then SetOverrideBindingClick(toggle, true, "F9", "EquipPlusToggleButton") end
end
bind()
local ev = CreateFrame("Frame")
ev:RegisterEvent("PLAYER_ENTERING_WORLD"); ev:RegisterEvent("PLAYER_REGEN_ENABLED")
ev:SetScript("OnEvent", bind)
SLASH_EQUIPPLUS1 = "/equipplus"; SLASH_EQUIPPLUS2 = "/ep"
SlashCmdList["EQUIPPLUS"] = function() toggle:Click() end
)lua";

    bool g_panelPending = false; // functions registered on this game context, panel not built yet

    /// Registers the panel's functions on every game context.
    /// Registers the panel's functions on every script context, and builds the panel once the game's
    /// interface has loaded (its first PLAYER_LOGIN / PLAYER_ENTERING_WORLD). The Lua checks for
    /// itself, so a context that is not the game's (the login screen has no UIParent) builds nothing.
    class PanelScript final : public wxl::LuaScript
    {
    public:
        void OnScriptVmReady(wxl_script_context, void*) override
        {
            const wxl::ScriptService script;
            if (!script) return;
            const wxl_result r = script.RegisterFunction<&ScriptStats>("EquipPlus_Stats");
            script.RegisterFunction<&ScriptSetPhysics>("EquipPlus_SetPhysics");
            script.RegisterFunction<&ScriptSetLog>("EquipPlus_SetLog");
            script.RegisterFunction<&ScriptReload>("EquipPlus_Reload");
            g_panelPending = r == WXL_OK;
            if (r != WXL_OK) Warn(Format("panel functions not registered (%d)", static_cast<int>(r)));
        }

        void OnUiEvent(uint32_t, std::string_view name, std::string_view, wxl::Bytes, uint32_t) override
        {
            if (!g_panelPending) return;
            if (name != "PLAYER_LOGIN" && name != "PLAYER_ENTERING_WORLD") return;
            g_panelPending = false;
            const wxl::ScriptService script;
            if (!script) return;
            const wxl_result r = script.Run(kPanelLua, "Equip Plus panel");
            if (r == WXL_OK) Info("panel ready: F9 or /equipplus");
            else Warn(Format("could not build the panel (%d)", static_cast<int>(r)));
        }
    };

    PanelScript g_panelScript;

    wxl::Subscription g_appearance;

    class EquipPlusMod final : public wxl::Mod
    {
    public:
        wxl_result OnLoad() override
        {
            if (!g_m2 || !g_storage)
            {
                wxl::Log(WXL_LOG_ERROR, kTag, "wxl.m2 or wxl.storage is not published; nothing to do");
                return WXL_E_UNSUPPORTED;
            }
            const std::string dir = ModDirectory();
            if (!dir.empty() && GetFileAttributesA((dir + "\\WarcraftXL_equip-plus.disable").c_str()) != INVALID_FILE_ATTRIBUTES)
            {
                Info("disabled by WarcraftXL_equip-plus.disable");
                return WXL_E_UNSUPPORTED;
            }
            LoadSettings(dir);
            g_appearance = wxl::Subscribe(WXL_EV_AppearanceChanged, &OnEquipment, nullptr);
            const wxl_result watched = g_appearance ? WXL_OK : WXL_E_UNSUPPORTED;
            if (watched != WXL_OK) return watched;
            wxl::ScriptMgr::Add(&g_panelScript);
            Info(Format("watching equipment%s", g_verbose ? " (verbose log)" : ""));
            return WXL_OK;
        }

        void OnStorageReady() override
        {
            const std::string dir = ModDirectory();
            if (!dir.empty()) MountPayload(dir);
            equip_plus::physics_live::LoadTable();
        }

        void OnReady() override
        {
            // The table may only be readable once every archive is up.
            if (equip_plus::physics_live::GetStats().tableRows == 0) equip_plus::physics_live::LoadTable();
        }


        void OnShutdown() override
        {
            Info(Format("session: %u piece(s) attached, %u stale stock model(s) ignored, %u missing model(s) skipped, %u "
                        "attachment(s) the character lacked",
                        g_stats.attached, g_stats.staleIgnored, g_stats.missingSkipped, g_stats.attachFailed));
            const auto ps = equip_plus::physics_live::GetStats();
            Info(Format("physics: %u table row(s), %u file(s) matched, %u step(s), %u fault(s), worst %.3f ms",
                        ps.tableRows, ps.files, ps.steps, ps.faults, ps.worstFrameMs));
            equip_plus::physics_live::Shutdown();
            ReleaseEverything();
        }

        void OnUnload() override
        {
            equip_plus::physics_live::Shutdown();
            g_appearance = wxl::Subscription();
            ReleaseEverything();
        }

    };
}

WXL_MOD(EquipPlusMod, "wxl.equip-plus", "Equip Plus")
