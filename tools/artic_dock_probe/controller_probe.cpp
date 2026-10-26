// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version

#include <chrono>
#include <iostream>
#include <thread>
#include <SDL.h>
#include "common/logging/log.h"
#include "common/logging/backend.h"
#include "common/param_package.h"
#include "core/frontend/input.h"
#include "input_common/main.h"

int main(int argc, char** argv) {
    Common::Log::Initialize();
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_JOYSTICK) != 0) return 1;
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        char guid[33]{};
        SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(i), guid, sizeof(guid));
        std::cout << "device=" << i << " name=" << SDL_JoystickNameForIndex(i)
                  << " controller=" << SDL_IsGameController(i) << " guid=" << guid << '\n';
    }
    if (argc < 2 || std::string_view(argv[1]) != "--virtual") {
        SDL_Quit();
        return 0;
    }
    const int index = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 6, 16, 1);
    if (index < 0) return 1;
    SDL_Joystick* joystick = SDL_JoystickOpen(index);
    if (!joystick) return 1;
    char guid[33]{};
    SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(joystick), guid, sizeof(guid));
    InputCommon::Init();
    Common::ParamPackage params;
    params.Set("engine", "sdl");
    params.Set("guid", guid);
    params.Set("port", 0);
    params.Set("button", 0);
    auto button = Input::CreateDevice<Input::ButtonDevice>(params.Serialize());
    const auto wait_for = [&](bool expected) {
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            SDL_JoystickUpdate();
            if (button->GetStatus() == expected) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    };
    SDL_JoystickSetVirtualButton(joystick, 0, 1);
    const bool pressed = wait_for(true);
    SDL_JoystickSetVirtualButton(joystick, 0, 0);
    const bool released = wait_for(false);
    button.reset();
    InputCommon::Shutdown();
    SDL_JoystickClose(joystick);
    SDL_JoystickDetachVirtual(index);
    SDL_Quit();
    std::cout << "virtual_press=" << pressed << " virtual_release=" << released << '\n';
    return pressed && released ? 0 : 1;
}
