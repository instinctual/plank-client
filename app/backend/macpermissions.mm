#include "macpermissions.h"
#include "../streaming/audio/macmicrophonepermission.h"
#include "../streaming/input/macrawwacom.h"
#include <QDesktopServices>
#include <QPointer>
#include <QThread>
#include <QUrl>
#import <ApplicationServices/ApplicationServices.h>
#import <IOKit/hidsystem/IOHIDLib.h>

MacPermissions::MacPermissions(std::function<bool()> canConfigure, QObject* parent)
    : QObject(parent), m_CanConfigure(std::move(canConfigure))
{
}

void MacPermissions::refresh()
{
    Q_ASSERT(QThread::currentThread() == thread());
    const bool accessibility = AXIsProcessTrusted();
    const int microphone = plankMacMicrophonePermission();
    const int tablet = MacRawWacomInput::supportedTabletPresence();
    const bool input = IOHIDCheckAccess(kIOHIDRequestTypeListenEvent) == kIOHIDAccessTypeGranted;
    auto row = [](QString name, QString detail, bool verified, QString status, bool actionable) {
        return QVariantMap{{"name", name}, {"detail", detail}, {"verified", verified},
                           {"status", status}, {"actionable", actionable}};
    };
    m_Rows = {
        row(tr("Accessibility"), tr("Capture system shortcuts in the focused stream."), accessibility,
            accessibility ? tr("Allowed") : tr("Not allowed"), true),
        row(tr("Microphone"), tr("Forward microphone audio when enabled."), microphone == 1,
            microphone == 1 ? tr("Allowed") : microphone == 0 ? tr("Not requested") : tr("Not allowed"), true),
        row(tr("Input Monitoring"), tr("Forward a supported USB Wacom tablet."), input,
            input ? tr("Allowed") : tablet == 1 ? tr("Not allowed") : tablet == 0 ?
                tr("No supported tablet attached") : tr("Could not check tablet"), input || tablet == 1)
    };
    emit changed();
}

void MacPermissions::request(int index)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (index < 0 || index > 2 || m_Requesting) return;
    if (!m_CanConfigure || !m_CanConfigure()) {
        m_Error = tr("Disconnect the stream before changing permissions.");
        emit changed(); return;
    }
    m_Error.clear();
    if (index == 1 && plankMacMicrophonePermission() == 0) {
        m_Requesting = true;
        plankMacRequestMicrophonePermission([self = QPointer<MacPermissions>(this)] {
            if (!self) return;
            self->m_Requesting = false;
            self->refresh();
        });
        return;
    }
    if (index == 2) MacRawWacomInput::requestPermissionIfNeeded();
    const char* panes[] = {"Privacy_Accessibility", "Privacy_Microphone", "Privacy_ListenEvent"};
    if (!QDesktopServices::openUrl(QUrl(QStringLiteral("x-apple.systempreferences:com.apple.preference.security?") +
                                       QString::fromLatin1(panes[index])))) {
        m_Error = tr("Open System Settings → Privacy & Security manually.");
    }
    refresh();
}
