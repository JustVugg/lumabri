/* Shared engine identity and chat formatting for native and POSIX clients.
 * No engine startup, filesystem, transport or UI dependency. */
#ifndef LUMABRI_CHAT_FORMAT_H
#define LUMABRI_CHAT_FORMAT_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include "lumabri_families.h"
typedef enum {
    EK_INVALID = -1,
    EK_GLM = 0,        /* colibri monolith: framed, templates internally */
    EK_GLM53,          /* glm53:         GLM-5.3 template, serve codec        */
    EK_DEEPSEEK,       /* deepseek_v4:  <｜User｜>…<｜Assistant｜></think>       */
    EK_OLMOE,          /* olmoe:        |||IP_ADDRESS|||<|user|>…<|assistant|> */
    EK_QWEN36,         /* qwen36:       ChatML <|im_start|>…                   */
    EK_QWEN38,         /* qwen38:       system reasoning prompt + ChatML     */
    EK_INKLING,        /* inkling:      <|message_user|><|content_text|>…      */
    EK_KIMI,           /* kimi_k3:      K3CHAT1 byte-counted wire              */
    EK_FLINT, EK_LFM3, EK_QWEN_CODER, EK_MUSE, EK_MISTRAL
} EngKind;
/* A growable capture of the assistant's reply, to feed back as history. */
typedef struct { char *p; size_t len, cap; } Cap;
static int cap_add(Cap *c, const char *d, size_t n) {
    if (c->len == SIZE_MAX || n > SIZE_MAX - c->len - 1) return -1;
    if (c->len + n + 1 > c->cap) {
        size_t nc = c->cap ? c->cap : 4096;
        while (nc < c->len + n + 1) {
            if (nc > SIZE_MAX / 2) { nc = c->len + n + 1; break; }
            nc *= 2;
        }
        char *np = realloc(c->p, nc);
        if (!np) return -1;
        c->p = np; c->cap = nc;
    }
    memcpy(c->p + c->len, d, n); c->len += n; c->p[c->len] = 0;
    return 0;
}
static int cap_str(Cap *c, const char *s) { return cap_add(c, s, strlen(s)); }
/* Append a printf-formatted fragment (used for kimi's `M <role> <len>\n` heads). */
static int cap_addf(Cap *c, const char *fmt, ...) {
    char tmp[64];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof tmp) return -1;
    return cap_add(c, tmp, (size_t)n);
}
#include "lumabri_muse_chat.h"
#include "lumabri_mistral_chat.h"

static int artifact_is(const char *value, const char *name) {
    if (!value || !name) return 0;
    const char *base = strrchr(value, '/');
    base = base ? base + 1 : value;
    if (!strcmp(base, name)) return 1;
    size_t n = strlen(name);
    return !strncmp(base, name, n) && !strcmp(base + n, "_p2p");
}

/* Protocol and prompt-template dispatch is exact for the same reason model
 * family dispatch is exact. Unknown is not GLM: it is an error. */
