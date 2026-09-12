#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "../src/update-storage.h"
#include <cassert>
#include <cstdio>
using namespace Updates;

static void Put(const std::wstring& root,const std::wstring& path,const char* value){assert(Parents(root,path,true));assert(RuntimeSupport::AtomicWrite(Native(root,path),std::string(value)));}
static std::vector<char> Bytes(const char* text){return std::vector<char>(text,text+strlen(text));}
static void Reset(const std::wstring& root){assert(RuntimeSupport::DeleteTree(root));assert(Tree(root));}

int wmain(int argc,wchar_t** argv){
    if(argc==4&&!wcscmp(argv[1],L"extract"))return Tree(argv[3])&&Extract(argv[2],argv[3])?0:1;
    Manifest full,incremental;
    assert(Parse(Bytes("version=1.2.3\ndownload=app-v1.2.3.zip\n"),full));
    assert(full.version==L"1.2.3"&&full.download==L"app-v1.2.3.zip"&&full.changes.empty()&&full.updates==L"background");
    assert(Render(full)==L"version=1.2.3\ndownload=app-v1.2.3.zip\n");
    assert(Parse(Bytes("version=1.2.4\ndownload=app-v1.2.3.zip\nchanges=app-v1.2.4-changes.zip\nupdates=restart\n"),incremental));
    assert(UseChanges(true,L"app-v1.2.3.zip",incremental));
    assert(!UseChanges(true,L"app-v1.0.0.zip",incremental));
    assert(!Parse(Bytes("format=1\nversion=1.2.3\ndownload=a.zip\n"),full));

    Inventory snapshot={{L"A",{1,L"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}},{L"B",{1,L"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"}},{L"C",{1,L"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"}}};
    Inventory one=snapshot;one[L"A"]={2,L"dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"};one.erase(L"B");
    auto changed=AccumulateChanges(snapshot,one,{});assert(changed==std::set<std::wstring>({L"A",L"B"}));
    Inventory two=one;two[L"C"]={2,L"eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"};changed=AccumulateChanges(snapshot,two,changed);assert(changed==std::set<std::wstring>({L"A",L"B",L"C"}));
    Inventory restored=two;restored[L"A"]=snapshot[L"A"];restored[L"B"]=snapshot[L"B"];changed=AccumulateChanges(snapshot,restored,changed);assert(ExistingChanges(restored,changed).size()==3&&DeletedChanges(restored,changed).empty());

    wchar_t temp[MAX_PATH]{};assert(GetTempPathW(MAX_PATH,temp));std::wstring root=std::wstring(temp)+L"1kb-simple-update-"+std::to_wstring(GetCurrentProcessId());Reset(root);
    assert(Tree(root+L"\\1.0.0"));Put(root+L"\\1.0.0",L"app.exe","old");Put(root+L"\\1.0.0",L"unchanged.dat","same");Put(root+L"\\1.0.0",L"old.dll","remove");assert(WriteCurrent(root,L"1.0.0"));
    assert(Tree(root+L"\\prepared"));Put(root+L"\\prepared",L"app.exe","new");Put(root+L"\\prepared",L"assets/new.png","png");std::set<std::wstring> deleted={L"old.dll"};
    assert(InstallIncremental(root,L"1.0.0",L"1.0.1",root+L"\\prepared",deleted));
    assert(Missing(root+L"\\1.0.0")&&Regular(root+L"\\1.0.1\\app.exe")&&Regular(root+L"\\1.0.1\\unchanged.dat")&&Missing(root+L"\\1.0.1\\old.dll"));std::wstring current;std::vector<char> currentBytes;assert(ReadMetadata(root+L"\\current.txt",currentBytes));assert(std::string(currentBytes.begin(),currentBytes.end())=="1.0.1\n");
    // A broken rename has no usable current directory, so the planner chooses full repair.
    assert(Move(root+L"\\1.0.1",root+L"\\1.0.2"));assert(!UseChanges(false,L"app-v1.0.0.zip",incremental));
    assert(RuntimeSupport::DeleteTree(root));puts("simple manifest, cumulative overlay, in-place install, and repair selection passed");return 0;
}
