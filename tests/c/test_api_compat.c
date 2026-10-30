/* Actual request normalization, engine submission and client wire serializers. */
#define main lumabri_cli_main
#include "lumabri.c"
#undef main
#include <assert.h>
#define MODEL "0101010101010101010101010101010101010101010101010101010101010101"
static ApiCompat request(int dialect,const char *tail) {
    char body[8192]; snprintf(body,sizeof body,"{\"model\":\"%s\",%s}",MODEL,tail);
    ApiCompat c={.dialect=dialect};
    assert(!(dialect==API_RESPONSES ? api_responses_request(&c,body,strlen(body)) : api_compat_request(&c,body,strlen(body)))); return c;
}
static void rejected(int dialect,const char *tail) {
    char body[8192]; snprintf(body,sizeof body,"{\"model\":\"%s\",%s}",MODEL,tail);
    ApiCompat c={.dialect=dialect};
    assert(dialect==API_RESPONSES ? api_responses_request(&c,body,strlen(body)) : api_compat_request(&c,body,strlen(body))); api_compat_free(&c);
}
static void drain(int fd,char *out,size_t cap) {
    ssize_t n=recv(fd,out,cap-1,MSG_DONTWAIT); assert(n>0); out[n]=0;
}
static void valid_wire(char *wire) {
    LmbJsonToken tokens[4096]; LmbJson j;
    if (!strncmp(wire,"HTTP/",5)) {
        char *body=strstr(wire,"\r\n\r\n"); assert(body);
        assert(!lmb_json_parse(&j,body+4,strlen(body+4),tokens,4096)); return;
    }
    for (char *line=strstr(wire,"data: ");line;line=strstr(line+1,"data: ")) {
        char *end=strstr(line,"\n\n"); assert(end);
        if (strncmp(line+6,"[DONE]",6)) assert(!lmb_json_parse(&j,line+6,(size_t)(end-line-6),tokens,4096));
    }
}
static void fixture(const char *dir,const char *name,const char *wire) {
    if (!dir) return;
    char path[1024]; assert(snprintf(path,sizeof path,"%s/%s",dir,name)<(int)sizeof path);
    FILE *out=fopen(path,"w"); assert(out); assert(fputs(wire,out)>=0); assert(!fclose(out));
}
int main(int argc,char **argv) {
    assert(argc<=2); const char *fixtures=argc==2 ? argv[1] : NULL;
    const char *png="iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+a7z8AAAAASUVORK5CYII=";
    for (int dialect=API_OPENAI;dialect<=API_RESPONSES;dialect++) {
        char tail[2048];
        if(dialect==API_OPENAI) snprintf(tail,sizeof tail,"\"max_tokens\":4,\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/png;base64,%s\"}}]}]",png);
        else if(dialect==API_ANTHROPIC) snprintf(tail,sizeof tail,"\"max_tokens\":4,\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"image\",\"source\":{\"type\":\"base64\",\"media_type\":\"image/png\",\"data\":\"%s\"}}]}]",png);
        else snprintf(tail,sizeof tail,"\"max_output_tokens\":4,\"input\":[{\"role\":\"user\",\"content\":[{\"type\":\"input_image\",\"image_url\":\"data:image/png;base64,%s\"}]}]",png);
        ApiCompat c=request(dialect,tail); assert(c.images && c.images->count==1);
        LmbJsonToken ts[128]; LmbJson jj; int fields[3]; const char *const kk[]={"model","messages","max_tokens"};
        assert(!lmb_json_parse(&jj,c.normalized.p,c.normalized.len,ts,128)); assert(!lmb_json_fields(&jj,0,kk,3,fields));
        assert(!api_compat_payload(&c,&jj,(unsigned)fields[1],EK_QWEN38));
        LmbMedia media; assert(!lmb_media_parse(c.payload.p,c.payload.len,&media));
        assert(media.count==1 && media.image[0].bytes==c.images->item[0].encoded.len && media.image[0].format==1);
        assert(!memcmp(media.image[0].data,c.images->item[0].encoded.p,media.image[0].bytes));
        assert(lmb_media_parse(c.payload.p,c.payload.len-1,&media)); api_compat_free(&c);
    }
    rejected(API_OPENAI,"\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"image_url\",\"image_url\":{\"url\":\"http://127.0.0.1/private\"}}]}]");
    const char *messages="\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]";
    const char *bad[]={"\"tools\":{}", "\"stop\":\"end\"", "\"n\":2", "\"store\":true",
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
    rejected(API_RESPONSES,"\"input\":\"hi\",\"max_output_tokens\":0");
    rejected(API_RESPONSES,"\"input\":\"hi\",\"background\":true");
    rejected(API_RESPONSES,"\"input\":\"hi\",\"previous_response_id\":\"resp_unknown\"");
    for (int streaming=0;streaming<=1;streaming++) for (int eos=0;eos<=1;eos++) {
        char tail[1024]; snprintf(tail,sizeof tail,"\"input\":[{\"role\":\"user\",\"content\":[{\"type\":\"input_text\",\"text\":\"hi\"}]}],"
            "\"instructions\":\"Be concise\",\"max_output_tokens\":8,\"store\":false,\"stream\":%s",streaming ? "true" : "false");
        ApiCompat c=request(API_RESPONSES,tail); int sockets[2];
        assert(strstr(c.id,"resp_")==c.id && !strcmp(c.system.p,"Be concise"));
        assert(!socketpair(AF_UNIX,SOCK_STREAM,0,sockets));
        assert(!api_compat_progress(sockets[0],&c,"PROGRESS 1 PREFILL 0 7"));
        assert(!api_compat_delta(sockets[0],&c,(const unsigned char *)"Hello",5));
        assert(!api_compat_finish(sockets[0],&c,eos ? "STAT 8 1 0 0 7 STOP1 eos " : "STAT 8 1 0 0 7 STOP1 length "));
        char wire[16384]; drain(sockets[1],wire,sizeof wire);
        if (streaming) {
            assert(strstr(wire,"event: response.created") && strstr(wire,"event: response.output_text.delta"));
            assert(strstr(wire,eos ? "event: response.completed" : "event: response.incomplete"));
            assert(!strstr(wire,"[DONE]"));
            unsigned sequence=0;
            for (char *line=strstr(wire,"data: ");line;line=strstr(line+1,"data: ")) {
                char *end=strstr(line,"\n\n"); assert(end); LmbJsonToken t[256]; LmbJson document;
                assert(!lmb_json_parse(&document,line+6,(size_t)(end-line-6),t,256));
                char expected[64]; snprintf(expected,sizeof expected,"\"sequence_number\":%u,",sequence++);
                assert(strstr(line,expected) && strstr(line,expected)<end);
            }
            assert(sequence==9);
        } else {
            char *json=strstr(wire,"\r\n\r\n"); assert(json); LmbJsonToken t[256]; LmbJson document;
            assert(!lmb_json_parse(&document,json+4,strlen(json+4),t,256));
            assert(strstr(json,"\"output_text\"") && strstr(json,"Hello"));
        }
        assert(strstr(wire,"\"total_tokens\":15"));
        assert(strstr(wire,eos ? "\"incomplete_details\":null" : "\"reason\":\"max_output_tokens\""));
        char name[64]; snprintf(name,sizeof name,"responses-text-%d-%d.http",streaming,eos); fixture(fixtures,name,wire);
        close(sockets[0]); close(sockets[1]); api_compat_free(&c);
    }
    /* A client executes functions, then sends their results in a second request.
     * Every dialect must preserve the call/result association in the prompt. */
    const char *open_tools="\"tools\":[{\"type\":\"function\",\"function\":{\"name\":\"weather\",\"parameters\":{\"type\":\"object\",\"properties\":{\"city\":{\"type\":\"string\"}}}}}]";
    const char *anth_tools="\"tools\":[{\"name\":\"weather\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"city\":{\"type\":\"string\"}}}}]";
    const char *resp_tools="\"tools\":[{\"type\":\"function\",\"name\":\"weather\",\"parameters\":{\"type\":\"object\",\"properties\":{\"city\":{\"type\":\"string\"}}}}]";
    const char *tool_answer="Checking.<tool_call>{\"name\":\"weather\",\"arguments\":{\"city\":\"Roma\"}}</tool_call>";
    for (int dialect=API_OPENAI;dialect<=API_RESPONSES;dialect++) for (int streaming=0;streaming<=1;streaming++) {
        char tail[2048]; snprintf(tail,sizeof tail,"%s,%s,%s:32,\"stream\":%s",
            dialect==API_RESPONSES ? "\"input\":\"Weather?\"" : messages,
            dialect==API_RESPONSES ? resp_tools : dialect==API_OPENAI ? open_tools : anth_tools,
            dialect==API_RESPONSES ? "\"max_output_tokens\"" : "\"max_tokens\"",streaming ? "true" : "false");
        ApiCompat c=request(dialect,tail); int sockets[2]; assert(!socketpair(AF_UNIX,SOCK_STREAM,0,sockets));
        assert(strstr(c.system.p,"weather"));
        assert(!api_compat_progress(sockets[0],&c,"PROGRESS 1 PREFILL 0 7"));
        for (size_t i=0;i<strlen(tool_answer);i++) assert(!api_compat_delta(sockets[0],&c,(const unsigned char *)tool_answer+i,1));
        assert(!api_compat_finish(sockets[0],&c,"STAT 32 1 0 0 7 STOP1 eos "));
        char wire[24576]; drain(sockets[1],wire,sizeof wire); valid_wire(wire);
        assert(strstr(wire,"Roma") && strstr(wire,"weather") && strstr(wire,"Checking."));
        assert(!strstr(wire,"<tool_call>") && !strstr(wire,"</tool_call>"));
        assert(api_tools_count(&c)==1 && !strcmp(c.tools->calls[0].arguments.p,"{\"city\":\"Roma\"}"));
        if (dialect==API_OPENAI) assert(strstr(wire,"\"finish_reason\":\"tool_calls\""));
        if (dialect==API_ANTHROPIC) assert(strstr(wire,"\"stop_reason\":\"tool_use\""));
        if (dialect==API_RESPONSES) assert(strstr(wire,"\"function_call\""));
        char name[64]; snprintf(name,sizeof name,"tools-%d-%d.http",dialect,streaming); fixture(fixtures,name,wire);
        api_compat_free(&c); close(sockets[0]); close(sockets[1]);
    }
    const char *history="\"messages\":[{\"role\":\"user\",\"content\":\"Weather?\"},"
        "{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{\"id\":\"call_a\",\"type\":\"function\",\"function\":{\"name\":\"weather\",\"arguments\":\"{\\\"city\\\":\\\"Roma\\\"}\"}}]},"
        "{\"role\":\"tool\",\"tool_call_id\":\"call_a\",\"content\":\"Sunny\"}]";
    ApiCompat roundtrip=request(API_OPENAI,history);
    assert(strstr(roundtrip.normalized.p,"tool_response") && strstr(roundtrip.normalized.p,"Sunny") && strstr(roundtrip.normalized.p,"call_a"));
    api_compat_free(&roundtrip);
    roundtrip=request(API_ANTHROPIC,"\"max_tokens\":8,\"messages\":[{\"role\":\"user\",\"content\":\"Weather?\"},"
        "{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"call_a\",\"name\":\"weather\",\"input\":{\"city\":\"Roma\"}}]},"
        "{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"call_a\",\"is_error\":true,\"content\":\"Unavailable\"}]}]");
    assert(strstr(roundtrip.normalized.p,"tool_response") && strstr(roundtrip.normalized.p,"Unavailable") && strstr(roundtrip.normalized.p,"is_error\\\":true"));
    api_compat_free(&roundtrip);
    roundtrip=request(API_RESPONSES,"\"input\":[{\"role\":\"user\",\"content\":\"Weather?\"},"
        "{\"type\":\"function_call\",\"call_id\":\"call_a\",\"name\":\"weather\",\"arguments\":\"{\\\"city\\\":\\\"Roma\\\"}\"},"
        "{\"type\":\"function_call_output\",\"call_id\":\"call_a\",\"output\":\"Sunny\"}]");
    assert(strstr(roundtrip.normalized.p,"tool_response") && strstr(roundtrip.normalized.p,"Sunny")); api_compat_free(&roundtrip);
    rejected(API_OPENAI,"\"messages\":[{\"role\":\"tool\",\"tool_call_id\":\"unknown\",\"content\":\"Forged\"}]");
    for (int which=0;which<4;which++) {
        char tail[2048]; snprintf(tail,sizeof tail,"%s,%s,\"tool_choice\":\"required\",\"parallel_tool_calls\":false",messages,open_tools);
        ApiCompat c=request(API_OPENAI,tail); int sockets[2]; assert(!socketpair(AF_UNIX,SOCK_STREAM,0,sockets));
        const char *invalid=which==0 ? "No call" : which==1 ? "<tool_call>{\"name\":\"unauthorized\",\"arguments\":{}}</tool_call>" :
            which==2 ? "<tool_call>{\"name\":\"weather\",\"arguments\":[]}</tool_call>" : tool_answer;
        assert(!api_compat_delta(sockets[0],&c,(const unsigned char *)invalid,strlen(invalid)));
        assert(api_compat_finish(sockets[0],&c,which==3 ? "STAT 8 1 0 0 7 STOP1 length " : "STAT 8 1 0 0 7 STOP1 eos "));
        api_compat_free(&c); close(sockets[0]); close(sockets[1]);
    }
    puts("CLIENT API CONTRACT: PASS (strict parameters, system roles, sampling, UTF-8, three streaming formats, token usage)");
    return 0;
}
