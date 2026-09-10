// The web transport: the plain transport's message set, as JSON, over an
// abstract message channel.
//
// The web transport exists so a module living inside a webview can be an
// ordinary Logos participant. It carries EXACTLY the message types the plain
// transport carries (Call, Result, Subscribe, Unsubscribe, Event, Token,
// Methods, MethodsResult) and drops only the byte framing, because a message
// channel — postMessage, a custom-scheme fetch pump — already delivers whole
// messages. The channel is injected, so these cases drive the real host and the
// real connection through an in-memory pair; a webview binding replaces the
// pair and nothing else.
//
// What each case is here to catch:
//
//   * the round trips (call, methods, subscribe/unsubscribe, events) — the
//     acceptance criterion is that a provider published on the web transport
//     answers everything it answers over TCP;
//   * the transport TAG. A token minted for one wire must not be usable on
//     another, and the only thing that can tell them apart is the label the
//     host hands ModuleProxy. "web" has to arrive, and a validator has to be
//     able to refuse it while accepting "local";
//   * the size cap. The plain transport rejects a frame above kMaxFrameLength;
//     a channel with no length prefix has to reject the same message with the
//     same error, or the web transport becomes the way around the cap.

#include <gtest/gtest.h>

#include "logos_mode.h"
#include "logos_object.h"
#include "logos_provider_interface.h"
#include "logos_transport.h"
#include "logos_transport_config.h"
#include "logos_transport_config_json.h"
#include "logos_transport_factory.h"
#include "module_proxy.h"
#include "token_manager.h"

#include "rpc_framing.h"

#include "incoming_call_handler.h"
#include "in_memory_channel.h"
#include "web_message_codec.h"
#include "web_transport_connection.h"
#include "web_transport_host.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QThread>
#include <QVariant>
#include <QVariantList>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace logos::web;

namespace {

QCoreApplication* ensureApp()
{
    static int argc = 0;
    static char* argv[] = { nullptr };
    if (!QCoreApplication::instance())
        new QCoreApplication(argc, argv);
    return QCoreApplication::instance();
}

// An ordinary provider: it answers two methods, lists one method and one event,
// and records the tokens pushed at it.
class WebProvider : public LogosProviderObject {
public:
    QVariant callMethod(const QString& method, const QVariantList& args) override
    {
        if (method == QLatin1String("echo")) return args.value(0);
        if (method == QLatin1String("sum"))
            return args.value(0).toInt() + args.value(1).toInt();
        return QVariant();
    }

    QJsonArray getMethods() override
    {
        QJsonObject m;
        m["name"] = "echo";
        m["type"] = "method";
        m["signature"] = "echo(QVariant)";
        m["returnType"] = "QVariant";
        QJsonObject e;
        e["name"] = "ping";
        e["type"] = "event";
        e["signature"] = "ping(QVariantList)";
        e["returnType"] = "void";
        return QJsonArray{ m, e };
    }

    bool informModuleToken(const QString& moduleName, const QString& token) override
    {
        ++tokenPushes;
        lastModule = moduleName;
        lastToken = token;
        return true;
    }

    void setEventListener(EventCallback) override {}
    void init(void*) override {}
    QString providerName() const override { return QStringLiteral("web_module"); }
    QString providerVersion() const override { return QStringLiteral("1.0.0"); }

