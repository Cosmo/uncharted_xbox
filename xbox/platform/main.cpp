#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.AccessCache.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.Gaming.Input.h>
#include <winrt/Windows.UI.Popups.h>
#include <windows.h>
#include <unknwn.h>
#include <gamingdeviceinformation.h>
#include <fileapifromapp.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <io.h>
#include <fstream>
#include <map>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <atomic>
#include <vector>
#include <utility>
#include "prepare_game.h"

extern "C" int uncharted_probe_main(int argc,char **argv);
extern "C" FILE *uncharted_uwp_fopen(const char *path,const char *mode);
extern "C" HANDLE uncharted_uwp_find(const char *pattern,WIN32_FIND_DATAA *out);
extern "C" BOOL uncharted_uwp_find_next(HANDLE handle,WIN32_FIND_DATAA *out);
static std::atomic<bool> finished{false};
static std::atomic<bool> game_mode{false};
static uint64_t memory_limit_override=0;
extern "C" void runtime_memory_start_paging();
extern "C" void runtime_wait_report(void);
extern "C" void runtime_sample_threads(void);
#ifdef UNCHARTED_LINK_RENDERER
extern "C" void uncharted_gpu_state(char *out,size_t size);
extern "C" void uncharted_gpu_history(char *out,size_t size);
#endif
extern "C" void uncharted_uwp_pager_thread_init() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
}
extern "C" uint64_t uncharted_uwp_app_memory_usage() {
    return winrt::Windows::System::MemoryManager::AppMemoryUsage();
}
extern "C" uint64_t uncharted_uwp_app_memory_limit() {
    auto actual=winrt::Windows::System::MemoryManager::AppMemoryUsageLimit();
    return memory_limit_override ? std::min(memory_limit_override,actual) : actual;
}
static winrt::agile_ref<winrt::Windows::UI::Core::CoreWindow> core_window;
static std::atomic<bool> window_closed{false};
#ifdef UNCHARTED_BUILD_GRAPHICS
#include "../graphics/present_bridge.h"
#include "input.h"
extern "C" IUnknown* uncharted_uwp_core_window() {
    auto window=core_window.get();
    void* result=nullptr;
    winrt::copy_to_abi(window,result);
    return reinterpret_cast<IUnknown*>(result);
}
#endif

// The normal program exit would run static destructors and DLL teardown while the game's threads
// still run, and during gameplay that hung for minutes with the last frame on screen.
[[noreturn]] static void exit_now(const char* reason) {
    if(!finished.load()) std::printf("UWP lifecycle: exiting (%s)\n",reason);
    std::fflush(nullptr);
    TerminateProcess(GetCurrentProcess(),0);
    for(;;) Sleep(INFINITE);
}
// Closes the app the way closing its window does; ends the process if the window is still open after 3 s.
[[noreturn]] static void close_app(const char* reason) {
    core_window.get().Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::High,[] {
        winrt::Windows::ApplicationModel::Core::CoreApplication::Exit();
    });
    for(int wait=0;wait<12&&!window_closed;++wait) Sleep(250);
    exit_now(window_closed ? "window closed" : reason);
}

// A system dialog for the player, which the controller operates: the index of the chosen button, -1 when it
// could not be shown. The last button is the one B (Escape) chooses.
static int ask(std::string const& text,std::vector<std::string> const& buttons) {
    std::printf("Player dialog: %s\n",text.c_str());
    auto choice=std::make_shared<std::promise<int>>();
    auto answer=choice->get_future();
    core_window.get().Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal,[text,buttons,choice] {
        try {
            winrt::Windows::UI::Popups::MessageDialog dialog(winrt::to_hstring(text),L"Uncharted");
            for(size_t i=0;i<buttons.size();++i)
                dialog.Commands().Append(winrt::Windows::UI::Popups::UICommand(winrt::to_hstring(buttons[i]),nullptr,winrt::box_value(int(i))));
            dialog.DefaultCommandIndex(0);
            dialog.CancelCommandIndex(uint32_t(buttons.size()-1));
            dialog.ShowAsync().Completed([choice](auto const& operation,winrt::Windows::Foundation::AsyncStatus status) {
                int index=-1;
                try {
                    if(status==winrt::Windows::Foundation::AsyncStatus::Completed)
                        if(auto command=operation.GetResults()) index=winrt::unbox_value<int>(command.Id());
                } catch(winrt::hresult_error const&) {}
                choice->set_value(index);
            });
        } catch(winrt::hresult_error const& error) {
            std::printf("Player dialog failed: 0x%08x\n",static_cast<unsigned>(error.code()));
            choice->set_value(-1);
        }
    });
    int index=answer.get();
    std::printf("Player choice: %d\n",index);
    return index;
}
// Players see the message and the app closes; developers (run.txt) find it in loader.log.
static void stop(std::string const& message,bool player) {
    std::printf("STOP: %s\n",message.c_str());
    if(!player) return;
    ask(message,{"OK"});
    close_app("closed after a message");
}

// A dump folder is usable when the game's file functions (the FromApp ones) can open its eboot.bin.
static bool dump_readable(std::string const& dump) {
    FILE *probe=uncharted_uwp_fopen((dump+"\\eboot.bin").c_str(),"rb");
    if(probe) std::fclose(probe);
    return probe!=nullptr;
}

// Where a game folder is, in the player's words.
static std::string describe_place(std::string const& dump) {
    return _strnicmp(dump.c_str(),"D:\\DevelopmentFiles",19)==0 ? "internal storage" : "USB drive";
}

// The chosen game folder (from the launcher's list or the picker) is kept in LocalState\game-folder.txt.
static std::wstring game_folder_file() {
    return std::wstring(winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path())+L"\\game-folder.txt";
}
static void remember_dump(std::string const& dump) {
    std::ofstream file(game_folder_file(),std::ios::trunc);
    file<<dump<<"\n";
}

