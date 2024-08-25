#include "../globals.hpp"
#include "../config/config.hpp"
#include "src/config/ConfigManager.hpp"
#include "src/helpers/AnimatedVariable.hpp"
#include "src/managers/AnimationManager.hpp"
#include "src/managers/EventManager.hpp"
#include "Shake.hpp"
#include <algorithm>
#include <chrono>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/debug/Log.hpp>

CShake::CShake() {
    // the timing and the bezier are quite crucial, as things will break down if they are just changed slighly
    // this is not ideal and should be fixed some time in the future, then it may be made configurable (if it has a substatntial enough effect on behaviour)

    int time = 400;

    // add custom bezier (and readd it after config reload)
    static auto bezier = "dynamic-cursors-magnification";
    g_pAnimationManager->addBezierWithName(bezier, {0.22, 1.0}, {0.36, 1.0});
    static const auto PCALLBACK = HyprlandAPI::registerCallbackDynamic( PHANDLE, "configReloaded", [&](void* self, SCallbackInfo&, std::any data) {
        g_pAnimationManager->addBezierWithName(bezier, {0.22, 1.0}, {0.36, 1.0});
    });

    // wtf is this struct?
    static SAnimationPropertyConfig properties = {false, bezier, "", time / 100.F, 1, nullptr, nullptr };
    properties.pValues = &properties;

    zoom.create(&properties, AVARDAMAGE_NONE);
    zoom.registerVar();
    zoom.setValueAndWarp(1);
}

CShake::~CShake() {
    zoom.unregister();
}

double CShake::update(Vector2D pos) {
    static auto* const* PTHRESHOLD = (Hyprlang::FLOAT* const*) getConfig(CONFIG_SHAKE_THRESHOLD);
    static auto* const* PBASE = (Hyprlang::FLOAT* const*) getConfig(CONFIG_SHAKE_BASE);
    static auto* const* PSPEED = (Hyprlang::FLOAT* const*) getConfig(CONFIG_SHAKE_SPEED);
    static auto* const* PINFLUENCE = (Hyprlang::FLOAT* const*) getConfig(CONFIG_SHAKE_INFLUENCE);
    static auto* const* PLIMIT = (Hyprlang::FLOAT* const*) getConfig(CONFIG_SHAKE_LIMIT);
    static auto* const* PTIMEOUT = (Hyprlang::INT* const*) getConfig(CONFIG_SHAKE_TIMEOUT);

    static auto* const* PIPC = (Hyprlang::INT* const*) getConfig(CONFIG_SHAKE_IPC);

    int max = g_pHyprRenderer->m_pMostHzMonitor->refreshRate; // 1s worth of history
    samples.resize(max);
    samples_distance.resize(max);

    int previous = samples_index == 0 ? max - 1 : samples_index - 1;
    samples[samples_index] = Vector2D{pos};
    samples_distance[samples_index] = samples[samples_index].distance(samples[previous]);
    samples_index = (samples_index + 1) % max; // increase for next sample

    // The idea for this algorith was largely inspired by KDE Plasma
    // https://invent.kde.org/plasma/kwin/-/blob/master/src/plugins/shakecursor/shakedetector.cpp

    // calculate total distance travelled
    double trail = 0;
    for (double distance : samples_distance) trail += distance;

    // calculate diagonal of bounding box travelled within
    double left = 1e100, right = 0, bottom = 0, top = 1e100;
    for (Vector2D position : samples) {
        left = std::min(left, position.x);
        right = std::max(right, position.x);
        top = std::min(top, position.y);
        bottom = std::max(bottom, position.y);
    }
    double diagonal = Vector2D{left, top}.distance(Vector2D(right, bottom));

    // if diagonal sufficiently large and over threshold
    double amount = (trail / diagonal) - **PTHRESHOLD;
    if (diagonal > 100 && amount > 0) {
        float delta = 1.F / g_pHyprRenderer->m_pMostHzMonitor->refreshRate;

        float next = this->zoom.goal();

        if (!started) next = **PBASE; // start on base zoom
        next += delta * (**PSPEED + (amount * amount) * **PINFLUENCE); // increase when moving
        if (**PLIMIT > 1) next = std::min(**PLIMIT, next); // limit overall zoom

        if (next != this->zoom.goal()) this->zoom = next;
        this->end = steady_clock::now() + milliseconds(**PTIMEOUT);
        started = true;
    } else {
        if (started && end < std::chrono::steady_clock::now()) {
            this->zoom = 1;
            started = false;
        }
    }

    if (**PIPC) {
        if (started || this->zoom.value() > 1) {
            if (!ipc) {
                g_pEventManager->postEvent(SHyprIPCEvent { IPC_SHAKE_START });
                ipc = true;
            }

            g_pEventManager->postEvent(SHyprIPCEvent { IPC_SHAKE_UPDATE, std::format("{},{},{},{},{}", (int) pos.x, (int) pos.y, trail, diagonal, this->zoom.value()) });
        } else {
            if (ipc) {
                g_pEventManager->postEvent(SHyprIPCEvent { IPC_SHAKE_END });
                ipc = false;
            }
        }
    }

    return this->zoom.value();
}
