// SPDX-FileCopyrightText: 2026 PicaLens contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <QObject>
#include <QPointer>
#include <QRectF>

class QQuickItem;
class QQuickTransform;
class QTimer;
class SurfaceWrapper;
class WaylandServer;

class WindowMagnifier : public QObject
{
public:
    explicit WindowMagnifier(WaylandServer *server);
    ~WindowMagnifier() override;

    bool canMagnify(SurfaceWrapper *source) const;
    void start(SurfaceWrapper *source);

protected:
    bool eventFilter(QObject *object, QEvent *event) override;

private:
    void onActivatedSurfaceChanged();
    void prepareInput();
    void stop();

    WaylandServer *m_server;
    QTimer *m_deadline;
    QPointer<SurfaceWrapper> m_source;
    QPointer<QQuickItem> m_view;
    QPointer<QQuickTransform> m_transform;
    QList<QMetaObject::Connection> m_watchers;
    QRectF m_output;
    QRectF m_picture;
    qreal m_zoom = 1.;
    Qt::MouseButtons m_pressed = Qt::NoButton;
    bool m_swallowEscape = false;
};
