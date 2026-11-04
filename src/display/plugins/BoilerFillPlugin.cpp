#include "BoilerFillPlugin.h"
#include <WiFi.h>
#include <display/core/Controller.h>
#include <display/core/Event.h>
#include <display/core/process/PressureFillProcess.h>
#include <display/core/process/PumpProcess.h>

bool BoilerFillPlugin::usePressureFill() const {
    if (controller == nullptr)
        return false;
    const auto &settings = controller->getSettings();
    const auto caps = controller->getSystemInfo().capabilities;
    return settings.isBoilerFillPressureMode() && caps.pressure && !caps.dualBoiler;
}

void BoilerFillPlugin::startFillProcess(bool startup) {
    auto &settings = controller->getSettings();
    if (usePressureFill()) {
        const unsigned long holdMs = static_cast<unsigned long>(settings.getBoilerFillHoldTime() * 1000.0f);
        const unsigned long maxMs = static_cast<unsigned long>(settings.getBoilerFillMaxTime()) * 1000UL;
        controller->startProcess(new PressureFillProcess([this]() { return this->controller->getCurrentPressure(); },
                                                         settings.getBoilerFillPressure(), holdMs, maxMs));
        return;
    }

    const int duration = startup ? settings.getStartupFillTime() : settings.getSteamFillTime();
    controller->startProcess(new PumpProcess(duration));
}

void BoilerFillPlugin::setup(Controller *controller, PluginManager *pluginManager) {
    this->controller = controller;
    pluginManager->on("controller:ready", [this](Event const &) {
        if (this->controller->getSettings().getStartupFillTime() == 0)
            return;
        this->startFillProcess(true);
    });
    pluginManager->on("controller:mode:change", [this](Event const &event) {
        // Event fires before mode is assigned, so getMode() is still the previous mode.
        const int newMode = event.getInt("value");
        if (this->controller->getMode() != MODE_STEAM)
            return;
        if (newMode == MODE_STEAM)
            return;

        if (this->usePressureFill()) {
            // Defer until idle outside steam/standby and below wait temperature.
            // Also covers steam → standby, which the time-based path skipped.
            this->steamFillPending = true;
            return;
        }

        if (newMode != MODE_STANDBY)
            this->startFillProcess(false);
    });
}

void BoilerFillPlugin::loop() {
    if (!steamFillPending || controller == nullptr)
        return;
    if (!usePressureFill()) {
        steamFillPending = false;
        return;
    }

    const int mode = controller->getMode();
    if (mode == MODE_STEAM || mode == MODE_STANDBY)
        return;
    if (!controller->isReady() || controller->isActive())
        return;

    const float waitTemp = controller->getSettings().getBoilerFillWaitTemp();
    if (waitTemp > 0.0f && controller->getCurrentTemp() >= waitTemp)
        return;

    steamFillPending = false;
    startFillProcess(false);
}
