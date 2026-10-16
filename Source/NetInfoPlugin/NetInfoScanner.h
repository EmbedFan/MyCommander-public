#pragma once

// FEA-0010: the source interface behind every Windows call, and the Scanner that runs those calls on worker
// threads and keeps the latest result as a Snapshot. The panel (UI thread) only ever copies the snapshot out;
// no plugin-host call is made from a worker. Nothing here touches the network itself: the real calls live in
// NetInfoSources.cpp, and tests use FakeNetInfoSources.h.

#include "NetInfoModel.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace netinfo {

// Returns true once the scan was cancelled (the panel closed); a source checks it between its calls.
using CancelFn = std::function<bool()>;

struct LocalInfo {
    std::wstring computerName;
    std::vector<ShareEntry> shares;
    std::vector<MappedDrive> mapped;
};

struct WifiResult {
    WifiStatus status = WifiStatus::Failed;
    std::wstring detail;
    std::vector<WifiNetwork> networks;
};

struct MachineScanResult {
    std::vector<Machine> machines;
    std::wstring note;  // why the list may be short ("Network browsing is not available ...")
};

struct SharesResult {
    bool ok = false;
    std::vector<ShareEntry> shares;
    std::wstring error;
};

class INetInfoSource {
public:
    virtual ~INetInfoSource() = default;
    virtual LocalInfo ReadLocal() = 0;
    virtual WifiResult ReadWifi(const CancelFn& cancelled) = 0;
    virtual MachineScanResult FindMachines(const CancelFn& cancelled) = 0;
    // `host` is the Machine::host (a name or an address).
    virtual SharesResult ReadSharesOf(const std::wstring& host, const CancelFn& cancelled) = 0;
};

class Scanner {
public:
    explicit Scanner(std::shared_ptr<INetInfoSource> source);
    ~Scanner();
    Scanner(const Scanner&) = delete;
    Scanner& operator=(const Scanner&) = delete;

    // Reads this computer's own information on the calling thread (quick, no waiting on other machines) and
    // starts the Wi-Fi and machine scans in the background.
    void StartAll();
    // A Wi-Fi / machine rescan. A scan of that kind already running is left alone.
    // The Start* functions start worker threads over shared state; const would misdescribe them.
    // cppcheck-suppress functionConst
    void StartWifi();
    // cppcheck-suppress functionConst
    void StartMachines();
    // Reads the shares of the machine with this Machine::host, in the background.
    // cppcheck-suppress functionConst
    void StartShares(const std::wstring& host);
    // Starts a Wi-Fi rescan when the last one is older than `intervalMs` (and none is running).
    void Tick(uint64_t nowMs, uint64_t intervalMs);

    Snapshot Get() const;
    // Changes whenever the snapshot changes, so the panel can skip rebuilding its rows.
    uint64_t Version() const;
    bool Busy() const;

    // Cancels every scan and waits up to `timeoutMs` for the workers to notice. Returns true when all of them
    // finished. Afterwards the scanner starts over with an empty snapshot; a worker that did not finish keeps
    // running on its own private state and can no longer change anything the panel shows.
    bool Stop(unsigned timeoutMs);

private:
    struct Shared;
    std::shared_ptr<INetInfoSource> source_;
    std::shared_ptr<Shared> shared_;
};

}  // namespace netinfo
