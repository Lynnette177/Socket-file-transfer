#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <objbase.h>
#include <string>
#include <vector>

namespace {
HWND window, host, port, key, peer, local, output, input, status;
HANDLE child = nullptr, childIn = nullptr, childOut = nullptr;
std::string pendingOutput;
bool previousCarriageReturn=false;
HFONT font;
std::wstring initialHost=L"127.0.0.1", initialPort=L"5005", initialKey, initialPeer, initialLocal=L".";
bool autoConnect=false;
constexpr UINT WM_AUTO_CONNECT = WM_APP + 1;
enum { CONNECT=101, DISCONNECT, KEY_PICK, PEER_PICK, LOCAL_PICK, SEND, LIST, UPLOAD, DOWNLOAD, CD, MKDIR, REMOVE, INPUT };
std::wstring value(HWND h) { int n=GetWindowTextLengthW(h); std::vector<wchar_t> b(static_cast<size_t>(n)+1); GetWindowTextW(h,b.data(),n+1); return b.data(); }
std::wstring quote(const std::wstring& s) {
    std::wstring r=L"\""; size_t slashes=0;
    for(wchar_t c:s) { if(c==L'\\') { ++slashes; continue; } if(c==L'\"') { r.append(slashes*2+1,L'\\'); r+=c; } else { r.append(slashes,L'\\'); r+=c; } slashes=0; }
    r.append(slashes*2,L'\\'); return r+L"\"";
}
std::string utf8(const std::wstring& s) { if(s.empty()) return {}; int n=WideCharToMultiByte(CP_UTF8,0,s.data(),static_cast<int>(s.size()),nullptr,0,nullptr,nullptr); std::string r(static_cast<size_t>(n),'\0'); WideCharToMultiByte(CP_UTF8,0,s.data(),static_cast<int>(s.size()),r.data(),n,nullptr,nullptr); return r; }
void append(const std::wstring& s) { auto end=GetWindowTextLengthW(output); SendMessageW(output,EM_SETSEL,end,end); SendMessageW(output,EM_REPLACESEL,FALSE,reinterpret_cast<LPARAM>(s.c_str())); SendMessageW(output,EM_SCROLLCARET,0,0); }
std::wstring normalizeOutput(const std::wstring& s) {
    std::wstring result;
    for(wchar_t c:s) {
        if(c==L'\r') { result+=L"\r\n"; previousCarriageReturn=true; }
        else if(c==L'\n') { if(!previousCarriageReturn) result+=L"\r\n"; previousCarriageReturn=false; }
        else { result+=c; previousCarriageReturn=false; }
    }
    return result;
}
void finish() { if(childIn) CloseHandle(childIn); if(childOut) CloseHandle(childOut); if(child) CloseHandle(child); childIn=childOut=child=nullptr; pendingOutput.clear(); previousCarriageReturn=false; SetWindowTextW(status,L"未连接"); }
void send(const std::wstring& s) { if(!childIn) { MessageBoxW(window,L"请先连接服务器。",L"提示",MB_OK|MB_ICONINFORMATION); return; } auto bytes=utf8(s+L"\n"); DWORD written=0; if(!WriteFile(childIn,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr) || written!=bytes.size()) { append(L"\r\n连接已断开。\r\n"); finish(); } else append(L"\r\n> "+s+L"\r\n"); }
std::wstring chooseFile() { wchar_t path[32768]{}; OPENFILENAMEW f{}; f.lStructSize=sizeof(f); f.hwndOwner=window; f.lpstrFile=path; f.nMaxFile=32768; f.lpstrFilter=L"所有文件\0*.*\0\0"; f.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR; return GetOpenFileNameW(&f)?path:L""; }
void pick(HWND target) { auto s=chooseFile(); if(!s.empty()) SetWindowTextW(target,s.c_str()); }
void connect() {
    if(child) { MessageBoxW(window,L"请先断开当前连接。",L"提示",MB_OK); return; }
    if(value(key).empty()||value(peer).empty()||value(local).empty()) { MessageBoxW(window,L"请填写私钥、服务端公钥和本地目录。",L"提示",MB_OK); return; }
    wchar_t exe[32768]; DWORD n=GetModuleFileNameW(nullptr,exe,32768); if(!n||n>=32768) return; std::wstring path(exe,n); path=path.substr(0,path.find_last_of(L"\\/" )+1)+L"sft.exe";
    std::wstring cmd=quote(path)+L" client --host "+quote(value(host))+L" --port "+quote(value(port))+L" --key "+quote(value(key))+L" --peer "+quote(value(peer))+L" --local "+quote(value(local));
    SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE}; HANDLE inRead=nullptr,outWrite=nullptr;
    if(!CreatePipe(&inRead,&childIn,&sa,0)||!CreatePipe(&childOut,&outWrite,&sa,0)) { if(inRead) CloseHandle(inRead); if(outWrite) CloseHandle(outWrite); finish(); MessageBoxW(window,L"无法创建通信管道。",L"错误",MB_OK|MB_ICONERROR); return; }
    SetHandleInformation(childIn,HANDLE_FLAG_INHERIT,0); SetHandleInformation(childOut,HANDLE_FLAG_INHERIT,0);
    STARTUPINFOW si{}; si.cb=sizeof(si); si.dwFlags=STARTF_USESTDHANDLES; si.hStdInput=inRead; si.hStdOutput=outWrite; si.hStdError=outWrite; PROCESS_INFORMATION pi{};
    std::vector<wchar_t> writable(cmd.begin(),cmd.end()); writable.push_back(0);
    BOOL ok=CreateProcessW(path.c_str(),writable.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi); CloseHandle(inRead); CloseHandle(outWrite);
    if(!ok) { finish(); MessageBoxW(window,L"无法启动 sft.exe。请将它与图形客户端放在同一目录。",L"错误",MB_OK|MB_ICONERROR); return; }
    CloseHandle(pi.hThread); child=pi.hProcess; SetWindowTextW(status,L"正在连接（查看下方日志）"); append(L"正在启动客户端...\r\n");
}
HWND control(const wchar_t* cls,const wchar_t* title,DWORD style,int x,int y,int w,int h,int id=0) { HWND c=CreateWindowExW(cls==std::wstring(L"EDIT")?WS_EX_CLIENTEDGE:0,cls,title,WS_CHILD|WS_VISIBLE|style,x,y,w,h,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr); SendMessageW(c,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE); return c; }
LRESULT CALLBACK proc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg) {
    case WM_CREATE: {
        window=h; font=CreateFontW(-16,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
        control(L"STATIC",L"安全文件传输",0,20,15,220,28); status=control(L"STATIC",L"未连接",0,650,20,200,24);
        control(L"STATIC",L"服务器",0,20,54,70,24); host=control(L"EDIT",initialHost.c_str(),ES_AUTOHSCROLL,90,50,180,26);
        control(L"STATIC",L"端口",0,285,54,42,24); port=control(L"EDIT",initialPort.c_str(),ES_AUTOHSCROLL,330,50,80,26);
        control(L"BUTTON",L"连接",BS_PUSHBUTTON,440,49,100,29,CONNECT); control(L"BUTTON",L"断开",BS_PUSHBUTTON,550,49,100,29,DISCONNECT);
        control(L"STATIC",L"客户端私钥",0,20,94,90,24); key=control(L"EDIT",initialKey.c_str(),ES_AUTOHSCROLL,115,90,530,26); control(L"BUTTON",L"浏览",0,660,89,90,28,KEY_PICK);
        control(L"STATIC",L"服务端公钥",0,20,134,90,24); peer=control(L"EDIT",initialPeer.c_str(),ES_AUTOHSCROLL,115,130,530,26); control(L"BUTTON",L"浏览",0,660,129,90,28,PEER_PICK);
        control(L"STATIC",L"本地目录",0,20,174,90,24); local=control(L"EDIT",initialLocal.c_str(),ES_AUTOHSCROLL,115,170,530,26); control(L"BUTTON",L"选择",0,660,169,90,28,LOCAL_PICK);
        control(L"BUTTON",L"刷新列表",0,20,215,95,30,LIST); control(L"BUTTON",L"上传文件",0,125,215,95,30,UPLOAD); control(L"BUTTON",L"下载",0,230,215,80,30,DOWNLOAD); control(L"BUTTON",L"进入目录",0,320,215,95,30,CD); control(L"BUTTON",L"新建目录",0,425,215,95,30,MKDIR); control(L"BUTTON",L"删除文件",0,530,215,95,30,REMOVE);
        control(L"STATIC",L"远程路径 / 命令",0,20,260,140,24); input=control(L"EDIT",L"",ES_AUTOHSCROLL,160,256,485,28,INPUT); control(L"BUTTON",L"执行命令",0,660,255,90,30,SEND);
        output=control(L"EDIT",L"填写连接信息后点击连接。文件操作按钮使用上方远程路径；也可直接输入客户端命令。\r\n",ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|WS_VSCROLL,20,300,730,300);
        SendMessageW(output, EM_SETLIMITTEXT, 1024 * 1024, 0);
        SetTimer(h,1,100,nullptr); if(autoConnect) PostMessageW(h,WM_AUTO_CONNECT,0,0); return 0; }
    case WM_AUTO_CONNECT: connect(); return 0;
    case WM_COMMAND: if(HIWORD(wp)==BN_CLICKED) { switch(LOWORD(wp)) {
        case CONNECT: connect(); break; case DISCONNECT: send(L"exit"); break;
        case KEY_PICK: pick(key); break; case PEER_PICK: pick(peer); break;
        case LOCAL_PICK: { BROWSEINFOW bi{}; bi.hwndOwner=h; bi.lpszTitle=L"选择本地文件目录"; bi.ulFlags=BIF_RETURNONLYFSDIRS|BIF_NEWDIALOGSTYLE; auto p=SHBrowseForFolderW(&bi); if(p) { wchar_t dir[MAX_PATH]; if(SHGetPathFromIDListW(p,dir)) SetWindowTextW(local,dir); CoTaskMemFree(p); } break; }
        case LIST: send(L"ls"); break; case UPLOAD: { auto f=chooseFile(); if(!f.empty()) send(L"upload "+quote(f)); break; }
        case DOWNLOAD: if(!value(input).empty()) send(L"get "+quote(value(input))); break;
        case CD: if(!value(input).empty()) send(L"cd "+quote(value(input))); break;
        case MKDIR: if(!value(input).empty()) send(L"mkdir "+quote(value(input))); break;
        case REMOVE: if(!value(input).empty() && MessageBoxW(h,(L"删除远程文件："+value(input)+L"？").c_str(),L"确认删除",MB_YESNO|MB_ICONQUESTION)==IDYES) send(L"rm "+quote(value(input))); break;
        case SEND: if(!value(input).empty()) send(value(input)); break;
    }} return 0;
    case WM_TIMER: if(childOut) { DWORD available=0; if(PeekNamedPipe(childOut,nullptr,0,nullptr,&available,nullptr)&&available) { std::vector<char> b(available); DWORD got=0; if(ReadFile(childOut,b.data(),available,&got,nullptr)&&got) { pendingOutput.append(b.data(),got); size_t length=pendingOutput.size(); int n=0; while(length && !(n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,pendingOutput.data(),static_cast<int>(length),nullptr,0)) && pendingOutput.size()-length<3) --length; if(n) { std::wstring s(static_cast<size_t>(n),L'\0'); MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,pendingOutput.data(),static_cast<int>(length),s.data(),n); pendingOutput.erase(0,length); append(normalizeOutput(s)); if(s.find(L"Fresh handshake complete")!=std::wstring::npos) SetWindowTextW(status,L"已连接"); } else if(pendingOutput.size()>4) { pendingOutput.clear(); append(L"\r\n[无法解码客户端输出]\r\n"); } } } if(WaitForSingleObject(child,0)==WAIT_OBJECT_0 && available==0) { append(L"\r\n客户端已退出。\r\n"); finish(); } } return 0;
    case WM_CLOSE: if(child) TerminateProcess(child,1); DestroyWindow(h); return 0;
    case WM_DESTROY: KillTimer(h,1); finish(); DeleteObject(font); PostQuitMessage(0); return 0;
    } return DefWindowProcW(h,msg,wp,lp);
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE, PWSTR,int show) {
    int argc=0; auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if(!argv) return 1;
    for(int i=1;i<argc;++i) {
        std::wstring arg=argv[i];
        if(arg==L"--connect") { autoConnect=true; continue; }
        if(i+1>=argc || (arg!=L"--host" && arg!=L"--port" && arg!=L"--key" && arg!=L"--peer" && arg!=L"--local")) {
            LocalFree(argv); MessageBoxW(nullptr,L"Invalid GUI command-line option.",L"Secure File Transfer",MB_OK|MB_ICONERROR); return 1;
        }
        std::wstring v=argv[++i];
        if(arg==L"--host") initialHost=v; else if(arg==L"--port") initialPort=v; else if(arg==L"--key") initialKey=v; else if(arg==L"--peer") initialPeer=v; else initialLocal=v;
    }
    LocalFree(argv);
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED); WNDCLASSW wc{}; wc.lpfnWndProc=proc; wc.hInstance=instance; wc.lpszClassName=L"SFTGui"; wc.hCursor=LoadCursor(nullptr,IDC_ARROW); wc.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1); RegisterClassW(&wc); HWND h=CreateWindowW(wc.lpszClassName,L"Secure File Transfer",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,CW_USEDEFAULT,CW_USEDEFAULT,790,660,nullptr,nullptr,instance,nullptr); if(!h) return 1; ShowWindow(h,show); MSG m; while(GetMessageW(&m,nullptr,0,0)>0) { TranslateMessage(&m); DispatchMessageW(&m); } CoUninitialize(); return static_cast<int>(m.wParam);
}
