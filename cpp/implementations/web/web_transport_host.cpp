#include "web_transport_host.h"

#include "web_rpc_connection.h"

#include "qvariant_rpc_value.h"
#include "json_mapping.h"

#include "../../module_proxy.h"

#include <QDebug>
#include <QJsonArray>
#include <QMetaObject>

#include <algorithm>
#include <set>
#include <utility>

namespace logos::web {

using logos::plain::CallMessage;
using logos::plain::EventMessage;
using logos::plain::MethodsMessage;
using logos::plain::MethodsResultMessage;
using logos::plain::ResultMessage;
using logos::plain::SubscribeMessage;
using logos::plain::TokenMessage;
using logos::plain::UnsubscribeMessage;

WebTransportHost::WebTransportHost() = default;

WebTransportHost::~WebTransportHost()
{
    // Move everything out under the lock and tear it down with the lock
    // RELEASED: stopping a connection runs RpcPeer::fail(), which calls back
    // into onConnectionClosed() to drop that connection's sinks — and that
    // re-takes m_mu. The plain host self-deadlocked on exactly this shape.
    decltype(m_published) published;
    decltype(m_connections) connections;
    {
        std::lock_guard<std::mutex> g(m_mu);
        published = std::move(m_published);
        connections = std::move(m_connections);
        m_sinks.clear();
    }
    for (auto& [name, pub] : published) QObject::disconnect(pub.eventConn);
    // stop() drops each connection's channel receiver and waits for an
    // in-flight delivery, so once this loop returns no channel thread is still
    // inside this host. That is the web transport's equivalent of the plain
    // host's I/O-drain barrier, and it is a wait rather than a hope because
    // IMessageChannel::setReceiver(nullptr) is defined to be one.
    for (auto& conn : connections) conn->stop("host destroyed");
}

bool WebTransportHost::attachChannel(MessageChannelPtr channel)
{
    if (!channel || !channel->isOpen()) return false;
    auto conn = std::make_shared<WebRpcConnection>(std::move(channel), this);
    std::vector<std::shared_ptr<WebRpcConnection>> dead;
    {
        std::lock_guard<std::mutex> g(m_mu);
        // Prune here rather than from an error handler. A webview that reloads
        // hands the host a fresh channel and abandons the old peer, so without
        // this the vector grows for the life of the host. Doing it on attach
        // keeps the drop OUT of the teardown path: erasing a connection from
        // inside its own fail() would release the last reference to an object
        // whose member function is still on the stack.
        auto stillOpen = [](const std::shared_ptr<WebRpcConnection>& c) {
            return c->isOpen();
        };
        for (auto& c : m_connections) if (!stillOpen(c)) dead.push_back(c);
        m_connections.erase(
            std::remove_if(m_connections.begin(), m_connections.end(),
                           [&](const std::shared_ptr<WebRpcConnection>& c) {
                               return !stillOpen(c);
                           }),
            m_connections.end());
        m_connections.push_back(conn);
    }
    // Released with m_mu dropped: destroying a peer touches its channel, and a
    // channel teardown waits on a delivery that may be inside this host.
    dead.clear();
    // Started with the lock RELEASED: start() installs a receiver, and a
    // message already queued on the channel can be delivered the instant it is
    // installed — into onCall/onSubscribe, which take m_mu.
    conn->start();
    return true;
}

std::size_t WebTransportHost::subscriberCount(const std::string& object,
                                              const std::string& eventName) const
{
    std::lock_guard<std::mutex> g(m_mu);
    auto objIt = m_sinks.find(object);
    if (objIt == m_sinks.end()) return 0;
    auto evtIt = objIt->second.find(eventName);
    if (evtIt == objIt->second.end()) return 0;
    return evtIt->second.size();
}

QString WebTransportHost::bindUrl(const QString& /*instanceId*/,
                                  const QString& /*moduleName*/)
{
    // A web host is not addressable by URL: it is reached over a channel
    // somebody handed it, never by dialing. The scheme is returned rather than
    // an empty string so a caller logging the endpoint says something true.
    return QStringLiteral("web://");
}

bool WebTransportHost::publishObject(const QString& name, QObject* object)
{
    if (!object) return false;
    auto* proxy = qobject_cast<ModuleProxy*>(object);
    if (!proxy) {
        qWarning() << "WebTransportHost::publishObject: expected ModuleProxy for"
                   << name << "(the web transport only publishes ModuleProxy)";
        return false;
    }

    std::lock_guard<std::mutex> g(m_mu);
    Published pub;
    pub.object = object;
    const std::string stdName = name.toStdString();

    pub.eventConn = QObject::connect(proxy, &ModuleProxy::eventResponse,
        [this, stdName](const QString& eventName, const QVariantList& data) {
            EventMessage msg;
            msg.object    = stdName;
            msg.eventName = eventName.toStdString();
            msg.data      = logos::plain::qvariantListToRpcList(data);
            fanOutEvent(stdName, std::move(msg));
        });

    m_published[stdName] = std::move(pub);
    return true;
}

void WebTransportHost::unpublishObject(const QString& name)
{
    std::lock_guard<std::mutex> g(m_mu);
    auto it = m_published.find(name.toStdString());
    if (it == m_published.end()) return;
    QObject::disconnect(it->second.eventConn);
    m_published.erase(it);
}

void WebTransportHost::fanOutEvent(const std::string& name, EventMessage msg)
{
    std::vector<EventSink> sinks;
    {
        std::lock_guard<std::mutex> g(m_mu);
        auto it = m_sinks.find(name);
        if (it == m_sinks.end()) return;
        // ONE COPY PER CONNECTION: a connection subscribed both by name and by
        // wildcard matches twice here, and both of its sinks do the same thing
        // (write this message down that channel), so pushing both would deliver
        // the event twice. The consumer decides which of its own handles a
        // delivery goes to — see RpcPeer::sendSubscribe.
        std::set<const void*> seen;
        for (auto which : {msg.eventName, std::string{}}) {
            auto evtIt = it->second.find(which);
            if (evtIt == it->second.end()) continue;
            for (auto& [key, sink] : evtIt->second)
                if (seen.insert(key).second) sinks.push_back(sink);
            if (msg.eventName.empty()) break;   // named IS wildcard here
        }
    }
    for (auto& sink : sinks) {
        try { sink(msg); } catch (...) {}
    }
}

void WebTransportHost::onCall(const CallMessage& req, CallReply reply)
{
    QObject* obj = nullptr;
    {
        std::lock_guard<std::mutex> g(m_mu);
        auto it = m_published.find(req.object);
        if (it != m_published.end()) obj = it->second.object;
    }
    if (!obj) {
        ResultMessage res; res.id = req.id; res.ok = false;
        res.err = "object not published: " + req.object;
        res.errCode = "MODULE_NOT_LOADED";
        reply(std::move(res));
        return;
    }

    const QString      authToken  = QString::fromStdString(req.authToken);
    const QString      methodName = QString::fromStdString(req.method);
    const QVariantList args       = logos::plain::rpcListToQVariantList(req.args);
    const uint64_t     id         = req.id;

    // The wire this call arrived on, so the provider can enforce local_only
    // tokens: a token minted for the in-process door must not be usable from a
    // webview, and this label is the only thing that can tell ModuleProxy the
    // difference. It is a constant because a WebTransportHost only ever serves
    // this one wire.
    const QString transportProtocol = QStringLiteral("web");

    QMetaObject::invokeMethod(obj, [obj, authToken, methodName, args, transportProtocol, id, reply]() {
        QVariant ret;
        const bool ok = QMetaObject::invokeMethod(obj, "callRemoteMethod",
                                                  Qt::DirectConnection,
                                                  Q_RETURN_ARG(QVariant, ret),
                                                  Q_ARG(QString, authToken),
                                                  Q_ARG(QString, methodName),
                                                  Q_ARG(QVariantList, args),
                                                  Q_ARG(QString, transportProtocol));
        ResultMessage res;
        res.id = id;
        if (ok) {
            res.ok = true;
            res.value = logos::plain::qvariantToRpcValue(ret);
        } else {
            res.ok = false;
            res.err = "callRemoteMethod failed";
            res.errCode = "METHOD_FAILED";
        }
        reply(std::move(res));
    }, Qt::QueuedConnection);
}

void WebTransportHost::onMethods(const MethodsMessage& req, MethodsReply reply)
{
    QObject* obj = nullptr;
    {
        std::lock_guard<std::mutex> g(m_mu);
        auto it = m_published.find(req.object);
        if (it != m_published.end()) obj = it->second.object;
    }
    if (!obj) {
        MethodsResultMessage res; res.id = req.id; res.ok = false;
        res.err = "object not published";
        reply(std::move(res));
        return;
    }
    const uint64_t id = req.id;
    QMetaObject::invokeMethod(obj, [obj, id, reply]() {
        QJsonArray arr;
        QMetaObject::invokeMethod(obj, "getPluginMethods",
                                  Qt::DirectConnection,
                                  Q_RETURN_ARG(QJsonArray, arr));
        MethodsResultMessage res;
        res.id = id;
        res.ok = true;
        res.methods = logos::plain::methodsFromJsonArray(arr);
        reply(std::move(res));
    }, Qt::QueuedConnection);
}

void WebTransportHost::onSubscribe(const SubscribeMessage& req, EventSink sink,
                                   const void* connectionId)
{
    std::lock_guard<std::mutex> g(m_mu);
    // Recorded whether or not `req.object` is published yet — same contract as
    // the plain host: a consumer subscribes exactly when the object is most
    // likely to be missing, and refusing here loses the subscription with
    // nobody upstream told to retry.
    m_sinks[req.object][req.eventName][connectionId] = std::move(sink);
}

void WebTransportHost::onUnsubscribe(const UnsubscribeMessage& req,
                                     const void* connectionId)
{
    std::lock_guard<std::mutex> g(m_mu);
    auto it = m_sinks.find(req.object);
    if (it == m_sinks.end()) return;
    auto evtIt = it->second.find(req.eventName);
    if (evtIt == it->second.end()) return;
    // Only the requesting connection's sink: other peers subscribed to the same
    // (object, event) keep theirs.
    evtIt->second.erase(connectionId);
    if (evtIt->second.empty()) it->second.erase(evtIt);
    if (it->second.empty()) m_sinks.erase(it);
}

void WebTransportHost::onConnectionClosed(const void* connectionId)
{
    std::lock_guard<std::mutex> g(m_mu);
    // A webview that navigates away never sends Unsubscribe; without this sweep
    // its sinks stay in the table forever and every emission writes into them.
    for (auto objIt = m_sinks.begin(); objIt != m_sinks.end(); ) {
        for (auto evtIt = objIt->second.begin(); evtIt != objIt->second.end(); ) {
            evtIt->second.erase(connectionId);
            if (evtIt->second.empty()) evtIt = objIt->second.erase(evtIt);
            else ++evtIt;
        }
        if (objIt->second.empty()) objIt = m_sinks.erase(objIt);
        else ++objIt;
    }
}

void WebTransportHost::onToken(const TokenMessage& req)
{
    QObject* obj = nullptr;
    {
        std::lock_guard<std::mutex> g(m_mu);
        // Route to the module named in the message when we host it; otherwise
        // the first published object, matching the plain host's behaviour for
        // the single-published-object provider pattern.
        auto it = m_published.find(req.moduleName);
        if (it != m_published.end()) obj = it->second.object;
        else if (!m_published.empty()) obj = m_published.begin()->second.object;
    }
    if (!obj) return;
    const QString authToken  = QString::fromStdString(req.authToken);
    const QString moduleName = QString::fromStdString(req.moduleName);
    const QString token      = QString::fromStdString(req.token);
    QMetaObject::invokeMethod(obj, "informModuleToken",
                              Qt::QueuedConnection,
                              Q_ARG(QString, authToken),
                              Q_ARG(QString, moduleName),
                              Q_ARG(QString, token));
}

} // namespace logos::web
