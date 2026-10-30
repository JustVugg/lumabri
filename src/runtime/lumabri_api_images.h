#ifndef LUMABRI_API_IMAGES_H
#define LUMABRI_API_IMAGES_H
#include "lumabri_media.h"
#include "lumabri_image.h"
struct ApiImages {
    unsigned count;
    struct { char marker[LMB_MEDIA_MARKER]; unsigned format; Cap encoded; } item[LMB_MEDIA_IMAGES];
};
static int api_image_field(const LmbJson *j,unsigned index,const char *name) {
    if (index>=j->count || j->tokens[index].kind!=LMB_JSON_OBJECT) return -1;
    int result=-1;
    for (unsigned i=index+1;i<j->tokens[index].next;i=j->tokens[i+1].next) {
        char key[128]; if (lmb_api_json_string(j,i,key,sizeof key)) return -1;
        if (!strcmp(key,name)) { if(result>=0) return -1; result=(int)i+1; }
    }
    return result;
}
static void api_images_free(ApiImages *images) {
    if (!images) return;
    for (unsigned i=0;i<images->count;i++) {
        lmb_api_wipe(images->item[i].encoded.p,images->item[i].encoded.len); free(images->item[i].encoded.p);
    }
    free(images);
}
static int api_image_base64(const char *s,Cap *out) {
    size_t n=strlen(s);
    if (!n || n%4 || n/4*3>LMB_IMAGE_ENCODED_MAX+2u) return -1;
    for (size_t i=0;i<n;i+=4) {
        unsigned v[4]={0},pad=0;
        for (unsigned k=0;k<4;k++) {
            unsigned char c=(unsigned char)s[i+k];
            if (c=='=' && k>=2 && i+4==n) { pad++; continue; }
            if (pad) return -1;
            if (c>='A' && c<='Z') v[k]=c-'A';
            else if (c>='a' && c<='z') v[k]=c-'a'+26;
            else if (c>='0' && c<='9') v[k]=c-'0'+52;
            else if (c=='+') v[k]=62; else if(c=='/') v[k]=63; else return -1;
        }
        if ((pad==1 && (v[2]&3)) || (pad==2 && (v[1]&15))) return -1;
        char decoded[3]={(char)(v[0]<<2 | v[1]>>4),(char)(v[1]<<4 | v[2]>>2),(char)(v[2]<<6 | v[3])};
        if (cap_add(out,decoded,3-pad)) return -1;
    }
    return out->len && out->len<=LMB_IMAGE_ENCODED_MAX ? 0 : -1;
}
/* Parse only inline image data. A model never fetches an arbitrary URL or
 * opens a client-supplied filesystem path under the server's credentials. */
