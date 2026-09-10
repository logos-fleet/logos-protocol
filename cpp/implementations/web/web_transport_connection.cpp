#include "web_transport_connection.h"

#include "web_rpc_connection.h"

#include "plain_logos_object.h"

#include <QDebug>

#include <utility>

namespace logos::web {

WebTransportConnection::WebTransportConnection(MessageChannelPtr channel)
    : m_channel(std::move(channel))
{
}

WebTransportConnection::~WebTransportConnection()
{
    if (m_conn) m_conn->stop("connection destroyed");
}

bool WebTransportConnection::connectToHost()
{
    if (m_connected) return true;
    if (!m_channel) {
        qWarning() << "WebTransportConnection: no message channel — this process "
                      "has no web bridge installed (see logos::web::"
                      "setMessageChannelFactory)";
        return false;
    }
    if (!m_channel->isOpen()) {
        qWarning() << "WebTransportConnection: the message channel is closed";
        return false;
    }
    auto conn = std::make_shared<WebRpcConnection>(m_channel, nullptr);
    conn->start();
    m_conn = std::move(conn);
    m_connected = true;
    return true;
}

bool WebTransportConnection::isConnected() const
{
    return m_connected && m_conn && m_conn->isOpen();
}

bool WebTransportConnection::reconnect()
{
    if (m_conn) m_conn->stop("reconnecting");
    m_conn.reset();
    m_connected = false;
    return connectToHost();
}

LogosObject* WebTransportConnection::requestObject(const QString& objectName,
                                                   int /*timeoutMs*/)
{
    if (!isConnected()) return nullptr;
    // No liveness probe, exactly as on the plain transport: the handle is
    // constructed over an open conversation and "the module isn't there" shows
    // up as a MODULE_NOT_LOADED result at call time rather than as an acquire
    // failure. Callers of both transports already handle it that way.
    return new logos::plain::PlainLogosObject(objectName.toStdString(), m_conn);
}

QString WebTransportConnection::endpointUrl(const QString& /*instanceId*/,
                                            const QString& /*moduleName*/)
{
    // Mirror of WebTransportHost::bindUrl: the web transport is reached over an
    // injected channel, never by dialing an address.
    return QStringLiteral("web://");
}

} // namespace logos::web
