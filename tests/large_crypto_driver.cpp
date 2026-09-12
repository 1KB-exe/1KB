#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../src/payload-crypto.h"
#include <cstdio>
#include <cstring>

static bool SameFile(const std::wstring& a,const std::wstring& b){
    HANDLE x=CreateFileW(a.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
    HANDLE y=CreateFileW(b.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
    if(x==INVALID_HANDLE_VALUE||y==INVALID_HANDLE_VALUE){if(x!=INVALID_HANDLE_VALUE)CloseHandle(x);if(y!=INVALID_HANDLE_VALUE)CloseHandle(y);return false;}
    unsigned char xb[65536],yb[65536];bool same=true;for(;;){DWORD xn=0,yn=0;if(!ReadFile(x,xb,sizeof(xb),&xn,nullptr)||!ReadFile(y,yb,sizeof(yb),&yn,nullptr)||xn!=yn||(xn&&memcmp(xb,yb,xn))){same=false;break;}if(!xn)break;}CloseHandle(x);CloseHandle(y);return same;
}
int wmain(int argc,wchar_t** argv){if(argc!=2)return 2;std::wstring input=argv[1],encrypted=input+L".1KB",output=input+L".plain";uint8_t key[PayloadSecretBytes]={1,2,3,4,5,6,7,8};PayloadCryptoError error;bool ok=EncryptPayloadZip(input,encrypted,key,L"large-test",L"large.zip",&error)&&DecryptPayloadZip(encrypted,output,key,L"large-test",L"large.zip",&error)&&SameFile(input,output);DeleteFileW(encrypted.c_str());DeleteFileW(output.c_str());if(!ok)fwprintf(stderr,L"%ls\n",error.message.c_str());return ok?0:1;}
