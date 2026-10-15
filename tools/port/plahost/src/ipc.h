// SPDX-FileCopyrightText: 2026 PLARecomp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// HIPC/CMIF on the calling thread: svcSendSyncRequest reads the message from the thread's
// TLS, the native service object answers, and the reply is written back in place.

#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "common.h"

namespace plahost::kernel {
struct Thread;
}

namespace plahost::ipc {

struct Buffer {
    u64 addr = 0;
    u64 size = 0;
};

class Service;

struct Request {
    u32 type = 0;
    u32 cmd = 0;
    std::vector<u8> data; // CMIF payload after the 16-byte header
    size_t pos = 0;
    bool has_pid = false;
    u64 pid = 0;
    std::vector<u32> copy_handles, move_handles;
    std::vector<Buffer> x, a, b, c;
    std::vector<std::shared_ptr<Service>> in_objects;

    template <typename T>
    T Pop() {
        T v{};
        if (pos + sizeof(T) <= data.size()) {
            std::memcpy(&v, data.data() + pos, sizeof(T));
        }
        pos += sizeof(T);
        return v;
    }
    void Align(size_t n) { pos = (pos + n - 1) & ~(n - 1); }
    /// Input buffer i: A (map) or X (pointer), whichever the client used.
    std::vector<u8> ReadBuffer(size_t i = 0) const;
    Buffer InBuffer(size_t i = 0) const;
    /// Output buffer i: B (map) or C (receive list).
    Buffer OutBuffer(size_t i = 0) const;
    size_t WriteBuffer(const void* src, size_t size, size_t i = 0) const;
};

struct Response {
    Result result = kOk;
    std::vector<u8> data;
    std::vector<u32> copy_handles, move_handles;
    std::vector<std::shared_ptr<Service>> objects;

    template <typename T>
    void Push(const T& v) {
        const size_t at = data.size();
        data.resize(at + sizeof(T));
        std::memcpy(data.data() + at, &v, sizeof(T));
    }
    void PushObject(std::shared_ptr<Service> s) { objects.push_back(std::move(s)); }
    void PushCopy(u32 handle) { copy_handles.push_back(handle); }
};

class Service : public std::enable_shared_from_this<Service> {
public:
    explicit Service(std::string name) : name_(std::move(name)) {}
    virtual ~Service() = default;
    const std::string& Name() const { return name_; }
    /// Answers one command. Unknown commands: log once, success with zeroed output.
    virtual void Handle(Request& rq, Response& rs);

protected:
    void Unknown(Request& rq, Response& rs);

private:
    std::string name_;
};

/// A service whose every command succeeds with empty output (logged once per command).
class StubService final : public Service {
public:
    using Service::Service;
};

class SessionState {
public:
    explicit SessionState(std::shared_ptr<Service> root) : root(std::move(root)) {}
    std::mutex lock;
    std::shared_ptr<Service> root;
    bool domain = false;
    std::map<u32, std::shared_ptr<Service>> objects;
    u32 next_id = 1;
};

/// A new client session handle to `service`.
u32 NewSession(std::shared_ptr<Service> service);
/// svcSendSyncRequest for `t`.
/// The message is at `msg_va` (the TLS, or a user buffer for the WithUserBuffer form).
Result SendSyncRequest(kernel::Thread* t, u32 handle, u64 msg_va);
/// svcConnectToNamedPort("sm:").
std::shared_ptr<Service> ConnectNamedPort(const std::string& name);
/// sm GetService.
std::shared_ptr<Service> GetService(const std::string& name);

std::string Report();

} // namespace plahost::ipc
