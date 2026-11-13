/* Adding a computer with a short code: the two commands and the two terminal
 * screens. Included by lumabri_home_ui.h once the household helpers exist.
 * The protocol is in src/runtime/lumabri_pairing_client.h; nothing here
 * handles a key or a code except to show it or hand it on. */
#ifndef LUMABRI_PAIRING_UI_H
#define LUMABRI_PAIRING_UI_H
#include "src/runtime/lumabri_pairing_client.h"

static const char *home_pair_error(int rc) {
    switch (rc) {
    case LMB_PAIR_E_CONNECT:
        return "No encrypted connection to the household. Check the address, that Lumabri is running on the owner's computer, and the firewall.";
    case LMB_PAIR_E_UNSUPPORTED:
        return "That household does not accept pairing codes; its Lumabri may be older. Update it, or join with the full household key.";
    case LMB_PAIR_E_NOT_OWNER:
        return "Pairing codes are created on the computer that owns the household. This computer does not.";
    case LMB_PAIR_E_REFUSED:
        return "The household refused the request. Start it again with lumabri start and retry.";
    case LMB_PAIR_E_CODE:
        return "That is not a pairing code. A code has eight letters and digits, such as 7K3M-9QXT.";
    case LMB_PAIR_E_FAILED:
        return "Pairing failed. Check the code on the owner's screen: it works once, for a few minutes, and a new code replaces the old one. Wait a few seconds before trying again.";
    case LMB_PAIR_E_IDENTITY:
        return "The computer that answered is not the household that was chosen. Nothing was sent to it and nothing was saved.";
    case LMB_PAIR_E_PINNED:
        return "The code was accepted, but this address is already pinned to a different computer identity. Nothing was saved. If the owner's computer was reinstalled, remove its line from known_hosts and ask for a new code.";
    default:
        return "The household sent an unexpected reply. Nothing was saved.";
    }
}

/* The lifetime the owner asks for. The variable can only shorten it: values
 * outside 1..default are ignored here and capped again by the tracker. */
static uint32_t home_pair_lifetime(void) {
    return (uint32_t)lmb_env_int("LUMABRI_PAIR_LIFETIME_S", 0, 1, (int)LMB_PAIR_LIFETIME_S);
}

/* `next->tracker` is the household to join. On 0 the code was accepted, the
 * tracker's identity is pinned, a fresh connection has authenticated with
 * the household key, the settings are saved through the usual path and
 * `next` is the new state. Otherwise nothing was saved and `notice` says why. */
static int home_join_with_code(const HomeSettings *current, HomeSettings *next, const char *code,
                               const uint8_t *expected_identity, char *notice, size_t cap) {
    char normalized[256];
    if (tracker_addr_set(normalized, sizeof normalized, next->tracker) ||
        checked_printf(next->tracker, sizeof next->tracker, "%s", normalized)) {
        snprintf(notice, cap, "The household address is too long."); return -1;
    }
    LmbPairJoined joined;
    int rc = lmb_pair_join(next->tracker, code, expected_identity, &joined);
    if (rc) { snprintf(notice, cap, "%s", home_pair_error(rc)); return -1; }
    memcpy(next->token, joined.household_key, sizeof next->token);
    next->owner = current->owner && !strcmp(current->tracker, next->tracker);
    int bad = setenv("LUMABRI_TOKEN", next->token, 1) ||
              home_connection_check(next->tracker, joined.tracker_identity);
    if (bad) snprintf(notice, cap, "The code was accepted, but the household did not accept its own key afterwards. Nothing was saved; ask for a new code.");
    else if (home_settings_save(next)) {
        bad = 1;
        snprintf(notice, cap, "The code was accepted, but the settings could not be saved. Check that your home directory is writable and ask for a new code.");
    }
    if (bad) {
        (void)setenv("LUMABRI_TOKEN", current->token, 1);
        lmb_cpace_wipe(next->token, sizeof next->token);
    }
    lmb_cpace_wipe(&joined, sizeof joined);
    return bad ? -1 : 0;
}

static int home_join_services_running(void) {
    HomeServiceSnapshot running;
    return !home_service_query("tracker", HOME_SVC_STATUS, NULL, &running) ||
           !home_service_query("donor", HOME_SVC_STATUS, NULL, &running) ||
           !home_service_query("prepare", HOME_SVC_STATUS, NULL, &running);
}

