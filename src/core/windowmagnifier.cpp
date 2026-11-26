// SPDX-FileCopyrightText: 2026 PicaLens contributors
// SPDX-License-Identifier: Apache-2.0

#include "windowmagnifier.h"

#include "output/output.h"
#include "qmlengine.h"
#include "session/sessionmanager.h"
#include "surface/surfacewrapper.h"
#include "waylandserver.h"

#include <woutputitem.h>
#include <woutputrenderwindow.h>
#include <wsurface.h>
#include <wsurfaceitem.h>

#include <QKeyEvent>
#include <QMatrix4x4>
#include <QMouseEvent>
#include <QQmlComponent>
#include <QQuickItem>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <utility>

namespace {

class WindowTransform : public QQuickTransform
{
public:
    WindowTransform(QQuickItem *item, QPointF offset, qreal factor)
        : QQuickTransform(item), m_offset(offset), m_factor(factor)
    {
        appendToItem(item);
    }

    void applyTo(QMatrix4x4 *matrix) const override
    {
        matrix->translate(m_offset.x(), m_offset.y());
        matrix->scale(m_factor, m_factor);
    }

private:
    QPointF m_offset;
    qreal m_factor;
};

}

WindowMagnifier::WindowMagnifier(WaylandServer *server)
    : QObject(server)
    , m_server(server)
    , m_deadline(new QTimer(this))
{
    m_deadline->setSingleShot(true);
    connect(m_deadline, &QTimer::timeout, this, &WindowMagnifier::stop);
    connect(server->sessionManager(), &SessionManager::sessionChanged, this, &WindowMagnifier::stop);
    connect(server, &WaylandServer::currentModeChanged, this, &WindowMagnifier::stop);
    connect(server, &WaylandServer::workspaceChanged, this, &WindowMagnifier::stop);
    connect(server, &WaylandServer::activatedSurfaceChanged,
            this, &WindowMagnifier::onActivatedSurfaceChanged);
    server->window()->installEventFilter(this);
}

WindowMagnifier::~WindowMagnifier()
{
    stop();
}

bool WindowMagnifier::canMagnify(SurfaceWrapper *source) const
{
    if (m_server->currentMode() != WaylandServer::CurrentMode::Normal
        || !source || !source->isVisible() || !source->ownsOutput() || !source->parentItem()
        || (source->type() != SurfaceWrapper::Type::XdgToplevel
            && source->type() != SurfaceWrapper::Type::XWayland)
        || !source->surface() || !source->surface()->mapped()
        || !m_server->surfaceBelongsToCurrentSession(source)) {
        return false;
    }

    const auto api = m_server->window()->rendererInterface()->graphicsApi();
    if (api != QSGRendererInterface::OpenGL && api != QSGRendererInterface::Vulkan)
        return false;

    const auto *outputItem = source->ownsOutput()->outputItem();
    const auto bounds = source->mapRectToScene(QRectF(QPointF(), source->size()));
    const auto output = outputItem->mapRectToScene(outputItem->boundingRect());
    return !bounds.isEmpty() && bounds.width() < output.width() && bounds.height() < output.height();
}