    std::atomic<int> tokenPushes{0};
    QString lastModule;
    QString lastToken;
};

// The module's own credential: what informModuleToken authorizes a push
// against, and the one thing a Token round trip has to get right before the
// transport can be blamed for anything.
const QString kCoreKey   = QStringLiteral("core");
const QString kCoreToken = QStringLiteral("web-core-token");

// A live web host serving `web_module` through a ModuleProxy on its OWN thread,
// plus a consumer connection wired to the other end of an in-memory pair.
//
// The proxy's thread is not a detail: WebTransportHost dispatches an inbound
// Call with a queued invokeMethod, exactly as PlainTransportHost does, so
// something other than the blocked caller has to run the proxy's event loop.
class WebFixture {
public:
    explicit WebFixture(const QString& objectName = QStringLiteral("web_module"))
    {
        m_proxy = new ModuleProxy(&m_provider);
        m_proxy->saveToken(QStringLiteral("origin"), QStringLiteral("web-token"));
        // The trust anchor a token push is gated on: informModuleToken accepts
        // only a caller holding this module's own credential, which it reads
        // from the TOKEN STORE (not from the proxy's own issued-token table),
        // so a fixture that wants to watch a Token message cross the wire seeds
        // it there exactly as a host does at startup. Saved and restored
        // because the store is process-global and a leaked anchor makes later
        // cases order-dependent.
        m_savedCore = TokenManager::instance().getToken(kCoreKey);
        TokenManager::instance().saveToken(kCoreKey, kCoreToken);
        m_thread = new QThread;
        m_proxy->moveToThread(m_thread);
        m_thread->start();

        m_host = std::make_unique<WebTransportHost>();
        m_published = m_host->publishObject(objectName, m_proxy);

        auto pair = makeInMemoryChannelPair();
        m_hostChannel = pair.first;
        m_consumerChannel = pair.second;
        m_attached = m_host->attachChannel(m_hostChannel);
        m_conn = std::make_unique<WebTransportConnection>(m_consumerChannel);
        m_connected = m_conn->connectToHost();
    }

    ~WebFixture()
    {
        // Same order as the plain fixtures: the transport goes first so no
        // inbound message can reach a proxy that is being dismantled.
        m_conn.reset();
        m_host.reset();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        m_thread->quit();
        m_thread->wait();
        delete m_proxy;
        delete m_thread;
        if (m_savedCore.isEmpty()) TokenManager::instance().removeToken(kCoreKey);
        else TokenManager::instance().saveToken(kCoreKey, m_savedCore);
    }

    bool ok() const { return m_published && m_attached && m_connected; }
    WebTransportConnection& connection() { return *m_conn; }
    WebTransportHost& host() { return *m_host; }
    ModuleProxy& proxy() { return *m_proxy; }
    WebProvider& provider() { return m_provider; }
    // The raw endpoints, so a case can put something on the wire that no
    // well-behaved peer would.
    const MessageChannelPtr& consumerChannel() const { return m_consumerChannel; }

private:
    WebProvider m_provider;
    ModuleProxy* m_proxy = nullptr;
    QThread* m_thread = nullptr;
    std::unique_ptr<WebTransportHost> m_host;
    std::unique_ptr<WebTransportConnection> m_conn;
    MessageChannelPtr m_hostChannel;
    MessageChannelPtr m_consumerChannel;
    QString m_savedCore;
    bool m_published = false;
    bool m_attached = false;
    bool m_connected = false;
};

// A handle released at scope exit — LogosObject has no destructor contract of
// its own, release() is the one.
struct ObjectGuard {
    LogosObject* obj = nullptr;
    ~ObjectGuard() { if (obj) obj->release(); }
};

bool waitFor(const std::function<bool()>& pred, int budgetMs)
{
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(budgetMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

} // namespace

class WebTransportTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ensureApp();
        m_savedMode = LogosModeConfig::getMode();
    }
    void TearDown() override { LogosModeConfig::setMode(m_savedMode); }
    LogosMode m_savedMode;
};

// ── the protocol value exists and survives the config round trip ─────────────
TEST_F(WebTransportTest, WebIsATransportValueThatSurvivesJson)
{
    LogosTransportConfig cfg;
    cfg.protocol = LogosProtocol::Web;

    const std::string json = logos::transportSetToJsonString({ cfg });
    EXPECT_NE(json.find("\"web\""), std::string::npos) << json;

    const LogosTransportSet back = logos::transportSetFromJsonString(json);
    ASSERT_EQ(back.size(), 1u);
    EXPECT_EQ(back[0].protocol, LogosProtocol::Web);
}

