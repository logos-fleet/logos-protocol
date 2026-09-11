#include "local_transport.h"
#include "../../logos_async_dispatch.h"
#include "../../plugin_registry.h"
#include "../../module_proxy.h"
#include <QDebug>
#include <QEventLoop>
#include <QHash>
#include <QMetaObject>
#include <QPointer>
#include <QTimer>
#include <memory>
#include <string>
#include <utility>

// ── LocalLogosObject ─────────────────────────────────────────────────────────

namespace {

class EventHelper : public QObject {
    Q_OBJECT
public:
    explicit EventHelper(QObject* parent = nullptr) : QObject(parent) {}

    void addCallback(const QString& eventName, LogosObject::EventCallback cb) {
        m_callbacks[eventName].append(std::move(cb));
    }

public slots:
    void onEventResponse(const QString& eventName, const QVariantList& data) {
        // Dispatch to callbacks registered for this specific event name,
        // plus any wildcard subscribers (callbacks registered with an
        // empty event name, meaning "receive every event").
        auto cbs = m_callbacks.value(eventName);
        cbs.append(m_callbacks.value(QString()));
        if (!cbs.isEmpty()) {
            qDebug() << "[LogosObject] Local EventHelper: dispatching event" << eventName << "to" << cbs.size() << "callback(s)";
        }
        for (const auto& cb : cbs) {
            try { cb(eventName, data); } catch (...) {}
        }
    }

private:
    QHash<QString, QList<LogosObject::EventCallback>> m_callbacks;
};

// The deferred-completion ledger for one handle, keyed by call id: the results
// that have landed, and the waiters for the ones that have not.
//
// A STANDALONE OBJECT, HELD BY shared_ptr, rather than members of the handle,
// and that is a lifetime decision rather than a tidiness one. Every lambda that
// touches this state is posted to the event loop with the EventHelper as its
// context — the completion sink, the async delivery hop, the async deadline —
// so each of them can still be pending when the handle they came from is
// destroyed. Capturing the handle would make every one of those a
// use-after-free waiting for the right interleaving; capturing this instead
// means the ledger simply outlives the handle by however long the last posted
// lambda takes to run.
//
// Touched only on the consumer's own thread: ModuleProxy queues every emission
// onto the thread it lives on, which is the thread that acquired this handle.
struct CompletionLedger {
    QHash<QString, QVariant> values;
    QHash<QString, QEventLoop*> syncWaiters;
    QHash<QString, LogosObjectErrorChannel::AsyncResultErrorCallback> asyncWaiters;
};

using LedgerPtr = std::shared_ptr<CompletionLedger>;

} // anonymous namespace

class LocalLogosObject : public LogosObject, public LogosObjectErrorChannel {
public:
    // objectName is carried purely so a failure can name the module it belongs
    // to (logos::CallError::origin).
    LocalLogosObject(ModuleProxy* proxy, QString objectName)
        : m_proxy(proxy), m_helper(nullptr), m_objectName(std::move(objectName))
    {
        qDebug() << "[LogosObject] Created LocalLogosObject wrapping ModuleProxy" << reinterpret_cast<quintptr>(proxy);
        // Eager, exactly as RemoteLogosObject wires its own: a DEFERRED call's
        // result arrives as a completion event, so the channel has to be live
        // even for a caller that never subscribes to a user event.
        ensureCompletionChannel();
    }

    ~LocalLogosObject() override {
        qDebug() << "[LogosObject] Destroying LocalLogosObject" << reinterpret_cast<quintptr>(m_proxy.data());
        // release() normally takes the helper down first (deferred). Reaching
        // here on a direct delete, defer it too: a direct delete can still be
        // reached from inside the helper's own slot dispatch, now that a
        // deferred call's completion is delivered through it. See
        // disconnectEvents().
        disconnectEvents();
    }

    // Adapters over the error-carrying implementations: they discard the
    // diagnosis, which is exactly what these entry points have always done.
    QVariant callMethod(const QString& authToken,
                        const QString& methodName,
                        const QVariantList& args,
                        int timeoutMs) override
    {
        return callMethodWithError(authToken, methodName, args, timeoutMs, nullptr);
    }

