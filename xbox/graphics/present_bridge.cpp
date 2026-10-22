#include "common/types.h"
#include "present_bridge.h"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <stdexcept>
#include <array>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <string>
#include <cwchar>
#include <cwctype>

extern "C" IUnknown* uncharted_uwp_core_window();
using Microsoft::WRL::ComPtr;

namespace UnchartedUwp {
static void Check(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        std::fprintf(stderr,"%s: HRESULT 0x%08lx\n",operation,static_cast<unsigned long>(result));
        throw std::runtime_error(operation);
    }
}
static void Check(vk::Result result, const char* operation) {
    if (result != vk::Result::eSuccess) {
        std::fprintf(stderr,"%s: Vulkan %d\n",operation,static_cast<int>(result));
        throw std::runtime_error(operation);
    }
}
// The D3D12 device Dozen renders with: D3D12 returns one device per adapter, so the presentation
// device is the same object, and its removal data describes the game's GPU crash.
static ComPtr<ID3D12Device> crash_device;
void InitializeDriver() {
    // BB_DRED=1 (graphics-debug.txt): D3D12 records breadcrumbs and page faults for a GPU crash report
    // (uncharted_uwp_dred_report). It must be on before Dozen creates its device.
    if (const char* dred=std::getenv("BB_DRED"); dred && *dred=='1') {
        ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> settings;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&settings)))) {
            settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            settings->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            std::printf("D3D12 DRED: breadcrumbs and page faults recorded\n");
        } else {
            std::printf("D3D12 DRED: unavailable\n");
        }
    }
    static HMODULE driver = LoadPackagedLibrary(L"vulkan_dzn.dll",0);
    if (!driver) throw std::runtime_error("Cannot load packaged Dozen driver");
    auto proc=reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(driver,"vk_icdGetInstanceProcAddr"));
    if (!proc) throw std::runtime_error("Dozen ICD entry point missing");
    VULKAN_HPP_DEFAULT_DISPATCHER.init(proc);
}
struct Presenter::Impl {
    vk::Device device;
    vk::Queue queue;
    u32 family;
    vk::UniqueCommandPool pool;
    vk::CommandBuffer cmd;
    vk::UniqueFence vk_fence;
    vk::Image image;
    vk::DeviceMemory memory;
    vk::ImageView view;
    vk::UniqueSemaphore acquired,ready;
    ComPtr<ID3D12Device> native;
    ComPtr<ID3D12CommandQueue> native_queue;
    ComPtr<IDXGISwapChain3> swapchain;
    ComPtr<ID3D12Resource> source;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE event=nullptr;
    UINT64 tick=0;
    bool first=true;
    void WaitNative() {
        if (fence->GetCompletedValue() >= tick) return;
        Check(fence->SetEventOnCompletion(tick,event),"Native fence event");
        if (WaitForSingleObject(event,10000)!=WAIT_OBJECT_0) throw std::runtime_error("Native presentation timed out");
    }
    void Submit(std::span<const vk::Semaphore> waits,std::span<const vk::Semaphore> signals) {
        std::array<vk::PipelineStageFlags,1> stages{vk::PipelineStageFlagBits::eAllCommands};
        Check(device.resetFences(*vk_fence),"Reset bridge fence");
        vk::SubmitInfo submit{
            .waitSemaphoreCount=static_cast<u32>(waits.size()),.pWaitSemaphores=waits.data(),
            .pWaitDstStageMask=stages.data(),.commandBufferCount=1,.pCommandBuffers=&cmd,
            .signalSemaphoreCount=static_cast<u32>(signals.size()),.pSignalSemaphores=signals.data(),
        };
        Check(queue.submit(submit,*vk_fence),"Submit presentation bridge");
        Check(device.waitForFences(*vk_fence,true,10'000'000'000ULL),"Wait presentation bridge");
    }
    ~Impl() {
        if(native_queue && fence) {
            native_queue->Signal(fence.Get(),++tick);
            if(event) { try { WaitNative(); } catch(...) {} }
        }
        if(device) {
            device.waitIdle();
            device.destroyImageView(view);
            device.destroyImage(image);
            device.freeMemory(memory);
        }
        if(event) CloseHandle(event);
    }
};
Presenter::Presenter(vk::PhysicalDevice physical,vk::Device device,vk::Queue queue,u32 family,u32 width,u32 height)
    : impl(std::make_unique<Impl>()) {
    auto& p=*impl; p.device=device; p.queue=queue; p.family=family;
    auto ids=physical.getProperties2<vk::PhysicalDeviceProperties2,vk::PhysicalDeviceIDProperties>();
    auto const& id=ids.get<vk::PhysicalDeviceIDProperties>();
    if(!id.deviceLUIDValid) throw std::runtime_error("Dozen adapter has no valid LUID");
    LUID luid{}; std::memcpy(&luid,id.deviceLUID.data(),sizeof(luid));
    ComPtr<IDXGIFactory4> factory;
    Check(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)),"Create DXGI factory");
    ComPtr<IDXGIAdapter1> adapter;
    Check(factory->EnumAdapterByLuid(luid,IID_PPV_ARGS(&adapter)),"Find Dozen adapter");
    Check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&p.native)),"Create presentation device");
    crash_device=p.native;
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    Check(p.native->CreateCommandQueue(&queue_desc,IID_PPV_ARGS(&p.native_queue)),"Create presentation queue");
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width=width; desc.Height=height; desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count=1; desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.BufferCount=2;
    desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD; desc.Scaling=DXGI_SCALING_STRETCH;
    ComPtr<IUnknown> window; window.Attach(uncharted_uwp_core_window());
    ComPtr<IDXGISwapChain1> chain;
    Check(factory->CreateSwapChainForCoreWindow(p.native_queue.Get(),window.Get(),&desc,nullptr,&chain),"Create CoreWindow swapchain");
    Check(chain.As(&p.swapchain),"Query CoreWindow swapchain");
    Check(p.native->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&p.allocator)),"Create copy allocator");
    Check(p.native->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,p.allocator.Get(),nullptr,IID_PPV_ARGS(&p.list)),"Create copy list");
    Check(p.list->Close(),"Close copy list");
    Check(p.native->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&p.fence)),"Create copy fence");
    p.event=CreateEventExW(nullptr,nullptr,0,EVENT_ALL_ACCESS);
    if(!p.event) throw std::runtime_error("Create presentation event");
    const vk::ExternalMemoryImageCreateInfo external{.handleTypes=vk::ExternalMemoryHandleTypeFlagBits::eD3D12Resource};
    vk::ImageCreateInfo image_info{.pNext=&external,.imageType=vk::ImageType::e2D,
        .format=vk::Format::eR8G8B8A8Unorm,.extent={width,height,1},.mipLevels=1,.arrayLayers=1,
        .samples=vk::SampleCountFlagBits::e1,.tiling=vk::ImageTiling::eOptimal,
        .usage=vk::ImageUsageFlagBits::eColorAttachment|vk::ImageUsageFlagBits::eTransferSrc|vk::ImageUsageFlagBits::eTransferDst,
        .sharingMode=vk::SharingMode::eExclusive,.initialLayout=vk::ImageLayout::eUndefined};
    auto image_result=device.createImage(image_info); Check(image_result.result,"Create presentation image"); p.image=image_result.value;
    auto req=device.getImageMemoryRequirements(p.image);
    auto props=physical.getMemoryProperties(); u32 type=props.memoryTypeCount;
    for(u32 i=0;i<props.memoryTypeCount;++i) if((req.memoryTypeBits&(1u<<i)) && (props.memoryTypes[i].propertyFlags&vk::MemoryPropertyFlagBits::eDeviceLocal)) { type=i; break; }
    if(type==props.memoryTypeCount) throw std::runtime_error("No presentation memory type");
    vk::MemoryDedicatedAllocateInfo dedicated{.image=p.image};
    vk::ExportMemoryAllocateInfo export_info{.pNext=&dedicated,.handleTypes=vk::ExternalMemoryHandleTypeFlagBits::eD3D12Resource};
    auto allocation=device.allocateMemory({.pNext=&export_info,.allocationSize=req.size,.memoryTypeIndex=type});
    Check(allocation.result,"Allocate shared presentation image"); p.memory=allocation.value;
    Check(device.bindImageMemory(p.image,p.memory,0),"Bind presentation image");
    auto exported=device.getMemoryWin32HandleKHR({.memory=p.memory,.handleType=vk::ExternalMemoryHandleTypeFlagBits::eD3D12Resource});
    Check(exported.result,"Export presentation image");
    HRESULT opened=p.native->OpenSharedHandle(exported.value,IID_PPV_ARGS(&p.source));
    CloseHandle(exported.value); Check(opened,"Open presentation image in D3D12");
    auto view_result=device.createImageView({.image=p.image,.viewType=vk::ImageViewType::e2D,
        .format=vk::Format::eR8G8B8A8Unorm,.subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}});
    Check(view_result.result,"Create presentation view"); p.view=view_result.value;
    auto pool_result=device.createCommandPoolUnique({.flags=vk::CommandPoolCreateFlagBits::eResetCommandBuffer,.queueFamilyIndex=family});
    Check(pool_result.result,"Create bridge command pool"); p.pool=std::move(pool_result.value);
    auto commands=device.allocateCommandBuffers({.commandPool=*p.pool,.level=vk::CommandBufferLevel::ePrimary,.commandBufferCount=1});
    Check(commands.result,"Allocate bridge command"); p.cmd=commands.value[0];
    auto fence_result=device.createFenceUnique({}); Check(fence_result.result,"Create bridge fence"); p.vk_fence=std::move(fence_result.value);
    auto acquired=device.createSemaphoreUnique({}); Check(acquired.result,"Create acquire semaphore"); p.acquired=std::move(acquired.value);
    auto ready=device.createSemaphoreUnique({}); Check(ready.result,"Create ready semaphore"); p.ready=std::move(ready.value);
    std::printf("CoreWindow presentation bridge: %ux%u, shared image %llu KiB\n",width,height,static_cast<unsigned long long>(req.size>>10));
}
Presenter::~Presenter()=default;
vk::Image Presenter::Image() const {return impl->image;}
vk::ImageView Presenter::View() const {return impl->view;}
vk::Semaphore Presenter::Acquired() const {return *impl->acquired;}
vk::Semaphore Presenter::Ready() const {return *impl->ready;}
bool Presenter::Acquire() {
    auto& p=*impl; p.WaitNative();
    Check(p.cmd.reset({}),"Reset acquire command");
    Check(p.cmd.begin({.flags=vk::CommandBufferUsageFlagBits::eOneTimeSubmit}),"Begin acquire command");
    if(!p.first) p.cmd.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,vk::PipelineStageFlagBits::eAllCommands,{}, {}, {},
        vk::ImageMemoryBarrier{.dstAccessMask=vk::AccessFlagBits::eMemoryRead|vk::AccessFlagBits::eMemoryWrite,
        .oldLayout=vk::ImageLayout::eGeneral,.newLayout=vk::ImageLayout::eGeneral,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_EXTERNAL,.dstQueueFamilyIndex=p.family,
        .image=p.image,.subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}});
    Check(p.cmd.end(),"End acquire command");
    std::array signals{*p.acquired}; p.Submit({},signals); p.first=false; return true;
}
bool Presenter::Present(bool vsync) {
    auto& p=*impl;
    Check(p.cmd.reset({}),"Reset release command");
    Check(p.cmd.begin({.flags=vk::CommandBufferUsageFlagBits::eOneTimeSubmit}),"Begin release command");
    p.cmd.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,vk::PipelineStageFlagBits::eAllCommands,{}, {}, {},
        vk::ImageMemoryBarrier{.srcAccessMask=vk::AccessFlagBits::eMemoryWrite,
        .oldLayout=vk::ImageLayout::eGeneral,.newLayout=vk::ImageLayout::eGeneral,
        .srcQueueFamilyIndex=p.family,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_EXTERNAL,
        .image=p.image,.subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}});
    Check(p.cmd.end(),"End release command");
    std::array waits{*p.ready}; p.Submit(waits,{});
    Check(p.allocator->Reset(),"Reset native copy allocator");
    Check(p.list->Reset(p.allocator.Get(),nullptr),"Reset native copy command");
    ComPtr<ID3D12Resource> dest;
    Check(p.swapchain->GetBuffer(p.swapchain->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&dest)),"Get CoreWindow backbuffer");
    std::array<D3D12_RESOURCE_BARRIER,2> barriers{};
    for(auto& barrier:barriers) {barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;}
    barriers[0].Transition={p.source.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE};
    barriers[1].Transition={dest.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_DEST};
    p.list->ResourceBarrier(2,barriers.data()); p.list->CopyResource(dest.Get(),p.source.Get());
    for(auto& barrier:barriers) std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);
    p.list->ResourceBarrier(2,barriers.data()); Check(p.list->Close(),"Close native copy");
    ID3D12CommandList* lists[]={p.list.Get()}; p.native_queue->ExecuteCommandLists(1,lists);
    Check(p.swapchain->Present(vsync ? 1 : 0,0),"Present CoreWindow frame");
    Check(p.native_queue->Signal(p.fence.Get(),++p.tick),"Signal native frame"); p.WaitNative();
    if(p.tick==1) std::puts("CoreWindow first frame presented");
    return true;
}
}

