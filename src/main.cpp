#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <winhttp.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <wincodec.h>
#include <string>
#include <vector>
#include <cstdint>
#include <cwctype>
#include <cstdio>
#include <cstdlib>
#include "Config.h"
#include "launcher-builder.h"
#include "overlay-identity.h"
#include "payload-crypto.h"
#ifndef ONEKB_RUNTIME_ONLY
#include "icon-crinkler-packer.h"
#include "icon-png-optimizer.h"
#include "deployment-manager.h"
#endif

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "gdi32.lib")

struct Paths { std::wstring root,versions,current,updateState; };
struct Version { unsigned long long a=0,b=0,c=0; };
struct HttpValidators {std::wstring etag,lastModified;};
enum class UpdateMode : DWORD { No=0, Yes=1, Restart=2 };
enum class IconMode { Small,None };
enum class AppProvider { Github,DirectUrl };
struct SourceInfo {std::wstring url,host,path,query;INTERNET_PORT port=0;bool https=false;};
struct AppIdentity {AppProvider provider=AppProvider::DirectUrl;std::wstring appId,owner,repository,appKey,relativeDirectory,mutexKey;SourceInfo source;};
struct RuntimeConfig {AppIdentity identity;std::wstring displayName,fallbackDisplayName;UpdateMode mode=UpdateMode::No;};
struct UpdateResponse {std::wstring version,download;bool githubDownload=false;UpdateMode mode=UpdateMode::No;HttpValidators validators;};
struct LaunchedProcess { HANDLE handle=nullptr;DWORD id=0;std::wstring version; };

constexpr WORD GuiRuntimeTemplateResourceId=101;
constexpr WORD ConsoleRuntimeTemplateResourceId=103;
constexpr WORD GuiCrinklerResourceId=104;
constexpr WORD ConsoleCrinklerResourceId=105;
constexpr WORD GuiIconCrinklerResourceId=106;
constexpr WORD ConsoleIconCrinklerResourceId=107;
// Runtime update/apply serialization reuses the recovery URL as its object name.
constexpr const wchar_t* RuntimeMutexName=LauncherRuntimeDownloadUrl;
using EmbeddedConfig=LauncherOverlay;

static Paths g;
static RuntimeConfig cfg;
static std::wstring gLauncherPath;
static LauncherOverlay gLauncherOverlay;
static std::wstring gAppArguments;
static bool gPersistenceWarningShown=false;
static bool gConsoleLauncher=false,gBackgroundUpdate=false,gRuntimeUpdate=false,gRuntimeApply=false;
static DWORD gRuntimeApplyParent=0;
static HttpValidators gRuntimeApplyValidators;
#ifdef ONEKB_RUNTIME_TESTS
static std::wstring gRuntimeTestUrl;
#endif
static HANDLE gConfigurationMutex=nullptr;
static std::wstring gResolvedVersion,gResolvedApplication;
static DWORD gLastHttpStatus=0;
static HttpValidators gApplicationValidators;

