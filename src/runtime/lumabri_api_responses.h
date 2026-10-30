/* Responses wire adapter over the same admitted chat and replay path.
 * Output item IDs belong to this response, never to engine sessions. */
#ifndef LUMABRI_API_RESPONSES_H
#define LUMABRI_API_RESPONSES_H

static int api_responses_content(const LmbJson *j,unsigned index,const char *role,Cap *text) {
    if (j->tokens[index].kind==LMB_JSON_STRING) return api_compat_text(j,index,text);
    const LmbJsonToken *t=&j->tokens[index];
    if (t->kind!=LMB_JSON_ARRAY || !t->children || t->children>128) return -1;
    const char *const keys[]={"type","text","annotations","logprobs"};
    for (unsigned p=index+1;p<t->next;p=j->tokens[p].next) {
        int f[4]; char type[32];
        if (lmb_json_fields(j,p,keys,4,f) || f[0]<0 || f[1]<0 ||
            lmb_api_json_string(j,(unsigned)f[0],type,sizeof type) ||
            (strcmp(type,"input_text") && (strcmp(role,"assistant") || strcmp(type,"output_text"))) ||
            (f[2]>=0 && (j->tokens[f[2]].kind!=LMB_JSON_ARRAY || j->tokens[f[2]].children)) ||
            (f[3]>=0 && (j->tokens[f[3]].kind!=LMB_JSON_ARRAY || j->tokens[f[3]].children)) ||
            api_compat_text(j,(unsigned)f[1],text)) return -1;
    }
    return 0;
}

