/* Actual request normalization, engine submission and client wire serializers. */
#define main lumabri_cli_main
#include "lumabri.c"
#undef main
#include <assert.h>
#define MODEL "0101010101010101010101010101010101010101010101010101010101010101"
static ApiCompat request(int dialect,const char *tail) {
    char body[8192]; snprintf(body,sizeof body,"{\"model\":\"%s\",%s}",MODEL,tail);
    ApiCompat c={.dialect=dialect}; assert(!api_compat_request(&c,body,strlen(body))); return c;
}
static void rejected(int dialect,const char *tail) {
    char body[8192]; snprintf(body,sizeof body,"{\"model\":\"%s\",%s}",MODEL,tail);
    ApiCompat c={.dialect=dialect}; assert(api_compat_request(&c,body,strlen(body))); api_compat_free(&c);
}
static void drain(int fd,char *out,size_t cap) {
    ssize_t n=recv(fd,out,cap-1,MSG_DONTWAIT); assert(n>0); out[n]=0;
}
int main(void) {
    const char *messages="\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]";
    const char *bad[]={"\"tools\":[]", "\"stop\":\"end\"", "\"n\":2", "\"store\":true",
        "\"temperature\":1e999", "\"temperature\":-1", "\"top_p\":0", "\"seed\":-1",
        "\"max_tokens\":true", "\"max_tokens\":4097", "\"max_tokens\":8,\"max_completion_tokens\":8",
        "\"stream\":true,\"stream_options\":{\"include_usage\":1}", "\"stream_options\":{}",
        "\"stream\":false,\"stream\":true", "\"response_format\":{\"type\":\"json_object\"}"};
    for (unsigned i=0;i<sizeof bad/sizeof *bad;i++) {
        char tail[2048]; snprintf(tail,sizeof tail,"%s,%s",messages,bad[i]); rejected(API_OPENAI,tail);
    }
    rejected(API_OPENAI,"\"messages\":[{\"role\":\"assistant\",\"content\":\"prefill\"}]");
    rejected(API_OPENAI,"\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"image_url\",\"image_url\":{}}]}]");
    rejected(API_OPENAI,"\"messages\":[{\"role\":\"user\",\"content\":\"hi\"},{\"role\":\"system\",\"content\":\"late\"}]");
    rejected(API_ANTHROPIC,messages);
    rejected(API_ANTHROPIC,"\"max_tokens\":8,\"messages\":[{\"role\":\"system\",\"content\":\"hi\"}]");
    ApiCompat normalized=request(API_OPENAI,"\"max_completion_tokens\":9,\"messages\":["
        "{\"role\":\"system\",\"content\":\"instruction\"},{\"role\":\"developer\",\"content\":\"more\"},"
        "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"a\"},{\"type\":\"text\",\"text\":\"b\"}]},"
        "{\"role\":\"user\",\"content\":\"c\"},{\"role\":\"assistant\",\"content\":\"d\"},{\"role\":\"user\",\"content\":\"e\"}]");
    assert(!strcmp(normalized.system.p,"instruction\n\nmore"));
    assert(strstr(normalized.normalized.p,"\"max_tokens\":9"));
    LmbJsonToken tokens[128]; LmbJson j; int f[3]; const char *const keys[]={"model","messages","max_tokens"};
    assert(!lmb_json_parse(&j,normalized.normalized.p,normalized.normalized.len,tokens,128));
    assert(!lmb_json_fields(&j,0,keys,3,f));
    EngKind kinds[]={EK_OLMOE,EK_GLM,EK_GLM53,EK_DEEPSEEK,EK_KIMI,EK_QWEN36,EK_QWEN38,EK_INKLING};
    for (unsigned i=0;i<sizeof kinds/sizeof *kinds;i++) {
        assert(!api_compat_payload(&normalized,&j,(unsigned)f[1],kinds[i]));
        assert(strstr(normalized.payload.p,"instruction\n\nmore"));
        if (kinds[i]==EK_OLMOE) assert(strstr(normalized.payload.p,"more\n<|user|>\nab\n\nc"));
        if (kinds[i]==EK_QWEN38) {
            const char *start=strstr(normalized.payload.p,"<|im_start|>system"); assert(start && !strstr(start+1,"<|im_start|>system"));
        }
        free(normalized.payload.p); normalized.payload=(Cap){0};
    }
    api_compat_free(&normalized);
    /* EOS at the token limit is still a natural stop, not truncation. A failed
     * stream must never emit the success terminator expected by either SDK. */
    for (int dialect=API_OPENAI;dialect<=API_ANTHROPIC;dialect++) {
        char tail[1024]; snprintf(tail,sizeof tail,"%s,\"max_tokens\":8,\"stream\":true",messages);
        ApiCompat c=request(dialect,tail); int sockets[2];
        assert(!socketpair(AF_UNIX,SOCK_STREAM,0,sockets));
        assert(!api_compat_progress(sockets[0],&c,"PROGRESS 1 PREFILL 0 7"));
        assert(!api_compat_finish(sockets[0],&c,"STAT 8 1 0 0 7 0 STOP1 eos PERF1 8 7 .1 1 1.1"));
        char wire[8192]; drain(sockets[1],wire,sizeof wire);
        assert(strstr(wire,dialect==API_OPENAI ? "\"finish_reason\":\"stop\"" : "\"stop_reason\":\"end_turn\""));
        api_compat_free(&c);
        c=request(dialect,tail);
        assert(api_compat_finish(sockets[0],&c,"STAT 8 1 0 0 7 0"));
        api_compat_failure(sockets[0],&c,"Missing completion metadata");
        drain(sockets[1],wire,sizeof wire);
        assert(strstr(wire,"\"error\"") && !strstr(wire,"[DONE]") && !strstr(wire,"message_stop"));
        api_compat_free(&c); close(sockets[0]); close(sockets[1]);
    }
    /* UTF-8 is split across real DATA frames, including a four-byte scalar. */
    const unsigned char text[]="hi \xe2\x82\xac \xf0\x9f\x99\x82\n\"";
    for (int dialect=API_OPENAI;dialect<=API_ANTHROPIC;dialect++) for (int streaming=0;streaming<=1;streaming++) {
        char tail[1024]; snprintf(tail,sizeof tail,"%s,\"max_tokens\":8,\"stream\":%s%s",messages,streaming ? "true" : "false",
            dialect==API_OPENAI && streaming ? ",\"stream_options\":{\"include_usage\":true}" : "");
        ApiCompat c=request(dialect,tail); int pair[2]; assert(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));
        assert(!api_compat_progress(pair[0],&c,"PROGRESS 1 PREFILL 0 7"));
        for (unsigned i=0;i<sizeof text-1;i++) assert(!api_compat_delta(pair[0],&c,text+i,1));
        assert(!api_compat_finish(pair[0],&c,"STAT 8 1 0 0 7 0 STOP1 length PERF1 8 7 .1 1 1.1"));
        char wire[16384]; drain(pair[1],wire,sizeof wire);
        assert(strstr(wire,"max_tokens") || strstr(wire,"\"length\""));
        assert(strstr(wire,dialect==API_OPENAI ? "completion_tokens\":8" : "output_tokens\":8"));
        assert(strstr(wire,streaming ? (dialect==API_OPENAI ? "data: [DONE]" : "event: message_stop") : "HTTP/1.1 200 OK"));
        assert(strstr(wire,"\xe2\x82\xac") && strstr(wire,"\xf0\x9f\x99\x82"));
        close(pair[0]); close(pair[1]); api_compat_free(&c);
    }
    ApiCompat broken=request(API_OPENAI,messages); int pair[2]; assert(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));
    assert(!api_compat_delta(pair[0],&broken,(const unsigned char *)"\xe2",1));
    assert(api_compat_finish(pair[0],&broken,"STAT 1 0 0 0 7 STOP1 eos "));
    assert(api_compat_delta(pair[0],&broken,(const unsigned char *)"A",1)==0); /* incomplete until third byte */
    assert(api_compat_delta(pair[0],&broken,(const unsigned char *)"B",1));
    api_compat_free(&broken);
    ApiCompat options=request(API_OPENAI,"\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}],\"temperature\":0.25,\"top_p\":0.8,\"seed\":42");
    Engine engine={.to=pair[0],.request_seed_supported=1};
    assert(!cap_str(&options.payload,"payload")); assert(!api_compat_submit(&engine,&options,8,&options.seed));
    char wire[512]; drain(pair[1],wire,sizeof wire);
    unsigned id,slot,maximum; size_t bytes; double temperature,top_p; unsigned long long seed;
    assert(sscanf(wire,"SUBMIT %u %u %zu %u %lf %lf %llu",&id,&slot,&bytes,&maximum,&temperature,&top_p,&seed)==7);
    assert(bytes==7 && maximum==8 && temperature==.25 && top_p==.8 && seed==42 && strstr(wire,"\npayload\n"));
    engine.greedy_only=1; assert(api_compat_submit(&engine,&options,8,NULL));
    api_compat_free(&options); close(pair[0]); close(pair[1]);
    puts("CLIENT API CONTRACT: PASS (strict parameters, system roles, sampling, UTF-8, both streaming formats, token usage)");
    return 0;
}
