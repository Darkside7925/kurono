// lx_emul: in-container generic netlink dispatcher.
//
// There is no netlink socket and no ring-3 caller in M3: the caller is a thread
// inside the container. This file mirrors the kernel's genl family/op lookup,
// attribute validation, doit/dump callbacks and multicast fan-out for that one
// in-process caller. Replies go to a callback instead of a socket. (satoru)
//
// end (satoru)

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/skbuff.h>
#include <linux/netlink.h>
#include <net/netlink.h>
#include <net/genetlink.h>
#include <net/sock.h>

#include "lx_kshim.h"

// ── family registry ─────────────────────────────────────────────────────────
#define LX_GENL_MAX 8
static const struct genl_family *g_genl[LX_GENL_MAX];
static int g_genl_n;
static unsigned int g_genl_mcgrp_offset;

int genl_register_family(struct genl_family *family)
{
    if (!family || g_genl_n >= LX_GENL_MAX)
        return -ENOSPC;
    family->id = 0x10 + g_genl_n;
    family->mcgrp_offset = g_genl_mcgrp_offset;
    g_genl_mcgrp_offset += family->n_mcgrps;
    g_genl[g_genl_n++] = family;
    lx_log("[lx_genl] registered family ");
    lx_log_line(family->name ? family->name : "?");
    return 0;
}

int genl_unregister_family(const struct genl_family *family)
{
    for (int i = 0; i < g_genl_n; i++) {
        if (g_genl[i] != family)
            continue;
        for (int j = i; j + 1 < g_genl_n; j++)
            g_genl[j] = g_genl[j + 1];
        g_genl_n--;
        return 0;
    }
    return -ENOENT;
}

static const struct genl_family *lx_genl_family(const char *name)
{
    for (int i = 0; i < g_genl_n; i++)
        if (g_genl[i]->name && strcmp(g_genl[i]->name, name) == 0)
            return g_genl[i];
    return NULL;
}

// ── op lookup: mirror genl_get_cmd_full/small/split ────────────────────────
static int lx_genl_get_op(const struct genl_family *f, u8 cmd, bool dump,
                          struct genl_split_ops *out)
{
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < f->n_ops; i++) {
        const struct genl_ops *o = &f->ops[i];
        if (o->cmd != cmd)
            continue;
        // doit and dumpit share storage in genl_split_ops' union. (satoru)
        if (dump) {
            out->start = o->start;
            out->dumpit = o->dumpit;
            out->done = o->done;
        } else {
            out->doit = o->doit;
        }
        out->policy = o->policy ? o->policy : f->policy;
        out->maxattr = o->maxattr ? o->maxattr : f->maxattr;
        out->cmd = o->cmd;
        out->internal_flags = o->internal_flags;
        out->flags = o->flags;
        out->validate = o->validate;
        return 0;
    }
    for (int i = 0; i < f->n_small_ops; i++) {
        const struct genl_small_ops *o = &f->small_ops[i];
        if (o->cmd != cmd)
            continue;
        if (dump)
            out->dumpit = o->dumpit;
        else
            out->doit = o->doit;
        out->policy = f->policy;
        out->maxattr = f->maxattr;
        out->cmd = o->cmd;
        out->internal_flags = o->internal_flags;
        out->flags = o->flags;
        out->validate = o->validate;
        return 0;
    }
    for (int i = 0; i < f->n_split_ops; i++) {
        const struct genl_split_ops *o = &f->split_ops[i];
        if (o->cmd != cmd)
            continue;
        *out = *o;
        if (!out->policy)
            out->policy = f->policy;
        if (!out->maxattr)
            out->maxattr = f->maxattr;
        return 0;
    }
    return -ENOENT;
}

// Dump handlers read sock_net(cb->skb->sk); the shim has no netlink socket,
// so give them a bare sock bound to init_net. (satoru)
static struct sock lx_genl_dump_sock;
static bool lx_genl_dump_sock_ready;

