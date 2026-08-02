#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <vector>
#include <cstring>
#include "Config.h"
#include "payload-crypto.h"

#pragma comment(lib, "bcrypt.lib")

namespace {
constexpr uint8_t Magic[8]={'1','K','P','A','C','K','1',0};
constexpr uint8_t AadMagic[8]={'1','K','A','A','D','1',0,0};
constexpr uint16_t Version=1;
constexpr unsigned DerivedKeyBytes=32;
constexpr ULONGLONG Pbkdf2Iterations=100000;

void Error(PayloadCryptoError* error,const wchar_t* text){if(error)error->message=text;}
void Put16(uint8_t* p,uint16_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
void Put64(uint8_t* p,unsigned long long v){for(unsigned i=0;i<8;++i)p[i]=(uint8_t)(v>>(i*8));}
uint16_t Get16(const uint8_t* p){return (uint16_t)(p[0]|((uint16_t)p[1]<<8));}
unsigned long long Get64(const uint8_t* p){unsigned long long v=0;for(unsigned i=0;i<8;++i)v|=(unsigned long long)p[i]<<(i*8);return v;}
bool ReadExact(HANDLE file,void* bytes,DWORD size){DWORD got=0;return ReadFile(file,bytes,size,&got,nullptr)&&got==size;}
bool Utf8(const std::wstring& value,std::vector<uint8_t>& bytes){if(value.empty())return false;int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),(int)value.size(),nullptr,0,nullptr,nullptr);if(n<=0||n>USHRT_MAX)return false;bytes.resize(n);return WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),(int)value.size(),(char*)bytes.data(),n,nullptr,nullptr)==n;}
bool BuildAad(const uint8_t header[EncryptedPayloadHeaderBytes],const std::wstring& appId,const std::wstring& releaseVersion,std::vector<uint8_t>& aad,PayloadCryptoError* error){std::vector<uint8_t> app,version;if(!Utf8(appId,app)||!Utf8(releaseVersion,version)){Error(error,L"The payload authentication identity is invalid.");return false;}aad.resize(EncryptedPayloadHeaderBytes+sizeof(AadMagic)+2+app.size()+2+version.size());uint8_t* at=aad.data();memcpy(at,header,EncryptedPayloadHeaderBytes);at+=EncryptedPayloadHeaderBytes;memcpy(at,AadMagic,sizeof(AadMagic));at+=sizeof(AadMagic);Put16(at,(uint16_t)app.size());at+=2;memcpy(at,app.data(),app.size());at+=app.size();Put16(at,(uint16_t)version.size());at+=2;memcpy(at,version.data(),version.size());return true;}

bool ReadHeader(HANDLE file,uint8_t header[EncryptedPayloadHeaderBytes],EncryptedPayloadInfo* info,PayloadCryptoError* error){
    LARGE_INTEGER size{};if(!GetFileSizeEx(file,&size)||size.QuadPart<EncryptedPayloadOverhead){Error(error,L"The encrypted application payload header is truncated.");return false;}
    if(!ReadExact(file,header,EncryptedPayloadHeaderBytes)){Error(error,L"The encrypted application payload header could not be read.");return false;}
    if(memcmp(header,Magic,sizeof(Magic))||Get16(header+8)!=Version){Error(error,L"The encrypted application payload has an invalid container version.");return false;}
    if(Get16(header+10)!=EncryptedPayloadHeaderBytes){Error(error,L"The encrypted application payload has an invalid header size.");return false;}
    for(unsigned i=32;i<40;++i)if(header[i]){Error(error,L"The encrypted application payload has invalid reserved header data.");return false;}
    unsigned long long plain=Get64(header+12);
    if(!plain||plain>Limits::MaxApplicationSize){Error(error,L"The encrypted application payload exceeds the configured size limit.");return false;}
    if((unsigned long long)size.QuadPart!=plain+EncryptedPayloadOverhead){Error(error,L"The encrypted application payload size does not match its header.");return false;}
    if(info){info->plaintextBytes=plain;memcpy(info->nonce,header+20,PayloadNonceBytes);}return true;
}