constexpr UINT UpdateUiMessage=WM_APP+40;
constexpr UINT UpdateUiCloseMessage=WM_APP+41;
struct UpdateUiPayload {std::wstring status,detail;int progress=-1;};
struct UpdateUiState {HANDLE thread=nullptr,ready=nullptr;HWND window=nullptr;std::wstring status,detail;int progress=-1,pulse=0;HFONT titleFont=nullptr,statusFont=nullptr,detailFont=nullptr;};
static UpdateUiState gUpdateUi;
static std::wstring gUpdateProgressDetail;
static bool gFreshInstall=false;
static LRESULT CALLBACK UpdateUiProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam){
    UpdateUiState* ui=(UpdateUiState*)GetWindowLongPtrW(window,GWLP_USERDATA);
    if(message==WM_NCCREATE){ui=(UpdateUiState*)((CREATESTRUCTW*)lParam)->lpCreateParams;SetWindowLongPtrW(window,GWLP_USERDATA,(LONG_PTR)ui);}
    if(!ui)return DefWindowProcW(window,message,wParam,lParam);
    if(message==UpdateUiMessage){auto* payload=(UpdateUiPayload*)lParam;ui->status=payload->status;ui->detail=payload->detail;ui->progress=payload->progress;delete payload;InvalidateRect(window,nullptr,FALSE);return 0;}
    if(message==UpdateUiCloseMessage){DestroyWindow(window);return 0;}
    if(message==WM_TIMER){ui->pulse=(ui->pulse+3)%140;InvalidateRect(window,nullptr,FALSE);return 0;}
    if(message==WM_MOUSEACTIVATE)return MA_NOACTIVATE;
    if(message==WM_NCHITTEST)return HTTRANSPARENT;
    if(message==WM_ERASEBKGND)return 1;
    if(message==WM_PAINT){PAINTSTRUCT ps{};HDC dc=BeginPaint(window,&ps);RECT client{};GetClientRect(window,&client);HDC memory=CreateCompatibleDC(dc);HBITMAP bitmap=CreateCompatibleBitmap(dc,client.right,client.bottom);HGDIOBJ oldBitmap=SelectObject(memory,bitmap);HBRUSH background=CreateSolidBrush(RGB(18,20,24));FillRect(memory,&client,background);DeleteObject(background);SetBkMode(memory,TRANSPARENT);
        SetTextColor(memory,RGB(242,244,248));SelectObject(memory,ui->titleFont);RECT title{28,20,client.right-28,50};DrawTextW(memory,cfg.displayName.c_str(),-1,&title,DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
        SetTextColor(memory,RGB(230,232,237));SelectObject(memory,ui->statusFont);RECT status{28,62,client.right-28,92};DrawTextW(memory,ui->status.c_str(),-1,&status,DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
        SetTextColor(memory,RGB(145,151,162));SelectObject(memory,ui->detailFont);RECT detail{28,94,client.right-28,116};DrawTextW(memory,ui->detail.c_str(),-1,&detail,DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
        RECT track{28,134,client.right-28,140};HBRUSH trackBrush=CreateSolidBrush(RGB(43,47,55));FillRect(memory,&track,trackBrush);DeleteObject(trackBrush);RECT fill=track;if(ui->progress>=0)fill.right=fill.left+(track.right-track.left)*ui->progress/100;else{int width=92;int travel=(track.right-track.left)+width;fill.left=track.left+ui->pulse*travel/140-width;fill.right=fill.left+width;if(fill.left<track.left)fill.left=track.left;if(fill.right>track.right)fill.right=track.right;}HBRUSH accent=CreateSolidBrush(RGB(98,126,255));if(fill.right>fill.left)FillRect(memory,&fill,accent);DeleteObject(accent);
        BitBlt(dc,0,0,client.right,client.bottom,memory,0,0,SRCCOPY);SelectObject(memory,oldBitmap);DeleteObject(bitmap);DeleteDC(memory);EndPaint(window,&ps);return 0;}
    if(message==WM_DESTROY){KillTimer(window,1);PostQuitMessage(0);return 0;}return DefWindowProcW(window,message,wParam,lParam);
}
static DWORD WINAPI UpdateUiThread(void* parameter){auto* ui=(UpdateUiState*)parameter;WNDCLASSEXW wc{};wc.cbSize=sizeof(wc);wc.style=CS_DROPSHADOW;wc.lpfnWndProc=UpdateUiProc;wc.hInstance=GetModuleHandleW(nullptr);wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.lpszClassName=L"OneKBUpdateWindow";RegisterClassExW(&wc);int width=460,height=166,x=(GetSystemMetrics(SM_CXSCREEN)-width)/2,y=(GetSystemMetrics(SM_CYSCREEN)-height)/2;ui->titleFont=CreateFontW(-17,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");ui->statusFont=CreateFontW(-20,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");ui->detailFont=CreateFontW(-14,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");ui->window=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_LAYERED|WS_EX_TRANSPARENT,wc.lpszClassName,L"Updating",WS_POPUP,x,y,width,height,nullptr,nullptr,wc.hInstance,ui);if(ui->window){SetLayeredWindowAttributes(ui->window,0,255,LWA_ALPHA);SetWindowRgn(ui->window,CreateRoundRectRgn(0,0,width+1,height+1,14,14),TRUE);SetTimer(ui->window,1,24,nullptr);ShowWindow(ui->window,SW_SHOWNOACTIVATE);UpdateWindow(ui->window);}SetEvent(ui->ready);MSG message{};while(ui->window&&GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}if(ui->titleFont)DeleteObject(ui->titleFont);if(ui->statusFont)DeleteObject(ui->statusFont);if(ui->detailFont)DeleteObject(ui->detailFont);ui->window=nullptr;return 0;}
static void ShowUpdateUi(const std::wstring& current){if(gUpdateUi.thread)return;gUpdateUi.status=current.empty()?L"Finding the latest version":L"Checking for updates";gUpdateUi.detail=current.empty()?L"":L"Installed version "+current;gUpdateUi.progress=-1;gUpdateUi.ready=CreateEventW(nullptr,TRUE,FALSE,nullptr);if(!gUpdateUi.ready)return;gUpdateUi.thread=CreateThread(nullptr,0,UpdateUiThread,&gUpdateUi,0,nullptr);if(gUpdateUi.thread)WaitForSingleObject(gUpdateUi.ready,3000);CloseHandle(gUpdateUi.ready);gUpdateUi.ready=nullptr;}
static void SetUpdateUi(const std::wstring& status,const std::wstring& detail,int progress=-1){if(gUpdateUi.window){auto* payload=new UpdateUiPayload{status,detail,progress};if(!PostMessageW(gUpdateUi.window,UpdateUiMessage,0,(LPARAM)payload))delete payload;}}
static void CloseUpdateUi(){if(!gUpdateUi.thread)return;if(gUpdateUi.window)PostMessageW(gUpdateUi.window,UpdateUiCloseMessage,0,0);WaitForSingleObject(gUpdateUi.thread,3000);CloseHandle(gUpdateUi.thread);gUpdateUi.thread=nullptr;}
static void UpdateUiDownloadProgress(unsigned long long received,unsigned long long total){if(gUpdateUi.window&&total){int percent=(int)(received*100/total);if(percent>100)percent=100;SetUpdateUi(L"Downloading",gUpdateProgressDetail,percent);}}
static std::wstring ShortUpdateError(const std::wstring& error){size_t http=error.find(L"HTTP ");if(http!=std::wstring::npos){size_t at=http+5,end=at;while(end<error.size()&&error[end]>=L'0'&&error[end]<=L'9')++end;if(end>at)return L"Server returned HTTP "+error.substr(at,end-at)+L".";}std::wstring lower=error;for(wchar_t& c:lower)c=(wchar_t)towlower(c);if(lower.find(L"download")!=std::wstring::npos)return L"Download failed.";if(lower.find(L"manifest")!=std::wstring::npos||lower.find(L"source url")!=std::wstring::npos)return L"Update information is invalid.";if(lower.find(L"decrypt")!=std::wstring::npos||lower.find(L"encrypt")!=std::wstring::npos||lower.find(L"verif")!=std::wstring::npos)return L"Download verification failed.";if(lower.find(L"install")!=std::wstring::npos||lower.find(L"zip")!=std::wstring::npos||lower.find(L"executable")!=std::wstring::npos||lower.find(L"directory")!=std::wstring::npos)return L"Installation failed.";return L"Please try again later.";}
static void ShowUpdateFailureUi(const std::wstring& error,bool haveInstalled){if(!gUpdateUi.thread)return;SetUpdateUi(haveInstalled?L"Update failed":L"Installation failed",ShortUpdateError(error),0);Sleep(1000);}

static bool EndsWithInsensitive(const std::wstring& value,const wchar_t* suffix);
static bool IsLocalHost(const std::wstring& h){return _wcsicmp(h.c_str(),L"127.0.0.1")==0||_wcsicmp(h.c_str(),L"localhost")==0;}
static const wchar_t* ModeName(UpdateMode mode){return mode==UpdateMode::Yes?L"before-launch":mode==UpdateMode::Restart?L"restart":L"background";}
static bool ParseMode(const std::wstring& value,UpdateMode& mode){if(value==L"before-launch")mode=UpdateMode::Yes;else if(value==L"background")mode=UpdateMode::No;else if(value==L"restart")mode=UpdateMode::Restart;else return false;return true;}
static unsigned long long Fnv1a64(const void* data,size_t size){auto p=(const unsigned char*)data;unsigned long long h=14695981039346656037ull;while(size--)h=(h^*p++)*1099511628211ull;return h;}
static std::wstring Hex64(unsigned long long value){wchar_t text[17]{};swprintf_s(text,L"%016llx",value);return text;}
static bool ParseAppId(const std::wstring& input,AppIdentity& identity);
static bool ValidateCanonicalOverlayIdentity(const std::wstring& supplied,std::wstring& normalized){AppIdentity identity;if(!ParseAppId(supplied,identity))return false;normalized=identity.appId;return true;}
static bool ParseOverlayBytes(const std::vector<uint8_t>& bytes,EmbeddedConfig& c){
    c=EmbeddedConfig{};OverlayIdentity::Decoded publicIdentity,privateIdentity;
    bool isPublic=OverlayIdentity::DecodeOverlayIdentity(bytes.data(),bytes.size(),0,Limits::MaxAppIdBytes,ValidateCanonicalOverlayIdentity,publicIdentity);
    bool isPrivate=OverlayIdentity::DecodeOverlayIdentity(bytes.data(),bytes.size(),PayloadSecretBytes,Limits::MaxAppIdBytes,ValidateCanonicalOverlayIdentity,privateIdentity);
    if(isPublic==isPrivate)return false;
    const auto& decoded=isPrivate?privateIdentity:publicIdentity;c.appId=decoded.identity;
    if(isPrivate){memcpy(c.secret,bytes.data()+decoded.secretOffset,PayloadSecretBytes);c.encryption=PayloadEncryption::Launcher;}return true;
}
static bool ReadEmbeddedFile(const std::wstring& path,EmbeddedConfig& c){HANDLE h=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);if(h==INVALID_HANDLE_VALUE)return false;LARGE_INTEGER z{};bool ok=GetFileSizeEx(h,&z)&&z.QuadPart>0&&(unsigned long long)z.QuadPart<=Limits::MaxApplicationSize&&z.QuadPart<=MAXDWORD;std::vector<uint8_t>b(ok?(size_t)z.QuadPart:0);DWORD got=0;if(ok)ok=ReadFile(h,b.data(),(DWORD)b.size(),&got,nullptr)&&got==b.size();CloseHandle(h);if(ok)ok=ParseOverlayBytes(b,c);if(!b.empty())SecureZeroMemory(b.data(),b.size());return ok;}
static bool GithubPart(const std::wstring& value){if(value.empty()||value.size()>100||value==L"."||value==L"..")return false;for(wchar_t c:value)if(!((c>=L'a'&&c<=L'z')||(c>=L'A'&&c<=L'Z')||(c>=L'0'&&c<=L'9')||c==L'-'||c==L'_'||c==L'.'))return false;return true;}
static bool GithubAppKey(const std::wstring& value){if(value.empty()||value.size()>63)return false;for(size_t i=0;i<value.size();++i){wchar_t c=value[i];if(!((c>=L'a'&&c<=L'z')||(c>=L'A'&&c<=L'Z')||(c>=L'0'&&c<=L'9')||(i&&c==L'-')))return false;}return true;}
static bool SafeUrlPath(const std::wstring& path){if(path.empty()||path[0]!=L'/'||path.find(L"..")!=std::wstring::npos||path.find(L'\\')!=std::wstring::npos||path.find(L'%')!=std::wstring::npos)return false;for(wchar_t c:path)if(c<L' '||c>0x7e)return false;return true;}
static bool ParseDirectUrl(const std::wstring& input,SourceInfo* result=nullptr){
    if(input.empty()||input.size()>4000||input.find(L'\0')!=std::wstring::npos||input.find(L'#')!=std::wstring::npos)return false;for(wchar_t c:input)if(c<L' '||c==0x7f)return false;
    URL_COMPONENTS u{};u.dwStructSize=sizeof(u);u.dwHostNameLength=(DWORD)-1;u.dwUrlPathLength=(DWORD)-1;u.dwExtraInfoLength=(DWORD)-1;u.dwUserNameLength=(DWORD)-1;u.dwPasswordLength=(DWORD)-1;if(!WinHttpCrackUrl(input.c_str(),0,0,&u)||!u.dwHostNameLength||u.dwUserNameLength||u.dwPasswordLength)return false;
    bool https=u.nScheme==INTERNET_SCHEME_HTTPS;if(!https&&u.nScheme!=INTERNET_SCHEME_HTTP)return false;std::wstring host(u.lpszHostName,u.dwHostNameLength),path(u.lpszUrlPath,u.dwUrlPathLength),extra(u.lpszExtraInfo?u.lpszExtraInfo:L"",u.dwExtraInfoLength);if(path.empty())path=L"/";if(host.find(L':')!=std::wstring::npos||path[0]!=L'/'||path.find(L'\\')!=std::wstring::npos||(!https&&!IsLocalHost(host)))return false;
    size_t segment=1;while(segment<=path.size()){size_t slash=path.find(L'/',segment);std::wstring part=path.substr(segment,slash==std::wstring::npos?slash:slash-segment);if(part==L"."||part==L"..")return false;if(slash==std::wstring::npos)break;segment=slash+1;}
    for(wchar_t& c:host)c=(wchar_t)towlower(c);std::wstring normalized=https?L"https://":L"http://";normalized+=host;if((https&&u.nPort!=INTERNET_DEFAULT_HTTPS_PORT)||(!https&&u.nPort!=INTERNET_DEFAULT_HTTP_PORT))normalized+=L":"+std::to_wstring(u.nPort);normalized+=path+extra;
    if(result){*result=SourceInfo{};result->url=normalized;result->host=host;result->path=path;result->query=extra;result->port=u.nPort;result->https=https;}return true;
}
static std::wstring ShortIdentityHash(const std::wstring& value){return Hex64(Fnv1a64(value.data(),value.size()*sizeof(wchar_t))).substr(0,8);}
static bool ReservedWindowsName(const std::wstring& value){std::wstring base=value.substr(0,value.find(L'.'));for(wchar_t& c:base)c=(wchar_t)towupper(c);if(base==L"CON"||base==L"PRN"||base==L"AUX"||base==L"NUL")return true;if(base.size()==4&&(base.rfind(L"COM",0)==0||base.rfind(L"LPT",0)==0)&&base[3]>=L'1'&&base[3]<=L'9')return true;return false;}
static std::wstring DirectoryComponent(const std::wstring& raw,const std::wstring& discriminator){std::wstring value;bool changed=raw.empty();for(wchar_t c:raw){bool bad=c<L' '||c==0x7f||wcschr(L"<>:\"/\\|?*",c);value.push_back(bad?L'_':c);changed|=bad;}if(value.empty())value=L"_empty";while(!value.empty()&&(value.back()==L'.'||value.back()==L' ')){value.pop_back();changed=true;}if(value.empty())value=L"_empty";if(ReservedWindowsName(value))changed=true;if(value.size()>80){value.resize(70);changed=true;}if(changed)value+=L"~"+ShortIdentityHash(discriminator);return value;}
static bool ParseAppId(const std::wstring& input,AppIdentity& identity){
    int bytes=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,input.data(),(int)input.size(),nullptr,0,nullptr,nullptr);if(bytes<=0||(unsigned)bytes>Limits::MaxAppIdBytes)return false;identity=AppIdentity{};
    if(input.rfind(L"gh:",0)==0){std::wstring value=input.substr(3);size_t hash=value.find(L'#'),slash=value.find(L'/');if(value.find(L'#',hash==std::wstring::npos?value.size():hash+1)!=std::wstring::npos||slash==std::wstring::npos||(hash!=std::wstring::npos&&slash>hash)||value.find(L'/',slash+1)!=std::wstring::npos||value.find(L'\\')!=std::wstring::npos||value.find(L'?')!=std::wstring::npos)return false;std::wstring owner=value.substr(0,slash),repo=value.substr(slash+1,hash==std::wstring::npos?hash:hash-slash-1),app=hash==std::wstring::npos?L"":value.substr(hash+1);if(owner.find(L'.')!=std::wstring::npos||!GithubPart(owner)||!GithubPart(repo)||(!app.empty()&&!GithubAppKey(app))||(hash!=std::wstring::npos&&app.empty()))return false;for(wchar_t& c:owner)c=(wchar_t)towlower(c);for(wchar_t& c:repo)c=(wchar_t)towlower(c);for(wchar_t& c:app)c=(wchar_t)towlower(c);identity.provider=AppProvider::Github;identity.owner=owner;identity.repository=repo;identity.appKey=app;identity.appId=L"gh:"+owner+L"/"+repo+(app.empty()?L"":L"#"+app);identity.relativeDirectory=DirectoryComponent(owner,identity.appId+L"#owner")+L"\\"+DirectoryComponent(app.empty()?repo:app,identity.appId+L"#application");std::wstring endpoint=CanonicalGithubManifestUrl(owner+L"/"+repo,app);if(!ParseDirectUrl(endpoint,&identity.source))return false;
    }else{std::wstring direct=input.rfind(L"url:",0)==0?input.substr(4):input,app;size_t hash=direct.find(L'#');if(hash!=std::wstring::npos){if(direct.find(L'#',hash+1)!=std::wstring::npos)return false;app=direct.substr(hash+1);direct.resize(hash);if(!GithubAppKey(app))return false;for(wchar_t& c:app)c=(wchar_t)towlower(c);}SourceInfo source;bool sourceOk=false;
#ifdef ONEKB_RUNTIME_ONLY
    if(input.rfind(L"url:",0)==0)sourceOk=ParseDirectUrl(direct,&source);
#else
    sourceOk=ParseDirectUrl(direct,&source);
#endif
    if(!sourceOk||source.host==L"github.com")return false;identity.provider=AppProvider::DirectUrl;identity.source=source;identity.appKey=app;identity.appId=L"url:"+source.url+(app.empty()?L"":L"#"+app);std::wstring host=source.host;if((source.https&&source.port!=INTERNET_DEFAULT_HTTPS_PORT)||(!source.https&&source.port!=INTERNET_DEFAULT_HTTP_PORT))host+=L"@"+std::to_wstring(source.port);std::wstring folder=app.empty()?Hex64(Fnv1a64(source.url.data(),source.url.size()*sizeof(wchar_t))):app;identity.relativeDirectory=DirectoryComponent(host,source.host+std::to_wstring(source.port))+L"\\"+DirectoryComponent(folder,identity.appId+L"#application");
    }
    identity.mutexKey=L"OneKBApp_"+Hex64(Fnv1a64(identity.appId.data(),identity.appId.size()*sizeof(wchar_t)));return true;
}
static std::wstring LauncherDisplayFallback(const std::wstring& path){size_t slash=path.find_last_of(L"\\/");std::wstring name=slash==std::wstring::npos?path:path.substr(slash+1);if(name.size()>4&&_wcsicmp(name.c_str()+name.size()-4,L".exe")==0)name.resize(name.size()-4);return name.empty()?L"1KB.exe":name;}
static bool EnvironmentValue(const wchar_t* name,std::wstring& value){DWORD n=GetEnvironmentVariableW(name,nullptr,0);if(!n||n>32767)return false;std::vector<wchar_t>b(n);if(GetEnvironmentVariableW(name,b.data(),n)!=n-1)return false;value.assign(b.data(),n-1);return true;}
static bool AbsoluteWindowsPath(const std::wstring& p){return p.rfind(L"\\\\",0)==0||(p.size()>=3&&((p[0]>=L'A'&&p[0]<=L'Z')||(p[0]>=L'a'&&p[0]<=L'z'))&&p[1]==L':'&&(p[2]==L'\\'||p[2]==L'/'));}
static bool LauncherPathFromCommandLine(std::wstring& path){int count=0;LPWSTR* arguments=CommandLineToArgvW(GetCommandLineW(),&count);if(!arguments||count<1){if(arguments)LocalFree(arguments);return false;}std::wstring supplied=arguments[0];LocalFree(arguments);DWORD needed=GetFullPathNameW(supplied.c_str(),0,nullptr,nullptr);if(!needed||needed>32768)return false;std::vector<wchar_t> absolute(needed);DWORD written=GetFullPathNameW(supplied.c_str(),needed,absolute.data(),nullptr);if(!written||written>=needed)return false;path.assign(absolute.data(),written);return AbsoluteWindowsPath(path);}
static bool SubsystemFromBytes(const std::vector<uint8_t>& b,WORD& subsystem){if(b.size()<64)return false;DWORD pe=0;memcpy(&pe,b.data()+0x3c,4);if(pe>b.size()-28)return false;DWORD sig=0;WORD machine=0,sections=0,optionalBytes=0,magic=0;memcpy(&sig,b.data()+pe,4);memcpy(&machine,b.data()+pe+4,2);memcpy(&sections,b.data()+pe+6,2);memcpy(&optionalBytes,b.data()+pe+20,2);memcpy(&magic,b.data()+pe+24,2);size_t offset=magic==IMAGE_NT_OPTIONAL_HDR32_MAGIC?FIELD_OFFSET(IMAGE_OPTIONAL_HEADER32,Subsystem):magic==IMAGE_NT_OPTIONAL_HDR64_MAGIC?FIELD_OFFSET(IMAGE_OPTIONAL_HEADER64,Subsystem):0;bool crinkler=pe==4&&machine==IMAGE_FILE_MACHINE_I386&&!sections&&optionalBytes==8&&magic==IMAGE_NT_OPTIONAL_HDR32_MAGIC;if(sig!=IMAGE_NT_SIGNATURE||!offset||(!crinkler&&optionalBytes<offset+2)||pe+24+offset>b.size()-2)return false;memcpy(&subsystem,b.data()+pe+24+offset,2);return subsystem==IMAGE_SUBSYSTEM_WINDOWS_GUI||subsystem==IMAGE_SUBSYSTEM_WINDOWS_CUI;}
static bool ReadLauncherOnce(const std::wstring& path,EmbeddedConfig& embedded,WORD& subsystem){HANDLE h=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);if(h==INVALID_HANDLE_VALUE)return false;LARGE_INTEGER z{};bool ok=GetFileSizeEx(h,&z)&&z.QuadPart>0&&(unsigned long long)z.QuadPart<=Limits::MaxApplicationSize&&z.QuadPart<=MAXDWORD;std::vector<uint8_t>b(ok?(size_t)z.QuadPart:0);DWORD got=0;if(ok)ok=ReadFile(h,b.data(),(DWORD)b.size(),&got,nullptr)&&got==b.size();CloseHandle(h);if(ok)ok=ParseOverlayBytes(b,embedded)&&SubsystemFromBytes(b,subsystem);if(!b.empty())SecureZeroMemory(b.data(),b.size());return ok;}
#ifdef ONEKB_RUNTIME_ONLY
static bool DecimalDword(const std::wstring& text,DWORD& value){if(text.empty())return false;unsigned long long n=0;for(wchar_t c:text){if(c<L'0'||c>L'9'||n>MAXDWORD/10ull)return false;n=n*10+(c-L'0');if(n>MAXDWORD)return false;}value=(DWORD)n;return value!=0;}
static void DetectPrivateWorkerMode(){
    std::wstring background,update,apply,etag,lastModified;
    gBackgroundUpdate=EnvironmentValue(L"ONEKB_BACKGROUND",background)&&background==L"yes";
    gRuntimeUpdate=EnvironmentValue(L"ONEKB_RUNTIME_UPDATE",update)&&update==L"yes";
    gRuntimeApply=EnvironmentValue(L"ONEKB_RUNTIME_APPLY",apply)&&DecimalDword(apply,gRuntimeApplyParent);
    if(EnvironmentValue(L"ONEKB_RUNTIME_ETAG",etag)&&!etag.empty()&&etag[0]==L'1')gRuntimeApplyValidators.etag=etag.substr(1);
    if(EnvironmentValue(L"ONEKB_RUNTIME_LAST_MODIFIED",lastModified)&&!lastModified.empty()&&lastModified[0]==L'1')gRuntimeApplyValidators.lastModified=lastModified.substr(1);
    const wchar_t* privateVariables[]={L"ONEKB_PATH",L"ONEKB_BACKGROUND",L"ONEKB_RUNTIME_UPDATE",L"ONEKB_RUNTIME_APPLY",L"ONEKB_RUNTIME_ETAG",L"ONEKB_RUNTIME_LAST_MODIFIED"};
    for(auto name:privateVariables)SetEnvironmentVariableW(name,nullptr);
}
#endif
static bool LoadRuntimeConfig(std::wstring& error){
#ifdef ONEKB_RUNTIME_ONLY
    bool haveLauncherPath=LauncherPathFromCommandLine(gLauncherPath);
    if(!haveLauncherPath){error=L"The launcher path in argv[0] could not be resolved.\n\n1KB.exe cannot locate the launcher configuration.";return false;}
    EmbeddedConfig embedded;WORD subsystem=0;if(!ReadLauncherOnce(gLauncherPath,embedded,subsystem)){error=L"The runtime could not read a valid launcher configuration.\n\nThe launcher may be damaged or built for a different runtime version. Rebuild it with the current 1KB.exe.\n\nReceived argv[0]:\n"+gLauncherPath;return false;}
    AppIdentity identity;if(!ParseAppId(embedded.appId,identity)||identity.appId!=embedded.appId){error=L"The launcher contains an invalid or noncanonical app-id.\n\nLauncher path:\n"+gLauncherPath;return false;}gLauncherOverlay=embedded;SecureZeroMemory(embedded.secret,PayloadSecretBytes);cfg.identity=identity;cfg.fallbackDisplayName=LauncherDisplayFallback(gLauncherPath);cfg.displayName=cfg.fallbackDisplayName;cfg.mode=UpdateMode::No;gConsoleLauncher=subsystem==IMAGE_SUBSYSTEM_WINDOWS_CUI;return true;
#else
    error=L"This executable does not contain the launcher runtime.";return false;
#endif
}
static bool Exists(const std::wstring& p){DWORD a=GetFileAttributesW(p.c_str());return a!=INVALID_FILE_ATTRIBUTES&&!(a&FILE_ATTRIBUTE_DIRECTORY);}
static bool EnsureDir(const std::wstring& p){return CreateDirectoryW(p.c_str(),nullptr)!=FALSE||GetLastError()==ERROR_ALREADY_EXISTS;}
static bool EnsureDirTree(const std::wstring& p){int result=SHCreateDirectoryExW(nullptr,p.c_str(),nullptr);return result==ERROR_SUCCESS||result==ERROR_ALREADY_EXISTS||result==ERROR_FILE_EXISTS;}
static bool InitPaths(){std::wstring local;if(!EnvironmentValue(L"LOCALAPPDATA",local)){PWSTR base=nullptr;if(FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData,KF_FLAG_CREATE,nullptr,&base)))return false;local=base;CoTaskMemFree(base);}g.root=local+L"\\"+cfg.identity.relativeDirectory;g.versions=g.root;g.current=g.root+L"\\current.txt";g.updateState=g.root+L"\\1kb.ini";return EnsureDirTree(g.root);}
static bool ValidVersion(const std::wstring& s,Version* result=nullptr){if(s.empty()||s.size()>63)return false;Version v{};unsigned part=0;unsigned long long value=0;size_t digits=0;for(size_t i=0;i<=s.size();++i){wchar_t ch=i<s.size()?s[i]:L'.';if(ch>=L'0'&&ch<=L'9'){if(!digits&&ch==L'0'&&i+1<s.size()&&s[i+1]!=L'.')return false;if(value>429496729ULL)return false;value=value*10+(ch-L'0');++digits;}else if(ch==L'.'&&digits&&part<3){if(value>0xffffffffULL)return false;if(!part)v.a=value;else if(part==1)v.b=value;else v.c=value;++part;value=0;digits=0;}else return false;}if(part!=3)return false;if(result)*result=v;return true;}
static bool ReadSmall(const std::wstring& p,std::wstring& out){out.clear();HANDLE h=CreateFileW(p.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);if(h==INVALID_HANDLE_VALUE)return false;LARGE_INTEGER z{};bool ok=GetFileSizeEx(h,&z)&&z.QuadPart>=0&&z.QuadPart<=256;std::vector<char>b(ok?(size_t)z.QuadPart:0);DWORD got=0;if(ok&&z.QuadPart)ok=ReadFile(h,b.data(),(DWORD)b.size(),&got,nullptr)&&got==b.size();CloseHandle(h);if(!ok)return false;if(!b.empty()&&b.back()=='\n'){b.pop_back();if(!b.empty()&&b.back()=='\r')b.pop_back();}if(b.empty())return false;for(char c:b){if(c<0x20||c>0x7e)return false;out.push_back((wchar_t)(unsigned char)c);}return true;}
static bool AtomicWriteText(const std::wstring& p,const std::wstring& value){std::wstring tmp=p+L".tmp."+std::to_wstring(GetCurrentProcessId());DeleteFileW(tmp.c_str());HANDLE h=CreateFileW(tmp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH,nullptr);if(h==INVALID_HANDLE_VALUE)return false;std::string bytes;for(wchar_t c:value){if(c>0x7f){CloseHandle(h);DeleteFileW(tmp.c_str());return false;}bytes.push_back((char)c);}DWORD n=0;bool ok=WriteFile(h,bytes.data(),(DWORD)bytes.size(),&n,nullptr)&&n==bytes.size()&&FlushFileBuffers(h);CloseHandle(h);if(ok){if(Exists(p))ok=ReplaceFileW(p.c_str(),tmp.c_str(),nullptr,REPLACEFILE_WRITE_THROUGH,nullptr,nullptr)!=FALSE;else ok=MoveFileExW(tmp.c_str(),p.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE;}if(!ok)DeleteFileW(tmp.c_str());return ok;}
static bool AtomicWriteLine(const std::wstring& p,const std::wstring& value){return AtomicWriteText(p,value+L"\n");}
static unsigned long long FileTimeValue(const FILETIME& value){return ((unsigned long long)value.dwHighDateTime<<32)|value.dwLowDateTime;}
static bool RecentlyWritten(const std::wstring& path,DWORD intervalMs){WIN32_FILE_ATTRIBUTE_DATA data{};FILETIME now{};if(!GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&data))return false;GetSystemTimeAsFileTime(&now);unsigned long long current=FileTimeValue(now),written=FileTimeValue(data.ftLastWriteTime),interval=(unsigned long long)intervalMs*10000;return current<=written||current-written<interval;}
static bool ReadBytes(const std::wstring& path,unsigned long long cap,std::vector<char>& bytes){bytes.clear();HANDLE h=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);if(h==INVALID_HANDLE_VALUE)return false;LARGE_INTEGER size{};bool ok=GetFileSizeEx(h,&size)&&size.QuadPart>0&&(unsigned long long)size.QuadPart<=cap;bytes.resize(ok?(size_t)size.QuadPart:0);DWORD got=0;if(ok)ok=ReadFile(h,bytes.data(),(DWORD)bytes.size(),&got,nullptr)&&got==bytes.size();CloseHandle(h);if(!ok)bytes.clear();return ok;}
static bool ExecutableSubsystem(const std::wstring& path,WORD& subsystem){HANDLE h=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);if(h==INVALID_HANDLE_VALUE)return false;IMAGE_DOS_HEADER dos{};IMAGE_FILE_HEADER file{};LARGE_INTEGER size{},at{};DWORD got=0,signature=0;WORD magic=0;bool ok=GetFileSizeEx(h,&size)&&ReadFile(h,&dos,sizeof(dos),&got,nullptr)&&got==sizeof(dos)&&dos.e_magic==IMAGE_DOS_SIGNATURE&&dos.e_lfanew>0&&dos.e_lfanew<=size.QuadPart-(LONGLONG)(sizeof(signature)+sizeof(file)+sizeof(magic));if(ok){at.QuadPart=dos.e_lfanew;ok=SetFilePointerEx(h,at,nullptr,FILE_BEGIN)&&ReadFile(h,&signature,sizeof(signature),&got,nullptr)&&got==sizeof(signature)&&signature==IMAGE_NT_SIGNATURE&&ReadFile(h,&file,sizeof(file),&got,nullptr)&&got==sizeof(file)&&ReadFile(h,&magic,sizeof(magic),&got,nullptr)&&got==sizeof(magic);if(ok){DWORD offset=magic==IMAGE_NT_OPTIONAL_HDR32_MAGIC?FIELD_OFFSET(IMAGE_OPTIONAL_HEADER32,Subsystem):magic==IMAGE_NT_OPTIONAL_HDR64_MAGIC?FIELD_OFFSET(IMAGE_OPTIONAL_HEADER64,Subsystem):0;bool crinklerHeader=dos.e_lfanew==4&&file.Machine==IMAGE_FILE_MACHINE_I386&&file.NumberOfSections==0&&file.SizeOfOptionalHeader==8&&magic==IMAGE_NT_OPTIONAL_HDR32_MAGIC;ok=offset&&(file.SizeOfOptionalHeader>=offset+sizeof(subsystem)||crinklerHeader);if(ok){at.QuadPart=(LONGLONG)dos.e_lfanew+sizeof(signature)+sizeof(file)+offset;ok=SetFilePointerEx(h,at,nullptr,FILE_BEGIN)&&ReadFile(h,&subsystem,sizeof(subsystem),&got,nullptr)&&got==sizeof(subsystem);}}}CloseHandle(h);return ok;}
static bool IsGuiExecutable(const std::wstring& path){WORD subsystem=0;return ExecutableSubsystem(path,subsystem)&&subsystem==IMAGE_SUBSYSTEM_WINDOWS_GUI;}
bool IsConsoleApplicationExecutable(const std::wstring& path){WORD subsystem=0;return ExecutableSubsystem(path,subsystem)&&subsystem==IMAGE_SUBSYSTEM_WINDOWS_CUI;}
static bool ValidLauncherRuntime(const std::wstring& path){DWORD type=0;WIN32_FILE_ATTRIBUTE_DATA data{};return IsGuiExecutable(path)&&GetBinaryTypeW(path.c_str(),&type)&&type==SCS_32BIT_BINARY&&GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&data)&&(((unsigned long long)data.nFileSizeHigh<<32)|data.nFileSizeLow)<=Limits::MaxApplicationSize;}
static bool SameFileBytes(const std::wstring& a,const std::wstring& b){std::vector<char> x,y;return ReadBytes(a,Limits::MaxApplicationSize,x)&&ReadBytes(b,Limits::MaxApplicationSize,y)&&x==y;}