// ── the factory resolves Web in Remote mode, and answers needsQtEventLoop ────
//
// A wrong needsQtEventLoop is either a hang (a client anchored nowhere) or a
// pointless main-thread hop; the web connection owns no QObject and no socket,
// so it is Qt-free exactly as the plain transports are.
TEST_F(WebTransportTest, TheFactoryResolvesWebInRemoteMode)
{
    LogosTransportConfig cfg;
    cfg.protocol = LogosProtocol::Web;

    LogosModeConfig::setMode(LogosMode::Remote);

    auto host = LogosTransportFactory::createHost(cfg, QStringLiteral("unused"));
    ASSERT_NE(host, nullptr);
    EXPECT_NE(dynamic_cast<WebTransportHost*>(host.get()), nullptr)
        << "Remote + Web must resolve to the web host";

    auto conn = LogosTransportFactory::createConnection(cfg, QStringLiteral("unused"));
    ASSERT_NE(conn, nullptr);
    EXPECT_NE(dynamic_cast<WebTransportConnection*>(conn.get()), nullptr)
        << "Remote + Web must resolve to the web connection";

    EXPECT_FALSE(LogosTransportFactory::needsQtEventLoop(cfg))
        << "the web connection owns no Qt object and no socket";

    // Mode still wins over cfg.protocol, exactly as for Tcp.
    LogosModeConfig::setMode(LogosMode::Mock);
    EXPECT_EQ(dynamic_cast<WebTransportConnection*>(
        LogosTransportFactory::createConnection(cfg, QStringLiteral("unused")).get()),
        nullptr);
    EXPECT_FALSE(LogosTransportFactory::needsQtEventLoop(cfg));
}

// ── a call answers through the channel pair ──────────────────────────────────
TEST_F(WebTransportTest, ACallAnswersThroughTheChannelPair)
{
    WebFixture fx;
    ASSERT_TRUE(fx.ok());

    ObjectGuard g{ fx.connection().requestObject(QStringLiteral("web_module"), 2000) };
    ASSERT_NE(g.obj, nullptr);

    const QVariant echoed = g.obj->callMethod(QStringLiteral("web-token"),
                                              QStringLiteral("echo"),
                                              QVariantList{ QStringLiteral("hello") },
                                              5000);
    EXPECT_EQ(echoed.toString(), QStringLiteral("hello"));

    const QVariant summed = g.obj->callMethod(QStringLiteral("web-token"),
                                              QStringLiteral("sum"),
                                              QVariantList{ 20, 22 }, 5000);
    EXPECT_EQ(summed.toInt(), 42);
}

// ── introspection answers through the channel pair ───────────────────────────
TEST_F(WebTransportTest, MethodsAnswerThroughTheChannelPair)
{
    WebFixture fx;
    ASSERT_TRUE(fx.ok());

    ObjectGuard g{ fx.connection().requestObject(QStringLiteral("web_module"), 2000) };
    ASSERT_NE(g.obj, nullptr);

    const QJsonArray methods = g.obj->getMethods();
    ASSERT_FALSE(methods.isEmpty()) << "the web transport answered no methods";
    EXPECT_EQ(methods.at(0).toObject().value("name").toString(),
              QStringLiteral("echo"));
}

// ── subscribe, receive, unsubscribe ──────────────────────────────────────────
TEST_F(WebTransportTest, EventsFlowUntilTheSubscriptionIsWithdrawn)
{
    WebFixture fx;
    ASSERT_TRUE(fx.ok());

    ObjectGuard g{ fx.connection().requestObject(QStringLiteral("web_module"), 2000) };
    ASSERT_NE(g.obj, nullptr);

    std::atomic<int> received{0};
    QString lastName;
    QVariantList lastData;
    g.obj->onEvent(QStringLiteral("ping"),
        [&](const QString& name, const QVariantList& data) {
            lastName = name;
            lastData = data;
            received.fetch_add(1);
        });

    // The Subscribe message has to reach the host before an emission can be
    // fanned out to it.
    ASSERT_TRUE(waitFor([&] { return fx.host().subscriberCount("web_module", "ping") > 0; }, 3000))
        << "the Subscribe message never reached the host";

    emit fx.proxy().eventResponse(QStringLiteral("ping"),
                                  QVariantList{ QStringLiteral("hi"), 42 });

    ASSERT_TRUE(waitFor([&] { return received.load() >= 1; }, 3000))
        << "the event never crossed the channel";
    EXPECT_EQ(lastName, QStringLiteral("ping"));
    ASSERT_EQ(lastData.size(), 2);
    EXPECT_EQ(lastData[0].toString(), QStringLiteral("hi"));
    EXPECT_EQ(lastData[1].toInt(), 42);

    g.obj->disconnectEvents();
    ASSERT_TRUE(waitFor([&] { return fx.host().subscriberCount("web_module", "ping") == 0; }, 3000))
        << "the Unsubscribe message never reached the host";

    const int before = received.load();
    emit fx.proxy().eventResponse(QStringLiteral("ping"),
                                  QVariantList{ QStringLiteral("after") });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(received.load(), before)
        << "an event arrived after the subscription was withdrawn";
}

