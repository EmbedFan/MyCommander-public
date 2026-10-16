#include "NetInfoScanner.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <set>
#include "DetachedThread.h"

namespace netinfo {

struct Scanner::Shared {
    explicit Shared(std::shared_ptr<INetInfoSource> s) : source(std::move(s)) {}
    std::mutex m;
    std::condition_variable cv;
    Snapshot snap;
    uint64_t version = 1;
    int running = 0;
    std::atomic<bool> cancel{false};
    bool wifiRunning = false;
    bool machinesRunning = false;
    std::set<std::wstring> sharesRunning;  // LowerKey(host)
    uint64_t lastWifiMs = 0;
    std::shared_ptr<INetInfoSource> source;
};

namespace {

// Runs `job` on a detached thread that owns a reference to the shared state: when the panel closes and the
// scanner starts over, a worker still inside a Windows call only ever touches its own, abandoned state.
template <typename SharedT, typename Job>
void Launch(const std::shared_ptr<SharedT>& sh, Job job) {
    {
        std::lock_guard<std::mutex> lock(sh->m);
        ++sh->running;
    }
    const bool started = RunDetached([sh, job]() {
        job();
        {
            std::lock_guard<std::mutex> lock(sh->m);
            --sh->running;
        }
        sh->cv.notify_all();
    });
    if (!started) {  // no thread: undo the count so Stop() does not wait for a worker that never ran
        std::lock_guard<std::mutex> lock(sh->m);
        --sh->running;
    }
}

Machine* FindMachine(Snapshot& snap, const std::wstring& key) {
    for (auto& m : snap.machines) {
        const std::wstring host = m.host.empty() ? m.name : m.host;
        if (LowerKey(host) == key) return &m;
    }
    return nullptr;
}

}  // namespace

Scanner::Scanner(std::shared_ptr<INetInfoSource> source)
    : source_(std::move(source)), shared_(std::make_shared<Shared>(source_)) {}

Scanner::~Scanner() { Stop(500); }

void Scanner::StartAll() {
    LocalInfo local = source_->ReadLocal();
    {
        std::lock_guard<std::mutex> lock(shared_->m);
        shared_->snap.computerName = local.computerName;
        shared_->snap.localShares = std::move(local.shares);
        shared_->snap.mapped = std::move(local.mapped);
        shared_->snap.localLoaded = true;
        ++shared_->version;
    }
    StartWifi();
    StartMachines();
}

void Scanner::StartWifi() {
    std::shared_ptr<Shared> sh = shared_;
    {
        std::lock_guard<std::mutex> lock(sh->m);
        if (sh->wifiRunning) return;
        sh->wifiRunning = true;
        sh->snap.wifiStatus = WifiStatus::Scanning;
        ++sh->version;
    }
    Launch(sh, [sh]() {
        auto cancelled = [sh]() { return sh->cancel.load(); };
        WifiResult r = sh->source->ReadWifi(cancelled);
        std::lock_guard<std::mutex> lock(sh->m);
        sh->wifiRunning = false;
        if (sh->cancel.load()) return;
        SortWifi(r.networks);
        sh->snap.wifiStatus = r.status;
        sh->snap.wifiDetail = r.detail;
        sh->snap.wifi = std::move(r.networks);
        ++sh->version;
    });
}

void Scanner::StartMachines() {
    std::shared_ptr<Shared> sh = shared_;
    {
        std::lock_guard<std::mutex> lock(sh->m);
        if (sh->machinesRunning) return;
        sh->machinesRunning = true;
        sh->snap.machinesScanning = true;
        sh->snap.machinesNote.clear();
        ++sh->version;
    }
    Launch(sh, [sh]() {
        auto cancelled = [sh]() { return sh->cancel.load(); };
        MachineScanResult r = sh->source->FindMachines(cancelled);
        std::lock_guard<std::mutex> lock(sh->m);
        sh->machinesRunning = false;
        if (sh->cancel.load()) return;
        // A machine seen again keeps the shares already read from it (and so does an open entry's view).
        for (auto& m : r.machines) {
            const std::wstring host = m.host.empty() ? m.name : m.host;
            if (Machine* old = FindMachine(sh->snap, LowerKey(host))) {
                if (old->shares == LoadState::Loaded || old->shares == LoadState::Failed) {
                    m.shares = old->shares;
                    m.shareList = old->shareList;
                    m.error = old->error;
                }
            }
        }
        SortMachines(r.machines);
        sh->snap.machines = std::move(r.machines);
        sh->snap.machinesNote = r.note;
        sh->snap.machinesScanning = false;
        sh->snap.machinesDone = true;
        ++sh->version;
    });
}

void Scanner::StartShares(const std::wstring& host) {
    std::shared_ptr<Shared> sh = shared_;
    const std::wstring key = LowerKey(host);
    {
        std::lock_guard<std::mutex> lock(sh->m);
        if (sh->sharesRunning.count(key)) return;
        Machine* m = FindMachine(sh->snap, key);
        if (!m) return;
        sh->sharesRunning.insert(key);
        m->shares = LoadState::Loading;
        m->error.clear();
        ++sh->version;
    }
    Launch(sh, [sh, host, key]() {
        auto cancelled = [sh]() { return sh->cancel.load(); };
        SharesResult r = sh->source->ReadSharesOf(host, cancelled);
        std::lock_guard<std::mutex> lock(sh->m);
        sh->sharesRunning.erase(key);
        if (sh->cancel.load()) return;
        Machine* m = FindMachine(sh->snap, key);
        if (!m) return;
        if (r.ok) {
            m->shares = LoadState::Loaded;
            m->shareList = std::move(r.shares);
            m->error.clear();
        } else {
            m->shares = LoadState::Failed;
            m->shareList.clear();
            m->error = r.error;
        }
        ++sh->version;
    });
}

void Scanner::Tick(uint64_t nowMs, uint64_t intervalMs) {
    bool start = false;
    {
        std::lock_guard<std::mutex> lock(shared_->m);
        if (shared_->lastWifiMs == 0) {
            shared_->lastWifiMs = nowMs ? nowMs : 1;
            return;
        }
        if (!shared_->wifiRunning && intervalMs > 0 && nowMs - shared_->lastWifiMs >= intervalMs) {
            shared_->lastWifiMs = nowMs;
            start = true;
        }
    }
    if (start) StartWifi();
}

Snapshot Scanner::Get() const {
    std::lock_guard<std::mutex> lock(shared_->m);
    return shared_->snap;
}

uint64_t Scanner::Version() const {
    std::lock_guard<std::mutex> lock(shared_->m);
    return shared_->version;
}

bool Scanner::Busy() const {
    std::lock_guard<std::mutex> lock(shared_->m);
    return shared_->running > 0;
}

bool Scanner::Stop(unsigned timeoutMs) {
    std::shared_ptr<Shared> old = shared_;
    old->cancel.store(true);
    bool done;
    {
        std::unique_lock<std::mutex> lock(old->m);
        done = old->cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return old->running == 0; });
    }
    shared_ = std::make_shared<Shared>(source_);
    return done;
}

}  // namespace netinfo