struct CngState {
    BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_KEY_HANDLE key=nullptr;
    std::vector<uint8_t> keyObject;
    ~CngState(){if(key)BCryptDestroyKey(key);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);if(!keyObject.empty())SecureZeroMemory(keyObject.data(),keyObject.size());}
};
bool DeriveKey(const uint8_t secret[PayloadSecretBytes],const std::vector<uint8_t>& context,uint8_t key[DerivedKeyBytes],PayloadCryptoError* error){
    if(!secret){Error(error,L"The private launcher secret is unavailable.");return false;}
    BCRYPT_ALG_HANDLE algorithm=nullptr;NTSTATUS s=BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if(s>=0)s=BCryptDeriveKeyPBKDF2(algorithm,(PUCHAR)secret,PayloadSecretBytes,(PUCHAR)context.data(),(ULONG)context.size(),Pbkdf2Iterations,key,DerivedKeyBytes,0);
    if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);if(s<0){SecureZeroMemory(key,DerivedKeyBytes);Error(error,L"Windows could not derive the payload encryption key.");return false;}return true;
}
bool Init(CngState& c,const uint8_t key[DerivedKeyBytes],PayloadCryptoError* error){
    NTSTATUS s=BCryptOpenAlgorithmProvider(&c.algorithm,BCRYPT_AES_ALGORITHM,nullptr,0);if(s<0){Error(error,L"Windows could not initialize payload encryption.");return false;}
    s=BCryptSetProperty(c.algorithm,BCRYPT_CHAINING_MODE,(PUCHAR)BCRYPT_CHAIN_MODE_GCM,sizeof(BCRYPT_CHAIN_MODE_GCM),0);DWORD objectBytes=0,got=0;
    if(s<0||BCryptGetProperty(c.algorithm,BCRYPT_OBJECT_LENGTH,(PUCHAR)&objectBytes,sizeof(objectBytes),&got,0)<0||!objectBytes){Error(error,L"Windows could not initialize AES-256-GCM.");return false;}
    c.keyObject.resize(objectBytes);
    if(BCryptGenerateSymmetricKey(c.algorithm,&c.key,c.keyObject.data(),objectBytes,(PUCHAR)key,DerivedKeyBytes,0)<0){Error(error,L"Windows could not initialize the payload key.");return false;}return true;
}
}

bool GeneratePayloadSecret(uint8_t secret[PayloadSecretBytes],PayloadCryptoError* error){if(!secret){Error(error,L"The payload secret buffer is invalid.");return false;}if(BCryptGenRandom(nullptr,secret,PayloadSecretBytes,BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0){SecureZeroMemory(secret,PayloadSecretBytes);Error(error,L"Windows could not generate the private launcher encryption secret.");return false;}return true;}

bool ValidateEncryptedPayloadHeader(const std::wstring& path,EncryptedPayloadInfo* info,PayloadCryptoError* error){
    if(error)error->message.clear();HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_SEQUENTIAL_SCAN,nullptr);if(file==INVALID_HANDLE_VALUE){Error(error,L"The encrypted application payload could not be opened.");return false;}uint8_t header[EncryptedPayloadHeaderBytes]{};bool ok=ReadHeader(file,header,info,error);SecureZeroMemory(header,sizeof(header));CloseHandle(file);return ok;
}

