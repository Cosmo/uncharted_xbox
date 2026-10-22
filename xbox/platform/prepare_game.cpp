#include "prepare_game.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>

// Each step follows its Python original, including its checks; the comments name the original.
namespace UnchartedPrepare {
namespace {
using View=std::span<const uint8_t>;
using Wide=__int128;

constexpr uint64_t image_limit=512ull<<20;
// The collection: Europe, America, the European 1.02 build, Asia, Japan.
constexpr const char* known_titles[]={"CUSA02343","CUSA02320","CUSA02344","CUSA02826","CUSA02810"};
// The dynamic tags of the two relocation tables: offset and size.
constexpr std::pair<uint64_t,uint64_t> relocation_tables[]={{0x61000029,0x6100002d},{0x6100002f,0x61000031}};

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error(message); }

std::string hex(uint64_t value) {
    char text[24]; std::snprintf(text,sizeof text,"%#llx",static_cast<unsigned long long>(value)); return text;
}
uint64_t add(uint64_t a,uint64_t b) { if(b>UINT64_MAX-a) fail("out of bounds: "+hex(a)+"+"+hex(b)); return a+b; }
uint64_t as_u64(Wide value) { if(value<0||value>Wide(UINT64_MAX)) fail("value out of range"); return uint64_t(value); }
int64_t as_i64(Wide value) { if(value<Wide(INT64_MIN)||value>Wide(INT64_MAX)) fail("value out of range"); return int64_t(value); }

View span(View data,uint64_t offset,uint64_t size) {
    if(offset>data.size()||size>data.size()-offset)
        fail("out of bounds: "+hex(offset)+"+"+hex(size)+" / "+hex(data.size()));
    return data.subspan(offset,size);
}
template<class T> T read(View data,uint64_t offset) {
    T value; std::memcpy(&value,span(data,offset,sizeof value).data(),sizeof value); return value;
}
template<class T> void put(Bytes& out,T value) {
    auto bytes=reinterpret_cast<const uint8_t*>(&value); out.insert(out.end(),bytes,bytes+sizeof value);
}
void put_text(Bytes& out,const char* text,size_t size) { out.insert(out.end(),text,text+size); }
// The caller has checked that the bytes lie in the image.
void store(Bytes& image,uint64_t offset,uint64_t value) { std::memcpy(image.data()+offset,&value,sizeof value); }

// prepare.py parse_self
struct Segment { uint32_t type,flags; uint64_t offset,vaddr,paddr,filesz,memsz,align; };
struct Self { Bytes elf; uint64_t entry=0; std::vector<Segment> ph; };

Self parse_self(View data) {
    if(data.empty()) fail("empty SELF (0 bytes)");
    View magic=span(data,0,4);
    const bool plain_elf=!std::memcmp(magic.data(),"\x7f" "ELF",4);
    uint64_t count=0,base=0;
    if(!plain_elf) {
        if(std::memcmp(magic.data(),"O\x15=\x1d",4)) fail("expected PS4 SELF or ELF");
        count=read<uint16_t>(data,24);
        base=32+count*32;
    }
    View header=span(data,base,64);
    if(std::memcmp(header.data(),"\x7f" "ELF\x02\x01\x01",7)||read<uint16_t>(header,18)!=62)
        fail("expected little-endian x86-64 ELF");
    const uint16_t phentsize=read<uint16_t>(header,54),phnum=read<uint16_t>(header,56);
    if(phentsize!=56||phnum==0||phnum>=256) fail("unsupported program headers");
    Self self;
    self.entry=read<uint64_t>(header,24);
    const uint64_t phoff=read<uint64_t>(header,32);
    for(uint64_t i=0;i<phnum;++i) {
        View entry=span(data,add(add(base,phoff),i*56),56);
        self.ph.push_back({read<uint32_t>(entry,0),read<uint32_t>(entry,4),read<uint64_t>(entry,8),read<uint64_t>(entry,16),
                           read<uint64_t>(entry,24),read<uint64_t>(entry,32),read<uint64_t>(entry,40),read<uint64_t>(entry,48)});
    }
    const uint64_t header_end=add(phoff,uint64_t(phnum)*56);
    uint64_t end=header_end;
    for(const auto& p:self.ph) end=std::max(end,add(p.offset,p.filesz));
    if(end>image_limit) fail("probe image exceeds 512 MiB limit");
    // An unpacked ELF already holds its segments at their file offsets.
    if(plain_elf) {
        View whole=span(data,0,end);
        self.elf.assign(whole.begin(),whole.end());
        return self;
    }
    View headers=span(data,base,header_end);
    self.elf.assign(end,0);
    std::copy(headers.begin(),headers.end(),self.elf.begin());
    std::vector<std::pair<uint64_t,uint64_t>> covered{{0,header_end}};
    for(uint64_t i=0;i<count;++i) {
        View entry=span(data,32+i*32,32);
        const uint64_t flags=read<uint64_t>(entry,0),offset=read<uint64_t>(entry,8);
        const uint64_t size=read<uint64_t>(entry,16),memory=read<uint64_t>(entry,24);
        if(!(flags&0x800)) continue;
        if(flags&10) fail("encrypted/compressed SELF segment is unsupported");
        const uint64_t index=(flags>>20)&4095;
        if(index>=self.ph.size()) fail("invalid segment index");
        const auto& p=self.ph[index];
        if(size!=p.filesz||size!=memory) fail("unsupported blocked segment layout");
        View bytes=span(data,offset,size);
        std::copy(bytes.begin(),bytes.end(),self.elf.begin()+p.offset);
        covered.emplace_back(p.offset,p.offset+size);
    }
    for(size_t i=0;i<self.ph.size();++i) {
        const auto& p=self.ph[i];
        if(!p.filesz||std::any_of(covered.begin(),covered.end(),[&](const auto& c) {
               return c.first<=p.offset&&p.offset+p.filesz<=c.second; })) continue;
        if(p.type==1||p.type==2||p.type==0x61000000||p.type==0x61000010) fail("required segment "+std::to_string(i)+" is unavailable");
    }
    return self;
}

const Segment* find_segment(const std::vector<Segment>& ph,uint32_t type) {
    for(const auto& p:ph) if(p.type==type) return &p;
    return nullptr;
}
const Segment& require_segment(const std::vector<Segment>& ph,uint32_t type) {
    if(auto p=find_segment(ph,type)) return *p;
    fail("program header "+hex(type)+" is missing");
}

// The dynamic table and the strings it names (prepare.py prepare, inspect_libc; link_modules.py module).
struct Dynamic { std::vector<std::pair<uint64_t,uint64_t>> entries; std::map<uint64_t,uint64_t> tags; View blob,strings; };

uint64_t tag(const std::map<uint64_t,uint64_t>& tags,uint64_t key) {
    auto found=tags.find(key);
    if(found==tags.end()) fail("dynamic tag "+hex(key)+" is missing");
    return found->second;
}
Dynamic dynamic(const Self& self,bool stop_at_null) {
    Dynamic d;
    const auto& dp=require_segment(self.ph,2);
    for(uint64_t pos=dp.offset;pos<dp.offset+dp.filesz;pos+=16) {
        View entry=span(self.elf,pos,16);
        const uint64_t key=read<uint64_t>(entry,0),value=read<uint64_t>(entry,8);
        if(stop_at_null&&!key) break;
        d.entries.emplace_back(key,value);
        d.tags[key]=value;
    }
    const auto& lib=require_segment(self.ph,0x61000000);
    d.blob=span(self.elf,lib.offset,lib.filesz);
    d.strings=span(d.blob,tag(d.tags,0x61000035),tag(d.tags,0x61000037));
    return d;
}
std::string text(View strings,uint64_t offset) {
    span(strings,offset,1);
    auto begin=strings.begin()+offset,end=std::find(begin,strings.end(),uint8_t(0));
    if(end==strings.end()) fail("unterminated string");
    if(std::any_of(begin,end,[](uint8_t c) { return c>=0x80; })) fail("non-ASCII symbol name");
    return std::string(begin,end);
}
View symbol_table(const Dynamic& d) { return span(d.blob,tag(d.tags,0x61000039),tag(d.tags,0x6100003f)); }

// prepare.py inspect_libc: whether this dump's _init_env is exactly RET.
bool init_env_is_ret(View source) {
    const Self self=parse_self(source);
    const Dynamic d=dynamic(self,false);
    View syms=symbol_table(d);
    bool result=false;
    for(uint64_t pos=0;pos<syms.size();pos+=24) {
        View entry=span(syms,pos,24);
        const uint8_t info=entry[4];
        const uint16_t section=read<uint16_t>(entry,6);
        const uint64_t value=read<uint64_t>(entry,8),size=read<uint64_t>(entry,16);
        const std::string symbol=text(d.strings,read<uint32_t>(entry,0));
        const std::string key=symbol.substr(0,symbol.find('#'));
        // The NIDs of _init_env and _ZNSt8ios_base4InitC1Ev (prepare.py nid).
        const bool init_env=key=="bzQExy189ZI";
        if((!init_env&&key!="sqWytnhYdEg")||!section||(info&15)!=2) continue;
        const Segment* code_segment=nullptr;
        for(const auto& p:self.ph)
            if(p.type==1&&(p.flags&1)&&p.vaddr<=value&&Wide(value)<Wide(p.vaddr)+p.filesz) { code_segment=&p; break; }
        if(!code_segment) fail("libc.prx: "+symbol+" lies outside its code");
        View code=span(self.elf,code_segment->offset+value-code_segment->vaddr,size);
        if(init_env) result=code.size()==1&&code[0]==0xc3;
    }
    return result;
}

// prepare.py prepare: the eboot's memory image, its imports and relocations (boot.bin, BBPROBE2).
struct Relocation { uint64_t target,kind; int64_t value,addend; };
struct Probe {
    uint64_t size=0,entry=0,flags=0;
    std::vector<std::array<uint64_t,3>> segments;
    std::vector<std::string> names;
    std::vector<Relocation> relocs;
    Bytes image;
};

bool mapped(const std::vector<const Segment*>& loads,uint64_t address,uint64_t size) {
    return std::any_of(loads.begin(),loads.end(),[&](const Segment* p) {
        return p->vaddr<=address&&Wide(address)+size<=Wide(p->vaddr)+p->memsz; });
}
std::vector<const Segment*> load_segments(const Self& self) {
    std::vector<const Segment*> loads;
    for(const auto& p:self.ph) if(p.type==1||p.type==0x61000010) loads.push_back(&p);
    if(loads.empty()) fail("no loadable segments");
    return loads;
}
uint64_t memory_size(const std::vector<const Segment*>& loads) {
    Wide size=0;
    for(auto p:loads) size=std::max(size,Wide(p->vaddr)+p->memsz);
    return size>Wide(image_limit) ? image_limit+1 : uint64_t(size);
}

Probe prepare_eboot(const Self& self,bool init_env) {
    const auto loads=load_segments(self);
    Probe probe;
    probe.size=memory_size(loads);
    if(probe.size>image_limit||!mapped(loads,self.entry,1)) fail("invalid memory image");
    probe.entry=self.entry;
    probe.image.assign(probe.size,0);
    for(auto p:loads) {
        if(p->filesz>p->memsz) fail("segment file size exceeds memory size");
        View bytes=span(self.elf,p->offset,p->filesz);
        std::copy(bytes.begin(),bytes.end(),probe.image.begin()+p->vaddr);
    }
    const Dynamic d=dynamic(self,true);
    View syms=symbol_table(d);
    if(syms.size()%24) fail("bad symbol table size");
    struct Symbol { std::string name; unsigned type; uint16_t section; uint64_t value; };
    std::vector<Symbol> symbols;
    for(uint64_t pos=0;pos<syms.size();pos+=24) {
        View entry=span(syms,pos,24);
        symbols.push_back({text(d.strings,read<uint32_t>(entry,0)),entry[4]&15u,read<uint16_t>(entry,6),read<uint64_t>(entry,8)});
    }
    std::map<uint64_t,uint64_t> imports;
    std::vector<uint64_t> import_order;
    for(auto [offset_tag,size_tag]:relocation_tables) {
        View table=span(d.blob,tag(d.tags,offset_tag),tag(d.tags,size_tag));
        if(table.size()%24) fail("bad relocation table size");
        for(uint64_t pos=0;pos<table.size();pos+=24) {
            View entry=span(table,pos,24);
            const uint64_t target=read<uint64_t>(entry,0),info=read<uint64_t>(entry,8);
            const int64_t addend=read<int64_t>(entry,16);
            const uint64_t kind=info&0xffffffff,sym=info>>32;
            if(!mapped(loads,target,8)) fail("relocation outside mapped segments: "+hex(target));
            if(kind==8) {
                probe.relocs.push_back({target,0,addend,0});
            } else if(kind==1||kind==6||kind==7) {
                if(sym>=symbols.size()) fail("relocation symbol out of range");
                const auto& symbol=symbols[sym];
                if(symbol.section) {
                    if(symbol.section==0xfff1) store(probe.image,target,as_u64(Wide(symbol.value)+addend));
                    else probe.relocs.push_back({target,0,as_i64(Wide(symbol.value)+addend),0});
                } else {
                    auto [import,inserted]=imports.try_emplace(sym,imports.size());
                    if(inserted) import_order.push_back(sym);
                    const uint64_t import_kind=symbol.type==1 ? 2 : 1;
                    if((import_kind==1&&addend)||addend<0||addend>=4096) fail("unsupported import addend");
                    probe.relocs.push_back({target,import_kind,int64_t(import->second),addend});
                }
            } else {
                fail("unsupported relocation "+std::to_string(kind));
            }
        }
    }
    for(auto sym:import_order) probe.names.push_back(symbols[sym].name);
    for(const auto& name:probe.names) if(name.size()>=128) fail("import name too long");
    for(auto p:loads) probe.segments.push_back({p->vaddr,p->memsz,p->flags});
    probe.flags=init_env;
    return probe;
}

// link_modules.py: the eboot and the bundled modules in one image (boot-linked.bin, BBPROBE5).
struct Identity {
    std::string nid,library; uint64_t library_version; std::string module; uint64_t module_version;
    auto operator<=>(const Identity&) const=default;
};
using Key=std::optional<Identity>;
struct Symbol { std::string name; unsigned type,binding; uint16_t section; uint64_t value,size; Key identity; };
struct ModuleRelocation { uint64_t target,kind,symbol; int64_t addend; };
struct Module {
    Self self;
    Dynamic d;
    std::map<std::string,std::pair<std::string,uint64_t>> libraries,modules;
    std::vector<Symbol> symbols;
    std::vector<ModuleRelocation> relocs;
    // NID#library#module names link; plain names such as module_start do not.
    Key identity(const std::string& name) const {
        const auto first=name.find('#'),second=first==std::string::npos ? first : name.find('#',first+1);
        if(second==std::string::npos||name.find('#',second+1)!=std::string::npos) return std::nullopt;
        auto library=libraries.find(name.substr(first+1,second-first-1));
        if(library==libraries.end()) fail("unknown library in "+name);
        auto module=modules.find(name.substr(second+1));
        if(module==modules.end()) fail("unknown module in "+name);
        return Identity{name.substr(0,first),library->second.first,library->second.second,module->second.first,module->second.second};
    }
};

// link_modules.py RUNTIME_SPELLING and runtime_name: library and module codes are assigned per executable; the
// host runtime spells imports with the codes of the executable it was first written for (Bloodborne CUSA03173),
// by library name, and NID#library otherwise.
std::string runtime_name(const Identity& key) {
    static const std::map<std::string,std::pair<char,char>> spelling={
        {"libSceAjm",{'N','O'}},{"libSceAppContent",{'c','d'}},{"libSceAudioIn",{'Y','Z'}},
        {"libSceAudioOut",{'G','H'}},{"libSceAvPlayer",{'m','n'}},{"libSceCommonDialog",{'Z','a'}},
        {"libSceFios2",{'F','G'}},{"libSceGameLiveStreaming",{'e','f'}},{"libSceGnmDriver",{'B','C'}},
        {"libSceHttp",{'K','L'}},{"libSceImeDialog",{'b','c'}},{"libSceMouse",{'U','V'}},
        {"libSceMsgDialog",{'j','k'}},{"libSceNet",{'E','F'}},{"libSceNetCtl",{'J','K'}},
        {"libSceNpAuth",{'X','Y'}},{"libSceNpCommerce",{'k','l'}},{"libSceNpCommon",{'Q','R'}},
        {"libSceNpManager",{'R','S'}},{"libSceNpMatching2",{'P','Q'}},{"libSceNpProfileDialog",{'h','i'}},
        {"libSceNpScore",{'o','p'}},{"libSceNpSignaling",{'T','U'}},{"libSceNpTrophy",{'l','m'}},
        {"libSceNpUtility",{'S','T'}},{"libSceNpWebApi",{'W','X'}},{"libScePad",{'A','B'}},
        {"libScePlayGo",{'n','o'}},{"libScePosix",{'I','J'}},{"libSceRtc",{'H','I'}},
        {"libSceSaveData",{'O','P'}},{"libSceSaveDataDialog",{'d','e'}},{"libSceScreenShot",{'g','h'}},
        {"libSceSsl",{'L','M'}},{"libSceSysmodule",{'M','N'}},{"libSceSystemService",{'V','W'}},
        {"libSceUserService",{'D','E'}},{"libSceVideoOut",{'C','D'}},{"libSceVideoRecording",{'f','g'}},
        {"libSceVoice",{'i','j'}},{"libSceWebBrowserDialog",{'a','b'}},{"libc",{'q','q'}},
        {"libkernel",{'p','J'}},
    };
    static const std::map<std::string,std::string> module_of={{"libSceAppContent","libSceAppContentUtil"},{"libScePosix","libkernel"}};
    const auto codes=spelling.find(key.library);
    const auto module=module_of.find(key.library);
    if(codes!=spelling.end()&&key.module==(module==module_of.end() ? key.library : module->second))
        return key.nid+"#"+codes->second.first+"#"+codes->second.second;
    return key.nid+"#"+key.library;
}

// link_libc.py encode_id
std::string encode_id(uint64_t value) {
    static const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
    std::string result(1,alphabet[value&63]);
    for(value>>=6;value;value>>=6) result.insert(result.begin(),alphabet[value&63]);
    return result;
}

Module load_module(View source) {
    Module m;
    m.self=parse_self(source);
    m.d=dynamic(m.self,true);
    for(auto [key,value]:m.d.entries) {
        if(key!=0x61000013&&key!=0x61000015&&key!=0x6100000d&&key!=0x6100000f) continue;
        auto& table=key==0x61000013||key==0x61000015 ? m.libraries : m.modules;
        const std::string id=encode_id(value>>48);
        std::pair<std::string,uint64_t> identity{text(m.d.strings,value&0xffffffff),(value>>32)&65535};
        auto found=table.find(id);
        if(found!=table.end()&&found->second!=identity) fail("conflicting module/library IDs");
        table[id]=identity;
    }
    View syms=symbol_table(m.d);
    for(uint64_t pos=0;pos<syms.size();pos+=24) {
        View entry=span(syms,pos,24);
        Symbol s;
        s.name=text(m.d.strings,read<uint32_t>(entry,0));
        s.type=entry[4]&15u; s.binding=entry[4]>>4;
        s.section=read<uint16_t>(entry,6); s.value=read<uint64_t>(entry,8); s.size=read<uint64_t>(entry,16);
        if(!s.name.empty()) s.identity=m.identity(s.name);
        m.symbols.push_back(std::move(s));
    }
    for(auto [offset_tag,size_tag]:relocation_tables) {
        View table=span(m.d.blob,tag(m.d.tags,offset_tag),tag(m.d.tags,size_tag));
        for(uint64_t pos=0;pos<table.size();pos+=24) {
            View entry=span(table,pos,24);
            const uint64_t info=read<uint64_t>(entry,8);
            m.relocs.push_back({read<uint64_t>(entry,0),info&0xffffffff,info>>32,read<int64_t>(entry,16)});
        }
    }
    return m;
}

// Initial-exec `mov rax, fs:[0]` becomes GS (link_modules.py patch_fs_loads).
void patch_fs_loads(Bytes& image,const std::vector<Segment>& ph,uint64_t base) {
    static const uint8_t load[]={0x64,0x48,0x8b,0x04,0x25,0,0,0,0};
    for(const auto& p:ph) {
        if(p.type!=1||!(p.flags&1)) continue;
        const uint64_t end=std::min<uint64_t>(base+p.vaddr+p.filesz,image.size());
        for(uint64_t at=base+p.vaddr;at+sizeof load<=end;) {
            auto found=std::search(image.begin()+at,image.begin()+end,std::begin(load),std::end(load));
            if(found==image.begin()+end) break;
            *found=0x65;
            at=uint64_t(found-image.begin())+sizeof load;
        }
    }
}

Bytes link(const Module& main,Probe& probe,const std::vector<Module>& modules) {
    // The import table: identity -> index, in the order identities first appear (a Python dict).
    std::vector<Key> keys;
    std::vector<uint64_t> indexes;
    std::map<Key,size_t> positions;
    auto set=[&](const Key& key,uint64_t index) {
        auto [position,inserted]=positions.try_emplace(key,keys.size());
        if(inserted) { keys.push_back(key); indexes.push_back(index); }
        else indexes[position->second]=index;
    };
    for(size_t i=0;i<probe.names.size();++i) set(main.identity(probe.names[i]),i);
    std::vector<std::optional<std::string>> names;
    for(const auto& name:probe.names) {
        auto key=main.identity(name);
        names.push_back(key ? runtime_name(*key) : name);
    }
    auto imported=[&](const Symbol& symbol)->uint64_t {
        auto position=positions.find(symbol.identity);
        if(position!=positions.end()) return indexes[position->second];
        set(symbol.identity,names.size());
        names.push_back(symbol.identity ? std::optional<std::string>(runtime_name(*symbol.identity)) : std::nullopt);
        return names.size()-1;
    };
    struct Export { uint64_t address,kind; };
    std::map<Identity,Export> exports;
    std::map<std::pair<std::string,std::string>,std::vector<Export>> by_nid;
    std::vector<std::array<uint64_t,7>> table;
    Bytes& image=probe.image;
    uint64_t base=(probe.size+65535)&~uint64_t(65535);
    patch_fs_loads(image,main.self.ph,0);
    uint64_t tls_module=2;
    for(const auto& m:modules) {
        const auto loads=load_segments(m.self);
        const uint64_t modsize=memory_size(loads);
        if(Wide(base)+modsize>Wide(image_limit)) fail("linked image exceeds probe limit");
        if(base+modsize<image.size()) fail("module overlaps the image");
        image.resize(base+modsize,0);
        for(auto p:loads) {
            if(p->filesz>p->memsz) fail("invalid module segment size");
            View bytes=span(m.self.elf,p->offset,p->filesz);
            std::copy(bytes.begin(),bytes.end(),image.begin()+base+p->vaddr);
            probe.segments.push_back({base+p->vaddr,p->memsz,p->flags});
        }
        const Segment* tls=find_segment(m.self.ph,7);
        uint64_t module_id=0;
        if(tls) {
            if(tls->filesz>tls->memsz||!mapped(loads,tls->vaddr,tls->memsz)) fail("unsupported module TLS layout");
            module_id=tls_module++;
        }
        for(const auto& r:m.relocs) {
            if(!mapped(loads,r.target,8)) fail("module relocation outside image "+hex(r.target));
            const uint64_t target=r.target+base;
            if(r.kind==8) {
                probe.relocs.push_back({target,0,as_i64(Wide(base)+r.addend),0});
            } else if(r.kind==16||r.kind==1||r.kind==6||r.kind==7) {
                if(r.symbol>=m.symbols.size()) fail("module relocation symbol out of range");
                const auto& symbol=m.symbols[r.symbol];
                if(r.kind==16) {
                    if(!symbol.name.empty()||symbol.type!=3||r.addend||!module_id) fail("unsupported external TLS relocation");
                    store(image,target,module_id);
                } else if(symbol.section) {
                    if(symbol.section==0xfff1) store(image,target,as_u64(Wide(symbol.value)+r.addend));
                    else probe.relocs.push_back({target,0,as_i64(Wide(base)+symbol.value+r.addend),0});
                } else if(!symbol.identity&&symbol.binding==2) {
                    store(image,target,0);  // weak undefined (module_start/stop)
                } else {
                    const uint64_t kind=symbol.type==1 ? 2 : 1;
                    if((kind==1&&r.addend)||r.addend<0||r.addend>=4096) fail("unsupported module import addend");
                    probe.relocs.push_back({target,kind,int64_t(imported(symbol)),r.addend});
                }
            } else {
                fail("unsupported module relocation type "+std::to_string(r.kind));
            }
        }
        for(const auto& s:m.symbols) {
            if(!s.section||(s.binding!=1&&s.binding!=2)||!s.identity||(s.type!=1&&s.type!=2)) continue;
            if(!mapped(loads,s.value,std::max<uint64_t>(1,s.size))) fail("export outside load segments");
            if(exports.count(*s.identity)) fail("ambiguous export");
            const Export entry{base+s.value,s.type==1 ? 2u : 1u};
            exports[*s.identity]=entry;
            by_nid[{s.identity->nid,s.identity->library}].push_back(entry);
        }
        patch_fs_loads(image,m.self.ph,base);
        auto init=m.d.tags.find(12);
        table.push_back({base,modsize,as_u64(Wide(base)+(init==m.d.tags.end() ? 0 : init->second)),
                         tls ? base+tls->vaddr : 0,tls ? tls->memsz : 0,tls ? tls->filesz : 0,module_id});
        base=(base+modsize+65535)&~uint64_t(65535);
    }
    std::vector<std::array<uint64_t,3>> bindings;
    for(size_t i=0;i<keys.size();++i) {
        const auto& key=keys[i];
        if(!key) continue;
        const Export* found=nullptr;
        if(auto entry=exports.find(*key);entry!=exports.end()) found=&entry->second;
        if(!found&&key->library=="libSceLibcInternal") {
            auto candidates=by_nid.find({key->nid,"libc"});
            if(candidates!=by_nid.end()&&candidates->second.size()==1) found=&candidates->second[0];
        }
        if(found) bindings.push_back({indexes[i],found->address,found->kind});
    }
    std::array<uint64_t,4> main_tls{};
    if(const Segment* tls=find_segment(main.self.ph,7)) {
        main_tls={tls->vaddr,tls->filesz,tls->memsz,tls->align};
        if(tls->filesz>tls->memsz||tls->memsz>1024*1024||Wide(tls->vaddr)+tls->filesz>Wide(probe.size)) fail("unsupported eboot TLS layout");
    }
    const auto& procparam=require_segment(main.self.ph,0x61000001);
    Bytes out;
    out.reserve(image.size()+probe.relocs.size()*32+names.size()*128+(1<<16));
    put_text(out,"BBPROBE5",8);
    for(uint64_t value:{uint64_t(image.size()),probe.entry,uint64_t(probe.segments.size()),uint64_t(probe.relocs.size()),
                        uint64_t(names.size()),probe.flags,procparam.vaddr}) put(out,value);
    for(auto value:main_tls) put(out,value);
    put<uint64_t>(out,table.size());
    for(const auto& entry:table) for(auto value:entry) put(out,value);
    put<uint64_t>(out,bindings.size());
    for(const auto& binding:bindings) for(auto value:binding) put(out,value);
    for(const auto& segment:probe.segments) for(auto value:segment) put(out,value);
    for(const auto& name:names) {
        if(!name) fail("import without a name");
        if(name->size()>=128) fail("name too long");
        put_text(out,name->data(),name->size());
        out.resize(out.size()+128-name->size(),0);
    }
    for(const auto& r:probe.relocs) { put(out,r.target); put(out,r.kind); put(out,r.value); put(out,r.addend); }
    out.insert(out.end(),image.begin(),image.end());
    return out;
}

// prepare.py sfo
struct SfoValue { std::optional<uint32_t> number; std::string text; };
std::map<std::string,SfoValue> sfo(View data) {
    if(data.empty()) fail("param.sfo is empty (0 bytes): the game files are incomplete");
    View head=span(data,0,20);
    if(std::memcmp(head.data(),"\0PSF",4)) fail("bad SFO signature");
    const uint32_t keys=read<uint32_t>(head,8),values=read<uint32_t>(head,12),count=read<uint32_t>(head,16);
    std::map<std::string,SfoValue> result;
    for(uint32_t i=0;i<count;++i) {
        View entry=span(data,20+uint64_t(i)*16,16);
        const uint16_t key=read<uint16_t>(entry,0),format=read<uint16_t>(entry,2);
        const uint32_t size=read<uint32_t>(entry,4),offset=read<uint32_t>(entry,12);
        const uint64_t start=uint64_t(keys)+key;
        if(start>=data.size()) fail("bad SFO key");
        auto end=std::find(data.begin()+start,data.end(),uint8_t(0));
        if(end==data.end()) fail("bad SFO key");
        View value=span(data,uint64_t(values)+offset,size);
        SfoValue parsed;
        if(format==0x404) {
            if(size!=4) fail("bad SFO integer");
            parsed.number=read<uint32_t>(value,0);
        } else {
            size_t length=value.size();
            while(length&&value[length-1]==0) --length;
            parsed.text.assign(value.begin(),value.begin()+length);
        }
        result[std::string(data.begin()+start,end)]=parsed;
    }
    return result;
}

// The collection by its title ID, else by its title without ® and ™.
bool known_game(const std::map<std::string,SfoValue>& values) {
    auto id=values.find("TITLE_ID");
    if(id!=values.end()&&!id->second.number)
        for(auto known:known_titles) if(id->second.text==known) return true;
    auto title=values.find("TITLE");
    if(title==values.end()||title->second.number) return false;
    std::string name=title->second.text;
    for(const char* mark:{"\xc2\xae","\xe2\x84\xa2"})
        for(size_t at;(at=name.find(mark))!=std::string::npos;) name.erase(at,std::strlen(mark));
    return name.find("Uncharted")!=std::string::npos;
}

// content_profile.py prepare (SKU full)
Bytes content(const std::map<std::string,SfoValue>& values) {
    Bytes out;
    put_text(out,"BBCONT01",8);
    put<uint32_t>(out,3);
    for(int i=1;i<=4;++i) {
        auto value=values.find("USER_DEFINED_PARAM_"+std::to_string(i));
        if(value!=values.end()&&!value->second.number) fail("invalid user-defined parameter");
        put<uint32_t>(out,value==values.end() ? 0 : *value->second.number);
    }
    return out;
}

}