static EngKind engine_kind_of(const char *engine) {
    if (!engine) return EK_INVALID;
    if (!strcmp(engine,"flint")) return EK_FLINT;
    if (!strcmp(engine,"lfm3") || !strcmp(engine,"lfm3_vl")) return EK_LFM3;
    if (!strcmp(engine,"qwen3_coder")) return EK_QWEN_CODER;
    if (!strcmp(engine,"muse_spark")) return EK_MUSE;
    if (!strcmp(engine,"mistral_large4")) return EK_MISTRAL;
    const LmbModelFamily *family=lmb_family_for(engine);
    if (family) engine=family->engine;
    if (artifact_is(engine, "colibri") || !strcmp(engine ? engine : "", "glm") ||
        !strcmp(engine ? engine : "", "glm_moe_dsa") ||
        !strcmp(engine ? engine : "", "glm5_moe")) return EK_GLM;
    if (artifact_is(engine, "glm53") ||
        !strcmp(engine ? engine : "", "glm5_next") ||
        !strcmp(engine ? engine : "", "glm5_next_text")) return EK_GLM53;
    if (artifact_is(engine, "deepseek_v4") || artifact_is(engine, "deepseek") ||
        artifact_is(engine, "deepseek_v41") || !strcmp(engine ? engine : "", "deepseek_v41_text"))
        return EK_DEEPSEEK;
    if (artifact_is(engine, "olmoe")) return EK_OLMOE;
    if (artifact_is(engine, "qwen36") ||
        !strcmp(engine ? engine : "", "qwen3_5_moe") ||
        !strcmp(engine ? engine : "", "qwen3_5_moe_text")) return EK_QWEN36;
    if (artifact_is(engine, "qwen38") ||
        !strcmp(engine ? engine : "", "qwen4_exp") ||
        !strcmp(engine ? engine : "", "qwen4_exp_text")) return EK_QWEN38;
    if (artifact_is(engine, "inkling") ||
        !strcmp(engine ? engine : "", "inkling_mm_model")) return EK_INKLING;
    if (artifact_is(engine, "kimi_k3") ||
        !strcmp(engine ? engine : "", "kimi") ||
        !strcmp(engine ? engine : "", "kimi_linear")) return EK_KIMI;
    return EK_INVALID;
}

static inline EngKind engine_family_kind(EngKind kind) {
    if(kind==EK_MISTRAL)return EK_GLM;
    return kind==EK_FLINT || kind==EK_LFM3 || kind==EK_QWEN_CODER || kind==EK_MUSE ? EK_QWEN36 : kind;
}
static inline const char *engine_format_name(EngKind kind) {
    switch (kind) {
    case EK_GLM: return "colibri"; case EK_GLM53: return "glm53";
    case EK_DEEPSEEK: return "deepseek_v4"; case EK_OLMOE: return "olmoe";
    case EK_QWEN36: return "qwen36"; case EK_QWEN38: return "qwen38";
    case EK_INKLING: return "inkling"; case EK_KIMI: return "kimi_k3";
    case EK_FLINT: return "flint"; case EK_LFM3: return "lfm3";
    case EK_QWEN_CODER: return "qwen3_coder"; case EK_MUSE: return "muse_spark";
    case EK_MISTRAL: return "mistral_large4"; default: return NULL;
    }
}
/* The serve-codec engines tokenize the SUBMIT payload as-is: coli_v4_prompt_build
 * and its per-engine siblings run only on the CLI path, so lumabri — standing in
 * for openai_server.py — applies each engine's own chat template. The whole
 * conversation is resent every turn (prefix + prior user/assistant turns + this
 * user turn ending at the assistant-generation marker); the serve reuses the KV
 * prefix, so it stays a real multi-turn chat without reprocessing the history.
 *
 * Each marker set mirrors its engine's serve/CLI source. The six-family tiny
 * release gate exercises GLM, Inkling, Kimi, OLMoE, Qwen and DeepSeek through
 * this exact codec and compares their generated token IDs with Colibri oracles.
 *
 * DeepSeek: ｜ is U+FF5C, ▁ is U+2581 — each its own literal so the next letter
 * is not eaten by the \x escape. </think> is the non-thinking "answer now" form. */
#define DS_PIPE "\xef\xbd\x9c"
#define DS_USCR "\xe2\x96\x81"
#define DS_BOS  "<" DS_PIPE "begin" DS_USCR "of" DS_USCR "sentence" DS_PIPE ">"
#define DS_USER "<" DS_PIPE "User" DS_PIPE ">"
#define DS_ASST "<" DS_PIPE "Assistant" DS_PIPE "></think>"
/* olmoe (olmoe.c fmt_user_turn): bos/eos is |||IP_ADDRESS|||; the first turn glues
 * <|user|> to it, later turns put a newline between. The reply is stored raw — the
 * next turn's leading |||IP_ADDRESS||| is the eos that closes it. */
#define OLMO_U0   "|||IP_ADDRESS|||<|user|>\n"
#define OLMO_UL   "|||IP_ADDRESS|||\n<|user|>\n"
#define OLMO_ASST "\n<|assistant|>\n"
/* GLM-5.2 (colibri.c's official chat_template.jinja path).  The monolithic
 * engine applies this internally; Segment Edge deliberately accepts exact
 * bytes, so only the Segment Serve2 path reaches this case. */
