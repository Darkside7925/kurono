#include "netfilter.h"
#include "../drivers/serial.h"

namespace {
    Netfilter::Chain g_chains[Netfilter::HOOK_COUNT];

    inline bool seq(const char* a, const char* b) {
        if (!a || !b) return false;
        while (*a && *b && *a == *b) { a++; b++; }
        return *a == *b;
    }

    inline bool ip_match(uint32_t pkt_ip, uint32_t rule_ip, uint32_t mask) {
        // mask=0 means "any"
        if (mask == 0 && rule_ip == 0) return true;
        return ((pkt_ip ^ rule_ip) & mask) == 0;
    }

    inline bool port_match(uint16_t pkt_port, uint16_t lo, uint16_t hi) {
        if (lo == 0 && hi == 0) return true;
        return pkt_port >= lo && pkt_port <= hi;
    }

    inline bool if_match(const char* pkt_if, const char* rule_if) {
        if (!rule_if[0]) return true;
        if (!pkt_if)     return false;
        return seq(pkt_if, rule_if);
    }
}

namespace Netfilter {

void Init() {
    for (int i = 0; i < HOOK_COUNT; i++) {
        g_chains[i].default_policy = NF_ACCEPT;
        g_chains[i].rule_count     = 0;
        for (int r = 0; r < NF_MAX_RULES_PER_HOOK; r++) {
            g_chains[i].rules[r].active = false;
        }
    }
    SerialLogger::Log("Netfilter: 5 hooks initialized (PRE/IN/FWD/OUT/POST), policy=ACCEPT\r\n");
}

Chain* GetChain(Hook h) {
    if (h >= HOOK_COUNT) return nullptr;
    return &g_chains[h];
}

void SetPolicy(Hook h, Action policy) {
    if (h >= HOOK_COUNT) return;
    g_chains[h].default_policy = policy;
}

int AddRule(Hook h, const Rule& r) {
    if (h >= HOOK_COUNT) return -1;
    Chain& c = g_chains[h];
    if (c.rule_count >= NF_MAX_RULES_PER_HOOK) return -1;
    int idx = c.rule_count++;
    c.rules[idx]            = r;
    c.rules[idx].active     = true;
    c.rules[idx].pkt_count  = 0;
    c.rules[idx].byte_count = 0;
    return idx;
}

bool DeleteRule(Hook h, int index) {
    if (h >= HOOK_COUNT) return false;
    Chain& c = g_chains[h];
    if (index < 0 || index >= c.rule_count) return false;
    // Shift the tail down so indexes stay stable for subsequent ops.
    for (int i = index; i < c.rule_count - 1; i++) c.rules[i] = c.rules[i + 1];
    c.rule_count--;
    c.rules[c.rule_count].active = false;
    return true;
}

void Flush(Hook h) {
    if (h >= HOOK_COUNT) return;
    g_chains[h].rule_count = 0;
    for (int r = 0; r < NF_MAX_RULES_PER_HOOK; r++) {
        g_chains[h].rules[r].active = false;
    }
}

Action Evaluate(Hook h,
                MatchProto proto,
                uint32_t   src_ip,
                uint32_t   dst_ip,
                uint16_t   src_port,
                uint16_t   dst_port,
                const char* in_if,
                const char* out_if,
                uint32_t   pkt_len)
{
    if (h >= HOOK_COUNT) return NF_ACCEPT;
    Chain& c = g_chains[h];
    for (int i = 0; i < c.rule_count; i++) {
        Rule& r = c.rules[i];
        if (!r.active) continue;
        if (r.proto != PROTO_ANY && r.proto != proto) continue;
        if (!ip_match(src_ip, r.src_ip, r.src_mask)) continue;
        if (!ip_match(dst_ip, r.dst_ip, r.dst_mask)) continue;
        if (!port_match(src_port, r.src_port_min, r.src_port_max)) continue;
        if (!port_match(dst_port, r.dst_port_min, r.dst_port_max)) continue;
        if (!if_match(in_if,  r.in_if))  continue;
        if (!if_match(out_if, r.out_if)) continue;

        r.pkt_count++;
        r.byte_count += pkt_len;

        if (r.action == NF_LOG) {
            SerialLogger::Log("Netfilter: LOG match in chain ");
            SerialLogger::LogDec((int)h);
            SerialLogger::Log("\r\n");
            continue;             // LOG continues evaluation
        }
        return r.action;
    }
    return c.default_policy;
}

bool ShouldDropInput(MatchProto proto, uint32_t src, uint32_t dst,
                     uint16_t sport, uint16_t dport,
                     const char* in_if, uint32_t pkt_len)
{
    Action a = Evaluate(HOOK_INPUT, proto, src, dst, sport, dport,
                        in_if, nullptr, pkt_len);
    return a == NF_DROP || a == NF_REJECT;
}

bool ShouldDropOutput(MatchProto proto, uint32_t src, uint32_t dst,
                      uint16_t sport, uint16_t dport,
                      const char* out_if, uint32_t pkt_len)
{
    Action a = Evaluate(HOOK_OUTPUT, proto, src, dst, sport, dport,
                        nullptr, out_if, pkt_len);
    return a == NF_DROP || a == NF_REJECT;
}

}  // namespace Netfilter
