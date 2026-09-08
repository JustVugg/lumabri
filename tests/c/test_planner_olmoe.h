static void olmoe_contract_test(void) {
    LmbModelShape m={.layers=2,.hidden=64,.vocab=256,.experts=8};
    uint8_t seen[16]={0};
    LmbOlmoeInventory v={.m=&m,.inter=128,.experts=seen};
    LmbPlanTensor t={.rank=2,.shape={64,64},.elements=4096,.bytes=8192};
    snprintf(t.dtype,sizeof t.dtype,"BF16");
    snprintf(t.name,sizeof t.name,"model.layers.0.self_attn.q_proj.weight");
    CHECK(!lmb_olmoe_tensor(&t,&v) && m.memory[0].resident_bytes==16384,
          "OLMoE BF16 dense source not expanded to native f32");
    CHECK(lmb_olmoe_tensor(&t,&v)!=0,"duplicate OLMoE dense tensor admitted");
    snprintf(t.name,sizeof t.name,"model.layers.1.self_attn.q_proj.weight");
    t.shape[0]=32;t.shape[1]=128;
    CHECK(lmb_olmoe_tensor(&t,&v)!=0,"OLMoE same-numel wrong matrix shape admitted");
    snprintf(t.name,sizeof t.name,"model.layers.0.mlp.experts.0.gate_proj.weight");
    CHECK(lmb_olmoe_tensor(&t,&v)!=0,"unconverted OLMoE expert admitted");
    snprintf(t.name,sizeof t.name,"model.layers.0.mlp.experts.0.merged_weight");
    snprintf(t.dtype,sizeof t.dtype,"U8");t.elements=t.bytes=3*128*64;
    CHECK(!lmb_olmoe_tensor(&t,&v),"merged OLMoE int8 expert rejected");
    snprintf(t.name,sizeof t.name,"model.layers.0.mlp.experts.1.merged_weight");
    t.bytes--;
    CHECK(lmb_olmoe_tensor(&t,&v)!=0,"truncated OLMoE expert admitted");
    snprintf(t.name,sizeof t.name,"model.layers.0.mlp.experts.0.qs");
    snprintf(t.dtype,sizeof t.dtype,"F32");t.elements=320;t.bytes=1280;
    CHECK(!lmb_olmoe_tensor(&t,&v) && seen[0]==3,"OLMoE row scales not validated");
}