void WindowMagnifier::start(SurfaceWrapper *source)
{
    stop();
    if (!canMagnify(source))
        return;

    const QPointer<SurfaceWrapper> candidate(source);
    m_server->forceActivateSurface(source, Qt::OtherFocusReason, m_server->seat());
    if (!candidate || m_server->activatedSurface() != source || !canMagnify(source))
        return;

    auto *output = source->ownsOutput();
    auto *outputItem = output->outputItem();
    const QRectF sourceRect(QPointF(), source->size());
    const auto bounds = source->mapRectToScene(sourceRect);
    m_output = outputItem->mapRectToScene(outputItem->boundingRect());
    m_zoom = std::min(m_output.width() / bounds.width(), m_output.height() / bounds.height());
    m_picture = QRectF(QPointF(), bounds.size() * m_zoom);
    m_picture.moveCenter(m_output.center());

    const auto *surface = source->surface();
    const auto buffer = surface->bufferSize();
    const auto logical = surface->size();
    if (buffer.isEmpty() || logical.isEmpty())
        return;
    const auto density = std::min(qreal(buffer.width()) / logical.width(),
                                  qreal(buffer.height()) / logical.height());
    const auto sourcePixels = (sourceRect.size() * density).toSize();
    if (sourcePixels.isEmpty())
        return;

    QQmlComponent component(m_server->qmlEngine(), "Treeland", "WindowMagnifier");
    auto *view = qobject_cast<QQuickItem *>(component.createWithInitialProperties({
        {QStringLiteral("sourceWindow"), QVariant::fromValue(source)},
        {QStringLiteral("sourceRect"), sourceRect},
        {QStringLiteral("sourcePixels"), sourcePixels},
        {QStringLiteral("picture"), m_picture.translated(-m_output.topLeft())},
    }));
    if (!view)
        return;

    m_source = source;
    m_view = view;
    view->setParent(this);
    view->setParentItem(m_server->window()->contentItem());
    view->setPosition(m_output.topLeft());
    view->setSize(m_output.size());
    view->setZ(10000);
    m_watchers = {
        connect(m_server->window(), &QQuickWindow::afterRendering, this,
                &WindowMagnifier::prepareInput, Qt::QueuedConnection),
        connect(source, &QObject::destroyed, this, &WindowMagnifier::stop),
        connect(source, &QQuickItem::visibleChanged, this, &WindowMagnifier::stop),
        connect(source, &QQuickItem::xChanged, this, &WindowMagnifier::stop),
        connect(source, &QQuickItem::yChanged, this, &WindowMagnifier::stop),
        connect(source, &QQuickItem::widthChanged, this, &WindowMagnifier::stop),
        connect(source, &QQuickItem::heightChanged, this, &WindowMagnifier::stop),
        connect(source, &SurfaceWrapper::ownsOutputChanged, this, &WindowMagnifier::stop),
        connect(output, &QObject::destroyed, this, &WindowMagnifier::stop),
        connect(output->output(), &WOutput::scaleChanged, this, &WindowMagnifier::stop),
        connect(output->output(), &WOutput::effectiveSizeChanged, this, &WindowMagnifier::stop),
        connect(outputItem, &QQuickItem::xChanged, this, &WindowMagnifier::stop),
        connect(outputItem, &QQuickItem::yChanged, this, &WindowMagnifier::stop),
    };
    m_deadline->start(3000);
    m_server->window()->update();
}

void WindowMagnifier::onActivatedSurfaceChanged()
{
    if (m_view && m_server->activatedSurface() != m_source)
        stop();
}

void WindowMagnifier::prepareInput()
{
    if (!m_view || !m_source || m_transform)
        return;
    auto *parent = m_source->parentItem();
    if (!parent) {
        stop();
        return;
    }
    if (!m_view->property("ready").toBool()) {
        m_server->window()->update();
        return;
    }

    // Match the scene item's input transform to the presented texture.
    const auto offset = parent->mapFromScene(m_picture.topLeft())
        - m_source->mapToItem(parent, QPointF());
    m_transform = new WindowTransform(m_source, offset, m_zoom);
    m_view->setProperty("captureHidden", true);
    m_deadline->stop();
}

bool WindowMagnifier::eventFilter(QObject *, QEvent *event)
{
    const auto type = event->type();
    if (type == QEvent::KeyPress || type == QEvent::KeyRelease) {
        const auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Escape) {
            if (m_swallowEscape) {
                if (type == QEvent::KeyRelease && !key->isAutoRepeat())
                    m_swallowEscape = false;
                return true;
            }
            if (m_view && type == QEvent::KeyPress && key->modifiers() == Qt::NoModifier) {
                m_swallowEscape = true;
                stop();
                return true;
            }
        }
    }
    if (!m_view)
        return false;

    if (type == QEvent::Wheel) {
        const auto pos = static_cast<QWheelEvent *>(event)->position();
        return m_output.contains(pos) && (!m_transform || !m_picture.contains(pos));
    }
    if (type == QEvent::MouseButtonPress || type == QEvent::MouseButtonDblClick
        || type == QEvent::MouseButtonRelease || type == QEvent::MouseMove) {
        const auto *mouse = static_cast<QMouseEvent *>(event);
        if (type == QEvent::MouseButtonRelease && m_pressed.testFlag(mouse->button())) {
            m_pressed.setFlag(mouse->button(), false);
            return false;
        }
        const bool visible = m_transform && m_picture.contains(mouse->position());
        if (visible && (type == QEvent::MouseButtonPress || type == QEvent::MouseButtonDblClick))
            m_pressed.setFlag(mouse->button());
        return m_output.contains(mouse->position()) && !visible && m_pressed == Qt::NoButton;
    }
    return false;
}

void WindowMagnifier::stop()
{
    m_pressed = Qt::NoButton;
    m_deadline->stop();
    for (auto connection : std::exchange(m_watchers, {}))
        disconnect(connection);
    delete m_view.data();
    delete m_transform.data();
    m_source.clear();
}