// The folder the player chose, kept across starts: game-folder.txt, else a folder of the picker in the app's access list
// (which follows renames); empty when there is none or it cannot be read any more.
static std::string chosen_dump() {
    {
        std::ifstream file(game_folder_file());
        std::string dump;
        if(std::getline(file,dump)&&!dump.empty()) {
            if(dump.back()=='\r') dump.pop_back();
            const bool readable=dump_readable(dump);
            std::printf("Chosen game folder %s: %s\n",dump.c_str(),readable ? "found" : "not readable");
            if(readable) return dump;
        }
    }
    using namespace winrt::Windows::Storage::AccessCache;
    try {
        auto list=StorageApplicationPermissions::FutureAccessList();
        if(!list.ContainsItem(L"dump")) return {};
        auto dump=winrt::to_string(list.GetFolderAsync(L"dump").get().Path());
        const bool readable=!dump.empty()&&dump_readable(dump);
        std::printf("Chosen game folder %s: %s\n",dump.c_str(),readable ? "found" : "not readable");
        return readable ? dump : std::string{};
    } catch(winrt::hresult_error const& error) {
        std::printf("Chosen game folder: 0x%08x\n",static_cast<unsigned>(error.code()));
        return {};
    }
}

// Opens the system's folder picker on the window's thread: the chosen folder, or null.
static winrt::Windows::Storage::StorageFolder pick_folder() {
    using winrt::Windows::Storage::StorageFolder;
    auto choice=std::make_shared<std::promise<StorageFolder>>();
    auto answer=choice->get_future();
    core_window.get().Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal,[choice] {
        try {
            winrt::Windows::Storage::Pickers::FolderPicker picker;
            picker.SuggestedStartLocation(winrt::Windows::Storage::Pickers::PickerLocationId::ComputerFolder);
            picker.FileTypeFilter().Append(L"*");
            picker.PickSingleFolderAsync().Completed([choice](auto const& operation,winrt::Windows::Foundation::AsyncStatus status) {
                StorageFolder folder{nullptr};
                try { if(status==winrt::Windows::Foundation::AsyncStatus::Completed) folder=operation.GetResults(); }
                catch(winrt::hresult_error const&) {}
                choice->set_value(folder);
            });
        } catch(winrt::hresult_error const& error) {
            std::printf("Folder picker failed: 0x%08x\n",static_cast<unsigned>(error.code()));
            choice->set_value(StorageFolder{nullptr});
        }
    });
    return answer.get();
}

// A folder from the picker: its path when the game can read it (then it is remembered for later starts), else empty
// with the reason for the player in `problem`. The picker's own access is checked too, so the log tells a folder
// without the game from one the game's file functions cannot read.
static std::string use_picked(winrt::Windows::Storage::StorageFolder const& folder,std::string& problem) {
    auto dump=winrt::to_string(folder.Path());
    bool listed=false;
    unsigned listed_error=0;
    try { folder.GetFileAsync(L"eboot.bin").get(); listed=true; }
    catch(winrt::hresult_error const& error) { listed_error=static_cast<unsigned>(error.code()); }
    const bool readable=!dump.empty()&&dump_readable(dump);
    const DWORD read_error=readable ? 0 : GetLastError();
    std::printf("Folder picker: '%s'; eboot.bin through the picker's access: %s (0x%08x), through the game's file "
                "functions: %s (error %lu)\n",dump.c_str(),listed ? "found" : "not found",listed_error,
                readable ? "readable" : "not readable",read_error);
    if(readable) {
        winrt::Windows::Storage::AccessCache::StorageApplicationPermissions::FutureAccessList().AddOrReplace(L"dump",folder);
        remember_dump(dump);
        return dump;
    }
    problem=listed ? "The game cannot read its files from "+dump+".\n\nCopy the dump to a USB drive and choose it there."
                   : "There is no eboot.bin in "+(dump.empty() ? std::string("this folder") : dump)+".\n\nChoose the folder of "
                     "the dump: the one with eboot.bin, sce_module, sce_sys, u1data, u2data and u3data.";
    return {};
}

// Asks for the dump folder until the game can read one; empty when the player closes the app instead.
static std::string choose_dump(std::string text) {
    for(;;) {
        if(ask(text,{"Choose folder","Close"})!=0) return {};
        auto folder=pick_folder();
        if(!folder) { std::puts("Folder picker: nothing chosen"); continue; }
        if(auto dump=use_picked(folder,text);!dump.empty()) return dump;
    }
}

// A folder chosen in the launcher (vk_presenter.cpp) is remembered. The running game keeps the folder it started with,
// so a different one closes the app after asking the player to start it again; the same one returns what the menu shows.
static std::string switch_game_folder(std::string const& dump) {
    remember_dump(dump);
    const char* current=std::getenv("BB_PRECOMPILE_DUMP");
    if(current&&_stricmp(dump.c_str(),current)==0) return "This is already the game folder.";
    std::printf("Game folder chosen: %s\n",dump.c_str());
    ask("Game folder: "+dump+" ("+describe_place(dump)+")\n\nStart the app again to play from this folder.",{"OK"});
    close_app("game folder changed");
}

// The launcher's "Browse USB drive...": the picker; `message` gets what the menu shows (empty when it was cancelled).
extern "C" void uncharted_uwp_choose_game_folder(char *message,size_t size) {
    auto folder=pick_folder();
    std::string text;
    if(!folder) std::puts("Folder picker: nothing chosen");
    else if(auto dump=use_picked(folder,text);!dump.empty()) text=switch_game_folder(dump);
    std::snprintf(message,size,"%s",text.c_str());
}

// The launcher's choice of a folder from its list.
extern "C" void uncharted_uwp_use_game_folder(const char *dump,char *message,size_t size) {
    std::snprintf(message,size,"%s",switch_game_folder(dump).c_str());
}

// The launcher's line about the folder the game runs from.
extern "C" void uncharted_uwp_game_folder_line(char *out,size_t size) {
    const char* current=std::getenv("BB_PRECOMPILE_DUMP");
    if(!current||!*current) { std::snprintf(out,size,"%s",""); return; }
    std::snprintf(out,size,"Game folder: %s (%s)",current,describe_place(current).c_str());
}