bool EncryptPayloadZip(const std::wstring& inputPath,const std::wstring& outputPath,const uint8_t secret[PayloadSecretBytes],const std::wstring& appId,const std::wstring& releaseVersion,PayloadCryptoError* error){
    if(error)error->message.clear();HANDLE input=CreateFileW(inputPath.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_SEQUENTIAL_SCAN,nullptr);if(input==INVALID_HANDLE_VALUE){Error(error,L"The staged application ZIP could not be opened.");return false;}LARGE_INTEGER size{};bool ok=GetFileSizeEx(input,&size)&&size.QuadPart>0&&(unsigned long long)size.QuadPart<=Limits::MaxApplicationSize;HANDLE output=INVALID_HANDLE_VALUE,inputMap=nullptr,outputMap=nullptr;uint8_t* inputView=nullptr,*outputView=nullptr;uint8_t header[EncryptedPayloadHeaderBytes]{},nonce[PayloadNonceBytes]{},tag[PayloadTagBytes]{},derivedKey[DerivedKeyBytes]{};std::vector<uint8_t> aad;CngState c;
    if(!ok)Error(error,L"The staged application ZIP exceeds the configured size limit.");if(ok&&BCryptGenRandom(nullptr,nonce,sizeof(nonce),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0){ok=false;Error(error,L"Windows could not generate a payload nonce.");}
    unsigned long long total=ok?(unsigned long long)size.QuadPart+EncryptedPayloadOverhead:0;if(ok){memcpy(header,Magic,sizeof(Magic));Put16(header+8,Version);Put16(header+10,EncryptedPayloadHeaderBytes);Put64(header+12,(unsigned long long)size.QuadPart);memcpy(header+20,nonce,sizeof(nonce));ok=BuildAad(header,appId,releaseVersion,aad,error)&&DeriveKey(secret,aad,derivedKey,error);}if(ok){output=CreateFileW(outputPath.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_SEQUENTIAL_SCAN,nullptr);LARGE_INTEGER end{};end.QuadPart=(LONGLONG)total;ok=output!=INVALID_HANDLE_VALUE&&SetFilePointerEx(output,end,nullptr,FILE_BEGIN)&&SetEndOfFile(output);if(!ok)Error(error,L"The encrypted payload temporary file could not be created.");}
    if(ok){inputMap=CreateFileMappingW(input,nullptr,PAGE_READONLY,0,0,nullptr);outputMap=CreateFileMappingW(output,nullptr,PAGE_READWRITE,0,0,nullptr);inputView=(uint8_t*)MapViewOfFile(inputMap,FILE_MAP_READ,0,0,0);outputView=(uint8_t*)MapViewOfFile(outputMap,FILE_MAP_WRITE,0,0,0);ok=inputMap&&outputMap&&inputView&&outputView;if(!ok)Error(error,L"The application payload could not be mapped for encryption.");}
    if(ok)ok=Init(c,derivedKey,error);if(ok){memcpy(outputView,header,sizeof(header));BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO auth;BCRYPT_INIT_AUTH_MODE_INFO(auth);auth.pbNonce=nonce;auth.cbNonce=sizeof(nonce);auth.pbAuthData=aad.data();auth.cbAuthData=(ULONG)aad.size();auth.pbTag=tag;auth.cbTag=sizeof(tag);ULONG produced=0;ok=BCryptEncrypt(c.key,inputView,(ULONG)size.QuadPart,&auth,nullptr,0,outputView+EncryptedPayloadHeaderBytes,(ULONG)size.QuadPart,&produced,0)>=0&&produced==(ULONG)size.QuadPart;if(ok){memcpy(outputView+EncryptedPayloadHeaderBytes+(size_t)size.QuadPart,tag,sizeof(tag));ok=FlushViewOfFile(outputView,0)&&FlushFileBuffers(output);}if(!ok)Error(error,L"The application payload could not be encrypted.");}
    if(outputView)UnmapViewOfFile(outputView);if(inputView)UnmapViewOfFile(inputView);if(outputMap)CloseHandle(outputMap);if(inputMap)CloseHandle(inputMap);if(output!=INVALID_HANDLE_VALUE)CloseHandle(output);CloseHandle(input);if(!ok)DeleteFileW(outputPath.c_str());SecureZeroMemory(header,sizeof(header));SecureZeroMemory(nonce,sizeof(nonce));SecureZeroMemory(tag,sizeof(tag));SecureZeroMemory(derivedKey,sizeof(derivedKey));return ok;
}

bool DecryptPayloadZip(const std::wstring& inputPath,const std::wstring& outputPath,const uint8_t secret[PayloadSecretBytes],const std::wstring& appId,const std::wstring& releaseVersion,PayloadCryptoError* error){
    if(error)error->message.clear();HANDLE input=CreateFileW(inputPath.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_SEQUENTIAL_SCAN,nullptr);if(input==INVALID_HANDLE_VALUE){Error(error,L"The encrypted application payload could not be opened.");return false;}uint8_t header[EncryptedPayloadHeaderBytes]{},tag[PayloadTagBytes]{},derivedKey[DerivedKeyBytes]{};EncryptedPayloadInfo info;bool ok=ReadHeader(input,header,&info,error);std::vector<uint8_t> aad;if(ok)ok=BuildAad(header,appId,releaseVersion,aad,error)&&DeriveKey(secret,aad,derivedKey,error);HANDLE output=INVALID_HANDLE_VALUE,inputMap=nullptr,outputMap=nullptr;uint8_t* inputView=nullptr,*outputView=nullptr;CngState c;
    if(ok){output=CreateFileW(outputPath.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_SEQUENTIAL_SCAN,nullptr);LARGE_INTEGER end{};end.QuadPart=(LONGLONG)info.plaintextBytes;ok=output!=INVALID_HANDLE_VALUE&&SetFilePointerEx(output,end,nullptr,FILE_BEGIN)&&SetEndOfFile(output);if(!ok)Error(error,L"The temporary application ZIP could not be created.");}
    if(ok){inputMap=CreateFileMappingW(input,nullptr,PAGE_READONLY,0,0,nullptr);outputMap=CreateFileMappingW(output,nullptr,PAGE_READWRITE,0,0,nullptr);inputView=(uint8_t*)MapViewOfFile(inputMap,FILE_MAP_READ,0,0,0);outputView=(uint8_t*)MapViewOfFile(outputMap,FILE_MAP_WRITE,0,0,0);ok=inputMap&&outputMap&&inputView&&outputView;if(!ok)Error(error,L"The encrypted application payload could not be mapped for authentication.");}
    if(ok)ok=Init(c,derivedKey,error);if(ok){memcpy(tag,inputView+EncryptedPayloadHeaderBytes+(size_t)info.plaintextBytes,sizeof(tag));BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO auth;BCRYPT_INIT_AUTH_MODE_INFO(auth);auth.pbNonce=info.nonce;auth.cbNonce=sizeof(info.nonce);auth.pbAuthData=aad.data();auth.cbAuthData=(ULONG)aad.size();auth.pbTag=tag;auth.cbTag=sizeof(tag);ULONG produced=0;ok=BCryptDecrypt(c.key,inputView+EncryptedPayloadHeaderBytes,(ULONG)info.plaintextBytes,&auth,nullptr,0,outputView,(ULONG)info.plaintextBytes,&produced,0)>=0&&produced==(ULONG)info.plaintextBytes;if(ok)ok=FlushViewOfFile(outputView,0)&&FlushFileBuffers(output);else{SecureZeroMemory(outputView,(size_t)info.plaintextBytes);Error(error,L"The encrypted application payload could not be authenticated. The private launcher, application identity, or release version may not match this payload, or the payload may be damaged.");}}
    if(outputView)UnmapViewOfFile(outputView);if(inputView)UnmapViewOfFile(inputView);if(outputMap)CloseHandle(outputMap);if(inputMap)CloseHandle(inputMap);if(output!=INVALID_HANDLE_VALUE)CloseHandle(output);CloseHandle(input);if(!ok)DeleteFileW(outputPath.c_str());SecureZeroMemory(header,sizeof(header));SecureZeroMemory(tag,sizeof(tag));SecureZeroMemory(info.nonce,sizeof(info.nonce));SecureZeroMemory(derivedKey,sizeof(derivedKey));return ok;
}