/* lumabri join --code CODE [HOST[:PORT]] */
static int cmd_join(int argc, char **argv) {
    const char *code = NULL, *address = NULL;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--code") && i + 1 < argc && !code) code = argv[++i];
        else if (argv[i][0] != '-' && !address) address = argv[i];
        else goto usage;
    }
    if (!code) goto usage;
    char checked[LMB_PAIR_CODE_LEN + 1];
    if (lmb_pair_code_normalize(checked, code)) return home_fail("%s", home_pair_error(LMB_PAIR_E_CODE));
    lmb_cpace_wipe(checked, sizeof checked);
    if (home_private_network()) return 1;
    if (home_join_services_running())
        return home_fail("This computer is running household services. Stop them with lumabri service stop before joining another household.");
    HomeSettings current; home_settings_load(&current);
    HomeSettings next = current;
    uint8_t expected[32]; int discovered = 0;
    if (address) {
        /* A bare host means the household's fixed port, not the swarm default. */
        int base = lmb_home_port_base();
        if (strchr(address, ':') ? checked_printf(next.tracker, sizeof next.tracker, "%s", address) :
            (base < 0 || checked_printf(next.tracker, sizeof next.tracker, "%s:%d", address, base)))
            return home_fail("The household address is not usable.");
    } else {
        uint8_t nonce[16]; lmb_random(nonce, sizeof nonce);
        LmbHomeFound found[LMB_HOME_DISC_MAX];
        int count = lmb_home_find(found, nonce), chosen = 0;
        if (!count)
            return home_fail("No household answered on this network. Give its address, shown on the owner's computer:\n"
                             "  lumabri join --code CODE HOST:PORT");
        if (count > 1) {
            printf("More than one household answered:\n");
            for (int i = 0; i < count; i++) {
                char hex[65]; lmb_hex(hex, found[i].identity, 32);
                printf("  %d  %-24.63s %-22s identity %.16s\n", i + 1, found[i].name, found[i].address, hex);
            }
            char line[32] = "";
            if (!isatty(0)) return home_fail("Run the command again with the address of yours:\n  lumabri join --code CODE HOST:PORT");
            printf("Number of the household to join: "); fflush(stdout);
            char *end = line;
            long picked = fgets(line, sizeof line, stdin) ? strtol(line, &end, 10) : 0;
            if (end == line || (*end && *end != '\n' && *end != '\r') || picked < 1 || picked > count)
                return home_fail("No household was chosen.");
            chosen = (int)picked - 1;
        }
        snprintf(next.tracker, sizeof next.tracker, "%s", found[chosen].address);
        memcpy(expected, found[chosen].identity, 32); discovered = 1;
        printf("Found %s at %s.\n", found[chosen].name, found[chosen].address);
    }
    char notice[400] = "";
    if (home_join_with_code(&current, &next, code, discovered ? expected : NULL, notice, sizeof notice))
        return home_fail("%s", notice);
    printf("Joined the household at %s.\n"
           "Its identity is pinned for this address, and the household key is saved in your private settings.\n"
           "Start sharing and the chat page with: lumabri start\n", next.tracker);
    lmb_cpace_wipe(&next, sizeof next); lmb_cpace_wipe(&current, sizeof current);
    return 0;
usage:
    fprintf(stderr, "lumabri join --code CODE [HOST[:PORT]]\n"
        "  Joins a household with the short code its owner sees after lumabri pair\n"
        "  (or Add a computer). Without an address the household is looked for on\n"
        "  this network. The full household key still works in the workspace: /join.\n");
    return 2;
}

/* Shared by the command and the screen: how a finished code is described. */
static void home_pair_outcome(const LmbPairStatus *status, char *text, size_t cap) {
    char identity[65]; lmb_hex(identity, status->joiner, 32);
    if (status->state == LMB_PAIR_USED)
        snprintf(text, cap, "A computer joined from %.63s (identity %.16s). The code no longer works.",
                 status->source[0] ? status->source : "an unknown address", identity);
    else if (status->state == LMB_PAIR_EXPIRED)
        snprintf(text, cap, "The code expired unused. Create a new one when the other computer is ready.");
    else if (status->state == LMB_PAIR_LOCKED)
        snprintf(text, cap, "The code was destroyed after %u wrong attempts, the last from %.63s. Nobody joined. "
                 "If that was not you, someone on this network is guessing.",
                 (unsigned)status->failed, status->source[0] ? status->source : "an unknown address");
    else if (status->state == LMB_PAIR_CANCELLED)
        snprintf(text, cap, "The code was withdrawn. Nobody joined.");
    else
        snprintf(text, cap, "The code was replaced by a newer one, or the household was restarted. Nobody joined with it.");
}

