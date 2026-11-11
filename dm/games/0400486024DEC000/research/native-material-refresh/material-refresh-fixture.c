#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static unsigned char material[0x90], source[0x700], rows[3*48], context[0x200];
static int calls,checks,failed;
extern void *test_entry(void *,void *,void *,void *,void *,void *,void *,void *);
#define CHECK(x) do{checks++;if(!(x)){failed++;printf("FAIL line %d\n",__LINE__);}}while(0)
static void put(unsigned char *p,size_t off,uintptr_t x){memcpy(p+off,&x,8);}
static uintptr_t get(unsigned char *p,size_t off){uintptr_t x;memcpy(&x,p+off,8);return x;}
static void setup(void){
 memset(material,0,sizeof material);memset(source,0,sizeof source);memset(rows,0xaa,sizeof rows);memset(context,0,sizeof context);
 material[0x59]=1;material[0x5e]=3;material[0x5f]=48;material[0x3b+6]=2;material[0x3b+7]=2;
 put(material,0x60,(uintptr_t)rows);put(material,0x68,(uintptr_t)source);context[0x62]=7;
 source[6*64+0x3d]=6;source[7*64+0x3d]=6;put(source,6*64+0x18,0x100100);put(source,6*64+0x20,0x200200);
 put(rows,96,0x100000);put(rows,104,0x200203);
}
void test_refresh(void *m,void *unused){
 CHECK(m==material&&unused==NULL);calls++;put(rows,96,get(source,6*64+0x18));put(rows,104,get(source,6*64+0x20));memset(rows+112,0,32);
 __asm__ volatile("movi v31.16b, #0x55":::"v31");
}
static void *run(void *row){return test_entry(row,material,0,0,context,0,0,0);}
int main(void){
 setup();unsigned char vector[16],expected[16];memset(expected,0x33,16);
 __asm__ volatile("movi v31.16b, #0x33":::"v31");
 CHECK(run(rows+96)==rows+96);
 __asm__ volatile("str q31, [%0]"::"r"(vector):"memory");
 CHECK(!memcmp(vector,expected,16));CHECK(calls==1);CHECK(get(rows,96)==0x100100&&get(rows,104)==0x200200);CHECK(get(rows,112)==0&&get(rows,128)==0);
 run(rows+96);CHECK(calls==1);
 setup();put(source,6*64+0x20,0x200203);run(rows+96);CHECK(calls==1&&get(rows,104)==0x200203);
 setup();material[0x59]=0;run(rows+96);CHECK(calls==1);
 setup();material[0x59]=3;run(rows+96);CHECK(calls==1);
 setup();run(rows);CHECK(calls==1);
 setup();source[7*64+0x3d]=23;run(rows+96);CHECK(calls==1);
 setup();context[0x62]=255;context[0x39]=0;material[0x2c]=7;run(rows+96);CHECK(calls==2);
 setup();context[0x62]=255;context[0x39]=15;run(rows+96);CHECK(calls==2);
 printf("NATIVE_REFRESH_BRIDGE %d checks, %d failed\n",checks,failed);return failed?1:0;
}
