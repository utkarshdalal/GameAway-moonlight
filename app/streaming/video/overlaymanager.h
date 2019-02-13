#pragma once

#include <QString>

namespace Overlay {

enum OverlayType {
    OverlayDebug,
    OverlayMinorNotification,
    OverlayMajorNotification,
    OverlayMax
};

class IOverlayRenderer
{
public:
    virtual ~IOverlayRenderer() = default;

    virtual void notifyOverlayUpdated(OverlayType type) = 0;
};

class OverlayManager
{
public:
    OverlayManager();

    bool isOverlayEnabled(OverlayType type);
    char* getOverlayText(OverlayType type);
    void setOverlayTextUpdated(OverlayType type);
    void setOverlayState(OverlayType type, bool enabled);

    void setOverlayRenderer(IOverlayRenderer* renderer);

    struct {
        bool enabled;
        char text[512];
    } m_Overlays[OverlayMax];
    IOverlayRenderer* m_Renderer;
};

}