/* lumabri pair */
static int cmd_pair(int argc, char **argv) {
    if (argc) {
        (void)argv;
        fprintf(stderr, "lumabri pair\n"
            "  On the computer that owns the household: shows a short code that lets one\n"
            "  other computer join, then waits until it is used or expires (5 minutes).\n"
            "  On the other computer: lumabri join --code CODE\n");
        return 2;
    }
    if (home_private_network()) return 1;
    HomeSettings s; home_settings_load(&s);
    if (!s.owner || !s.token[0] || !s.tracker[0])
        return home_fail("This computer does not own a household. Run lumabri start here first; "
                         "a computer that joined a household cannot add others.");
    if (home_settings_activate(&s)) return home_fail("Cannot activate the saved household credentials.");
    g_stopping = 0; install_chat_signal_handlers();
    pid_t tracker_child = 0; char notice[400] = "";
    /* Our own tracker if it is running; otherwise start it as /create does. */
    if (home_connection_check(s.tracker, g_sec_pk) &&
        home_tracker_resume(&s, &tracker_child, notice, sizeof notice)) {
        fprintf(stderr, "%s\n", notice[0] ? notice : "The household could not be started."); return 1;
    }
    LmbPairOpened opened; LmbPairStatus status; char shown[LMB_PAIR_CODE_LEN + 2];
    int rc = lmb_pair_owner_open(s.tracker, home_pair_lifetime(), &opened), result = 1;
    if (rc) { home_stop_child(&tracker_child); return home_fail("%s", home_pair_error(rc)); }
    lmb_pair_code_format(shown, opened.code);
    printf("Add a computer to this household\n\n"
           "  Code:       %s\n"
           "  Household:  %s\n"
           "  Valid for %u seconds, for one computer, up to %u attempts.\n\n"
           "On the other computer run:  lumabri join --code %s\n"
           "If it cannot find this household, add the address:  lumabri join --code %s %s\n\n"
           "Waiting. Ctrl-C withdraws the code.\n",
           shown, s.tracker, (unsigned)opened.lifetime_s, (unsigned)opened.max_attempts, shown, shown, s.tracker);
    fflush(stdout);
    uint32_t reported = 0; unsigned lost = 0;
    memset(&status, 0, sizeof status); status.state = LMB_PAIR_WAITING;
    while (!g_stopping) {
        (void)poll(NULL, 0, 400);
        if (g_stopping) break;
        if (lmb_pair_owner_status(s.tracker, opened.id, &status)) {
            if (++lost < 10) { status.state = LMB_PAIR_WAITING; continue; }
            fprintf(stderr, "The household stopped answering. The code is gone with it.\n"); break;
        }
        lost = 0;
        if (status.failed > reported && status.state != LMB_PAIR_LOCKED) {
            printf("A wrong code was tried from %s (%u of %u).\n", status.source[0] ? status.source : "an unknown address",
                   (unsigned)status.failed, (unsigned)status.max_attempts);
            fflush(stdout);
        }
        reported = status.failed;
        if (status.state != LMB_PAIR_WAITING) {
            home_pair_outcome(&status, notice, sizeof notice);
            printf("%s\n", notice); fflush(stdout);
            result = status.state == LMB_PAIR_USED ? 0 : 1;
            break;
        }
    }
    if (g_stopping) {
        (void)lmb_pair_owner_cancel(s.tracker, opened.id);
        printf("The code was withdrawn. Nobody joined.\n"); result = 130;
    }
    lmb_cpace_wipe(&opened, sizeof opened); lmb_cpace_wipe(shown, sizeof shown);
    home_stop_child(&tracker_child);
    return result;
}

/* "Add a computer" in the workspace. The code lives on the alternate screen
 * only: leaving the screen removes it from the terminal. */