    void callMethodAsync(const QString& authToken,
                         const QString& methodName,
                         const QVariantList& args,
                         int timeoutMs,
                         AsyncResultCallback callback) override
    {
        if (!callback) return;
        callMethodAsyncWithError(authToken, methodName, args, timeoutMs,
            [cb = std::move(callback)](QVariant v, const logos::CallError&) mutable {
                cb(std::move(v));
            });
    }

    // In-process direct dispatch: there is no wire to drop, so a vanished
    // ModuleProxy and a deferred result that never completes are the only two
    // failures this transport has.
    //
    // THE DEADLINE IS NOT DEAD WEIGHT, which is what `int /*timeoutMs*/` used
    // to say it was. A provider may answer the PENDING SENTINEL instead of the
    // result (logos_async_dispatch.h), and then the call really does have a
    // deadline: the completion arrives later, over the event channel, and may
    // not arrive at all.
    QVariant callMethodWithError(const QString& authToken,
                                 const QString& methodName,
                                 const QVariantList& args,
                                 int timeoutMs,
                                 logos::CallError* err) override
    {
        if (err) err->clear();
        if (!m_proxy) {
            if (err) *err = proxyGoneError();
            return QVariant();
        }
        qDebug() << "[LogosObject] LocalLogosObject::callMethod" << methodName << "args:" << args.size();
        // Before the dispatch, never after: the provider may complete from its
        // own thread the instant callRemoteMethod returns, and a channel armed
        // afterwards would have missed the emission.
        ensureCompletionChannel();
        return resolveDeferred(m_proxy->callRemoteMethod(authToken, methodName, args),
                               timeoutMs, methodName, err);
    }

    void callMethodAsyncWithError(const QString& authToken,
                                  const QString& methodName,
                                  const QVariantList& args,
                                  int timeoutMs,
                                  AsyncResultErrorCallback callback) override
    {
        if (!callback) return;
        if (!m_proxy) {
            const logos::CallError e = proxyGoneError();
            QTimer::singleShot(0, [callback, e]() { callback(QVariant(), e); });
            return;
        }
        ensureCompletionChannel();
        // Nothing below captures `this`: the helper is the context, the ledger
        // is shared, and the proxy and origin are copied — so a handle
        // destroyed while any of these is still posted takes nothing with it.
        EventHelper* helper = m_helper;
        QPointer<ModuleProxy> proxy = m_proxy;
        const std::string origin = m_objectName.toStdString();
        QTimer::singleShot(0, helper,
            [helper, proxy, ledger = m_ledger, origin,
             authToken, methodName, args, timeoutMs, callback]() {
                if (!proxy) {
                    callback(QVariant(), logos::callErrorObjectUnavailable(
                        origin, "module '" + origin + "' is no longer registered locally"));
                    return;
                }
                const QVariant result = proxy->callRemoteMethod(authToken, methodName, args);
                QString callId;
                if (!logos::isPendingCallSentinel(result, &callId)) {
                    callback(result, logos::CallError{});
                    return;
                }
                // The completion can already have landed: the provider's worker
                // runs while this lambda does, and ModuleProxy queues the
                // emission onto this very thread.
                if (ledger->values.contains(callId)) {
                    callback(ledger->values.take(callId), logos::CallError{});
                    return;
                }
                ledger->asyncWaiters.insert(callId, callback);
                // Bound the wait: a completion that never lands is a timeout,
                // reported as one instead of as an empty result.
                const int budget = deferredBudget(timeoutMs);
                QTimer::singleShot(budget, helper,
                    [ledger, callId, origin, method = methodName.toStdString(), budget]() {
                        if (!ledger->asyncWaiters.contains(callId)) return;
                        auto cb = ledger->asyncWaiters.take(callId);
                        if (cb)
                            cb(QVariant(), logos::callErrorTimeout(origin, method, budget));
                    });
            });
    }

    bool informModuleToken(const QString& authToken,
                           const QString& moduleName,
                           const QString& token,
                           int /*timeoutMs*/) override
    {
        if (!m_proxy) return false;
        return m_proxy->informModuleToken(authToken, moduleName, token);
    }

