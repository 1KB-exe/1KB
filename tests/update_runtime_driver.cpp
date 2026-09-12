#define ONEKB_RUNTIME_ONLY
#define wWinMain UnusedRuntimeMain
#include "../src/main.cpp"
#undef wWinMain
int wmain(int argc,wchar_t** argv){
    if(argc!=3)return 2;gBackgroundUpdate=true;cfg.fallbackDisplayName=L"Update test";
    if(!ParseAppId(L"url:"+std::wstring(argv[2]),cfg.identity))return 2;
    g.root=argv[1];g.versions=g.root;g.current=g.root+L"\\current.txt";g.updateState=g.root+L"\\1kb.ini";if(!Updates::Tree(g.root))return 2;LoadPersistedOverrides();
    HANDLE mutex=CreateMutexW(nullptr,FALSE,ObjectName(L"_LauncherUpdate").c_str());if(!mutex)return 2;DWORD lock=WaitForSingleObject(mutex,INFINITE);bool ok=lock==WAIT_OBJECT_0||lock==WAIT_ABANDONED;
    std::vector<char> bytes;UpdateResponse update;ok=ok&&HttpGetUrl(argv[2],&bytes,nullptr,Updates::MetadataLimit,5000)&&ParseRemoteConfiguration(bytes,argv[2],update);std::wstring error;if(ok)ok=PrepareAndActivate(update,true,error);if(ok)Cleanup(update.manifest.version);ReleaseMutex(mutex);CloseHandle(mutex);if(!ok)fwprintf(stderr,L"%ls\n",error.c_str());return ok?0:1;
}
