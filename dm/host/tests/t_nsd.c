// No game files, credentials or network calls. Exercise the actual NSD adapter.
#include "../nsd.h"
#include "../net_socket.h"
#include <stdio.h>
#include <string.h>
static dm_net_mode_t mode = DM_NET_NEXTENDO;
dm_net_mode_t dm_net_mode(void) { return mode; }
static int checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; fprintf(stderr,"NSD failed at line %d\n",__LINE__); } } while (0)
int main(void) {
    struct { char fqdn[256]; unsigned char guard[32]; } output;
    memset(&output,0xa5,sizeof output);
    CHECK(!dm_nsd_resolve(output.fqdn,"g23380901-%.s.n.srv.nintendo.net"));
    CHECK(!strcmp(output.fqdn,"g23380901-lp1.s.n.srv.nintendo.net"));
    CHECK(output.fqdn[255]==0 && output.guard[0]==0xa5 && output.guard[31]==0xa5);
    CHECK(!dm_nsd_resolve(output.fqdn,"CTEST.CDN.NINTENDO.NET.") && !strcmp(output.fqdn,"ctest.cdn.nintendo.net"));
    CHECK(!dm_nsd_resolve(output.fqdn,"accounts.nintendo.com") && !strcmp(output.fqdn,"e0d67c509fb203858ebcb2fe3f88c2aa.baas.nintendo.com"));
    CHECK(!dm_nsd_resolve(output.fqdn,"api.accounts.nintendo.com") && !strcmp(output.fqdn,"e0d67c509fb203858ebcb2fe3f88c2aa.baas.nintendo.com"));
    strcpy(output.fqdn,"nncs1-%.n.n.srv.nintendo.net");
    CHECK(!dm_nsd_resolve(output.fqdn,output.fqdn) && !strcmp(output.fqdn,"nncs1-lp1.n.n.srv.nintendo.net"));
    CHECK(!dm_nsd_resolve(output.fqdn,"one.%.%.example") && !strcmp(output.fqdn,"one.lp1.lp1.example"));
    const char *bad[]={"","a..b",".example","-bad.example","bad-.example","bad/name","http://example.com","white space","example:443","a..","invalid\x80.example"};
    for (unsigned i=0;i<sizeof bad/sizeof *bad;i++) { memset(&output,0xa5,sizeof output); CHECK(dm_nsd_resolve(output.fqdn,bad[i])!=0 && (unsigned char)output.fqdn[0]==0xa5 && output.guard[0]==0xa5); }
    char input[256];memset(input,'a',sizeof input);CHECK(dm_nsd_resolve(output.fqdn,input)!=0);
    input[64]='.';input[65]='b';input[66]=0;CHECK(dm_nsd_resolve(output.fqdn,input)!=0);
    memset(input,'%',85);input[85]=0;CHECK(dm_nsd_resolve(output.fqdn,input)!=0);
    memset(input,'a',sizeof input);input[63]=input[127]=input[191]='.';input[253]=0;
    CHECK(!dm_nsd_resolve(output.fqdn,input)&&strlen(output.fqdn)==253);
    input[254]=0;input[253]='b';CHECK(dm_nsd_resolve(output.fqdn,input)!=0);
    CHECK(dm_nsd_resolve(NULL,"example.com")!=0 && dm_nsd_resolve(output.fqdn,NULL)!=0);
    for (int i=0;i<3;i++) { mode=(dm_net_mode_t)i;memset(&output,0xa5,sizeof output);CHECK(dm_nsd_resolve(output.fqdn,"accounts.nintendo.com")!=0 && (unsigned char)output.fqdn[0]==0xa5); }
    mode=DM_NET_AUTO; unsetenv("DM_NEXTENDO_ROUTES"); CHECK(dm_nsd_resolve(output.fqdn,"nncs1-%.n.n.srv.nintendo.net")!=0);
    setenv("DM_NEXTENDO_ROUTES","/synthetic/public-policy.tsv",1); CHECK(!dm_nsd_resolve(output.fqdn,"nncs1-%.n.n.srv.nintendo.net") && !strcmp(output.fqdn,"nncs1-lp1.n.n.srv.nintendo.net"));
    printf("NSD: %d checks, %d failures; no network or account data\n",checks,failures);
    return failures?1:0;
}
