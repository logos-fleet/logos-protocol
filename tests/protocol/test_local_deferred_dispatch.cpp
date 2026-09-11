// The qt_local consumer against a provider that DEFERS.
//
// logos_async_dispatch.h states the contract: a provider may answer the
// pending-call sentinel instead of the result, and "the CONSUMER transport
// detects the sentinel, waits for the matching completion keyed by callId, and
// returns the real result — so generated clients call transparently". Two of
// the three consumer transports did that; qt_local did not, and returned the
// sentinel map to the caller as if it were the answer.
//
// It cost nothing while the only deferring providers were `multi` Qt plugins,
// which are always reached over qt_remote. The Native container changed that:
// a Bundled Bare module is published in the SAME process as its consumer, so
// the handle it hands out is a LocalLogosObject, and BareModuleGlue defers
// every published dispatch by construction (bare_module_glue.h). Measured on
// the iOS simulator (logos-workspace#53): `add(1, 2)` on the bundled
// bare_counter came back empty with CallError::ok() true, and the module's
// __logos_call_complete__ arrived AFTER the caller had already given up on it.
//
// So this file is the qt_local arm of what test_concurrent_dispatch.cpp
// asserts for plain. The provider here defers the way a published
// BareModuleGlue does — sentinel now, completion from another thread later —
// and the assertions are on the VALUE, because an empty QVariant with no error
// set is exactly the shape the bug produced.

#include <gtest/gtest.h>

#include "logos_async_dispatch.h"
#include "logos_call_error.h"
#include "logos_object.h"
#include "logos_provider_interface.h"
#include "module_proxy.h"
#include "plugin_registry.h"

#include "local_transport.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QString>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

namespace {

const QString kModule = QStringLiteral("deferring_module");
const QString kToken  = QStringLiteral("tok");

// A provider shaped like a published BareModuleGlue: callMethod hands the work
// to a worker thread and answers the pending-call sentinel, and the worker
// pushes the real result back over the event channel under the same call id.
//
// `delayMs` is what puts the completion genuinely AFTER callMethod returns. A
// worker that finishes first would let a consumer that never waits still see
// the right answer, and this file would stop detecting anything.
class DeferringProvider : public LogosProviderObject {
public:
    explicit DeferringProvider(int delayMs) : m_delayMs(delayMs) {}

    ~DeferringProvider() override { drain(); }

    // Block until every worker has finished emitting. A worker's completion
    // goes out through m_eventCb, which is ModuleProxy's lambda capturing the
    // proxy, so no worker may still be running when that proxy is destroyed.
    void drain()
    {
        while (m_inFlight.load() > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    QVariant callMethod(const QString& method, const QVariantList& args) override
    {
        if (method != QLatin1String("add")) return QVariant();
        const int sum = args.value(0).toInt() + args.value(1).toInt();

        const QString callId = QStringLiteral("bc-%1").arg(
            static_cast<qulonglong>(m_callCounter.fetch_add(1, std::memory_order_relaxed)));
        m_inFlight.fetch_add(1);
        std::thread([this, callId, sum]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(m_delayMs));
            if (m_eventCb)
                m_eventCb(logos::callCompleteEvent(), QVariantList{ callId, QVariant(sum) });
            m_inFlight.fetch_sub(1);
        }).detach();

        QVariantMap pending;
        pending[logos::pendingCallKey()] = callId;
        return pending;
    }

    QJsonArray getMethods() override { return QJsonArray{}; }
    bool informModuleToken(const QString&, const QString&) override { return true; }
    void setEventListener(EventCallback cb) override { m_eventCb = std::move(cb); }
    void init(void*) override {}
    QString providerName() const override { return QStringLiteral("deferring"); }
    QString providerVersion() const override { return QStringLiteral("1.0.0"); }

private:
    int m_delayMs;
    EventCallback m_eventCb;
    std::atomic<unsigned long long> m_callCounter{0};
    std::atomic<int> m_inFlight{0};
};

// One deferring module published on the in-process registry, plus the consumer
// handle a caller in the same process gets for it.
class LocalDeferredDispatchTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        m_provider = std::make_unique<DeferringProvider>(120);
        m_proxy = std::make_unique<ModuleProxy>(m_provider.get());
        m_proxy->saveToken(QStringLiteral("caller"), kToken);

        m_host = std::make_unique<LocalTransportHost>();
        ASSERT_TRUE(m_host->publishObject(kModule, m_proxy.get()));

        m_connection = std::make_unique<LocalTransportConnection>();
        m_object = m_connection->requestObject(kModule, 5000);
        ASSERT_NE(m_object, nullptr);
    }

    void TearDown() override
    {
        if (m_object) { m_object->release(); m_object = nullptr; }
        m_connection.reset();
        m_provider->drain();
        m_host->unpublishObject(kModule);
        m_host.reset();
        m_proxy.reset();
        m_provider.reset();
    }

    std::unique_ptr<DeferringProvider> m_provider;
    std::unique_ptr<ModuleProxy> m_proxy;
    std::unique_ptr<LocalTransportHost> m_host;
    std::unique_ptr<LocalTransportConnection> m_connection;
    LogosObject* m_object = nullptr;

    // The error-carrying half is a SEPARATE interface reached by dynamic_cast
    // (logos_object.h) — LogosObject's own layout is frozen by the ABI.
    LogosObjectErrorChannel* channel() const
    {
        return dynamic_cast<LogosObjectErrorChannel*>(m_object);
    }
};

} // namespace