static void lx_genl_dump_sock_init(void)
{
    if (!lx_genl_dump_sock_ready) {
        write_pnet(&lx_genl_dump_sock.sk_net, &init_net);
        lx_genl_dump_sock_ready = true;
    }
}

// ── replies / multicast to the in-process caller ───────────────────────────
// A dump reply skb can hold many netlink messages. Deliver one callback per
// nlmsghdr, then free the skb exactly once. (satoru)
typedef void (*lx_genl_msg_cb)(struct sk_buff *skb, struct nlmsghdr *nlh);

static void lx_genl_walk_skb(struct sk_buff *skb, lx_genl_msg_cb cb)
{
    int len = skb->len;
    struct nlmsghdr *nlh = (struct nlmsghdr *)skb->data;
    while (nlmsg_ok(nlh, len)) {
        if (nlh->nlmsg_type != NLMSG_DONE && nlh->nlmsg_type != NLMSG_ERROR)
            cb(skb, nlh);
        nlh = nlmsg_next(nlh, &len);
    }
    kfree_skb(skb);
}

// A doit reply is synchronous on the doit's own task. Record that owner with
// the callback so an unsolicited control-port unicast arriving on the RX thread
// cannot be consumed by a doit running on the main thread. (satoru)
struct lx_genl_reply_ctx {
    struct lx_task *owner;
    lx_genl_msg_cb cb;
};
static struct lx_genl_reply_ctx g_reply_ctx;
static volatile unsigned int g_reply_lock;

void lx_genl_set_reply_cb(lx_genl_msg_cb cb)
{
    lx_flock(&g_reply_lock);
    g_reply_ctx.owner = cb ? lx_current() : NULL;
    g_reply_ctx.cb = cb;
    lx_funlock(&g_reply_lock);
}

#define LX_GENL_PORTIDS 4
struct lx_genl_portid_listener {
    u32 portid;
    lx_genl_msg_cb fn;
};
static struct lx_genl_portid_listener g_portids[LX_GENL_PORTIDS];

int lx_genl_listen_portid(u32 portid, lx_genl_msg_cb fn)
{
    for (int i = 0; i < LX_GENL_PORTIDS; i++) {
        if (g_portids[i].fn && g_portids[i].portid == portid) {
            g_portids[i].fn = fn;
            return 0;
        }
    }
    for (int i = 0; i < LX_GENL_PORTIDS; i++) {
        if (!g_portids[i].fn) {
            g_portids[i].portid = portid;
            g_portids[i].fn = fn;
            return 0;
        }
    }
    return -ENOSPC;
}

void lx_genl_clear_portid(u32 portid)
{
    for (int i = 0; i < LX_GENL_PORTIDS; i++)
        if (g_portids[i].portid == portid)
            g_portids[i].fn = NULL;
}

struct lx_genl_listener {
    char family[GENL_NAMSIZ];
    char group[GENL_NAMSIZ];
    lx_genl_msg_cb fn;
};
#define LX_GENL_LISTENERS 8
static struct lx_genl_listener g_listeners[LX_GENL_LISTENERS];
static int g_listener_n;

int lx_genl_listen(const char *family, const char *group, lx_genl_msg_cb fn)
{
    if (!family || !group || !fn || g_listener_n >= LX_GENL_LISTENERS)
        return -ENOSPC;
    strscpy(g_listeners[g_listener_n].family, family, GENL_NAMSIZ);
    strscpy(g_listeners[g_listener_n].group, group, GENL_NAMSIZ);
    g_listeners[g_listener_n].fn = fn;
    g_listener_n++;
    return 0;
}

