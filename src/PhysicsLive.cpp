// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 WarcraftXL
//
// wxl.equip-plus: live retail physics, the x64 glue.
//
// Upstream (src/PhysicsLive.cpp) detoured the client right after it built a model's bone matrices
// (CM2Model::AnimateMT, 0x0082F0F0) and read the instance at raw offsets. The recompiled client has
// no detours; wxl.m2's bone watchers are called at that same point (the M2.AnimateMT seam) and hand
// over the same values by name:
//
//   upstream                        here (wxl_m2_bones)
//   instance + 0x98 bone palette    matrices        (CM2Model::m_boneMatrices)
//   instance + 0xF4 own matrix      model_matrix    (CM2Model::m_matrixF4)
//   scene + 0xC4 scene matrix       view_to_world   (CM2Scene::m_viewInv, a mirror: never decomposed)
//   scene + 0x0C clock              time_ms         (CM2Scene::m_time)
//
// Everything else is upstream's, unchanged: a model whose file has a row in the physics table is
// stepped once per frame; bone world = palette bone x scene matrix; placement = own matrix x scene
// matrix; each dynamic body's model-space bone goes back as bone x own matrix, and every bone under
// a moved one is carried by its parent's delta; a second build in one frame takes 0 s, a clock
// standing still for 0.1 s falls back to the high-resolution clock; state per instance, forgotten
// when unseen for 2 s.

#include "PhysicsLive.hpp"

#include "physics/physics_domino.h"

#include "wxl/sdk.hpp"
#include "wxl/services/m2.h"
#include "wxl/services/storage.hpp"

#include <windows.h>

