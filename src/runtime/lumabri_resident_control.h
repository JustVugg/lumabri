/* Common authenticated control used by both the TUI and inference API.
 * Saved plans are hints, never authority to recreate an allocation. */
#ifndef LUMABRI_RESIDENT_CONTROL_H
#define LUMABRI_RESIDENT_CONTROL_H
static int home_resident_peer(const LmbResidentPlan *p, uint32_t i, int release) {
    if (!home_resident_plan_valid(p) || !lmb_home_nonzero(p->allocation, 32) ||
        i >= p->execution.count || !lmb_home_nonzero(p->peer_keys[i], 32)) return -1;
    int fd = lmb_connect_ms_io(p->execution.nodes[i].address, 600, 800);
    if (fd < 0) return -1;
    uint8_t body[64]; memcpy(body, p->allocation, 32);
    int bad = lmb_unhex(body + 32, p->root, 32) || !lmb_secure_peer_matches(fd, p->peer_keys[i]) || lmb_auth(fd);
    LmbMsg msg = {0};
    if (!bad) bad = lmb_send(fd, release ? LMB_HOME_RELEASE : LMB_HOME_QUERY, body, sizeof body, NULL, 0) || lmb_recv(fd, &msg);
    lmb_close(fd);
    LmbCur c = {msg.body, msg.body_len, 0}; uint32_t version, phase, segment, host;
    char reason[160];
    if (!bad) bad = msg.op != LMB_HOME_STATUS || msg.pay_len || lmb_cur_u32(&c, &version) ||
        version != LMB_HOME_VERSION || c.len - c.off < 32;
    if (!bad) { bad = memcmp(c.p + c.off, p->allocation, 32); c.off += 32; }
    if (!bad) bad = lmb_cur_u32(&c, &phase) || lmb_cur_u32(&c, &segment) ||
        lmb_cur_u32(&c, &host) || lmb_inventory_string(&c, reason, sizeof reason) || c.off != c.len;
    if (!bad) bad = release ? phase != LMB_HOME_CLOSED || segment || host :
        !segment || (p->execution.nodes[i].edge ? phase != LMB_HOME_READY || !host : phase != LMB_HOME_SEGMENT_READY);
    lmb_msg_free(&msg); return bad ? -1 : 0;
}
#endif