// Dozen's D3D12 device: its device factory does not share devices with D3D12CreateDevice, so it is
// found in Dozen's VkDevice structure (a field after the dispatch tables): the first pointer there to
// a D3D12 object (its vtable in a d3d12 module) that is an ID3D12Device.
static ComPtr<ID3D12Device> FindDozenDevice(const void* vk_device) {
    auto readable=[](const void* address,size_t size) {
        MEMORY_BASIC_INFORMATION info{};
        if(!VirtualQuery(address,&info,sizeof info)||info.State!=MEM_COMMIT||(info.Protect&(PAGE_GUARD|PAGE_NOACCESS))) return false;
        return static_cast<const char*>(address)+size<=static_cast<const char*>(info.BaseAddress)+info.RegionSize;
    };
    auto in_d3d12=[](const void* address) {
        HMODULE module=nullptr;
        if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                static_cast<LPCWSTR>(address),&module)) return false;
        wchar_t name[MAX_PATH]{};
        GetModuleFileNameW(module,name,MAX_PATH);
        for(wchar_t* c=name;*c;++c) *c=static_cast<wchar_t>(towlower(*c));
        return std::wcsstr(name,L"d3d12")!=nullptr;
    };
    const auto* words=static_cast<void* const*>(vk_device);
    for(size_t i=0;i<0x10000/sizeof(void*);++i) {
        if(!readable(words+i,sizeof(void*))) break;
        void* object=words[i];
        const auto value=reinterpret_cast<uintptr_t>(object);
        if(value<0x10000||value>0x7fffffffffffull||value%8||!readable(object,sizeof(void*))) continue;
        void* vtable=*static_cast<void**>(object);
        if(!vtable||!readable(vtable,sizeof(void*))||!in_d3d12(vtable)||!in_d3d12(*static_cast<void**>(vtable))) continue;
        ComPtr<ID3D12Device> device;
        if(SUCCEEDED(static_cast<IUnknown*>(object)->QueryInterface(IID_PPV_ARGS(&device)))) {
            std::printf("D3D12 DRED: Dozen's device %p (VkDevice + 0x%zx)\n",static_cast<void*>(device.Get()),i*sizeof(void*));
            return device;
        }
    }
    return {};
}