    void onEvent(const QString& eventName, EventCallback callback) override
    {
        if (!m_proxy) return;

        qDebug() << "[LogosObject] LocalLogosObject::onEvent subscribing to event:" << eventName;
        ensureCompletionChannel();
        m_helper->addCallback(eventName, std::move(callback));
    }

    void disconnectEvents() override
    {
        if (!m_helper) return;
        // Deferred, not inline, for the reason RemoteLogosObject spells out:
        // this can be reached SYNCHRONOUSLY from inside the helper's own slot
        // dispatch — a deferred call's completion runs the user callback, which
        // routinely release()s the handle it was delivered on. Deleting the
        // signal receiver while Qt is still unwinding that emission is a
        // use-after-free. Disconnect now so nothing more is dispatched, and let
        // the event loop do the deleting.
        if (m_proxy)
            QObject::disconnect(m_proxy.data(), nullptr, m_helper, nullptr);
        m_helper->deleteLater();
        m_helper = nullptr;
        // Every waiter's channel just went away. The sync ones are woken so
        // they report a timeout rather than parking for their whole budget; the
        // async ones go with the helper their delivery was contexted on.
        for (QEventLoop* loop : m_ledger->syncWaiters)
            if (loop) loop->quit();
        m_ledger->asyncWaiters.clear();
    }

    void emitEvent(const QString& eventName, const QVariantList& data) override
    {
        if (!m_proxy) return;
        qDebug() << "[LogosObject] LocalLogosObject::emitEvent" << eventName << "data:" << data.size() << "items";
        QMetaObject::invokeMethod(m_proxy.data(), "eventResponse",
                                  Qt::QueuedConnection,
                                  Q_ARG(QString, eventName),
                                  Q_ARG(QVariantList, data));
    }

    QJsonArray getMethods() override
    {
        if (!m_proxy) return QJsonArray();
        return m_proxy->getPluginMethods();
    }

    void release() override
    {
        // Local mode: we don't own the ModuleProxy, just stop using it
        disconnectEvents();
    }

    quintptr id() const override { return reinterpret_cast<quintptr>(m_proxy.data()); }

private:
    logos::CallError proxyGoneError() const
    {
        const std::string origin = m_objectName.toStdString();
        return logos::callErrorObjectUnavailable(
            origin, "module '" + origin + "' is no longer registered locally");
    }

    // A caller that passes no deadline still gets one. 30s matches the bound
    // RemoteLogosObject applies to the same wait.
    static int deferredBudget(int timeoutMs) { return timeoutMs > 0 ? timeoutMs : 30000; }

    // Bring the event channel up and install the completion sink on it. Both
    // halves in one place because a helper without the sink is exactly the bug
    // this transport had: events flowed, completions did not.
    //
    // Idempotent, and called from every entry point that can produce or consume
    // a completion — disconnectEvents() may have taken the channel down between
    // two calls on the same handle.
    void ensureCompletionChannel()
    {
        if (m_helper || !m_proxy) return;

        m_helper = new EventHelper();
        QObject::connect(m_proxy.data(), SIGNAL(eventResponse(QString,QVariantList)),
                         m_helper, SLOT(onEventResponse(QString,QVariantList)));
        qDebug() << "[LogosObject] LocalLogosObject: connected EventHelper to ModuleProxy signals";

        EventHelper* helper = m_helper;
        m_helper->addCallback(logos::callCompleteEvent(),
            [helper, ledger = m_ledger](const QString&, const QVariantList& data) {
                if (data.size() != 2) return;
                const QString id = data.at(0).toString();
                const QVariant result = data.at(1);
                ledger->values.insert(id, result);
                if (QEventLoop* loop = ledger->syncWaiters.value(id, nullptr))
                    loop->quit();                          // wake a sync waiter
                if (ledger->asyncWaiters.contains(id)) {   // fire an async one
                    auto cb = ledger->asyncWaiters.take(id);
                    ledger->values.remove(id);
                    // NEXT event-loop turn, never inline. The user callback
                    // (LogosAPIConsumer's async lambda -> the module's own
                    // completion handler) routinely emits an event and then
                    // release()s this handle, and doing either from inside the
                    // helper's slot re-enters the object Qt is still
                    // dispatching through. The helper is the context, so the
                    // delivery is dropped if the channel is torn down first.
                    if (cb)
                        QTimer::singleShot(0, helper,
                            [cb = std::move(cb), result]() { cb(result, logos::CallError{}); });
                }
            });
    }