struct IniValues {bool version=false,download=false,mode=false;std::wstring versionValue,downloadValue;UpdateMode modeValue=UpdateMode::No;};
static bool ValidHttpValidator(const std::wstring& value){if(value.empty()||value.size()>512)return false;for(wchar_t c:value)if(c<L' '||c>0x7e)return false;return true;}
static bool ParseIni(const std::vector<char>& bytes,IniValues& values){if(bytes.empty()||bytes.size()>Limits::MaxUpdateConfigurationSize)return false;std::string text(bytes.begin(),bytes.end());size_t pos=0;while(pos<text.size()){size_t end=text.find('\n',pos);if(end==std::string::npos)end=text.size();std::string line=text.substr(pos,end-pos);if(!line.empty()&&line.back()=='\r')line.pop_back();pos=end+1;if(line.empty())continue;if(line.size()>5000)return false;for(unsigned char c:line)if(c<0x20||c==0x7f)return false;size_t eq=line.find('=');if(eq==std::string::npos||!eq||eq==line.size()-1)return false;std::string key=line.substr(0,eq),value=line.substr(eq+1);int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),(int)value.size(),nullptr,0);if(n<=0)return false;std::wstring w(n,0);if(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),(int)value.size(),w.data(),n)!=n)return false;if(key=="version"){if(values.version)return false;values.version=true;values.versionValue=w;}else if(key=="download"){if(values.download)return false;values.download=true;values.downloadValue=w;}else if(key=="updates"){if(values.mode)return false;values.mode=true;if(!ParseMode(w,values.modeValue))return false;}else return false;}
return values.version&&values.download&&ValidVersion(values.versionValue);}
static bool ParseUpdateState(const std::vector<char>& bytes,UpdateMode& mode,HttpValidators& validators){mode=UpdateMode::No;validators=HttpValidators{};if(bytes.empty()||bytes.size()>Limits::MaxUpdateConfigurationSize)return false;std::string text(bytes.begin(),bytes.end());bool haveMode=false,haveEtag=false,haveLastModified=false,any=false;size_t pos=0;while(pos<text.size()){size_t end=text.find('\n',pos);if(end==std::string::npos)end=text.size();std::string line=text.substr(pos,end-pos);if(!line.empty()&&line.back()=='\r')line.pop_back();pos=end+1;if(line.empty())continue;size_t eq=line.find('=');if(eq==std::string::npos||!eq||eq==line.size()-1)return false;std::string key=line.substr(0,eq),value=line.substr(eq+1);for(unsigned char c:value)if(c<0x20||c>0x7e)return false;std::wstring wide(value.begin(),value.end());if(key=="updates"){if(haveMode||!ParseMode(wide,mode))return false;haveMode=true;}else if(key=="etag"){if(haveEtag||!ValidHttpValidator(wide))return false;validators.etag=wide;haveEtag=true;}else if(key=="last_modified"){if(haveLastModified||!ValidHttpValidator(wide))return false;validators.lastModified=wide;haveLastModified=true;}else return false;any=true;}return any;}
static bool AtomicWriteUtf8(const std::wstring& path,const std::wstring& value);
static bool WriteUpdateState(const std::wstring& path,bool includeMode,UpdateMode mode,const HttpValidators& validators){std::wstring text;if(includeMode&&mode!=UpdateMode::No)text=L"updates="+std::wstring(ModeName(mode))+L"\n";if(ValidHttpValidator(validators.etag))text+=L"etag="+validators.etag+L"\n";if(ValidHttpValidator(validators.lastModified))text+=L"last_modified="+validators.lastModified+L"\n";if(text.empty())return DeleteFileW(path.c_str())||GetLastError()==ERROR_FILE_NOT_FOUND;return AtomicWriteUtf8(path,text);}
static void LoadPersistedOverrides(){cfg.displayName=cfg.fallbackDisplayName;cfg.mode=UpdateMode::No;gApplicationValidators=HttpValidators{};DWORD lock=gConfigurationMutex?WaitForSingleObject(gConfigurationMutex,INFINITE):WAIT_OBJECT_0;bool owns=lock==WAIT_OBJECT_0||lock==WAIT_ABANDONED;std::vector<char> bytes;UpdateMode mode=UpdateMode::No;HttpValidators validators;if(owns&&ReadBytes(g.updateState,Limits::MaxUpdateConfigurationSize,bytes)&&ParseUpdateState(bytes,mode,validators)){cfg.mode=mode;gApplicationValidators=validators;}if(owns&&gConfigurationMutex)ReleaseMutex(gConfigurationMutex);}
static bool PersistApplicationState(){DWORD lock=gConfigurationMutex?WaitForSingleObject(gConfigurationMutex,INFINITE):WAIT_OBJECT_0;bool owns=lock==WAIT_OBJECT_0||lock==WAIT_ABANDONED;bool ok=owns&&WriteUpdateState(g.updateState,true,cfg.mode,gApplicationValidators);if(owns&&gConfigurationMutex)ReleaseMutex(gConfigurationMutex);return ok;}
static void WarnPersistence(){if(gBackgroundUpdate||gPersistenceWarningShown)return;gPersistenceWarningShown=true;MessageBoxW(nullptr,L"The application update state could not be saved. Updating and launching will continue.",cfg.displayName.c_str(),MB_ICONWARNING);}

