#pragma once
#define NOMINMAX
#include <windows.h>
#include <string>
#include <vector>
#include <cstring>
#include <climits>

namespace RuntimeSupport {

struct Version {unsigned long long major=0,minor=0,patch=0;};

inline bool ParseVersion(const std::wstring& text,Version* returned=nullptr){
    if(text.empty()||text.size()>63)return false;Version result{};unsigned part=0;unsigned long long value=0;size_t digits=0;
    for(size_t i=0;i<=text.size();++i){wchar_t c=i<text.size()?text[i]:L'.';if(c>=L'0'&&c<=L'9'){if(!digits&&c==L'0'&&i+1<text.size()&&text[i+1]!=L'.')return false;if(value>429496729ull)return false;value=value*10+(c-L'0');if(value>0xffffffffull)return false;++digits;}else if(c==L'.'&&digits&&part<3){if(!part)result.major=value;else if(part==1)result.minor=value;else result.patch=value;++part;value=0;digits=0;}else return false;}
    if(part!=3)return false;if(returned)*returned=result;return true;
}

inline bool Utf8(const std::wstring& value,std::string& bytes){
    if(value.empty()){bytes.clear();return true;}if(value.size()>INT_MAX)return false;int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),(int)value.size(),nullptr,0,nullptr,nullptr);if(n<=0)return false;bytes.resize(n);return WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),(int)value.size(),bytes.data(),n,nullptr,nullptr)==n;
}
inline bool Wide(const char* bytes,size_t size,std::wstring& value){
    if(!size){value.clear();return true;}if(!bytes||size>INT_MAX)return false;int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,bytes,(int)size,nullptr,0);if(n<=0)return false;value.resize(n);return MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,bytes,(int)size,value.data(),n)==n;
}

inline bool AtomicWrite(const std::wstring& path,const void* data,size_t size){
    if(size>MAXDWORD||(!data&&size))return false;std::wstring temporary=path+L".tmp."+std::to_wstring(GetCurrentProcessId());DeleteFileW(temporary.c_str());HANDLE file=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH,nullptr);if(file==INVALID_HANDLE_VALUE)return false;DWORD wrote=0;bool ok=WriteFile(file,data,(DWORD)size,&wrote,nullptr)&&wrote==size&&FlushFileBuffers(file);CloseHandle(file);if(ok){DWORD attributes=GetFileAttributesW(path.c_str());ok=attributes!=INVALID_FILE_ATTRIBUTES?ReplaceFileW(path.c_str(),temporary.c_str(),nullptr,REPLACEFILE_WRITE_THROUGH,nullptr,nullptr)!=FALSE:MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE;}if(!ok)DeleteFileW(temporary.c_str());return ok;
}
inline bool AtomicWrite(const std::wstring& path,const std::string& bytes){return AtomicWrite(path,bytes.data(),bytes.size());}
inline bool AtomicWriteUtf8(const std::wstring& path,const std::wstring& value){std::string bytes;return Utf8(value,bytes)&&AtomicWrite(path,bytes);}

inline bool ExecutableSubsystem(const std::wstring& path,WORD& subsystem,bool allowCrinkler=false){
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);if(file==INVALID_HANDLE_VALUE)return false;IMAGE_DOS_HEADER dos{};IMAGE_FILE_HEADER header{};LARGE_INTEGER size{},at{};DWORD got=0,signature=0;WORD magic=0;bool ok=GetFileSizeEx(file,&size)&&ReadFile(file,&dos,sizeof(dos),&got,nullptr)&&got==sizeof(dos)&&dos.e_magic==IMAGE_DOS_SIGNATURE&&dos.e_lfanew>0&&dos.e_lfanew<=size.QuadPart-(LONGLONG)(sizeof(signature)+sizeof(header)+sizeof(magic));if(ok){at.QuadPart=dos.e_lfanew;ok=SetFilePointerEx(file,at,nullptr,FILE_BEGIN)&&ReadFile(file,&signature,sizeof(signature),&got,nullptr)&&got==sizeof(signature)&&signature==IMAGE_NT_SIGNATURE&&ReadFile(file,&header,sizeof(header),&got,nullptr)&&got==sizeof(header)&&ReadFile(file,&magic,sizeof(magic),&got,nullptr)&&got==sizeof(magic);}if(ok){DWORD offset=magic==IMAGE_NT_OPTIONAL_HDR32_MAGIC?FIELD_OFFSET(IMAGE_OPTIONAL_HEADER32,Subsystem):magic==IMAGE_NT_OPTIONAL_HDR64_MAGIC?FIELD_OFFSET(IMAGE_OPTIONAL_HEADER64,Subsystem):0;bool crinkler=allowCrinkler&&dos.e_lfanew==4&&header.Machine==IMAGE_FILE_MACHINE_I386&&!header.NumberOfSections&&header.SizeOfOptionalHeader==8&&magic==IMAGE_NT_OPTIONAL_HDR32_MAGIC;ok=offset&&(header.SizeOfOptionalHeader>=offset+sizeof(subsystem)||crinkler);if(ok){at.QuadPart=(LONGLONG)dos.e_lfanew+sizeof(signature)+sizeof(header)+offset;ok=SetFilePointerEx(file,at,nullptr,FILE_BEGIN)&&ReadFile(file,&subsystem,sizeof(subsystem),&got,nullptr)&&got==sizeof(subsystem)&&(subsystem==IMAGE_SUBSYSTEM_WINDOWS_GUI||subsystem==IMAGE_SUBSYSTEM_WINDOWS_CUI);}}CloseHandle(file);return ok;
}