static bool is_game_dump(std::string const& folder);
// The dump on this console's first USB drive, tried before anything else.
static constexpr const char* known_dump="E:\\dumps\\CUSA02343-app0";
// The collection's dumps on USB drives: the known one, then every folder in \dumps on each drive.
static std::vector<std::string> usb_dumps() {
    std::vector<std::string> roots;
    try {
        for(auto const& drive:winrt::Windows::Storage::KnownFolders::RemovableDevices().GetFoldersAsync().get()) {
            auto root=winrt::to_string(drive.Path());
            if(!root.empty()) roots.push_back(root);
        }
    } catch(winrt::hresult_error const& error) {
        std::printf("USB drive list failed: 0x%08x\n",static_cast<unsigned>(error.code()));
    }
    // The console names its first USB drive E:; it is tried when the list does not include it.
    if(std::none_of(roots.begin(),roots.end(),[](std::string const& root) { return _strnicmp(root.c_str(),"E:",2)==0; }))
        roots.push_back("E:\\");
    std::vector<std::string> found;
    const auto add=[&](std::string const& dump) {
        if(std::none_of(found.begin(),found.end(),[&](std::string const& known) { return _stricmp(known.c_str(),dump.c_str())==0; }))
            found.push_back(dump);
    };
    const bool known=is_game_dump(known_dump);
    std::printf("Game dump %s: %s\n",known_dump,known ? "found" : "not found");
    if(known) add(known_dump);
    for(auto root:roots) {
        if(root.back()!='\\') root+='\\';
        WIN32_FIND_DATAA entry{};
        HANDLE search=uncharted_uwp_find((root+"dumps\\*").c_str(),&entry);
        if(search==INVALID_HANDLE_VALUE) continue;
        do {
            const std::string name=entry.cFileName;
            if(!(entry.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)||name=="."||name=="..") continue;
            const std::string dump=root+"dumps\\"+name;
            if(is_game_dump(dump)) { std::printf("Game dump %s: found\n",dump.c_str()); add(dump); }
        } while(uncharted_uwp_find_next(search,&entry));
        FindClose(search);
    }
    return found;
}

// The player's dump on a USB drive.
static std::string find_dump() {
    auto found=usb_dumps();
    return found.empty() ? std::string{} : found.front();
}

static bool read_file(std::string const& path,UnchartedPrepare::Bytes& data);

// A dump of this game: an eboot.bin and a param.sfo that names Uncharted: The Nathan Drake Collection (other PS4 dumps may sit
// next to it).
static bool is_game_dump(std::string const& folder) {
    UnchartedPrepare::Bytes sfo;
    return dump_readable(folder)&&read_file(folder+"\\sce_sys\\param.sfo",sfo)&&UnchartedPrepare::CheckTitle(sfo).empty();
}

// The internal storage a Dev Mode app may read without the picker: the dumps of this game under `root`
// (D:\DevelopmentFiles on the Xbox), at most three folders deep, nearest first, at most `limit` of them.
static std::vector<std::string> find_internal_dumps(std::string const& root="D:\\DevelopmentFiles",size_t limit=8) {
    const auto started=std::chrono::steady_clock::now();
    std::vector<std::pair<std::string,int>> folders={{root,0}};
    std::vector<std::string> found;
    size_t index=0;
    for(;index<folders.size()&&index<500&&found.size()<limit;++index) {
        auto [folder,depth]=folders[index];
        if(is_game_dump(folder)) { found.push_back(folder); continue; }
        if(depth==3) continue;
        WIN32_FIND_DATAA entry{};
        HANDLE search=uncharted_uwp_find((folder+"\\*").c_str(),&entry);
        if(search==INVALID_HANDLE_VALUE) continue;
        do {
            const std::string name=entry.cFileName;
            // WindowsApps holds the installed Dev Mode apps.
            if(!(entry.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)||name=="."||name==".."||name=="WindowsApps") continue;
            folders.emplace_back(folder+(folder.back()=='\\' ? "" : "\\")+name,depth+1);
        } while(uncharted_uwp_find_next(search,&entry));
        FindClose(search);
    }
    std::string names;
    for(auto const& dump:found) names+=(names.empty() ? "" : ", ")+dump;
    std::printf("Game search in %s: %s (%zu folders, %lld ms)\n",root.c_str(),found.empty() ? "not found" : names.c_str(),
        std::min(index,folders.size()),static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now()-started).count()));
    return found;
}
static std::string find_internal_dump() {
    auto found=find_internal_dumps();
    return found.empty() ? std::string{} : found.front();
}

// The launcher's list for "Choose game folder" (vk_presenter.cpp): every dump of this game the app finds by itself (the
// current and chosen folders, D:\DevelopmentFiles, \dumps on USB drives), one "path<TAB>place" per line.
extern "C" void uncharted_uwp_find_game_folders(char *out,size_t size) {
    std::vector<std::string> found;
    const auto add=[&](std::string const& dump) {
        if(dump.empty()||std::any_of(found.begin(),found.end(),[&](std::string const& known) {
            return _stricmp(known.c_str(),dump.c_str())==0; })) return;
        found.push_back(dump);
    };
    if(const char* current=std::getenv("BB_PRECOMPILE_DUMP")) add(current);
    add(chosen_dump());
    for(auto const& dump:find_internal_dumps()) add(dump);
    for(auto const& dump:usb_dumps()) add(dump);
    std::string text;
    for(auto const& dump:found) text+=dump+"\t"+describe_place(dump)+"\n";
    std::snprintf(out,size,"%s",text.c_str());
}

static bool read_file(std::string const& path,UnchartedPrepare::Bytes& data) {
    FILE *file=uncharted_uwp_fopen(path.c_str(),"rb");
    if(!file) return false;
    data.clear();
    std::vector<uint8_t> chunk(1<<20);
    for(size_t count;(count=std::fread(chunk.data(),1,chunk.size(),file))>0;) data.insert(data.end(),chunk.begin(),chunk.begin()+count);
    const bool ok=!std::ferror(file);
    std::fclose(file);
    return ok;
}
static bool write_file(std::string const& path,UnchartedPrepare::Bytes const& data) {
    FILE *file=uncharted_uwp_fopen(path.c_str(),"wb");
    if(!file) return false;
    const bool ok=std::fwrite(data.data(),1,data.size(),file)==data.size();
    return std::fclose(file)==0&&ok;
}