// ── a Token message flows, and it arrives tagged "web" ───────────────────────
//
// Two claims, and the second is the one that has teeth: a validator installed
// on the module can REFUSE a call that arrived over the web while accepting the
// identical token over the in-process path, which is only possible if the host
// labels the wire correctly.
TEST_F(WebTransportTest, TokensFlowAndTheTransportTagIsWeb)
{
    WebFixture fx;
    ASSERT_TRUE(fx.ok());

    ObjectGuard g{ fx.connection().requestObject(QStringLiteral("web_module"), 2000) };
    ASSERT_NE(g.obj, nullptr);

    EXPECT_TRUE(g.obj->informModuleToken(kCoreToken,
                                         QStringLiteral("peer"),
                                         QStringLiteral("peer-token"), 3000));
    ASSERT_TRUE(waitFor([&] { return fx.provider().tokenPushes.load() >= 1; }, 3000))
        << "the Token message never reached the provider";
    EXPECT_EQ(fx.provider().lastModule, QStringLiteral("peer"));
    EXPECT_EQ(fx.provider().lastToken, QStringLiteral("peer-token"));

    // A transport-sensitive validator: this token is good on the web and
    // nowhere else. Nothing but the tag can distinguish the two calls below.
    QString seenTag;
    std::mutex tagMu;
    fx.proxy().setTokenValidator([&](const QString& token, const QString& transport) {
        {
            std::lock_guard<std::mutex> lock(tagMu);
            seenTag = transport;
        }
        return token == QLatin1String("web-only") && transport == QLatin1String("web");
    });

    const QVariant overWeb = g.obj->callMethod(QStringLiteral("web-only"),
                                               QStringLiteral("echo"),
                                               QVariantList{ QStringLiteral("ok") },
                                               5000);
    EXPECT_EQ(overWeb.toString(), QStringLiteral("ok"))
        << "a web-only token was refused on the web transport";
    {
        std::lock_guard<std::mutex> lock(tagMu);
        EXPECT_EQ(seenTag, QStringLiteral("web"))
            << "the host labelled the wire " << seenTag.toStdString();
    }

    // The same token over the in-process (3-arg, "local") door is refused —
    // the validator can tell web from local.
    QVariant overLocal;
    QMetaObject::invokeMethod(&fx.proxy(), "callRemoteMethod",
                              Qt::BlockingQueuedConnection,
                              Q_RETURN_ARG(QVariant, overLocal),
                              Q_ARG(QString, QStringLiteral("web-only")),
                              Q_ARG(QString, QStringLiteral("echo")),
                              Q_ARG(QVariantList, QVariantList{ QStringLiteral("ok") }));
    EXPECT_NE(overLocal.toString(), QStringLiteral("ok"))
        << "a web-only token was accepted over the local door";
}

// ── the plain transport's size cap, enforced without a length prefix ─────────
//
// The cap is the plain transport's own kMaxFrameLength and the refusal is the
// plain transport's own FramingError, on purpose: a message channel that let a
// bigger message through would be the documented way around a limit that exists
// to stop runaway allocation.
TEST_F(WebTransportTest, AnOversizedMessageIsRejectedLikeAnOversizedFrame)
{
    using logos::plain::CallMessage;
    using logos::plain::FramingError;
    using logos::plain::kMaxFrameLength;
    using logos::plain::AnyMessage;

    CallMessage small;
    small.id = 1;
    small.object = "web_module";
    small.method = "echo";
    small.args.push_back(logos::plain::RpcValue{ std::string("hi") });
    const std::string ok = encodeWebMessage(AnyMessage{ small });
    EXPECT_FALSE(ok.empty());
    EXPECT_EQ(std::get<CallMessage>(decodeWebMessage(ok)).method, "echo");

    CallMessage huge;
    huge.id = 2;
    huge.object = "web_module";
    huge.method = "echo";
    huge.args.push_back(logos::plain::RpcValue{ std::string(kMaxFrameLength + 16, 'x') });
    EXPECT_THROW({ encodeWebMessage(AnyMessage{ huge }); }, FramingError);

    // The receive side rejects it too — a peer that framed it by hand does not
    // get in either.
    const std::string oversizedText(std::size_t(kMaxFrameLength) + 16, 'x');
    EXPECT_THROW({ decodeWebMessage(oversizedText); }, FramingError);
}

