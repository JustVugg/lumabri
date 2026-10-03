#define _GNU_SOURCE
#define main lmb_v41_unused_cli_main
#include "v41_range_core.c"
#undef main
#define LMB_V41_PRODUCT 1
#include "v41_adapter.h"
int lmb_v41_adapter_register(void) {return lmb_v41_register_impl();}