static void home_pair_screen(const HomeSettings *s, char *notice, size_t cap) {
    LmbPairOpened opened; LmbPairStatus status; char shown[LMB_PAIR_CODE_LEN + 2], outcome[400] = "";
    int rc = lmb_pair_owner_open(s->tracker, home_pair_lifetime(), &opened);
    if (rc) { snprintf(notice, cap, "%.*s", (int)cap - 1, home_pair_error(rc)); return; }
    lmb_pair_code_format(shown, opened.code);
    memset(&status, 0, sizeof status);
    status.state = LMB_PAIR_WAITING; status.remaining_s = opened.lifetime_s; status.max_attempts = opened.max_attempts;
    double polled = nowd(); unsigned lost = 0;
    HomeTerminal term; home_terminal_begin(&term);
    while (!g_stopping) {
        if (status.state == LMB_PAIR_WAITING && nowd() - polled >= .5) {
            LmbPairStatus fresh; polled = nowd();
            if (!lmb_pair_owner_status(s->tracker, opened.id, &fresh)) { status = fresh; lost = 0; }
            else if (++lost >= 10) status.state = LMB_PAIR_UNKNOWN;
            if (status.state != LMB_PAIR_WAITING) home_pair_outcome(&status, outcome, sizeof outcome);
        }
        ui_begin("add a computer");
        if (status.state == LMB_PAIR_WAITING) {
            ui_text(5, 5, UI_TEXT, "On the other computer open Lumabri, choose /join and type this code.");
            ui_printf(6, 5, UI_MUTED, "Or run there:  lumabri join --code %s", shown);
            ui_printf(9, 5, UI_ACCENT, "%s", shown);
            ui_printf(12, 5, UI_TEXT, "Valid for %u:%02u more, for one computer.",
                      (unsigned)(status.remaining_s / 60), (unsigned)(status.remaining_s % 60));
            ui_printf(13, 5, UI_MUTED, "If it cannot find this household, its address is %.80s", s->tracker);
            if (status.failed)
                ui_printf(15, 5, UI_SAND, "%u wrong code%s tried, the last from %.63s. After %u the code is destroyed.",
                          (unsigned)status.failed, status.failed == 1 ? " was" : "s were",
                          status.source[0] ? status.source : "an unknown address", (unsigned)status.max_attempts);
            ui_footer("Whoever types this code while it is valid joins your household.",
                      "Esc withdraw the code   Ctrl-C exit");
        } else {
            const char *p = outcome; int width = ui_w > 14 ? ui_w - 10 : 10; if (width > 120) width = 120;
            ui_text(5, 5, status.state == LMB_PAIR_USED ? UI_ACCENT : UI_SAND,
                    status.state == LMB_PAIR_USED ? "Computer added" : "No computer was added");
            for (int row = 8; *p && row < ui_h - 4; row++) {
                char line[121]; size_t n = strlen(p);
                if (n > (size_t)width) {
                    n = (size_t)width;
                    while (n > 0 && p[n] != ' ') n--;
                    if (!n) n = (size_t)width;
                }
                memcpy(line, p, n); line[n] = 0;
                ui_text(row, 5, UI_TEXT, line);
                p += n; while (*p == ' ') p++;
            }
            ui_footer(status.state == LMB_PAIR_USED ? "It can now choose Share resources to help." :
                      "Choose /pair again for a new code.", "Enter or Esc back   Ctrl-C exit");
        }
        ui_present();
        int key = home_key();
        if (key == 3) { g_stopping = 1; break; }
        if (status.state == LMB_PAIR_WAITING ? key == 27 : (key == 27 || key == '\r' || key == '\n')) break;
        (void)poll(NULL, 0, 100);
    }
    home_terminal_end(&term);
    if (status.state == LMB_PAIR_WAITING) {
        (void)lmb_pair_owner_cancel(s->tracker, opened.id);
        snprintf(notice, cap, "The code was withdrawn. Nobody joined.");
    } else snprintf(notice, cap, "%s", status.state == LMB_PAIR_USED ? "Computer added. It appears in Your computers once it shares resources." :
                    "No computer was added. Choose /pair for a new code.");
    lmb_cpace_wipe(&opened, sizeof opened); lmb_cpace_wipe(shown, sizeof shown);
}

#endif /* LUMABRI_PAIRING_UI_H */