    // Resolve a possibly-deferred result. A provider that answered the pending
    // sentinel has promised the real value as a completion event keyed by
    // callId; wait for it, pumping this thread's event loop, and return that.
    // An ordinary result is returned unchanged.
    //
    // THE NESTED LOOP IS THE POINT, not an accident of the implementation: the
    // completion is emitted from the provider's worker and ModuleProxy queues
    // it onto THIS thread, so a caller that blocked instead of pumping would be
    // blocking the one thread the answer can arrive on.
    QVariant resolveDeferred(const QVariant& rv, int timeoutMs,
                             const QString& methodName, logos::CallError* err)
    {
        QString callId;
        if (!logos::isPendingCallSentinel(rv, &callId)) return rv;
        // It can already be here: the provider's worker runs concurrently with
        // the dispatch that returned the sentinel.
        if (m_ledger->values.contains(callId)) return m_ledger->values.take(callId);

        const int budget = deferredBudget(timeoutMs);
        QEventLoop loop;
        m_ledger->syncWaiters.insert(callId, &loop);
        QTimer timer;
        timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        timer.start(budget);
        loop.exec();
        m_ledger->syncWaiters.remove(callId);

        if (m_ledger->values.contains(callId)) return m_ledger->values.take(callId);
        qWarning() << "LocalLogosObject: deferred call" << callId << "timed out";
        if (err)
            *err = logos::callErrorTimeout(m_objectName.toStdString(),
                                           methodName.toStdString(), budget);
        return QVariant();
    }

    // A QPointer, NOT A RAW ONE, and that is load-bearing rather than tidy.
    // This handle does not own the ModuleProxy — the module's LogosAPIProvider
    // does — and the two are torn down independently: unloading a module
    // destroys the proxy while every consumer still holds its handle. Every
    // `if (!m_proxy)` guard in this class was reading a dangling pointer as a
    // live one; with a QPointer the guards mean what they say, and
    // disconnectEvents() can name the sender without dereferencing a corpse.
    QPointer<ModuleProxy> m_proxy;
    EventHelper* m_helper;
    LedgerPtr m_ledger = std::make_shared<CompletionLedger>();
    QString m_objectName;
};

// ── LocalTransportHost ───────────────────────────────────────────────────────

bool LocalTransportHost::publishObject(const QString& name, QObject* object)
{
    PluginRegistry::registerPlugin(object, name);
    qDebug() << "LocalTransportHost: Published object:" << name;
    return true;
}

void LocalTransportHost::unpublishObject(const QString& name)
{
    if (!name.isEmpty()) {
        PluginRegistry::unregisterPlugin(name);
        qDebug() << "LocalTransportHost: Unpublished object:" << name;
    }
}

// ── LocalTransportConnection ─────────────────────────────────────────────────

bool LocalTransportConnection::connectToHost()
{
    qDebug() << "LocalTransportConnection: Local mode - no connection needed";
    return true;
}

bool LocalTransportConnection::isConnected() const
{
    return true;
}

bool LocalTransportConnection::reconnect()
{
    return true;
}

LogosObject* LocalTransportConnection::requestObject(const QString& objectName, int /*timeoutMs*/)
{
    QObject* plugin = PluginRegistry::getPlugin<QObject>(objectName);
    if (!plugin) {
        qWarning() << "LocalTransportConnection: Plugin not found in registry:" << objectName;
        return nullptr;
    }

    ModuleProxy* proxy = qobject_cast<ModuleProxy*>(plugin);
    if (!proxy) {
        qWarning() << "LocalTransportConnection: Plugin is not a ModuleProxy:" << objectName;
        return nullptr;
    }

    qDebug() << "[LogosObject] LocalTransportConnection: returning LocalLogosObject for:" << objectName;
    return new LocalLogosObject(proxy, objectName);
}

#include "local_transport.moc"
