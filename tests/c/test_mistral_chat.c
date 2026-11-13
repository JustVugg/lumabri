#include "src/runtime/lumabri_chat_format.h"
#include <assert.h>
static void check(const char *wire,const char *expected) {
    size_t n=strlen(wire);
    for(size_t split=0;split<=n;split++) {
        LmbMistralStream s={0};Cap out={0};
        assert(!lmb_mistral_feed(&s,&out,wire,split));
        assert(!lmb_mistral_feed(&s,&out,wire+split,n-split));
        assert(!lmb_mistral_finish(&s,&out));
        assert(!strcmp(out.p?out.p:"",expected));free(out.p);
    }
    LmbMistralStream s={0};Cap out={0};
    for(size_t i=0;i<n;i++)assert(!lmb_mistral_feed(&s,&out,wire+i,1));
    assert(!lmb_mistral_finish(&s,&out));assert(!strcmp(out.p?out.p:"",expected));free(out.p);
}
int main(void) {
    check("[THINK]caffè[/THINK] Answer","<think>caffè</think> Answer");
    check("[THINK][THINK]a[/THINK][/THINK]","<think><think>a</think></think>");
    check("[THINX][[THINK]x[/THIN","[THINX][<think>x[/THIN");
    check("","");check("[[[[[[[[[[", "[[[[[[[[[[");
    Cap out={0};assert(!serve2_prefix(&out,EK_MISTRAL));
    assert(!serve2_turn(&out,EK_MISTRAL,1,"Hello","<think>x</think>Y"));
    assert(!serve2_turn(&out,EK_MISTRAL,0,"Next?",NULL));
    assert(!strcmp(out.p,"<s>[MODEL_SETTINGS]{\"reasoning_effort\": \"high\"}[/MODEL_SETTINGS]"
        "[INST]Hello[/INST][THINK]x[/THINK]Y</s>[INST]Next?[/INST]"));
    free(out.p);puts("MISTRAL CHAT: PASS (stream boundaries, UTF-8, history, native template)");return 0;
}
