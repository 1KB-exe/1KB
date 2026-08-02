#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vector>
#include <algorithm>
#include <cstdio>
#include "../src/payload-crypto.h"

static bool Write(const std::wstring& path,const std::vector<unsigned char>& bytes){HANDLE h=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);if(h==INVALID_HANDLE_VALUE)return false;DWORD n=0;bool ok=WriteFile(h,bytes.data(),(DWORD)bytes.size(),&n,nullptr)&&n==bytes.size();CloseHandle(h);return ok;}
static bool Read(const std::wstring& path,std::vector<unsigned char>& bytes){HANDLE h=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);if(h==INVALID_HANDLE_VALUE)return false;LARGE_INTEGER z{};bool ok=GetFileSizeEx(h,&z)&&z.QuadPart<=MAXDWORD;bytes.resize(ok?(size_t)z.QuadPart:0);DWORD n=0;if(ok)ok=ReadFile(h,bytes.data(),(DWORD)bytes.size(),&n,nullptr)&&n==bytes.size();CloseHandle(h);return ok;}
int wmain(){
    wchar_t root[MAX_PATH]{},name[MAX_PATH]{};if(!GetTempPathW(MAX_PATH,root)||!GetTempFileNameW(root,L"llp",0,name))return 1;DeleteFileW(name);
    std::wstring base=name,input=base+L".zip",a=base+L".a.1KB",b=base+L".b.1KB",out=base+L".out.zip",bad=base+L".bad.1KB",appId=L"gh:1kb-exe/1kb",version=L"1.2.3";
    std::vector<unsigned char> plain(150000);for(size_t i=0;i<plain.size();++i)plain[i]=(unsigned char)(i*37);uint8_t secret[PayloadSecretBytes]{},wrong[PayloadSecretBytes]{};PayloadCryptoError error;
    bool ok=Write(input,plain)&&GeneratePayloadSecret(secret,&error)&&GeneratePayloadSecret(wrong,&error)&&EncryptPayloadZip(input,a,secret,appId,version,&error)&&EncryptPayloadZip(input,b,secret,appId,version,&error);
    std::vector<unsigned char> x,y,z;if(ok)ok=Read(a,x)&&Read(b,y)&&x.size()==plain.size()+EncryptedPayloadOverhead&&x[8]==1&&x[9]==0&&x[10]==40&&x[11]==0&&!std::equal(x.begin()+20,x.begin()+32,y.begin()+20);
    if(ok)ok=DecryptPayloadZip(a,out,secret,appId,version,&error)&&Read(out,z)&&z==plain;DeleteFileW(out.c_str());
    if(ok)ok=!DecryptPayloadZip(a,out,wrong,appId,version,&error)&&GetFileAttributesW(out.c_str())==INVALID_FILE_ATTRIBUTES;
    if(ok)ok=!DecryptPayloadZip(a,out,secret,L"gh:1kb-exe/1kb#other",version,&error)&&GetFileAttributesW(out.c_str())==INVALID_FILE_ATTRIBUTES;
    if(ok)ok=!DecryptPayloadZip(a,out,secret,appId,L"1.2.4",&error)&&GetFileAttributesW(out.c_str())==INVALID_FILE_ATTRIBUTES;
    if(ok){x[EncryptedPayloadHeaderBytes+10]^=1;ok=Write(bad,x)&&!DecryptPayloadZip(bad,out,secret,appId,version,&error)&&GetFileAttributesW(out.c_str())==INVALID_FILE_ATTRIBUTES;}
    if(ok){x[EncryptedPayloadHeaderBytes+10]^=1;x[32]=1;ok=Write(bad,x)&&!ValidateEncryptedPayloadHeader(bad,nullptr,&error);}
    SecureZeroMemory(secret,sizeof(secret));SecureZeroMemory(wrong,sizeof(wrong));DeleteFileW(input.c_str());DeleteFileW(a.c_str());DeleteFileW(b.c_str());DeleteFileW(bad.c_str());DeleteFileW(out.c_str());if(!ok)fwprintf(stderr,L"payload crypto test failed: %ls\n",error.message.c_str());return ok?0:1;
}