// THE REGRESSION, in the shape the simulator reported it: a value, not a map.
TEST_F(LocalDeferredDispatchTest, ASynchronousCallReturnsTheDeferredValue)
{
    ASSERT_NE(channel(), nullptr);
    logos::CallError err;
    const QVariant result = channel()->callMethodWithError(
        kToken, QStringLiteral("add"),
        QVariantList{ QVariant(1), QVariant(2) }, 5000, &err);

    EXPECT_TRUE(err.ok()) << err.message;

    QString leaked;
    ASSERT_FALSE(logos::isPendingCallSentinel(result, &leaked))
        << "the pending-call sentinel reached the caller (call id " << leaked.toStdString()
        << "); qt_local did not await the completion";
    EXPECT_EQ(result.toLongLong(), 3);
}

// The same call through the adapter that discards the diagnosis — which is the
// entry point LogosAPIConsumer uses when the caller passes no CallError.
TEST_F(LocalDeferredDispatchTest, TheErrorFreeEntryPointResolvesItToo)
{
    const QVariant result = m_object->callMethod(
        kToken, QStringLiteral("add"),
        QVariantList{ QVariant(20), QVariant(22) }, 5000);

    EXPECT_FALSE(logos::isPendingCallSentinel(result));
    EXPECT_EQ(result.toLongLong(), 42);
}

// The async half of the same contract: the callback must carry the value, not
// the sentinel. Nothing above the transport unwraps it, so a leak here reaches
// the module's own completion handler.
TEST_F(LocalDeferredDispatchTest, AnAsynchronousCallDeliversTheDeferredValue)
{
    QVariant delivered;
    logos::CallError err;
    std::atomic<bool> done{false};

    ASSERT_NE(channel(), nullptr);
    channel()->callMethodAsyncWithError(
        kToken, QStringLiteral("add"),
        QVariantList{ QVariant(3), QVariant(4) }, 5000,
        [&](QVariant v, const logos::CallError& e) {
            delivered = std::move(v);
            err = e;
            done = true;
        });

    QElapsedTimer t; t.start();
    while (!done.load() && t.elapsed() < 5000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);

    ASSERT_TRUE(done.load()) << "the async callback never fired";
    EXPECT_TRUE(err.ok()) << err.message;
    EXPECT_FALSE(logos::isPendingCallSentinel(delivered));
    EXPECT_EQ(delivered.toLongLong(), 7);
}

// Back-to-back calls on the same handle: each waiter must be woken by ITS OWN
// completion. One shared slot would let the first answer satisfy the second.
TEST_F(LocalDeferredDispatchTest, EachCallGetsItsOwnCompletion)
{
    for (int i = 0; i < 5; ++i) {
        const QVariant result = m_object->callMethod(
            kToken, QStringLiteral("add"),
            QVariantList{ QVariant(i), QVariant(100) }, 5000);
        EXPECT_EQ(result.toLongLong(), 100 + i) << "round " << i;
    }
}

// A completion that never comes is a TIMEOUT, reported as one. The empty
// QVariant with no error set is the shape that made #53 look like a wrong
// answer rather than a missing one, and it is the shape not to go back to.
TEST(LocalDeferredDispatchTimeoutTest, ACompletionThatNeverArrivesIsATimeout)
{
    // A provider that answers the sentinel and then forgets about it entirely.
    class SilentProvider : public LogosProviderObject {
    public:
        QVariant callMethod(const QString&, const QVariantList&) override
        {
            QVariantMap pending;
            pending[logos::pendingCallKey()] = QStringLiteral("bc-silent");
            return pending;
        }
        QJsonArray getMethods() override { return QJsonArray{}; }
        bool informModuleToken(const QString&, const QString&) override { return true; }
        void setEventListener(EventCallback) override {}
        void init(void*) override {}
        QString providerName() const override { return QStringLiteral("silent"); }
        QString providerVersion() const override { return QStringLiteral("1.0.0"); }
    };

    const QString name = QStringLiteral("silent_module");
    SilentProvider provider;
    ModuleProxy proxy(&provider);
    proxy.saveToken(QStringLiteral("caller"), kToken);

    LocalTransportHost host;
    ASSERT_TRUE(host.publishObject(name, &proxy));
    LocalTransportConnection connection;
    LogosObject* object = connection.requestObject(name, 5000);
    ASSERT_NE(object, nullptr);

    auto* channel = dynamic_cast<LogosObjectErrorChannel*>(object);
    ASSERT_NE(channel, nullptr);
    logos::CallError err;
    const QVariant result = channel->callMethodWithError(
        kToken, QStringLiteral("add"), QVariantList{}, 300, &err);

    EXPECT_FALSE(result.isValid());
    EXPECT_FALSE(err.ok()) << "a deferred call that never completed was reported as a success";
    EXPECT_EQ(err.code, logos::callErrorTimeout(name.toStdString(), "add", 300).code);

    object->release();
    host.unpublishObject(name);
}