#define GLM_BOS  "[gMASK]<sop>"
#define GLM_U    "<|user|>"
#define GLM_ASST "<|assistant|><think></think>"
#define GLM53_ASST "<|assistant|><think>"
/* qwen36 (qwen36.c: ChatML, no BOS). */
#define QW_U    "<|im_start|>user\n"
#define QW_ASST "<|im_end|>\n<|im_start|>assistant\n<think>\n"
#define QW_AEND "<|im_end|>\n"
#define LFM3_BOS "<|startoftext|>"
#define QW38_SYS "<|im_start|>system\nReasoning effort is set to xhigh. Please " \
    "think carefully through the task, validate key assumptions, consider " \
    "plausible alternatives, and prioritize correctness, consistency, and " \
    "clarity in the final answer.<|im_end|>\n"
/* inkling (inkling.c chat template, thinking-effort system line dropped). */
#define INK_U    "<|message_user|><|content_text|>"
#define INK_ASST "<|end_message|><|message_model|>"
#define INK_AEND "<|end_message|>"

/* Flint's checkpoint template removes thinking markers from user/system
 * text, then escapes role markers. Apply replacements in template order. */
static int flint_text(Cap *out,const char *text) {
    const char *const old[]={"<think>","</think>","<|begin_of_text|>","<|end_of_text|>",
        "<|system|>","<|developer|>","<|user|>","<|assistant|>","<|observation|>"};
    const char *const replacement[]={"","","&lt;|begin_of_text|&gt;","&lt;|end_of_text|&gt;",
        "&lt;|system|&gt;","&lt;|developer|&gt;","&lt;|user|&gt;","&lt;|assistant|&gt;","&lt;|observation|&gt;"};
    Cap current={0}; if (cap_str(&current,text)) return -1;
    for (size_t i=0;i<sizeof old/sizeof *old;i++) {
        Cap next={0}; const char *p=current.p,*match; int bad=0;
        while ((match=strstr(p,old[i]))) {
            if (cap_add(&next,p,(size_t)(match-p)) || cap_str(&next,replacement[i])) { bad=1; break; }
            p=match+strlen(old[i]);
        }
        if (!bad) bad=cap_str(&next,p);
        free(current.p); current=next;
        if (bad) { free(current.p); return -1; }
    }
    int bad=cap_str(out,current.p); free(current.p); return bad;
}

static int lfm3_history_text(Cap *out,const char *reply) {
    const char *last=NULL,*p=reply;
    while ((p=strstr(p,"</think>"))) { last=p+8; p=last; }
    if (!last) return cap_str(out,reply);
    size_t n=strlen(last);
    while (*last && strchr(" \t\r\n\f\v",*last)) { last++; n--; }
    while (n && strchr(" \t\r\n\f\v",last[n-1])) n--;
    return cap_add(out,last,n);
}

/* Append one turn for engine kind k. `first` = the conversation is empty so far
 * (matters only where the leading marker differs by position). reply==NULL builds
 * the *pending* turn — user text ending at the assistant-generation marker, for
 * the SUBMIT payload; reply!=NULL builds a *completed* turn — user text plus the
 * assistant's reply, for the running history. 0, or -1 on OOM. */
