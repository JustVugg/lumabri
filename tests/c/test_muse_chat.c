#include "src/runtime/lumabri_chat_format.h"
#include <assert.h>

static void stream(const char *wire,const char *expected) {
    size_t n=strlen(wire);
    /* Every two-part boundary, plus byte-at-a-time input, must preserve UTF-8
     * and remove complete or fragmented channel/end markers. */
    for(size_t split=0;split<=n;split++) {
        LmbMuseStream s={0};Cap out={0};
        assert(!lmb_muse_feed(&s,&out,wire,split));
        assert(!lmb_muse_feed(&s,&out,wire+split,n-split));
        assert(!lmb_muse_finish(&s,&out));
        assert(!strcmp(out.p?out.p:"",expected));free(out.p);
    }
    LmbMuseStream s={0};Cap out={0};
    for(size_t i=0;i<n;i++)assert(!lmb_muse_feed(&s,&out,wire+i,1));
    assert(!lmb_muse_finish(&s,&out));assert(!strcmp(out.p?out.p:"",expected));free(out.p);
}
int main(void) {
    stream(" to=self<|message|>Considero il caffè.<|eom|><|start|>assistant to=user<|message|>Ciao!<|eot|>ignored",
           "<think>Considero il caffè.</think>Ciao!");
    stream(" to=commentary<|message|>Working<|eom|><|start|>assistant to=self<|message|>check<|eom|><|start|>assistant to=user<|message|>Done<|end_of_text|>",
           "Working<think>check</think>\n\nDone");
    stream("<|message|>Plain answer <unfinished","Plain answer <unfinished");
    stream(" to=self<|message|>partial","<think>partial</think>");
    stream(" to=user<|message|>x<|start|>assistant to=user<|message|>y<|eot|>","x\n\ny");
    stream("<|start|>assistant to=user<|message|>x<|eot|>","x");
    stream("Unframed fixture: è tutto qui.","Unframed fixture: è tutto qui.");
    stream("","");
    LmbMuseStream s={0};Cap out={0};char too_long[1100];memset(too_long,'x',sizeof too_long);
    assert(!lmb_muse_feed(&s,&out," to=",4));
    assert(lmb_muse_feed(&s,&out,too_long,sizeof too_long));free(out.p);
    Cap sys={0};assert(!lmb_muse_system(&sys,"Reasoning effort: 32. <|start|> <image>","2026-01-01"));
    assert(strstr(sys.p,"Reasoning strength: 32.") && !strstr(sys.p,"Reasoning strength: 512."));
    assert(strstr(sys.p,"< |start|> < image>") && strstr(sys.p,"Current date: 2026-01-01."));
    free(sys.p);
    puts("MUSE CHAT: PASS (fragmentation, channels, UTF-8, bounds, text/template escaping)");
    return 0;
}