struct ApplicationCandidates {unsigned total=0,root=0,gui=0;std::wstring totalPath,rootPath,guiPath;};
static void FindApplications(const std::wstring& directory,ApplicationCandidates& candidates,unsigned depth,unsigned& visited,bool& complete){if(!complete)return;if(depth>32||visited>10000){complete=false;return;}WIN32_FIND_DATAW f{};HANDLE h=FindFirstFileW((directory+L"\\*").c_str(),&f);if(h==INVALID_HANDLE_VALUE)return;do{if(!wcscmp(f.cFileName,L".")||!wcscmp(f.cFileName,L".."))continue;if(++visited>10000){complete=false;break;}std::wstring path=directory+L"\\"+f.cFileName;if(f.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)continue;if(f.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)FindApplications(path,candidates,depth+1,visited,complete);else if(EndsWithInsensitive(f.cFileName,L".exe")){++candidates.total;candidates.totalPath=path;if(!depth){++candidates.root;candidates.rootPath=path;}if(IsGuiExecutable(path)){++candidates.gui;candidates.guiPath=path;}}}while(complete&&FindNextFileW(h,&f));FindClose(h);}
static std::wstring AppPath(const std::wstring& v){if(v==gResolvedVersion&&!gResolvedApplication.empty()&&Exists(gResolvedApplication))return gResolvedApplication;ApplicationCandidates candidates;unsigned visited=0;bool complete=true;FindApplications(g.versions+L"\\"+v,candidates,0,visited,complete);std::wstring found;if(complete){if(candidates.total==1)found=candidates.totalPath;else if(candidates.root==1)found=candidates.rootPath;else if(candidates.gui==1)found=candidates.guiPath;}if(!found.empty()){gResolvedVersion=v;gResolvedApplication=found;}return found;}
static bool CurrentValid(std::wstring& v){if(!ReadSmall(g.current,v)||!ValidVersion(v))return false;std::wstring app=AppPath(v);return !app.empty()&&Exists(app);}
static std::wstring ConditionalHeaders(const HttpValidators& validators){std::wstring headers;if(ValidHttpValidator(validators.etag))headers=L"If-None-Match: "+validators.etag+L"\r\n";if(ValidHttpValidator(validators.lastModified))headers+=L"If-Modified-Since: "+validators.lastModified+L"\r\n";return headers;}
static bool HttpGetUrl(const std::wstring& initial,std::vector<char>* memory,const std::wstring* file,unsigned long long cap,DWORD timeout,const wchar_t* headers=nullptr,HttpValidators* responseValidators=nullptr){
    bool ok=false;HANDLE output=INVALID_HANDLE_VALUE;if(memory)memory->clear();if(responseValidators)*responseValidators=HttpValidators{};if(file){DeleteFileW(file->c_str());output=CreateFileW(file->c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_SEQUENTIAL_SCAN,nullptr);if(output==INVALID_HANDLE_VALUE)return false;}URL_COMPONENTS initialParts{};initialParts.dwStructSize=sizeof(initialParts);initialParts.dwHostNameLength=(DWORD)-1;bool localDirect=WinHttpCrackUrl(initial.c_str(),0,0,&initialParts)&&initialParts.dwHostNameLength&&IsLocalHost(std::wstring(initialParts.lpszHostName,initialParts.dwHostNameLength));HINTERNET session=WinHttpOpen(L"1KB/1",localDirect?WINHTTP_ACCESS_TYPE_NO_PROXY:WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);if(!session){if(output!=INVALID_HANDLE_VALUE){CloseHandle(output);DeleteFileW(file->c_str());}return false;}WinHttpSetTimeouts(session,timeout,timeout,timeout,timeout);std::wstring requestUrl=initial;
    for(unsigned redirect=0;redirect<=5;++redirect){URL_COMPONENTS u{};u.dwStructSize=sizeof(u);u.dwHostNameLength=(DWORD)-1;u.dwUrlPathLength=(DWORD)-1;u.dwExtraInfoLength=(DWORD)-1;u.dwUserNameLength=(DWORD)-1;u.dwPasswordLength=(DWORD)-1;if(!WinHttpCrackUrl(requestUrl.c_str(),0,0,&u)||!u.dwHostNameLength||u.dwUserNameLength||u.dwPasswordLength)break;std::wstring host(u.lpszHostName,u.dwHostNameLength),object(u.lpszUrlPath,u.dwUrlPathLength);bool secure=u.nScheme==INTERNET_SCHEME_HTTPS;if((!secure&&u.nScheme!=INTERNET_SCHEME_HTTP)||(!secure&&!IsLocalHost(host)))break;if(u.dwExtraInfoLength)object.append(u.lpszExtraInfo,u.dwExtraInfoLength);const wchar_t* connectHost=_wcsicmp(host.c_str(),L"localhost")==0?L"127.0.0.1":host.c_str();HINTERNET connect=WinHttpConnect(session,connectHost,u.nPort,0);HINTERNET request=connect?WinHttpOpenRequest(connect,L"GET",object.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,secure?WINHTTP_FLAG_SECURE:0):nullptr;DWORD disable=WINHTTP_DISABLE_REDIRECTS;if(request&&!WinHttpSetOption(request,WINHTTP_OPTION_DISABLE_FEATURE,&disable,sizeof(disable))){WinHttpCloseHandle(request);request=nullptr;}bool received=request&&WinHttpSendRequest(request,headers,headers?(DWORD)-1L:0,WINHTTP_NO_REQUEST_DATA,0,0,0)&&WinHttpReceiveResponse(request,nullptr);DWORD status=0,n=sizeof(status);if(received)received=WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,nullptr,&status,&n,nullptr)!=FALSE;
        if(received&&(status==301||status==302||status==303||status==307||status==308)&&redirect<5){DWORD bytes=0;WinHttpQueryHeaders(request,WINHTTP_QUERY_LOCATION,nullptr,nullptr,&bytes,nullptr);std::vector<wchar_t> location(bytes/sizeof(wchar_t)+1);bool moved=bytes&&WinHttpQueryHeaders(request,WINHTTP_QUERY_LOCATION,nullptr,location.data(),&bytes,nullptr)!=FALSE;WinHttpCloseHandle(request);if(connect)WinHttpCloseHandle(connect);if(!moved)break;requestUrl=location.data();continue;}
        if(received&&status==200){DWORD declared=0;n=sizeof(declared);bool hasLength=WinHttpQueryHeaders(request,WINHTTP_QUERY_CONTENT_LENGTH|WINHTTP_QUERY_FLAG_NUMBER,nullptr,&declared,&n,nullptr)!=FALSE;if(hasLength&&declared>cap){WinHttpCloseHandle(request);if(connect)WinHttpCloseHandle(connect);break;}unsigned long long total=0;char block[32768];ok=true;for(;;){DWORD got=0;if(!WinHttpReadData(request,block,sizeof(block),&got)){ok=false;break;}if(!got)break;total+=got;if(total>cap){ok=false;break;}if(memory)memory->insert(memory->end(),block,block+got);else{DWORD wrote=0;if(!WriteFile(output,block,got,&wrote,nullptr)||wrote!=got){ok=false;break;}if(file&&hasLength)UpdateUiDownloadProgress(total,declared);}}if(ok&&responseValidators){auto readValidator=[&](DWORD query,std::wstring& destination){DWORD bytes=0;WinHttpQueryHeaders(request,query,nullptr,nullptr,&bytes,nullptr);if(!bytes||bytes>513*sizeof(wchar_t))return;std::vector<wchar_t> value(bytes/sizeof(wchar_t)+1);if(WinHttpQueryHeaders(request,query,nullptr,value.data(),&bytes,nullptr)){std::wstring parsed(value.data());if(ValidHttpValidator(parsed))destination=parsed;}};readValidator(WINHTTP_QUERY_ETAG,responseValidators->etag);readValidator(WINHTTP_QUERY_LAST_MODIFIED,responseValidators->lastModified);}}
        else if(received)gLastHttpStatus=status;
        if(request)WinHttpCloseHandle(request);if(connect)WinHttpCloseHandle(connect);break;
    }WinHttpCloseHandle(session);if(output!=INVALID_HANDLE_VALUE){if(ok)ok=FlushFileBuffers(output)!=FALSE;CloseHandle(output);if(!ok)DeleteFileW(file->c_str());}return ok;
}
static bool ValidateDownloadUrl(const std::wstring& url,std::wstring& path){if(url.empty()||url.size()>2048||url.find(L"..")!=std::wstring::npos||url.find(L'%')!=std::wstring::npos)return false;URL_COMPONENTS u{};u.dwStructSize=sizeof(u);u.dwHostNameLength=(DWORD)-1;u.dwUrlPathLength=(DWORD)-1;u.dwExtraInfoLength=(DWORD)-1;u.dwUserNameLength=(DWORD)-1;u.dwPasswordLength=(DWORD)-1;if(!WinHttpCrackUrl(url.c_str(),0,0,&u)||!u.dwHostNameLength||u.dwUserNameLength||u.dwPasswordLength||u.dwExtraInfoLength)return false;std::wstring host(u.lpszHostName,u.dwHostNameLength);path.assign(u.lpszUrlPath,u.dwUrlPathLength);bool secure=u.nScheme==INTERNET_SCHEME_HTTPS;if((!secure&&u.nScheme!=INTERNET_SCHEME_HTTP)||(!secure&&!IsLocalHost(host))||!SafeUrlPath(path)||path.size()>=1024)return false;size_t slash=path.find_last_of(L'/');std::wstring name=path.substr(slash+1);if(name.size()<4||name.size()>255)return false;if(!EndsWithInsensitive(name,L".exe")&&!EndsWithInsensitive(name,L".zip")&&!EndsWithInsensitive(name,L".1KB"))return false;for(wchar_t c:path)if(!((c>=L'a'&&c<=L'z')||(c>=L'A'&&c<=L'Z')||(c>=L'0'&&c<=L'9')||c==L'-'||c==L'_'||c==L'.'||c==L'/'))return false;return true;}
static bool GithubDownloadUrl(const std::wstring& url);
static bool GithubDownloadPath(const std::wstring& url,std::wstring& path);
static bool ResolveDownloadUrl(const std::wstring& value,const std::wstring& base,std::wstring& resolved){if(value.empty()||value.size()>2048||value.find(L"..")!=std::wstring::npos||value.find(L'%')!=std::wstring::npos||value.find(L'\\')!=std::wstring::npos||value.find(L'?')!=std::wstring::npos||value.find(L'#')!=std::wstring::npos)return false;if(value.find(L"://")!=std::wstring::npos)resolved=value;else{URL_COMPONENTS u{};u.dwStructSize=sizeof(u);u.dwHostNameLength=(DWORD)-1;u.dwUrlPathLength=(DWORD)-1;if(!WinHttpCrackUrl(base.c_str(),0,0,&u)||!u.dwHostNameLength)return false;size_t originLength=(size_t)(u.lpszUrlPath-base.c_str());if(value[0]==L'/')resolved=base.substr(0,originLength)+value;else{size_t slash=base.find_last_of(L'/');if(slash==std::wstring::npos)return false;resolved=base.substr(0,slash+1)+value;}}std::wstring object;return ValidateDownloadUrl(resolved,object)||GithubDownloadUrl(resolved);}
static bool EndsWithInsensitive(const std::wstring& value,const wchar_t* suffix){size_t n=wcslen(suffix);return value.size()>=n&&_wcsicmp(value.c_str()+value.size()-n,suffix)==0;}
static bool GithubDownloadPath(const std::wstring& url,std::wstring& path){SourceInfo source;if(url.size()>2048||!ParseDirectUrl(url,&source)||!source.https||source.host!=L"github.com"||!source.query.empty()||source.path.size()>=1024||source.path.find(L"/releases/download/")==std::wstring::npos)return false;size_t slash=source.path.find_last_of(L'/');std::wstring name=source.path.substr(slash+1);if(name.size()<4||name.size()>255||(!EndsWithInsensitive(name,L".exe")&&!EndsWithInsensitive(name,L".zip")&&!EndsWithInsensitive(name,L".1KB")))return false;path=source.path;return true;}
static bool GithubDownloadUrl(const std::wstring& url){std::wstring path;return GithubDownloadPath(url,path);}
static bool ParseRemoteConfiguration(const std::vector<char>& bytes,const std::wstring& base,UpdateResponse& response){IniValues values;if(!ParseIni(bytes,values))return false;std::wstring resolved;if(!ResolveDownloadUrl(values.downloadValue,base,resolved))return false;bool encrypted=EndsWithInsensitive(resolved,L".1KB"),plain=EndsWithInsensitive(resolved,L".exe")||EndsWithInsensitive(resolved,L".zip");if((gLauncherOverlay.encryption==PayloadEncryption::Launcher&&!encrypted)||(gLauncherOverlay.encryption==PayloadEncryption::None&&!plain))return false;HttpValidators validators=response.validators;response=UpdateResponse{};response.version=values.versionValue;response.download=resolved;response.githubDownload=GithubDownloadUrl(resolved);response.mode=values.mode?values.modeValue:UpdateMode::No;response.validators=validators;return true;}
enum class ResolveResult {Failed,NotModified,Modified};
static ResolveResult ResolveApplicationSource(const SourceInfo& source,bool conditional,UpdateResponse& response){gLastHttpStatus=0;std::vector<char> bytes;std::wstring headers=conditional?ConditionalHeaders(gApplicationValidators):L"";response=UpdateResponse{};if(!HttpGetUrl(source.url,&bytes,nullptr,Limits::MaxUpdateConfigurationSize,Limits::UpdateConfigurationTimeoutMs,headers.empty()?nullptr:headers.c_str(),&response.validators))return gLastHttpStatus==304?ResolveResult::NotModified:ResolveResult::Failed;return ParseRemoteConfiguration(bytes,source.url,response)?ResolveResult::Modified:ResolveResult::Failed;}
static bool WideUtf8(const std::wstring& value,std::string& bytes){int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),(int)value.size(),nullptr,0,nullptr,nullptr);if(n<=0)return false;bytes.resize(n);return WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),(int)value.size(),bytes.data(),n,nullptr,nullptr)==n;}
static bool AtomicWriteUtf8(const std::wstring& path,const std::wstring& value){std::string bytes;if(!WideUtf8(value,bytes))return false;std::wstring tmp=path+L".tmp."+std::to_wstring(GetCurrentProcessId());DeleteFileW(tmp.c_str());HANDLE h=CreateFileW(tmp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH,nullptr);if(h==INVALID_HANDLE_VALUE)return false;DWORD wrote=0;bool ok=WriteFile(h,bytes.data(),(DWORD)bytes.size(),&wrote,nullptr)&&wrote==bytes.size()&&FlushFileBuffers(h);CloseHandle(h);if(ok)ok=Exists(path)?ReplaceFileW(path.c_str(),tmp.c_str(),nullptr,REPLACEFILE_WRITE_THROUGH,nullptr,nullptr)!=FALSE:MoveFileExW(tmp.c_str(),path.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE;if(!ok)DeleteFileW(tmp.c_str());return ok;}
static bool HaveValidators(const HttpValidators& validators){return ValidHttpValidator(validators.etag)||ValidHttpValidator(validators.lastModified);}
static ResolveResult ResolveRemote(bool haveCurrent,UpdateResponse& response){bool conditional=haveCurrent&&HaveValidators(gApplicationValidators);ResolveResult result=ResolveApplicationSource(cfg.identity.source,conditional,response);if(result==ResolveResult::NotModified&&!conditional)return ResolveResult::Failed;if(result==ResolveResult::Modified){cfg.mode=response.mode;if(!PersistApplicationState())WarnPersistence();}return result;}
static void CommitApplicationValidators(const HttpValidators& validators){gApplicationValidators=validators;if(!PersistApplicationState())WarnPersistence();}

static bool AppRunning(HANDLE* returned=nullptr);
static void DeleteTree(const std::wstring& p){WIN32_FIND_DATAW f{};HANDLE h=FindFirstFileW((p+L"\\*").c_str(),&f);if(h!=INVALID_HANDLE_VALUE){do{if(wcscmp(f.cFileName,L".")&&wcscmp(f.cFileName,L"..")){std::wstring q=p+L"\\"+f.cFileName;if(f.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)DeleteTree(q);else{SetFileAttributesW(q.c_str(),FILE_ATTRIBUTE_NORMAL);DeleteFileW(q.c_str());}}}while(FindNextFileW(h,&f));FindClose(h);}RemoveDirectoryW(p.c_str());}
static bool ProcessPathRunning(const std::wstring& wanted,bool directory){std::wstring match=directory?wanted+L"\\":wanted;HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);if(snapshot==INVALID_HANDLE_VALUE)return true;PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);bool running=false;if(Process32FirstW(snapshot,&entry))do{HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,entry.th32ProcessID);if(process){wchar_t path[32768];DWORD size=_countof(path);if(QueryFullProcessImageNameW(process,0,path,&size)&&(directory?(size>=match.size()&&!_wcsnicmp(path,match.c_str(),match.size())):!_wcsicmp(path,match.c_str())))running=true;CloseHandle(process);}}while(!running&&Process32NextW(snapshot,&entry));CloseHandle(snapshot);return running;}
static bool DirectoryRunning(const std::wstring& directory){return ProcessPathRunning(directory,true);}
static bool TrackRunningApplication(const std::wstring& wanted,const std::wstring& version,LaunchedProcess& launched){HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);if(snapshot==INVALID_HANDLE_VALUE)return false;PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);bool found=false;if(Process32FirstW(snapshot,&entry))do{HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|PROCESS_TERMINATE|SYNCHRONIZE,FALSE,entry.th32ProcessID);if(process){wchar_t path[32768];DWORD size=_countof(path);if(QueryFullProcessImageNameW(process,0,path,&size)&&!_wcsicmp(path,wanted.c_str())){launched.handle=process;launched.id=entry.th32ProcessID;launched.version=version;found=true;}else CloseHandle(process);}}while(!found&&Process32NextW(snapshot,&entry));CloseHandle(snapshot);return found;}
static void Cleanup(const std::wstring& current){
    WIN32_FIND_DATAW f{};HANDLE h=FindFirstFileW((g.versions+L"\\*").c_str(),&f);if(h==INVALID_HANDLE_VALUE)return;do{std::wstring n=f.cFileName,path=g.versions+L"\\"+n;if((f.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)&&!(f.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)&&n!=L"."&&n!=L".."&&n!=current&&ValidVersion(n)&&!DirectoryRunning(path))DeleteTree(path);else if(!(f.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)&&n.rfind(L"current.txt.tmp.",0)==0)DeleteFileW(path.c_str());}while(FindNextFileW(h,&f));FindClose(h);
}
static std::wstring ObjectName(const wchar_t* suffix){return L"Local\\"+cfg.identity.mutexKey+suffix;}
static bool AppRunning(HANDLE* returned){std::wstring name=ObjectName(L"_AppRunning");HANDLE h=OpenMutexW(SYNCHRONIZE|MUTEX_MODIFY_STATE,FALSE,name.c_str());if(returned)*returned=h;else if(h)CloseHandle(h);return h!=nullptr;}
static bool StartApplication(const std::wstring& version,LaunchedProcess* launched){std::wstring exe=AppPath(version);if(exe.empty()||!Exists(exe))return false;std::wstring cmd=L"\""+exe+L"\""+(gAppArguments.empty()?L"":L" "+gAppArguments);std::vector<wchar_t> mutableCmd(cmd.begin(),cmd.end());mutableCmd.push_back(0);size_t slash=exe.find_last_of(L"\\/");std::wstring working=slash==std::wstring::npos?g.root:exe.substr(0,slash);STARTUPINFOW si{};si.cb=sizeof(si);si.dwFlags=STARTF_FORCEOFFFEEDBACK;if(gConsoleLauncher){si.dwFlags|=STARTF_USESTDHANDLES;si.hStdInput=GetStdHandle(STD_INPUT_HANDLE);si.hStdOutput=GetStdHandle(STD_OUTPUT_HANDLE);si.hStdError=GetStdHandle(STD_ERROR_HANDLE);}PROCESS_INFORMATION pi{};bool environment=SetEnvironmentVariableW(L"ONEKB_PATH",gLauncherPath.c_str())&&SetEnvironmentVariableW(L"ONEKB_VERSION",version.c_str())&&SetEnvironmentVariableW(L"ONEKB_VERSION_FILE",g.current.c_str());BOOL ok=environment&&CreateProcessW(exe.c_str(),mutableCmd.data(),nullptr,nullptr,gConsoleLauncher,0,nullptr,working.c_str(),&si,&pi);SetEnvironmentVariableW(L"ONEKB_VERSION_FILE",nullptr);SetEnvironmentVariableW(L"ONEKB_VERSION",nullptr);SetEnvironmentVariableW(L"ONEKB_PATH",nullptr);if(ok){CloseHandle(pi.hThread);if(launched){launched->handle=pi.hProcess;launched->id=pi.dwProcessId;launched->version=version;}else CloseHandle(pi.hProcess);}return ok!=FALSE;}
static bool LaunchInstalled(LaunchedProcess* launched){std::wstring current;if(!CurrentValid(current))return false;if(AppRunning()){if(launched)TrackRunningApplication(AppPath(current),current,*launched);return true;}return StartApplication(current,launched);}
static bool SafeZipListing(const std::wstring& archive){
    std::wstring listing=archive+L".list."+std::to_wstring(GetCurrentProcessId());DeleteFileW(listing.c_str());SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};HANDLE log=CreateFileW(listing.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,&security,CREATE_NEW,FILE_ATTRIBUTE_TEMPORARY,nullptr);if(log==INVALID_HANDLE_VALUE)return false;wchar_t system[MAX_PATH]{};bool ok=GetSystemDirectoryW(system,_countof(system))!=0;std::wstring tar=std::wstring(system)+L"\\tar.exe",command=L"\""+tar+L"\" -tf \""+archive+L"\"";std::vector<wchar_t> mutableCommand(command.begin(),command.end());mutableCommand.push_back(0);STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESTDHANDLES;startup.hStdInput=GetStdHandle(STD_INPUT_HANDLE);startup.hStdOutput=log;startup.hStdError=log;PROCESS_INFORMATION process{};if(ok)ok=CreateProcessW(tar.c_str(),mutableCommand.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process)!=FALSE;if(ok){WaitForSingleObject(process.hProcess,INFINITE);DWORD code=1;ok=GetExitCodeProcess(process.hProcess,&code)&&code==0;CloseHandle(process.hThread);CloseHandle(process.hProcess);}FlushFileBuffers(log);LARGE_INTEGER size{},zero{};ok=ok&&GetFileSizeEx(log,&size)&&size.QuadPart>0&&size.QuadPart<=4*1024*1024&&SetFilePointerEx(log,zero,nullptr,FILE_BEGIN);std::vector<char> bytes(ok?(size_t)size.QuadPart:0);DWORD got=0;if(ok)ok=ReadFile(log,bytes.data(),(DWORD)bytes.size(),&got,nullptr)&&got==bytes.size();CloseHandle(log);DeleteFileW(listing.c_str());if(!ok)return false;size_t at=0;unsigned entries=0;while(at<bytes.size()){size_t end=at;while(end<bytes.size()&&bytes[end]!='\r'&&bytes[end]!='\n')++end;if(end>at){if(++entries>10000)return false;std::string value(bytes.data()+at,bytes.data()+end);for(char& c:value)if(c=='\\')c='/';if(value.empty()||value[0]=='/'||value.find(':')!=std::string::npos)return false;size_t part=0;for(;;){size_t slash=value.find('/',part);if(value.substr(part,slash==std::string::npos?slash:slash-part)=="..")return false;if(slash==std::string::npos)break;part=slash+1;}}at=end;while(at<bytes.size()&&(bytes[at]=='\r'||bytes[at]=='\n'))++at;}return entries!=0;
}
static bool ExtractZip(const std::wstring& archive,const std::wstring& destination){if(!SafeZipListing(archive))return false;wchar_t system[MAX_PATH]{};if(!GetSystemDirectoryW(system,_countof(system)))return false;std::wstring tar=std::wstring(system)+L"\\tar.exe",command=L"\""+tar+L"\" -xf \""+archive+L"\" -C \""+destination+L"\"";std::vector<wchar_t> mutableCommand(command.begin(),command.end());mutableCommand.push_back(0);STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};if(!CreateProcessW(tar.c_str(),mutableCommand.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process))return false;WaitForSingleObject(process.hProcess,INFINITE);DWORD exitCode=1;bool ok=GetExitCodeProcess(process.hProcess,&exitCode)&&exitCode==0;CloseHandle(process.hThread);CloseHandle(process.hProcess);return ok;}
static bool PrepareAndActivate(const UpdateResponse& update,bool zip,std::wstring& error){
    bool encrypted=gLauncherOverlay.encryption==PayloadEncryption::Launcher;std::wstring dir=g.versions+L"\\"+update.version;
    SetUpdateUi(L"Downloading",gUpdateProgressDetail,0);
    if(DirectoryRunning(dir)){error=L"The incomplete version directory is still in use; cleanup will be retried later.";return false;}DeleteTree(dir);if(!EnsureDir(dir)){error=L"The version directory could not be created.";return false;}
    std::wstring downloaded=dir+L"\\"+(encrypted?L"download.1KB":zip?L"download.zip":L"application.exe");unsigned long long cap=Limits::MaxApplicationSize+(encrypted?EncryptedPayloadOverhead:0);
    gLastHttpStatus=0;if(!HttpGetUrl(update.download,nullptr,&downloaded,cap,Limits::DownloadTimeoutMs)){DeleteTree(dir);error=gLastHttpStatus?L"The application server returned HTTP "+std::to_wstring(gLastHttpStatus)+L" while downloading the update.":L"The application file could not be downloaded. Check the download URL, network connection, and 100 MB size limit.";return false;}
    if(encrypted){SetUpdateUi(L"Verifying download",gUpdateProgressDetail,-1);PayloadCryptoError crypto;std::wstring plain=dir+L"\\download.zip.tmp."+std::to_wstring(GetCurrentProcessId());bool valid=ValidateEncryptedPayloadHeader(downloaded,nullptr,&crypto)&&DecryptPayloadZip(downloaded,plain,gLauncherOverlay.secret,cfg.identity.appId,update.version,&crypto);DeleteFileW(downloaded.c_str());if(!valid){DeleteFileW(plain.c_str());DeleteTree(dir);error=crypto.message;return false;}downloaded=plain;zip=true;}
    if(zip){SetUpdateUi(L"Installing",gUpdateProgressDetail,-1);bool extracted=ExtractZip(downloaded,dir);DeleteFileW(downloaded.c_str());if(!extracted){DeleteTree(dir);error=L"The downloaded ZIP file is invalid or could not be safely extracted. This launcher requires the tar.exe included with current Windows versions.";return false;}}
    SetUpdateUi(L"Installing",gUpdateProgressDetail,-1);std::wstring application=AppPath(update.version);if(application.empty()){DeleteTree(dir);error=L"The downloaded release does not have an unambiguous application executable.";return false;}
    if(!AtomicWriteLine(g.current,update.version)){DeleteTree(dir);error=L"The installed version could not be activated.";return false;}return true;
}
enum class UpdateResult { Failed,NoUpdate,Installed };
static UpdateResult CheckAndInstall(std::wstring& installed,std::wstring& error,bool restarting=false){installed.clear();error.clear();std::wstring current;bool haveCurrent=CurrentValid(current);gFreshInstall=!haveCurrent;UpdateResponse response;ResolveResult resolved=ResolveRemote(haveCurrent,response);if(resolved==ResolveResult::NotModified){CloseUpdateUi();return UpdateResult::NoUpdate;}if(resolved==ResolveResult::Failed){error=gLastHttpStatus?L"The application source server returned HTTP "+std::to_wstring(gLastHttpStatus)+L" while resolving the application manifest.":L"The application source URL could not be resolved to a valid application manifest.";return UpdateResult::Failed;}if(haveCurrent&&response.version==current){CommitApplicationValidators(response.validators);CloseUpdateUi();return UpdateResult::NoUpdate;}if(!gUpdateUi.thread&&(restarting||cfg.mode==UpdateMode::Yes))ShowUpdateUi(current);gUpdateProgressDetail=gFreshInstall?L"Version "+response.version:L"Version "+response.version+L"  |  Installed "+current;SetUpdateUi(gFreshInstall?L"Latest version found":L"Update available",gUpdateProgressDetail,0);std::wstring object;if(!ValidateDownloadUrl(response.download,object)&&!(response.githubDownload&&GithubDownloadPath(response.download,object))){error=L"The application manifest's download URL is invalid.";return UpdateResult::Failed;}if(gLauncherOverlay.encryption==PayloadEncryption::Launcher&&!EndsWithInsensitive(response.download,L".1KB")){error=L"The private launcher encryption secret is missing or invalid.";return UpdateResult::Failed;}if(!PrepareAndActivate(response,EndsWithInsensitive(object,L".zip"),error))return UpdateResult::Failed;CommitApplicationValidators(response.validators);installed=response.version;return UpdateResult::Installed;}
static bool StopTrackedApplication(LaunchedProcess& launched){if(!launched.handle)return false;DWORD code=0;if(!GetExitCodeProcess(launched.handle,&code))return false;if(code==STILL_ACTIVE&&(!TerminateProcess(launched.handle,0)||WaitForSingleObject(launched.handle,Limits::ForcedRestartTimeoutMs)!=WAIT_OBJECT_0))return false;CloseHandle(launched.handle);launched.handle=nullptr;return true;}
static bool RestartTrackedApplication(LaunchedProcess& launched,const std::wstring& installed){return StopTrackedApplication(launched)&&StartApplication(installed,&launched);}
static void CloseLaunched(LaunchedProcess& launched){if(launched.handle){CloseHandle(launched.handle);launched.handle=nullptr;}}
static int FinishLaunched(LaunchedProcess& launched,int result){if(gConsoleLauncher&&launched.handle){DWORD code=0;if(WaitForSingleObject(launched.handle,INFINITE)==WAIT_OBJECT_0&&GetExitCodeProcess(launched.handle,&code))result=(int)code;else result=2;}CloseLaunched(launched);return result;}

