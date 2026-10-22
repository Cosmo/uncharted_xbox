#pragma once
#include <cstdint>
namespace Vulkan { class Runtime; }
namespace VideoCore { struct Image; struct Buffer; }
namespace UnchartedUwp {
void InspectTexture(Vulkan::Runtime&,VideoCore::Image&,const VideoCore::Buffer*,std::uint64_t,const VideoCore::Buffer*,std::uint64_t);
}