// The game files the launcher runs are made from the player's own dump and are never packaged: on the first
// start into LocalState\Prepared\<executable>, and again when the dump or their format changes. Players are
// asked first; developers (run.txt) get them without asking, and the prepare mode always makes them anew.
static bool prepare_game(std::string const& dump,std::string const& exe,std::string const& prepared,bool player,bool always) {
    UnchartedPrepare::Dump files;
    const std::pair<std::string,UnchartedPrepare::Bytes*> inputs[]={{exe,&files.exe},
        {"sce_module\\libc.prx",&files.libc},{"sce_module\\libSceFios2.prx",&files.fios},{"sce_sys\\param.sfo",&files.sfo}};
    for(auto const& [name,data]:inputs) {
        if(!read_file(dump+"\\"+name,*data)||data->empty()) {
            stop("The game dump in "+dump+" is incomplete: "+name+" is missing or empty.\n\nCopy the whole dump again.",player);
            return false;
        }
    }
    if(auto problem=UnchartedPrepare::CheckTitle(files.sfo);!problem.empty()) { stop("The game dump in "+dump+" "+problem,player); return false; }
    char source[24];
    std::snprintf(source,sizeof source,"%016llx",static_cast<unsigned long long>(UnchartedPrepare::Fingerprint(files)));
    const std::string marker="uncharted-uwp-prepared " UNCHARTED_PREPARED_FORMAT "\nexe "+exe+"\nsource "+source+"\n";
    const std::string outputs[]={"boot-linked.bin","content.bin"};
    if(!always) {
        UnchartedPrepare::Bytes current;
        bool ready=read_file(prepared+"\\format.txt",current)&&std::string(current.begin(),current.end())==marker;
        for(auto const& name:outputs) {
            FILE *probe=ready ? uncharted_uwp_fopen((prepared+"\\"+name).c_str(),"rb") : nullptr;
            ready=probe!=nullptr;
            if(probe) std::fclose(probe);
        }
        if(ready) { std::printf("Prepared game files: %s\n",prepared.c_str()); return true; }
    }
    if(player&&ask("Game found: "+dump+"\n\nPrepare "+exe+"? This is needed once and takes a few seconds.",{"Prepare","Cancel"})!=0)
        close_app("game preparation declined");
    std::printf("Preparing %s from %s\n",exe.c_str(),dump.c_str());
    const auto started=std::chrono::steady_clock::now();
    UnchartedPrepare::Files result;
    try { result=UnchartedPrepare::Prepare(files); }
    catch(std::exception const& error) { stop("Preparing "+exe+" failed: "+error.what(),player); return false; }
    CreateDirectoryFromAppW(winrt::to_hstring(prepared).c_str(),nullptr);
    DeleteFileFromAppW(winrt::to_hstring(prepared+"\\format.txt").c_str());
    const std::pair<const char*,UnchartedPrepare::Bytes*> written[]={{"boot-linked.bin",&result.boot},{"content.bin",&result.content}};
    for(auto const& [name,data]:written) {
        if(!write_file(prepared+"\\"+name,*data)) { stop(std::string("Writing the prepared game file ")+name+" failed.",player); return false; }
    }
    // The 720p mode's patches: without them the game runs at 1080p.
    DeleteFileFromAppW(winrt::to_hstring(prepared+"\\render720p.bin").c_str());
    if(result.render720p.empty()) std::printf("720p mode unavailable: %s\n",result.render720p_error.c_str());
    else if(!write_file(prepared+"\\render720p.bin",result.render720p)) std::puts("Writing render720p.bin failed: the game runs at 1080p");
    if(!write_file(prepared+"\\format.txt",UnchartedPrepare::Bytes(marker.begin(),marker.end()))) {
        stop("Writing the prepared game files failed.",player); return false;
    }
    std::printf("Prepared game files in %lld ms: %s\n",static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now()-started).count()),prepared.c_str());
    return true;
}

// The game asked for another executable of the collection (sceSystemServiceLoadExec: exit code 101 and
// loadexec.txt, which run.py handles the same way): the app starts again and runs that one (next.txt), with the
// guest's arguments (guest-argv.txt). An app cannot start a second copy of itself, so it restarts.
constexpr int loadexec_exit=101;
static std::string take_next_executable(std::wstring const& local,std::string& guest_argv) {
    std::ifstream file(local+L"\\next.txt");
    std::vector<std::string> lines;
    for(std::string line;std::getline(file,line);) {
        if(!line.empty()&&line.back()=='\r') line.pop_back();
        lines.push_back(line);
    }
    file.close();
    DeleteFileFromAppW((local+L"\\next.txt").c_str());
    if(lines.empty()||lines[0].empty()) return {};
    std::string target=lines[0].substr(lines[0].find_last_of("\\/")+1);
    guest_argv=winrt::to_string(local)+"\\guest-argv.txt";
    std::string text;
    for(auto const& line:lines) text+=line+"\n";
    if(!write_file(guest_argv,UnchartedPrepare::Bytes(text.begin(),text.end()))) guest_argv.clear();
    return target;
}

