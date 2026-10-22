// The Xbox shows the game at its native resolution: Dozen runs neither the temporal upscalers
// (FSR 3.1, FSR 4, TAA), frame generation nor the DLSS bridge (gpu/CMakeLists.txt leaves them
// out), so their classes do nothing here and the menu offers none of them.
#include "video_core/renderer_vulkan/vk_dlss.h"
#include "video_core/renderer_vulkan/vk_frame_generation.h"
#include "video_core/renderer_vulkan/vk_fsr4.h"
#include "video_core/renderer_vulkan/vk_temporal_upscaler.h"
#include "bbport_settings.h"

namespace Vulkan {
TemporalUpscaler::TemporalUpscaler(const Instance& i, Scheduler& s, VideoCore::TextureCache& t,
                                   Runtime& r, CameraMotion& c, SceneTargets& targets)
    : instance(i), scheduler(s), texture_cache(t), runtime(r), camera_motion(c), scene_targets(targets) {
    BbSettings::ConfigureUpscalerSupport(false, false);
    BbSettings::ConfigureDlssSupport(false, "not on the Xbox");
}
TemporalUpscaler::~TemporalUpscaler() = default;
void TemporalUpscaler::OnSceneColor(VideoCore::ImageId) {}
void TemporalUpscaler::OnBlendedSceneDraw() {}
void TemporalUpscaler::OnSceneComposite() {}
void TemporalUpscaler::OnDispatch(u64) {}
void TemporalUpscaler::OnDrawRecorded(u64, VideoCore::ImageId) {}
bool TemporalUpscaler::OnFrameStart() { return false; }
bool TemporalUpscaler::RasterScaling() const { return false; }
float TemporalUpscaler::SceneMipBias() const { return 0; }
void TemporalUpscaler::OnDraw(u64, VideoCore::ImageId, VideoCore::ImageId, bool) {}
void TemporalUpscaler::OnColorTarget(VideoCore::ImageId) {}
bool TemporalUpscaler::RedirectColor(VideoCore::ImageId, const VideoCore::ImageViewInfo&, Target&) { return false; }
bool TemporalUpscaler::RedirectDepth(VideoCore::ImageId, Target&) { return false; }
bool TemporalUpscaler::RedirectSampled(VideoCore::ImageId, const VideoCore::ImageViewInfo&,
                                       vk::ImageView&, vk::ImageLayout&) { return false; }
bool TemporalUpscaler::DisplayOverride(VAddr, Display&) { return false; }

struct Fsr4Upscaler::Impl {};
Fsr4Upscaler::Fsr4Upscaler(const Instance&, Scheduler&) {}
Fsr4Upscaler::~Fsr4Upscaler() = default;
bool Fsr4Upscaler::Record(const Frame&) { return false; }
const char* Fsr4Upscaler::Problem() const noexcept { return "not on the Xbox"; }
bool Fsr4Upscaler::Fatal() const noexcept { return true; }

FrameGenerator::FrameGenerator(const Instance& i, Scheduler& s) : instance(i), scheduler(s) {}
FrameGenerator::~FrameGenerator() = default;
bool FrameGenerator::Wanted() const { return false; }
vk::ImageView FrameGenerator::BeginCapture(vk::CommandBuffer, vk::ImageView, u32, u32) { return {}; }
void FrameGenerator::EndCapture(vk::CommandBuffer, std::array<float, 2>, bool) {}
void FrameGenerator::CaptureHudless(vk::CommandBuffer, vk::Image, vk::Format, u32, u32) {}
vk::ImageView FrameGenerator::Hudless(vk::Format, vk::Extent2D&) { return {}; }
u32 FrameGenerator::Generate(vk::CommandBuffer, vk::Image, vk::ImageView, vk::ImageUsageFlags, vk::Image,
                             vk::ImageView, u32, u32, std::span<const Output>) { return 0; }
void FrameGenerator::DumpIfRequested(vk::CommandBuffer, vk::Image, u32, u32, const char*) {}

// Callers skip DLSS when there is none.
struct Dlss::Impl {};
Dlss* Dlss::Get() { return nullptr; }
Dlss::Dlss() = default;
Dlss::~Dlss() = default;
void Dlss::AppendInstanceExtensions(std::vector<const char*>&) {}
void Dlss::AppendDeviceExtensions(vk::Instance, vk::PhysicalDevice, std::vector<const char*>&) {}
void Dlss::Initialize(vk::Instance, vk::PhysicalDevice, vk::Device) {}
void Dlss::Shutdown() {}
bool Dlss::Available() const { return false; }
std::string Dlss::Problem() const { return "not on the Xbox"; }
int Dlss::QualityForScale(float) { return 0; }
bool Dlss::HasFeature(const FeatureDesc&) const { return false; }
bool Dlss::CreateFeature(vk::CommandBuffer, const FeatureDesc&) { return false; }
bool Dlss::Evaluate(vk::CommandBuffer, const Frame&) { return false; }
void Dlss::ReleaseFeature() {}
int Dlss::FrameGenMaxCount() const { return 0; }
bool Dlss::HasFrameGen(const FrameGenDesc&) const { return false; }
bool Dlss::CreateFrameGen(vk::CommandBuffer, const FrameGenDesc&) { return false; }
bool Dlss::EvaluateFrameGen(vk::CommandBuffer, const FrameGenFrame&) { return false; }
void Dlss::ReleaseFrameGen() {}
} // namespace Vulkan