std::string CheckTitle(const Bytes& data) {
    try {
        if(known_game(sfo(data))) return {};
        return "is not Uncharted: The Nathan Drake Collection.";
    } catch(const std::exception& error) {
        return std::string("has an unreadable sce_sys\\param.sfo (")+error.what()+").";
    }
}

Files Prepare(const Dump& dump) {
    const auto values=sfo(dump.sfo);
    if(!known_game(values)) fail("param.sfo does not name Uncharted: The Nathan Drake Collection");
    if(dump.exe.empty()) fail("the executable is empty (0 bytes): the game files are incomplete");
    if(dump.libc.empty()) fail("libc.prx is empty (0 bytes): the game files are incomplete");
    if(dump.fios.empty()) fail("libSceFios2.prx is empty (0 bytes): the game files are incomplete");
    Files files;
    Probe probe=prepare_eboot(parse_self(dump.exe),init_env_is_ret(dump.libc));
    std::vector<Module> modules;
    modules.push_back(load_module(dump.libc));
    modules.push_back(load_module(dump.fios));
    files.boot=link(load_module(dump.exe),probe,modules);
    files.content=content(values);
    try { files.render720p=Render720p(dump.exe); }
    catch(const std::exception& error) { files.render720p_error=error.what(); }
    return files;
}