// ── a malformed message ends the conversation, from the channel's own thread ─
//
// THIS CASE IS A DEADLOCK DETECTOR, and it fails by HANGING rather than by
// asserting, which is why it is worth naming what it watches. A message that
// cannot be decoded runs RpcPeer::fail(), which calls closeTransport(), which
// detaches the channel receiver — on the channel's own delivery thread, while
// that thread is inside the delivery. The channel holds a lock across a
// delivery so that a detach from ANOTHER thread waits the delivery out; if that
// lock is not recursive, this path re-enters it and the pump wedges forever,
// taking every later message on that channel with it.
//
// It is not a hypothetical path: it is the one a transport most needs to
// survive, because it is what a peer sending garbage produces.
TEST_F(WebTransportTest, AMalformedMessageEndsTheConversationWithoutWedgingTheChannel)
{
    WebFixture fx;
    ASSERT_TRUE(fx.ok());

    ObjectGuard g{ fx.connection().requestObject(QStringLiteral("web_module"), 2000) };
    ASSERT_NE(g.obj, nullptr);

    // Straight onto the wire, under the consumer peer, so what arrives at the
    // host is something its codec must refuse.
    ASSERT_TRUE(fx.consumerChannel()->send("this is not a logos envelope"));

    // The host's peer is now torn down, so nothing it is told afterwards
    // registers. A Subscribe is the cheapest observable: on a LIVE host it
    // shows up within milliseconds (the case above waits for exactly that), so
    // a full budget with no subscriber is the conversation being over rather
    // than the message being slow.
    g.obj->onEvent(QStringLiteral("ping"),
                   [](const QString&, const QVariantList&) {});
    EXPECT_FALSE(waitFor(
        [&] { return fx.host().subscriberCount("web_module", "ping") > 0; }, 500))
        << "the host kept serving a peer that sent an undecodable message";

    // Reaching here at all is the other half: a wedged pump never returns from
    // its delivery, so the fixture's teardown — which joins that thread — would
    // hang instead of failing.
}

