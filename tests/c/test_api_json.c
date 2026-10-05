#include "src/runtime/lumabri_api_json.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    LmbJson j; LmbJsonToken tokens[256];
    const char *valid=" {\"model\":\"caf\\u00e8\\ud83c\\udf0d\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\\nworld\"}],\"max_tokens\":8} ";
    assert(!lmb_json_parse(&j,valid,strlen(valid),tokens,256));
    const char *const schema[]={"model","messages","max_tokens"}; int f[3];
    assert(!lmb_json_fields(&j,0,schema,3,f));
    char value[64]; assert(!lmb_api_json_string(&j,(unsigned)f[0],value,sizeof value));
    assert(!strcmp(value,"caf\xc3\xa8\xf0\x9f\x8c\x8d"));
    uint32_t n; assert(!lmb_json_uint(&j,(unsigned)f[2],&n) && n==8);
    assert(lmb_api_json_string(&j,(unsigned)f[0],value,3));
    const char *bad[]={"", "[]x", "{}{}", "[1,]", "{\"x\":}", "{1:2}", "[+1]", "[01]", "[1.]", "[1e]", "[1e+]", "[-]",
        "[NaN]", "[true false]", "[nul]", "[\"\\u0000\"]", "[\"\\ud800\"]", "[\"\\udc00\"]", "[\"\\ud800\\u0041\"]",
        "[\"\\x00\"]", "[\"raw\nline\"]", "[\"\xc0\xaf\"]", "[\"\xed\xa0\x80\"]", "[\"\xf4\x90\x80\x80\"]", "[\"\x80\"]", "[\"\xe2\x82\"]"};
    for (size_t i=0; i<sizeof bad/sizeof *bad; i++) assert(lmb_json_parse(&j,bad[i],strlen(bad[i]),tokens,256));
    const char *numbers[]={"0","4294967295","4294967296","-1","1.0","1e1"};
    for (size_t i=0; i<sizeof numbers/sizeof *numbers; i++) {
        assert(!lmb_json_parse(&j,numbers[i],strlen(numbers[i]),tokens,256));
        assert((lmb_json_uint(&j,0,&n)==0)==(i<2));
    }
    const char *objects[]={"{\"model\":1,\"model\":2}","{\"model\":1,\"mo\\u0064el\":2}","{\"host\":\"unapproved\"}"};
    for (size_t i=0; i<sizeof objects/sizeof *objects; i++) {
        assert(!lmb_json_parse(&j,objects[i],strlen(objects[i]),tokens,256));
        assert(lmb_json_fields(&j,0,schema,3,f));
    }
    assert(lmb_json_parse(&j,valid,strlen(valid),tokens,2));
    char deep[65]; memset(deep,'[',32); memset(deep+32,']',32); deep[64]=0;
    assert(lmb_json_parse(&j,deep,64,tokens,256));
    const char nul[]={'"','x',0,'"'}; assert(lmb_json_parse(&j,nul,sizeof nul,tokens,256));
    for (size_t i=0; i<strlen(valid)-2; i++) assert(lmb_json_parse(&j,valid,i,tokens,256));
    puts("API JSON: PASS (bounded syntax, strict schema keys, Unicode, integer overflow, depth and token limits)");
    return 0;
}
