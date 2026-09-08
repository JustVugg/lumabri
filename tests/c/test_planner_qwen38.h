static int q38_metadata_visit(const LmbPlanTensor *t, void *opaque) {
    unsigned *visited=opaque;
    CHECK(!strcmp(t->dtype,"I64") && t->elements==2 &&
          t->meta_i64[0]==UINT64_C(0x0102030405060708) && t->meta_i64[1]==UINT64_MAX,
          "bounded metadata reader changed signed bits or byte order");
    (*visited)++;
    return 0;
}
static void qwen38_contract_test(void) {
    LmbModelShape m={.layers=4,.hidden=32,.vocab=256,.memory_contract=1,
        .sizing_verified=1,.max_context=128,.boundary_width=128};
    LmbQwen38Inventory v={.m=&m,.h=32,.hc=4,.rank=8,.ple=0,.pd=32,.pc=4,.nh=4,.nd=8,.parts=2};
    LmbPlanTensor t={.rank=1,.elements=128,.shape={128},.bytes=512};
    snprintf(t.dtype,sizeof t.dtype,"F32");
    snprintf(t.name,sizeof t.name,"model.hyper_connection_mixer.hc_norm.weight");
    CHECK(!lmb_q38_tensor(&t,&v) && m.edge_resident_bytes==512,"Qwen3.8 hyper-width norm not sized");
    CHECK(lmb_q38_tensor(&t,&v)!=0,"duplicate Qwen3.8 Edge tensor admitted");
    snprintf(t.name,sizeof t.name,"model.language_model.embed_tokens.weight");
    CHECK(lmb_q38_tensor(&t,&v)!=0,"mixed Qwen3.8 namespaces admitted");
    snprintf(t.name,sizeof t.name,"model.layers.1.ple.norm_key.weight");
    CHECK(lmb_q38_tensor(&t,&v)!=0,"PLE loaded outside its owning range");
    snprintf(t.name,sizeof t.name,"model.layers.0.ple.norm_key.weight");
    snprintf(t.dtype,sizeof t.dtype,"I64");
    CHECK(lmb_q38_tensor(&t,&v)!=0,"metadata accepted as Qwen3.8 floating weight");
    uint16_t expert_seen[4]={0};
    v.e=1; v.inter=136; v.experts=expert_seen;
    snprintf(t.name,sizeof t.name,"model.layers.0.mlp.experts.0.gate_proj.weight");
    snprintf(t.dtype,sizeof t.dtype,"F8_E4M3");
    t.rank=2; t.shape[0]=136; t.shape[1]=32; t.elements=136*32; t.bytes=t.elements;
    CHECK(!lmb_q38_tensor(&t,&v) && expert_seen[0]==33 && v.kinds[0]==8,
          "native FP8 expert was not tracked separately from float weights");
    CHECK(m.memory[0].resident_bytes==136*32*4,"FP8 opt-out expansion not budgeted");
    snprintf(t.name,sizeof t.name,"model.layers.0.mlp.experts.0.gate_proj.weight_scale_inv");
    snprintf(t.dtype,sizeof t.dtype,"F32");
    t.shape[0]=1; t.shape[1]=2; t.elements=2; t.bytes=8;
    CHECK(lmb_q38_tensor(&t,&v)!=0,"transposed partial FP8 block scales admitted");
    t.shape[0]=2; t.shape[1]=1;
    CHECK(!lmb_q38_tensor(&t,&v) && expert_seen[0]==289 && v.kinds[0]==8,
          "FP8 scale sidecar changed the arithmetic class");
    CHECK(m.memory[0].resident_bytes==136*32*4+16,"shared and per-slot scales not both budgeted");
    CHECK(lmb_q38_tensor(&t,&v)!=0,"duplicate FP8 sidecar admitted");
    snprintf(t.name,sizeof t.name,"model.layers.0.mlp.experts.0.up_proj.weight");
    snprintf(t.dtype,sizeof t.dtype,"F8_E8M0");
    CHECK(lmb_q38_tensor(&t,&v)!=0,"scale-only FP8 type admitted as an expert weight");
    m.memory[0].state_fixed_bytes=4096;
    m.memory[1].state_token_bytes=144;
    LmbRangeCost dn=lmb_estimate_segment(&m,0,1,64,2), qsa=lmb_estimate_segment(&m,1,2,64,2);
    CHECK(dn.ok && qsa.ok && dn.state_bytes==8192 && qsa.state_bytes==18432,
          "Qwen3.8 recurrent state and context state conflated");
    char path[]="/tmp/lumabri-i64-metadata-XXXXXX";
    int fd=mkstemp(path);
    CHECK(fd>=0,"metadata test could not create isolated file");
    if(fd<0) return;
    FILE *f=fdopen(fd,"wb");
    const char *header="{\"offsets\":{\"dtype\":\"I64\",\"shape\":[2],\"data_offsets\":[0,16]}}";
    unsigned char prefix[8], data[16]={8,7,6,5,4,3,2,1,255,255,255,255,255,255,255,255};
    for(unsigned i=0;i<8;i++) prefix[i]=(unsigned char)((uint64_t)strlen(header)>>(8*i));
    fwrite(prefix,1,8,f); fwrite(header,1,strlen(header),f); fwrite(data,1,sizeof data,f); fclose(f);
    unsigned visited=0; uint64_t budget=4096;
    CHECK(!lmb_plan_tensor_file(path,&budget,q38_metadata_visit,&visited) && visited==1,
          "valid small I64 metadata rejected");
    budget=strlen(header)+15;
    CHECK(lmb_plan_tensor_file(path,&budget,q38_metadata_visit,&visited)!=0,
          "I64 payload escaped shared metadata budget");
    CHECK(!truncate(path,8+(off_t)strlen(header)+15),"truncate metadata fixture failed");
    budget=4096;
    CHECK(lmb_plan_tensor_file(path,&budget,q38_metadata_visit,&visited)!=0,"truncated I64 metadata admitted");
    unlink(path);
}
