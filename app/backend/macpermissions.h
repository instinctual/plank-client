#pragma once

#include <QObject>
#include <QVariantList>
#include <functional>

// Launcher-only permission status/actions. These are not forwarding switches.
class MacPermissions : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList rows READ rows NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
public:
    explicit MacPermissions(std::function<bool()> canConfigure, QObject* parent = nullptr);
    QVariantList rows() const { return m_Rows; }
    QString error() const { return m_Error; }
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void request(int index);
signals:
    void changed();
private:
    std::function<bool()> m_CanConfigure;
    QVariantList m_Rows;
    QString m_Error;
    bool m_Requesting = false;
};
