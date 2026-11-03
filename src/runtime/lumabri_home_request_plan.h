/* Exact reviewed plan validation and approved donor handoff. Both native
 * requesters use these rules; platform owners supply source/process/UI I/O. */
#ifndef LMB_HOME_REQUEST_PLAN_H
#define LMB_HOME_REQUEST_PLAN_H
#include "src/runtime/lumabri_home_session.h"
#include "src/planner/lumabri_stage_placement.h"
#include "src/ui/lumabri_tui.h"
#include "src/runtime/lumabri_probe_deadline.h"
static int home_review_plan(LmbTuiState *st,LmbTuiModel *m,LmbClusterNode *nodes,
    uint32_t *indices,uint32_t *node_count,LmbClusterPlan *reviewed) {
    uint32_t count=0;
    for (uint32_t i = 0; i < st->nnodes; i++) {
        if (!st->nodes[i].addr[0] || !st->identities[i][0] || !lmb_tui_node_enabled(st, i)) continue;
        indices[count] = i; nodes[count] = st->nodes[i];
        count++;
    }
    /* Apply the reviewed snapshot, not a second independently chosen split.
     * Revalidate all reservations before any network side effect. */
    LmbClusterPlan plan = m->plan;
    if (plan.sessions != st->sessions || (plan.hybrid && st->sessions != 1))
        return home_fail("The plan's session limit changed. Refresh the model catalogue before approval.");
    if (!m->planned || !count || plan.nslices != count)
        return home_fail("No resident assignment for every selected computer. Refresh the plan and check each donor's offered RAM.");
    plan.edge_node = UINT32_MAX;
    for (uint32_t j = 0; j < plan.nslices; j++) {
        plan.slices[j].node = UINT32_MAX;
        for (uint32_t i = 0; i < count; i++) {
            if (m->plan.edge_node == indices[i]) plan.edge_node = i;
            if (m->plan.slices[j].node == indices[i]) plan.slices[j].node = i;
        }
    }
    if (lmb_home_plan_budgets(&m->shape, m->checkpoint_bytes, nodes, count,
        st->context, &plan) ||
        plan.state != LMB_PLAN_RESIDENT) {
        return home_fail("No complete resident plan found for the selected computers. Keep Share resources open and check their offered RAM.");
    }
    /* Discovery is not proof that inbound connections work. Authenticate the
     * planned donors before indexing/starting a source, without any OFFER,
     * reservation or engine launch. Recheck again when actually sending. */
    for (uint32_t j = 0; j < plan.nslices; j++) {
        uint32_t i = plan.slices[j].node;
        uint8_t expected[32];
        if (lmb_unhex(expected, st->identities[indices[i]], 32))
            return home_fail("Invalid identity for %.64s. Refresh the computer list.", nodes[i].name);
        LmbSocket fd = lmb_connect_ms_io(nodes[i].addr, 2000, home_control_io_ms(2000));
        if (fd == LMB_INVALID_SOCKET) return home_fail("Cannot reach %.64s at %.64s: %s. Check inbound permissions on that computer; no model was loaded.",
                                    nodes[i].name, nodes[i].addr, lmb_connect_why());
        int matched = lmb_secure_peer_matches(fd, expected);
        int authenticated = matched && !lmb_auth(fd);
        if (authenticated && plan.hybrid) {
            LmbMsg reply = {0};
            authenticated = !lmb_send(fd, LMB_HOME_FEATURES, NULL, 0, NULL, 0) &&
                !lmb_recv(fd, &reply) && reply.op == LMB_OK && reply.body_len == 4 &&
                !reply.pay_len && lmb_get32(reply.body) == 2;
            lmb_msg_free(&reply);
            if (!authenticated) {
                lmb_close(fd);
                return home_fail("Update Lumabri on %.64s: this plan needs approved resident Hybrid support. No resources were shared.", nodes[i].name);
            }
        }
        lmb_close(fd);
        if (!authenticated) return home_fail("Cannot authenticate %.64s at %.64s. %s No model was loaded.",
            nodes[i].name, nodes[i].addr, matched ? "Check the household key and whether the donor is responding in time." : "The donor identity changed; refresh the computer list.");
    }
    *node_count=count;*reviewed=plan;return 0;
}
static int home_offer_plan(HomeSession *s,LmbTuiState *st,LmbTuiModel *m,
    const LmbClusterNode *nodes,const uint32_t *indices,const LmbClusterPlan *plan,
    const uint8_t pk[32],const uint8_t id[32],const uint8_t root[32],const char *model,
    uint64_t model_bytes,uint64_t metadata_bytes,int resident,uint8_t edge_pk[32]) {

    if (lmb_unhex(edge_pk, st->identities[indices[plan->edge_node]], 32)) return -1;
    /* Each recipient gets one exact range, and only the selected Edge owner
     * gets permission to host text. Edge must also own a Segment slice in v1. */
    int have_edge = 0;
    for (uint32_t j = 0; j < plan->nslices; j++) {
        const LmbSlice *slice = &plan->slices[j]; uint32_t n = slice->node;
        LmbHomeOffer *o = &s->offers[s->count];
        memcpy(o->id, id, 32); memcpy(o->requester, pk, 32);
        memcpy(o->edge_peer, edge_pk, 32); memcpy(o->model_root, root, 32);
        snprintf(o->model, sizeof o->model, "%s", model);
        snprintf(o->model_type, sizeof o->model_type, "%s", m->shape.model_type);
        snprintf(o->tracker, sizeof o->tracker, "%s", st->tracker);
        o->begin = slice->layer_begin; o->end = slice->layer_end; o->layers = m->shape.layers;
        o->context = st->context; o->threads = nodes[n].threads ? nodes[n].threads : 1;
        o->sessions = st->sessions > 1 ? st->sessions : 0;
        if (o->threads > 256) o->threads = 256;
        o->max_new = st->quick_calibration ? LMB_QUICK_PROBE_TOKENS : (st->max_new ? st->max_new : 256);
        o->model_bytes = model_bytes;
        o->runs_edge = n == plan->edge_node;
        if (plan->hybrid) {
            o->hybrid_role = o->runs_edge ? LMB_HYBRID_COORDINATOR : LMB_HYBRID_ACCELERATOR;
            if (o->runs_edge) {
                memcpy(o->hybrid.allocation, id, 32); memcpy(o->hybrid.root, root, 32);
                for (uint32_t k = 0; k < plan->nslices; k++) {
                    const LmbSlice *a = &plan->slices[k];
                    if (a->node == plan->edge_node) continue;
                    LmbHybridPeer *p = &o->hybrid.peers[o->hybrid.count++];
                    p->begin = a->layer_begin; p->end = a->layer_end;
                    if (lmb_unhex(p->key, st->identities[indices[a->node]], 32)) return -1;
                }
            }
        }
        LmbHomeReservation reservation;
        if (lmb_home_reservation_sessions(&m->shape, model_bytes, o->begin, o->end,
                                o->context, st->sessions, o->runs_edge, &reservation)) {
            home_fail("The model's memory requirements cannot be represented safely.");
            return -1;
        }
        o->ram_bytes = reservation.total_bytes;
        if (plan->hybrid) o->ram_bytes = lmb_budget_add(o->ram_bytes, lmb_home_hybrid_extra(&m->shape));
        o->edge_ram_bytes = reservation.edge_bytes;
        /* Resident weights never occupy the file mirror. Keep a bounded
         * allowance for metadata, signed block hashes, sparse-file maps and
         * logs; do not reserve two complete checkpoints on each donor. */
        uint64_t disk_input = resident ? metadata_bytes : model_bytes;
        o->disk_bytes = lmb_budget_add(lmb_budget_add(disk_input, disk_input), UINT64_C(256) << 20);
        if (resident)
            o->disk_bytes = lmb_budget_add(o->disk_bytes, model_bytes / 1024);
        if (o->disk_bytes == UINT64_MAX) {
            home_fail("The model's disk requirements exceed the supported size.");
            return -1;
        }
        if (o->ram_bytes > nodes[n].ram_budget_bytes) {
            home_fail("%.64s needs %.2f GB for this plan, but offers %.2f GB. No allocation was committed.",
                      nodes[n].name, o->ram_bytes / 1e9, nodes[n].ram_budget_bytes / 1e9);
            return -1;
        }
        if (!lmb_home_offer_valid(o)) { home_fail("The proposed allocation failed validation. No allocation was committed."); return -1; }
        snprintf(s->names[s->count], sizeof s->names[0], "%s", nodes[n].name);
        snprintf(s->addresses[s->count], sizeof s->addresses[0], "%s", nodes[n].addr);
        uint8_t recipient[32];
        if (lmb_unhex(recipient, st->identities[indices[n]], 32)) return -1;
        memcpy(s->peer_keys[s->count], recipient, 32);
        LmbSocket fd = lmb_connect_ms_io(nodes[n].addr, 1500, home_control_io_ms(1000));
        if (fd == LMB_INVALID_SOCKET) {
            home_fail("Cannot reach %.64s at %.64s while sending the request: %s.", nodes[n].name, nodes[n].addr, lmb_connect_why());
            return -1;
        }
        lmb_set_io_timeout(fd, home_control_io_ms(1000));
        if (lmb_socket_noinherit(fd)) {lmb_close(fd);return -1;}
        if (!lmb_secure_peer_matches(fd, recipient) || lmb_auth(fd)) {
            home_fail("Authentication failed while requesting %.64s. Refresh the household identity and key.", nodes[n].name);
            lmb_close(fd); return -1;
        }
        LmbBuf b = {0};
        int bad = lmb_home_offer_pack(&b, o) || lmb_send(fd, LMB_HOME_OFFER, b.p, (uint32_t)b.len, NULL, 0);
        free(b.p);
        if (bad) { home_fail("Cannot send the request to %.64s. Check its connection.", nodes[n].name); lmb_close(fd); return -1; }
        s->fd[s->count] = fd; s->phase[s->count] = LMB_HOME_PENDING;
        if (o->runs_edge) { s->edge = s->count; have_edge = 1; }
        s->count++;
        /* Drain the initial acknowledgement before starting heartbeats. A
         * busy donor replies then closes; sending PULSE first can replace its
         * queued rejection with a write error and hide the actual reason. */
        if (home_session_receive(s, s->count - 1)) {
            if (!s->reason[s->count - 1][0])
                home_fail("No valid allocation acknowledgement from %.64s. Check its connection; no plan was committed.", nodes[n].name);
            return -1;
        }
        if (s->phase[s->count - 1] != LMB_HOME_PENDING) {
            home_fail("Unexpected initial allocation status from %.64s. No plan was committed.", nodes[n].name);
            return -1;
        }
    }
    if (!have_edge || !s->count) return -1;
    return 0;
}
#endif
