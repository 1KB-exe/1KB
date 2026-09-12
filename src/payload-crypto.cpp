#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <vector>
#include <cstring>
#include <algorithm>
#include "payload-crypto.h"
#pragma comment(lib,"bcrypt.lib")
namespace {
constexpr uint8_t Magic[8]={'1','K','P','A','C','K','1',0};
constexpr uint8_t AadMagic[8]={'1','K','A','A','D','1',0,0};
constexpr uint64_t MaximumPayload=2ull*1024*1024*1024-1-EncryptedPayloadOverhead;
void Error(PayloadCryptoError* e,const wchar_t* s){if(e)e->message=s;}
void Put16(uint8_t* p,uint16_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
void Put64(uint8_t* p,uint64_t v){for(unsigned i=0;i<8;++i)p[i]=(uint8_t)(v>>(i*8));}
uint16_t Get16(const uint8_t* p){return p[0]|uint16_t(p[1])<<8;}
uint64_t Get64(const uint8_t* p){uint64_t v=0;for(unsigned i=0;i<8;++i)v|=uint64_t(p[i])<<(i*8);return v;}
bool Write(HANDLE h,const void* p,DWORD size){DWORD n=0;return WriteFile(h,p,size,&n,nullptr)&&n==size;}
bool Utf8(const std::wstring& s,std::vector<uint8_t>& b){if(s.empty()||s.size()>USHRT_MAX)return false;int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s.data(),(int)s.size(),nullptr,0,nullptr,nullptr);if(n<=0||n>USHRT_MAX)return false;b.resize(n);return WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s.data(),(int)s.size(),(char*)b.data(),n,nullptr,nullptr)==n;}
bool Aad(const uint8_t* header,const std::wstring& id,const std::wstring& package,std::vector<uint8_t>& aad){std::vector<uint8_t> app,v;if(!Utf8(id,app)||!Utf8(package,v))return false;aad.assign(header,header+EncryptedPayloadHeaderBytes);aad.insert(aad.end(),AadMagic,AadMagic+8);uint8_t length[2];Put16(length,(uint16_t)app.size());aad.insert(aad.end(),length,length+2);aad.insert(aad.end(),app.begin(),app.end());Put16(length,(uint16_t)v.size());aad.insert(aad.end(),length,length+2);aad.insert(aad.end(),v.begin(),v.end());return true;}
struct Key {
    BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_KEY_HANDLE key=nullptr;std::vector<uint8_t> object;
    ~Key(){if(key)BCryptDestroyKey(key);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);if(!object.empty())SecureZeroMemory(object.data(),object.size());}
    bool init(const uint8_t* secret,const std::vector<uint8_t>& context){if(!secret)return false;uint8_t derived[32]{};BCRYPT_ALG_HANDLE hash=nullptr;NTSTATUS s=BCryptOpenAlgorithmProvider(&hash,BCRYPT_SHA256_ALGORITHM,nullptr,BCRYPT_ALG_HANDLE_HMAC_FLAG);if(s>=0)s=BCryptDeriveKeyPBKDF2(hash,(PUCHAR)secret,PayloadSecretBytes,(PUCHAR)context.data(),(ULONG)context.size(),100000,derived,32,0);if(hash)BCryptCloseAlgorithmProvider(hash,0);if(s>=0)s=BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_AES_ALGORITHM,nullptr,0);if(s>=0)s=BCryptSetProperty(algorithm,BCRYPT_CHAINING_MODE,(PUCHAR)BCRYPT_CHAIN_MODE_GCM,sizeof(BCRYPT_CHAIN_MODE_GCM),0);DWORD size=0,got=0;if(s>=0)s=BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,(PUCHAR)&size,sizeof(size),&got,0);if(s>=0){object.resize(size);s=BCryptGenerateSymmetricKey(algorithm,&key,object.data(),size,derived,32,0);}SecureZeroMemory(derived,sizeof(derived));return s>=0;}
};
struct CryptState {
    bool encrypt=false,ok=true,initialized=false;uint64_t total=0,processed=0;HANDLE file=INVALID_HANDLE_VALUE;PayloadBytes output=nullptr;void* outputContext=nullptr;PayloadCryptoError* error=nullptr;Key key;std::vector<uint8_t> aad,pending;std::wstring id,package;uint8_t secret[PayloadSecretBytes]{},header[EncryptedPayloadHeaderBytes]{},nonce[PayloadNonceBytes]{},tag[PayloadTagBytes]{},mac[PayloadTagBytes]{},iv[16]{};BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO auth{};
    ~CryptState(){if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);SecureZeroMemory(secret,sizeof(secret));SecureZeroMemory(header,sizeof(header));SecureZeroMemory(nonce,sizeof(nonce));SecureZeroMemory(tag,sizeof(tag));SecureZeroMemory(mac,sizeof(mac));SecureZeroMemory(iv,sizeof(iv));if(!pending.empty())SecureZeroMemory(pending.data(),pending.size());}
    bool init(const uint8_t* suppliedSecret,const std::wstring& suppliedId,const std::wstring& suppliedPackage){if(!Aad(header,suppliedId,suppliedPackage,aad)||!key.init(suppliedSecret,aad))return false;BCRYPT_INIT_AUTH_MODE_INFO(auth);auth.pbNonce=nonce;auth.cbNonce=sizeof(nonce);auth.pbAuthData=aad.data();auth.cbAuthData=(ULONG)aad.size();auth.pbTag=tag;auth.cbTag=sizeof(tag);auth.pbMacContext=mac;auth.cbMacContext=sizeof(mac);initialized=true;return true;}
    bool crypt(const uint8_t* bytes,ULONG count,bool final){uint8_t out[65536];if(count>sizeof(out))return false;ULONG produced=0;auth.dwFlags=final?0:BCRYPT_AUTH_MODE_CHAIN_CALLS_FLAG;NTSTATUS status=encrypt?BCryptEncrypt(key.key,(PUCHAR)bytes,count,&auth,iv,sizeof(iv),out,sizeof(out),&produced,0):BCryptDecrypt(key.key,(PUCHAR)bytes,count,&auth,iv,sizeof(iv),out,sizeof(out),&produced,0);auth.pbAuthData=nullptr;auth.cbAuthData=0;if(status<0||produced!=count)return false;if(encrypt){if(!Write(file,out,produced))return false;}else if(produced&&(!output||!output(outputContext,out,produced)))return false;processed+=count;return true;}
};
bool EncryptInput(void* context,const uint8_t* bytes,unsigned count){auto& s=*(CryptState*)context;if(!s.ok||!count)return s.ok;if(s.processed+s.pending.size()>s.total||count>s.total-s.processed-s.pending.size())return s.ok=false;s.pending.insert(s.pending.end(),bytes,bytes+count);while(s.ok&&s.pending.size()>32768){s.ok=s.crypt(s.pending.data(),32768,false);if(s.ok)s.pending.erase(s.pending.begin(),s.pending.begin()+32768);}return s.ok;}
}

