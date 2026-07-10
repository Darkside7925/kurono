#ifndef KURONO_NET_NETFILTER_H
#define KURONO_NET_NETFILTER_H

#include "../kernel/types.h"

// Netfilter packet-filter chain engine (iptables-compatible semantics).
//
// Five hook points along the packet path:
//
//   PREROUTING  ─→ ROUTE? ─→ INPUT  (delivered to local socket)
//                      └──→ FORWARD (routed out)
//   OUTPUT      ─→ POSTROUTING (about to leave)
//
// Each hook holds a list of rules.  A rule is a 5-tuple match + action.
// Actions: ACCEPT (let it through), DROP (silently discard), REJECT
// (ICMP unreachable), LOG (record + continue), JUMP (target chain - not
// yet implemented).  Default policy per chain is configurable.
//
// We support TCP, UDP, ICMP matching plus interface name matching.
// Source / destination are 32-bit IPv4 addresses with masks.

namespace Netfilter {

    enum Hook : uint8_t {
        HOOK_PREROUTING  = 0,
        HOOK_INPUT       = 1,
        HOOK_FORWARD     = 2,
        HOOK_OUTPUT      = 3,
        HOOK_POSTROUTING = 4,
        HOOK_COUNT       = 5,
    };

    enum Action : uint8_t {
        NF_ACCEPT = 0,
        NF_DROP   = 1,
        NF_REJECT = 2,
        NF_LOG    = 3,
        NF_RETURN = 4,
    };

    enum MatchProto : uint8_t {
        PROTO_ANY  = 0,
        PROTO_ICMP = 1,
        PROTO_TCP  = 6,
        PROTO_UDP  = 17,
    };

    static const int NF_MAX_RULES_PER_HOOK = 64;
    static const int NF_MAX_IF_NAME        = 16;

    struct Rule {
        bool         active;
        MatchProto   proto;          // PROTO_ANY = match all
        uint32_t     src_ip;         // network order, 0 = any
        uint32_t     src_mask;       // 0 = match exact / any
        uint32_t     dst_ip;
        uint32_t     dst_mask;
        uint16_t     src_port_min;   // 0 = any
        uint16_t     src_port_max;
        uint16_t     dst_port_min;
        uint16_t     dst_port_max;
        char         in_if[NF_MAX_IF_NAME];   // "" = any
        char         out_if[NF_MAX_IF_NAME];
        Action       action;
        uint64_t     pkt_count;
        uint64_t     byte_count;
    };

    struct Chain {
        Action  default_policy;
        Rule    rules[NF_MAX_RULES_PER_HOOK];
        int     rule_count;
    };

    void Init();

    // Rule management.  Returns rule index (>=0) or -1 on overflow.
    int  AddRule(Hook h, const Rule& r);
    bool DeleteRule(Hook h, int index);
    void Flush(Hook h);
    void SetPolicy(Hook h, Action policy);

    // Evaluate a packet against a hook.  Inputs are in network byte order.
    // Returns the action to take.  Hits update rule pkt/byte counters.
    Action Evaluate(Hook h,
                    MatchProto proto,
                    uint32_t   src_ip,
                    uint32_t   dst_ip,
                    uint16_t   src_port,
                    uint16_t   dst_port,
                    const char* in_if,
                    const char* out_if,
                    uint32_t   pkt_len);

    // Convenience helpers used by the IP stack.
    bool ShouldDropInput(MatchProto proto, uint32_t src, uint32_t dst,
                         uint16_t sport, uint16_t dport,
                         const char* in_if, uint32_t pkt_len);
    bool ShouldDropOutput(MatchProto proto, uint32_t src, uint32_t dst,
                          uint16_t sport, uint16_t dport,
                          const char* out_if, uint32_t pkt_len);

    Chain* GetChain(Hook h);
}

#endif