// genlmsg_multicast_allns(family, skb, portid, group): the group is an index
// into family->mcgrps. Deliver to the in-container listener on that group.
int genlmsg_multicast_allns(const struct genl_family *family,
                            struct sk_buff *skb, u32 portid,
                            unsigned int group)
{
    (void)portid;
    if (!family || !skb)
        return -EINVAL;
    const char *gname = NULL;
    if (group < family->n_mcgrps && family->mcgrps)
        gname = family->mcgrps[group].name;
    if (gname) {
        for (int i = 0; i < g_listener_n; i++) {
            if (strcmp(g_listeners[i].family, family->name) == 0 &&
                strcmp(g_listeners[i].group, gname) == 0) {
                lx_genl_walk_skb(skb, g_listeners[i].fn);
                return 0;
            }
        }
    }
    kfree_skb(skb);
    return 0;
}

// netlink_unicast: during a dispatch this is genlmsg_reply's path. hand the
// skb to the caller instead of a socket. outside a dispatch there is none.
int netlink_unicast(struct sock *ssk, struct sk_buff *skb, u32 portid, int nonblock)
{
    (void)ssk; (void)nonblock;
    if (!skb)
        return -EINVAL;
    // only the task that installed the reply callback may consume a doit
    // reply; any other task's unicast goes to its port-id listener. (satoru)
    lx_flock(&g_reply_lock);
    lx_genl_msg_cb reply = g_reply_ctx.cb;
    struct lx_task *owner = g_reply_ctx.owner;
    lx_funlock(&g_reply_lock);
    if (reply && owner == lx_current()) {
        lx_genl_walk_skb(skb, reply);
        return 0;
    }
    for (int i = 0; i < LX_GENL_PORTIDS; i++) {
        if (g_portids[i].fn && g_portids[i].portid == portid) {
            lx_genl_walk_skb(skb, g_portids[i].fn);
            return 0;
        }
    }
    kfree_skb(skb);
    return -ECONNREFUSED;
}

int netlink_broadcast_filtered(struct sock *ssk, struct sk_buff *skb,
                               u32 portid, u32 group, gfp_t allocation,
                               netlink_filter_fn filter, void *filter_data)
{
    (void)ssk; (void)portid; (void)allocation; (void)filter; (void)filter_data;
    if (!skb)
        return -EINVAL;
    // nl80211's genlmsg_multicast_netns is inline and lands here with the
    // global group id. Map it back to a family/name listener. (satoru)
    for (int i = 0; i < g_listener_n; i++) {
        for (int f = 0; f < g_genl_n; f++) {
            const struct genl_family *fam = g_genl[f];
            if (!fam || strcmp(fam->name, g_listeners[i].family) != 0)
                continue;
            for (unsigned int g = 0; g < fam->n_mcgrps; g++) {
                if (fam->mcgrp_offset + g != group || !fam->mcgrps)
                    continue;
                if (strcmp(fam->mcgrps[g].name, g_listeners[i].group) == 0) {
                    lx_genl_walk_skb(skb, g_listeners[i].fn);
                    return 0;
                }
            }
        }
    }
    kfree_skb(skb);
    return -ECONNREFUSED;
}

// ── message construction / dispatch ────────────────────────────────────────
struct nlmsghdr *__nlmsg_put(struct sk_buff *skb, u32 portid, u32 seq,
                             int type, int len, int flags)
{
    struct nlmsghdr *nlh = skb_put(skb, nlmsg_total_size(len));
    nlh->nlmsg_len = nlmsg_total_size(len);
    nlh->nlmsg_type = type;
    nlh->nlmsg_flags = flags;
    nlh->nlmsg_seq = seq;
    nlh->nlmsg_pid = portid;
    return nlh;
}

void *genlmsg_put(struct sk_buff *skb, u32 portid, u32 seq,
                  const struct genl_family *family, int flags, u8 cmd)
{
    if (!skb || !family)
        return NULL;
    struct nlmsghdr *nlh = nlmsg_put(skb, portid, seq, family->id,
                                     GENL_HDRLEN + family->hdrsize, flags);
    if (!nlh)
        return NULL;
    struct genlmsghdr *ghdr = (struct genlmsghdr *)nlmsg_data(nlh);
    ghdr->cmd = cmd;
    ghdr->version = family->version;
    ghdr->reserved = 0;
    // genlmsg_end() subtracts GENL_HDRLEN + NLMSG_HDRLEN, so the returned
    // pointer is the user header AFTER the genlmsghdr, as upstream. (satoru)
    return (void *)((char *)ghdr + GENL_HDRLEN);
}

