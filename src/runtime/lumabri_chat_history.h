/* Authenticated chat-history API. Requires the gateway's JSON/Cap helpers.
 * Client-saved text is a transcript, never a verified generation receipt. */
#ifndef LMB_CHAT_HISTORY_H
#define LMB_CHAT_HISTORY_H
#include "lumabri_chat_store.h"

/* A model's byte tokenizer can emit NUL. Preserve it as escaped JSON in
 * archived assistant text; it is NOT a valid C-string prompt/template. Keys,
 * metadata, user messages and ordinary inference requests remain NUL-free. */
static int api_history_parse(LmbJson *j, const void *text, size_t n, LmbJsonToken *tokens, unsigned cap) {
    if (!j || !text || !tokens || !cap) return -1;
    *j=(LmbJson){.text=text,.size=n,.tokens=tokens,.capacity=cap,.allow_nul=1};
    if (lmb_json_value(j,0)) return -1;
    lmb_json_space(j); return j->at==n ? 0 : -1;
}

static int api_conversation_validate(const LmbJson *j, unsigned index, uint8_t model[32]) {
    const char *const keys[]={"model","title","state","messages"};
    int f[4]; char id[65], title[129], state[24];
    if (lmb_json_fields(j,index,keys,4,f)) return -1;
    for (unsigned i=0; i<4; i++) if (f[i]<0) return -1;
    if (lmb_api_json_string(j,(unsigned)f[0],id,sizeof id) || strlen(id)!=64 || lmb_unhex(model,id,32) ||
        lmb_api_json_string(j,(unsigned)f[1],title,sizeof title) || !*title ||
        lmb_api_json_string(j,(unsigned)f[2],state,sizeof state)) return -1;
    const LmbJsonToken *array=&j->tokens[f[3]];
    if (array->kind!=LMB_JSON_ARRAY || array->children>130) return -1;
    unsigned count=array->children;
    if (!strcmp(state,"idle")) { if (count) return -1; }
    else if (!strcmp(state,"pending")) { if (!count || !(count&1)) return -1; }
    else if (!strcmp(state,"complete")) { if (!count || (count&1)) return -1; }
    else if (!strcmp(state,"interrupted")) { if (!count) return -1; }
    else return -1;
    const char *const mk[]={"role","content"}; unsigned turn=0;
    for (unsigned p=(unsigned)f[3]+1; p<array->next; p=j->tokens[p].next,turn++) {
        int fields[2]; char role[16]; size_t decoded;
        if (lmb_json_fields(j,p,mk,2,fields) || fields[0]<0 || fields[1]<0 ||
            lmb_api_json_string(j,(unsigned)fields[0],role,sizeof role) || strcmp(role,turn&1 ? "assistant" : "user")) return -1;
        const LmbJsonToken *content=&j->tokens[fields[1]];
        if (content->kind!=LMB_JSON_STRING || lmb_json_decode_mode(j->text+content->begin,content->end-content->begin,NULL,0,&decoded,turn&1) ||
            decoded>65536 || (!(turn&1) && !decoded)) return -1;
    }
    return 0;
}
static int api_conversation_read(int dir, const char *id, LmbChat *chat) {
    int rc=lmb_chat_load(dir,id,chat); if (rc) return rc;
    LmbJsonToken tokens[1024]; LmbJson j; uint8_t model[32];
    if (api_history_parse(&j,chat->data,chat->size,tokens,1024) || api_conversation_validate(&j,0,model)) {
        lmb_chat_free(chat); return LMB_CHAT_UNSAFE;
    }
    return LMB_CHAT_OK;
}
static void api_history_error(int fd, int rc) {
    if (rc==LMB_CHAT_MISSING) api_error(fd,404,"conversation_not_found");
    else if (rc==LMB_CHAT_CONFLICT) api_error(fd,409,"conversation_changed_reload_before_editing");
    else if (rc==LMB_CHAT_QUOTA) api_error(fd,409,"conversation_limit_32");
    else if (rc==LMB_CHAT_BUSY) api_error(fd,409,"history_busy_retry");
    else api_error(fd,500,"history_storage_unavailable");
}
static void api_history_reply(int fd, const LmbChat *chat) {
    Cap body={0};
    int bad=api_addf(&body,"{\"id\":\"%s\",\"revision\":%u,\"updated\":%llu,\"conversation\":",chat->id,
        chat->revision,(unsigned long long)chat->updated) || cap_add(&body,chat->data,chat->size) || cap_str(&body,"}\n");
    if (bad) api_error(fd,500,"allocation_failed"); else (void)api_response(fd,200,body.p);
    free(body.p);
}
static void api_history_list(int fd, int dir) {
    char ids[LMB_CHAT_MAX][33]; size_t count; int lock=lmb_api_access_lock(dir);
    if (lock<0) { api_history_error(fd,LMB_CHAT_BUSY); return; }
    int rc=lmb_chat_list(dir,ids,&count); Cap body={0};
    if (!rc && cap_str(&body,"{\"schema\":1,\"conversations\":[")) rc=LMB_CHAT_UNSAFE;
    for (size_t i=0; i<count && !rc; i++) {
        LmbChat chat; rc=api_conversation_read(dir,ids[i],&chat); if (rc) break;
        LmbJsonToken tokens[1024]; LmbJson j; int fields[4];
        const char *const keys[]={"model","title","state","messages"};
        int bad=api_history_parse(&j,chat.data,chat.size,tokens,1024) || lmb_json_fields(&j,0,keys,4,fields) ||
            api_addf(&body,"%s{\"id\":\"%s\",\"revision\":%u,\"updated\":%llu",i ? "," : "",chat.id,chat.revision,
                     (unsigned long long)chat.updated);
        for (unsigned k=0; k<3 && !bad; k++) {
            const LmbJsonToken *t=&j.tokens[fields[k]];
            bad=api_addf(&body,",\"%s\":",keys[k]) || cap_add(&body,chat.data+t->begin-1,t->end-t->begin+2);
        }
        if (!bad) bad=cap_str(&body,"}");
        lmb_chat_free(&chat); if (bad) rc=LMB_CHAT_UNSAFE;
    }
    close(lock);
    if (!rc && cap_str(&body,"]}\n")) rc=LMB_CHAT_UNSAFE;
    if (rc) api_history_error(fd,rc); else (void)api_response(fd,200,body.p);
    free(body.p);
}
static void api_history(int fd, int access_dir, const LmbApiUser *user, const LmbApiHttp *request,
                        const char *body, size_t length) {
    const char *base="/api/v1/conversations"; size_t prefix=strlen(base);
    const char *id=request->path+prefix; int creating=!*id;
    if (!creating && (*id++!='/' || !lmb_chat_id(id))) { api_error(fd,404,"conversation_not_found"); return; }
    int dir=lmb_chat_dir(access_dir,user);
    if (dir<0) { api_history_error(fd,LMB_CHAT_UNSAFE); return; }
    if (!strcmp(request->method,"GET") && !length) {
        if (creating) api_history_list(fd,dir);
        else { LmbChat chat; int rc=api_conversation_read(dir,id,&chat);
            if (rc) api_history_error(fd,rc); else { api_history_reply(fd,&chat); lmb_chat_free(&chat); } }
        close(dir); return;
    }
    LmbJsonToken tokens[1024]; LmbJson j; int fields[2]; uint32_t expected;
    const char *const keys[]={"revision","conversation"}; int deleting=!strcmp(request->method,"DELETE");
    if ((!deleting && strcmp(request->method,"POST")) || length>LMB_CHAT_BYTES ||
        api_history_parse(&j,body,length,tokens,1024) || lmb_json_fields(&j,0,keys,deleting ? 1 : 2,fields) ||
        fields[0]<0 || lmb_json_uint(&j,(unsigned)fields[0],&expected) ||
        (creating && (expected || deleting)) || (!creating && !expected)) {
        api_error(fd,400,"invalid_conversation_request"); close(dir); return;
    }
    const void *payload=NULL; size_t size=0; uint8_t model[32]; char generated[33];
    if (!deleting) {
        if (fields[1]<0 || api_conversation_validate(&j,(unsigned)fields[1],model)) {
            api_error(fd,400,"invalid_conversation"); close(dir); return;
        }
        if (!lmb_api_user_allows(user,model)) { api_error(fd,403,"model_not_authorized"); close(dir); return; }
        const LmbJsonToken *t=&j.tokens[fields[1]]; payload=j.text+t->begin; size=t->end-t->begin;
    }
    if (creating) { uint8_t bytes[16]; lmb_random(bytes,sizeof bytes); lmb_hex(generated,bytes,sizeof bytes); id=generated; }
    int rc=lmb_chat_commit(dir,id,expected,payload,size,deleting);
    if (rc) api_history_error(fd,rc);
    else if (deleting) (void)api_response(fd,200,"{\"deleted\":true}\n");
    else { LmbChat chat; rc=api_conversation_read(dir,id,&chat);
        if (rc) api_history_error(fd,rc); else { api_history_reply(fd,&chat); lmb_chat_free(&chat); } }
    close(dir);
}
#endif