static void loader() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    auto local=winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path();
    {
        std::ifstream limit_file(std::wstring(local)+L"\\memory_limit_mb.txt");
        uint64_t mb=0;
        if(limit_file>>mb && mb>=2048 && mb<=65536) memory_limit_override=mb<<20;
    }
    auto log=std::wstring(local)+L"\\loader.log";
    _wfreopen(log.c_str(),L"w",stdout);
    auto error_log=std::wstring(local)+L"\\errors.log";
    _wfreopen(error_log.c_str(),L"w",stderr);
    auto driver_log=std::wstring(local)+L"\\dozen.log";
    HANDLE driver_stream=CreateFile2FromAppW(driver_log.c_str(),GENERIC_WRITE,
        FILE_SHARE_READ|FILE_SHARE_WRITE,CREATE_ALWAYS,nullptr);
    if(driver_stream!=INVALID_HANDLE_VALUE) SetStdHandle(STD_ERROR_HANDLE,driver_stream);
    setvbuf(stdout,nullptr,_IONBF,0); setvbuf(stderr,nullptr,_IONBF,0);
    std::printf("Uncharted UWP loader\n");
    if(memory_limit_override) std::printf("UWP memory: debug budget %llu MiB (OS limit unchanged)\n",uncharted_uwp_app_memory_limit()>>20);
    GAMING_DEVICE_MODEL_INFORMATION model{};
    GetGamingDeviceModelInformation(&model);
    std::printf("Device model: 0x%08x; app budget: %llu MiB; controllers: %u\n",model.deviceId,
                static_cast<unsigned long long>(winrt::Windows::System::MemoryManager::AppMemoryUsageLimit()>>20),
                winrt::Windows::Gaming::Input::Gamepad::Gamepads().Size());
    void *code=VirtualAllocFromApp(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    DWORD old{};
    if(!code) { std::printf("STOP: executable smoke allocation failed (%lu)\n",GetLastError()); finished=true; return; }
    const unsigned char instructions[]={0xb8,0x2a,0,0,0,0xc3};
    std::memcpy(code,instructions,sizeof(instructions));
    if(!VirtualProtectFromApp(code,4096,PAGE_EXECUTE_READ,&old)) {
        std::printf("STOP: executable smoke protection failed (%lu)\n",GetLastError()); VirtualFree(code,0,MEM_RELEASE); finished=true; return;
    }
    FlushInstructionCache(GetCurrentProcess(),code,4096);
    using NativeFunction=int (__attribute__((sysv_abi)) *)();
    int smoke=reinterpret_cast<NativeFunction>(code)();
    VirtualFree(code,0,MEM_RELEASE);
    std::printf("Executable System V ABI smoke: %s (%d)\n",smoke==42 ? "PASS" : "FAIL",smoke);
    if(smoke!=42) { finished=true; return; }
    // Players have no run.txt: the game runs from their dump on the USB drive. Developers pick a test mode
    // (line 1) and the dump folder (line 2) in it.
    std::ifstream settings(std::wstring(local)+L"\\run.txt");
    const bool player=!settings.is_open();
    std::string mode=player ? "boot" : "",dump;
    if(!player) { std::getline(settings,mode); std::getline(settings,dump); }
    if(!mode.empty()&&mode.back()=='\r') mode.pop_back();
    if(!dump.empty()&&dump.back()=='\r') dump.pop_back();
    std::ifstream debug_settings(std::wstring(local)+L"\\graphics-debug.txt");
    for(std::string line;std::getline(debug_settings,line);) {
        if(!line.empty()&&line.back()=='\r') line.pop_back();
        auto separator=line.find('=');
        if(separator==std::string::npos) continue;
        auto key=line.substr(0,separator),value=line.substr(separator+1);
        if(key=="DZN_DEBUG"||key=="DZN_MAX_SHADER_MODEL"||key=="DZN_MAX_D3D12_OPTIONS"||
           key=="BB_DUMP_SHADERS"||key=="BB_UWP_CAPTURE"||key=="BB_NO_MSAA"||
           key=="BB_UWP_TEXTURE_DUMP"||key=="BB_UWP_RENDER_RES"||key=="BB_DRAW_PIPE"||
           key=="BB_UWP_TEXTURE_ADDRESSES"||key=="BB_DEFERRED_READBACK"||
           key=="BB_STALE_READBACK"||key=="BB_ASYNC_SHADERS"||key=="BB_PIPELINE_CACHE"||
           key=="BB_UWP_METRICS"||key=="BB_VK_RECORD_THREAD"||key=="BB_GC_BUDGET_MB"||key=="BB_UWP_SWAP"||
           key=="BB_PRECOMPILE_AUTORUN"||key=="BB_FRAME_STATS"||key=="BB_IO_STATS"||
           key=="BB_BACKGROUND_WARMUP"||key=="BB_PARALLEL_WARMUP"||key=="BB_WARMUP_THREADS"||
           key=="BB_PIPELINE_BINARIES"||key=="BB_FRAMES_AHEAD"||key=="BB_STREAM_MEMORY"||
           key=="BB_RECORDER_TIMINGS"||key=="BB_BAKED_PIPELINES"||
           key=="BB_PERF_DIAG"||key=="BB_PREP_WORKERS"||key=="BB_COPY_THREADS"||key=="BB_CHOOSE_DUMP"||
           key=="BB_READ_FAULT_EVERY"||key=="BB_RENDER_720P"||key=="BB_DRED"||key=="BB_GPU_CHECKPOINTS"||key=="BB_SKIP_SHADERS"||key=="BB_GPU_CHECKPOINTS_FROM"||key=="BB_DUMP_GCN"||
           key=="BB_FRAME_DUMP_TRIGGER"||key=="BB_DUMP_DIR"||key=="BB_START_MENU") {
            _putenv_s(key.c_str(),value.c_str());
            SetEnvironmentVariableW(winrt::to_hstring(key).c_str(),winrt::to_hstring(value).c_str());
        }
    }
    game_mode=(mode=="boot");
    _putenv_s("BB_UWP_LAUNCHER",mode=="boot" ? "1" : "0");
    // Players: the folder they chose before, else \dumps\Uncharted on a USB drive, else a dump under
    // D:\DevelopmentFiles on the internal storage, else the folder picker.
    // BB_CHOOSE_DUMP=1 (graphics-debug.txt) opens the picker even when a dump is found.
    if(player) {
        const char* force=std::getenv("BB_CHOOSE_DUMP");
        const bool choose=force&&*force=='1';
        if(!choose) dump=chosen_dump();
        if(!choose&&dump.empty()) dump=find_dump();
        if(!choose&&dump.empty()) dump=find_internal_dump();
        if(dump.empty()) dump=choose_dump(choose ? "Choose the game folder." :
            "No game found.\n\nChoose the folder of your Uncharted: The Nathan Drake Collection dump (the folder with eboot.bin) on "
            "a USB drive, or copy the dump to the console's internal storage anywhere under D:\\DevelopmentFiles.");
        if(dump.empty()) close_app("no game folder chosen");
        std::printf("Game folder: %s (%s)\n",dump.c_str(),describe_place(dump).c_str());
    }
    _putenv_s("BB_PRECOMPILE_DUMP",dump.c_str());
    std::string user=winrt::to_string(local)+"\\user";
    CreateDirectoryW((std::wstring(local)+L"\\user").c_str(),nullptr);
    CreateDirectoryW((std::wstring(local)+L"\\Prepared").c_str(),nullptr);
    _putenv_s("BB_GPU_USER_DIR",user.c_str());
    _putenv_s("BB_UWP_SWAP_FILE",(winrt::to_string(local)+"\\uncharted-swap.bin").c_str());
#ifdef UNCHARTED_BUILD_GRAPHICS
    if(mode=="graphics") { int result=UnchartedUwp::GraphicsProbe(); std::printf("Graphics probe returned %d\n",result); finished=true; return; }
#ifdef UNCHARTED_LINK_RENDERER
    if(mode=="texture") { int result=UnchartedUwp::TextureProbe(); std::printf("Texture probe returned %d\n",result); finished=true; return; }
#endif
#endif
    if(mode=="memory") {
        char *args[]={const_cast<char *>("uncharted-uwp"),const_cast<char *>("--memory-selftest")};
        int result=uncharted_probe_main(2,args);
        std::printf("Memory contract self-test returned %d\n",result); finished=true; return;
    }
    // Developer check of what the app may read: every drive root through the game's file functions (folder names only).
    if(mode=="drives") {
        for(char letter='A';letter<='Z';++letter) {
            const std::string root=std::string(1,letter)+":\\";
            WIN32_FIND_DATAA entry{};
            HANDLE search=uncharted_uwp_find((root+"*").c_str(),&entry);
            if(search==INVALID_HANDLE_VALUE) {
                const DWORD error=GetLastError();
                if(error!=ERROR_PATH_NOT_FOUND&&error!=ERROR_INVALID_DRIVE&&error!=ERROR_NOT_READY) std::printf("Drive %s: error %lu\n",root.c_str(),error);
                continue;
            }
            std::string names;
            unsigned count=0;
            unsigned folders=0;
            do {
                if(!(entry.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)) continue;
                if(folders++<10) names+=std::string(folders>1 ? ", " : "")+entry.cFileName;
            } while(++count<200&&uncharted_uwp_find_next(search,&entry));
            FindClose(search);
            std::printf("Drive %s: %u entries, folders: %s\n",root.c_str(),count,names.c_str());
        }
        finished=true; return;
    }
    // Developer check of the internal search: the folder on line 2 of run.txt, else D:\DevelopmentFiles.
    if(mode=="search") { find_internal_dumps(dump.empty() ? "D:\\DevelopmentFiles" : dump); finished=true; return; }
    if(mode!="boot"&&mode!="imports"&&mode!="storage"&&mode!="prepare") {
        std::puts("READY: upload run.txt (boot, imports, storage or prepare on line 1, dump folder on line 2), then relaunch."); finished=true; return;
    }
    if(dump.empty()) { std::puts("STOP: dump folder missing from run.txt"); finished=true; return; }
    if(mode=="storage") {
        std::string paths[]={dump.substr(0,3),dump.substr(0,dump.find_last_of("\\/")),dump};
        for(auto const& path:paths) {
            WIN32_FIND_DATAA entry{};
            HANDLE search=uncharted_uwp_find((path+"\\*").c_str(),&entry);
            if(search==INVALID_HANDLE_VALUE) { std::printf("List %s: error %lu\n",path.c_str(),GetLastError()); continue; }
            std::printf("List %s:\n",path.c_str());
            unsigned count=0;
            do { std::printf("  %s%s\n",entry.cFileName,entry.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY ? "/" : ""); }
            while(++count<24&&uncharted_uwp_find_next(search,&entry));
            FindClose(search);
        }
    }
    std::string dump_eboot=dump+"\\eboot.bin";
    try {
        auto file=winrt::Windows::Storage::StorageFile::GetFileFromPathAsync(winrt::to_hstring(dump_eboot)).get();
        std::printf("Game file broker lookup: PASS (%s)\n",winrt::to_string(file.Name()).c_str());
    } catch(winrt::hresult_error const& error) {
        std::printf("Game file broker lookup: 0x%08x\n",static_cast<unsigned>(error.code()));
    }
    FILE *dump_file=uncharted_uwp_fopen(dump_eboot.c_str(),"rb");
    DWORD dump_error=dump_file ? 0 : GetLastError();
    std::printf("External dump eboot.bin readable: %s\n",dump_file ? "yes" : "no (only packaged-code initialization can be tested)");
    if(!dump_file) std::printf("External dump open error: %lu\n",dump_error);
    if(dump_file) std::fclose(dump_file);
    if(mode=="storage") {
        auto name=winrt::to_hstring(dump_eboot);
        HANDLE strict=CreateFile2FromAppW(name.c_str(),GENERIC_READ,FILE_SHARE_READ,OPEN_EXISTING,nullptr);
        DWORD strict_error=strict==INVALID_HANDLE_VALUE ? GetLastError() : 0;
        std::printf("Native read-only open, default parameters: %s (error %lu)\n",strict==INVALID_HANDLE_VALUE ? "failed" : "PASS",strict_error);
        if(strict!=INVALID_HANDLE_VALUE) CloseHandle(strict);
        try {
            auto file=winrt::Windows::Storage::StorageFile::GetFileFromPathAsync(name).get();
            std::printf("WinRT file lookup: PASS (%s)\n",winrt::to_string(file.Name()).c_str());
        } catch(winrt::hresult_error const& error) {
            std::printf("WinRT file lookup: 0x%08x %s\n",static_cast<unsigned>(error.code()),winrt::to_string(error.message()).c_str());
        }
        finished=true; return;
    }
    // The collection menu and Uncharted 1 (eboot.bin), unless the game asked for another executable before.
    std::string guest_argv;
    std::string exe=take_next_executable(std::wstring(local),guest_argv);
    if(exe.empty()) exe="eboot.bin";
    const std::string stem=exe.substr(0,exe.find_first_of(".-"));
    std::string prepared=winrt::to_string(local)+"\\Prepared\\"+stem;
    if(!prepare_game(dump,exe,prepared,player,mode=="prepare")||mode=="prepare") { finished=true; return; }
    std::string boot=prepared+"\\boot-linked.bin";
    std::string content=prepared+"\\content.bin";
    // Shader preparation (start menu): the game whose data folder is scanned and whose library is used.
    _putenv_s("BB_PRECOMPILE_GAME",stem=="big2" ? "u2" : stem=="big3" ? "u3" : "u1");
    _putenv_s("BB_CONFIG",(user+"\\graphics.cfg").c_str());
    _putenv_s("BB_PAD_FILE",(winrt::to_string(local)+"\\controls.txt").c_str());
    _putenv_s("BB_VMA_DUMP",(winrt::to_string(local)+"\\vma.json").c_str());  // overlay.cpp: Vulkan allocations
    _putenv_s("BB_CAPTURE_TRIGGER",(winrt::to_string(local)+"\\capture.txt").c_str());
    _putenv_s("BB_TOGGLE_FILE",(winrt::to_string(local)+"\\toggles.txt").c_str());
    _putenv_s("BB_UPSCALER","off");
    _putenv_s("BB_READBACK_LINEAR","1");
    if(!std::getenv("BB_NO_MSAA")) _putenv_s("BB_NO_MSAA","1");
    if(!std::getenv("BB_DRAW_PIPE")) _putenv_s("BB_DRAW_PIPE","1");
    if(!std::getenv("BB_VK_RECORD_THREAD")) _putenv_s("BB_VK_RECORD_THREAD","1");
    // Guest memory copies on two threads: the Vulkan recording thread is the busiest one with Dozen.
    if(!std::getenv("BB_COPY_THREADS")) _putenv_s("BB_COPY_THREADS","2");
    if(!std::getenv("BB_PREP_WORKERS")) _putenv_s("BB_PREP_WORKERS","0");
    // Finish saved-pipeline loading before play; reserve background compilation for new effects.
    // Compiler concurrency and retained IR pools are bounded separately in the UWP renderer.
    const std::pair<const char*,const char*> performance_defaults[]={
        {"BB_PIPELINE_CACHE","1"},{"BB_BACKGROUND_WARMUP","0"},{"BB_PARALLEL_WARMUP","1"},
        {"BB_PIPELINE_BINARIES","1"},{"BB_STREAM_MEMORY","cached"}
    };
    for(auto [key,value]:performance_defaults) if(!std::getenv(key)) _putenv_s(key,value);
    // The in-game menu's settings, as upstream's launcher reads them before a start: the settings the renderer
    // reads as variables are passed on (graphics-debug.txt still wins).
    std::map<std::string,std::string> menu;
    {
        std::ifstream file(winrt::to_hstring(user+"\\graphics.cfg").c_str());
        for(std::string line;std::getline(file,line);) {
            if(!line.empty()&&line.back()=='\r') line.pop_back();
            const auto separator=line.find('=');
            if(separator!=std::string::npos&&line[0]!='#') menu[line.substr(0,separator)]=line.substr(separator+1);
        }
    }
    const auto setting=[&](const char* key,const char* fallback) {
        const auto found=menu.find(key);
        return found==menu.end() ? std::string(fallback) : found->second;
    };
    struct Passed {const char* variable; const char* key; const char* fallback;};
    const Passed passed[]={{"BB_ASYNC_SHADERS","async_shaders","1"},
        {"BB_DEFERRED_READBACK","deferred_readback","1"},{"BB_STALE_READBACK","stale_readback","1"}};
    for(auto const& entry:passed) if(!std::getenv(entry.variable)) _putenv_s(entry.variable,setting(entry.key,entry.fallback).c_str());
    _putenv_s("BB_GLYPHS","xbox");  // the menu names the Xbox controller's buttons
    if(mode=="boot") runtime_memory_start_paging();
    _putenv_s("BB_VBLANK_HZ","60");
    const std::string loadexec=winrt::to_string(local)+"\\loadexec.txt";
    DeleteFileFromAppW(winrt::to_hstring(loadexec).c_str());
    _putenv_s("BB_LOADEXEC_FILE",loadexec.c_str());
    std::printf("Executable: %s (prepared in %s)\n",exe.c_str(),prepared.c_str());
    std::printf("UWP renderer: draw pipe %s, record thread %s, async shaders %s, pipeline cache %s\n",
        std::getenv("BB_DRAW_PIPE"),std::getenv("BB_VK_RECORD_THREAD"),
        std::getenv("BB_ASYNC_SHADERS"),std::getenv("BB_PIPELINE_CACHE"));
    std::printf("UWP performance: background warm-up %s, parallel warm-up %s, pipeline binaries %s, stream memory %s, "
        "readbacks deferred %s stale %s\n",
        std::getenv("BB_BACKGROUND_WARMUP"),std::getenv("BB_PARALLEL_WARMUP"),
        std::getenv("BB_PIPELINE_BINARIES"),std::getenv("BB_STREAM_MEMORY"),
        std::getenv("BB_DEFERRED_READBACK"),std::getenv("BB_STALE_READBACK"));
    std::puts("Running the existing loader and runtime with native UWP audio output.");
    std::vector<std::string> arguments={"uncharted-uwp",boot};
#ifdef UNCHARTED_LINK_RENDERER
    if(mode!="boot") arguments.push_back("--cpu-only");
#else
    arguments.push_back("--cpu-only");
#endif
    arguments.insert(arguments.end(),{"--content-profile",content,"--app0",dump,"--user",user,"--timeout","0"});
    if(!guest_argv.empty()) arguments.insert(arguments.end(),{"--guest-argv",guest_argv});
    // The game renders at 1280x720 (menus and movies too) unless BB_RENDER_720P=0 (graphics-debug.txt).
    const std::string render720p=prepared+"\\render720p.bin";
    const char* render720p_setting=std::getenv("BB_RENDER_720P");
    const bool use720p=!(render720p_setting&&*render720p_setting=='0')&&
        GetFileAttributesW(winrt::to_hstring(render720p).c_str())!=INVALID_FILE_ATTRIBUTES;
    if(use720p) arguments.insert(arguments.end(),{"--patches",render720p});
    std::printf("Render resolution: %s\n",use720p ? "1280x720" : "1920x1080");
    if(mode=="imports") arguments.push_back("--list-missing");
    std::vector<char*> args;
    for(auto& argument:arguments) args.push_back(argument.data());
    int result=uncharted_probe_main(static_cast<int>(args.size()),args.data());
    std::printf("Loader returned %d\n",result);
    if(result==loadexec_exit&&CopyFileFromAppW(winrt::to_hstring(loadexec).c_str(),(std::wstring(local)+L"\\next.txt").c_str(),FALSE)) {
        std::puts("The game asked for another executable: restarting the app");
        std::fflush(nullptr);
        const auto reason=winrt::Windows::ApplicationModel::Core::CoreApplication::RequestRestartAsync(L"").get();
        std::printf("Restart refused: %d\n",static_cast<int>(reason));
    }
    finished=true;
}