static int api_responses_request_inner(ApiCompat *c,const char *body,size_t length) {
    LmbJsonToken tokens[4096]; LmbJson j; int f[14];
    const char *const keys[]={"model","input","instructions","max_output_tokens","stream",
        "temperature","top_p","store","background","include","text","tools","tool_choice","parallel_tool_calls"};
    if (lmb_json_parse(&j,body,length,tokens,4096) || lmb_json_fields(&j,0,keys,14,f) ||
        f[0]<0 || f[1]<0 || (f[7]>=0 && tokens[f[7]].kind!=LMB_JSON_FALSE) ||
        (f[8]>=0 && tokens[f[8]].kind!=LMB_JSON_FALSE) ||
        (f[9]>=0 && (tokens[f[9]].kind!=LMB_JSON_ARRAY || tokens[f[9]].children))) return -1;
    if (f[10]>=0) {
        const char *const text_keys[]={"format"},*const format_keys[]={"type"};
        int text[1],format[1]; char type[16];
        if (lmb_json_fields(&j,(unsigned)f[10],text_keys,1,text) || text[0]<0 ||
            lmb_json_fields(&j,(unsigned)text[0],format_keys,1,format) || format[0]<0 ||
            lmb_api_json_string(&j,(unsigned)format[0],type,sizeof type) || strcmp(type,"text")) return -1;
    }
    Cap converted={0},part={0}; int bad=1; unsigned count=0;
    if (cap_str(&converted,"{\"model\":") || api_json_token(&converted,&j,(unsigned)f[0])) goto done;
    const char *const mapped[]={"max_tokens","stream","temperature","top_p"};
    for (unsigned i=0;i<4;i++) if (f[3+i]>=0 &&
        (api_addf(&converted,",\"%s\":",mapped[i]) || api_json_token(&converted,&j,(unsigned)f[3+i]))) goto done;
    if (f[13]>=0 && (cap_str(&converted,",\"parallel_tool_calls\":") || api_json_token(&converted,&j,(unsigned)f[13]))) goto done;
    if (f[11]>=0) {
        if (tokens[f[11]].kind!=LMB_JSON_ARRAY || cap_str(&converted,",\"tools\":[")) goto done;
        const char *const tk[]={"type","name","description","parameters","strict"}; unsigned n=0;
        for (unsigned p=(unsigned)f[11]+1;p<tokens[f[11]].next;p=tokens[p].next) {
            int g[5]; char type[24];
            if (lmb_json_fields(&j,p,tk,5,g) || g[0]<0 || g[1]<0 ||
                lmb_api_json_string(&j,(unsigned)g[0],type,sizeof type) || strcmp(type,"function") ||
                (n++ && cap_str(&converted,",")) || cap_str(&converted,"{\"type\":\"function\",\"function\":{")) goto done;
            unsigned fields=0;
            for (unsigned z=1;z<5;z++) if (g[z]>=0 && ((fields++ && cap_str(&converted,",")) ||
                api_json_text(&converted,tk[z]) || cap_str(&converted,":") || api_json_token(&converted,&j,(unsigned)g[z]))) goto done;
            if (cap_str(&converted,"}}")) goto done;
        }
        if (cap_str(&converted,"]")) goto done;
    }
    if (f[12]>=0) {
        if (cap_str(&converted,",\"tool_choice\":")) goto done;
        if (tokens[f[12]].kind==LMB_JSON_STRING) { if (api_json_token(&converted,&j,(unsigned)f[12])) goto done; }
        else {
            const char *const ck[]={"type","name"}; int g[2]; char type[24];
            if (lmb_json_fields(&j,(unsigned)f[12],ck,2,g) || g[0]<0 || g[1]<0 ||
                lmb_api_json_string(&j,(unsigned)g[0],type,sizeof type) || strcmp(type,"function") ||
                cap_str(&converted,"{\"type\":\"function\",\"function\":{\"name\":") ||
                api_json_token(&converted,&j,(unsigned)g[1]) || cap_str(&converted,"}}")) goto done;
        }
    }
    if (cap_str(&converted,",\"messages\":[")) goto done;
    if (f[2]>=0 && tokens[f[2]].kind!=LMB_JSON_NULL) {
        if (api_compat_text(&j,(unsigned)f[2],&part) || api_compat_turn(&converted,"system",&part,count++)) goto done;
        free(part.p); part=(Cap){0};
    }
    if (tokens[f[1]].kind==LMB_JSON_STRING) {
        if (api_compat_text(&j,(unsigned)f[1],&part) || api_compat_turn(&converted,"user",&part,count++)) goto done;
    } else {
        const LmbJsonToken *input=&tokens[f[1]];
        if (input->kind!=LMB_JSON_ARRAY || !input->children || input->children>130) goto done;
        const char *const item_keys[]={"type","role","content","id","status","phase","call_id","name","arguments","output"};
        unsigned calls=0;
        for (unsigned p=(unsigned)f[1]+1;p<input->next;p=tokens[p].next) {
            int m[10]; char role[16],value[128],type[32]="message";
            if (lmb_json_fields(&j,p,item_keys,10,m) ||
                (m[0]>=0 && lmb_api_json_string(&j,(unsigned)m[0],type,sizeof type))) goto done;
            if (!strcmp(type,"function_call")) {
                if (m[1]>=0 || m[2]>=0 || m[5]>=0 || m[6]<0 || m[7]<0 || m[8]<0 || m[9]>=0) goto done;
                if (!calls && ((count++ && cap_str(&converted,",")) || cap_str(&converted,"{\"role\":\"assistant\",\"content\":null,\"tool_calls\":["))) goto done;
                if ((calls++ && cap_str(&converted,",")) || cap_str(&converted,"{\"type\":\"function\",\"id\":") ||
                    api_json_token(&converted,&j,(unsigned)m[6]) || cap_str(&converted,",\"function\":{\"name\":") ||
                    api_json_token(&converted,&j,(unsigned)m[7]) || cap_str(&converted,",\"arguments\":") ||
                    api_json_token(&converted,&j,(unsigned)m[8]) || cap_str(&converted,"}}")) goto done;
                continue;
            }
            if (calls) { if (cap_str(&converted,"]}")) goto done; calls=0; }
            if (!strcmp(type,"function_call_output")) {
                if (m[1]>=0 || m[2]>=0 || m[4]>=0 || m[5]>=0 || m[6]<0 || m[7]>=0 || m[8]>=0 || m[9]<0 ||
                    (count++ && cap_str(&converted,",")) || cap_str(&converted,"{\"role\":\"tool\",\"tool_call_id\":") ||
                    api_json_token(&converted,&j,(unsigned)m[6]) || cap_str(&converted,",\"content\":") ||
                    api_responses_content(&j,(unsigned)m[9],"user",&part) || api_json_text(&converted,part.p ? part.p : "") ||
                    cap_str(&converted,"}")) goto done;
                api_tool_cap_free(&part); continue;
            }
            if (strcmp(type,"message") || m[1]<0 || m[2]<0 || m[6]>=0 || m[7]>=0 || m[8]>=0 || m[9]>=0 ||
                lmb_api_json_string(&j,(unsigned)m[1],role,sizeof role) ||
                (m[3]>=0 && lmb_api_json_string(&j,(unsigned)m[3],value,sizeof value))) goto done;
            if (m[4]>=0 && (lmb_api_json_string(&j,(unsigned)m[4],value,sizeof value) ||
                (strcmp(value,"completed") && strcmp(value,"incomplete")))) goto done;
            if (m[5]>=0 && (strcmp(role,"assistant") || lmb_api_json_string(&j,(unsigned)m[5],value,sizeof value) ||
                (strcmp(value,"commentary") && strcmp(value,"final_answer")))) goto done;
            if (api_responses_content(&j,(unsigned)m[2],role,&part) || api_compat_turn(&converted,role,&part,count++)) goto done;
            free(part.p); part=(Cap){0};
        }
        if (calls && cap_str(&converted,"]}")) goto done;
    }
    if (cap_str(&converted,"]}")) goto done;
    c->dialect=API_OPENAI;
    bad=api_compat_request(c,converted.p,converted.len);
    c->dialect=API_RESPONSES;
    if (!bad) {
        uint8_t random[16]; char hex[33]; lmb_random(random,sizeof random); lmb_hex(hex,random,sizeof random);
        snprintf(c->id,sizeof c->id,"resp_%s",hex); snprintf(c->item_id,sizeof c->item_id,"msg_%s",hex);
        bad=(f[2]>=0 ? api_json_token(&c->response_instructions,&j,(unsigned)f[2]) : cap_str(&c->response_instructions,"null")) ||
            (f[11]>=0 ? api_json_token(&c->response_tools,&j,(unsigned)f[11]) : cap_str(&c->response_tools,"[]")) ||
            (f[12]>=0 ? api_json_token(&c->response_choice,&j,(unsigned)f[12]) : cap_str(&c->response_choice,"\"auto\""));
    }
done:
    if (converted.p) { lmb_api_wipe(converted.p,converted.len); free(converted.p); }
    if (part.p) { lmb_api_wipe(part.p,part.len); free(part.p); }
    return bad ? -1 : 0;
}

