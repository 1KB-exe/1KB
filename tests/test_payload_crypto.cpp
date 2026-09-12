#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <vector>
#include <string>
#include <cstdio>
#include <algorithm>
#include "../src/update-storage.h"
#include "../src/payload-crypto.h"

static bool FileBytes(const std::wstring& path,std::vector<unsigned char>& bytes){
    Updates::Handle file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr));LARGE_INTEGER size{};if(!file.valid()||!GetFileSizeEx(file,&size)||size.QuadPart<0||size.QuadPart>MAXDWORD)return false;bytes.resize((size_t)size.QuadPart);return bytes.empty()||Updates::ReadAt(file,0,bytes.data(),(DWORD)bytes.size());
}
static bool SameBytes(const std::wstring& path,const std::vector<unsigned char>& expected){std::vector<unsigned char> actual;return FileBytes(path,actual)&&actual==expected;}
static bool WriteBytes(const std::wstring& path,const std::vector<unsigned char>& bytes){Updates::Handle file(CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_FLAG_SEQUENTIAL_SCAN,nullptr));return file.valid()&&(bytes.empty()||Updates::Write(file,bytes.data(),(DWORD)bytes.size()));}

// Independently check the normalized shape expected by ordinary ZIP readers.
static bool StandardZipShape(const std::wstring& path,unsigned expectedFiles){
    std::vector<unsigned char> b;if(!FileBytes(path,b)||b.size()<22)return false;size_t end=b.size()-22;if(Updates::U32(b.data()+end)!=0x06054b50||Updates::U16(b.data()+end+20))return false;unsigned count=Updates::U16(b.data()+end+10);uint32_t central=Updates::U32(b.data()+end+16),centralSize=Updates::U32(b.data()+end+12);if(count!=expectedFiles||central+centralSize!=end)return false;
    size_t at=0;for(unsigned i=0;i<count;++i){if(at+30>central||Updates::U32(b.data()+at)!=0x04034b50)return false;uint16_t flags=Updates::U16(b.data()+at+6),name=Updates::U16(b.data()+at+26),extra=Updates::U16(b.data()+at+28);uint32_t compressed=Updates::U32(b.data()+at+18);if((flags&8)||!name||compressed==UINT32_MAX||at+30ull+name+extra+compressed>central)return false;at+=30ull+name+extra+compressed;}if(at!=central)return false;
    at=central;for(unsigned i=0;i<count;++i){if(at+46>end||Updates::U32(b.data()+at)!=0x02014b50)return false;uint16_t name=Updates::U16(b.data()+at+28),extra=Updates::U16(b.data()+at+30),comment=Updates::U16(b.data()+at+32);if(!name||comment||at+46ull+name+extra>end)return false;at+=46ull+name+extra;}return at==end;
}
static bool ProduceZip(void* context,PayloadBytes write,void* writeContext){return Updates::PackageFromZip(*(const std::wstring*)context,(Updates::PackageWrite)write,writeContext);}
static bool ExtractBytes(void* context,const uint8_t* bytes,unsigned count){return ((Updates::StreamExtractor*)context)->consume(bytes,count);}
static bool Feed(PayloadDecryptStream& decrypt,const std::vector<unsigned char>& bytes){
    static const unsigned chunks[]={1,7,31,32768,113,6553};size_t at=0,index=0;while(at<bytes.size()){unsigned n=(unsigned)std::min<size_t>(chunks[index++%_countof(chunks)],bytes.size()-at);if(!DecryptPayloadBytes(decrypt,bytes.data()+at,n))return false;at+=n;}return true;
}
int wmain(){
    wchar_t temp[MAX_PATH]{};if(!GetTempPathW(_countof(temp),temp))return 1;std::wstring root=std::wstring(temp)+L"1kb-package-roundtrip-"+std::to_wstring(GetCurrentProcessId());RuntimeSupport::DeleteTree(root);bool ok=Updates::Tree(root+L"\\source\\nested");
    std::vector<unsigned char> app(100000),data(777);for(size_t i=0;i<app.size();++i)app[i]=(unsigned char)(i*37);for(size_t i=0;i<data.size();++i)data[i]=(unsigned char)(255-i);if(ok)ok=WriteBytes(root+L"\\source\\app.exe",app)&&WriteBytes(root+L"\\source\\nested\\data.bin",data);
    Updates::Inventory inventory;if(ok)ok=Updates::InventoryTree(root+L"\\source",L"",inventory);std::wstring sourceZip=root+L"\\source.zip",publicZip=root+L"\\public.zip",privatePackage=root+L"\\private.1KB";if(ok)ok=Updates::CreateZip(root+L"\\source",inventory,sourceZip)&&Updates::CreatePackageFromZip(sourceZip,publicZip)&&StandardZipShape(publicZip,2);
    uint64_t zipSize=0;uint8_t secret[PayloadSecretBytes]={1,2,3,4,5,6,7,8};PayloadCryptoError error;if(ok)ok=Updates::PackageSizeFromZip(sourceZip,zipSize)&&EncryptPayloadStream(privatePackage,zipSize,secret,L"gh:1kb-exe/1kb",L"private.1KB",ProduceZip,&sourceZip,&error);
    std::vector<unsigned char> encrypted;if(ok)ok=FileBytes(privatePackage,encrypted)&&encrypted.size()==zipSize+EncryptedPayloadOverhead&&Updates::Tree(root+L"\\staging");
    Updates::StreamExtractor extractor(root+L"\\staging");PayloadDecryptStream decrypt;if(ok)ok=BeginPayloadDecrypt(decrypt,secret,L"gh:1kb-exe/1kb",L"private.1KB",ExtractBytes,&extractor,&error)&&Feed(decrypt,encrypted)&&FinishPayloadDecrypt(decrypt)&&extractor.finish();EndPayloadDecrypt(decrypt);if(ok)ok=SameBytes(root+L"\\staging\\app.exe",app)&&SameBytes(root+L"\\staging\\nested\\data.bin",data);
    if(ok){encrypted.back()^=1;ok=Updates::Tree(root+L"\\bad");Updates::StreamExtractor rejected(root+L"\\bad");PayloadDecryptStream bad;if(ok)ok=BeginPayloadDecrypt(bad,secret,L"gh:1kb-exe/1kb",L"private.1KB",ExtractBytes,&rejected,&error)&&Feed(bad,encrypted)&&!FinishPayloadDecrypt(bad)&&!rejected.finish();EndPayloadDecrypt(bad);}
    SecureZeroMemory(secret,sizeof(secret));RuntimeSupport::DeleteTree(root);if(!ok)fwprintf(stderr,L"streamable ZIP/AES-GCM round-trip failed: %ls\n",error.message.c_str());else puts("standard ZIP and streaming AES-GCM round-trip passed");return ok?0:1;
}
