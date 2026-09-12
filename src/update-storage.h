#pragma once
#include "runtime-support.h"
#include <map>
#include <set>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <bcrypt.h>
#include "../third_party/zlib/zlib.h"
#include "../third_party/lzma/LzmaDec.h"
#pragma comment(lib,"bcrypt.lib")

namespace Updates {
constexpr uint64_t MetadataLimit=64ull*1024;
constexpr uint64_t ZipDirectoryLimit=16ull*1024*1024;
constexpr uint64_t PackageLimit=2ull*1024*1024*1024-1;
constexpr uint64_t ExtractedLimit=1ull<<40;
constexpr unsigned FileLimit=10000;

inline std::wstring Fold(std::wstring s){for(auto& c:s)c=(wchar_t)towlower(c);return s;}
inline bool Path(const std::wstring& s){
    if(s.empty()||s.size()>220||s.front()==L'/'||s.back()==L'/')return false;
    size_t at=0;unsigned depth=0;while(at<s.size()){
        size_t end=s.find(L'/',at);auto p=s.substr(at,end==s.npos?end:end-at);if(p.empty()||p==L"."||p==L".."||p.back()==L'.'||p.back()==L' '||++depth>32)return false;
        for(auto c:p)if(c<32||c>126||wcschr(L"<>:\\|?*\"~",c))return false;
        auto base=Fold(p.substr(0,p.find(L'.')));if(base==L"con"||base==L"prn"||base==L"aux"||base==L"nul"||base==L"conin$"||base==L"conout$"||(base.size()==4&&(base.substr(0,3)==L"com"||base.substr(0,3)==L"lpt")&&base[3]>=L'0'&&base[3]<=L'9'))return false;
        if(at==0&&(Fold(p)==L".update"||Fold(p)==L"current.txt"||Fold(p)==L"1kb.ini"||(Fold(p)==L"1kb.updates.ini"&&p!=L"1kb.updates.ini")))return false;
        if(end==s.npos)break;at=end+1;
    }return true;
}
inline bool Reference(const std::wstring& s){if(s.empty()||s.size()>2048||s.find(L"..")!=s.npos||s.find_first_of(L"\\%?#|\r\n ")!=s.npos)return false;for(auto c:s)if(c<33||c>126)return false;return true;}
struct Manifest {std::wstring version,download,changes,updates=L"background";};
inline bool Valid(const Manifest& m){return RuntimeSupport::ParseVersion(m.version)&&Reference(m.download)&&(m.changes.empty()||Reference(m.changes))&&(m.updates==L"background"||m.updates==L"before-launch"||m.updates==L"restart");}
inline bool Parse(const std::vector<char>& bytes,Manifest& out){
    Manifest m;std::set<std::string> seen;bool ok=RuntimeSupport::ParseKeyValues(bytes,MetadataLimit,[&](const std::string& key,const std::wstring& value){if(!seen.insert(key).second)return false;if(key=="version")m.version=value;else if(key=="download")m.download=value;else if(key=="changes")m.changes=value;else if(key=="updates")m.updates=value;else return false;return true;});
    if(!ok||!seen.count("version")||!seen.count("download")||!Valid(m))return false;out=std::move(m);return true;
}
inline std::wstring Render(const Manifest& m){std::wstring s=L"version="+m.version+L"\ndownload="+m.download+L"\n";if(!m.changes.empty())s+=L"changes="+m.changes+L"\n";if(m.updates!=L"background")s+=L"updates="+m.updates+L"\n";return s;}
inline bool UseChanges(bool installationUsable,const std::wstring& localDownload,const Manifest& remote){return installationUsable&&!remote.changes.empty()&&!localDownload.empty()&&localDownload==remote.download;}
inline bool Newer(const std::wstring& a,const std::wstring& b){RuntimeSupport::Version x,y;if(!RuntimeSupport::ParseVersion(a,&x)||!RuntimeSupport::ParseVersion(b,&y))return false;return x.major!=y.major?x.major>y.major:x.minor!=y.minor?x.minor>y.minor:x.patch>y.patch;}

// Publisher-only inventories. They never appear in the public manifest.
struct File {uint64_t size=0;std::wstring hash;bool operator==(const File& b)const{return size==b.size&&hash==b.hash;}bool operator!=(const File& b)const{return !(*this==b);}};
using Inventory=std::map<std::wstring,File>;
inline bool Hash(const std::wstring& s){if(s.size()!=64)return false;for(auto c:s)if(!((c>=L'0'&&c<=L'9')||(c>=L'a'&&c<=L'f')))return false;return true;}
inline bool CanonicalPrefixes(const std::wstring& path,std::map<std::wstring,std::wstring>& spelling){for(size_t at=path.find(L'/');;at=path.find(L'/',at+1)){auto prefix=path.substr(0,at);auto result=spelling.emplace(Fold(prefix),prefix);if(!result.second&&result.first->second!=prefix)return false;if(at==path.npos)break;}return true;}
inline bool ValidInventory(const Inventory& files,bool allowEmpty=false){if((files.empty()&&!allowEmpty)||files.size()>FileLimit)return false;std::set<std::wstring> names;std::map<std::wstring,std::wstring> spelling;uint64_t total=0;for(auto& f:files){if(!Path(f.first)||!CanonicalPrefixes(f.first,spelling)||!Hash(f.second.hash)||f.second.size>ExtractedLimit-total)return false;total+=f.second.size;if(!names.insert(Fold(f.first)).second)return false;}for(auto& n:names)for(size_t at=n.find(L'/');at!=n.npos;at=n.find(L'/',at+1))if(names.count(n.substr(0,at)))return false;return true;}
inline std::set<std::wstring> AccumulateChanges(const Inventory& snapshot,const Inventory& current,std::set<std::wstring> changed){for(auto& f:snapshot){auto now=current.find(f.first);if(now==current.end()||now->second.hash!=f.second.hash)changed.insert(f.first);}for(auto& f:current){auto old=snapshot.find(f.first);if(old==snapshot.end()||old->second.hash!=f.second.hash)changed.insert(f.first);}return changed;}
inline Inventory ExistingChanges(const Inventory& current,const std::set<std::wstring>& changed){Inventory result;for(auto& path:changed){auto f=current.find(path);if(f!=current.end())result.insert(*f);}return result;}
inline std::set<std::wstring> DeletedChanges(const Inventory& current,const std::set<std::wstring>& changed){std::set<std::wstring> result;for(auto& path:changed)if(!current.count(path))result.insert(path);return result;}
struct Handle {HANDLE h=INVALID_HANDLE_VALUE;explicit Handle(HANDLE v=INVALID_HANDLE_VALUE):h(v){}~Handle(){if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);}Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;operator HANDLE()const{return h;}bool valid()const{return h!=INVALID_HANDLE_VALUE;}};
inline bool ReadAt(HANDLE h,uint64_t at,void* p,DWORD n){LARGE_INTEGER offset;offset.QuadPart=at;DWORD got=0;return SetFilePointerEx(h,offset,nullptr,FILE_BEGIN)&&ReadFile(h,p,n,&got,nullptr)&&got==n;}
inline bool Write(HANDLE h,const void* p,DWORD n){DWORD wrote=0;return WriteFile(h,p,n,&wrote,nullptr)&&wrote==n;}
inline std::wstring Native(const std::wstring& root,const std::wstring& relative){auto s=root+L"\\"+relative;std::replace(s.begin(),s.end(),L'/',L'\\');return s;}
inline DWORD Attributes(const std::wstring& p){return GetFileAttributesW(p.c_str());}
inline bool Missing(const std::wstring& p){if(Attributes(p)!=INVALID_FILE_ATTRIBUTES)return false;return GetLastError()==ERROR_FILE_NOT_FOUND||GetLastError()==ERROR_PATH_NOT_FOUND;}
inline bool Directory(const std::wstring& p){DWORD a=Attributes(p);return a!=INVALID_FILE_ATTRIBUTES&&(a&FILE_ATTRIBUTE_DIRECTORY)&&!(a&FILE_ATTRIBUTE_REPARSE_POINT);}
inline bool Tree(const std::wstring& p){if(Directory(p))return true;if(!Missing(p))return false;auto at=p.find_last_of(L"\\/");return at!=p.npos&&Tree(p.substr(0,at))&&CreateDirectoryW(p.c_str(),nullptr);}
inline bool SafeRoot(const std::wstring& root){if(root.size()<3||root[1]!=L':'||root[2]!=L'\\')return false;for(size_t at=3;;){auto next=root.find(L'\\',at);if(!Directory(next==root.npos?root:root.substr(0,next)))return false;if(next==root.npos)break;at=next+1;}return true;}
inline bool ExactComponent(const std::wstring& path,const std::wstring& wanted,bool directory){WIN32_FIND_DATAW data{};HANDLE search=FindFirstFileW(path.c_str(),&data);if(search==INVALID_HANDLE_VALUE)return false;FindClose(search);return data.cFileName==wanted&&!(data.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)&&(!directory||data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY);}
inline bool Parents(const std::wstring& root,const std::wstring& relative,bool create=false){if(!SafeRoot(root))return false;size_t at=0;while((at=relative.find(L'/',at))!=relative.npos){std::wstring prefix=relative.substr(0,at);size_t slash=prefix.find_last_of(L'/');std::wstring wanted=prefix.substr(slash==prefix.npos?0:slash+1),p=Native(root,prefix);if(Directory(p)){if(!ExactComponent(p,wanted,true))return false;}else if(!create||!Missing(p)||!CreateDirectoryW(p.c_str(),nullptr))return false;++at;}return true;}
inline bool Spelled(const std::wstring& root,const std::wstring& relative){size_t slash=relative.find_last_of(L'/');std::wstring wanted=relative.substr(slash==relative.npos?0:slash+1);return Parents(root,relative)&&ExactComponent(Native(root,relative),wanted,false);}
inline bool Regular(const std::wstring& p){DWORD a=Attributes(p);return a!=INVALID_FILE_ATTRIBUTES&&!(a&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT));}
struct Sha256 {
    BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;std::vector<unsigned char> object;
    Sha256(){DWORD size=0,got=0;if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0&&BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,(PUCHAR)&size,sizeof(size),&got,0)>=0){object.resize(size);BCryptCreateHash(algorithm,&hash,object.data(),size,nullptr,0,0);}}
    ~Sha256(){if(hash)BCryptDestroyHash(hash);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);}
    bool add(const void* p,DWORD n){return hash&&BCryptHashData(hash,(PUCHAR)p,n,0)>=0;}
    bool finish(std::wstring& s){unsigned char digest[32];if(!hash||BCryptFinishHash(hash,digest,32,0)<0)return false;s.clear();for(auto c:digest){s+=L"0123456789abcdef"[c>>4];s+=L"0123456789abcdef"[c&15];}return true;}
};
inline bool Digest(const std::wstring& p,File& result){
    if(!Regular(p))return false;Handle h(CreateFileW(p.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));if(!h.valid())return false;
    BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER size{};if(!GetFileInformationByHandle(h,&info)||(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))||!GetFileSizeEx(h,&size)||size.QuadPart<0)return false;
    Sha256 hash;unsigned char block[65536];uint64_t total=0;for(;;){DWORD got=0;if(!ReadFile(h,block,sizeof(block),&got,nullptr))return false;if(!got)break;if(!hash.add(block,got))return false;total+=got;}
    result.size=total;return total==(uint64_t)size.QuadPart&&hash.finish(result.hash);
}
inline bool Verify(const std::wstring& p,const File& expected){File actual;return Digest(p,actual)&&actual==expected;}
inline bool ReadMetadata(const std::wstring& p,std::vector<char>& bytes){if(!Regular(p))return false;Handle h(CreateFileW(p.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));LARGE_INTEGER size{};BY_HANDLE_FILE_INFORMATION info{};if(!h.valid()||!GetFileInformationByHandle(h,&info)||(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))||!GetFileSizeEx(h,&size)||size.QuadPart<=0||(uint64_t)size.QuadPart>MetadataLimit)return false;bytes.resize((size_t)size.QuadPart);return ReadAt(h,0,bytes.data(),(DWORD)bytes.size());}
inline bool InventoryTree(const std::wstring& root,const std::wstring& relative,Inventory& files,unsigned depth=0){
    if(depth>32||files.size()>FileLimit||!Directory(relative.empty()?root:Native(root,relative)))return false;
    WIN32_FIND_DATAW data{};HANDLE search=FindFirstFileW((Native(root,relative)+L"\\*").c_str(),&data);if(search==INVALID_HANDLE_VALUE)return GetLastError()==ERROR_FILE_NOT_FOUND;
    bool ok=true;do{if(!wcscmp(data.cFileName,L".")||!wcscmp(data.cFileName,L".."))continue;auto name=relative.empty()?std::wstring(data.cFileName):relative+L"/"+data.cFileName;if(!Path(name)||(data.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)){ok=false;break;}if(data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)ok=InventoryTree(root,name,files,depth+1);else{File file;ok=files.size()<FileLimit&&Digest(Native(root,name),file)&&files.emplace(name,file).second;}}while(ok&&FindNextFileW(search,&data));if(ok&&GetLastError()!=ERROR_NO_MORE_FILES)ok=false;FindClose(search);return ok;
}
inline bool InventoryMatches(const std::wstring& root,const Inventory& expected,bool exact){if(exact){Inventory actual;return InventoryTree(root,L"",actual)&&actual==expected;}for(auto& f:expected)if(!Spelled(root,f.first)||!Verify(Native(root,f.first),f.second))return false;return true;}
inline uint16_t U16(const unsigned char* p){return p[0]|uint16_t(p[1])<<8;}
inline uint32_t U32(const unsigned char* p){return U16(p)|uint32_t(U16(p+2))<<16;}
inline uint64_t U64(const unsigned char* p){return U32(p)|uint64_t(U32(p+4))<<32;}
struct ZipEntry {std::wstring name;uint64_t offset=0,compressed=0,size=0;uint32_t crc=0;uint16_t method=0,flags=0;bool directory=false;};
// Parse central and local records ourselves. Never give untrusted ZIPs to a shell extractor.
inline bool ZipDirectory(HANDLE h,std::vector<ZipEntry>& entries,uint64_t& central){
    LARGE_INTEGER length{};if(!GetFileSizeEx(h,&length)||length.QuadPart<22||(uint64_t)length.QuadPart>PackageLimit)return false;
    DWORD tailSize=(DWORD)std::min<uint64_t>(65557,length.QuadPart);std::vector<unsigned char> tail(tailSize);if(!ReadAt(h,length.QuadPart-tailSize,tail.data(),tailSize))return false;
    size_t end=tail.size()-22;for(;;){if(U32(tail.data()+end)==0x06054b50&&end+22+U16(tail.data()+end+20)==tail.size())break;if(!end--)return false;}
    auto p=tail.data()+end;if(U16(p+4)||U16(p+6)||U16(p+8)!=U16(p+10))return false;
    uint64_t count=U16(p+10),centralSize=U32(p+12);central=U32(p+16);uint64_t endOffset=length.QuadPart-tailSize+end;
    if(count==65535||central==UINT32_MAX||centralSize==UINT32_MAX){unsigned char locator[20],record[56];if(endOffset<20||!ReadAt(h,endOffset-20,locator,20)||U32(locator)!=0x07064b50||U32(locator+4)||U32(locator+16)!=1||!ReadAt(h,U64(locator+8),record,56)||U32(record)!=0x06064b50||U64(record+4)!=44||U32(record+16)||U32(record+20)||U64(record+24)!=U64(record+32))return false;count=U64(record+32);centralSize=U64(record+40);central=U64(record+48);endOffset=U64(locator+8);}
    if(count>FileLimit*2||central>endOffset||centralSize!=endOffset-central||centralSize>ZipDirectoryLimit)return false;
    uint64_t at=central,total=0;std::set<std::wstring> names,regularNames;std::map<std::wstring,std::wstring> spelling;
    for(uint64_t i=0;i<count;++i){unsigned char fixed[46];if(at+46>central+centralSize||!ReadAt(h,at,fixed,46)||U32(fixed)!=0x02014b50)return false;ZipEntry e;e.flags=U16(fixed+8);e.method=U16(fixed+10);e.crc=U32(fixed+16);e.compressed=U32(fixed+20);e.size=U32(fixed+24);e.offset=U32(fixed+42);uint16_t n=U16(fixed+28),extra=U16(fixed+30),comment=U16(fixed+32);uint32_t attributes=U32(fixed+38);unsigned type=(attributes>>16)&0170000;
        if(!n||n>221||U16(fixed+34)||(e.flags&~(8|2048|6))||(e.method!=0&&e.method!=8&&e.method!=14)||(type&&type!=0100000&&type!=0040000)||(attributes&FILE_ATTRIBUTE_REPARSE_POINT))return false;
        std::vector<unsigned char> variable(n+extra);if(at+46+n+extra+comment>central+centralSize||!ReadAt(h,at+46,variable.data(),(DWORD)variable.size()))return false;
        if(!RuntimeSupport::Wide((char*)variable.data(),n,e.name))return false;e.directory=e.name.back()==L'/';if(e.directory)e.name.pop_back();if(!Path(e.name)||!CanonicalPrefixes(e.name,spelling)||!names.insert(Fold(e.name)).second)return false;if(!e.directory)regularNames.insert(Fold(e.name));if((type==0040000)!=e.directory&&type)return false;
        bool zip64=false;for(size_t x=n;x<variable.size();){if(x+4>variable.size())return false;auto tag=U16(variable.data()+x),z=U16(variable.data()+x+2);x+=4;if(x+z>variable.size())return false;if(tag==1){if(zip64)return false;zip64=true;size_t off=x;auto take=[&](uint64_t& v){if(v!=UINT32_MAX)return true;if(off+8>x+z)return false;v=U64(variable.data()+off);off+=8;return true;};if(!take(e.size)||!take(e.compressed)||!take(e.offset))return false;}x+=z;}
        if(e.size>ExtractedLimit-total||e.offset>=central||e.compressed>central||(!e.method&&e.size!=e.compressed)||(e.directory&&(e.size||e.compressed||e.crc||e.method)))return false;total+=e.size;entries.push_back(e);at+=46+n+extra+comment;
    }
    if(at!=central+centralSize)return false;std::sort(entries.begin(),entries.end(),[](auto& a,auto& b){return a.offset<b.offset;});
    for(auto& name:names)for(size_t slash=name.find(L'/');slash!=name.npos;slash=name.find(L'/',slash+1))if(regularNames.count(name.substr(0,slash)))return false;
    return true;
}
inline void* LzmaAlloc(ISzAllocPtr,size_t size){return malloc(size);}
inline void LzmaFree(ISzAllocPtr,void* address){free(address);}
template<class Emit> inline bool ExtractLzma(HANDLE input,uint64_t data,const ZipEntry& e,Emit emit,uint64_t& produced,uLong& crc,bool (*cancelled)()){
    if(e.compressed<4+LZMA_PROPS_SIZE)return false;unsigned char header[4+LZMA_PROPS_SIZE];if(!ReadAt(input,data,header,sizeof(header))||U16(header+2)!=LZMA_PROPS_SIZE||U32(header+5)>64u*1024*1024)return false;
    ISzAlloc allocator{LzmaAlloc,LzmaFree};CLzmaDec decoder;LzmaDec_Construct(&decoder);if(LzmaDec_Allocate(&decoder,header+4,LZMA_PROPS_SIZE,&allocator)!=SZ_OK)return false;LzmaDec_Init(&decoder);
    unsigned char in[65536+LZMA_REQUIRED_INPUT_MAX],out[65536];size_t buffered=0;uint64_t loaded=sizeof(header),consumed=sizeof(header);bool ok=true,finished=false;
    while(ok&&!finished){
        if(cancelled&&cancelled()){ok=false;break;}
        if(buffered<LZMA_REQUIRED_INPUT_MAX&&loaded<e.compressed){DWORD count=(DWORD)std::min<uint64_t>(sizeof(in)-buffered,e.compressed-loaded);if(!ReadAt(input,data+loaded,in+buffered,count)){ok=false;break;}buffered+=count;loaded+=count;}
        SizeT source=buffered,destination=(SizeT)std::min<uint64_t>(sizeof(out),e.size-produced);ELzmaStatus status=LZMA_STATUS_NOT_SPECIFIED;SRes result=LzmaDec_DecodeToBuf(&decoder,out,&destination,in,&source,destination==(SizeT)(e.size-produced)?LZMA_FINISH_END:LZMA_FINISH_ANY,&status);if(result!=SZ_OK||source>buffered||produced+destination>e.size){ok=false;break;}
        if(source){memmove(in,in+source,buffered-source);buffered-=source;consumed+=source;}if(destination){if(!emit(out,(DWORD)destination)){ok=false;break;}produced+=destination;crc=crc32(crc,out,(uInt)destination);}
        if(status==LZMA_STATUS_FINISHED_WITH_MARK){finished=true;ok=produced==e.size;}else if(produced==e.size&&status==LZMA_STATUS_MAYBE_FINISHED_WITHOUT_MARK){finished=true;ok=(e.flags&2)==0;}else if(!source&&!destination){if(loaded==e.compressed||buffered>=LZMA_REQUIRED_INPUT_MAX)ok=false;}
    }
    LzmaDec_Free(&decoder,&allocator);return ok&&finished&&consumed==e.compressed&&!buffered;
}
inline bool Extract(const std::wstring& archive,const std::wstring& destination,const Inventory* expected=nullptr,const Inventory* retain=nullptr,bool (*cancelled)()=nullptr){
    if(retain&&!expected)return false;
    if(!Directory(destination))return false;Handle input(CreateFileW(archive.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));std::vector<ZipEntry> entries;uint64_t central=0;if(!input.valid()||!Regular(archive)||!ZipDirectory(input,entries,central))return false;
    Inventory declared;for(auto& e:entries)if(!e.directory){if(expected){auto f=expected->find(e.name);if(f==expected->end()||f->second.size!=e.size)return false;declared.insert(*f);}else declared.emplace(e.name,File{e.size,std::wstring(64,L'0')});}if(expected&&declared!=*expected)return false;if(!declared.empty()&&!ValidInventory(declared))return false;
    uint64_t previousEnd=0;for(size_t index=0;index<entries.size();++index){if(cancelled&&cancelled())return false;auto& e=entries[index];unsigned char local[30];if(e.offset!=previousEnd||!ReadAt(input,e.offset,local,30)||U32(local)!=0x04034b50||U16(local+6)!=e.flags||U16(local+8)!=e.method)return false;auto n=U16(local+26),extra=U16(local+28);std::vector<char> name(n);if(n>221||!ReadAt(input,e.offset+30,name.data(),n))return false;std::wstring wide;if(!RuntimeSupport::Wide(name.data(),name.size(),wide)||wide!=e.name+(e.directory?L"/":L""))return false;
        uint64_t data=e.offset+30+n+extra;if(data>central||e.compressed>central-data)return false;previousEnd=data+e.compressed;
        if(e.flags&8){unsigned char descriptor[24]{};DWORD available=(DWORD)std::min<uint64_t>(24,central-previousEnd);if(available<12||!ReadAt(input,previousEnd,descriptor,available))return false;size_t d=U32(descriptor)==0x08074b50?4:0;if(U32(descriptor+d)!=e.crc)return false;bool large=U32(local+18)==UINT32_MAX||U32(local+22)==UINT32_MAX;if(large){if(available<d+20||U64(descriptor+d+4)!=e.compressed||U64(descriptor+d+12)!=e.size)return false;previousEnd+=d+20;}else{if(available<d+12||U32(descriptor+d+4)!=e.compressed||U32(descriptor+d+8)!=e.size)return false;previousEnd+=d+12;}}
        else if(U32(local+14)!=e.crc||(U32(local+18)!=UINT32_MAX&&U32(local+18)!=e.compressed)||(U32(local+22)!=UINT32_MAX&&U32(local+22)!=e.size))return false;
        if(previousEnd!=(index+1<entries.size()?entries[index+1].offset:central))return false;
        bool keep=!retain||retain->count(e.name)!=0;if(e.directory){if(retain)continue;if(!Parents(destination,e.name,true))return false;auto path=Native(destination,e.name);if(!Directory(path)&&(!Missing(path)||!CreateDirectoryW(path.c_str(),nullptr)))return false;continue;}
        if(keep&&!Parents(destination,e.name,true))return false;auto outputPath=Native(destination,e.name);
        Handle output(keep?CreateFileW(outputPath.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_FLAG_SEQUENTIAL_SCAN,nullptr):INVALID_HANDLE_VALUE);if(keep&&!output.valid())return false;
        Sha256 hash;auto emit=[&](const unsigned char* bytes,DWORD count){return (!expected||hash.add(bytes,count))&&(!keep||Write(output,bytes,count));};
        unsigned char in[65536],out[65536];uint64_t consumed=0,produced=0;uLong crc=crc32(0,nullptr,0);z_stream stream{};bool ok=true,ended=e.method==0;if(e.method==8&&inflateInit2(&stream,-MAX_WBITS)!=Z_OK)return false;
        if(e.method==14){ok=ExtractLzma(input,data,e,emit,produced,crc,cancelled);ended=ok;}else while(ok&&consumed<e.compressed){if(cancelled&&cancelled()){ok=false;break;}DWORD nread=(DWORD)std::min<uint64_t>(sizeof(in),e.compressed-consumed);if(!ReadAt(input,data+consumed,in,nread)){ok=false;break;}consumed+=nread;if(!e.method){if(produced+nread>e.size||!emit(in,nread)){ok=false;break;}produced+=nread;crc=crc32(crc,in,nread);}else{stream.next_in=in;stream.avail_in=nread;do{stream.next_out=out;stream.avail_out=sizeof(out);int result=inflate(&stream,Z_NO_FLUSH);DWORD count=sizeof(out)-stream.avail_out;if(produced+count>e.size||!emit(out,count)){ok=false;break;}produced+=count;crc=crc32(crc,out,count);if(result==Z_STREAM_END){ended=true;if(stream.avail_in||consumed!=e.compressed)ok=false;break;}if(result==Z_BUF_ERROR&&!stream.avail_in&&!count&&consumed<e.compressed)break;if(result!=Z_OK){ok=false;break;}}while(stream.avail_in||!stream.avail_out);}}
        if(e.method==8)inflateEnd(&stream);std::wstring digest;if(!ok||!ended||produced!=e.size||crc!=e.crc||(expected&&(!hash.finish(digest)||digest!=expected->at(e.name).hash)))return false;
    }
    return !expected||InventoryMatches(destination,retain?*retain:*expected,true);
}
inline void Put16(std::vector<unsigned char>& b,uint16_t n){b.push_back((unsigned char)n);b.push_back((unsigned char)(n>>8));}
inline void Put32(std::vector<unsigned char>& b,uint32_t n){Put16(b,(uint16_t)n);Put16(b,(uint16_t)(n>>16));}
inline void Put64(std::vector<unsigned char>& b,uint64_t n){Put32(b,(uint32_t)n);Put32(b,(uint32_t)(n>>32));}
inline bool CreateZip(const std::wstring& root,const Inventory& files,const std::wstring& destination){
    Handle output(CreateFileW(destination.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_FLAG_SEQUENTIAL_SCAN,nullptr));if(!output.valid())return false;
    std::vector<ZipEntry> entries;uint64_t offset=0;
    for(auto& f:files){if(!Parents(root,f.first)||!Verify(Native(root,f.first),f.second))return false;std::string name;if(!RuntimeSupport::Utf8(f.first,name))return false;
        ZipEntry e;e.name=f.first;e.offset=offset;e.size=f.second.size;e.method=8;e.flags=8|2048;
        std::vector<unsigned char> header;Put32(header,0x04034b50);Put16(header,45);Put16(header,e.flags);Put16(header,8);Put32(header,0);Put32(header,0);Put32(header,UINT32_MAX);Put32(header,UINT32_MAX);Put16(header,(uint16_t)name.size());Put16(header,20);header.insert(header.end(),name.begin(),name.end());Put16(header,1);Put16(header,16);Put64(header,e.size);Put64(header,0);if(!Write(output,header.data(),(DWORD)header.size()))return false;offset+=header.size();
        Handle input(CreateFileW(Native(root,f.first).c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr));if(!input.valid())return false;z_stream stream{};if(deflateInit2(&stream,Z_DEFAULT_COMPRESSION,Z_DEFLATED,-MAX_WBITS,8,Z_DEFAULT_STRATEGY)!=Z_OK)return false;
        unsigned char in[65536],out[65536];uint64_t read=0;bool ok=true;uLong crc=crc32(0,nullptr,0);int result=Z_OK;
        while(ok&&result!=Z_STREAM_END){DWORD got=0;if(!ReadFile(input,in,sizeof(in),&got,nullptr)){ok=false;break;}read+=got;crc=crc32(crc,in,got);stream.next_in=in;stream.avail_in=got;do{stream.next_out=out;stream.avail_out=sizeof(out);result=deflate(&stream,got?Z_NO_FLUSH:Z_FINISH);DWORD count=sizeof(out)-stream.avail_out;if(result<0||!Write(output,out,count)){ok=false;break;}e.compressed+=count;offset+=count;if(offset>PackageLimit){ok=false;break;}}while(stream.avail_in||!stream.avail_out||(got==0&&result!=Z_STREAM_END));}
        deflateEnd(&stream);if(!ok||read!=e.size)return false;e.crc=(uint32_t)crc;std::vector<unsigned char> descriptor;Put32(descriptor,0x08074b50);Put32(descriptor,e.crc);Put64(descriptor,e.compressed);Put64(descriptor,e.size);if(!Write(output,descriptor.data(),(DWORD)descriptor.size()))return false;offset+=descriptor.size();entries.push_back(e);
    }
    uint64_t central=offset;for(auto& e:entries){std::string name;RuntimeSupport::Utf8(e.name,name);std::vector<unsigned char> b;Put32(b,0x02014b50);Put16(b,45);Put16(b,45);Put16(b,e.flags);Put16(b,8);Put32(b,0);Put32(b,e.crc);Put32(b,UINT32_MAX);Put32(b,UINT32_MAX);Put16(b,(uint16_t)name.size());Put16(b,28);Put16(b,0);Put16(b,0);Put16(b,0);Put32(b,0);Put32(b,UINT32_MAX);b.insert(b.end(),name.begin(),name.end());Put16(b,1);Put16(b,24);Put64(b,e.size);Put64(b,e.compressed);Put64(b,e.offset);if(!Write(output,b.data(),(DWORD)b.size()))return false;offset+=b.size();}
    std::vector<unsigned char> end;Put32(end,0x06054b50);Put16(end,0);Put16(end,0);Put16(end,(uint16_t)entries.size());Put16(end,(uint16_t)entries.size());Put32(end,(uint32_t)(offset-central));Put32(end,(uint32_t)central);Put16(end,0);return offset+end.size()<=PackageLimit&&Write(output,end.data(),(DWORD)end.size())&&FlushFileBuffers(output);
}
using PackageWrite=bool (*)(void*,const unsigned char*,unsigned);
inline uint16_t ZipVersion(const ZipEntry& e){return e.method==14?63:(e.size>UINT32_MAX?45:20);}
inline uint16_t ZipFlags(const ZipEntry& e){return uint16_t(2048|(e.method==14?(e.flags&2):0));}
inline bool ZipPayloads(const std::wstring& archive,Handle& input,std::vector<ZipEntry>& files,uint64_t& packageBytes){
    input.h=CreateFileW(archive.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);std::vector<ZipEntry> entries;uint64_t central=0;if(!input.valid()||!Regular(archive)||!ZipDirectory(input,entries,central))return false;packageBytes=22;
    for(auto e:entries){unsigned char local[30];if(!ReadAt(input,e.offset,local,sizeof(local))||U32(local)!=0x04034b50||U16(local+6)!=e.flags||U16(local+8)!=e.method)return false;uint16_t name=U16(local+26),extra=U16(local+28);std::vector<char> bytes(name);std::wstring wide;if(!name||name>221||!ReadAt(input,e.offset+30,bytes.data(),name)||!RuntimeSupport::Wide(bytes.data(),bytes.size(),wide)||wide!=e.name+(e.directory?L"/":L""))return false;uint64_t data=e.offset+30ull+name+extra;if(data>central||e.compressed>central-data)return false;
        if(!(e.flags&8)&&(U32(local+14)!=e.crc||(U32(local+18)!=UINT32_MAX&&U32(local+18)!=e.compressed)||(U32(local+22)!=UINT32_MAX&&U32(local+22)!=e.size)))return false;if(e.directory)continue;e.offset=data;std::string utf8;if(!RuntimeSupport::Utf8(e.name,utf8)||utf8.empty()||utf8.size()>221)return false;uint64_t x=e.size>UINT32_MAX?12:0,addition=30+utf8.size()+x+e.compressed+46+utf8.size()+x;if(addition>PackageLimit||packageBytes>PackageLimit-addition)return false;packageBytes+=addition;files.push_back(e);
    }return !files.empty()&&files.size()<=FileLimit;
}
inline bool PackageFromZip(const std::wstring& archive,PackageWrite write,void* context,uint64_t* returnedSize=nullptr){
    Handle input;std::vector<ZipEntry> files;uint64_t size=0;if(!write||!ZipPayloads(archive,input,files,size))return false;if(returnedSize)*returnedSize=size;uint64_t offset=0;unsigned char block[65536];
    for(auto& e:files){std::string name;if(!RuntimeSupport::Utf8(e.name,name))return false;uint64_t localOffset=offset;std::vector<unsigned char> b;Put32(b,0x04034b50);Put16(b,ZipVersion(e));Put16(b,ZipFlags(e));Put16(b,e.method);Put32(b,0);Put32(b,e.crc);Put32(b,(uint32_t)e.compressed);Put32(b,e.size>UINT32_MAX?UINT32_MAX:(uint32_t)e.size);Put16(b,(uint16_t)name.size());Put16(b,e.size>UINT32_MAX?12:0);b.insert(b.end(),name.begin(),name.end());if(e.size>UINT32_MAX){Put16(b,1);Put16(b,8);Put64(b,e.size);}if(!write(context,b.data(),(unsigned)b.size()))return false;offset+=b.size();uint64_t copied=0;while(copied<e.compressed){DWORD count=(DWORD)std::min<uint64_t>(sizeof(block),e.compressed-copied);if(!ReadAt(input,e.offset+copied,block,count)||!write(context,block,count))return false;copied+=count;offset+=count;}e.offset=localOffset;
    }
    uint64_t central=offset;for(auto& e:files){std::string name;if(!RuntimeSupport::Utf8(e.name,name))return false;std::vector<unsigned char> b;Put32(b,0x02014b50);Put16(b,ZipVersion(e));Put16(b,ZipVersion(e));Put16(b,ZipFlags(e));Put16(b,e.method);Put32(b,0);Put32(b,e.crc);Put32(b,(uint32_t)e.compressed);Put32(b,e.size>UINT32_MAX?UINT32_MAX:(uint32_t)e.size);Put16(b,(uint16_t)name.size());Put16(b,e.size>UINT32_MAX?12:0);Put16(b,0);Put16(b,0);Put16(b,0);Put32(b,0);Put32(b,(uint32_t)e.offset);b.insert(b.end(),name.begin(),name.end());if(e.size>UINT32_MAX){Put16(b,1);Put16(b,8);Put64(b,e.size);}if(!write(context,b.data(),(unsigned)b.size()))return false;offset+=b.size();}
    std::vector<unsigned char> end;Put32(end,0x06054b50);Put16(end,0);Put16(end,0);Put16(end,(uint16_t)files.size());Put16(end,(uint16_t)files.size());Put32(end,(uint32_t)(offset-central));Put32(end,(uint32_t)central);Put16(end,0);return offset+end.size()==size&&write(context,end.data(),(unsigned)end.size());
}
inline bool PackageSizeFromZip(const std::wstring& archive,uint64_t& size){Handle input;std::vector<ZipEntry> files;return ZipPayloads(archive,input,files,size);}
inline bool FilePackageWrite(void* context,const unsigned char* bytes,unsigned count){return Write(*(HANDLE*)context,bytes,count);}
inline bool CreatePackageFromZip(const std::wstring& archive,const std::wstring& destination){Handle output(CreateFileW(destination.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_FLAG_SEQUENTIAL_SCAN,nullptr));bool ok=output.valid()&&PackageFromZip(archive,FilePackageWrite,&output.h)&&FlushFileBuffers(output);if(!ok){if(output.valid()){CloseHandle(output.h);output.h=INVALID_HANDLE_VALUE;}DeleteFileW(destination.c_str());}return ok;}