inline bool DeleteTree(const std::wstring& path){
    DWORD root=GetFileAttributesW(path.c_str());if(root==INVALID_FILE_ATTRIBUTES)return GetLastError()==ERROR_FILE_NOT_FOUND||GetLastError()==ERROR_PATH_NOT_FOUND;if(!(root&FILE_ATTRIBUTE_DIRECTORY)||(root&FILE_ATTRIBUTE_REPARSE_POINT)){SetFileAttributesW(path.c_str(),FILE_ATTRIBUTE_NORMAL);return (root&FILE_ATTRIBUTE_DIRECTORY)?RemoveDirectoryW(path.c_str())!=FALSE:DeleteFileW(path.c_str())!=FALSE;}bool ok=true;WIN32_FIND_DATAW data{};HANDLE search=FindFirstFileW((path+L"\\*").c_str(),&data);if(search!=INVALID_HANDLE_VALUE){do{if(wcscmp(data.cFileName,L".")&&wcscmp(data.cFileName,L"..")){std::wstring child=path+L"\\"+data.cFileName;if((data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)&&!(data.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))ok=DeleteTree(child)&&ok;else{SetFileAttributesW(child.c_str(),FILE_ATTRIBUTE_NORMAL);bool removed=(data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)?RemoveDirectoryW(child.c_str())!=FALSE:DeleteFileW(child.c_str())!=FALSE;ok=removed&&ok;}}}while(FindNextFileW(search,&data));FindClose(search);}SetFileAttributesW(path.c_str(),FILE_ATTRIBUTE_NORMAL);return RemoveDirectoryW(path.c_str())!=FALSE&&ok;
}

template<class String> inline bool SafeArchiveEntry(String value){using Character=typename String::value_type;for(Character& c:value)if(c==(Character)'\\')c=(Character)'/';if(value.empty()||value[0]==(Character)'/'||value.find((Character)':')!=String::npos)return false;size_t at=0;while(at<=value.size()){size_t slash=value.find((Character)'/',at);String part=value.substr(at,slash==String::npos?slash:slash-at);if(part==String{(Character)'.',(Character)'.'})return false;if(slash==String::npos)break;at=slash+1;}return true;}

template<class Consumer> inline bool ParseKeyValues(const std::vector<char>& bytes,size_t cap,Consumer consume){
    if(bytes.empty()||bytes.size()>cap)return false;size_t at=0;while(at<bytes.size()){size_t end=at;while(end<bytes.size()&&bytes[end]!='\n')++end;size_t lineEnd=end;if(lineEnd>at&&bytes[lineEnd-1]=='\r')--lineEnd;if(lineEnd==at){at=end+1;continue;}size_t count=lineEnd-at;if(count>5000)return false;const char* line=bytes.data()+at;for(size_t i=0;i<count;++i)if((unsigned char)line[i]<0x20||(unsigned char)line[i]==0x7f)return false;const char* equal=(const char*)memchr(line,'=',count);if(!equal||equal==line||equal==line+count-1)return false;std::string key(line,equal);std::wstring value;if(!Wide(equal+1,count-(size_t)(equal+1-line),value)||!consume(key,value))return false;at=end+1;}return true;
}

} // namespace RuntimeSupport