static inline int serve2_turn(Cap *c, EngKind k, int first, const char *u, const char *reply) {
    (void)first;
    switch (k) {
    case EK_MISTRAL:
        if(first && cap_str(c,"[MODEL_SETTINGS]{\"reasoning_effort\": \"high\"}[/MODEL_SETTINGS]"))return -1;
        if(cap_str(c,"[INST]") || cap_str(c,u) || cap_str(c,"[/INST]"))return -1;
        return reply ? (lmb_mistral_history(c,reply) || cap_str(c,"</s>")) : 0;
    case EK_GLM:
        if (cap_str(c, GLM_U) || cap_str(c, u) || cap_str(c, GLM_ASST)) return -1;
        return reply ? cap_str(c, reply) : 0;
    case EK_GLM53:
        if (cap_str(c, GLM_U) || cap_str(c, u) || cap_str(c, GLM53_ASST)) return -1;
        return reply ? cap_str(c, reply) : 0;
    case EK_DEEPSEEK:
        if (cap_str(c, DS_USER) || cap_str(c, u) || cap_str(c, DS_ASST)) return -1;
        return reply ? cap_str(c, reply) : 0;
    case EK_OLMOE:
        if (cap_str(c, first ? OLMO_U0 : OLMO_UL) || cap_str(c, u) ||
            cap_str(c, OLMO_ASST)) return -1;
        return reply ? cap_str(c, reply) : 0;
    case EK_QWEN36:
        if (cap_str(c, QW_U) || cap_str(c, u) || cap_str(c, QW_ASST)) return -1;
        return reply ? (cap_str(c, reply) || cap_str(c, QW_AEND)) : 0;
    case EK_QWEN_CODER:
        if (cap_str(c,QW_U) || cap_str(c,u) ||
            cap_str(c,"<|im_end|>\n<|im_start|>assistant\n")) return -1;
        return reply ? (cap_str(c,reply) || cap_str(c,QW_AEND)) : 0;
    case EK_FLINT:
        if (cap_str(c,"<|user|>") || flint_text(c,u) || cap_str(c,"<|assistant|>")) return -1;
        return reply ? cap_str(c,reply) : 0;
    case EK_MUSE:
        if(cap_str(c,"<|start|>user<|message|>") || lmb_muse_text(c,u,0) ||
           cap_str(c,"<|eot|><|start|>assistant"))return -1;
        return reply ? (cap_str(c," to=user<|message|>") || lmb_muse_text(c,reply,0) || cap_str(c,"<|eot|>")) : 0;
    case EK_LFM3:
        if (cap_str(c,QW_U) || cap_str(c,u) ||
            cap_str(c,"<|im_end|>\n<|im_start|>assistant\n<think>")) return -1;
        return reply ? (cap_str(c,"</think>") || lfm3_history_text(c,reply) || cap_str(c,QW_AEND)) : 0;
    case EK_QWEN38:
        if (cap_str(c, QW_U) || cap_str(c, u) || cap_str(c, QW_ASST)) return -1;
        return reply ? (cap_str(c, reply) || cap_str(c, QW_AEND)) : 0;
    case EK_INKLING:
        if (cap_str(c, INK_U) || cap_str(c, u) || cap_str(c, INK_ASST)) return -1;
        return reply ? (cap_str(c, reply) || cap_str(c, INK_AEND)) : 0;
    case EK_KIMI:
        /* K3CHAT1 wire (kimi_k3.c chat_build_wire): `M <role> <bytes>\n<text>`,
         * byte-counted so the text may hold newlines. The engine appends the
         * assistant-generation open after the wire, so a pending user turn needs
         * no trailing marker. */
        if (cap_addf(c, "M user %zu\n", strlen(u)) || cap_str(c, u)) return -1;
        return reply ? (cap_addf(c, "M assistant %zu\n", strlen(reply)) ||
                        cap_str(c, reply)) : 0;
    default:
        return -1;
    }
}

/* The once-at-front SUBMIT prefix (kept out of the stored history). */
static inline int serve2_prefix(Cap *c, EngKind k) {
    if (k == EK_MISTRAL)  return cap_str(c,"<s>");
    if (k == EK_GLM)      return cap_str(c, GLM_BOS);
    if (k == EK_GLM53)    return cap_str(c, GLM_BOS);
    if (k == EK_DEEPSEEK) return cap_str(c, DS_BOS);
    if (k == EK_QWEN38)   return cap_str(c, QW38_SYS);
    if (k == EK_KIMI)     return cap_str(c, "K3CHAT1\n");
    if (k == EK_LFM3)     return cap_str(c, LFM3_BOS);
    if (k == EK_MUSE)     return lmb_muse_system(c,"",NULL);
    return 0;
}


#endif
