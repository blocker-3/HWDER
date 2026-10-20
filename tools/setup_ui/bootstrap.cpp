// Code-free native Windows setup shell. Links only Windows system libraries.
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <memory>
#include <cstring>
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='amd64' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace fs=std::filesystem;
constexpr UINT Event=WM_APP+1;
enum { Game=101,Keys,Data,BrowseGame,BrowseKeys,BrowseData,Check,PythonSite,ToolsSite,Components,Prepare,Play,Shortcut,Details,Cancel };
HWND window,gameBox,keyBox,dataBox,statusBox,progress,detailBox,prepareButton,playButton,shortcutButton,cancelButton;
HFONT font,titleFont;fs::path source,application;bool demo=false,busy=false,publishing=false,closeDeferred=false,ready=false,requirements=false,pendingReady=false,pendingChecked=false,launching=false,committed=false,hadPublishPermission=false;
std::wstring activeData,certifiedData,activeAction,failureMessage;
bool demoFail=false;
bool noPythonPreview=false;
unsigned generation=0;std::atomic<bool> cancelRequested{false};std::mutex handles;HANDLE currentJob=nullptr,inputWrite=nullptr;
struct Notice{unsigned generation;std::wstring line;DWORD exit=STILL_ACTIVE;bool control=false;};
std::wstring wide(const std::string& s){if(s.empty())return{};int n=MultiByteToWideChar(CP_UTF8,0,s.data(),(int)s.size(),nullptr,0);std::wstring r(n,0);MultiByteToWideChar(CP_UTF8,0,s.data(),(int)s.size(),r.data(),n);return r;}
std::string utf8(const std::wstring& s){if(s.empty())return{};int n=WideCharToMultiByte(CP_UTF8,0,s.data(),(int)s.size(),nullptr,0,nullptr,nullptr);std::string r(n,0);WideCharToMultiByte(CP_UTF8,0,s.data(),(int)s.size(),r.data(),n,nullptr,nullptr);return r;}
std::wstring text(HWND h){int n=GetWindowTextLengthW(h);std::wstring s(n+1,0);GetWindowTextW(h,s.data(),n+1);s.resize(n);return s;}
std::wstring quote(const std::wstring& s){std::wstring r=L"\"";unsigned slash=0;for(wchar_t c:s){if(c==L'\\'){++slash;continue;}if(c==L'"'){r.append(slash*2+1,L'\\');r+=c;}else{r.append(slash,L'\\');r+=c;}slash=0;}r.append(slash*2,L'\\');return r+L"\"";}
std::wstring field(const std::wstring& s,const std::wstring& key){auto at=s.find(L"\""+key+L"\"");if(at==s.npos)return{};at=s.find(L':',at+key.size()+2);if(at==s.npos)return{};at=s.find(L'"',at+1);if(at==s.npos)return{};std::wstring out;for(++at;at<s.size();++at){wchar_t c=s[at];if(c==L'"')return out;if(c==L'\\'&&at+1<s.size()){c=s[++at];if(c==L'n')c=L'\n';else if(c==L'r')c=L'\r';else if(c==L't')c=L'\t';}out+=c;}return{};}
void post(unsigned gen,std::wstring line,bool control=false,DWORD exit=STILL_ACTIVE){auto* p=new Notice{gen,std::move(line),exit,control};if(!PostMessageW(window,Event,0,(LPARAM)p))delete p;}
void state(const std::wstring& s){SetWindowTextW(statusBox,s.c_str());}
void log(const std::wstring& s){if(GetWindowTextLengthW(detailBox)>1024*1024)SetWindowTextW(detailBox,L"[Earlier details omitted]\r\n");SendMessageW(detailBox,EM_SETSEL,-1,-1);std::wstring row=s+L"\r\n";SendMessageW(detailBox,EM_REPLACESEL,0,(LPARAM)row.c_str());}
fs::path data(){return fs::path(text(dataBox));}
bool supportedPython(const fs::path& p){
    // Isolated standard-library-only compatibility probe; no manager/Store aliases.
    std::wstring cmd=quote(p.wstring())+L" -I -S -c \"import sys,struct,platform,sysconfig;sys.exit(not(sys.implementation.name=='cpython' and (3,11)<=sys.version_info[:2]<=(3,14) and struct.calcsize('P')==8 and platform.machine().lower() in ('amd64','x86_64') and not sysconfig.get_config_var('Py_GIL_DISABLED')))\"";
    STARTUPINFOW si{sizeof si};PROCESS_INFORMATION pi{};
    if(!CreateProcessW(p.c_str(),cmd.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi))return false;
    DWORD code=1;if(WaitForSingleObject(pi.hProcess,5000)==WAIT_TIMEOUT){TerminateProcess(pi.hProcess,1);WaitForSingleObject(pi.hProcess,INFINITE);}else GetExitCodeProcess(pi.hProcess,&code);
    CloseHandle(pi.hThread);CloseHandle(pi.hProcess);return code==0;
}
fs::path findPython(bool allowOwned=true){
    if(noPythonPreview)return{};
    std::vector<fs::path> candidates;
    auto own=data()/L"setup-python/Scripts/python.exe";if(allowOwned&&fs::is_regular_file(own))candidates.push_back(own);
    // Existing paths only: never execute Store aliases or Python Install Manager discovery.
    wchar_t found[32768];
    for(auto hive:{HKEY_CURRENT_USER,HKEY_LOCAL_MACHINE})for(auto version:{L"3.14",L"3.13",L"3.12",L"3.11"}){
        std::wstring key=L"Software\\Python\\PythonCore\\"+std::wstring(version)+L"\\InstallPath";DWORD bytes=sizeof found;
        if(RegGetValueW(hive,key.c_str(),L"ExecutablePath",RRF_RT_REG_SZ,nullptr,found,&bytes)==ERROR_SUCCESS&&fs::is_regular_file(found))candidates.emplace_back(found);
        bytes=sizeof found;if(RegGetValueW(hive,key.c_str(),nullptr,RRF_RT_REG_SZ,nullptr,found,&bytes)==ERROR_SUCCESS){
            auto p=fs::path(found)/L"python.exe";if(fs::is_regular_file(p))candidates.push_back(p);
        }
    }for(const auto& p:candidates)if(supportedPython(p))return p;return{};
}
void controls(){
    const bool bound=ready&&certifiedData==text(dataBox);
    EnableWindow(prepareButton,!busy&&requirements);EnableWindow(playButton,!busy&&bound&&!demo);EnableWindow(shortcutButton,!busy&&bound&&!demo);
    EnableWindow(cancelButton,busy&&!publishing&&!launching);
    for(int id:{Game,Keys,Data,BrowseGame,BrowseKeys,BrowseData,Check,Components,PythonSite,ToolsSite})EnableWindow(GetDlgItem(window,id),!busy);
}
void run(std::wstring action){
    if(busy)return;
    // Normal setup enters through the Windows system helper, before Python exists.
    // Only the explicitly synthetic preview uses a pre-existing interpreter.
    fs::path previewPython;if(demo){previewPython=findPython(false);if(previewPython.empty()){state(L"Preview interpreter unavailable. Normal setup installs its own private tools and needs no installed Python.");return;}}
    fs::path root;try{root=fs::absolute(data());fs::create_directories(root);}catch(...){state(L"Choose a writable data folder.");return;}
    if(action==L"prepare"&&!demo&&(!fs::is_regular_file(text(gameBox))||!fs::is_regular_file(text(keyBox)))){state(L"Choose your existing NSP and prod.keys files. They stay in place.");return;}
    if(action==L"tools"&&!demo&&MessageBoxW(window,L"Download pinned private tools into your chosen Data folder?\n\nPython, LLVM-MinGW C/C++ tools, CMake, Ninja and four Python components. About 260 MB of downloads; allow at least 3 GB for tools plus space for preparing your game.\n\nDownloads come from python.org, GitHub and PyPI and are verified against fixed SHA-256 checksums. No administrator access, system installation or global PATH change. Component license notices stay with the tools.",L"Install private tools",MB_OKCANCEL|MB_ICONINFORMATION)!=IDOK)return;
    GUID guid;CoCreateGuid(&guid);wchar_t nonce[40];swprintf(nonce,40,L"%08x%04x%04x%02x%02x%02x%02x%02x%02x%02x%02x",guid.Data1,guid.Data2,guid.Data3,guid.Data4[0],guid.Data4[1],guid.Data4[2],guid.Data4[3],guid.Data4[4],guid.Data4[5],guid.Data4[6],guid.Data4[7]);
    wchar_t system[32768];if(!GetSystemDirectoryW(system,32768)){state(L"Windows system tools could not be located.");return;}
    auto shell=fs::path(system)/L"WindowsPowerShell/v1.0/powershell.exe";
    std::wstring command=quote(shell.wstring())+L" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "+quote((source/L"tools/setup_ui/install_private_python.ps1").wstring())+L" -Action "+action+L" -DataRoot "+quote(root.wstring())+L" -SourceRoot "+quote(source.wstring())+L" -Nonce "+nonce;
    if(demo)command+=L" -Preview -PreviewPython "+quote(previewPython.wstring());
    if(demoFail)command+=L" -PreviewFail";
    if(action==L"prepare")command+=L" -Nsp "+quote(text(gameBox))+L" -Keys "+quote(text(keyBox));
    if(text(dataBox)!=root.wstring())SetWindowTextW(dataBox,root.c_str());activeData=root.wstring();activeAction=action;
    if(action==L"tools")requirements=ready=false;
    busy=true;publishing=false;committed=false;hadPublishPermission=false;failureMessage.clear();launching=action==L"launch";pendingReady=false;pendingChecked=false;cancelRequested=false;unsigned gen=++generation;controls();state(L"Starting…");SendMessageW(progress,PBM_SETMARQUEE,TRUE,35);
    const bool cancellable=action!=L"launch";std::string prefix="HWDER_UI:"+utf8(nonce)+":";
    std::thread([command,gen,cancellable,root,prefix]{
        SECURITY_ATTRIBUTES sa{sizeof sa,nullptr,TRUE};HANDLE read=nullptr,write=nullptr,inRead=nullptr,inWrite=nullptr;
        HANDLE job=nullptr;PROCESS_INFORMATION pi{};DWORD code=1;bool assigned=false;
        auto fail=[&](const wchar_t* message){post(gen,message);};
        if(CreatePipe(&read,&write,&sa,0)&&CreatePipe(&inRead,&inWrite,&sa,0)){
            SetHandleInformation(read,HANDLE_FLAG_INHERIT,0);SetHandleInformation(inWrite,HANDLE_FLAG_INHERIT,0);
            STARTUPINFOEXW si{};si.StartupInfo.cb=sizeof si;si.StartupInfo.dwFlags=STARTF_USESTDHANDLES;si.StartupInfo.hStdOutput=si.StartupInfo.hStdError=write;si.StartupInfo.hStdInput=inRead;
            SIZE_T size=0;InitializeProcThreadAttributeList(nullptr,1,0,&size);std::vector<unsigned char> attrs(size);si.lpAttributeList=(LPPROC_THREAD_ATTRIBUTE_LIST)attrs.data();
            if(InitializeProcThreadAttributeList(si.lpAttributeList,1,0,&size)){
                HANDLE inherited[]={write,inRead};
                if(UpdateProcThreadAttribute(si.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited,sizeof inherited,nullptr,nullptr)){
                    std::wstring cmd=command;
                    if(CreateProcessW(nullptr,cmd.data(),nullptr,nullptr,TRUE,CREATE_SUSPENDED|CREATE_NO_WINDOW|EXTENDED_STARTUPINFO_PRESENT,nullptr,source.c_str(),&si.StartupInfo,&pi)){
                        if(cancellable){job=CreateJobObjectW(nullptr,nullptr);JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                            assigned=job&&SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof limits)&&AssignProcessToJobObject(job,pi.hProcess);
                        }else assigned=true;
                        if(assigned){std::lock_guard<std::mutex> lock(handles);currentJob=job;inputWrite=inWrite;inWrite=nullptr;if(cancelRequested&&job)TerminateJobObject(job,130);else if(ResumeThread(pi.hThread)==DWORD(-1)){if(job)TerminateJobObject(job,1);else TerminateProcess(pi.hProcess,1);fail(L"Could not resume the owned setup process.");}}
                        else{TerminateProcess(pi.hProcess,1);fail(L"Could not safely own the setup process. No uncontrolled child was started.");}
                    }else fail(L"Could not start Windows PowerShell. See Details or check Windows system tools.");
                }DeleteProcThreadAttributeList(si.lpAttributeList);
            }
        }else fail(L"Could not create setup communication pipes.");
        if(write){CloseHandle(write);write=nullptr;}if(inRead){CloseHandle(inRead);inRead=nullptr;}
        if(pi.hProcess){
            if(assigned){
                std::ofstream logfile(root/L"setup-last-run.log",std::ios::binary|std::ios::trunc);size_t logged=0;std::string pending;char buf[4096];DWORD n=0;
                while(ReadFile(read,buf,sizeof buf,&n,nullptr)&&n){
                    if(logged<4*1024*1024){auto take=std::min<size_t>(n,4*1024*1024-logged);logfile.write(buf,take);logged+=take;}
                    pending.append(buf,n);size_t at;while((at=pending.find('\n'))!=pending.npos){auto line=pending.substr(0,at);pending.erase(0,at+1);bool control=line.rfind(prefix,0)==0;post(gen,wide(control?line.substr(prefix.size()):line),control);}
                    if(pending.size()>65536){post(gen,wide(pending.substr(0,65536)));pending.clear();}
                }if(!pending.empty())post(gen,wide(pending));
            }
            WaitForSingleObject(pi.hProcess,INFINITE);GetExitCodeProcess(pi.hProcess,&code);CloseHandle(pi.hThread);CloseHandle(pi.hProcess);
        }
        {std::lock_guard<std::mutex> lock(handles);currentJob=nullptr;if(inputWrite)CloseHandle(inputWrite);inputWrite=nullptr;if(job)CloseHandle(job);}
        if(inWrite)CloseHandle(inWrite); // only a pre-transfer startup failure retains local ownership
        if(read)CloseHandle(read);if(write)CloseHandle(write);if(inRead)CloseHandle(inRead);
        post(gen,L"",false,code);
    }).detach();
}
void browse(int id){
    if(id==BrowseData){IFileDialog* dialog=nullptr;if(SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog)))){
        DWORD flags;dialog->GetOptions(&flags);dialog->SetOptions(flags|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM);
        if(SUCCEEDED(dialog->Show(window))){IShellItem* item=nullptr;if(SUCCEEDED(dialog->GetResult(&item))){PWSTR p=nullptr;if(SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&p))){SetWindowTextW(dataBox,p);CoTaskMemFree(p);requirements=ready=false;controls();}item->Release();}}dialog->Release();}return;}
    wchar_t name[32768]{};OPENFILENAMEW of{sizeof of};of.hwndOwner=window;of.lpstrFile=name;of.nMaxFile=32768;of.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
    of.lpstrFilter=id==BrowseGame?L"Game dump (*.nsp)\0*.nsp\0\0":L"Key file (prod.keys)\0*.keys\0\0";
    if(GetOpenFileNameW(&of))SetWindowTextW(id==BrowseGame?gameBox:keyBox,name);
}
void shortcut(){
    wchar_t desktop[MAX_PATH];if(FAILED(SHGetFolderPathW(nullptr,CSIDL_DESKTOPDIRECTORY,nullptr,0,desktop)))return;
    IShellLinkW* link=nullptr;if(SUCCEEDED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&link)))){
        link->SetPath(application.c_str());std::wstring args=L"--data "+quote(data().wstring())+L" --play";link->SetArguments(args.c_str());link->SetDescription(L"HWDER — play your verified prepared game");link->SetWorkingDirectory(application.parent_path().c_str());
        IPersistFile* file=nullptr;if(SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file)))){auto path=fs::path(desktop)/L"HWDER.lnk";if(fs::exists(path))state(L"Desktop HWDER shortcut already exists; it was not overwritten.");else if(SUCCEEDED(file->Save(path.c_str(),TRUE)))state(L"Desktop shortcut created. It opens this setup with your data folder.");file->Release();}link->Release();}
}
HWND control(const wchar_t* cls,const wchar_t* label,DWORD style,int x,int y,int w,int h,int id=0){HWND c=CreateWindowExW(cls==std::wstring(L"EDIT")?WS_EX_CLIENTEDGE:0,cls,label,WS_CHILD|WS_VISIBLE|style,x,y,w,h,window,(HMENU)(INT_PTR)id,GetModuleHandleW(nullptr),nullptr);SendMessageW(c,WM_SETFONT,(WPARAM)font,TRUE);return c;}
LRESULT CALLBACK procedure(HWND h,UINT message,WPARAM wp,LPARAM lp){
    if(message==WM_CTLCOLORSTATIC){SetBkColor((HDC)wp,RGB(255,255,255));return(LRESULT)GetStockObject(WHITE_BRUSH);}
    if(message==WM_CREATE){window=h;font=CreateFontW(-17,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");titleFont=CreateFontW(-27,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        auto title=control(L"STATIC",L"Let’s get HWDER ready",0,28,20,780,40);SendMessageW(title,WM_SETFONT,(WPARAM)titleFont,TRUE);
        control(L"STATIC",L"Use your own game dump and keys. They stay where you choose — no keys are copied.",0,28,64,780,30);
        control(L"STATIC",L"Game file (.nsp)",0,28,108,180,24);gameBox=control(L"EDIT",L"",ES_AUTOHSCROLL,210,104,505,28,Game);control(L"BUTTON",L"Browse…",BS_PUSHBUTTON,727,104,95,28,BrowseGame);
        control(L"STATIC",L"Your prod.keys file",0,28,148,180,24);keyBox=control(L"EDIT",L"",ES_AUTOHSCROLL,210,144,505,28,Keys);control(L"BUTTON",L"Browse…",BS_PUSHBUTTON,727,144,95,28,BrowseKeys);
        control(L"STATIC",L"Data folder",0,28,188,180,24);dataBox=control(L"EDIT",L"",ES_AUTOHSCROLL,210,184,505,28,Data);control(L"BUTTON",L"Choose…",BS_PUSHBUTTON,727,184,95,28,BrowseData);
        control(L"STATIC",L"Saves and settings stay in Data folder\\play. Application files stay beside this setup.",0,210,217,615,26);
        control(L"BUTTON",L"Check requirements",BS_PUSHBUTTON,28,258,170,32,Check);control(L"BUTTON",L"About tools",BS_PUSHBUTTON,210,258,130,32,PythonSite);control(L"BUTTON",L"Open data folder",BS_PUSHBUTTON,352,258,160,32,ToolsSite);control(L"BUTTON",L"Install private tools",BS_PUSHBUTTON,524,258,220,32,Components);
        control(L"STATIC",L"Private tools stay in your Data folder. No Python or Visual Studio installation is needed.",0,28,301,790,28);
        statusBox=control(L"STATIC",L"First install private tools, then Check requirements. Downloads always need your confirmation.",0,28,345,790,54);
        progress=control(PROGRESS_CLASSW,L"",PBS_MARQUEE,28,405,790,10);
        prepareButton=control(L"BUTTON",L"Prepare game",BS_DEFPUSHBUTTON,28,438,165,36,Prepare);playButton=control(L"BUTTON",L"Play",BS_PUSHBUTTON,205,438,100,36,Play);shortcutButton=control(L"BUTTON",L"Desktop shortcut",BS_PUSHBUTTON,317,438,160,36,Shortcut);cancelButton=control(L"BUTTON",L"Cancel",BS_PUSHBUTTON,489,438,110,36,Cancel);control(L"BUTTON",L"Show details",BS_AUTOCHECKBOX,650,442,170,30,Details);
        detailBox=control(L"EDIT",L"",ES_MULTILINE|ES_READONLY|WS_VSCROLL|ES_AUTOVSCROLL,28,492,790,185);ShowWindow(detailBox,SW_HIDE);controls();return 0;}
    if(message==WM_COMMAND){int id=LOWORD(wp);if(HIWORD(wp)==EN_CHANGE&&(id==Game||id==Keys||id==Data)){ready=false;if(id==Data)requirements=false;controls();return 0;}if(id==BrowseGame||id==BrowseKeys||id==BrowseData)browse(id);else if(id==Check)run(L"check");else if(id==Components)run(L"tools");else if(id==Prepare)run(L"prepare");else if(id==Play)run(L"launch");else if(id==Shortcut)shortcut();else if(id==Cancel){cancelRequested=true;std::lock_guard<std::mutex> lock(handles);if(!publishing&&currentJob)TerminateJobObject(currentJob,130);state(publishing?L"Finishing the safe update; this commit may complete before cancellation.":L"Cancelling owned setup work; your previous playable version is retained.");}
        else if(id==Details){bool shown=SendMessageW(GetDlgItem(h,Details),BM_GETCHECK,0,0)==BST_CHECKED;ShowWindow(detailBox,shown?SW_SHOW:SW_HIDE);SetWindowPos(h,nullptr,0,0,866,shown?735:535,SWP_NOMOVE|SWP_NOZORDER);}
        else if(id==PythonSite){MessageBoxW(h,L"HWDER uses its own pinned Python, LLVM-MinGW, CMake and Ninja, plus cryptography, lz4, cffi and pycparser.\n\nInstall private tools is the only action that downloads. Check, Prepare and Play only verify existing tools.\n\nTools and full license notices live in the Data folder. No global PATH, registry or administrator installation is needed.",L"About private tools",MB_OK|MB_ICONINFORMATION);}
        else if(id==ToolsSite){if(fs::is_directory(data()))ShellExecuteW(h,L"open",data().c_str(),nullptr,nullptr,SW_SHOWNORMAL);else state(L"Choose or create your Data folder first.");}return 0;}
    if(message==Event){std::unique_ptr<Notice> p((Notice*)lp);if(p->generation!=generation)return 0;
        if(p->exit!=STILL_ACTIVE){busy=false;publishing=false;launching=false;SendMessageW(progress,PBM_SETMARQUEE,FALSE,0);ready=p->exit==0&&pendingReady&&!cancelRequested;if(ready)certifiedData=activeData;if(activeAction==L"check")requirements=p->exit==0&&pendingChecked;if(cancelRequested)state(committed?L"The safe update finished before closing. Open setup again to verify and play.":hadPublishPermission?L"Setup ended after the safe update boundary. Retry to verify the playable version.":L"Cancelled before updating the playable version; you can retry.");else if(p->exit!=0){ready=false;state(failureMessage.empty()?L"Setup did not finish. Saves/settings are retained; retry to verify the playable version.":failureMessage);SetWindowTextW(prepareButton,L"Retry");}controls();if(closeDeferred)DestroyWindow(h);return 0;}
        if(!p->control){log(p->line);return 0;}auto type=field(p->line,L"type"),msg=field(p->line,L"message");log(msg);
        if(type==L"publish_request"){std::lock_guard<std::mutex> lock(handles);const char* reply=cancelRequested?"cancel\n":"commit\n";publishing=!cancelRequested;hadPublishPermission=publishing;DWORD done=0;if(inputWrite&&(!WriteFile(inputWrite,reply,(DWORD)strlen(reply),&done,nullptr)||done!=strlen(reply))){CloseHandle(inputWrite);inputWrite=nullptr;log(L"Commit acknowledgement channel failed; waiting for safe completion/rollback.");}controls();}
        else if(type==L"checked")pendingChecked=true;else if(type==L"ready"||type==L"ready_available"){pendingReady=true;committed=type==L"ready";}
        else if(type==L"failed")failureMessage=msg;
        if(type!=L"log")state(msg);return 0;}
    if(message==WM_CLOSE){if(busy){if(launching){closeDeferred=true;state(L"Finishing the explicit launch check before closing…");return 0;}if(MessageBoxW(h,publishing?L"Close when the safe update finishes? The new playable version may be committed.":L"Cancel the current setup and close? The playable version has not been updated.",L"Close setup",MB_YESNO|MB_ICONQUESTION)!=IDYES)return 0;cancelRequested=true;closeDeferred=true;std::lock_guard<std::mutex> lock(handles);if(!publishing&&currentJob)TerminateJobObject(currentJob,130);if(publishing)state(L"Finishing the safe update before closing…");return 0;}DestroyWindow(h);return 0;}
    if(message==WM_DESTROY){PostQuitMessage(0);return 0;}return DefWindowProcW(h,message,wp,lp);
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int){
    // Native controls use a 96-DPI layout; Windows scales GDI text/geometry for desktop scaling.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_UNAWARE_GDISCALED);CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);INITCOMMONCONTROLSEX cc{sizeof cc,ICC_PROGRESS_CLASS};InitCommonControlsEx(&cc);
    wchar_t exe[32768];GetModuleFileNameW(nullptr,exe,32768);application=exe;source=application.parent_path()/L"source";if(!fs::is_regular_file(source/L"tools/setup_bridge.py")){source=application.parent_path();for(int n=0;n<4&&!fs::is_regular_file(source/L"tools/setup_bridge.py");++n)source=source.parent_path();}
    if(!fs::is_regular_file(source/L"tools/setup_bridge.py")||!fs::is_regular_file(source/L"tools/managed_toolchain.json")||!fs::is_regular_file(source/L"tools/setup_ui/install_private_python.ps1")){MessageBoxW(nullptr,L"Keep HWDER Setup.exe beside its complete source folder. Extract the whole setup kit first.",L"Setup files missing",MB_OK|MB_ICONERROR);return 1;}
    int argc=0;auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);std::wstring selected;bool autoPlay=false;for(int i=1;i<argc;++i){if(std::wstring(argv[i])==L"--demo")demo=true;else if(std::wstring(argv[i])==L"--demo-no-python"){demo=true;noPythonPreview=true;}else if(std::wstring(argv[i])==L"--demo-fail"){demo=true;demoFail=true;}else if(std::wstring(argv[i])==L"--play")autoPlay=true;else if(std::wstring(argv[i])==L"--data"&&i+1<argc)selected=argv[++i];}LocalFree(argv);
    if(selected.empty()){PWSTR local=nullptr;SHGetKnownFolderPath(FOLDERID_LocalAppData,KF_FLAG_DEFAULT,nullptr,&local);selected=(fs::path(local)/L"HWDER").wstring();CoTaskMemFree(local);}
    WNDCLASSW cls{};cls.lpfnWndProc=procedure;cls.hInstance=instance;cls.lpszClassName=L"HWDERSetupPrototype";cls.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);cls.hCursor=LoadCursor(nullptr,IDC_ARROW);RegisterClassW(&cls);
    window=CreateWindowW(cls.lpszClassName,demo?L"HWDER Setup — Preview (no game or installs)":L"HWDER Setup",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,CW_USEDEFAULT,CW_USEDEFAULT,866,535,nullptr,nullptr,instance,nullptr);SetWindowTextW(dataBox,selected.c_str());
    std::ifstream saved(fs::path(selected)/L"setup-inputs.json",std::ios::binary);if(saved){std::string value((std::istreambuf_iterator<char>(saved)),{});auto json=wide(value);SetWindowTextW(gameBox,field(json,L"nsp").c_str());SetWindowTextW(keyBox,field(json,L"keys").c_str());}
    ShowWindow(window,SW_SHOW);UpdateWindow(window);if(autoPlay&&!demo)run(L"launch");
    MSG message;while(GetMessageW(&message,nullptr,0,0)>0){if(!IsDialogMessageW(window,&message)){TranslateMessage(&message);DispatchMessageW(&message);}}CoUninitialize();return 0;
}
