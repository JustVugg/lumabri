static void v4_contract_test(void) {
    LmbModelShape m={.layers=3,.hidden=128,.vocab=128,.experts=4,.experts_per_tok=2};
    LmbV4Expert experts[12]={0};
    LmbV4Inventory v={.m=&m,.h=128,.heads=4,.hd=32,.qr=128,.og=1,.orr=128,
        .inter=128,.hc=2,.ih=2,.id=32,.window=8,.topk=2,.hash=1,.expert=experts};
    v.ratios[1]=4;v.ratios[2]=8;
    for(unsigned layer=0;layer<3;layer++) {
        LmbV4Spec specs[48];unsigned n=lmb_v4_specs(&v,layer,specs);
        CHECK(n<48,"V4 required tensor plan exceeds its bounded spec array");
        for(unsigned i=0;i<n;i++) {
            LmbPlanTensor t={.rank=specs[i].cols ? 2 : 1,.shape={specs[i].rows,specs[i].cols}};
            t.elements=specs[i].rows*(specs[i].cols ? specs[i].cols : 1);
            unsigned width=!strcmp(specs[i].dtype,"F32") ? 4 : !strcmp(specs[i].dtype,"BF16") ? 2 :
                !strcmp(specs[i].dtype,"I64") ? 8 : 1;
            t.bytes=t.elements*width;
            snprintf(t.dtype,sizeof t.dtype,"%s",specs[i].dtype);
            snprintf(t.name,sizeof t.name,"layers.%u.%.79s",layer,specs[i].name);
            uint64_t before=m.memory[layer].resident_bytes;
            CHECK(!lmb_v4_tensor(&t,&v),"valid native V4 tensor rejected: %s",t.name);
            CHECK(m.memory[layer].resident_bytes-before==t.elements*
                (!strcmp(t.dtype,"F8_E8M0") ? 4u : width),"V4 dense scales not expanded");
            CHECK(lmb_v4_tensor(&t,&v)!=0,"duplicate V4 tensor admitted");
        }
    }
    LmbPlanTensor t={.rank=2,.shape={128,64},.elements=8192,.bytes=8192,.file_id=1};
    snprintf(t.name,sizeof t.name,"layers.0.ffn.experts.0.w1.weight");
    snprintf(t.dtype,sizeof t.dtype,"I8");
    CHECK(!lmb_v4_tensor(&t,&v),"native V4 packed expert rejected");
    snprintf(t.name,sizeof t.name,"layers.0.ffn.experts.0.w2.weight");t.file_id=2;
    CHECK(lmb_v4_tensor(&t,&v)!=0,"V4 expert spanning different shards admitted");
    t.file_id=1;snprintf(t.dtype,sizeof t.dtype,"U8");
    CHECK(lmb_v4_tensor(&t,&v)!=0,"V4 incompatible packed expert dtype admitted");
}
