/* SPDX-License-Identifier: MIT */
/* Real parser over the exact SM8550 GPU/GMU reg cells; no device access. */
#include "XnuMemoryFixture.h"
#include "NativeAdrenoBoard.h"
#include <cstdio>
#include <cstring>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
static void put(uint8_t *at,uint32_t value)
{at[0]=uint8_t(value>>24);at[1]=uint8_t(value>>16);at[2]=uint8_t(value>>8);at[3]=uint8_t(value);}
static size_t reg(uint8_t *out,const uint32_t (*cells)[4],size_t windows)
{
    for(size_t i=0;i<windows;++i)for(size_t c=0;c<4;++c)put(out+i*16+c*4,cells[i][c]);
    return windows*16;
}
static NativeAdrenoWindow windows[NativeAdrenoMaxWindows];
int main()
{
    /* sm8550.dtsi gpu@3d00000: three reg windows, #address/size-cells = 2. */
    {
        static const uint32_t cells[][4]={{0,0x03d00000,0,0x40000},{0,0x03d9e000,0,0x1000},
            {0,0x03d61000,0,0x800}};
        uint8_t raw[64];const size_t bytes=reg(raw,cells,3);
        size_t count=0;
        CHECK(NativeAdrenoBoard::parseReg(raw,bytes,2,2,windows,NativeAdrenoMaxWindows,&count)==kIOReturnSuccess);
        CHECK(count==3);
        CHECK(windows[0].base==0x03d00000 && windows[0].bytes==0x40000);
        CHECK(windows[1].base==0x03d9e000 && windows[1].bytes==0x1000);
        CHECK(windows[2].base==0x03d61000 && windows[2].bytes==0x800);
    }
    /* sm8550.dtsi gmu@3d6a000: gmu, rscc and pdc windows. */
    {
        static const uint32_t cells[][4]={{0,0x03d6a000,0,0x35000},{0,0x03d50000,0,0x10000},
            {0,0x0b280000,0,0x10000}};
        uint8_t raw[64];const size_t bytes=reg(raw,cells,3);
        size_t count=0;
        CHECK(NativeAdrenoBoard::parseReg(raw,bytes,2,2,windows,NativeAdrenoMaxWindows,&count)==kIOReturnSuccess);
        CHECK(count==3 && windows[2].base==0x0b280000 && windows[2].bytes==0x10000);
    }
    /* One-cell values and a 2/1 node both parse. */
    {
        uint8_t raw[16];size_t count=0;
        put(raw,0x03d00000);put(raw+4,0x40000);
        CHECK(NativeAdrenoBoard::parseReg(raw,8,1,1,windows,1,&count)==kIOReturnSuccess);
        CHECK(count==1 && windows[0].base==0x03d00000 && windows[0].bytes==0x40000);
        uint8_t mixed[24];
        put(mixed,0);put(mixed+4,0x03d00000);put(mixed+8,0x40000);
        put(mixed+12,0);put(mixed+16,0x03d9e000);put(mixed+20,0x1000);
        CHECK(NativeAdrenoBoard::parseReg(mixed,24,2,1,windows,2,&count)==kIOReturnSuccess);
        CHECK(count==2 && windows[1].base==0x03d9e000 && windows[1].bytes==0x1000);
    }
    /* sm8550.dtsi gpu iommus: <&adreno_smmu 0 0>, <&adreno_smmu 1 0>. */
    {
        static const uint32_t cells[][3]={{0x1a,0,0},{0x1a,1,0}};
        uint8_t raw[32];
        for(size_t i=0;i<2;++i)for(size_t c=0;c<3;++c)put(raw+i*12+c*4,cells[i][c]);
        uint16_t sids[4]{};
        size_t count=0;
        CHECK(NativeAdrenoBoard::parseIommus(raw,24,2,sids,4,&count)==kIOReturnSuccess);
        CHECK(count==2 && sids[0]==0 && sids[1]==1);
        uint16_t untouched[2]={0xbeef,0xbeef};
        CHECK(NativeAdrenoBoard::parseIommus(raw,24,2,untouched,1,&count)==kIOReturnNoResources);
        CHECK(untouched[0]==0xbeef);
        CHECK(NativeAdrenoBoard::parseIommus(raw,23,2,sids,4,&count)==kIOReturnBadArgument);
        CHECK(NativeAdrenoBoard::parseIommus(raw,24,0,sids,4,&count)==kIOReturnBadArgument);
        CHECK(NativeAdrenoBoard::parseIommus(raw,24,5,sids,4,&count)==kIOReturnBadArgument);
        CHECK(NativeAdrenoBoard::parseIommus(nullptr,24,2,sids,4,&count)==kIOReturnBadArgument);
        CHECK(NativeAdrenoBoard::parseIommus(raw,24,2,nullptr,4,&count)==kIOReturnNoResources);
        CHECK(NativeAdrenoBoard::parseIommus(raw,24,2,sids,4,nullptr)==kIOReturnBadArgument);
        uint8_t unnamed[12];put(unnamed,0);put(unnamed+4,0);put(unnamed+8,0);
        CHECK(NativeAdrenoBoard::parseIommus(unnamed,12,2,sids,4,&count)==kIOReturnBadArgument);
        uint8_t wide[12];put(wide,0x1a);put(wide+4,0x10000);put(wide+8,0);
        CHECK(NativeAdrenoBoard::parseIommus(wide,12,2,sids,4,&count)==kIOReturnBadArgument);
    }
    /* Refusals precede any output: nothing is written on failure. */
    {
        static const uint32_t cells[][4]={{0,0x03d00000,0,0x40000},{0,0x03d9e000,0,0x1000}};
        uint8_t raw[64];const size_t bytes=reg(raw,cells,2);
        size_t count=99;
        std::memset(windows,0x5a,sizeof(windows));
        CHECK(NativeAdrenoBoard::parseReg(nullptr,bytes,2,2,windows,2,&count)==kIOReturnBadArgument);
        CHECK(NativeAdrenoBoard::parseReg(raw,bytes,0,2,windows,2,&count)==kIOReturnBadArgument);
        CHECK(NativeAdrenoBoard::parseReg(raw,bytes,3,2,windows,2,&count)==kIOReturnBadArgument);
        CHECK(NativeAdrenoBoard::parseReg(raw,bytes,2,0,windows,2,&count)==kIOReturnBadArgument);
        CHECK(NativeAdrenoBoard::parseReg(raw,bytes,2,2,windows,1,&count)==kIOReturnNoResources);
        CHECK(NativeAdrenoBoard::parseReg(raw,bytes+1,2,2,windows,2,&count)==kIOReturnBadArgument);
        CHECK(NativeAdrenoBoard::parseReg(raw,bytes,2,2,nullptr,2,&count)==kIOReturnNoResources);
        CHECK(NativeAdrenoBoard::parseReg(raw,bytes,2,2,windows,2,nullptr)==kIOReturnBadArgument);
        CHECK(windows[0].base==UINT64_C(0x5a5a5a5a5a5a5a5a));
        /* A zero size and an address wrap are refused. */
        static const uint32_t zero[][4]={{0,0x03d00000,0,0}};
        uint8_t one[16];reg(one,zero,1);
        CHECK(NativeAdrenoBoard::parseReg(one,16,2,2,windows,1,&count)==kIOReturnBadArgument);
        uint8_t wrap[16];put(wrap,0xffffffff);put(wrap+4,0xfffff000);put(wrap+8,0);put(wrap+12,0x2000);
        CHECK(NativeAdrenoBoard::parseReg(wrap,16,2,2,windows,1,&count)==kIOReturnBadArgument);
        CHECK(windows[0].base==UINT64_C(0x5a5a5a5a5a5a5a5a));
    }
    std::printf("PASS: 2 real SM8550 reg sets, 2 alternate cell formats, the real GPU iommu specifiers and 19 malformed/limit refusals over the device-tree parser\n");
}
