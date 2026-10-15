// SPDX-FileCopyrightText: 2026 PLARecomp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ipc.h"

#include <algorithm>
#include <atomic>
#include <set>
#include <unordered_map>

#include "kernel.h"
#include "log.h"
#include "memory.h"

namespace plahost::ipc {
namespace {

constexpr u32 kSfci = 0x49434653; // "SFCI"
constexpr u32 kSfco = 0x4F434653; // "SFCO"
constexpr u32 kTypeClose = 2, kTypeRequest = 4, kTypeControl = 5, kTypeRequestCtx = 6, kTypeControlCtx = 7;
constexpr u16 kPointerBufferSize = 0x8000;

std::mutex g_stats_lock;
std::map<std::string, u64> g_stats;
std::set<std::string> g_unknown_logged;
const bool g_trace = [] {
    const char* e = std::getenv("PLAHOST_IPC_TRACE");
    return e && *e == '1';
}();

void Count(const std::string& key) {
    std::scoped_lock lk{g_stats_lock};
    ++g_stats[key];
}

struct Message {
    u32* m;
    size_t words = 0x40; // 0x100-byte message buffer
};

} // namespace

// ---- Request / Service ----

Buffer Request::InBuffer(size_t i) const {
    if (i < a.size() && a[i].size) return a[i];
    if (i < x.size()) return x[i];
    if (i < a.size()) return a[i];
    return {};
}

std::vector<u8> Request::ReadBuffer(size_t i) const {
    const Buffer in = InBuffer(i);
    std::vector<u8> out(in.size);
    if (in.size) {
        mem::Read(in.addr, out.data(), in.size);
    }
    return out;
}

Buffer Request::OutBuffer(size_t i) const {
    if (i < this->b.size() && this->b[i].size) return this->b[i];
    if (i < c.size()) return c[i];
    if (i < this->b.size()) return this->b[i];
    return {};
}

size_t Request::WriteBuffer(const void* src, size_t size, size_t i) const {
    const Buffer out = OutBuffer(i);
    const size_t n = std::min<size_t>(size, out.size);
    if (n) {
        mem::Write(out.addr, src, n);
    }
    return n;
}

void Service::Handle(Request& rq, Response& rs) {
    Unknown(rq, rs);
}

void Service::Unknown(Request& rq, Response& rs) {
    const std::string key = name_ + ":" + std::to_string(rq.cmd);
    {
        std::scoped_lock lk{g_stats_lock};
        if (g_unknown_logged.insert(key).second) {
            LOGW("IPC", "%s command %u not implemented natively: success with empty output", name_.c_str(), rq.cmd);
        }
    }
    rs.data.assign(0x40, 0);
}

// ---- sessions ----

u32 NewSession(std::shared_ptr<Service> service) {
    auto s = std::make_shared<kernel::Session>();
    s->state = std::make_shared<SessionState>(std::move(service));
    return kernel::AddHandle(s);
}

namespace {

// Parses the request in `m` into `rq`; returns the word index where the raw data starts.
size_t Parse(const u32* m, Request& rq, u32& num_data_words) {
    const u32 w0 = m[0], w1 = m[1];
    rq.type = w0 & 0xFFFF;
    const u32 nx = (w0 >> 16) & 0xF, na = (w0 >> 20) & 0xF, nb = (w0 >> 24) & 0xF, nw = (w0 >> 28) & 0xF;
    num_data_words = w1 & 0x3FF;
    const u32 c_flag = (w1 >> 10) & 0xF;
    size_t i = 2;
    if (w1 >> 31) {
        const u32 sh = m[i++];
        rq.has_pid = sh & 1;
        const u32 ncopy = (sh >> 1) & 0xF, nmove = (sh >> 5) & 0xF;
        if (rq.has_pid) {
            rq.pid = m[i] | (u64{m[i + 1]} << 32);
            i += 2;
        }
        for (u32 k = 0; k < ncopy; ++k) rq.copy_handles.push_back(m[i++]);
        for (u32 k = 0; k < nmove; ++k) rq.move_handles.push_back(m[i++]);
    }
    for (u32 k = 0; k < nx; ++k, i += 2) {
        const u32 d0 = m[i], d1 = m[i + 1];
        Buffer b;
        b.size = d0 >> 16;
        b.addr = d1 | (u64{(d0 >> 12) & 0xF} << 32) | (u64{(d0 >> 6) & 0x7} << 36);
        rq.x.push_back(b);
    }
    const auto abw = [&](std::vector<Buffer>* out, u32 n) {
        for (u32 k = 0; k < n; ++k, i += 3) {
            const u32 d0 = m[i], d1 = m[i + 1], d2 = m[i + 2];
            Buffer b;
            b.size = d0 | (u64{(d2 >> 24) & 0xF} << 32);
            b.addr = d1 | (u64{(d2 >> 28) & 0xF} << 32) | (u64{(d2 >> 2) & 0x7} << 36);
            if (out) out->push_back(b);
        }
    };
    abw(&rq.a, na);
    abw(&rq.b, nb);
    abw(nullptr, nw);
    const size_t raw = i;
    // Receive list (C descriptors) after the raw data.
    size_t ci = raw + num_data_words;
    const u32 nc = c_flag == 2 ? 1 : (c_flag > 2 ? c_flag - 2 : 0);
    for (u32 k = 0; k < nc && ci + 1 < 0x40; ++k, ci += 2) {
        const u32 d0 = m[ci], d1 = m[ci + 1];
        Buffer b;
        b.addr = d0 | (u64{d1 & 0xFFFF} << 32);
        b.size = d1 >> 16;
        rq.c.push_back(b);
    }
    return raw;
}

void WriteReply(u32* m, size_t raw_hint, bool domain, const Response& rs, const std::vector<u32>& object_words,
                const std::vector<u32>& move_out) {
    (void)raw_hint;
    std::vector<u32> words;
    const size_t payload_words = (rs.data.size() + 3) / 4;
    std::vector<u32> raw;
    if (domain) {
        raw.push_back(static_cast<u32>(object_words.size()));
        raw.push_back(0);
        raw.push_back(0);
        raw.push_back(0);
    }
    raw.push_back(kSfco);
    raw.push_back(0);
    raw.push_back(rs.result);
    raw.push_back(0);
    const size_t at = raw.size();
    raw.resize(at + payload_words, 0);
    if (!rs.data.empty()) {
        std::memcpy(raw.data() + at, rs.data.data(), rs.data.size());
    }
    if (domain) {
        raw.insert(raw.end(), object_words.begin(), object_words.end());
    }
    const bool special = !rs.copy_handles.empty() || !move_out.empty();
    u32 out[0x40]{};
    size_t i = 2;
    if (special) {
        out[i++] = (static_cast<u32>(rs.copy_handles.size()) << 1) | (static_cast<u32>(move_out.size()) << 5);
        for (u32 h : rs.copy_handles) out[i++] = h;
        for (u32 h : move_out) out[i++] = h;
    }
    const size_t aligned = AlignUp(i, 4);
    const u32 num_data = static_cast<u32>(4 + raw.size());
    if (aligned + raw.size() > 0x40) {
        LOGE("IPC", "reply does not fit the message buffer (%zu words)", raw.size());
    }
    for (size_t k = 0; k < raw.size() && aligned + k < 0x40; ++k) {
        out[aligned + k] = raw[k];
    }
    out[0] = 0;
    out[1] = num_data | (special ? 0x80000000u : 0);
    std::memcpy(m, out, sizeof out);
}

} // namespace

Result SendSyncRequest(kernel::Thread* t, u32 handle, u64 msg_va) {
    auto session = kernel::Get<kernel::Session>(handle);
    if (!session || !session->state) {
        return kResultInvalidHandle;
    }
    SessionState& st = *session->state;
    const u64 tls = msg_va;
    if (!mem::IsMapped(tls, 0x100)) {
        return kResultInvalidCurrentMemory;
    }
    u32* m = reinterpret_cast<u32*>(mem::Ptr(tls));
    Request rq;
    u32 num_data = 0;
    const size_t raw_word = Parse(m, rq, num_data);
    const u8* raw = reinterpret_cast<const u8*>(m) + AlignUp(raw_word * 4, 16);
    const u8* raw_end = reinterpret_cast<const u8*>(m) + std::min<size_t>(0x100, (raw_word + num_data) * 4);

    std::scoped_lock lk{st.lock};
    if (g_trace) {
        const u32* r = reinterpret_cast<const u32*>(raw);
        LOGI("IPC", "msg handle %#x %s%s type %u words %08x %08x raw %08x %08x %08x %08x", handle,
             st.root->Name().c_str(), st.domain ? " (domain)" : "", rq.type, m[0], m[1], r[0], r[1], r[2], r[3]);
    }
    if (rq.type == kTypeClose) {
        m[0] = 0;
        m[1] = 0;
        return kOk;
    }
    Response rs;
    std::shared_ptr<Service> target = st.root;
    bool domain_msg = false;
    u32 domain_object = 0;
    const u8* p = raw;
    if (rq.type == kTypeControl || rq.type == kTypeControlCtx) {
        u32 hdr[4];
        std::memcpy(hdr, p, 16);
        rq.cmd = hdr[2];
        Count(st.root->Name() + ":control" + std::to_string(rq.cmd));
        switch (rq.cmd) {
        case 0: // ConvertCurrentObjectToDomain
            st.domain = true;
            st.objects[st.next_id] = st.root;
            rs.Push<u32>(st.next_id++);
            break;
        case 1: { // CopyFromCurrentDomain
            u32 id;
            std::memcpy(&id, p + 16, 4);
            const auto it = st.objects.find(id);
            if (it == st.objects.end()) {
                rs.result = MakeResult(10, 410);
            } else {
                rs.move_handles.push_back(NewSession(it->second));
            }
            break;
        }
        case 2:
        case 4: { // CloneCurrentObject(Ex): another session to the same object (and domain)
            auto clone = std::make_shared<kernel::Session>();
            clone->state = session->state;
            rs.move_handles.push_back(kernel::AddHandle(clone));
            break;
        }
        case 3:
            rs.Push<u16>(kPointerBufferSize);
            break;
        default:
            LOGW("IPC", "unknown control command %u on %s", rq.cmd, st.root->Name().c_str());
            rs.result = MakeResult(10, 11);
            break;
        }
        WriteReply(m, raw_word, false, rs, {}, rs.move_handles);
        return kOk;
    }
    if (rq.type != kTypeRequest && rq.type != kTypeRequestCtx) {
        LOGW("IPC", "message type %u on %s", rq.type, st.root->Name().c_str());
        rs.result = MakeResult(10, 11);
        WriteReply(m, raw_word, false, rs, {}, {});
        return kOk;
    }
    size_t payload_size = static_cast<size_t>(raw_end > raw ? raw_end - raw : 0);
    std::vector<u32> in_object_ids;
    if (st.domain) {
        domain_msg = true;
        const u8 dcmd = p[0];
        const u8 nin = p[1];
        u16 dsize;
        std::memcpy(&dsize, p + 2, 2);
        std::memcpy(&domain_object, p + 4, 4);
        p += 16;
        if (dcmd == 2) { // CloseVirtualHandle
            st.objects.erase(domain_object);
            WriteReply(m, raw_word, true, rs, {}, {});
            return kOk;
        }
        for (u8 k = 0; k < nin; ++k) {
            u32 id;
            std::memcpy(&id, p + dsize + 4 * k, 4);
            in_object_ids.push_back(id);
        }
        payload_size = dsize;
        const auto it = st.objects.find(domain_object);
        if (it == st.objects.end()) {
            rs.result = MakeResult(10, 410);
            WriteReply(m, raw_word, true, rs, {}, {});
            return kOk;
        }
        target = it->second;
        for (u32 id : in_object_ids) {
            const auto o = st.objects.find(id);
            rq.in_objects.push_back(o == st.objects.end() ? nullptr : o->second);
        }
    }
    u32 hdr[4];
    std::memcpy(hdr, p, 16);
    if (hdr[0] != kSfci) {
        LOGW("IPC", "%s: request without SFCI magic (%08x)", target->Name().c_str(), hdr[0]);
    }
    rq.cmd = hdr[2];
    if (payload_size > 16) {
        rq.data.assign(p + 16, p + std::min<size_t>(payload_size, 0x100));
    }
    Count(target->Name() + ":" + std::to_string(rq.cmd));
    if (g_trace) {
        LOGI("IPC", "%s cmd %u (thread %llu)", target->Name().c_str(), rq.cmd, t->id);
    }
    target->Handle(rq, rs);

    std::vector<u32> object_words;
    std::vector<u32> move_out;
    if (domain_msg) {
        for (auto& o : rs.objects) {
            const u32 id = st.next_id++;
            st.objects[id] = o;
            object_words.push_back(id);
        }
    } else {
        for (auto& o : rs.objects) move_out.push_back(NewSession(o));
    }
    move_out.insert(move_out.end(), rs.move_handles.begin(), rs.move_handles.end());
    WriteReply(m, raw_word, domain_msg, rs, object_words, move_out);
    return kOk;
}

std::string Report() {
    std::scoped_lock lk{g_stats_lock};
    std::vector<std::pair<u64, std::string>> v;
    u64 total = 0;
    for (const auto& [k, n] : g_stats) {
        v.emplace_back(n, k);
        total += n;
    }
    std::sort(v.rbegin(), v.rend());
    std::string out = "IPC requests served natively: " + std::to_string(total) + "\n";
    for (const auto& [n, k] : v) {
        char b[160];
        std::snprintf(b, sizeof b, "  %-56s %10llu\n", k.c_str(), static_cast<unsigned long long>(n));
        out += b;
    }
    return out;
}

} // namespace plahost::ipc
