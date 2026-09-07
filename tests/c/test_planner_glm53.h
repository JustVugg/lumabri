static void glm53_contract_test(void) {
    LmbModelShape m = {.layers=4, .hidden=128, .vocab=128, .memory_contract=1,
        .sizing_verified=1, .max_context=128, .session_fixed_bytes=65536,
        .session_token_bytes=512, .boundary_width=256};
    LmbRangeCost one=lmb_estimate_segment(&m,0,1,64,2);
    LmbRangeCost all=lmb_estimate_segment(&m,0,4,64,2);
    CHECK(one.ok && all.ok && one.state_bytes==all.state_bytes &&
          one.state_bytes==(65536u+64u*512u)*2u,
          "GLM5.3's engine-wide state was incorrectly divided by range length");
    m.session_token_bytes=UINT64_MAX;
    CHECK(!lmb_estimate_segment(&m,0,1,64,2).ok, "engine-wide session overflow admitted");
    m.session_token_bytes=512;
    LmbGlm53Inventory v={.model=&m,.h=128};
    LmbPlanTensor t={.elements=128,.bytes=512,.rank=1,.shape={128}};
    snprintf(t.dtype,sizeof t.dtype,"F32");
    snprintf(t.name,sizeof t.name,"model.language_model.norm.weight");
    CHECK(!lmb_glm53_tensor(&t,&v) && v.prefix==2 && m.edge_resident_bytes==1024,
          "GLM5.3 text wrapper or conservative Edge norm bound failed");
    CHECK(lmb_glm53_tensor(&t,&v)!=0,"GLM5.3 duplicated final norm accepted");
    snprintf(t.name,sizeof t.name,"model.embed_tokens.weight");
    CHECK(lmb_glm53_tensor(&t,&v)!=0,"mixed GLM5.3 namespaces accepted");
    snprintf(t.name,sizeof t.name,"model.visual.patch_embed.proj.weight");
    CHECK(lmb_glm53_tensor(&t,&v)!=0,"unbudgeted replicated vision tower accepted");
    snprintf(t.name,sizeof t.name,"lm_head.weight");
    snprintf(t.dtype,sizeof t.dtype,"U8");
    CHECK(lmb_glm53_tensor(&t,&v)!=0,"unverified GLM5.3 packed format admitted");
}