static void lx_genl_print_extack(struct netlink_ext_ack *extack)
{
    if (extack && extack->_msg) {
        lx_log("[kld-nl] extack: ");
        lx_log_line(extack->_msg);
    }
}

static int lx_genl_parse(const struct genl_family *f, const struct genl_split_ops *op,
                         struct nlmsghdr *nlh, struct nlattr **tb,
                         struct netlink_ext_ack *extack)
{
    int maxtype = op->maxattr ? (int)op->maxattr : (int)f->maxattr;
    const struct nla_policy *policy = op->policy ? op->policy : f->policy;
    int hdrlen = GENL_HDRLEN + (int)f->hdrsize;
    memset(tb, 0, sizeof(struct nlattr *) * ((size_t)maxtype + 1));
    if (nlh->nlmsg_len < nlmsg_msg_size(hdrlen))
        return -EINVAL;
    unsigned int validate = NL_VALIDATE_STRICT;
    if (op->validate & GENL_DONT_VALIDATE_STRICT)
        validate = NL_VALIDATE_LIBERAL;
    return __nla_parse(tb, maxtype, nlmsg_attrdata(nlh, hdrlen),
                       nlmsg_attrlen(nlh, hdrlen), policy, validate, extack);
}

int lx_genl_doit(const char *family_name, struct sk_buff *skb)
{
    if (!family_name || !skb)
        return -EINVAL;
    const struct genl_family *f = lx_genl_family(family_name);
    if (!f)
        return -ENOENT;
    struct nlmsghdr *nlh = (struct nlmsghdr *)skb->data;
    if (nlh->nlmsg_len < nlmsg_msg_size(GENL_HDRLEN))
        return -EINVAL;
    struct genlmsghdr *ghdr = (struct genlmsghdr *)nlmsg_data(nlh);
    struct genl_split_ops op;
    int err = lx_genl_get_op(f, ghdr->cmd, false, &op);
    if (err)
        return err;
    if (!op.doit)
        return -EOPNOTSUPP;

    int maxtype = op.maxattr ? (int)op.maxattr : (int)f->maxattr;
    struct nlattr **tb = kcalloc((size_t)maxtype + 1, sizeof(*tb), GFP_KERNEL);
    if (!tb)
        return -ENOMEM;
    struct netlink_ext_ack extack = { };
    err = lx_genl_parse(f, &op, nlh, tb, &extack);
    if (err) {
        lx_genl_print_extack(&extack);
        kfree(tb);
        return err;
    }

    struct genl_info info = { };
    info.snd_seq = nlh->nlmsg_seq;
    info.snd_portid = nlh->nlmsg_pid;
    info.family = f;
    info.nlhdr = nlh;
    info.genlhdr = ghdr;
    info.attrs = tb;
    info.extack = &extack;
    genl_info_net_set(&info, &init_net);
    memset(info.user_ptr, 0, sizeof(info.user_ptr));

    if (f->pre_doit)
        err = f->pre_doit(&op, skb, &info);
    if (!err && op.pre_doit)
        err = op.pre_doit(&op, skb, &info);
    if (!err)
        err = op.doit(skb, &info);
    if (op.post_doit)
        op.post_doit(&op, skb, &info);
    if (f->post_doit)
        f->post_doit(&op, skb, &info);
    lx_genl_print_extack(&extack);
    kfree(tb);
    return err;
}

