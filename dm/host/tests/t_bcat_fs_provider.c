#include <stddef.h>
#include <string.h>
char bcat_fixture_overlay[1024],bcat_fixture_delivery[1024];
const char *dm_bcat_overlay(void){return bcat_fixture_overlay[0]?bcat_fixture_overlay:NULL;}
const char *dm_bcat_delivery(void){return bcat_fixture_delivery[0]?bcat_fixture_delivery:NULL;}
int dm_bcat_copy_delivery(char *out,size_t size){if(!bcat_fixture_delivery[0])return 0;return strlcpy(out,bcat_fixture_delivery,size)<size;}