// render_720p.py (spacepepperoni/uncharted_Linux): the collection's engine keeps its video settings in
// one structure, set by default setups and by a mode switch whose 1080p case sets every size field
// (scene, UI, viewport) to 1920x1080; the PS4's 720p case already exists. The defaults get the 720p
// values (1280x720, and 160x180 in fields 0x40/0x44) and the 1080p case jumps to the 720p case. The
// movie decoder's maximum frame size becomes 1280x720 (Videodec2 scales the 1080p movies to fit).
// The sites are found by their instruction bytes, the same in U1, U2 and U3.
Bytes Render720p(const Bytes& exe) {
    const Self self=parse_self(exe);
    const Bytes& elf=self.elf;
    if(self.ph.empty()) fail("no program headers");
    const uint64_t base=self.ph[0].offset-self.ph[0].vaddr;
    const size_t size=elf.size();
    auto u32=[&](size_t at) { uint32_t value; std::memcpy(&value,elf.data()+at,4); return value; };
    // movl $imm32, disp8(%reg): an optional REX.B prefix, C7, ModRM mod=01 reg=0 with any base but rsp.
    auto modrm=[](uint8_t value) { return value>=0x40&&value<0x48&&value!=0x44; };
    auto movl=[&](size_t at,uint8_t disp,uint32_t imm) -> size_t {
        const size_t op=at<size&&elf[at]==0x41 ? at+1 : at;
        if(op+7>size||elf[op]!=0xc7||!modrm(elf[op+1])||elf[op+2]!=disp||u32(op+3)!=imm) return 0;
        return op+7-at;
    };
    // movl $1920, 0x8(%reg); movl $1080, 0xc(%reg): the first two size fields of a setup.
    std::vector<size_t> sites;
    for(size_t at=0;at<size;) {
        const size_t first=movl(at,0x08,1920),second=first ? movl(at+first,0x0c,1080) : 0;
        if(second) { sites.push_back(at); at+=first+second; } else ++at;
    }
    constexpr size_t max_sites=3,window=0x90;
    if(sites.empty()||sites.size()>max_sites)
        fail("expected 1 to 3 video setting sites, found "+std::to_string(sites.size()));
    std::vector<std::pair<uint64_t,Bytes>> writes;
    auto write32=[&](size_t at,uint32_t value) {
        Bytes data(4); std::memcpy(data.data(),&value,4); writes.push_back({at-base,data});
    };
    for(size_t index=0;index<sites.size();++index) {
        const size_t site=sites[index];
        const size_t end=std::min(site+window,index+1<sites.size() ? sites[index+1]-9 : size);
        // cmp $1, %reg32; jne <next mode>: the mode switch's 1080p case jumps to the 720p case.
        if(site>=9&&elf[site-9]==0x83&&elf[site-8]>=0xf8&&elf[site-7]==0x01&&elf[site-6]==0x0f&&elf[site-5]==0x85) {
            int32_t rel; std::memcpy(&rel,elf.data()+site-4,4);
            ++rel;  // jmp rel32 is one byte shorter
            Bytes data{0xe9,0,0,0,0,0x90}; std::memcpy(data.data()+1,&rel,4);
            writes.push_back({site-6-base,data});
            continue;
        }
        for(size_t at=site;at<end&&at+4<=size;++at) {
            const uint32_t value=u32(at);
            uint32_t replacement;
            if(value==1920) replacement=1280;
            else if(value==1080) replacement=720;
            else if(value==1920/8) replacement=1280/8;
            else if(value==1080/4) replacement=720/4;
            else continue;
            // movl $imm32, disp8(%reg), or either half of movabs $imm64, %rax
            const bool is_movl=at>=3&&elf[at-3]==0xc7&&modrm(elf[at-2]);
            const bool is_movabs=(at>=2&&elf[at-2]==0x48&&elf[at-1]==0xb8)||(at>=6&&elf[at-6]==0x48&&elf[at-5]==0xb8);
            // 1920/8 and 1080/4 only in fields 0x40/0x44 (the 1080p mode's 240x270)
            if((value==1920/8||value==1080/4)&&!(is_movl&&(elf[at-1]==0x40||elf[at-1]==0x44))) continue;
            if(is_movl||is_movabs) write32(at,replacement);
        }
    }
    // movl $1920, d(%rsp); movl $1088, d+4(%rsp): the movie decoder's maximum frame size.
    std::vector<size_t> decoders;
    static constexpr uint8_t width_bytes[]={0x80,0x07,0x00,0x00},height_bytes[]={0x40,0x04,0x00,0x00};
    for(size_t at=0;at+16<=size;) {
        const uint8_t* p=elf.data()+at;
        if(p[0]==0xc7&&p[1]==0x44&&p[2]==0x24&&!std::memcmp(p+4,width_bytes,4)&&
           p[8]==0xc7&&p[9]==0x44&&p[10]==0x24&&!std::memcmp(p+12,height_bytes,4)) {
            if(int(p[11])==int(p[3])+4) decoders.push_back(at);
            at+=16;
        } else ++at;
    }
    if(decoders.size()!=1) fail("expected one movie decoder setup, found "+std::to_string(decoders.size()));
    write32(decoders[0]+4,1280);
    write32(decoders[0]+12,720);
    Bytes out;
    put_text(out,"BBPATCH2",8);
    put<uint64_t>(out,0x400000);
    put<uint64_t>(out,writes.size());
    for(const auto& [offset,data]:writes) {
        put<uint64_t>(out,offset);
        put<uint64_t>(out,data.size());
        out.insert(out.end(),data.begin(),data.end());
    }
    return out;
}

uint64_t Fingerprint(const Dump& dump) {
    uint64_t hash=0xcbf29ce484222325ull;
    auto mix=[&](const uint8_t* data,size_t size) {
        const uint64_t length=size;
        for(unsigned i=0;i<8;++i) { hash^=uint8_t(length>>(8*i)); hash*=0x100000001b3ull; }
        for(size_t i=0;i<size;++i) { hash^=data[i]; hash*=0x100000001b3ull; }
    };
    for(const Bytes* file:{&dump.exe,&dump.libc,&dump.fios,&dump.sfo}) mix(file->data(),file->size());
    return hash;
}
}
