#include "input.h"
#include <windows.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>

namespace {
using winrt::Windows::Gaming::Input::GamepadButtons;
using winrt::Windows::Gaming::Input::GamepadReading;
constexpr uint32_t PAD_L3=0x2, PAD_TOUCHPAD=0x100000;
// With View held: the left stick deflection that puts the finger on the touchpad (as in KytyPS5).
constexpr double TOUCH_START=0.25;

void Apartment() {
    thread_local bool initialized=false;
    if(!initialized) {
        try {winrt::init_apartment(winrt::apartment_type::multi_threaded);}
        catch(winrt::hresult_error const& e) {if(e.code()!=RPC_E_CHANGED_MODE) throw;}
        initialized=true;
    }
}
bool Held(const GamepadReading& reading,GamepadButtons button) {
    return (reading.Buttons&button)!=GamepadButtons::None;
}
std::optional<GamepadReading> Read() {
    try {
        Apartment();
        auto pads=winrt::Windows::Gaming::Input::Gamepad::Gamepads();
        if(!pads.Size()) return std::nullopt;
        return pads.GetAt(0).GetCurrentReading();
    } catch(winrt::hresult_error const& e) {
        static bool reported=false;
        if(!reported) {std::fprintf(stderr,"Xbox gamepad API: 0x%08x\n",static_cast<unsigned>(e.code()));reported=true;}
        return std::nullopt;
    }
}
// The PS4 pad's buttons and axes. View is no button of the game: callers decide what it does.
void Map(const GamepadReading& reading,uint32_t* buttons,uint8_t axes[6]) {
    using B=GamepadButtons;
    struct Mapping {B host;uint32_t guest;};
    constexpr Mapping map[]={
        {B::A,0x4000},{B::B,0x2000},{B::X,0x8000},{B::Y,0x1000},
        {B::LeftShoulder,0x400},{B::RightShoulder,0x800},
        {B::LeftThumbstick,PAD_L3},{B::RightThumbstick,4},{B::Menu,8},
        {B::DPadUp,0x10},{B::DPadRight,0x20},{B::DPadDown,0x40},{B::DPadLeft,0x80}};
    *buttons=0;
    for(auto const& entry:map) if(Held(reading,entry.host)) *buttons|=entry.guest;
    auto axis=[](double value){return static_cast<uint8_t>(std::clamp((value+1)*127.5,0.0,255.0)+0.5);};
    axes[0]=axis(reading.LeftThumbstickX); axes[1]=axis(-reading.LeftThumbstickY);
    axes[2]=axis(reading.RightThumbstickX); axes[3]=axis(-reading.RightThumbstickY);
    axes[4]=static_cast<uint8_t>(std::clamp(reading.LeftTrigger*255,0.0,255.0));
    axes[5]=static_cast<uint8_t>(std::clamp(reading.RightTrigger*255,0.0,255.0));
    if(axes[4]>30) *buttons|=0x100;
    if(axes[5]>30) *buttons|=0x200;
}
} // namespace

// KytyPS5's View scheme: View + left stick is a finger on the touchpad where the stick points,
// from when it moves (or clicks) until View is released, and the stick's click is the touchpad
// click; the sticks do not reach the game while View is held. View + Menu opens the port's menu
// (window.cpp): the game gets nothing until both are released.
extern "C" int uncharted_uwp_gamepad(uint32_t* buttons,uint8_t axes[6],float touch[2]) {
    static bool chord=false, touching=false;
    if(touch) touch[0]=touch[1]=-1.0f;
    const auto reading=Read();
    if(!reading) return 0;
    Map(*reading,buttons,axes);
    const bool view=Held(*reading,GamepadButtons::View), menu=Held(*reading,GamepadButtons::Menu);
    if(view&&menu) chord=true;
    if(chord&&!view&&!menu) chord=false;
    if(chord) {
        *buttons=0;
        std::fill_n(axes,4,uint8_t{128}); axes[4]=axes[5]=0;
    }
    if(chord||!view) {
        touching=false;
        return 1;
    }
    const double x=std::clamp(reading->LeftThumbstickX,-1.0,1.0), y=std::clamp(reading->LeftThumbstickY,-1.0,1.0);
    const bool click=Held(*reading,GamepadButtons::LeftThumbstick);
    if(click||std::hypot(x,y)>TOUCH_START) touching=true;
    *buttons&=~PAD_L3;
    if(click) *buttons|=PAD_TOUCHPAD;
    if(touching&&touch) {touch[0]=static_cast<float>((x+1)/2); touch[1]=static_cast<float>((1-y)/2);}
    std::fill_n(axes,4,uint8_t{128});
    return 1;
}
extern "C" int uncharted_uwp_dialog_gamepad(uint32_t* buttons,uint8_t axes[6]) {
    const auto reading=Read();
    if(!reading) return 0;
    Map(*reading,buttons,axes);
    if(Held(*reading,GamepadButtons::View)) *buttons|=UNCHARTED_UWP_VIEW;
    return 1;
}
extern "C" void uncharted_uwp_vibration(uint8_t large,uint8_t small) {
    try {
        Apartment(); auto pads=winrt::Windows::Gaming::Input::Gamepad::Gamepads();
        if(pads.Size()) pads.GetAt(0).Vibration({large/255.0,small/255.0,0,0});
    } catch(winrt::hresult_error const&) {}
}