bool GeneratePayloadSecret(uint8_t secret[PayloadSecretBytes],PayloadCryptoError* error){if(secret&&BCryptGenRandom(nullptr,secret,PayloadSecretBytes,BCRYPT_USE_SYSTEM_PREFERRED_RNG)>=0)return true;Error(error,L"Windows could not generate the private launcher secret.");return false;}

bool EncryptPayloadStream(const std::wstring& output,uint64_t plaintextBytes,const uint8_t secret[PayloadSecretBytes],const std::wstring& id,const std::wstring& package,PayloadProducer produce,void* context,PayloadCryptoError* error){
    if(error)error->message.clear();if(!plaintextBytes||plaintextBytes>MaximumPayload||!produce){Error(error,L"The streaming payload size is invalid.");return false;}CryptState s;s.encrypt=true;s.total=plaintextBytes;s.error=error;memcpy(s.header,Magic,8);Put16(s.header+8,1);Put16(s.header+10,EncryptedPayloadHeaderBytes);Put64(s.header+12,plaintextBytes);if(BCryptGenRandom(nullptr,s.nonce,sizeof(s.nonce),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0){Error(error,L"Windows could not generate the payload nonce.");return false;}memcpy(s.header+20,s.nonce,sizeof(s.nonce));s.file=CreateFileW(output.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_FLAG_SEQUENTIAL_SCAN,nullptr);if(s.file==INVALID_HANDLE_VALUE){Error(error,L"The encrypted payload could not be created.");return false;}bool ok=s.init(secret,id,package)&&Write(s.file,s.header,sizeof(s.header))&&produce(context,EncryptInput,&s)&&s.ok&&s.processed+s.pending.size()==s.total&&s.crypt(s.pending.data(),(ULONG)s.pending.size(),true)&&Write(s.file,s.tag,sizeof(s.tag))&&FlushFileBuffers(s.file);CloseHandle(s.file);s.file=INVALID_HANDLE_VALUE;if(!ok){DeleteFileW(output.c_str());Error(error,L"The payload could not be encrypted. Check disk space and the under-2-GiB asset limit.");}return ok;
}

bool BeginPayloadDecrypt(PayloadDecryptStream& stream,const uint8_t secret[PayloadSecretBytes],const std::wstring& id,const std::wstring& package,PayloadBytes output,void* outputContext,PayloadCryptoError* error){
    EndPayloadDecrypt(stream);if(error)error->message.clear();if(!secret||!output){Error(error,L"The payload authentication context is invalid.");return false;}auto* s=new CryptState;s->output=output;s->outputContext=outputContext;s->error=error;s->id=id;s->package=package;memcpy(s->secret,secret,PayloadSecretBytes);s->pending.reserve(32784);stream.state=s;return true;
}

bool DecryptPayloadBytes(PayloadDecryptStream& stream,const uint8_t* bytes,unsigned count){
    auto* s=(CryptState*)stream.state;if(!s||!s->ok||(!bytes&&count))return false;
    while(count){
        if(!s->initialized){unsigned have=(unsigned)s->pending.size(),take=std::min<unsigned>(count,EncryptedPayloadHeaderBytes-have);s->pending.insert(s->pending.end(),bytes,bytes+take);bytes+=take;count-=take;if(s->pending.size()<EncryptedPayloadHeaderBytes)continue;memcpy(s->header,s->pending.data(),sizeof(s->header));s->pending.clear();if(memcmp(s->header,Magic,8)||Get16(s->header+8)!=1||Get16(s->header+10)!=EncryptedPayloadHeaderBytes){Error(s->error,L"Invalid encrypted payload header.");return s->ok=false;}for(unsigned i=32;i<40;++i)if(s->header[i]){Error(s->error,L"Invalid encrypted payload header.");return s->ok=false;}s->total=Get64(s->header+12);memcpy(s->nonce,s->header+20,sizeof(s->nonce));if(!s->total||s->total>MaximumPayload||!s->init(s->secret,s->id,s->package)){Error(s->error,L"The payload decryptor could not be initialized.");return s->ok=false;}SecureZeroMemory(s->secret,sizeof(s->secret));
        }
        unsigned take=std::min<unsigned>(count,32768);s->pending.insert(s->pending.end(),bytes,bytes+take);bytes+=take;count-=take;
        // Preserve the encryption call boundaries while retaining the trailing tag.
        while(s->pending.size()>32768+PayloadTagBytes){if(!s->crypt(s->pending.data(),32768,false)){Error(s->error,L"The encrypted payload could not be decrypted.");return s->ok=false;}s->pending.erase(s->pending.begin(),s->pending.begin()+32768);}
    }return true;
}

bool FinishPayloadDecrypt(PayloadDecryptStream& stream){
    auto* s=(CryptState*)stream.state;if(!s||!s->ok||!s->initialized||s->pending.size()<PayloadTagBytes){if(s)Error(s->error,L"The encrypted payload is truncated.");return false;}size_t cipher=s->pending.size()-PayloadTagBytes;memcpy(s->tag,s->pending.data()+cipher,PayloadTagBytes);bool ok=s->processed+cipher==s->total&&s->crypt(s->pending.data(),(ULONG)cipher,true);if(!ok)Error(s->error,L"The payload could not be authenticated. Check the launcher secret, identity, and package name.");s->ok=ok;return ok;
}
void EndPayloadDecrypt(PayloadDecryptStream& stream){delete (CryptState*)stream.state;stream.state=nullptr;}
