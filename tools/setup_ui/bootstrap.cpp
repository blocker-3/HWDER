// Code-free native Windows setup shell. Links only Windows system libraries.
// One screen, one primary button whose label follows the installation state
// (Next / Install / Prepare / Retry / Play); everything else is under Advanced.
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
enum { Game=101,Keys,Data,BrowseGame,BrowseKeys,BrowseData,Check,PythonSite,ToolsSite,Components,Primary,Unused112,Shortcut,Details,Cancel,OpenGame,Advanced };
enum class Step { Inputs,Install,Prepare,Retry,Play };
constexpr int Width=866;
HWND window,gameBox,keyBox,dataBox,statusBox,progress,progressBox,detailBox,primaryButton,shortcutButton,cancelButton,locationBox,titleBox,advancedButton;
std::vector<HWND> advancedControls;
HFONT font,titleFont;fs::path source,application;bool demo=false,busy=false,publishing=false,closeDeferred=false,ready=false,pendingReady=false,launching=false,committed=false,hadPublishPermission=false;
bool advanced=false,detailsShown=false,toolsMissingHint=false,retryHint=false;
std::wstring activeData,certifiedData,activeAction,failureMessage,failureNext,stageTitle;
int stageStep=0,stageSteps=0;unsigned long long actionStart=0,buildStart=0;unsigned buildDone=0,buildTotal=0,buildFirst=0;
bool demoFail=false;
bool noPythonPreview=false;
bool lifecycleRace=false;bool pathsReady=false;bool lifecyclePreview=false;fs::path installedExe,shortcutDirectory;HANDLE runningGame=nullptr;
unsigned generation=0;std::atomic<bool> cancelRequested{false};std::mutex handles;HANDLE currentJob=nullptr,inputWrite=nullptr;
struct Notice{unsigned generation;std::wstring line;DWORD exit=STILL_ACTIVE;bool control=false;};
std::wstring wide(const std::string& s){if(s.empty())return{};int n=MultiByteToWideChar(CP_UTF8,0,s.data(),(int)s.size(),nullptr,0);std::wstring r(n,0);MultiByteToWideChar(CP_UTF8,0,s.data(),(int)s.size(),r.data(),n);return r;}
std::string utf8(const std::wstring& s){if(s.empty())return{};int n=WideCharToMultiByte(CP_UTF8,0,s.data(),(int)s.size(),nullptr,0,nullptr,nullptr);std::string r(n,0);WideCharToMultiByte(CP_UTF8,0,s.data(),(int)s.size(),r.data(),n,nullptr,nullptr);return r;}
std::wstring text(HWND h){int n=GetWindowTextLengthW(h);std::wstring s(n+1,0);GetWindowTextW(h,s.data(),n+1);s.resize(n);return s;}
std::wstring quote(const std::wstring& s){std::wstring r=L"\"";unsigned slash=0;for(wchar_t c:s){if(c==L'\\'){++slash;continue;}if(c==L'"'){r.append(slash*2+1,L'\\');r+=c;}else{r.append(slash,L'\\');r+=c;}slash=0;}r.append(slash*2,L'\\');return r+L"\"";}
std::wstring field(const std::wstring& s,const std::wstring& key){auto at=s.find(L"\""+key+L"\"");if(at==s.npos)return{};at=s.find(L':',at+key.size()+2);if(at==s.npos)return{};at=s.find(L'"',at+1);if(at==s.npos)return{};std::wstring out;for(++at;at<s.size();++at){wchar_t c=s[at];if(c==L'"')return out;if(c==L'\\'&&at+1<s.size()){c=s[++at];if(c==L'n')c=L'\n';else if(c==L'r')c=L'\r';else if(c==L't')c=L'\t';}out+=c;}return{};}
unsigned number(const std::wstring& s){unsigned v=0;for(wchar_t c:s){if(c<L'0'||c>L'9')return v;v=v*10+(c-L'0');}return v;}
std::string jsonText(const std::wstring& s){std::string r="\"";for(unsigned char c:utf8(s)){if(c=='"'||c=='\\'){r+='\\';r+=(char)c;}else if(c<0x20){char b[8];snprintf(b,8,"\\u%04x",c);r+=b;}else r+=(char)c;}return r+"\"";}
void post(unsigned gen,std::wstring line,bool control=false,DWORD exit=STILL_ACTIVE){auto* p=new Notice{gen,std::move(line),exit,control};if(!PostMessageW(window,Event,0,(LPARAM)p))delete p;}
void state(const std::wstring& s){SetWindowTextW(statusBox,s.c_str());}
void log(const std::wstring& s){if(GetWindowTextLengthW(detailBox)>1024*1024)SetWindowTextW(detailBox,L"[Earlier details omitted]\r\n");SendMessageW(detailBox,EM_SETSEL,-1,-1);std::wstring row=s+L"\r\n";SendMessageW(detailBox,EM_REPLACESEL,0,(LPARAM)row.c_str());}
fs::path data(){return fs::path(text(dataBox));}
bool isFile(const std::wstring& p){std::error_code e;return !p.empty()&&fs::is_regular_file(fs::path(p),e);}
bool samePath(const fs::path& a,const fs::path& b){try{auto x=fs::absolute(a).lexically_normal().make_preferred().wstring(),y=fs::absolute(b).lexically_normal().make_preferred().wstring();return CompareStringOrdinal(x.c_str(),-1,y.c_str(),-1,TRUE)==CSTR_EQUAL;}catch(...){return false;}}
// Cheap presence probe only. Prepare verifies every tool file itself and reports Install when they are missing or changed.
bool toolsPresent(){std::error_code e;auto root=data()/L"managed-tools";if(!fs::is_directory(root,e))return false;for(auto& entry:fs::directory_iterator(root,e)){if(entry.is_directory(e)&&fs::is_regular_file(entry.path()/L"complete.json",e))return true;}return false;}
fs::path prefsPath(){PWSTR local=nullptr;fs::path p;if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData,KF_FLAG_DEFAULT,nullptr,&local))){p=fs::path(local)/L"HWDER"/L"setup-prefs.json";CoTaskMemFree(local);}return p;}
// Picked-but-unprepared paths live in the Data folder; a successful Prepare writes setup-inputs.json, which wins.
void savePrefs(){if(demo)return;std::error_code e;auto root=data();if(root.empty())return;fs::create_directories(root,e);std::ofstream out(root/L"setup-prefs.json",std::ios::binary|std::ios::trunc);if(!out)return;out<<"{\"schema\":1,\"nsp\":"<<jsonText(text(gameBox))<<",\"keys\":"<<jsonText(text(keyBox))<<"}\n";}
// Only an explicitly chosen Data folder is remembered outside it (default Local AppData\HWDER).
void rememberData(){if(demo)return;auto p=prefsPath();if(p.empty())return;std::error_code e;fs::create_directories(p.parent_path(),e);std::ofstream out(p,std::ios::binary|std::ios::trunc);if(!out)return;out<<"{\"schema\":1,\"data\":"<<jsonText(text(dataBox))<<"}\n";}
std::wstring readJson(const fs::path& p){std::ifstream in(p,std::ios::binary);if(!in)return{};std::string value((std::istreambuf_iterator<char>(in)),{});return wide(value);}
void forgetGame(){if(runningGame)CloseHandle(runningGame);runningGame=nullptr;KillTimer(window,1);}
bool monitorGame(const std::wstring& json){
    forgetGame();auto pid=field(json,L"pid"),created=field(json,L"created");if(pid.empty()&&created.empty())return true;
    try{
        size_t p=0,c=0;auto id=std::stoul(pid,&p);auto stamp=std::stoull(created,&c);if(p!=pid.size()||c!=created.size()||!id)return false;
        HANDLE process=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,id);
        if(!process)return GetLastError()==ERROR_INVALID_PARAMETER; // Already exited is still a ready game.
        if(WaitForSingleObject(process,0)==WAIT_OBJECT_0){CloseHandle(process);return true;}
        wchar_t name[32768];DWORD count=32768;FILETIME start,end,kernel,user;
        bool match=QueryFullProcessImageNameW(process,0,name,&count)&&GetProcessTimes(process,&start,&end,&kernel,&user)&&
            (((ULONGLONG)start.dwHighDateTime<<32)|start.dwLowDateTime)==stamp&&
            CompareStringOrdinal(name,-1,installedExe.c_str(),-1,TRUE)==CSTR_EQUAL;
        if(!match){CloseHandle(process);return false;}runningGame=process;SetTimer(window,1,250,nullptr);return true;
    }catch(...){return false;}
}
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
// The whole state machine. Everything the window shows derives from this.
Step step(){
    if(ready&&certifiedData==text(dataBox))return Step::Play;
    if(!demo&&(!isFile(text(gameBox))||!isFile(text(keyBox))))return Step::Inputs;
    if(!demo&&(toolsMissingHint||!toolsPresent()))return Step::Install;
    return retryHint?Step::Retry:Step::Prepare;
}
const wchar_t* label(Step s){switch(s){case Step::Inputs:return L"Next";case Step::Install:return L"Install";case Step::Prepare:return L"Prepare";case Step::Retry:return L"Retry";default:return L"Play";}}
std::wstring hint(Step s){
    switch(s){
    case Step::Inputs:return L"Pick your game file (.nsp) and your prod.keys. They stay where they are.";
    case Step::Install:return L"Private tools (Python, compiler, CMake, Ninja) go into your Data folder. Choose Install; downloads need your confirmation.";
    case Step::Prepare:return L"Everything is in place. Choose Prepare to build your game. This takes several minutes.";
    case Step::Retry:return L"Choose Retry to run preparation again. Nothing was changed.";
    default:return L"Ready to play. Choose Play now, or create a Desktop shortcut under Advanced.";}
}
void layout(){
    bool details=advanced&&detailsShown;for(HWND h:advancedControls)ShowWindow(h,advanced?SW_SHOW:SW_HIDE);ShowWindow(detailBox,details?SW_SHOW:SW_HIDE);
    SetWindowTextW(advancedButton,advanced?L"Advanced \u25B4":L"Advanced \u25BE");
    RECT r{0,0,Width,details?727:advanced?538:360};AdjustWindowRect(&r,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,FALSE);
    SetWindowPos(window,nullptr,0,0,r.right-r.left,r.bottom-r.top,SWP_NOMOVE|SWP_NOZORDER);
}
void controls(){
    const Step s=step();const bool bound=s==Step::Play;
    SetWindowTextW(primaryButton,label(s));
    EnableWindow(primaryButton,!busy&&!runningGame&&(s!=Step::Play||!demo||lifecyclePreview));
    SendMessageW(primaryButton,BM_SETSTYLE,BS_DEFPUSHBUTTON,TRUE);
    EnableWindow(shortcutButton,!busy&&bound&&pathsReady&&(!demo||lifecyclePreview));
    EnableWindow(GetDlgItem(window,OpenGame),!busy&&bound);SetWindowTextW(titleBox,bound?L"Ready to play":L"Let\u2019s get HWDER ready");
    SetWindowTextW(locationBox,bound?(L"Installed game: "+installedExe.wstring()+(pathsReady?L"\r\nThe installed game needs no setup kit; a Desktop shortcut starts it directly.":L"\r\nGame running. The installed shortcut handoff finishes when it closes.")).c_str():L"Private tools stay in your Data folder. No Python or Visual Studio installation is needed.");
    EnableWindow(cancelButton,busy&&!publishing&&!launching);
    for(int id:{Game,Keys,Data,BrowseGame,BrowseKeys,BrowseData,Check,Components,PythonSite,ToolsSite,Advanced,Details})EnableWindow(GetDlgItem(window,id),!busy);
}
void refresh(){controls();if(!busy&&failureMessage.empty())state(hint(step()));}
std::wstring elapsedText(unsigned long long ms){wchar_t b[32];unsigned s=(unsigned)(ms/1000);swprintf(b,32,L"%u:%02u",s/60,s%60);return b;}
void progressLine(){
    if(!busy){SetWindowTextW(progressBox,L"");return;}
    auto now=GetTickCount64();std::wstring line;
    if(stageStep)line=L"Step "+std::to_wstring(stageStep)+L" of "+std::to_wstring(stageSteps)+L" \u00B7 "+stageTitle;else if(!stageTitle.empty())line=stageTitle;
    if(buildTotal){line+=L" \u00B7 "+std::to_wstring(buildDone)+L" of "+std::to_wstring(buildTotal)+L" files";
        auto elapsed=now-buildStart;unsigned made=buildDone>buildFirst?buildDone-buildFirst:0;
        if(made>=20&&elapsed>=5000&&buildDone<buildTotal){double rate=made/(double)elapsed;unsigned long long left=(unsigned long long)((buildTotal-buildDone)/rate);unsigned minutes=(unsigned)((left+59999)/60000);line+=minutes<=1?L" \u00B7 under a minute left":L" \u00B7 about "+std::to_wstring(minutes)+L" min left";}}
    if(actionStart)line+=(line.empty()?L"":L" \u00B7 ")+elapsedText(now-actionStart)+L" elapsed";
    SetWindowTextW(progressBox,line.c_str());
}
void marquee(bool on){LONG_PTR style=GetWindowLongPtrW(progress,GWL_STYLE);SetWindowLongPtrW(progress,GWL_STYLE,on?style|PBS_MARQUEE:style&~PBS_MARQUEE);SendMessageW(progress,PBM_SETMARQUEE,on,35);if(!on){SendMessageW(progress,PBM_SETRANGE32,0,buildTotal);SendMessageW(progress,PBM_SETPOS,buildDone,0);}}
bool buildLine(const std::wstring& line){ // Ninja "[done/total] ..." lines give real progress during the build step.
    if(line.size()<5||line[0]!=L'[')return false;size_t slash=line.find(L'/'),close=line.find(L']');if(slash==line.npos||close==line.npos||slash>close)return false;
    for(size_t i=1;i<close;++i)if(i!=slash&&(line[i]<L'0'||line[i]>L'9'))return false;
    unsigned done=number(line.substr(1,slash-1)),total=number(line.substr(slash+1,close-slash-1));if(!total||done>total)return false;
    if(!buildTotal||total!=buildTotal){buildStart=GetTickCount64();buildFirst=done;}buildDone=done;buildTotal=total;marquee(false);progressLine();return true;
}
void run(std::wstring action){
    if(busy)return;
    if(action==L"launch"&&runningGame){state(L"Your game is already running. Play will be available when it closes.");return;}
    // Normal setup enters through the Windows system helper, before Python exists.
    // Only the explicitly synthetic preview uses a pre-existing interpreter.
    fs::path previewPython;if(demo){previewPython=findPython(false);if(previewPython.empty()){state(L"Preview interpreter unavailable. Normal setup installs its own private tools and needs no installed Python.");return;}}
    fs::path root;try{root=fs::absolute(data());fs::create_directories(root);}catch(...){state(L"Choose a writable Data folder under Advanced.");return;}
    if(action==L"prepare"&&!demo&&(!isFile(text(gameBox))||!isFile(text(keyBox)))){state(L"Pick your game file (.nsp) and your prod.keys first. They stay in place.");return;}
    if(action==L"tools"&&!demo&&MessageBoxW(window,L"Download pinned private tools into your Data folder?\n\nPython, LLVM-MinGW C/C++ tools, CMake, Ninja and four Python components. About 260 MB of downloads; allow at least 3 GB for tools plus space for preparing your game.\n\nDownloads come from python.org, GitHub and PyPI and are verified against fixed SHA-256 checksums. No administrator access, system installation or global PATH change. Component license notices stay with the tools.",L"Install private tools",MB_OKCANCEL|MB_ICONINFORMATION)!=IDOK)return;
    GUID guid;CoCreateGuid(&guid);wchar_t nonce[40];swprintf(nonce,40,L"%08x%04x%04x%02x%02x%02x%02x%02x%02x%02x%02x",guid.Data1,guid.Data2,guid.Data3,guid.Data4[0],guid.Data4[1],guid.Data4[2],guid.Data4[3],guid.Data4[4],guid.Data4[5],guid.Data4[6],guid.Data4[7]);
    wchar_t system[32768];if(!GetSystemDirectoryW(system,32768)){state(L"Windows system tools could not be located.");return;}
    auto shell=fs::path(system)/L"WindowsPowerShell/v1.0/powershell.exe";
    std::wstring command=quote(shell.wstring())+L" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "+quote((source/L"tools/setup_ui/install_private_python.ps1").wstring())+L" -Action "+action+L" -DataRoot "+quote(root.wstring())+L" -SourceRoot "+quote(source.wstring())+L" -Nonce "+nonce;
    if(demo)command+=L" -Preview -PreviewPython "+quote(previewPython.wstring());
    if(demoFail)command+=L" -PreviewFail";
    if(lifecyclePreview)command+=L" -PreviewLifecycle";if(lifecycleRace)command+=L" -PreviewRace";
    if(action==L"prepare")command+=L" -Nsp "+quote(text(gameBox))+L" -Keys "+quote(text(keyBox));
    if(text(dataBox)!=root.wstring())SetWindowTextW(dataBox,root.c_str());activeData=root.wstring();activeAction=action;
    if(action==L"prepare"||action==L"tools")savePrefs();
    busy=true;publishing=false;committed=false;hadPublishPermission=false;failureMessage.clear();failureNext.clear();launching=action==L"launch";pendingReady=false;cancelRequested=false;unsigned gen=++generation;
    stageStep=stageSteps=0;stageTitle.clear();buildDone=buildTotal=buildFirst=0;buildStart=0;actionStart=action==L"status"?0:GetTickCount64();
    controls();state(action==L"status"?L"Checking your installed game\u2026":action==L"launch"?L"Starting your game\u2026":L"Starting\u2026");marquee(true);progressLine();SetTimer(window,2,1000,nullptr);
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
        if(SUCCEEDED(dialog->Show(window))){IShellItem* item=nullptr;if(SUCCEEDED(dialog->GetResult(&item))){PWSTR p=nullptr;if(SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&p))){SetWindowTextW(dataBox,p);CoTaskMemFree(p);ready=false;toolsMissingHint=false;rememberData();savePrefs();refresh();}item->Release();}}dialog->Release();}return;}
    wchar_t name[32768]{};OPENFILENAMEW of{sizeof of};of.hwndOwner=window;of.lpstrFile=name;of.nMaxFile=32768;of.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
    of.lpstrFilter=id==BrowseGame?L"Game dump (*.nsp)\0*.nsp\0\0":L"Key file (prod.keys)\0*.keys\0\0";of.lpstrTitle=id==BrowseGame?L"Choose your game file (.nsp)":L"Choose your prod.keys file";
    if(GetOpenFileNameW(&of)){SetWindowTextW(id==BrowseGame?gameBox:keyBox,name);savePrefs();}
}
void shortcut(){
    if(!ready||!pathsReady||certifiedData!=text(dataBox)||installedExe.empty())return;
    wchar_t desktop[MAX_PATH];if(shortcutDirectory.empty()&&FAILED(SHGetFolderPathW(nullptr,CSIDL_DESKTOPDIRECTORY,nullptr,0,desktop)))return;
    IShellLinkW* link=nullptr;if(SUCCEEDED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&link)))){
        link->SetPath(installedExe.c_str());link->SetDescription(L"HWDER \u2014 play your installed game");link->SetWorkingDirectory(installedExe.parent_path().c_str());
        IPersistFile* file=nullptr;if(SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file)))){auto path=(shortcutDirectory.empty()?fs::path(desktop):shortcutDirectory)/L"HWDER.lnk";if(fs::exists(path))state(L"A Desktop HWDER shortcut already exists; it was not overwritten.");else if(SUCCEEDED(file->Save(path.c_str(),TRUE)))state(L"Desktop HWDER shortcut created. It opens your installed game, even without this setup kit.");else state(L"Could not create a Desktop shortcut. Open the game folder to launch hwder.exe.");file->Release();}link->Release();}
}
HWND control(const wchar_t* cls,const wchar_t* label,DWORD style,int x,int y,int w,int h,int id=0,bool hidden=false){HWND c=CreateWindowExW(cls==std::wstring(L"EDIT")?WS_EX_CLIENTEDGE:0,cls,label,WS_CHILD|WS_VISIBLE|style,x,y,w,h,window,(HMENU)(INT_PTR)id,GetModuleHandleW(nullptr),nullptr);SendMessageW(c,WM_SETFONT,(WPARAM)font,TRUE);if(hidden)advancedControls.push_back(c);return c;}
void primary(){
    switch(step()){
    case Step::Inputs:browse(isFile(text(gameBox))?BrowseKeys:BrowseGame);refresh();break;
    case Step::Install:run(L"tools");break;
    case Step::Play:run(L"launch");break;
    default:run(L"prepare");}
}
LRESULT CALLBACK procedure(HWND h,UINT message,WPARAM wp,LPARAM lp){
    if(message==WM_CTLCOLORSTATIC){SetBkColor((HDC)wp,RGB(255,255,255));return(LRESULT)GetStockObject(WHITE_BRUSH);}
    if(message==WM_CREATE){window=h;font=CreateFontW(-17,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");titleFont=CreateFontW(-27,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        titleBox=control(L"STATIC",L"Let\u2019s get HWDER ready",0,28,20,780,40);SendMessageW(titleBox,WM_SETFONT,(WPARAM)titleFont,TRUE);
        control(L"STATIC",L"Use your own game dump and keys. They stay where they are; nothing is copied.",0,28,64,780,30);
        control(L"STATIC",L"Game file (.nsp)",0,28,108,180,24);gameBox=control(L"EDIT",L"",ES_AUTOHSCROLL,210,104,505,28,Game);control(L"BUTTON",L"Browse\u2026",BS_PUSHBUTTON,727,104,95,28,BrowseGame);
        control(L"STATIC",L"Your prod.keys file",0,28,148,180,24);keyBox=control(L"EDIT",L"",ES_AUTOHSCROLL,210,144,505,28,Keys);control(L"BUTTON",L"Browse\u2026",BS_PUSHBUTTON,727,144,95,28,BrowseKeys);
        progressBox=control(L"STATIC",L"",0,28,242,790,22);
        progress=control(PROGRESS_CLASSW,L"",PBS_MARQUEE,28,268,790,12);
        primaryButton=control(L"BUTTON",L"Next",BS_DEFPUSHBUTTON,28,296,170,40,Primary);cancelButton=control(L"BUTTON",L"Cancel",BS_PUSHBUTTON,210,298,90,36,Cancel);advancedButton=control(L"BUTTON",L"Advanced \u25BE",BS_PUSHBUTTON,690,298,132,36,Advanced);
        // Advanced: hidden until expanded.
        control(L"STATIC",L"Data folder",0,28,360,180,24,0,true);dataBox=control(L"EDIT",L"",ES_AUTOHSCROLL,210,356,505,28,Data,true);control(L"BUTTON",L"Choose\u2026",BS_PUSHBUTTON,727,356,95,28,BrowseData,true);
        locationBox=control(L"STATIC",L"",0,28,392,790,42,0,true);
        control(L"BUTTON",L"Check requirements",BS_PUSHBUTTON,28,438,170,36,Check,true);control(L"BUTTON",L"Install private tools",BS_PUSHBUTTON,210,438,190,36,Components,true);control(L"BUTTON",L"Open game folder",BS_PUSHBUTTON,412,438,150,36,OpenGame,true);shortcutButton=control(L"BUTTON",L"Create Desktop shortcut",BS_PUSHBUTTON,574,438,200,36,Shortcut,true);
        control(L"BUTTON",L"Open data folder",BS_PUSHBUTTON,28,482,150,36,ToolsSite,true);control(L"BUTTON",L"About tools",BS_PUSHBUTTON,190,482,120,36,PythonSite,true);control(L"BUTTON",L"Details",BS_AUTOCHECKBOX,322,486,90,30,Details,true);
        detailBox=control(L"EDIT",L"",ES_MULTILINE|ES_READONLY|WS_VSCROLL|ES_AUTOVSCROLL,28,522,790,185);
        statusBox=control(L"STATIC",L"",0,28,186,790,54); // created last: the tests read the final STATIC as the status line
        layout();refresh();return 0;}
    if(message==WM_COMMAND){int id=LOWORD(wp);if(HIWORD(wp)==EN_CHANGE&&(id==Game||id==Keys||id==Data)){ready=false;pathsReady=false;installedExe.clear();forgetGame();if(id==Data)toolsMissingHint=false;retryHint=false;if(!busy)failureMessage.clear();refresh();return 0;}if(id==BrowseGame||id==BrowseKeys||id==BrowseData){browse(id);refresh();}else if(id==Primary)primary();else if(id==Check)run(lifecyclePreview?L"status":L"check");else if(id==Components)run(L"tools");else if(id==Shortcut)shortcut();else if(id==OpenGame){if(ready&&certifiedData==text(dataBox)&&fs::is_directory(installedExe.parent_path()))ShellExecuteW(h,L"open",installedExe.parent_path().c_str(),nullptr,nullptr,SW_SHOWNORMAL);}else if(id==Cancel){cancelRequested=true;std::lock_guard<std::mutex> lock(handles);if(!publishing&&currentJob)TerminateJobObject(currentJob,130);state(publishing?L"Finishing the safe update; this commit may complete before cancellation.":L"Cancelling\u2026 your previous playable version is kept.");}
        else if(id==Advanced){advanced=!advanced;layout();}
        else if(id==Details){detailsShown=SendMessageW(GetDlgItem(h,Details),BM_GETCHECK,0,0)==BST_CHECKED;layout();}
        else if(id==PythonSite){MessageBoxW(h,L"HWDER uses its own pinned Python, LLVM-MinGW, CMake and Ninja, plus cryptography, lz4, cffi and pycparser.\n\nInstall is the only action that downloads. Prepare, Check and Play only verify existing tools.\n\nTools and full license notices live in the Data folder. No global PATH, registry or administrator installation is needed.",L"About private tools",MB_OK|MB_ICONINFORMATION);}
        else if(id==ToolsSite){if(fs::is_directory(data()))ShellExecuteW(h,L"open",data().c_str(),nullptr,nullptr,SW_SHOWNORMAL);else state(L"Choose or create your Data folder first.");}return 0;}
    if(message==Event){std::unique_ptr<Notice> p((Notice*)lp);if(p->generation!=generation)return 0;
        if(p->exit!=STILL_ACTIVE){busy=false;publishing=false;launching=false;KillTimer(window,2);marquee(false);SendMessageW(progress,PBM_SETPOS,0,0);progressLine();ready=p->exit==0&&pendingReady&&!cancelRequested;
            if(ready){certifiedData=activeData;retryHint=false;} // the bridge's own ready message stays on screen
            if(activeAction==L"tools"&&p->exit==0&&!cancelRequested){toolsMissingHint=false;retryHint=false;state(L"Private tools installed and verified. Choose Prepare next.");}
            if(cancelRequested){failureMessage=committed?L"The safe update finished before closing. Open setup again to verify and play.":hadPublishPermission?L"Setup ended after the safe update boundary. Choose Retry to verify the playable version.":L"Cancelled. Nothing was changed; choose "+std::wstring(activeAction==L"tools"?L"Install":L"Prepare")+L" to continue.";if(hadPublishPermission)retryHint=true;state(failureMessage);}
            else if(p->exit!=0||!failureMessage.empty()){ready=false;if(failureMessage.empty())failureMessage=activeAction==L"status"?L"Your installed game could not be verified. Choose Prepare to rebuild it; saves and settings are kept.":activeAction==L"tools"?L"The tool installation did not finish. Choose Install to try again.":L"Setup did not finish. Open Advanced, Details for the error, then Retry.";
                if(failureNext==L"install")toolsMissingHint=true;else if(failureNext==L"prepare"||(failureNext.empty()&&activeAction==L"prepare"))retryHint=true;state(failureMessage);}
            else if(activeAction==L"check")state(L"Requirements ready. Private tools and this kit check out.");
            controls();if(closeDeferred)DestroyWindow(h);else if(ready&&!pathsReady&&!runningGame&&(!demo||lifecyclePreview))run(L"status");return 0;}
        if(!p->control){if(!buildLine(p->line))log(p->line);return 0;}auto type=field(p->line,L"type"),msg=field(p->line,L"message");log(msg);
        if(type==L"publish_request"){std::lock_guard<std::mutex> lock(handles);const char* reply=cancelRequested?"cancel\n":"commit\n";publishing=!cancelRequested;hadPublishPermission=publishing;DWORD done=0;if(inputWrite&&(!WriteFile(inputWrite,reply,(DWORD)strlen(reply),&done,nullptr)||done!=strlen(reply))){CloseHandle(inputWrite);inputWrite=nullptr;log(L"Commit acknowledgement channel failed; waiting for safe completion/rollback.");}controls();}
        else if(type==L"stage"){auto stepText=field(p->line,L"step");if(!stepText.empty()){stageStep=(int)number(stepText);stageSteps=(int)number(field(p->line,L"steps"));stageTitle=field(p->line,L"stage");if(stageStep!=5){buildTotal=0;marquee(true);}}else{stageTitle=msg;}progressLine();}
        else if(type==L"ready"||type==L"ready_available"){auto exe=field(p->line,L"exe");if(demo&&!lifecyclePreview){pendingReady=true;}else if(!exe.empty()&&(lifecyclePreview||samePath(fs::path(exe),fs::path(activeData)/L"play/hwder.exe"))){installedExe=exe;pathsReady=field(p->line,L"paths_ready")==L"yes";pendingReady=true;}committed=type==L"ready";}
        else if(type==L"launched"){if(installedExe.empty())installedExe=fs::path(activeData)/L"play/hwder.exe";if(field(p->line,L"paths_ready")==L"yes")pathsReady=true;pendingReady=monitorGame(p->line);if(!pendingReady){failureMessage=L"The game may be running, but its identity could not be verified. Close it, then choose Play again.";failureNext=L"close";state(failureMessage);}}else if(type==L"failed"){failureMessage=msg;failureNext=field(p->line,L"next");}
        if(type!=L"log"&&type!=L"stage")state(msg);else if(type==L"stage"&&stageStep==0)state(msg);return 0;}
    if(message==WM_TIMER&&wp==1){if(runningGame&&WaitForSingleObject(runningGame,0)==WAIT_OBJECT_0){forgetGame();if(ready&&certifiedData==text(dataBox)){state(L"Game closed. Checking your installed game for Play again\u2026");run(L"status");}controls();}return 0;}
    if(message==WM_TIMER&&wp==2){progressLine();return 0;}
    if(message==WM_CLOSE){if(busy){if(launching){closeDeferred=true;state(L"Finishing the launch check before closing\u2026");return 0;}if(MessageBoxW(h,publishing?L"Close when the safe update finishes? The new playable version may be committed.":L"Cancel the current setup and close? The playable version has not been updated.",L"Close setup",MB_YESNO|MB_ICONQUESTION)!=IDYES)return 0;cancelRequested=true;closeDeferred=true;std::lock_guard<std::mutex> lock(handles);if(!publishing&&currentJob)TerminateJobObject(currentJob,130);if(publishing)state(L"Finishing the safe update before closing\u2026");return 0;}DestroyWindow(h);return 0;}
    if(message==WM_DESTROY){forgetGame();PostQuitMessage(0);return 0;}return DefWindowProcW(h,message,wp,lp);
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int){
    // Native controls use a 96-DPI layout; Windows scales GDI text/geometry for desktop scaling.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_UNAWARE_GDISCALED);CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);INITCOMMONCONTROLSEX cc{sizeof cc,ICC_PROGRESS_CLASS};InitCommonControlsEx(&cc);
    wchar_t exe[32768];GetModuleFileNameW(nullptr,exe,32768);application=exe;source=application.parent_path()/L"source";if(!fs::is_regular_file(source/L"tools/setup_bridge.py")){source=application.parent_path();for(int n=0;n<4&&!fs::is_regular_file(source/L"tools/setup_bridge.py");++n)source=source.parent_path();}
    if(!fs::is_regular_file(source/L"tools/setup_bridge.py")||!fs::is_regular_file(source/L"tools/managed_toolchain.json")||!fs::is_regular_file(source/L"tools/setup_ui/install_private_python.ps1")){MessageBoxW(nullptr,L"Keep HWDER Setup.exe beside its complete source folder. Extract the whole setup kit first.",L"Setup files missing",MB_OK|MB_ICONERROR);return 1;}
    int argc=0;auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);std::wstring selected;bool autoPlay=false;for(int i=1;i<argc;++i){if(std::wstring(argv[i])==L"--demo")demo=true;else if(std::wstring(argv[i])==L"--demo-lifecycle-race"){demo=true;lifecyclePreview=true;lifecycleRace=true;}else if(std::wstring(argv[i])==L"--demo-lifecycle"){demo=true;lifecyclePreview=true;}else if(std::wstring(argv[i])==L"--shortcut-directory"&&i+1<argc)shortcutDirectory=argv[++i];else if(std::wstring(argv[i])==L"--demo-no-python"){demo=true;noPythonPreview=true;}else if(std::wstring(argv[i])==L"--demo-fail"){demo=true;demoFail=true;}else if(std::wstring(argv[i])==L"--play")autoPlay=true;else if(std::wstring(argv[i])==L"--data"&&i+1<argc)selected=argv[++i];else if(std::wstring(argv[i])==L"--advanced")advanced=true;}LocalFree(argv);if(!lifecyclePreview)shortcutDirectory.clear();
    // Remembered Data folder (default Local AppData\HWDER), then the paths bound to the prepared game, then the last picked paths.
    std::wstring chosen=demo?std::wstring():readJson(prefsPath());
    if(selected.empty()){auto remembered=field(chosen,L"data");std::error_code e;if(!remembered.empty()&&fs::is_directory(fs::path(remembered),e))selected=remembered;}
    if(selected.empty()){PWSTR local=nullptr;SHGetKnownFolderPath(FOLDERID_LocalAppData,KF_FLAG_DEFAULT,nullptr,&local);selected=(fs::path(local)/L"HWDER").wstring();CoTaskMemFree(local);}
    WNDCLASSW cls{};cls.lpfnWndProc=procedure;cls.hInstance=instance;cls.lpszClassName=L"HWDERSetupPrototype";cls.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);cls.hCursor=LoadCursor(nullptr,IDC_ARROW);RegisterClassW(&cls);
    window=CreateWindowW(cls.lpszClassName,demo?L"HWDER Setup \u2014 Preview (no game or installs)":L"HWDER Setup",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,CW_USEDEFAULT,CW_USEDEFAULT,Width,360,nullptr,nullptr,instance,nullptr);SetWindowTextW(dataBox,selected.c_str());
    auto inputs=readJson(fs::path(selected)/L"setup-inputs.json"),prefs=demo?std::wstring():readJson(fs::path(selected)/L"setup-prefs.json");auto nsp=field(inputs,L"nsp"),keys=field(inputs,L"keys");if(nsp.empty())nsp=field(prefs,L"nsp");if(keys.empty())keys=field(prefs,L"keys");SetWindowTextW(gameBox,nsp.c_str());SetWindowTextW(keyBox,keys.c_str());
    layout();refresh();ShowWindow(window,SW_SHOW);UpdateWindow(window);if(autoPlay&&!demo)run(L"launch");else if(lifecyclePreview||(!demo&&fs::is_regular_file(fs::path(selected)/L"last-success.json")))run(L"status");
    MSG message;while(GetMessageW(&message,nullptr,0,0)>0){if(!IsDialogMessageW(window,&message)){TranslateMessage(&message);DispatchMessageW(&message);}}CoUninitialize();return 0;
}