int lx_genl_dump(const char *family_name, struct sk_buff *skb,
                 lx_genl_msg_cb reply)
{
    if (!family_name || !skb || !reply)
        return -EINVAL;
    const struct genl_family *f = lx_genl_family(family_name);
    if (!f)
        return -ENOENT;
    struct nlmsghdr *nlh = (struct nlmsghdr *)skb->data;
    if (nlh->nlmsg_len < nlmsg_msg_size(GENL_HDRLEN))
        return -EINVAL;
    struct genlmsghdr *ghdr = (struct genlmsghdr *)nlmsg_data(nlh);
    struct genl_split_ops op;
    int err = lx_genl_get_op(f, ghdr->cmd, true, &op);
    if (err)
        return err;
    if (!op.dumpit)
        return -EOPNOTSUPP;

    int maxtype = op.maxattr ? (int)op.maxattr : (int)f->maxattr;
    struct nlattr **tb = kcalloc((size_t)maxtype + 1, sizeof(*tb), GFP_KERNEL);
    struct genl_dumpit_info *di = kzalloc(sizeof(*di), GFP_KERNEL);
    if (!tb || !di) {
        kfree(tb);
        kfree(di);
        return -ENOMEM;
    }
    struct netlink_ext_ack extack = { };
    err = lx_genl_parse(f, &op, nlh, tb, &extack);
    if (err) {
        lx_genl_print_extack(&extack);
        kfree(tb);
        kfree(di);
        return err;
    }
    di->op = op;
    di->info.family = f;
    di->info.snd_seq = nlh->nlmsg_seq;
    di->info.snd_portid = nlh->nlmsg_pid;
    di->info.nlhdr = nlh;
    di->info.genlhdr = ghdr;
    di->info.attrs = tb;
    di->info.extack = &extack;
    genl_info_net_set(&di->info, &init_net);
    memset(di->info.user_ptr, 0, sizeof(di->info.user_ptr));

    lx_genl_dump_sock_init();
    skb->sk = &lx_genl_dump_sock;
    struct netlink_callback cb = { };
    cb.nlh = nlh;
    cb.skb = skb;
    cb.data = di;
    cb.extack = &extack;
    cb.seq = nlh->nlmsg_seq;
    cb.flags = nlh->nlmsg_flags;

    if (op.start)
        err = op.start(&cb);
    if (!err) {
        for (int round = 0; round < 4096; round++) {
            struct sk_buff *reply_skb = alloc_skb(8192, GFP_KERNEL);
            if (!reply_skb) { err = -ENOMEM; break; }
            int rc = op.dumpit(reply_skb, &cb);
            if (reply_skb->len)
                lx_genl_walk_skb(reply_skb, reply);
            else
                kfree_skb(reply_skb);
            if (rc <= 0)
                break;
        }
    }
    if (op.done)
        op.done(&cb);
    lx_genl_print_extack(&extack);
    kfree(tb);
    kfree(di);
    return err;
}

// ── netlink notifier chain ──────────────────────────────────────────────────
#define LX_NL_NOTIFIERS 8
static struct notifier_block *g_nl_notifiers[LX_NL_NOTIFIERS];
static int g_nl_notifier_n;

int netlink_register_notifier(struct notifier_block *nb)
{
    if (!nb || g_nl_notifier_n >= LX_NL_NOTIFIERS)
        return -ENOSPC;
    g_nl_notifiers[g_nl_notifier_n++] = nb;
    return 0;
}

int netlink_unregister_notifier(struct notifier_block *nb)
{
    for (int i = 0; i < g_nl_notifier_n; i++) {
        if (g_nl_notifiers[i] != nb)
            continue;
        for (int j = i; j + 1 < g_nl_notifier_n; j++)
            g_nl_notifiers[j] = g_nl_notifiers[j + 1];
        g_nl_notifier_n--;
        return 0;
    }
    return -ENOENT;
}

int lx_genl_notify_release(u32 portid)
{
    struct netlink_notify n = {
        .protocol = NETLINK_GENERIC,
        .portid = portid,
    };
    for (int i = 0; i < g_nl_notifier_n; i++)
        if (g_nl_notifiers[i] && g_nl_notifiers[i]->notifier_call)
            g_nl_notifiers[i]->notifier_call(g_nl_notifiers[i], NETLINK_URELEASE, &n);
    return 0;
}

// end (satoru)