static int api_image_block(ApiCompat *c,const LmbJson *j,unsigned p,const char *role,Cap *out) {
    int type_index=api_image_field(j,p,"type"); char type[32];
    if (type_index<0 || lmb_api_json_string(j,(unsigned)type_index,type,sizeof type)) return api_json_token(out,j,p);
    if (strcmp(type,"image_url") && strcmp(type,"image") && strcmp(type,"input_image")) return api_json_token(out,j,p);
    if (strcmp(role,"user")) return -1;
    int source=-1,format=0; char mime[32],detail[16]; Cap data={0}; int bad=1;
    if (c->dialect==API_ANTHROPIC && !strcmp(type,"image")) {
        const char *const keys[]={"type","source"},*const sk[]={"type","media_type","data"}; int f[2],s[3];
        if (lmb_json_fields(j,p,keys,2,f) || f[1]<0 || lmb_json_fields(j,(unsigned)f[1],sk,3,s) ||
            s[0]<0 || s[1]<0 || s[2]<0 || lmb_api_json_string(j,(unsigned)s[0],detail,sizeof detail) ||
            strcmp(detail,"base64") || lmb_api_json_string(j,(unsigned)s[1],mime,sizeof mime)) goto done;
        source=s[2]; format=!strcmp(mime,"image/png") ? 1 : !strcmp(mime,"image/jpeg") ? 2 : 0;
    } else if (c->dialect==API_OPENAI && !strcmp(type,"image_url")) {
        const char *const keys[]={"type","image_url"},*const sk[]={"url","detail"}; int f[2],s[2];
        if (lmb_json_fields(j,p,keys,2,f) || f[1]<0 || lmb_json_fields(j,(unsigned)f[1],sk,2,s) || s[0]<0 ||
            (s[1]>=0 && (lmb_api_json_string(j,(unsigned)s[1],detail,sizeof detail) || strcmp(detail,"auto")))) goto done;
        source=s[0];
    } else if (c->dialect==API_RESPONSES && !strcmp(type,"input_image")) {
        const char *const keys[]={"type","image_url","detail"}; int f[3];
        if (lmb_json_fields(j,p,keys,3,f) || f[1]<0 ||
            (f[2]>=0 && (lmb_api_json_string(j,(unsigned)f[2],detail,sizeof detail) || strcmp(detail,"auto")))) goto done;
        source=f[1];
    } else goto done;
    if (source<0 || j->tokens[source].kind!=LMB_JSON_STRING) goto done;
    size_t size=j->tokens[source].end-j->tokens[source].begin+1;
    data.p=malloc(size); if (!data.p || lmb_api_json_string(j,(unsigned)source,data.p,size)) goto done;
    data.len=strlen(data.p); const char *base=data.p;
    if (c->dialect!=API_ANTHROPIC) {
        if (!strncmp(base,"data:image/png;base64,",22)) { base+=22; format=1; }
        else if (!strncmp(base,"data:image/jpeg;base64,",23)) { base+=23; format=2; }
        else goto done;
    }
    if (!format) goto done;
    if (!c->images) c->images=calloc(1,sizeof *c->images);
    if (!c->images || c->images->count>=LMB_MEDIA_IMAGES) goto done;
    unsigned i=c->images->count++; Cap *bytes=&c->images->item[i].encoded;
    if (api_image_base64(base,bytes)) goto done;
    if ((format==1 && (bytes->len<8 || memcmp(bytes->p,"\211PNG\r\n\032\n",8))) ||
        (format==2 && (bytes->len<3 || memcmp(bytes->p,"\377\330\377",3)))) goto done;
    c->images->item[i].format=(unsigned)format;
    uint8_t random[16]; char hex[33]; lmb_random(random,sizeof random); lmb_hex(hex,random,sizeof random);
    snprintf(c->images->item[i].marker,LMB_MEDIA_MARKER,"[lmb-image-%s]",hex);
    bad=cap_str(out,c->dialect==API_RESPONSES ? "{\"type\":\"input_text\",\"text\":" : "{\"type\":\"text\",\"text\":") ||
        api_json_text(out,c->images->item[i].marker) || cap_str(out,"}");
done:
    if (data.p) { lmb_api_wipe(data.p,data.len); free(data.p); }
    return bad ? -1 : 0;
}
static int api_images_request(ApiCompat *c,const char *body,size_t length,Cap *converted) {
    LmbJsonToken tokens[4096]; LmbJson j;
    if (lmb_json_parse(&j,body,length,tokens,4096) || tokens[0].kind!=LMB_JSON_OBJECT || cap_str(converted,"{")) return -1;
    for (unsigned p=1;p<tokens[0].next;p=tokens[p+1].next) {
        char name[128]; if (lmb_api_json_string(&j,p,name,sizeof name) ||
            (p>1 && cap_str(converted,",")) || api_json_token(converted,&j,p) || cap_str(converted,":")) return -1;
        const char *field=c->dialect==API_RESPONSES ? "input" : "messages";
        if (strcmp(name,field) || tokens[p+1].kind!=LMB_JSON_ARRAY) { if (api_json_token(converted,&j,p+1)) return -1; continue; }
        if (cap_str(converted,"[")) return -1;
        for (unsigned turn=p+2;turn<tokens[p+1].next;turn=tokens[turn].next) {
            if (turn>p+2 && cap_str(converted,",")) return -1;
            if (tokens[turn].kind!=LMB_JSON_OBJECT) return -1;
            int ri=api_image_field(&j,turn,"role"); char role[20]="";
            if (ri>=0 && lmb_api_json_string(&j,(unsigned)ri,role,sizeof role)) return -1;
            if (cap_str(converted,"{")) return -1;
            for (unsigned k=turn+1;k<tokens[turn].next;k=tokens[k+1].next) {
                char key[64]; if (lmb_api_json_string(&j,k,key,sizeof key) ||
                    (k>turn+1 && cap_str(converted,",")) || api_json_token(converted,&j,k) || cap_str(converted,":")) return -1;
                if (strcmp(key,"content") || tokens[k+1].kind!=LMB_JSON_ARRAY) {
                    if (api_json_token(converted,&j,k+1)) return -1;
                    continue;
                }
                if (cap_str(converted,"[")) return -1;
                for (unsigned b=k+2;b<tokens[k+1].next;b=tokens[b].next)
                    if ((b>k+2 && cap_str(converted,",")) || api_image_block(c,&j,b,role,converted)) return -1;
                if (cap_str(converted,"]")) return -1;
            }
            if (cap_str(converted,"}")) return -1;
        }
        if (cap_str(converted,"]")) return -1;
    }
    return cap_str(converted,"}");
}
static int api_images_payload(ApiCompat *c) {
    if (!c->images || !c->images->count) return 0;
    Cap frame={0}; unsigned char header[16]; memcpy(header,"LMBIMG01",8);
    if (c->payload.len>LMB_MEDIA_TEXT_MAX) return -1;
    lmb_media_put(header+8,(uint32_t)c->payload.len); lmb_media_put(header+12,c->images->count);
    if (cap_add(&frame,(char *)header,16) || cap_add(&frame,c->payload.p,c->payload.len)) goto fail;
    for (unsigned i=0;i<c->images->count;i++) {
        const char *marker=c->images->item[i].marker,*at=strstr(c->payload.p,marker);
        if (!at || strstr(at+strlen(marker),marker)) goto fail;
        lmb_media_put(header,(uint32_t)(at-c->payload.p)); lmb_media_put(header+4,(uint32_t)strlen(marker));
        lmb_media_put(header+8,c->images->item[i].format); lmb_media_put(header+12,(uint32_t)c->images->item[i].encoded.len);
        if (cap_add(&frame,(char *)header,16) || cap_add(&frame,c->images->item[i].encoded.p,c->images->item[i].encoded.len)) goto fail;
    }
    LmbMedia check; if (lmb_media_parse(frame.p,frame.len,&check)) goto fail;
    lmb_api_wipe(c->payload.p,c->payload.len); free(c->payload.p); c->payload=frame; return 0;
fail:
    lmb_api_wipe(frame.p,frame.len); free(frame.p); return -1;
}
#endif