struct StreamExtractor {
    std::wstring root;std::vector<unsigned char> metadata,out,lzmaHeader;std::vector<ZipEntry> entries;std::set<std::wstring> names,prefixes;std::map<std::wstring,std::wstring> spelling;HANDLE output=INVALID_HANDLE_VALUE;ZipEntry entry{};uint64_t received=0,produced=0,total=0,position=0,recordOffset=0,centralOffset=0;uint32_t crc=0;uint16_t pathBytes=0,extraBytes=0,commentBytes=0;size_t centralIndex=0;int stage=0;bool ok=true,ended=false,centralStarted=false,lzmaAllocated=false;z_stream deflate{};CLzmaDec lzma{};ELzmaStatus lzmaStatus=LZMA_STATUS_NOT_SPECIFIED;ISzAlloc allocator{LzmaAlloc,LzmaFree};
    explicit StreamExtractor(const std::wstring& destination):root(destination),out(65536){ok=Directory(root);LzmaDec_Construct(&lzma);}
    ~StreamExtractor(){closeEntry();}
    void closeEntry(){if(output!=INVALID_HANDLE_VALUE){CloseHandle(output);output=INVALID_HANDLE_VALUE;}if(entry.method==8&&deflate.state)inflateEnd(&deflate);if(lzmaAllocated){LzmaDec_Free(&lzma,&allocator);lzmaAllocated=false;}}
    bool emit(const unsigned char* bytes,unsigned n){if(produced+n>entry.size||!Write(output,bytes,n))return false;produced+=n;crc=(uint32_t)crc32(crc,bytes,n);return true;}
    bool parseExtra(const unsigned char* p,unsigned n,uint64_t& size){if(size<=UINT32_MAX)return n==0;if(n!=12||U16(p)!=1||U16(p+2)!=8)return false;size=U64(p+4);return size>UINT32_MAX;}
    bool beginEntry(){std::wstring path;if(!RuntimeSupport::Wide((char*)metadata.data(),pathBytes,path)||!Path(path)||!CanonicalPrefixes(path,spelling)||!parseExtra(metadata.data()+pathBytes,extraBytes,entry.size))return false;auto folded=Fold(path);if(!names.insert(folded).second||prefixes.count(folded))return false;for(size_t slash=folded.find(L'/');slash!=folded.npos;slash=folded.find(L'/',slash+1)){auto prefix=folded.substr(0,slash);if(names.count(prefix))return false;prefixes.insert(prefix);}if((!entry.method&&entry.compressed!=entry.size)||entry.size>ExtractedLimit-total||!Parents(root,path,true))return false;total+=entry.size;entry.name=path;output=CreateFileW(Native(root,path).c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_FLAG_SEQUENTIAL_SCAN,nullptr);if(output==INVALID_HANDLE_VALUE)return false;received=produced=0;crc=(uint32_t)crc32(0,nullptr,0);ended=entry.method==0;lzmaStatus=LZMA_STATUS_NOT_SPECIFIED;lzmaHeader.clear();if(entry.method==8&&inflateInit2(&deflate,-MAX_WBITS)!=Z_OK)return false;metadata.clear();stage=3;return true;}
    bool compressedBytes(const unsigned char* bytes,unsigned n){received+=n;if(received>entry.compressed)return false;if(entry.method==0)return emit(bytes,n);if(entry.method==8){deflate.next_in=(Bytef*)bytes;deflate.avail_in=n;while(deflate.avail_in){deflate.next_out=out.data();deflate.avail_out=(uInt)out.size();int result=inflate(&deflate,Z_NO_FLUSH);unsigned made=(unsigned)out.size()-deflate.avail_out;if(made&&!emit(out.data(),made))return false;if(result==Z_STREAM_END){ended=true;return !deflate.avail_in&&received==entry.compressed;}if(result!=Z_OK||(deflate.avail_in&&deflate.avail_out&&!made))return false;}return true;}
        if(lzmaHeader.size()<4+LZMA_PROPS_SIZE){unsigned take=std::min<unsigned>(n,4+LZMA_PROPS_SIZE-(unsigned)lzmaHeader.size());lzmaHeader.insert(lzmaHeader.end(),bytes,bytes+take);bytes+=take;n-=take;if(lzmaHeader.size()==4+LZMA_PROPS_SIZE){if(U16(lzmaHeader.data()+2)!=LZMA_PROPS_SIZE||U32(lzmaHeader.data()+5)>64u*1024*1024||LzmaDec_Allocate(&lzma,lzmaHeader.data()+4,LZMA_PROPS_SIZE,&allocator)!=SZ_OK)return false;lzmaAllocated=true;LzmaDec_Init(&lzma);}else return true;}
        while(n){SizeT source=n,destination=(SizeT)std::min<uint64_t>(out.size(),entry.size-produced);SRes result=LzmaDec_DecodeToBuf(&lzma,out.data(),&destination,bytes,&source,destination==(SizeT)(entry.size-produced)?LZMA_FINISH_END:LZMA_FINISH_ANY,&lzmaStatus);if(result!=SZ_OK||source>n||(!source&&!destination)||(destination&&!emit(out.data(),(unsigned)destination)))return false;bytes+=source;n-=(unsigned)source;if(lzmaStatus==LZMA_STATUS_FINISHED_WITH_MARK){ended=true;if(n||received!=entry.compressed)return false;}}return true;
    }
    bool finishEntry(){bool valid=received==entry.compressed&&produced==entry.size&&crc==entry.crc;if(entry.method==8)valid=valid&&ended;if(entry.method==14)valid=valid&&lzmaAllocated&&(lzmaStatus==LZMA_STATUS_FINISHED_WITH_MARK||(lzmaStatus==LZMA_STATUS_MAYBE_FINISHED_WITHOUT_MARK&&!(entry.flags&2)));closeEntry();if(valid)entries.push_back(entry);metadata.clear();stage=0;return valid;}
    bool localFixed(){entry=ZipEntry{};uint16_t version=U16(metadata.data()),time=U16(metadata.data()+6),date=U16(metadata.data()+8);entry.flags=U16(metadata.data()+2);entry.method=U16(metadata.data()+4);entry.crc=U32(metadata.data()+10);entry.compressed=U32(metadata.data()+14);entry.size=U32(metadata.data()+18);pathBytes=U16(metadata.data()+22);extraBytes=U16(metadata.data()+24);entry.offset=recordOffset;uint16_t wanted=entry.method==14?63:(entry.size==UINT32_MAX?45:20);if(version!=wanted||time||date||!pathBytes||pathBytes>221||(entry.method!=0&&entry.method!=8&&entry.method!=14)||entry.flags!=uint16_t(2048|(entry.method==14?(entry.flags&2):0))||(entry.size==UINT32_MAX)!=(extraBytes==12)||entry.compressed>PackageLimit||(!entry.method&&entry.size!=UINT32_MAX&&entry.compressed!=entry.size))return false;metadata.clear();stage=2;return true;}
    bool centralFixed(){if(centralIndex>=entries.size())return false;entry=ZipEntry{};uint16_t made=U16(metadata.data()),needed=U16(metadata.data()+2),time=U16(metadata.data()+8),date=U16(metadata.data()+10);entry.flags=U16(metadata.data()+4);entry.method=U16(metadata.data()+6);entry.crc=U32(metadata.data()+12);entry.compressed=U32(metadata.data()+16);entry.size=U32(metadata.data()+20);pathBytes=U16(metadata.data()+24);extraBytes=U16(metadata.data()+26);commentBytes=U16(metadata.data()+28);entry.offset=U32(metadata.data()+38);uint16_t wanted=entry.method==14?63:(entry.size==UINT32_MAX?45:20);if(made!=wanted||needed!=wanted||time||date||U16(metadata.data()+30)||U16(metadata.data()+32)||U32(metadata.data()+34)||!pathBytes||pathBytes>221||commentBytes||(entry.size==UINT32_MAX)!=(extraBytes==12))return false;metadata.clear();stage=5;return true;}
    bool finishCentral(){std::wstring path;if(!RuntimeSupport::Wide((char*)metadata.data(),pathBytes,path)||!parseExtra(metadata.data()+pathBytes,extraBytes,entry.size))return false;auto& local=entries[centralIndex++];if(path!=local.name||entry.offset!=local.offset||entry.flags!=local.flags||entry.method!=local.method||entry.crc!=local.crc||entry.compressed!=local.compressed||entry.size!=local.size)return false;metadata.clear();stage=0;return true;}
    bool eocd(){uint64_t centralSize=recordOffset-centralOffset;bool valid=!U16(metadata.data())&&!U16(metadata.data()+2)&&U16(metadata.data()+4)==entries.size()&&U16(metadata.data()+6)==entries.size()&&U32(metadata.data()+8)==centralSize&&U32(metadata.data()+12)==centralOffset&&!U16(metadata.data()+16);metadata.clear();stage=7;return valid;}
    bool consume(const unsigned char* bytes,unsigned n){if(!ok||(!bytes&&n)||stage==7&&n)return ok=false;while(n&&ok){if(stage==3){unsigned take=(unsigned)std::min<uint64_t>(n,entry.compressed-received);if(!compressedBytes(bytes,take))return ok=false;bytes+=take;n-=take;position+=take;if(received==entry.compressed&&!finishEntry())return ok=false;continue;}unsigned wanted=stage==0?4:stage==1?26:stage==2?pathBytes+extraBytes:stage==4?42:stage==5?pathBytes+extraBytes+commentBytes:18;unsigned take=std::min<unsigned>(n,wanted-(unsigned)metadata.size());if(stage==0&&metadata.empty())recordOffset=position;metadata.insert(metadata.end(),bytes,bytes+take);bytes+=take;n-=take;position+=take;if(metadata.size()!=wanted)continue;if(stage==0){uint32_t signature=U32(metadata.data());metadata.clear();if(signature==0x04034b50){if(centralStarted||entries.size()>=FileLimit)return ok=false;stage=1;}else if(signature==0x02014b50){if(entries.empty()){return ok=false;}if(!centralStarted){centralStarted=true;centralOffset=recordOffset;}stage=4;}else if(signature==0x06054b50){if(!centralStarted||centralIndex!=entries.size())return ok=false;stage=6;}else return ok=false;}else if(stage==1){if(!localFixed())return ok=false;}else if(stage==2){if(!beginEntry())return ok=false;if(!entry.compressed&&!finishEntry())return ok=false;}else if(stage==4){if(!centralFixed())return ok=false;}else if(stage==5){if(!finishCentral())return ok=false;}else if(!eocd())return ok=false;}return ok;}
    bool finish(){return ok&&stage==7&&centralIndex==entries.size();}
};
inline bool Move(const std::wstring& from,const std::wstring& to,bool replace=false){return MoveFileExW(from.c_str(),to.c_str(),MOVEFILE_WRITE_THROUGH|(replace?MOVEFILE_REPLACE_EXISTING:0))!=FALSE;}
inline void EmptyParents(const std::wstring& root,const std::wstring& relative){auto at=relative.find_last_of(L'/');while(at!=relative.npos){RemoveDirectoryW(Native(root,relative.substr(0,at)).c_str());if(!at)break;at=relative.find_last_of(L'/',at-1);}}
inline bool RemoveObject(const std::wstring& path){DWORD a=Attributes(path);if(a==INVALID_FILE_ATTRIBUTES)return Missing(path);if(a&FILE_ATTRIBUTE_DIRECTORY)return RuntimeSupport::DeleteTree(path);SetFileAttributesW(path.c_str(),FILE_ATTRIBUTE_NORMAL);return DeleteFileW(path.c_str())!=FALSE;}
inline bool ReadUpdateInstructions(const std::wstring& root,std::set<std::wstring>& deleted){
    auto path=root+L"\\1kb.updates.ini";if(Missing(path))return true;std::vector<char> bytes;if(!ReadMetadata(path,bytes))return false;std::set<std::wstring> folded;bool ok=RuntimeSupport::ParseKeyValues(bytes,MetadataLimit,[&](const std::string& key,const std::wstring& value){return key=="delete"&&Path(value)&&Fold(value)!=L"1kb.updates.ini"&&folded.insert(Fold(value)).second&&deleted.insert(value).second;});return ok&&DeleteFileW(path.c_str());
}
inline bool EnsureOverlayParents(const std::wstring& root,const std::wstring& relative){size_t at=0;while((at=relative.find(L'/',at))!=relative.npos){auto path=Native(root,relative.substr(0,at));if(!Directory(path)){if(!Missing(path)&&!RemoveObject(path))return false;if(!CreateDirectoryW(path.c_str(),nullptr)&&GetLastError()!=ERROR_ALREADY_EXISTS)return false;}++at;}return true;}
inline bool CollectFiles(const std::wstring& root,const std::wstring& relative,std::vector<std::wstring>& files,unsigned depth=0){
    if(depth>32||files.size()>FileLimit||!Directory(relative.empty()?root:Native(root,relative)))return false;WIN32_FIND_DATAW data{};HANDLE search=FindFirstFileW((Native(root,relative)+L"\\*").c_str(),&data);if(search==INVALID_HANDLE_VALUE)return GetLastError()==ERROR_FILE_NOT_FOUND;bool ok=true;do{if(!wcscmp(data.cFileName,L".")||!wcscmp(data.cFileName,L".."))continue;auto name=relative.empty()?std::wstring(data.cFileName):relative+L"/"+data.cFileName;if(!Path(name)||(data.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)){ok=false;break;}if(data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)ok=CollectFiles(root,name,files,depth+1);else if(files.size()<FileLimit)files.push_back(name);else ok=false;}while(ok&&FindNextFileW(search,&data));if(ok&&GetLastError()!=ERROR_NO_MORE_FILES)ok=false;FindClose(search);return ok;
}
inline bool Overlay(const std::wstring& destination,const std::wstring& prepared,const std::set<std::wstring>& deleted){
    std::vector<std::wstring> files;if(!CollectFiles(prepared,L"",files))return false;for(auto& path:deleted){auto target=Native(destination,path);if(!RemoveObject(target))return false;EmptyParents(destination,path);}
    for(auto& path:files){auto target=Native(destination,path);if(!EnsureOverlayParents(destination,path)||(!Missing(target)&&!RemoveObject(target))||!Move(Native(prepared,path),target))return false;}return true;
}
inline bool WriteCurrent(const std::wstring& root,const std::wstring& version){return RuntimeSupport::AtomicWriteUtf8(root+L"\\current.txt",version+L"\n");}
inline bool InstallIncremental(const std::wstring& root,const std::wstring& from,const std::wstring& to,const std::wstring& prepared,const std::set<std::wstring>& deleted){
    auto old=root+L"\\"+from,target=root+L"\\"+to;if(!Directory(old)||!Missing(target)||!Move(old,target))return false;return Overlay(target,prepared,deleted)&&WriteCurrent(root,to);
}
inline bool InstallFull(const std::wstring& root,const std::wstring& version,const std::wstring& preparedSnapshot,const std::wstring* preparedChanges,const std::set<std::wstring>& deleted){
    auto target=root+L"\\"+version;if(!Missing(target)&&!RuntimeSupport::DeleteTree(target))return false;if(!Move(preparedSnapshot,target))return false;if(preparedChanges&&!Overlay(target,*preparedChanges,deleted))return false;return WriteCurrent(root,version);
}
} // namespace Updates
