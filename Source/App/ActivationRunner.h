#pragma once

#include "Core/Errors.h"
#include "Core/MainThread.h"

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Manifold::Table
{
    struct MemoryRecord;
}

namespace Manifold
{
    class Engine;

    struct ActivationProgress
    {
        int Done = 0;
        int Total = 0;
        std::string Current;
        bool TableScript = false;
        double TableScriptSeconds = 0.0;
    };

    class ActivationRunner
    {
    public:
        explicit ActivationRunner(Engine& engine);
        ~ActivationRunner();

        ActivationRunner(const ActivationRunner&) = delete;
        ActivationRunner& operator=(const ActivationRunner&) = delete;

        Status SetActive(Table::MemoryRecord& record, bool active);
        void RequestActive(Table::MemoryRecord& record, bool active, bool fromScript = false);
        bool ActivationPending(int recordId) const { return m_pending.count(recordId) > 0; }
        bool InsideActivation() const { return m_running != nullptr && m_workerLuaActive; }
        void DropQueued(int recordId);
        void CancelActivations();
        ActivationProgress Progress() const;
        bool Working() const;
        void SetAsync(bool async) { m_async = async; }
        bool Async() const { return m_async; }
        static bool OnActivationWorker();
        void RunOnMainAndWait(const std::function<void()>& work);
        void StopTableScript();
        std::vector<Table::MemoryRecord*> ActiveScripts();
        std::vector<std::string> DeactivateAll();
        void RequestDeactivateAll();

        void RunTableScript(const std::string& script);
        void WaitForActivation();
        void DrainMainWork();
        void PumpActivations(double budgetSeconds);
        std::vector<int> BottomToTop();

    private:
        struct ActivationJob
        {
            int RecordId = 0;
            bool Active = false;
            bool FromScript = false;
            bool ChildrenDone = false;
        };
        struct RunningActivation;

        Status ActivateTree(Table::MemoryRecord& record, bool active);
        Status ActivatePlain(Table::MemoryRecord& record, bool active);
        void FinishRecordActivation(Table::MemoryRecord& record, bool active, bool fromScript);
        void StartActivation(const ActivationJob& job);
        void FinishActivation();
        void Unpend(int recordId);
        void QueueChildren(Table::MemoryRecord& record, bool active, bool fromScript);
        void RunRecordLua(const std::string& code, const char* name);
        Status RunLuaForWorker(std::string_view code, int recordId, bool syntaxCheck, std::string& output);

        Engine& m_engine;
        bool m_async = false;
        std::deque<ActivationJob> m_activations;
        std::unordered_map<int, int> m_pending;
        std::unique_ptr<RunningActivation> m_running;
        int m_progressDone = 0;
        int m_progressTotal = 0;
        std::string m_progressCurrent;
        MainThreadQueue m_mainQueue;
        bool m_waitingForWorker = false;
        bool m_workerLuaActive = false;
        int m_tableTask = 0;
        bool m_tableTaskStopped = false;
        double m_tableTaskStarted = 0.0;
    };
}
