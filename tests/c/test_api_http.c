#include "src/runtime/lumabri_api_http.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    LmbApiHttp r;
    const char *request="POST /api/v1/chat HTTP/1.1\r\nHost: 127.0.0.1:47380\r\nAuthorization: Bearer secret\r\nContent-Length: 123\r\nContent-Type: application/json\r\nOrigin: http://127.0.0.1:47380\r\n\r\n";
    assert(!lmb_api_http_parse(&r,request,strlen(request)));
    assert(r.has_length && r.length==123 && r.json && !lmb_api_http_origin(&r,47380));
    assert(!strcmp(r.authorization,"Bearer secret"));
    strcpy(r.origin,"http://evil.test"); assert(lmb_api_http_origin(&r,47380));
    r.has_origin=0; strcpy(r.host,"evil.test:47380"); assert(lmb_api_http_origin(&r,47380));
    strcpy(r.host,"localhost:47380"); assert(!lmb_api_http_origin(&r,47380));
    for (size_t n=0; n<strlen(request); n++) assert(lmb_api_http_parse(&r,request,n));
    const char *headers[]={"Content-Length: 1\r\nContent-Length: 1\r\n","Content-Length: 1048577\r\n","Content-Length: -1\r\n",
        "Content-Length: 1,1\r\n","Transfer-Encoding: chunked\r\n","Expect: 100-continue\r\n","Upgrade: websocket\r\n",
        "HOST: localhost:1\r\n","Authorization: a\r\nauthorization: b\r\n"," Content-Length: 1\r\n",
        "Bad Header: x\r\n","Origin: a\r\nOrigin: a\r\n","Content-Type: a\r\nContent-Type: a\r\n"};
    char wire[16384];
    for (size_t i=0; i<sizeof headers/sizeof *headers; i++) {
        int n=snprintf(wire,sizeof wire,"POST /api/v1/chat HTTP/1.1\r\nHost: localhost:1\r\n%s\r\n",headers[i]);
        assert(n>0 && lmb_api_http_parse(&r,wire,(size_t)n));
    }
    const char *bad[]={"GET / HTTP/1.0\r\nHost: x\r\n\r\n","GET / HTTP/1.1\nHost: x\n\n","GET / HTTP/1.1\r\n\r\n",
        "GET http://example.com/ HTTP/1.1\r\nHost: x\r\n\r\n","GET /a%2fb HTTP/1.1\r\nHost: x\r\n\r\n"};
    for (size_t i=0; i<sizeof bad/sizeof *bad; i++) assert(lmb_api_http_parse(&r,bad[i],strlen(bad[i])));
    memset(wire,'x',sizeof wire); assert(lmb_api_http_parse(&r,wire,sizeof wire));
    memcpy(wire,request,strlen(request)); wire[10]=0; assert(lmb_api_http_parse(&r,wire,strlen(request)));
    puts("API HTTP: PASS (framing, bounds, duplicate headers, same-origin/Host policy)");
    return 0;
}
