#pragma once

class Process {
  public:
    Process() = default;
    virtual ~Process() = default;
    virtual bool isRelayActive() = 0;
    virtual bool isAltRelayActive() = 0;
    virtual float getPumpValue() = 0;
    virtual void progress() = 0;
    virtual bool isActive() = 0;
    virtual bool isComplete() = 0;
    virtual int getType() = 0;
    virtual void updateVolume(double volume) = 0;
};