// ── ONE CHANNEL, BOTH DIRECTIONS ─────────────────────────────────────────────
//
// A Web module is not only something the host calls: a page has to be able to
// call BACK — `capability_module.requestModule` for a token, another module's
// method, a subscription to a native module's event. All of that is inbound
// traffic on the very channel the host already holds for its own outbound
// calls, and a second WebRpcConnection cannot be laid over that channel: each
// one installs the channel's single receiver, so the second silently steals
// every message from the first.
//
// The conversation was always full duplex — RpcPeer serves whatever
// IncomingCallHandler it was given while its own calls are in flight, and the
// browser SDK's WebPeer is one object for both roles too. Only the CONSUMER
// entry point refused to say so, hard-wiring a null handler. So the door is a
// second constructor argument rather than a new class: hand the connection a
// handler and it serves the far end as well as consuming it.
TEST_F(WebTransportTest, AConnectionWithAHandlerServesTheFarEndToo)
{
    // What the host offers a page: one method, one introspection answer, and a
    // sink to push events into.
    struct HostSide : logos::plain::IncomingCallHandler {
        void onCall(const logos::plain::CallMessage& req, CallReply reply) override
        {
            lastObject = req.object;
            lastMethod = req.method;
            lastAuthToken = req.authToken;
            logos::plain::ResultMessage res;
            res.id = req.id;
            res.ok = true;
            res.value = logos::plain::RpcValue(int64_t{42});
            reply(std::move(res));
        }
        void onMethods(const logos::plain::MethodsMessage& req, MethodsReply reply) override
        {
            logos::plain::MethodsResultMessage res;
            res.id = req.id;
            res.ok = true;
            logos::plain::MethodMetadata m;
            m.name = "requestModule";
            res.methods = { m };
            reply(std::move(res));
        }
        void onSubscribe(const logos::plain::SubscribeMessage& req, EventSink s,
                         const void*) override
        {
            subscribedTo = req.object + "/" + req.eventName;
            sink = std::move(s);
        }
        void onUnsubscribe(const logos::plain::UnsubscribeMessage&, const void*) override {}
        void onConnectionClosed(const void*) override { sink = nullptr; }
        void onToken(const logos::plain::TokenMessage& req) override
        {
            tokenFor = req.moduleName;
        }

        std::string lastObject, lastMethod, lastAuthToken, subscribedTo, tokenFor;
        EventSink sink;
    } host;

    auto pair = makeInMemoryChannelPair();
    WebTransportConnection conn(pair.first, &host);
    ASSERT_TRUE(conn.connectToHost());

    // The far end is raw, because that is what a page is from here: whole JSON
    // envelopes on a channel, encoded and decoded by the transport's own codec.
    std::mutex mu;
    std::vector<logos::plain::AnyMessage> fromHost;
    pair.second->setReceiver([&](const std::string& text) {
        std::lock_guard<std::mutex> g(mu);
        fromHost.push_back(decodeWebMessage(text));
    });
    auto received = [&](auto pick) {
        std::lock_guard<std::mutex> g(mu);
        for (auto& m : fromHost) if (pick(m)) return true;
        return false;
    };

    // ── page → host: a call ──────────────────────────────────────────────────
    logos::plain::CallMessage call;
    call.id = 7;
    call.object = "capability_module";
    call.method = "requestModule";
    call.authToken = "the-page-credential";
    ASSERT_TRUE(pair.second->send(encodeWebMessage(logos::plain::AnyMessage{call})));

    ASSERT_TRUE(waitFor([&] {
        return received([](const logos::plain::AnyMessage& m) {
            auto* r = std::get_if<logos::plain::ResultMessage>(&m);
            return r && r->id == 7 && r->ok;
        });
    }, 2000)) << "a call from the far end was never answered";
    EXPECT_EQ(host.lastObject, "capability_module");
    EXPECT_EQ(host.lastMethod, "requestModule");
    EXPECT_EQ(host.lastAuthToken, "the-page-credential")
        << "the credential a page presents has to survive the crossing";

    // ── page → host: introspection and a token push ──────────────────────────
    logos::plain::MethodsMessage q;
    q.id = 8;
    q.object = "capability_module";
    ASSERT_TRUE(pair.second->send(encodeWebMessage(logos::plain::AnyMessage{q})));
    ASSERT_TRUE(waitFor([&] {
        return received([](const logos::plain::AnyMessage& m) {
            auto* r = std::get_if<logos::plain::MethodsResultMessage>(&m);
            return r && r->id == 8 && r->methods.size() == 1;
        });
    }, 2000));

    logos::plain::TokenMessage tok;
    tok.moduleName = "js_counter";
    tok.token = "pair-token";
    ASSERT_TRUE(pair.second->send(encodeWebMessage(logos::plain::AnyMessage{tok})));
    EXPECT_TRUE(waitFor([&] { return host.tokenFor == "js_counter"; }, 2000));

    // ── page → host: a subscription, and the event that answers it ───────────
    logos::plain::SubscribeMessage sub;
    sub.object = "clock_module";
    sub.eventName = "ticked";
    ASSERT_TRUE(pair.second->send(encodeWebMessage(logos::plain::AnyMessage{sub})));
    ASSERT_TRUE(waitFor([&] { return host.sink != nullptr; }, 2000))
        << "a Subscribe from the far end never reached the handler";
    EXPECT_EQ(host.subscribedTo, "clock_module/ticked");

    logos::plain::EventMessage ev;
    ev.object = "clock_module";
    ev.eventName = "ticked";
    ev.data = { logos::plain::RpcValue(int64_t{1}) };
    host.sink(ev);
    EXPECT_TRUE(waitFor([&] {
        return received([](const logos::plain::AnyMessage& m) {
            auto* e = std::get_if<logos::plain::EventMessage>(&m);
            return e && e->eventName == "ticked";
        });
    }, 2000)) << "a native module's event never reached the page";

    pair.second->setReceiver(nullptr);
}