struct View : winrt::implements<View,winrt::Windows::ApplicationModel::Core::IFrameworkView> {
    winrt::Windows::UI::Core::CoreWindow window{nullptr};
    void Initialize(winrt::Windows::ApplicationModel::Core::CoreApplicationView const& app) {
        app.Activated([this](auto const&,auto const&) { window.Activate(); });
        winrt::Windows::ApplicationModel::Core::CoreApplication::Suspending([](auto const&,auto const&) {
            std::printf("UWP lifecycle: suspending; memory=%llu MiB, limit=%llu MiB\n",
                winrt::Windows::System::MemoryManager::AppMemoryUsage()>>20,
                winrt::Windows::System::MemoryManager::AppMemoryUsageLimit()>>20);
            std::fflush(stdout);
        });
        winrt::Windows::ApplicationModel::Core::CoreApplication::Resuming([](auto const&,auto const&) {
            std::puts("UWP lifecycle: resuming"); std::fflush(stdout);
        });
    }
    void SetWindow(winrt::Windows::UI::Core::CoreWindow const& value) {
        window=value; window.Closed([](auto const&,auto const&) {
            if(!finished.load()) { std::puts("UWP lifecycle: window closed"); std::fflush(stdout); }
            window_closed=true;
        });
        winrt::Windows::UI::Core::SystemNavigationManager::GetForCurrentView().BackRequested(
            [](auto const&,winrt::Windows::UI::Core::BackRequestedEventArgs const& event) {
                event.Handled(true);
            });
        window.VisibilityChanged([](auto const&,auto const& event) {
            std::printf("UWP lifecycle: visible=%u\n",unsigned(event.Visible())); std::fflush(stdout);
        });
#ifdef UNCHARTED_LINK_RENDERER
        window.KeyDown([](auto const&,auto const& event) {
            if(uncharted_uwp_dialog_key(uint32_t(event.VirtualKey()),1,event.KeyStatus().WasKeyDown)) event.Handled(true);
        });
        window.KeyUp([](auto const&,auto const& event) {
            if(uncharted_uwp_dialog_key(uint32_t(event.VirtualKey()),0,0)) event.Handled(true);
        });
#endif
        core_window=winrt::agile_ref(value);
    }
    void Load(winrt::hstring const&) {}
    void Run() {
        std::thread([] {
            try { loader(); }
            catch(winrt::hresult_error const& error) { std::printf("STOP: WinRT error 0x%08x\n",static_cast<unsigned>(error.code())); finished=true; }
            catch(std::exception const& error) { std::printf("STOP: host error: %s\n",error.what()); finished=true; }
            std::fflush(nullptr);
            std::fclose(stdout); std::fclose(stderr);
        }).detach();
        std::thread([] {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            for(unsigned sample=0;sample<24&&!finished.load();++sample) {
                Sleep(5000);
                std::printf("UWP startup memory: %llu/%llu MiB, level=%u\n",
                    winrt::Windows::System::MemoryManager::AppMemoryUsage()>>20,
                    winrt::Windows::System::MemoryManager::AppMemoryUsageLimit()>>20,
                    unsigned(winrt::Windows::System::MemoryManager::AppMemoryUsageLevel()));
                std::fflush(stdout);
                if(sample==5&&!finished.load()&&!game_mode.load()) {
                    std::fputs("STOP: 30 second milestone watchdog\n",stderr); std::fflush(nullptr); ExitProcess(124);
                }
            }
        }).detach();
        // LocalState\threads.txt asks for the thread report in loader.log: what every game thread waits for (a hang).
        std::thread([request=std::wstring(winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path())+L"\\threads.txt"] {
            WIN32_FILE_ATTRIBUTE_DATA attributes{};
            DeleteFileFromAppW(request.c_str());
            for(;;) {
                if(GetFileAttributesExFromAppW(request.c_str(),GetFileExInfoStandard,&attributes)) {
                    DeleteFileFromAppW(request.c_str());
                    runtime_wait_report();
#ifdef UNCHARTED_LINK_RENDERER
                    char gpu[256]{};
                    uncharted_gpu_state(gpu,sizeof gpu);
                    std::printf("GPU state: %s\n",gpu);
                    char history[1400]{};
                    uncharted_gpu_history(history,sizeof history);
                    std::printf("GPU last packets (opcode@address): %s\n",history);
                    std::fflush(stdout);
#endif
                    runtime_sample_threads();
                }
                Sleep(500);
            }
        }).detach();
        // Scripts request a normal close through LocalState\quit.txt, the same path as closing the window,
        // instead of terminating the process (which leaves the window or the last frame on screen).
        std::thread([quit=std::wstring(winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path())+L"\\quit.txt"] {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            WIN32_FILE_ATTRIBUTE_DATA attributes{};
            DeleteFileFromAppW(quit.c_str());
            while(!GetFileAttributesExFromAppW(quit.c_str(),GetFileExInfoStandard,&attributes)) Sleep(250);
            DeleteFileFromAppW(quit.c_str());
            if(!finished.load()) { std::puts("UWP lifecycle: quit requested"); std::fflush(stdout); }
            close_app("quit requested, window still open");
        }).detach();
        while(!window_closed) window.Dispatcher().ProcessEvents(winrt::Windows::UI::Core::CoreProcessEventsOption::ProcessOneAndAllPending);
        exit_now("window closed");
    }
    void Uninitialize() {}
};
struct ViewSource : winrt::implements<ViewSource,winrt::Windows::ApplicationModel::Core::IFrameworkViewSource> {
    winrt::Windows::ApplicationModel::Core::IFrameworkView CreateView() { return winrt::make<View>(); }
};
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
    winrt::init_apartment();
    winrt::Windows::ApplicationModel::Core::CoreApplication::Run(winrt::make<ViewSource>());
    return 0;
}