static int api_responses_part(Cap *p,const char *text) {
    return cap_str(p,"{\"type\":\"output_text\",\"annotations\":[],\"logprobs\":[],\"text\":") ||
        api_json_text(p,text) || cap_str(p,"}") ? -1 : 0;
}
static int api_responses_item(Cap *p,ApiCompat *c,const char *status,int content) {
    return api_addf(p,"{\"id\":\"%s\",\"type\":\"message\",\"role\":\"assistant\",\"status\":\"%s\",\"content\":[",c->item_id,status) ||
        (content && api_responses_part(p,c->output.p ? c->output.p : "")) || cap_str(p,"]}") ? -1 : 0;
}
static int api_responses_object(Cap *p,ApiCompat *c,const char *status,unsigned input,unsigned output,const char *error) {
    int pending=!strcmp(status,"in_progress"),incomplete=!strcmp(status,"incomplete");
    if (api_addf(p,"{\"id\":\"%s\",\"object\":\"response\",\"created_at\":%lld,\"model\":\"%s\",\"status\":\"%s\","
        "\"background\":false,\"store\":false,\"error\":",c->id,c->created,c->model,status)) return -1;
    if (error ? (cap_str(p,"{\"code\":\"server_error\",\"message\":") || api_json_text(p,error) || cap_str(p,"}")) : cap_str(p,"null")) return -1;
    if (cap_str(p,incomplete ? ",\"incomplete_details\":{\"reason\":\"max_output_tokens\"}" : ",\"incomplete_details\":null") ||
        cap_str(p,",\"instructions\":") || cap_str(p,c->response_instructions.p ? c->response_instructions.p : "null") ||
        api_addf(p,",\"metadata\":{},\"parallel_tool_calls\":%s,\"tool_choice\":",c->tools && !c->tools->parallel ? "false" : "true") ||
        cap_str(p,c->response_choice.p ? c->response_choice.p : "\"auto\"") || cap_str(p,",\"tools\":") ||
        cap_str(p,c->response_tools.p ? c->response_tools.p : "[]") ||
        api_addf(p,",\"max_output_tokens\":%u,\"temperature\":%.17g,\"top_p\":%.17g,\"previous_response_id\":null,"
            "\"text\":{\"format\":{\"type\":\"text\"}},\"truncation\":\"disabled\",\"output\":[",
            c->maximum,c->has_temperature ? c->temperature : .7,c->has_top_p ? c->top_p : .95) ||
        (!pending && api_responses_item(p,c,incomplete || error ? "incomplete" : "completed",1)) ||
        (!pending && !error && api_tools_count(c) && (cap_str(p,",") || api_tools_wire(p,c,API_RESPONSES))) || cap_str(p,"],\"usage\":")) return -1;
    if (pending || error) { if (cap_str(p,"null")) return -1; }
    else if (api_addf(p,"{\"input_tokens\":%u,\"input_tokens_details\":{\"cached_tokens\":0},\"output_tokens\":%u,"
        "\"output_tokens_details\":{\"reasoning_tokens\":0},\"total_tokens\":%u}",input,output,input+output)) return -1;
    return cap_str(p,"}");
}
static int api_responses_event(int fd,ApiCompat *c,const char *type,Cap *payload) {
    Cap frame={0};
    int bad=api_addf(&frame,"{\"type\":\"%s\",\"sequence_number\":%u,",type,c->sequence++) ||
        cap_str(&frame,payload->p) || cap_str(&frame,"}") || api_event(fd,type,frame.p);
    free(frame.p); return bad ? -1 : 0;
}
static int api_responses_lifecycle(int fd,ApiCompat *c,const char *event,const char *status,unsigned input,unsigned output,const char *error) {
    Cap payload={0}; int bad=cap_str(&payload,"\"response\":") || api_responses_object(&payload,c,status,input,output,error) ||
        api_responses_event(fd,c,event,&payload);
    free(payload.p); return bad ? -1 : 0;
}
static int api_responses_begin(int fd,ApiCompat *c) {
    if (api_responses_lifecycle(fd,c,"response.created","in_progress",0,0,NULL) ||
        api_responses_lifecycle(fd,c,"response.in_progress","in_progress",0,0,NULL)) return -1;
    Cap p={0};
    int bad=cap_str(&p,"\"output_index\":0,\"item\":") || api_responses_item(&p,c,"in_progress",0) ||
        api_responses_event(fd,c,"response.output_item.added",&p);
    free(p.p); p=(Cap){0};
    if (!bad) bad=api_addf(&p,"\"item_id\":\"%s\",\"output_index\":0,\"content_index\":0,\"part\":",c->item_id) ||
        api_responses_part(&p,"") || api_responses_event(fd,c,"response.content_part.added",&p);
    free(p.p); if (!bad) c->begun=1; return bad ? -1 : 0;
}
static int api_responses_delta(int fd,ApiCompat *c,const char *text) {
    if (c->streaming && api_compat_begin(fd,c)) return -1;
    if (cap_str(&c->output,text)) return -1;
    if (!c->streaming) return 0;
    Cap p={0}; int bad=api_addf(&p,"\"item_id\":\"%s\",\"output_index\":0,\"content_index\":0,\"logprobs\":[],\"delta\":",c->item_id) ||
        api_json_text(&p,text) || api_responses_event(fd,c,"response.output_text.delta",&p);
    free(p.p); return bad ? -1 : 0;
}
static int api_responses_tools(int fd,ApiCompat *c) {
    for (unsigned i=0;i<api_tools_count(c);i++) {
        ApiToolCall *call=&c->tools->calls[i]; Cap p={0};
        int bad=api_addf(&p,"\"output_index\":%u,\"item\":{\"type\":\"function_call\",\"id\":\"fc_%s\",\"call_id\":\"%s\",\"name\":\"%s\",\"arguments\":\"\",\"status\":\"in_progress\"}",i+1,call->id,call->id,call->name) ||
            api_responses_event(fd,c,"response.output_item.added",&p);
        api_tool_cap_free(&p);
        if (!bad) bad=api_addf(&p,"\"output_index\":%u,\"item_id\":\"fc_%s\",\"delta\":",i+1,call->id) ||
            api_json_text(&p,call->arguments.p) || api_responses_event(fd,c,"response.function_call_arguments.delta",&p);
        api_tool_cap_free(&p);
        if (!bad) bad=api_addf(&p,"\"output_index\":%u,\"item_id\":\"fc_%s\",\"name\":\"%s\",\"arguments\":",i+1,call->id,call->name) ||
            api_json_text(&p,call->arguments.p) || api_responses_event(fd,c,"response.function_call_arguments.done",&p);
        api_tool_cap_free(&p);
        if (!bad) bad=api_addf(&p,"\"output_index\":%u,\"item\":{\"type\":\"function_call\",\"id\":\"fc_%s\",\"call_id\":\"%s\",\"name\":\"%s\",\"status\":\"completed\",\"arguments\":",i+1,call->id,call->id,call->name) ||
            api_json_text(&p,call->arguments.p) || cap_str(&p,"}") || api_responses_event(fd,c,"response.output_item.done",&p);
        api_tool_cap_free(&p); if (bad) return -1;
    }
    return 0;
}
static int api_responses_finish(int fd,ApiCompat *c,int eos,unsigned input,unsigned output) {
    Cap p={0}; int bad;
    const char *status=eos ? "completed" : "incomplete";
    if (!c->streaming) {
        bad=api_responses_object(&p,c,status,input,output,NULL) || api_response(fd,200,p.p);
        free(p.p); return bad ? -1 : 0;
    }
    if (api_compat_begin(fd,c)) return -1;
    bad=api_addf(&p,"\"item_id\":\"%s\",\"output_index\":0,\"content_index\":0,\"logprobs\":[],\"text\":",c->item_id) ||
        api_json_text(&p,c->output.p ? c->output.p : "") || api_responses_event(fd,c,"response.output_text.done",&p);
    free(p.p); p=(Cap){0};
    if (!bad) bad=api_addf(&p,"\"item_id\":\"%s\",\"output_index\":0,\"content_index\":0,\"part\":",c->item_id) ||
        api_responses_part(&p,c->output.p ? c->output.p : "") || api_responses_event(fd,c,"response.content_part.done",&p);
    free(p.p); p=(Cap){0};
    if (!bad) bad=cap_str(&p,"\"output_index\":0,\"item\":") || api_responses_item(&p,c,status,1) ||
        api_responses_event(fd,c,"response.output_item.done",&p);
    free(p.p);
    return bad || api_responses_tools(fd,c) || api_responses_lifecycle(fd,c,eos ? "response.completed" : "response.incomplete",status,input,output,NULL) ? -1 : 0;
}
static void api_responses_failure(int fd,ApiCompat *c,const char *message) {
    if (!c->streaming) { api_error(fd,502,"generation_incomplete"); return; }
    (void)api_responses_lifecycle(fd,c,"response.failed","failed",0,0,message);
}
static int api_responses_request(ApiCompat *c,const char *body,size_t length) {
    Cap converted={0};
    int bad=api_images_request(c,body,length,&converted) || api_responses_request_inner(c,converted.p,converted.len);
    api_tool_cap_free(&converted); return bad ? -1 : 0;
}
#endif
