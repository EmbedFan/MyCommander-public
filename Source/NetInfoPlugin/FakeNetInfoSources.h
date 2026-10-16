#pragma once

// FEA-0010: an in-memory INetInfoSource for tests -- fixed answers, call counters, and hooks to make a call
// block (to prove that closing the panel never waits for a slow network call).

#include "NetInfoScanner.h"

#include <atomic>
#include <functional>
#include <map>
#include <mutex>

namespace netinfo {

class FakeSource : public INetInfoSource {
public:
    LocalInfo local;
    WifiResult wifi;
    MachineScanResult machines;
    std::map<std::wstring, SharesResult> shares;  // keyed by Machine::host

    std::function<void(const CancelFn&)> onReadWifi;      // runs before the answer is returned
    std::function<void(const CancelFn&)> onFindMachines;  // e.g. block until cancelled
    std::function<void(const CancelFn&)> onReadShares;

    std::atomic<int> localCalls{0}, wifiCalls{0}, machineCalls{0}, shareCalls{0};
    std::mutex m;
    std::vector<std::wstring> sharesAsked;

    LocalInfo ReadLocal() override {
        ++localCalls;
        return local;
    }
    WifiResult ReadWifi(const CancelFn& cancelled) override {
        ++wifiCalls;
        if (onReadWifi) onReadWifi(cancelled);
        return wifi;
    }
    MachineScanResult FindMachines(const CancelFn& cancelled) override {
        ++machineCalls;
        if (onFindMachines) onFindMachines(cancelled);
        return machines;
    }
    SharesResult ReadSharesOf(const std::wstring& host, const CancelFn& cancelled) override {
        ++shareCalls;
        {
            std::lock_guard<std::mutex> lock(m);
            sharesAsked.push_back(host);
        }
        if (onReadShares) onReadShares(cancelled);
        auto it = shares.find(host);
        if (it != shares.end()) return it->second;
        SharesResult r;
        r.error = L"Access denied";
        return r;
    }
};

}  // namespace netinfo