#ifdef ONEKB_RUNTIME_ONLY
struct RuntimeLocations {std::wstring local,state,canonical,updateState,checked;};
static bool GetRuntimeLocations(RuntimeLocations& p){if(!EnvironmentValue(L"LOCALAPPDATA",p.local)){PWSTR local=nullptr;if(FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData,KF_FLAG_CREATE,nullptr,&local)))return false;p.local=local;CoTaskMemFree(local);}std::wstring temp;if(!EnvironmentValue(L"TMP",temp))return false;p.state=p.local+L"\\1kb";p.canonical=temp+((temp.back()==L'\\'||temp.back()==L'/')?L"r":L"\\r");p.updateState=p.state+L"\\1kb.ini";p.checked=p.state+L"\\last-runtime-update-check.txt";return EnsureDir(p.state);}
static HttpValidators ReadRuntimeValidators(const std::wstring& path){std::vector<char> bytes;UpdateMode ignored=UpdateMode::No;HttpValidators validators;if(ReadBytes(path,Limits::MaxUpdateConfigurationSize,bytes)&&ParseUpdateState(bytes,ignored,validators))return validators;return HttpValidators{};}
static bool RuntimeTemporaryName(const std::wstring& name){return name.rfind(L"runtime-update-",0)==0||name.rfind(L"runtime-install-",0)==0;}
static void CleanupRuntimeTemporaryFiles(const std::wstring& state,const wchar_t* skip=nullptr){WIN32_FIND_DATAW f{};HANDLE search=FindFirstFileW((state+L"\\runtime-*.tmp").c_str(),&f);if(search==INVALID_HANDLE_VALUE)return;do{std::wstring path=state+L"\\"+f.cFileName;if(!(f.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)&&RuntimeTemporaryName(f.cFileName)&&(!skip||_wcsicmp(path.c_str(),skip)))DeleteFileW(path.c_str());}while(FindNextFileW(search,&f));FindClose(search);}
static std::wstring RuntimeApplyEvent(const wchar_t* kind,DWORD parent){return L"Local\\OneKBRuntimeApply"+std::wstring(kind)+L"_"+std::to_wstring(parent);}
static int RuntimeApplyMain(){
    HANDLE parent=OpenProcess(SYNCHRONIZE,FALSE,gRuntimeApplyParent);if(!parent)return 1;
    HANDLE mutex=CreateMutexW(nullptr,FALSE,RuntimeMutexName);HANDLE ready=CreateEventW(nullptr,TRUE,FALSE,RuntimeApplyEvent(L"Ready",gRuntimeApplyParent).c_str());HANDLE acquired=CreateEventW(nullptr,TRUE,FALSE,RuntimeApplyEvent(L"Acquired",gRuntimeApplyParent).c_str());
    if(!mutex||!ready||!acquired){if(mutex)CloseHandle(mutex);if(ready)CloseHandle(ready);if(acquired)CloseHandle(acquired);CloseHandle(parent);return 1;}
    SetEvent(ready);DWORD lock=WaitForSingleObject(mutex,INFINITE);bool owns=lock==WAIT_OBJECT_0||lock==WAIT_ABANDONED;if(!owns){CloseHandle(acquired);CloseHandle(ready);CloseHandle(mutex);CloseHandle(parent);return 1;}SetEvent(acquired);
    bool ok=WaitForSingleObject(parent,INFINITE)==WAIT_OBJECT_0;CloseHandle(parent);RuntimeLocations paths;wchar_t self[32768]{};ok=ok&&GetModuleFileNameW(nullptr,self,_countof(self))>0&&GetRuntimeLocations(paths);if(ok)CleanupRuntimeTemporaryFiles(paths.state,self);
    std::wstring install;if(ok){install=paths.state+L"\\runtime-install-"+std::to_wstring(GetCurrentProcessId())+L".tmp";DeleteFileW(install.c_str());ok=CopyFileW(self,install.c_str(),FALSE)!=FALSE;}
    if(ok){HANDLE file=CreateFileW(install.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH,nullptr);ok=file!=INVALID_HANDLE_VALUE;if(ok){ok=FlushFileBuffers(file)!=FALSE;CloseHandle(file);}ok=ok&&ValidLauncherRuntime(install);}
    if(ok)ok=MoveFileExW(install.c_str(),paths.canonical.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE;
    if(ok)WriteUpdateState(paths.updateState,false,UpdateMode::No,gRuntimeApplyValidators);
    if(!install.empty())DeleteFileW(install.c_str());ReleaseMutex(mutex);CloseHandle(acquired);CloseHandle(ready);CloseHandle(mutex);return ok?0:1;
}
static void RuntimeStartupCleanup(){RuntimeLocations paths;if(!GetRuntimeLocations(paths))return;HANDLE mutex=CreateMutexW(nullptr,FALSE,RuntimeMutexName);if(!mutex)return;DWORD lock=WaitForSingleObject(mutex,INFINITE);if(lock==WAIT_OBJECT_0||lock==WAIT_ABANDONED){CleanupRuntimeTemporaryFiles(paths.state);ReleaseMutex(mutex);}CloseHandle(mutex);}
static void PromoteRecoveryRuntime(){
    wchar_t self[32768]{};RuntimeLocations paths;if(!GetModuleFileNameW(nullptr,self,_countof(self))||!GetRuntimeLocations(paths)||!_wcsicmp(self,paths.canonical.c_str()))return;
    HANDLE mutex=CreateMutexW(nullptr,FALSE,RuntimeMutexName);if(!mutex)return;DWORD lock=WaitForSingleObject(mutex,INFINITE);bool owns=lock==WAIT_OBJECT_0||lock==WAIT_ABANDONED;if(!owns){CloseHandle(mutex);return;}
    std::wstring install=paths.state+L"\\runtime-install-"+std::to_wstring(GetCurrentProcessId())+L".tmp";DeleteFileW(install.c_str());bool ok=ValidLauncherRuntime(self)&&CopyFileW(self,install.c_str(),FALSE)!=FALSE;
    if(ok){HANDLE file=CreateFileW(install.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH,nullptr);ok=file!=INVALID_HANDLE_VALUE;if(ok){ok=FlushFileBuffers(file)!=FALSE;CloseHandle(file);}ok=ok&&ValidLauncherRuntime(install);}
    if(ok)ok=MoveFileExW(install.c_str(),paths.canonical.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE;if(!ok)DeleteFileW(install.c_str());ReleaseMutex(mutex);CloseHandle(mutex);
}
static bool StartRuntimeApply(const std::wstring& candidate,const HttpValidators& validators,HANDLE mutex,bool& owns){
    DWORD parentId=GetCurrentProcessId();HANDLE ready=CreateEventW(nullptr,TRUE,FALSE,RuntimeApplyEvent(L"Ready",parentId).c_str());HANDLE acquired=CreateEventW(nullptr,TRUE,FALSE,RuntimeApplyEvent(L"Acquired",parentId).c_str());if(!ready||!acquired){if(ready)CloseHandle(ready);if(acquired)CloseHandle(acquired);return false;}
    std::wstring parentText=std::to_wstring(parentId),etagText=L"1"+validators.etag,lastModifiedText=L"1"+validators.lastModified,command=L"\""+candidate+L"\"";std::vector<wchar_t> mutableCommand(command.begin(),command.end());mutableCommand.push_back(0);
    bool environment=SetEnvironmentVariableW(L"ONEKB_RUNTIME_APPLY",parentText.c_str())&&SetEnvironmentVariableW(L"ONEKB_RUNTIME_ETAG",etagText.c_str())&&SetEnvironmentVariableW(L"ONEKB_RUNTIME_LAST_MODIFIED",lastModifiedText.c_str());STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_FORCEOFFFEEDBACK;PROCESS_INFORMATION process{};bool started=environment&&CreateProcessW(candidate.c_str(),mutableCommand.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process);SetEnvironmentVariableW(L"ONEKB_RUNTIME_APPLY",nullptr);SetEnvironmentVariableW(L"ONEKB_RUNTIME_ETAG",nullptr);SetEnvironmentVariableW(L"ONEKB_RUNTIME_LAST_MODIFIED",nullptr);
    bool handed=false;if(started){CloseHandle(process.hThread);HANDLE waits[]={ready,process.hProcess};DWORD wait=WaitForMultipleObjects(2,waits,FALSE,10000);if(wait==WAIT_OBJECT_0){ReleaseMutex(mutex);owns=false;HANDLE handoff[]={acquired,process.hProcess};handed=WaitForMultipleObjects(2,handoff,FALSE,30000)==WAIT_OBJECT_0;}CloseHandle(process.hProcess);}CloseHandle(acquired);CloseHandle(ready);return handed;
}
#endif
static void UpdateLauncherRuntime(){
#ifdef ONEKB_RUNTIME_ONLY
    RuntimeLocations paths;if(!GetRuntimeLocations(paths))return;HANDLE mutex=CreateMutexW(nullptr,FALSE,RuntimeMutexName);if(!mutex)return;DWORD lock=WaitForSingleObject(mutex,0);bool owns=lock==WAIT_OBJECT_0||lock==WAIT_ABANDONED;if(!owns){CloseHandle(mutex);return;}CleanupRuntimeTemporaryFiles(paths.state);
    if(RecentlyWritten(paths.checked,Limits::RuntimeUpdateCheckIntervalMs)||!AtomicWriteLine(paths.checked,L"1")){ReleaseMutex(mutex);CloseHandle(mutex);return;}
    std::wstring downloaded=paths.state+L"\\runtime-update-"+std::to_wstring(GetCurrentProcessId())+L".tmp";DeleteFileW(downloaded.c_str());HttpValidators validators=ReadRuntimeValidators(paths.updateState),responseValidators;std::wstring headers=ConditionalHeaders(validators);
#ifdef ONEKB_RUNTIME_TESTS
    const wchar_t* updateUrl=gRuntimeTestUrl.empty()?LauncherRuntimeDownloadUrl:gRuntimeTestUrl.c_str();
#else
    const wchar_t* updateUrl=LauncherRuntimeDownloadUrl;
#endif
    gLastHttpStatus=0;bool responseOk=HttpGetUrl(updateUrl,nullptr,&downloaded,Limits::MaxApplicationSize,Limits::DownloadTimeoutMs,headers.empty()?nullptr:headers.c_str(),&responseValidators);if(!responseOk&&gLastHttpStatus==304){DeleteFileW(downloaded.c_str());ReleaseMutex(mutex);CloseHandle(mutex);return;}
    wchar_t active[32768]{};bool haveActive=GetModuleFileNameW(nullptr,active,_countof(active))>0;bool valid=responseOk&&ValidLauncherRuntime(downloaded);
    if(valid&&haveActive&&SameFileBytes(downloaded,active)){DeleteFileW(downloaded.c_str());WriteUpdateState(paths.updateState,false,UpdateMode::No,responseValidators);}
    else if(valid)StartRuntimeApply(downloaded,responseValidators,mutex,owns);
    else DeleteFileW(downloaded.c_str());
    if(owns)ReleaseMutex(mutex);CloseHandle(mutex);
#endif
}

#ifdef ONEKB_RUNTIME_ONLY
static bool StartWorker(const wchar_t* flag){wchar_t runtime[32768]{};if(!GetModuleFileNameW(nullptr,runtime,_countof(runtime)))return false;std::wstring command=L"\""+gLauncherPath+L"\"";std::vector<wchar_t> mutableCommand(command.begin(),command.end());mutableCommand.push_back(0);bool environment=SetEnvironmentVariableW(flag,L"yes")!=FALSE;STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_FORCEOFFFEEDBACK;PROCESS_INFORMATION process{};bool started=environment&&CreateProcessW(runtime,mutableCommand.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process);SetEnvironmentVariableW(flag,nullptr);if(started){CloseHandle(process.hThread);CloseHandle(process.hProcess);}return started;}
static bool StartRuntimeUpdate(){return StartWorker(L"ONEKB_RUNTIME_UPDATE");}
static bool StartBackgroundUpdate(){return StartWorker(L"ONEKB_BACKGROUND");}
static int BackgroundUpdateMain(){if(!InitPaths())return 1;HANDLE updateMutex=CreateMutexW(nullptr,FALSE,ObjectName(L"_LauncherUpdate").c_str());if(!updateMutex)return 1;gConfigurationMutex=CreateMutexW(nullptr,FALSE,ObjectName(L"_LauncherConfiguration").c_str());if(!gConfigurationMutex){CloseHandle(updateMutex);return 1;}DWORD lock=WaitForSingleObject(updateMutex,INFINITE);if(lock==WAIT_OBJECT_0||lock==WAIT_ABANDONED){LoadPersistedOverrides();std::wstring installed,error;CheckAndInstall(installed,error);std::wstring current;if(CurrentValid(current))Cleanup(current);UpdateLauncherRuntime();ReleaseMutex(updateMutex);}CloseHandle(gConfigurationMutex);gConfigurationMutex=nullptr;CloseHandle(updateMutex);return 0;}
#else
static bool StartRuntimeUpdate(){return false;}
static bool StartBackgroundUpdate(){return false;}
#endif

#pragma pack(push,2)
struct GroupIconHeader {WORD reserved,type,count;};
struct GroupIconEntry {BYTE width,height,colorCount,reserved;WORD planes,bitCount;DWORD bytesInResource;WORD resourceId;};
#pragma pack(pop)
#pragma pack(push,1)
struct FileIconEntry {BYTE width,height,colorCount,reserved;WORD planes,bitCount;DWORD bytesInResource,imageOffset;};
#pragma pack(pop)

static void ReleaseUnknown(IUnknown* value){if(value)value->Release();}
static bool MemoryStream(const void* bytes,size_t size,IStream** result){
    *result=nullptr;if(size>MAXDWORD)return false;HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE,size?size:1);if(!memory)return false;void* destination=GlobalLock(memory);if(!destination){GlobalFree(memory);return false;}if(size)memcpy(destination,bytes,size);GlobalUnlock(memory);if(FAILED(CreateStreamOnHGlobal(memory,TRUE,result))){GlobalFree(memory);return false;}return true;
}
static bool StreamBytes(IStream* stream,std::vector<BYTE>& bytes){
    STATSTG stat{};if(FAILED(stream->Stat(&stat,STATFLAG_NONAME))||stat.cbSize.HighPart||stat.cbSize.LowPart>16*1024*1024)return false;LARGE_INTEGER zero{};if(FAILED(stream->Seek(zero,STREAM_SEEK_SET,nullptr)))return false;bytes.resize(stat.cbSize.LowPart);ULONG read=0;return bytes.empty()||(SUCCEEDED(stream->Read(bytes.data(),(ULONG)bytes.size(),&read))&&read==bytes.size());
}
struct IconPixels {UINT width=0,height=0;std::vector<BYTE> bgra;};
struct IconOptimizer {
    IWICImagingFactory* factory=nullptr;bool uninitialize=false;
    IconOptimizer(){HRESULT initialized=CoInitializeEx(nullptr,COINIT_MULTITHREADED);uninitialize=SUCCEEDED(initialized);if(SUCCEEDED(initialized)||initialized==RPC_E_CHANGED_MODE)CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory));}
    ~IconOptimizer(){ReleaseUnknown(factory);if(uninitialize)CoUninitialize();}
    bool Decode(const void* bytes,size_t size,IconPixels& pixels){
        IStream* stream=nullptr;IWICBitmapDecoder* decoder=nullptr;IWICBitmapFrameDecode* frame=nullptr;IWICFormatConverter* converter=nullptr;bool ok=MemoryStream(bytes,size,&stream)&&SUCCEEDED(factory->CreateDecoderFromStream(stream,nullptr,WICDecodeMetadataCacheOnLoad,&decoder))&&SUCCEEDED(decoder->GetFrame(0,&frame))&&SUCCEEDED(frame->GetSize(&pixels.width,&pixels.height))&&pixels.width&&pixels.height&&pixels.width<=256&&pixels.height<=256&&SUCCEEDED(factory->CreateFormatConverter(&converter))&&SUCCEEDED(converter->Initialize(frame,GUID_WICPixelFormat32bppBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom));
        size_t stride=(size_t)pixels.width*4,total=stride*pixels.height;if(ok){pixels.bgra.resize(total);ok=SUCCEEDED(converter->CopyPixels(nullptr,(UINT)stride,(UINT)total,pixels.bgra.data()));}ReleaseUnknown(converter);ReleaseUnknown(frame);ReleaseUnknown(decoder);ReleaseUnknown(stream);return ok;
    }
    bool EncodePng(const IconPixels& pixels,std::vector<BYTE>& png){
        IStream* stream=nullptr;IWICBitmapEncoder* encoder=nullptr;IWICBitmapFrameEncode* frame=nullptr;IPropertyBag2* properties=nullptr;bool ok=MemoryStream(nullptr,0,&stream)&&SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder))&&SUCCEEDED(encoder->Initialize(stream,WICBitmapEncoderNoCache))&&SUCCEEDED(encoder->CreateNewFrame(&frame,&properties))&&SUCCEEDED(frame->Initialize(properties))&&SUCCEEDED(frame->SetSize(pixels.width,pixels.height));WICPixelFormatGUID format=GUID_WICPixelFormat32bppBGRA;if(ok)ok=SUCCEEDED(frame->SetPixelFormat(&format))&&IsEqualGUID(format,GUID_WICPixelFormat32bppBGRA);UINT stride=pixels.width*4;if(ok)ok=SUCCEEDED(frame->WritePixels(pixels.height,stride,(UINT)pixels.bgra.size(),const_cast<BYTE*>(pixels.bgra.data())))&&SUCCEEDED(frame->Commit())&&SUCCEEDED(encoder->Commit())&&StreamBytes(stream,png);ReleaseUnknown(properties);ReleaseUnknown(frame);ReleaseUnknown(encoder);ReleaseUnknown(stream);return ok;
    }
    bool EncodePalettePng(const IconPixels& pixels,unsigned wanted,bool compact,std::vector<BYTE>& png){
        // Lossy palette candidates deliberately have a different alpha policy
        // from exact candidates. Cluster visible RGB only, and reserve one
        // fully-transparent entry; this avoids wasting a tiny palette on alpha
        // variants and avoids translucent dark fringes.
        struct ColorCount {DWORD rgb,count;};
        std::vector<DWORD> values;values.reserve(pixels.bgra.size()/4);bool transparent=false;
        for(size_t i=0;i<pixels.bgra.size();i+=4){
            if(pixels.bgra[i+3]<128){transparent=true;continue;}
            values.push_back(((DWORD)pixels.bgra[i+2]<<16)|((DWORD)pixels.bgra[i+1]<<8)|pixels.bgra[i]);
        }
        std::sort(values.begin(),values.end());std::vector<ColorCount> unique;
        for(DWORD rgb:values){if(unique.empty()||unique.back().rgb!=rgb)unique.push_back({rgb,1});else ++unique.back().count;}
        if(unique.empty())unique.push_back({0,1});
        unsigned available=std::max(1u,std::min(16u,wanted)-(transparent?1u:0u));
        unsigned count=(unsigned)std::min<size_t>(unique.size(),available);DWORD centers[16]{};
        auto distance=[](DWORD a,DWORD b){int ar=(a>>16)&255,ag=(a>>8)&255,ab=a&255,br=(b>>16)&255,bg=(b>>8)&255,bb=b&255;return (unsigned)((ar-br)*(ar-br)+(ag-bg)*(ag-bg)+(ab-bb)*(ab-bb));};
        size_t first=0;for(size_t i=1;i<unique.size();++i)if(unique[i].count>unique[first].count)first=i;centers[0]=unique[first].rgb;
        for(unsigned c=1;c<count;++c){unsigned long long best=0;size_t selected=0;for(size_t i=0;i<unique.size();++i){unsigned nearest=UINT_MAX;for(unsigned j=0;j<c;++j)nearest=std::min(nearest,distance(unique[i].rgb,centers[j]));unsigned long long score=(unsigned long long)nearest*unique[i].count;if(score>best){best=score;selected=i;}}centers[c]=unique[selected].rgb;}
        for(unsigned pass=0;pass<8;++pass){unsigned long long red[16]{},green[16]{},blue[16]{},weight[16]{};for(const auto& color:unique){unsigned nearest=0,best=distance(color.rgb,centers[0]);for(unsigned c=1;c<count;++c){unsigned d=distance(color.rgb,centers[c]);if(d<best){best=d;nearest=c;}}red[nearest]+=(unsigned long long)((color.rgb>>16)&255)*color.count;green[nearest]+=(unsigned long long)((color.rgb>>8)&255)*color.count;blue[nearest]+=(unsigned long long)(color.rgb&255)*color.count;weight[nearest]+=color.count;}for(unsigned c=0;c<count;++c)if(weight[c])centers[c]=(DWORD)(((red[c]+weight[c]/2)/weight[c])<<16|((green[c]+weight[c]/2)/weight[c])<<8|((blue[c]+weight[c]/2)/weight[c]));}
        if(compact){
            // A saturated Windows palette is a useful separate candidate for
            // pixel-art and high-contrast artwork.
            static const DWORD fixed[]={0x000000,0x800000,0x008000,0x808000,0x000080,0x800080,0x008080,0xc0c0c0,0x808080,0xff0000,0x00ff00,0xffff00,0x0000ff,0xff00ff,0x00ffff,0xffffff};
            for(unsigned c=0;c<count;++c){unsigned nearest=0,best=distance(centers[c],fixed[0]);for(unsigned i=1;i<_countof(fixed);++i){unsigned d=distance(centers[c],fixed[i]);if(d<best){best=d;nearest=i;}}centers[c]=fixed[nearest];}
        }
        for(unsigned c=0;c<count;++c)for(unsigned prior=0;prior<c;++prior)if(centers[prior]==centers[c]){centers[c--]=centers[--count];break;}
        unsigned offset=transparent?1:0;WICColor colors[17]{};if(transparent)colors[0]=0;for(unsigned c=0;c<count;++c)colors[c+offset]=0xff000000|centers[c];
        std::vector<BYTE> indices((size_t)pixels.width*pixels.height);for(size_t pixel=0;pixel<indices.size();++pixel){size_t i=pixel*4;if(transparent&&pixels.bgra[i+3]<128){indices[pixel]=0;continue;}DWORD rgb=((DWORD)pixels.bgra[i+2]<<16)|((DWORD)pixels.bgra[i+1]<<8)|pixels.bgra[i];unsigned nearest=0,best=distance(rgb,centers[0]);for(unsigned c=1;c<count;++c){unsigned d=distance(rgb,centers[c]);if(d<best){best=d;nearest=c;}}indices[pixel]=(BYTE)(nearest+offset);}
        IWICPalette* palette=nullptr;IStream* stream=nullptr;IWICBitmapEncoder* encoder=nullptr;IWICBitmapFrameEncode* frame=nullptr;IPropertyBag2* properties=nullptr;UINT stride=pixels.width;
        bool ok=SUCCEEDED(factory->CreatePalette(&palette))&&SUCCEEDED(palette->InitializeCustom(colors,count+offset))&&MemoryStream(nullptr,0,&stream)&&SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder))&&SUCCEEDED(encoder->Initialize(stream,WICBitmapEncoderNoCache))&&SUCCEEDED(encoder->CreateNewFrame(&frame,&properties))&&SUCCEEDED(frame->Initialize(properties))&&SUCCEEDED(frame->SetSize(pixels.width,pixels.height));WICPixelFormatGUID format=GUID_WICPixelFormat8bppIndexed;if(ok)ok=SUCCEEDED(frame->SetPixelFormat(&format))&&IsEqualGUID(format,GUID_WICPixelFormat8bppIndexed)&&SUCCEEDED(frame->SetPalette(palette))&&SUCCEEDED(frame->WritePixels(pixels.height,stride,(UINT)indices.size(),indices.data()))&&SUCCEEDED(frame->Commit())&&SUCCEEDED(encoder->Commit())&&StreamBytes(stream,png);ReleaseUnknown(properties);ReleaseUnknown(frame);ReleaseUnknown(encoder);ReleaseUnknown(stream);ReleaseUnknown(palette);return ok;
    }
    bool DecodeIcon(const void* icon,DWORD iconSize,const GroupIconEntry& metadata,IconPixels& pixels){
        if(!factory||!icon||!iconSize||iconSize>16*1024*1024)return false;static const BYTE pngSignature[]={137,80,78,71,13,10,26,10};if(iconSize>=sizeof(pngSignature)&&!memcmp(icon,pngSignature,sizeof(pngSignature)))return Decode(icon,iconSize,pixels);GroupIconHeader header{0,1,1};FileIconEntry entry{metadata.width,metadata.height,metadata.colorCount,0,metadata.planes,metadata.bitCount,iconSize,(DWORD)(sizeof(header)+sizeof(FileIconEntry))};std::vector<BYTE> ico(entry.imageOffset+iconSize);memcpy(ico.data(),&header,sizeof(header));memcpy(ico.data()+sizeof(header),&entry,sizeof(entry));memcpy(ico.data()+entry.imageOffset,icon,iconSize);return Decode(ico.data(),ico.size(),pixels);
    }
    void StripPngColorMetadata(std::vector<BYTE>& png){
        if(png.size()<8)return;std::vector<BYTE> compact(png.begin(),png.begin()+8);size_t at=8;while(at+12<=png.size()){DWORD size=((DWORD)png[at]<<24)|((DWORD)png[at+1]<<16)|((DWORD)png[at+2]<<8)|png[at+3];size_t chunk=(size_t)size+12;if(chunk>png.size()-at){return;}const BYTE* type=png.data()+at+4;if(memcmp(type,"sRGB",4)&&memcmp(type,"gAMA",4))compact.insert(compact.end(),png.begin()+at,png.begin()+at+chunk);at+=chunk;}if(at==png.size())png.swap(compact);
    }
    bool PixelsEqual(const IconPixels& a,const IconPixels& b){
        if(a.width!=b.width||a.height!=b.height||a.bgra.size()!=b.bgra.size())return false;for(size_t i=0;i<a.bgra.size();i+=4)if(a.bgra[i+3]!=b.bgra[i+3]||(a.bgra[i+3]&&memcmp(a.bgra.data()+i,b.bgra.data()+i,3)))return false;return true;
    }
    bool Verified(const std::vector<BYTE>& png,const IconPixels& intended){IconPixels decoded;return Decode(png.data(),png.size(),decoded)&&PixelsEqual(decoded,intended);}
    bool Resize(const IconPixels& source,UINT width,UINT height,WICBitmapInterpolationMode mode,IconPixels& result){
        IWICBitmap* bitmap=nullptr;IWICBitmapScaler* scaler=nullptr;UINT stride=source.width*4,total=(UINT)source.bgra.size();bool ok=factory&&source.width&&source.height&&width&&height&&total==source.width*source.height*4&&SUCCEEDED(factory->CreateBitmapFromMemory(source.width,source.height,GUID_WICPixelFormat32bppBGRA,stride,total,const_cast<BYTE*>(source.bgra.data()),&bitmap))&&SUCCEEDED(factory->CreateBitmapScaler(&scaler))&&SUCCEEDED(scaler->Initialize(bitmap,width,height,mode));result.width=width;result.height=height;result.bgra.resize((size_t)width*height*4);if(ok)ok=SUCCEEDED(scaler->CopyPixels(nullptr,width*4,(UINT)result.bgra.size(),result.bgra.data()));ReleaseUnknown(scaler);ReleaseUnknown(bitmap);return ok;
    }
    bool PixelArt(const IconPixels& pixels){
        std::vector<DWORD> colors;for(size_t i=0;i<pixels.bgra.size();i+=4){BYTE alpha=pixels.bgra[i+3];if(alpha!=0&&alpha!=255)return false;if(!alpha)continue;DWORD value=((DWORD)pixels.bgra[i+2]<<16)|((DWORD)pixels.bgra[i+1]<<8)|pixels.bgra[i];if(std::find(colors.begin(),colors.end(),value)==colors.end()){if(colors.size()==64)return false;colors.push_back(value);}}return true;
    }
    unsigned long long Fidelity(const IconPixels& source,const IconPixels& candidate,unsigned long long& alphaError){
        alphaError=0;if(source.width!=candidate.width||source.height!=candidate.height||source.bgra.size()!=candidate.bgra.size())return ~0ull;
        unsigned long long error=0;for(size_t i=0;i<source.bgra.size();i+=4){int sa=source.bgra[i+3],ca=candidate.bgra[i+3],da=sa-ca;alphaError+=(unsigned long long)da*da;error+=(unsigned long long)da*da*4;for(unsigned channel=0;channel<3;++channel){int sv=source.bgra[i+channel]*sa/255,cv=candidate.bgra[i+channel]*ca/255,d=sv-cv;error+=(unsigned long long)d*d;}}
        return error/(source.bgra.size()/4);
    }
};
struct IconCandidate {
    GroupIconEntry entry{};std::vector<BYTE> bytes;IconPixels pixels;
    unsigned tier=UINT_MAX,paletteRank=0,dimension=0;unsigned long long fidelity=0,alphaError=0;
    bool lossless=false,generated=false,optimized=false;
};
static bool BetterIconCandidate(const IconCandidate& a,const IconCandidate& b){
    if(a.tier!=b.tier)return a.tier<b.tier;
    if(a.lossless!=b.lossless)return a.lossless;
    if(a.paletteRank!=b.paletteRank)return a.paletteRank>b.paletteRank;
    if(a.fidelity!=b.fidelity)return a.fidelity<b.fidelity;
    if(a.alphaError!=b.alphaError)return a.alphaError<b.alphaError;
    if(a.generated!=b.generated)return !a.generated;
    if(a.dimension!=b.dimension)return a.dimension>b.dimension;
    if(a.bytes.size()!=b.bytes.size())return a.bytes.size()<b.bytes.size();
    return a.bytes<b.bytes;
}
struct IconCopyContext {HANDLE update;IconOptimizer* optimizer;size_t budget;bool found=false,ok=true;};
static BOOL CALLBACK CopySmallIconLanguage(HMODULE source,LPCWSTR,LPCWSTR name,WORD language,LONG_PTR value){
    auto* context=(IconCopyContext*)value;context->found=true;HRSRC groupResource=FindResourceExW(source,RT_GROUP_ICON,name,language);DWORD groupSize=groupResource?SizeofResource(source,groupResource):0;HGLOBAL loaded=groupResource?LoadResource(source,groupResource):nullptr;const BYTE* group=loaded?(const BYTE*)LockResource(loaded):nullptr;if(!group||groupSize<sizeof(GroupIconHeader)){context->ok=false;return FALSE;}auto* header=(const GroupIconHeader*)group;if(header->reserved||header->type!=1||!header->count||groupSize<sizeof(*header)+(DWORD)header->count*sizeof(GroupIconEntry)){context->ok=false;return FALSE;}auto* entries=(const GroupIconEntry*)(group+sizeof(*header));
    std::vector<IconCandidate> candidates;IconPixels largest;GroupIconEntry largestEntry{};unsigned largestDimension=0;bool authored32=false,authored48=false;
    auto tier=[](unsigned dimension,bool lossless,bool generated){
        if(dimension<=16)return lossless?80u:1000u;
        if(lossless){if(!generated&&dimension==48)return 0u;if(!generated&&dimension==32)return 1u;if(generated&&dimension==48)return 2u;if(!generated)return 3u;if(dimension==32)return 4u;return 5u;}
        if(!generated&&dimension==32)return 10u;if(!generated&&dimension==48)return 11u;if(generated&&dimension==32)return 12u;if(generated&&dimension==48)return 13u;return 14u;
    };
    auto add=[&](const IconPixels& sourcePixels,const IconPixels& candidatePixels,std::vector<BYTE> bytes,GroupIconEntry entry,bool lossless,bool generated,unsigned paletteRank){
        if(bytes.empty())return;unsigned dimension=std::max(sourcePixels.width,sourcePixels.height);entry.width=sourcePixels.width==256?0:(BYTE)sourcePixels.width;entry.height=sourcePixels.height==256?0:(BYTE)sourcePixels.height;entry.colorCount=0;entry.bytesInResource=(DWORD)bytes.size();IconCandidate candidate;candidate.entry=entry;candidate.bytes=std::move(bytes);candidate.pixels=candidatePixels;candidate.dimension=dimension;candidate.lossless=lossless;candidate.generated=generated;candidate.paletteRank=paletteRank;candidate.tier=tier(dimension,lossless,generated);candidate.fidelity=lossless?0:context->optimizer->Fidelity(sourcePixels,candidatePixels,candidate.alphaError);candidates.push_back(std::move(candidate));
    };
    auto consider=[&](const IconPixels& pixels,const void* original,size_t originalSize,GroupIconEntry entry,bool generated){
        std::vector<BYTE> exact;if(original&&originalSize)exact.assign((const BYTE*)original,(const BYTE*)original+originalSize);std::vector<BYTE> png;if(context->optimizer->EncodePng(pixels,png)){context->optimizer->StripPngColorMetadata(png);if(context->optimizer->Verified(png,pixels)&&(exact.empty()||png.size()<exact.size()))exact.swap(png);}if(!exact.empty())add(pixels,pixels,std::move(exact),entry,true,generated,UINT_MAX);
        auto palette=[&](unsigned colors,bool compact){std::vector<BYTE> encoded;if(!context->optimizer->EncodePalettePng(pixels,colors,compact,encoded))return;context->optimizer->StripPngColorMetadata(encoded);IconPixels decoded;if(!context->optimizer->Decode(encoded.data(),encoded.size(),decoded))return;add(pixels,decoded,std::move(encoded),entry,false,generated,compact?12u:colors);};
        for(unsigned colors:{16u,12u,8u})palette(colors,false);palette(7,true);for(unsigned colors:{6u,4u,2u})palette(colors,false);
    };
    for(unsigned i=0;i<header->count;++i){GroupIconEntry entry=entries[i];HRSRC resource=FindResourceExW(source,RT_ICON,MAKEINTRESOURCEW(entry.resourceId),language);if(!resource)resource=FindResourceW(source,MAKEINTRESOURCEW(entry.resourceId),RT_ICON);DWORD size=resource?SizeofResource(source,resource):0;HGLOBAL loadedIcon=resource?LoadResource(source,resource):nullptr;const void* icon=loadedIcon?LockResource(loadedIcon):nullptr;IconPixels pixels;if(!icon||!size||!context->optimizer->DecodeIcon(icon,size,entry,pixels))continue;unsigned dimension=std::max(pixels.width,pixels.height);if(dimension==32)authored32=true;if(dimension==48)authored48=true;if(dimension<=48)consider(pixels,icon,size,entry,false);if(pixels.width==pixels.height&&dimension>largestDimension){largestDimension=dimension;largest=pixels;largestEntry=entry;}}
    if(largestDimension){WICBitmapInterpolationMode mode=context->optimizer->PixelArt(largest)?WICBitmapInterpolationModeNearestNeighbor:WICBitmapInterpolationModeFant;for(UINT dimension:{48u,32u})if(dimension<largestDimension&&!((dimension==48&&authored48)||(dimension==32&&authored32))){IconPixels resized;if(context->optimizer->Resize(largest,dimension,dimension,mode,resized)){GroupIconEntry entry=largestEntry;entry.planes=1;entry.bitCount=32;consider(resized,nullptr,0,entry,true);}}}
    std::sort(candidates.begin(),candidates.end(),BetterIconCandidate);IconCandidate best;bool haveBest=false;unsigned nearAttempts=0;(void)nearAttempts;
    for(auto& candidate:candidates){
        if(candidate.bytes.size()<=context->budget){best=std::move(candidate);haveBest=true;break;}
#ifndef ONEKB_RUNTIME_ONLY
        if(nearAttempts<2&&candidate.bytes.size()<=context->budget+128){++nearAttempts;std::vector<BYTE> compact;if(OptimizeTinyPng(candidate.pixels.width,candidate.pixels.height,candidate.pixels.bgra.data(),candidate.pixels.bgra.size(),compact)&&compact.size()<=context->budget&&context->optimizer->Verified(compact,candidate.pixels)){candidate.bytes.swap(compact);candidate.entry.bytesInResource=(DWORD)candidate.bytes.size();candidate.optimized=true;best=std::move(candidate);haveBest=true;break;}}
#endif
    }
    if(!haveBest){context->found=false;return FALSE;}
#ifndef ONEKB_RUNTIME_ONLY
    // One exhaustive serialization for the winner; only the two higher-quality
    // near-budget probes above may add work.
    if(!best.optimized){std::vector<BYTE> compact;if(OptimizeTinyPng(best.pixels.width,best.pixels.height,best.pixels.bgra.data(),best.pixels.bgra.size(),compact)&&compact.size()<best.bytes.size()&&context->optimizer->Verified(compact,best.pixels)){best.bytes.swap(compact);best.entry.bytesInResource=(DWORD)best.bytes.size();}}
#endif
    GroupIconHeader smallHeader{0,1,1};std::vector<BYTE> compactGroup(sizeof(smallHeader)+sizeof(best.entry));memcpy(compactGroup.data(),&smallHeader,sizeof(smallHeader));memcpy(compactGroup.data()+sizeof(smallHeader),&best.entry,sizeof(best.entry));context->ok=best.bytes.size()<=context->budget&&UpdateResourceW(context->update,RT_GROUP_ICON,name,language,compactGroup.data(),(DWORD)compactGroup.size())&&UpdateResourceW(context->update,RT_ICON,MAKEINTRESOURCEW(best.entry.resourceId),language,best.bytes.data(),(DWORD)best.bytes.size());return FALSE;
}
static BOOL CALLBACK CopySmallIconName(HMODULE source,LPCWSTR,LPWSTR name,LONG_PTR value){EnumResourceLanguagesExW(source,RT_GROUP_ICON,name,CopySmallIconLanguage,value,RESOURCE_ENUM_LN,0);return FALSE;}
static bool CopySmallIcon(HMODULE source,HANDLE update,size_t budget,bool& copied){IconOptimizer optimizer;IconCopyContext context{update,&optimizer,budget};SetLastError(ERROR_SUCCESS);EnumResourceNamesExW(source,RT_GROUP_ICON,CopySmallIconName,(LONG_PTR)&context,RESOURCE_ENUM_LN,0);copied=context.found;return context.ok;}
static bool WriteEmbeddedBinary(WORD id,const std::wstring& destination){HMODULE self=GetModuleHandleW(nullptr);HRSRC resource=FindResourceW(self,MAKEINTRESOURCEW(id),RT_RCDATA);if(!resource)return false;DWORD size=SizeofResource(self,resource);HGLOBAL loaded=LoadResource(self,resource);const void* bytes=loaded?LockResource(loaded):nullptr;if(!bytes)return false;HANDLE file=CreateFileW(destination.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);if(file==INVALID_HANDLE_VALUE)return false;DWORD written=0;bool ok=WriteFile(file,bytes,size,&written,nullptr)&&written==size;CloseHandle(file);if(!ok)DeleteFileW(destination.c_str());return ok;}
static bool ReadResourceBytes(WORD id,std::vector<uint8_t>& out){HMODULE self=GetModuleHandleW(nullptr);HRSRC resource=FindResourceW(self,MAKEINTRESOURCEW(id),RT_RCDATA);DWORD size=resource?SizeofResource(self,resource):0;HGLOBAL loaded=resource?LoadResource(self,resource):nullptr;const void* bytes=loaded?LockResource(loaded):nullptr;if(!bytes||!size)return false;out.assign((const uint8_t*)bytes,(const uint8_t*)bytes+size);return true;}
static size_t LauncherIconBudget(const EmbeddedConfig& config,bool console){
    // Icon-capable packed image = core + 100-byte single-image carrier overhead
    // + RT_ICON bytes + decoder safety byte + the identity overlay.
    std::vector<uint8_t> core,overlay;bool privateLauncher=config.encryption==PayloadEncryption::Launcher;if(!ReadResourceBytes(console?ConsoleIconCrinklerResourceId:GuiIconCrinklerResourceId,core)||!OverlayIdentity::EncodeOverlayIdentity(config.appId,privateLauncher?config.secret:nullptr,privateLauncher?PayloadSecretBytes:0,ValidateCanonicalOverlayIdentity,overlay))return 0;constexpr size_t Limit=1024,FixedIconCarrier=100,SafetyByte=1;size_t fixed=core.size()+FixedIconCarrier+SafetyByte+overlay.size();if(!overlay.empty())SecureZeroMemory(overlay.data(),overlay.size());return fixed<Limit?Limit-fixed:0;
}
static bool ValidateCrinklerCandidate(const std::vector<uint8_t>& bytes,WORD subsystem){
    // Crinkler deliberately overlaps its PE header and decompressor. Check every
    // loader-significant field we rely on instead of feeding this zero-section
    // image to conventional section-table validation.
    auto u16=[&](size_t at){WORD value=0;if(at+sizeof(value)<=bytes.size())memcpy(&value,bytes.data()+at,sizeof(value));return value;};
    auto u32=[&](size_t at){DWORD value=0;if(at+sizeof(value)<=bytes.size())memcpy(&value,bytes.data()+at,sizeof(value));return value;};
    return bytes.size()>=100&&bytes.size()<128*1024&&u16(0)==IMAGE_DOS_SIGNATURE&&u32(0x3c)==4&&u32(4)==IMAGE_NT_SIGNATURE&&u16(8)==IMAGE_FILE_MACHINE_I386&&u16(10)==0&&u16(24)==8&&u16(28)==IMAGE_NT_OPTIONAL_HDR32_MAGIC&&u32(44)!=0&&u32(56)==0x400000&&u32(60)==4&&u32(64)==4&&u32(88)==48&&u16(96)==subsystem;
}
static bool ReadWholeFile(const std::wstring& path,std::vector<uint8_t>& out){std::vector<char> bytes;if(!ReadBytes(path,Limits::MaxApplicationSize,bytes))return false;out.assign(bytes.begin(),bytes.end());return true;}
static bool HasPeResources(const std::vector<uint8_t>& bytes){if(bytes.size()<sizeof(IMAGE_DOS_HEADER))return false;auto* dos=(const IMAGE_DOS_HEADER*)bytes.data();if(dos->e_magic!=IMAGE_DOS_SIGNATURE||dos->e_lfanew<0||(size_t)dos->e_lfanew>bytes.size()||sizeof(IMAGE_NT_HEADERS32)>bytes.size()-(size_t)dos->e_lfanew)return false;auto* nt=(const IMAGE_NT_HEADERS32*)(bytes.data()+dos->e_lfanew);return nt->Signature==IMAGE_NT_SIGNATURE&&nt->OptionalHeader.Magic==IMAGE_NT_OPTIONAL_HDR32_MAGIC&&nt->OptionalHeader.NumberOfRvaAndSizes>IMAGE_DIRECTORY_ENTRY_RESOURCE&&nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE].VirtualAddress&&nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE].Size;}
static bool WriteWholeFile(const std::wstring& path,const std::vector<uint8_t>& bytes){HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);if(file==INVALID_HANDLE_VALUE)return false;DWORD wrote=0;bool ok=bytes.size()<=MAXDWORD&&WriteFile(file,bytes.data(),(DWORD)bytes.size(),&wrote,nullptr)&&wrote==bytes.size();CloseHandle(file);if(!ok)DeleteFileW(path.c_str());return ok;}
static bool LoadableExecutable(const std::wstring& path){
    STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};
    if(!CreateProcessW(path.c_str(),nullptr,nullptr,nullptr,FALSE,CREATE_SUSPENDED|CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process))return false;
    TerminateProcess(process.hProcess,0);WaitForSingleObject(process.hProcess,5000);CloseHandle(process.hThread);CloseHandle(process.hProcess);return true;
}
static bool IconLoadableExecutable(const std::wstring& path){
    HMODULE module=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_AS_DATAFILE|LOAD_LIBRARY_AS_IMAGE_RESOURCE);if(!module)return false;HRSRC group=FindResourceW(module,MAKEINTRESOURCEW(1),MAKEINTRESOURCEW(14)),icon=FindResourceW(module,MAKEINTRESOURCEW(1),MAKEINTRESOURCEW(3));DWORD groupBytes=group?SizeofResource(module,group):0,iconBytes=icon?SizeofResource(module,icon):0;HGLOBAL loadedGroup=group?LoadResource(module,group):nullptr,loadedIcon=icon?LoadResource(module,icon):nullptr;const BYTE* groupData=loadedGroup?(const BYTE*)LockResource(loadedGroup):nullptr;bool ok=groupBytes==sizeof(GroupIconHeader)+sizeof(GroupIconEntry)&&iconBytes&&groupData&&groupData[0]==0&&groupData[1]==0&&groupData[2]==1&&groupData[3]==0&&groupData[4]==1&&groupData[5]==0&&groupData[18]==1&&groupData[19]==0&&loadedIcon&&LockResource(loadedIcon);FreeLibrary(module);if(!ok)return false;HICON largeIcon=nullptr,smallIcon=nullptr;UINT extracted=ExtractIconExW(path.c_str(),0,&largeIcon,&smallIcon,1);bool haveIcon=largeIcon||smallIcon;if(largeIcon)DestroyIcon(largeIcon);if(smallIcon)DestroyIcon(smallIcon);return extracted&&haveIcon;
}
static bool ReadEmbeddedForInspection(const std::wstring& source,EmbeddedConfig& config){return ReadEmbeddedFile(source,config);}
#ifndef ONEKB_RUNTIME_ONLY
static bool PackCrinklerRepresentation(const std::wstring& target,WORD subsystem,WORD stockCoreId){
    std::vector<uint8_t> source,core,packed;bool icon=ReadWholeFile(target,source)&&HasPeResources(source);
    WORD coreId=icon?(subsystem==IMAGE_SUBSYSTEM_WINDOWS_CUI?ConsoleIconCrinklerResourceId:GuiIconCrinklerResourceId):stockCoreId;
    std::wstring reason;
    bool ok=!source.empty()&&ReadResourceBytes(coreId,core);
    if(ok&&icon)ok=PackCrinklerIconPe(source,core,packed,&reason);
    else if(ok){ok=ValidateCrinklerCandidate(core,subsystem);if(ok)packed=core;}
    // Isolate the arithmetic decoder's one-byte read-ahead from the overlay.
    if(ok)packed.push_back(0);
    std::wstring candidate=target+(icon?L".crinkler-icon":L".crinkler");DeleteFileW(candidate.c_str());
    if(ok)ok=WriteWholeFile(candidate,packed)&&LoadableExecutable(candidate)&&(!icon||IconLoadableExecutable(candidate))&&CopyFileW(candidate.c_str(),target.c_str(),FALSE);
    DWORD error=ok?ERROR_SUCCESS:GetLastError();if(!error)error=ERROR_BAD_EXE_FORMAT;
    if(!GetEnvironmentVariableW(L"ONEKB_KEEP_CANDIDATES",nullptr,0))DeleteFileW(candidate.c_str());
    SetLastError(error);return ok;
}
#else
static bool PackCrinklerRepresentation(const std::wstring& target,WORD subsystem,WORD stockCoreId){(void)target;(void)subsystem;(void)stockCoreId;return false;}
#endif
static bool AppendOverlay(const std::wstring& path,const EmbeddedConfig& config){bool privateLauncher=config.encryption==PayloadEncryption::Launcher;std::vector<uint8_t> overlay;if(!OverlayIdentity::EncodeOverlayIdentity(config.appId,privateLauncher?config.secret:nullptr,privateLauncher?PayloadSecretBytes:0,ValidateCanonicalOverlayIdentity,overlay)){SetLastError(ERROR_INVALID_DATA);return false;}HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);if(file==INVALID_HANDLE_VALUE)return false;LARGE_INTEGER end{};bool ok=SetFilePointerEx(file,end,nullptr,FILE_END)!=FALSE;DWORD wrote=0;if(ok)ok=WriteFile(file,overlay.data(),(DWORD)overlay.size(),&wrote,nullptr)&&wrote==overlay.size();ok=ok&&FlushFileBuffers(file);DWORD error=ok?ERROR_SUCCESS:GetLastError();CloseHandle(file);if(!overlay.empty())SecureZeroMemory(overlay.data(),overlay.size());SetLastError(error);return ok;}
static bool ReplaceLauncherFile(const std::wstring& temporary,const std::wstring& output){for(unsigned attempt=0;;++attempt){if(MoveFileExW(temporary.c_str(),output.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))return true;DWORD error=GetLastError();if(attempt==39&&(error==ERROR_ACCESS_DENIED||error==ERROR_SHARING_VIOLATION||error==ERROR_LOCK_VIOLATION)){SetLastError(error);return false;}if(error!=ERROR_ACCESS_DENIED&&error!=ERROR_SHARING_VIOLATION&&error!=ERROR_LOCK_VIOLATION)return false;Sleep(50);}}
static bool GenerateLauncher(const std::wstring& source,const std::wstring& output,const EmbeddedConfig& config,bool console,IconMode iconMode,std::wstring& failure){
    std::wstring temporary=output+L".tmp."+std::to_wstring(GetCurrentProcessId())+L".exe";DeleteFileW(temporary.c_str());failure=L"write the temporary launcher";SetLastError(ERROR_SUCCESS);
    if(!WriteEmbeddedBinary(console?ConsoleRuntimeTemplateResourceId:GuiRuntimeTemplateResourceId,temporary)){DWORD error=GetLastError();DeleteFileW(temporary.c_str());SetLastError(error?error:ERROR_WRITE_FAULT);return false;}
    bool ok=true,includedIcon=false;DWORD error=ERROR_SUCCESS;
    if(iconMode!=IconMode::None){failure=L"copy the application icon";HMODULE app=LoadLibraryExW(source.c_str(),nullptr,LOAD_LIBRARY_AS_DATAFILE|LOAD_LIBRARY_AS_IMAGE_RESOURCE);HANDLE update=BeginUpdateResourceW(temporary.c_str(),FALSE);bool copied=false;if(!update)ok=false;else{if(app)ok=CopySmallIcon(app,update,LauncherIconBudget(config,console),copied);if(!EndUpdateResourceW(update,!ok||!copied)&&ok)ok=false;}includedIcon=ok&&copied;if(app)FreeLibrary(app);if(!ok)error=GetLastError();}
    if(ok){failure=L"finalize the launcher";ok=PackCrinklerRepresentation(temporary,console?IMAGE_SUBSYSTEM_WINDOWS_CUI:IMAGE_SUBSYSTEM_WINDOWS_GUI,console?ConsoleCrinklerResourceId:GuiCrinklerResourceId)&&AppendOverlay(temporary,config);if(ok){WIN32_FILE_ATTRIBUTE_DATA attributes{};bool withinLimit=!includedIcon||(GetFileAttributesExW(temporary.c_str(),GetFileExInfoStandard,&attributes)&&!attributes.nFileSizeHigh&&attributes.nFileSizeLow<=1024);EmbeddedConfig verified;WORD verifiedSubsystem=0;ok=withinLimit&&ReadLauncherOnce(temporary,verified,verifiedSubsystem)&&verified.appId==config.appId&&verified.encryption==config.encryption&&(!memcmp(verified.secret,config.secret,PayloadSecretBytes))&&verifiedSubsystem==(console?IMAGE_SUBSYSTEM_WINDOWS_CUI:IMAGE_SUBSYSTEM_WINDOWS_GUI);SecureZeroMemory(verified.secret,PayloadSecretBytes);if(!ok)SetLastError(ERROR_BAD_EXE_FORMAT);}if(!ok)error=GetLastError();}
    if(ok){failure=L"replace the output file";if(!ReplaceLauncherFile(temporary,output)){ok=false;error=GetLastError();}}
    if(!ok)DeleteFileW(temporary.c_str());SetLastError(error);return ok;
}
bool NormalizeLauncherAppId(const std::wstring& input,std::wstring& appId,bool& github,std::wstring& repository,std::wstring& appKey){AppIdentity identity;if(!ParseAppId(input,identity))return false;appId=identity.appId;github=identity.provider==AppProvider::Github;repository=github?identity.owner+L"/"+identity.repository:L"";appKey=github?identity.appKey:L"";return true;}
bool ReadDeploymentLauncherOverlay(const std::wstring& path,LauncherOverlay& overlay){return ReadEmbeddedFile(path,overlay);}
bool GenerateDeploymentLauncher(const std::wstring& application,const std::wstring& output,const std::wstring& appId,bool removeIcon,bool detachConsole,PayloadEncryption encryption,const uint8_t secret[PayloadSecretBytes],std::wstring& error){AppIdentity identity;if(!ParseAppId(appId,identity)||identity.appId!=appId){error=L"The application ID is invalid.";return false;}if(encryption==PayloadEncryption::Launcher&&!secret){error=L"The private launcher encryption secret is unavailable.";return false;}WORD subsystem=0;if(!ExecutableSubsystem(application,subsystem)){error=L"The release application executable is invalid.";return false;}EmbeddedConfig config;config.appId=appId;config.encryption=encryption;if(secret)memcpy(config.secret,secret,PayloadSecretBytes);std::wstring operation;bool generated=GenerateLauncher(application,output,config,subsystem==IMAGE_SUBSYSTEM_WINDOWS_CUI&&!detachConsole,removeIcon?IconMode::None:IconMode::Small,operation);SecureZeroMemory(config.secret,PayloadSecretBytes);if(generated)return true;DWORD code=GetLastError();error=L"Could not "+operation+L" (Windows error "+std::to_wstring(code)+L").";return false;}
static int LauncherMain(){
    if(!InitPaths()){MessageBoxW(nullptr,L"Cannot create the per-user application directory.",cfg.displayName.c_str(),MB_ICONERROR);return 1;}
    HANDLE updateMutex=CreateMutexW(nullptr,FALSE,ObjectName(L"_LauncherUpdate").c_str());if(!updateMutex)return 1;
    gConfigurationMutex=CreateMutexW(nullptr,FALSE,ObjectName(L"_LauncherConfiguration").c_str());if(!gConfigurationMutex){CloseHandle(updateMutex);return 1;}
    LoadPersistedOverrides();std::wstring existing;bool hadInstalled=CurrentValid(existing);
    if(cfg.mode==UpdateMode::Yes||!hadInstalled)ShowUpdateUi(hadInstalled?existing:L"");
    bool updateFirst=cfg.mode==UpdateMode::Yes||!hadInstalled;
    DWORD updateLock=WaitForSingleObject(updateMutex,updateFirst?INFINITE:0);bool ownsUpdate=updateLock==WAIT_OBJECT_0||updateLock==WAIT_ABANDONED;if(ownsUpdate)LoadPersistedOverrides();
    std::wstring installed,updateError;UpdateResult result=UpdateResult::NoUpdate;bool backgroundUpdate=false;
    if(updateFirst&&ownsUpdate)result=CheckAndInstall(installed,updateError);
    if(gUpdateUi.thread&&result==UpdateResult::Installed)SetUpdateUi(hadInstalled?L"Update ready":L"Ready to launch",L"Version "+installed+L" is installed",100);else if(result==UpdateResult::Failed)ShowUpdateFailureUi(updateError,hadInstalled);
    HANDLE launchMutex=CreateMutexW(nullptr,FALSE,ObjectName(L"_LauncherLaunch").c_str());if(!launchMutex){if(ownsUpdate)ReleaseMutex(updateMutex);CloseHandle(gConfigurationMutex);gConfigurationMutex=nullptr;CloseHandle(updateMutex);return 1;}
    DWORD launchLock=WaitForSingleObject(launchMutex,INFINITE);LaunchedProcess launched;bool launchedOk=(launchLock==WAIT_OBJECT_0||launchLock==WAIT_ABANDONED)&&LaunchInstalled(&launched);if(launchLock==WAIT_OBJECT_0||launchLock==WAIT_ABANDONED)ReleaseMutex(launchMutex);CloseHandle(launchMutex);
    if(!launchedOk){CloseUpdateUi();std::wstring message=updateError.empty()?L"The installed application could not be started.":updateError;MessageBoxW(nullptr,message.c_str(),cfg.displayName.c_str(),MB_ICONERROR);CloseLaunched(launched);if(ownsUpdate){StartRuntimeUpdate();ReleaseMutex(updateMutex);}CloseHandle(gConfigurationMutex);gConfigurationMutex=nullptr;CloseHandle(updateMutex);return 2;}
    CloseUpdateUi();
    if(!ownsUpdate){CloseHandle(gConfigurationMutex);gConfigurationMutex=nullptr;CloseHandle(updateMutex);return FinishLaunched(launched,0);}
    if(!updateFirst){
        if(cfg.mode==UpdateMode::No&&StartBackgroundUpdate())backgroundUpdate=true;
        else{UpdateMode operationMode=cfg.mode;result=CheckAndInstall(installed,updateError,operationMode==UpdateMode::Restart);
            if(result==UpdateResult::Installed&&operationMode==UpdateMode::Restart){SetUpdateUi(L"Restarting",L"Starting version "+installed,100);if(launched.handle)RestartTrackedApplication(launched,installed);CloseUpdateUi();}
            else{if(result==UpdateResult::Failed)ShowUpdateFailureUi(updateError,true);CloseUpdateUi();}}
    }
    std::wstring active;if(CurrentValid(active))Cleanup(active);if(!backgroundUpdate)StartRuntimeUpdate();ReleaseMutex(updateMutex);CloseHandle(gConfigurationMutex);gConfigurationMutex=nullptr;CloseHandle(updateMutex);return FinishLaunched(launched,0);
}
#ifdef ONEKB_RUNTIME_ONLY
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR arguments,int){DetectPrivateWorkerMode();if(gRuntimeApply)return RuntimeApplyMain();RuntimeStartupCleanup();PromoteRecoveryRuntime();
#ifdef ONEKB_RUNTIME_TESTS
if(EnvironmentValue(L"ONEKB_RUNTIME_TEST_UPDATE",gRuntimeTestUrl)){SetEnvironmentVariableW(L"ONEKB_RUNTIME_TEST_UPDATE",nullptr);UpdateLauncherRuntime();return 0;}
{std::wstring output;if(EnvironmentValue(L"ONEKB_RUNTIME_TEST_OUTPUT",output)){SetEnvironmentVariableW(L"ONEKB_RUNTIME_TEST_OUTPUT",nullptr);wchar_t self[32768]{};return GetModuleFileNameW(nullptr,self,_countof(self))&&CopyFileW(self,output.c_str(),FALSE)?43:1;}}
#endif
if(arguments)gAppArguments=arguments;std::wstring startupError;if(!LoadRuntimeConfig(startupError)){MessageBoxW(nullptr,startupError.c_str(),L"1KB.exe Runtime",MB_OK|MB_ICONERROR);return 1;}int result=0;if(gRuntimeUpdate)UpdateLauncherRuntime();else if(gBackgroundUpdate)result=BackgroundUpdateMain();else{if(gConsoleLauncher)AttachConsole(ATTACH_PARENT_PROCESS);result=LauncherMain();}SecureZeroMemory(gLauncherOverlay.secret,PayloadSecretBytes);return result;}
#else
int wmain(int argc,wchar_t** argv){
    int result=0;
    if(argc==1)result=DeploymentManagerMain();
    else if(argc==2){wchar_t full[32768]{};DWORD n=GetFullPathNameW(argv[1],_countof(full),full,nullptr);DWORD attributes=n&&n<_countof(full)?GetFileAttributesW(full):INVALID_FILE_ATTRIBUTES;if(attributes==INVALID_FILE_ATTRIBUTES){fwprintf(stderr,L"Error: expected an existing release EXE, ZIP, folder, or generated launcher.\n");result=2;}else{std::wstring path(full,n);EmbeddedConfig embedded;if(!(attributes&FILE_ATTRIBUTE_DIRECTORY)&&ReadEmbeddedForInspection(path,embedded)){result=DeploymentManagerOpenLauncher(embedded.appId.c_str());SecureZeroMemory(embedded.secret,PayloadSecretBytes);}else result=DeploymentManagerAddDroppedPath(path.c_str());}}
    else{fwprintf(stderr,L"Error: 1KB.exe does not provide command-line commands. Open it directly or pass one release path.\n");result=2;}
    return result;
}
#endif