// The game's GPU crash as D3D12 saw it: why the device was removed, the command lists the GPU had not
// finished with the operations around the one it stopped in, and the allocation a page fault hit
// (with BB_DRED=1; without it only the reason).
extern "C" void uncharted_uwp_dred_report(const void* vk_device) {
    using namespace UnchartedUwp;
    if(vk_device) if(auto dozen=FindDozenDevice(vk_device)) crash_device=dozen;
    if(!crash_device) { std::printf("D3D12 DRED: no device\n"); return; }
    const HRESULT reason=crash_device->GetDeviceRemovedReason();
    const char* name=reason==DXGI_ERROR_DEVICE_HUNG ? "hung (a command ran too long)" :
        reason==DXGI_ERROR_DEVICE_REMOVED ? "removed" : reason==DXGI_ERROR_DEVICE_RESET ? "reset (a bad command)" :
        reason==DXGI_ERROR_DRIVER_INTERNAL_ERROR ? "driver internal error" : reason==DXGI_ERROR_INVALID_CALL ? "invalid call" : "?";
    std::printf("D3D12 DRED: device removed reason 0x%08lx %s\n",static_cast<unsigned long>(reason),name);
    ComPtr<ID3D12DeviceRemovedExtendedData1> dred;
    if(FAILED(crash_device.As(&dred))) { std::printf("D3D12 DRED: no removal data\n"); return; }
    static constexpr const char* ops[]={"SetMarker","BeginEvent","EndEvent","DrawInstanced","DrawIndexedInstanced",
        "ExecuteIndirect","Dispatch","CopyBufferRegion","CopyTextureRegion","CopyResource","CopyTiles","ResolveSubresource",
        "ClearRenderTargetView","ClearUnorderedAccessView","ClearDepthStencilView","ResourceBarrier","ExecuteBundle","Present",
        "ResolveQueryData","BeginSubmission","EndSubmission","DecodeFrame","ProcessFrames","AtomicCopyBufferUint",
        "AtomicCopyBufferUint64","ResolveSubresourceRegion","WriteBufferImmediate","DecodeFrame1","SetProtectedResourceSession",
        "DecodeFrame2","ProcessFrames1","BuildRaytracingAccelerationStructure","EmitRaytracingAccelerationStructurePostbuildInfo",
        "CopyRaytracingAccelerationStructure","DispatchRays","InitializeMetaCommand","ExecuteMetaCommand","EstimateMotion",
        "ResolveMotionVectorHeap","SetPipelineState1","InitializeExtensionCommand","ExecuteExtensionCommand","DispatchMesh",
        "EncodeFrame","ResolveEncoderOutputMetadata","Barrier","BeginCommandList"};
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 crumbs{};
    if(const HRESULT result=dred->GetAutoBreadcrumbsOutput1(&crumbs);FAILED(result))
        std::printf("D3D12 DRED: no breadcrumbs (0x%08lx)\n",static_cast<unsigned long>(result));
    else {
        int shown=0;
        for(auto node=crumbs.pHeadAutoBreadcrumbNode;node&&shown<4;node=node->pNext) {
            const UINT32 count=node->BreadcrumbCount,done=node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
            if(done>=count) continue;
            ++shown;
            std::printf("D3D12 DRED: list %p on queue %p: %u of %u operations completed\n",
                static_cast<void*>(node->pCommandList),static_cast<void*>(node->pCommandQueue),done,count);
            // Operations of each kind before the one the GPU stopped in (to match it with the renderer's draws).
            UINT32 kinds[64]{};
            for(UINT32 i=0;i<done&&i<count;++i) if(node->pCommandHistory[i]<64) ++kinds[node->pCommandHistory[i]];
            std::printf("D3D12 DRED:   completed: draws %u, indexed draws %u, dispatches %u, barriers %u, copies %u\n",
                kinds[3],kinds[4],kinds[6],kinds[15]+kinds[45],kinds[7]+kinds[8]+kinds[9]);
            // The draws (D), indirect executions (I) and dispatches (C) as letters, "|" where the GPU
            // stopped: the renderer's checkpoint report (BB_GPU_CHECKPOINTS=1) prints its recordings so.
            std::string before,after;
            for(UINT32 i=0;i<count;++i) {
                const auto op=node->pCommandHistory[i];
                const char letter=op==3||op==4 ? 'D' : op==5 ? 'I' : op==6 ? 'C' : 0;
                if(letter) (i<done ? before : after)+=letter;
            }
            if(before.size()>160) before="..."+before.substr(before.size()-160);
            if(after.size()>40) after=after.substr(0,40)+"...";
            std::printf("D3D12 DRED:   %s|%s\n",before.c_str(),after.c_str());
            const UINT32 first=done>8 ? done-8 : 0,last=std::min(count,done+6);
            for(UINT32 i=first;i<last;++i) {
                const auto op=node->pCommandHistory[i];
                const wchar_t* context=L"";
                for(UINT32 c=0;c<node->BreadcrumbContextsCount;++c)
                    if(node->pBreadcrumbContexts[c].BreadcrumbIndex==i) context=node->pBreadcrumbContexts[c].pContextString;
                std::printf("D3D12 DRED:   %s%u %s %ls\n",i==done ? "-> " : "   ",i,op<std::size(ops) ? ops[op] : "?",context);
            }
        }
        if(!shown) std::printf("D3D12 DRED: every recorded command list completed\n");
    }
    D3D12_DRED_PAGE_FAULT_OUTPUT1 fault{};
    if(SUCCEEDED(dred->GetPageFaultAllocationOutput1(&fault))) {
        if(!fault.PageFaultVA) std::printf("D3D12 DRED: no page fault\n");
        else {
            std::printf("D3D12 DRED: page fault at GPU address 0x%llx\n",static_cast<unsigned long long>(fault.PageFaultVA));
            for(auto node=fault.pHeadExistingAllocationNode;node;node=node->pNext)
                std::printf("D3D12 DRED:   existing allocation type %d %ls\n",static_cast<int>(node->AllocationType),node->ObjectNameW ? node->ObjectNameW : L"");
            for(auto node=fault.pHeadRecentFreedAllocationNode;node;node=node->pNext)
                std::printf("D3D12 DRED:   recently freed allocation type %d %ls\n",static_cast<int>(node->AllocationType),node->ObjectNameW ? node->ObjectNameW : L"");
        }
    }
}
