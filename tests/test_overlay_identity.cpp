#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <string>
#include <vector>
#include "../src/overlay-identity.h"
#include "../src/overlay-identity-model.h"
#include "../src/launcher-builder.h"

using Bytes=std::vector<uint8_t>;
static bool Validate(const std::wstring& value,std::wstring& normalized){
    normalized=value;
    if(value.rfind(L"gh:",0)==0){std::wstring body=value.substr(3);size_t slash=body.find(L'/');if(body.rfind(L"gh:",0)==0||slash==std::wstring::npos||!slash||slash+1>=body.size()||body.find(L'/',slash+1)!=std::wstring::npos)return false;for(size_t i=3;i<normalized.size();++i)normalized[i]=(wchar_t)towlower(normalized[i]);return true;}
    if(value.rfind(L"url:https://",0)==0)return value.size()>12&&value.substr(12).rfind(L"url:",0)!=0;
    if(value.rfind(L"url:http://localhost",0)==0)return value.size()>20&&value.substr(11).rfind(L"url:",0)!=0;
    return false;
}
static bool Encode(const std::wstring& id,Bytes& wire,const Bytes& key={}){return OverlayIdentity::EncodeOverlayIdentity(id,key.empty()?nullptr:key.data(),key.size(),Validate,wire);}
static bool Decode(const Bytes& wire,std::wstring& id,size_t key=0,size_t limit=4096){OverlayIdentity::Decoded out;if(!OverlayIdentity::DecodeOverlayIdentity(wire.data(),wire.size(),key,limit,Validate,out))return false;id=out.identity;return true;}
static bool DecodeComplete(const Bytes& wire,std::wstring& id,size_t key=0,size_t limit=4096){OverlayIdentity::Decoded out;if(!OverlayIdentity::DecodeOverlayIdentity(wire.data(),wire.size(),key,limit,Validate,out)||out.encodedBodyOffset!=0)return false;id=out.identity;return true;}
static Bytes Physical(const char* body,uint8_t kind){Bytes wire(body,body+std::strlen(body));wire.push_back(uint8_t(kind|wire.size()));return wire;}
#define CHECK(x) do{if(!(x)){std::fwprintf(stderr,L"overlay identity test failed at line %d: %hs\n",__LINE__,#x);return 1;}}while(0)