#include <cctype>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace equip_plus::physics_live
{
    namespace phys = wow::ec::physics;

    namespace
    {
        constexpr uint32_t kMaxBones = 256;
        constexpr uint32_t kForgetMs = 2000;
        // Equip Plus's (the Backport Helper's item pieces), and the Retroport Workbench's creatures'
        // (and mounts') and spell models' (Avenging Wrath's wings).
        const char* const kTablePaths[] = {"DBFilesClient\\WXLEquipPlusPhysics.phy2",
                                           "DBFilesClient\\WXLCreaturePhysics.phy2",
                                           "DBFilesClient\\WXLSpellPhysics.phy2"};

        wxl::Service<wxl_m2_api> g_m2(WXL_M2_SERVICE, WXL_M2_SERVICE_VERSION);

        /// A model file with a row: the row, and the file's bone tree.
        struct FileRow
        {
            std::string                  path;
            std::shared_ptr<phys::Model> row;
            std::vector<int16_t>         parents;
            std::vector<phys::V3>        pivots;
        };

        struct InstState
        {
            void*                           file = nullptr;
            std::unique_ptr<phys::Instance> sim;
            uint32_t      lastClock = 0;
            uint32_t      lastSeen  = 0;
            LARGE_INTEGER stillSince{};
            LARGE_INTEGER lastQpc{};
            bool          haveClock = false;
        };

        std::mutex                            g_lock;
        bool                                  g_enabled = false;
        std::vector<std::vector<uint8_t>>     g_tables;
        std::unordered_map<void*, FileRow>    g_files; // model file -> its row
        std::unordered_map<void*, InstState>  g_inst;  // model -> its simulation
        Stats                                 g_stats;
        uint32_t                              g_lastSweep = 0;
        uint32_t                              g_watch     = 0;
        void (*g_log)(const char*) = nullptr;
        LARGE_INTEGER                         g_qpcFreq{};

        void Log(const char* fmt, ...)
        {
            if (!g_log) return;
            char    buf[512];
            va_list ap;
            va_start(ap, fmt);
            std::vsnprintf(buf, sizeof buf, fmt, ap);
            va_end(ap);
            g_log(buf);
        }

        /// The table's row for a model path: as it is, as a .m2, then by file name alone (the
        /// converter writes a row under each, because the client may report any of them).
        std::shared_ptr<phys::Model> FindRow(const std::string& stem)
        {
            if (g_tables.empty() || stem.empty()) return nullptr;
            std::string withExt = stem;
            const size_t dot   = stem.find_last_of('.');
            const size_t slash = stem.find_last_of("\\/");
            if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) withExt += ".m2";
            const size_t nameAt = withExt.find_last_of("\\/");
            const std::string name = withExt.substr(nameAt == std::string::npos ? 0 : nameAt + 1);
            const std::string* candidates[] = {&stem, &withExt, &name};
            for (const std::string* c : candidates)
            {
                char norm[300];
                if (!phys::NormPath(c->c_str(), norm, sizeof norm)) continue;
                for (const std::vector<uint8_t>& table : g_tables)
                {
                    auto m = std::make_shared<phys::Model>();
                    if (phys::Find(table.data(), table.size(), norm, *m)) return m;
                }
            }
            return nullptr;
        }

        void SweepLocked(uint32_t now)
        {
            if (now - g_lastSweep < 500) return;
            g_lastSweep = now;
            for (auto it = g_inst.begin(); it != g_inst.end();)
            {
                if (now - it->second.lastSeen > kForgetMs) { it = g_inst.erase(it); ++g_stats.dropped; }
                else ++it;
            }
            g_stats.live = uint32_t(g_inst.size());
        }

        // --- the bone watcher (animate threads) ------------------------------------------------

        wxl_bool WXL_CALL WantFile(void* file, wxl_str path, uint32_t boneCount, void*)
        {
            const std::string stem(path.data ? path.data : "", path.size);
            if (boneCount < 2 || boneCount > kMaxBones) return WXL_FALSE; // one bone: nothing can swing
            std::lock_guard<std::mutex> g(g_lock);
            std::shared_ptr<phys::Model> row = FindRow(stem);
            if (!row)
            {
                static int listed = 0;
                std::string lower = stem;
                for (char& ch : lower) ch = char(std::tolower(static_cast<unsigned char>(ch)));
                if (listed < 100 && lower.find("objectcomponents") != std::string::npos)
                {   // only items: doodads and creatures are expected to have no row
                    ++listed;
                    Log("physics: no row for item model '%s' (%u bones)", stem.c_str(), boneCount);
                }
                g_files.erase(file);
                return WXL_FALSE;
            }
            for (const phys::BodyDef& b : row->bodies)
                if (b.bone != 0xFFFF && b.bone >= boneCount)
                {
                    ++g_stats.faults;
                    Log("physics: row for '%s' names bone %u but the file has %u", stem.c_str(), b.bone, boneCount);
                    return WXL_FALSE;
                }
            FileRow fr;
            fr.path = stem;
            fr.row  = row;
            fr.parents.resize(boneCount);
            fr.pivots.resize(boneCount);
            for (uint32_t k = 0; k < boneCount; ++k)
            {
                wxl_vec3 pivot{};
                if (g_m2->bone_info(file, k, &fr.parents[k], &pivot) != WXL_OK) return WXL_FALSE;
                fr.pivots[k] = phys::V3{pivot.x, pivot.y, pivot.z};
            }
            Log("physics: MATCH '%s' (%u bones): %zu bodies, %zu joints", stem.c_str(), boneCount, row->bodies.size(),
                row->joints.size());
            g_files[file] = std::move(fr);
            g_stats.files = uint32_t(g_files.size());
            return WXL_TRUE;
        }

        void WXL_CALL OnBones(const wxl_m2_bones* b, void*)
        {
            if (!g_enabled || !b || !b->matrices || b->bone_count < 2) return;

            LARGE_INTEGER t0;
            QueryPerformanceCounter(&t0);

            std::lock_guard<std::mutex> g(g_lock);
            auto fi = g_files.find(b->file);
            if (fi == g_files.end() || fi->second.parents.size() != b->bone_count) return;
            const FileRow& fr = fi->second;
            const uint32_t n  = b->bone_count;

            const uint32_t now = GetTickCount();
            SweepLocked(now);

            InstState& st = g_inst[b->model];
            if (st.file != b->file || !st.sim)
            {
                st      = InstState{};
                st.file = b->file;
                st.sim  = std::make_unique<phys::Instance>(fr.row.get());
            }
            st.lastSeen = now;

            // the frame's time: the scene clock; a second build in one frame takes 0 s; a clock
            // standing still for 0.1 s (paused scenes, interface models) uses the high-resolution clock
            float dt = 0;
            if (st.haveClock)
            {
                if (b->time_ms != st.lastClock)
                {
                    dt            = float(b->time_ms - st.lastClock) * 0.001f;
                    st.stillSince = t0;
                }
                else if (g_qpcFreq.QuadPart && double(t0.QuadPart - st.stillSince.QuadPart) / g_qpcFreq.QuadPart > 0.1)
                {
                    dt = float(double(t0.QuadPart - st.lastQpc.QuadPart) / g_qpcFreq.QuadPart);
                }
            }
            else st.stillSince = t0;
            if (dt < 0 || dt > 1.0f) dt = 0; // clock wrapped, or the instance was away
            st.haveClock = true;
            st.lastClock = b->time_ms;
            st.lastQpc   = t0;

            // inputs
            const std::vector<float> before(b->matrices, b->matrices + size_t(n) * 16);
            std::vector<double>      world(size_t(n) * 16);
            for (uint32_t k = 0; k < n; ++k) phys::World(&before[size_t(k) * 16], b->view_to_world, &world[size_t(k) * 16]);
            phys::Frame f;
            f.boneWorld = reinterpret_cast<const double (*)[16]>(world.data());
            f.pivots    = fr.pivots.data();
            f.boneCount = n;
            phys::World(b->model_matrix, b->view_to_world, f.placement);
            f.dt = dt;
            st.sim->Step(f);
            if (dt > 0 && ++g_stats.steps == 1) Log("physics: first simulation step ran ('%s')", fr.path.c_str());

            // outputs: each dynamic body's bone, then everything under a moved bone
            std::vector<float> bones(before);
            std::unique_ptr<bool[]> moved(new bool[n]());
            bool any = false;
            for (size_t i = 0; i < fr.row->bodies.size(); ++i)
            {
                const uint16_t bone = fr.row->bodies[i].bone;
                if (bone >= n) continue;
                double M[16];
                if (!st.sim->Output(i, M)) continue;
                phys::FromModel(M, b->model_matrix, &bones[size_t(bone) * 16]);
                moved[bone] = true;
                any         = true;
            }
            if (!any) return;
            phys::Carry(fr.parents.data(), n, before.data(), bones.data(), moved.get());
            for (float v : bones)
                if (!(v == v)) { ++g_stats.faults; g_inst.erase(b->model); return; } // never write a NaN
            std::memcpy(b->matrices, bones.data(), bones.size() * sizeof(float));

            LARGE_INTEGER t1;
            QueryPerformanceCounter(&t1);
            if (g_qpcFreq.QuadPart)
            {
                const double ms   = double(t1.QuadPart - t0.QuadPart) * 1000.0 / g_qpcFreq.QuadPart;
                g_stats.lastFrameMs = ms;
                if (ms > g_stats.worstFrameMs) g_stats.worstFrameMs = ms;
            }
            g_stats.live = uint32_t(g_inst.size());
        }

        void Unwatch()
        {
            if (g_watch && g_m2 && offsetof(wxl_m2_api, unwatch_bones) < g_m2->struct_size) g_m2->unwatch_bones(g_watch);
            g_watch = 0;
        }
    }

    void SetLogger(void (*log)(const char*)) { g_log = log; }

    void Init(const std::string& modIni, const std::string& clientIni)
    {
        QueryPerformanceFrequency(&g_qpcFreq);
        // The mod's own ini first; the client's (where upstream and the Workbench keep it) if the
        // mod's says nothing.
        int on = GetPrivateProfileIntA("Physics", "Enabled", -1, modIni.c_str());
        if (on < 0) on = GetPrivateProfileIntA("Physics", "Enabled", 0, clientIni.c_str());
        g_enabled = on > 0;
        Log("physics: %s ([Physics] Enabled in WXLExtendedEquipment.ini)", g_enabled ? "on" : "off");
    }

    bool Enabled() { return g_enabled; }

    void SetEnabled(bool on)
    {
        std::lock_guard<std::mutex> g(g_lock);
        g_enabled = on;
        g_inst.clear(); // pieces go back to their pose next frame, and start fresh when turned on
        g_stats.live = 0;
    }

    void LoadTable()
    {
        Unwatch();
        std::vector<std::vector<uint8_t>> tables;
        uint32_t                          rows = 0;
        const wxl::StorageService         storage;
        for (const char* path : kTablePaths)
        {
            if (!storage) break;
            auto bytes = storage.Read(path);
            if (!bytes || bytes->size() < 8 || std::memcmp(bytes->data(), "PHY2", 4) != 0) continue;
            uint32_t n = 0;
            std::memcpy(&n, bytes->data() + 4, 4);
            Log("physics: table %s (%u rows)", path, n);
            rows += n;
            tables.push_back(std::move(*bytes));
        }
        {
            std::lock_guard<std::mutex> g(g_lock);
            g_tables.swap(tables);
            g_files.clear();
            g_inst.clear();
            g_stats.tableRows = rows;
            g_stats.files     = 0;
            g_stats.live      = 0;
        }
        if (!rows)
        {
            Log("physics: no physics table found");
            return;
        }
        if (!g_m2 || offsetof(wxl_m2_api, bone_info) >= g_m2->struct_size)
        {
            Log("physics: this client's wxl.m2 has no bone watchers; relink the client for live physics");
            return;
        }
        const wxl_result r = g_m2->watch_bones(&WantFile, &OnBones, nullptr, &g_watch);
        if (r != WXL_OK) Log("physics: could not watch bones (%d)", static_cast<int>(r));
    }

    void Shutdown()
    {
        Unwatch();
        std::lock_guard<std::mutex> g(g_lock);
        g_files.clear();
        g_inst.clear();
    }

    Stats GetStats()
    {
        std::lock_guard<std::mutex> g(g_lock);
        return g_stats;
    }
}
