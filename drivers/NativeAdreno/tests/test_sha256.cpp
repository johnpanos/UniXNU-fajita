/* SPDX-License-Identifier: MIT */
/* FIPS vectors plus the real pinned GMU firmware; host-side file read only. */
#include "XnuMemoryFixture.h"
#include "NativeAdrenoFirmware.h"
#include "NativeAdrenoSha256.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
static int hex(const char *text,uint8_t *out,size_t bytes)
{
    if(std::strlen(text)!=bytes*2)return 1;
    for(size_t i=0;i<bytes;++i) {
        unsigned value=0;
        if(std::sscanf(text+2*i,"%2x",&value)!=1)return 1;
        out[i]=uint8_t(value);
    }
    return 0;
}
static void expect(const char *label,const char *message,const char *wanted)
{
    uint8_t digest[32],expected[32];
    NativeAdrenoSha256::hash(reinterpret_cast<const uint8_t *>(message),
        message?std::strlen(message):0,digest);
    CHECK(hex(wanted,expected,32)==0);
    if(std::memcmp(digest,expected,32)) {
        std::fprintf(stderr,"vector %s mismatch\n",label);
        std::exit(1);
    }
}
int main(int argc,char **argv)
{
    uint8_t digest[32];
    NativeAdrenoSha256::hash(nullptr,0,digest);
    uint8_t empty[32];
    CHECK(hex("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",empty,32)==0);
    CHECK(std::memcmp(digest,empty,32)==0);
    expect("abc","abc","ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    expect("two-block","abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    {
        std::vector<uint8_t> million(1000000,uint8_t('a'));
        NativeAdrenoSha256::hash(million.data(),million.size(),digest);
        CHECK(hex("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",empty,32)==0);
        CHECK(std::memcmp(digest,empty,32)==0);
    }
    /* Real firmware: exact length and pinned digest, then refusals. */
    if(argc==3) {
        FILE *file=std::fopen(argv[1],"rb");
        CHECK(file);
        CHECK(std::fseek(file,0,SEEK_END)==0);
        const long size=std::ftell(file);
        CHECK(size>0);
        CHECK(std::fseek(file,0,SEEK_SET)==0);
        std::vector<uint8_t> image(static_cast<size_t>(size));
        CHECK(std::fread(image.data(),1,image.size(),file)==image.size());
        CHECK(std::fclose(file)==0);
        NativeAdrenoFirmwareRules rules{};
        rules.bytes=image.size();
        CHECK(hex(argv[2],rules.sha256,32)==0);
        NativeAdrenoFirmwareView view{};
        CHECK(NativeAdrenoFirmware::validate(image.data(),image.size(),&rules,&view)==kIOReturnSuccess);
        CHECK(view.bytes==image.data() && view.length==image.size());
        CHECK(!std::memcmp(view.sha256,rules.sha256,32));
        NativeAdrenoFirmwareRules wrong_size=rules;
        wrong_size.bytes=image.size()-1;
        CHECK(NativeAdrenoFirmware::validate(image.data(),image.size(),&wrong_size,&view)==kIOReturnBadArgument);
        CHECK(!view.bytes);
        NativeAdrenoFirmwareRules wrong_hash=rules;
        wrong_hash.sha256[0]^=1;
        CHECK(NativeAdrenoFirmware::validate(image.data(),image.size(),&wrong_hash,&view)==kIOReturnIOError);
        CHECK(!view.bytes && !view.length);
        std::vector<uint8_t> corrupt=image;
        corrupt[corrupt.size()/2]^=1;
        CHECK(NativeAdrenoFirmware::validate(corrupt.data(),corrupt.size(),&rules,&view)==kIOReturnIOError);
        CHECK(NativeAdrenoFirmware::validate(nullptr,image.size(),&rules,&view)==kIOReturnBadArgument);
        CHECK(NativeAdrenoFirmware::validate(image.data(),image.size(),nullptr,&view)==kIOReturnBadArgument);
        CHECK(NativeAdrenoFirmware::validate(image.data(),image.size(),&rules,nullptr)==kIOReturnBadArgument);
        CHECK(NativeAdrenoFirmware::validate(image.data(),0,&rules,&view)==kIOReturnBadArgument);
        std::printf("PASS: 4 FIPS vectors and the pinned GMU firmware (%ld bytes) validated, 6 malformed/limit refusals\n",
            static_cast<long>(image.size()));
    } else {
        std::printf("PASS: 4 FIPS vectors; no firmware path supplied\n");
    }
}