int wmain(){
    Bytes wire,again;std::wstring decoded,normalized;
    CHECK(Validate(L"gh:owner/repository",normalized)&&normalized==L"gh:owner/repository");
    CHECK(Validate(L"gh:owner/repository#app",normalized)&&normalized==L"gh:owner/repository#app");
    for(const wchar_t* spelling:{L"gh:1KB-exe/1KB",L"gh:1kb-exe/1kb",L"gh:1Kb-Exe/1kB"})
        CHECK(Validate(spelling,normalized)&&normalized==L"gh:1kb-exe/1kb");
    CHECK(Validate(L"gh:Owner/Repo#SubApp",normalized)&&normalized==L"gh:owner/repo#subapp");
    CHECK(CanonicalGithubManifestUrl(L"1kb-exe/1kb",L"")==L"https://github.com/1kb-exe/1kb/releases/latest/download/1KB.ini");
    CHECK(CanonicalGithubManifestUrl(L"owner/repo",L"subapp")==L"https://github.com/owner/repo/releases/download/subapp/1KB.ini");
    CHECK(std::count(OverlayIdentity::Model::UrlTokens.begin(),OverlayIdentity::Model::UrlTokens.end(),std::string_view("1kb"))==1);
    CHECK(std::count(OverlayIdentity::Model::UrlTokens.begin(),OverlayIdentity::Model::UrlTokens.end(),std::string_view("1KB"))==1);
    CHECK(Validate(L"url:https://example.com/config.ini",normalized)&&normalized==L"url:https://example.com/config.ini");
    CHECK(Validate(L"url:https://farzher.com/desktop-honey/latest#desktop-honey",normalized)&&normalized==L"url:https://farzher.com/desktop-honey/latest#desktop-honey");
    CHECK(Validate(L"url:http://localhost:12345/config.ini",normalized)&&normalized==L"url:http://localhost:12345/config.ini");

    CHECK(Encode(OverlayIdentity::OneKBIdentity,wire)&&wire==Bytes{0xc0});
    for(const wchar_t* spelling:{L"gh:1KB-exe/1KB",L"gh:1kb-exe/1kb",L"gh:1Kb-Exe/1kB"}) {
        CHECK(Validate(spelling,normalized)&&Encode(normalized,again)&&again==wire);
    }
    CHECK(DecodeComplete(Bytes{0xc0},decoded)&&decoded==OverlayIdentity::OneKBIdentity);
    CHECK(Encode(decoded,again)&&again==wire);
    for(const wchar_t* nearMatch:{L"gh:1kb-exe/1kb#app",L"gh:other/repository",L"gh:other/repo",L"url:https://1KB-exe.com/1KB"})
        CHECK(Encode(nearMatch,wire)&&wire.back()!=OverlayIdentity::BuiltInOneKB);
    CHECK(!Encode(L"gh:Owner/Repo",wire)&&!Encode(L"gh:1KB-exe",wire));
    CHECK(!Decode(Bytes{0xc0},decoded,0,sizeof("gh:1kb-exe/1kb")-2));

    CHECK(Encode(L"gh:owner/repo",wire)&&wire.size()<11&&Decode(wire,decoded)&&decoded==L"gh:owner/repo");
    CHECK(Encode(L"gh:owner/repo#app",wire)&&Decode(wire,decoded)&&decoded==L"gh:owner/repo#app");
    for(const wchar_t* referenced:{L"gh:same/same",L"gh:foobar/foo",L"gh:foobar/bar",L"gh:xxfooyy/foo",
            L"gh:vuejs/vue",L"gh:nodejs/node",L"gh:php/php-src",L"gh:farzher/farzher-test",
            L"gh:farzher/test-farzher",L"gh:farzher/my-farzher-test",L"gh:abcdefgh/xxcdefyycdef",
            L"gh:owner/repository#owner",L"gh:owner/repository#repository",L"gh:owner/repository#owner-repository"}) {
        CHECK(Encode(referenced,wire)&&Decode(wire,decoded)&&decoded==referenced);
        CHECK(Encode(referenced,again)&&again==wire);
    }
    CHECK(Encode(L"gh:1kb-exe/1kb#stable-2",wire));
    CHECK(Decode(wire,decoded)&&decoded==L"gh:1kb-exe/1kb#stable-2");

    CHECK(Encode(L"url:https://example.com/config.ini",wire)&&Decode(wire,decoded)&&decoded==L"url:https://example.com/config.ini");
    CHECK(Encode(L"url:https://example.com:8443/a.ini?q=one.com",wire)&&wire.back()==(0x40|(wire.size()-1))&&Decode(wire,decoded)&&decoded==L"url:https://example.com:8443/a.ini?q=one.com");
    CHECK(Encode(L"url:http://localhost:12345/1KB.ini",wire)&&Decode(wire,decoded)&&decoded==L"url:http://localhost:12345/1KB.ini");
    CHECK(Encode(L"url:https://x/1kb.ini",wire)&&Decode(wire,decoded)&&decoded==L"url:https://x/1kb.ini");
    size_t lowerTokenSize=wire.size();
    CHECK(Encode(L"url:https://x/1KB.ini",wire)&&Decode(wire,decoded)&&decoded==L"url:https://x/1KB.ini");
    CHECK(wire.size()==lowerTokenSize);
    CHECK(Encode(L"url:https://farzher.com/desktop-honey/latest#desktop-honey",wire));
    CHECK(Decode(wire,decoded)&&decoded==L"url:https://farzher.com/desktop-honey/latest#desktop-honey");
    CHECK(Encode(L"url:https://example.com/update/update/update/config.ini",wire));
    CHECK(Decode(wire,decoded)&&decoded==L"url:https://example.com/update/update/update/config.ini");
    CHECK(Encode(L"url:https://caf\u00e9.example/config.ini",wire)&&Decode(wire,decoded)&&decoded==L"url:https://caf\u00e9.example/config.ini");
    for(const wchar_t* ood:{L"url:https://example.com/Releases/MyApp/Manifest.INI",
            L"url:https://example.com/downloads/ProductName/current.txt",
            L"url:https://example.com/api/Manifest.json?Channel=Stable",
            L"url:https://example.com/files/CaseSensitiveObjectKey",
            L"url:https://example.com/a+b;v@x!y$z",L"url:https://example.com/%E2%9C%93?q=a%20b"}) {
        CHECK(Encode(ood,wire)&&Decode(wire,decoded)&&decoded==ood);
        CHECK(Encode(ood,again)&&again==wire);
    }

    for(size_t length:{size_t(1),size_t(62),size_t(63),size_t(255),size_t(1024)}){
        std::wstring id=L"url:https://"+std::wstring(length,L'a');CHECK(Encode(id,wire));
        CHECK(Decode(wire,decoded,0,2048)&&decoded==id);
        if(length==1024)CHECK(!Decode(wire,decoded,0,100));
        CHECK(Encode(id,again)&&again==wire);
    }
    CHECK(!Encode(L"",wire)&&!Encode(L"GH:owner/repo",wire)&&!Encode(L"url:ftp://example.com/x",wire));
    CHECK(!Encode(L"gh:owner",wire)&&!Encode(L"url:http://example.com/x",wire));

    CHECK(!Decode(Bytes{0xc1},decoded)&&!Decode(Bytes{0xd0},decoded)&&!Decode(Bytes{0xe0},decoded)&&!Decode(Bytes{0xff},decoded));
    CHECK(!DecodeComplete(Bytes{'x',0xc0},decoded));
    CHECK(!DecodeComplete(Bytes{0x3f,0xc0},decoded));
    CHECK(!Decode(Bytes{0xc0},decoded,32));
    CHECK(!Decode(Bytes{0x00,0x41},decoded)&&!Decode(Bytes{0xf8,0x41},decoded));
    CHECK(!Decode(Bytes{0x00},decoded));
    CHECK(!Decode(Bytes{'a',0x02},decoded));
    CHECK(!Decode(Bytes{'a',0x3f},decoded));
    CHECK(!Decode(Bytes{'a',62,0x7f},decoded));
    CHECK(!Decode(Physical("gh:owner/repo",OverlayIdentity::GitHubKind),decoded));
    CHECK(!Decode(Physical("url:https://example.com/config.ini",OverlayIdentity::HttpsKind),decoded));
    CHECK(!Decode(Physical("url:http://localhost:12345/config.ini",OverlayIdentity::HttpKind),decoded));
    CHECK(!Decode(Bytes{0xff,0x01},decoded));
    CHECK(!Decode(Bytes{'a',0x41},decoded,0,12));
    // Invalid UPPER payload, COPY from empty output, and out-of-range distance.
    CHECK(!DecodeComplete(Bytes{0xff,0xf7,0xf7,0x43},decoded));
    CHECK(!DecodeComplete(Bytes{0xeb,0xb8,0x42},decoded));
    CHECK(!DecodeComplete(Bytes{0x0e,0x96,0xe0,0x43},decoded));

    Bytes key(8);for(size_t i=0;i<key.size();++i)key[i]=uint8_t(i*7+3);
    CHECK(Encode(L"gh:owner/repo#app",wire,key)&&wire.size()>9&&wire[wire.size()-9]==key[0]&&wire[wire.size()-2]==key[7]);
    OverlayIdentity::Decoded privateDecoded;CHECK(OverlayIdentity::DecodeOverlayIdentity(wire.data(),wire.size(),8,4096,Validate,privateDecoded));
    CHECK(privateDecoded.identity==L"gh:owner/repo#app"&&privateDecoded.secretOffset==wire.size()-9);
    CHECK(Encode(OverlayIdentity::OneKBIdentity,wire,key));
    CHECK(OverlayIdentity::DecodeOverlayIdentity(wire.data(),wire.size(),8,4096,Validate,privateDecoded));
    CHECK(privateDecoded.identity==OverlayIdentity::OneKBIdentity&&privateDecoded.secretOffset==privateDecoded.encodedBodyBytes);

    // Every truncated prefix, full trailing bytes, wrong typed kind, and random
    // malformed input must fail or decode to a canonical identity without faulting.
    CHECK(Encode(L"gh:farzher/my-farzher-test#farzher-test",wire));
    for(size_t n=0;n<wire.size();++n){Bytes cut(wire.begin(),wire.begin()+n);CHECK(!DecodeComplete(cut,decoded));}
    Bytes trailing=wire;trailing.insert(trailing.begin(),0x55);CHECK(!DecodeComplete(trailing,decoded));
    Bytes wrong=wire;wrong.back()=uint8_t(OverlayIdentity::HttpsKind|(wrong.back()&OverlayIdentity::LengthMask));CHECK(!DecodeComplete(wrong,decoded));
    uint32_t random=0x91e10da5;for(unsigned test=0;test<2000;++test){random=random*1664525+1013904223;Bytes fuzz((random>>27)+1);for(auto& b:fuzz){random=random*1664525+1013904223;b=uint8_t(random>>24);}OverlayIdentity::Decoded ignored;OverlayIdentity::DecodeOverlayIdentity(fuzz.data(),fuzz.size(),0,64,Validate,ignored);}

    std::puts("PASS compact overlay identity codec");return 0;
}
