#include <gtest/gtest.h>

#include "json_codec.h"

#include <cstdint>

using namespace logos::plain;

namespace {
AnyMessage roundtrip(JsonCodec& codec, const AnyMessage& msg)
{
    auto bytes = codec.encode(msg);
    return codec.decode(messageTypeOf(msg), bytes.data(), bytes.size());
}
} // anonymous namespace

TEST(JsonCodecTest, CallMessage)
{
    JsonCodec codec;
    CallMessage c;
    c.id = 7;
    c.authToken = "tok";
    c.object = "core_service";
    c.method = "loadModule";
    c.args.push_back(RpcValue{std::string{"chat"}});
    c.args.push_back(RpcValue{int64_t{42}});
    c.args.push_back(RpcValue{true});

    auto dec = std::get<CallMessage>(roundtrip(codec, AnyMessage{c}));
    EXPECT_EQ(dec.id, 7u);
    EXPECT_EQ(dec.authToken, "tok");
    EXPECT_EQ(dec.object, "core_service");
    EXPECT_EQ(dec.method, "loadModule");
    ASSERT_EQ(dec.args.size(), 3u);
    EXPECT_EQ(dec.args[0].asString(), "chat");
    EXPECT_EQ(dec.args[1].asInt(), 42);
    EXPECT_EQ(dec.args[2].asBool(), true);
}

// THE CALLER DOCUMENT SURVIVES THE WIRE, and is ABSENT when nobody set one.
//
// A relay is not identifiable by its token: the Web container presents the
// relayed module's OWN root credential on every call it forwards, so a page
// that derives the caller from that token names itself for every caller in the
// fleet (logos-workspace#129). `caller` is how the relay says who it is
// relaying for, and the two halves below are both load-bearing — the value has
// to arrive intact, and a consumer that sets nothing has to emit exactly the
// frame it emitted before the field existed.
TEST(JsonCodecTest, CallMessageCarriesTheCallerDocument)
{
    JsonCodec codec;
    CallMessage c;
    c.id = 8;
    c.authToken = "tok";
    c.object = "keystore_module";
    c.method = "create_unrelated_account";
    c.caller = R"({"kind":"module","name":"wallet_ui"})";

    auto dec = std::get<CallMessage>(roundtrip(codec, AnyMessage{c}));
    EXPECT_EQ(dec.caller, R"({"kind":"module","name":"wallet_ui"})");
}

TEST(JsonCodecTest, ACallWithNoCallerDoesNotCarryTheKeyAtAll)
{
    JsonCodec codec;
    CallMessage c;
    c.id = 9;
    c.authToken = "tok";
    c.object = "js_counter";
    c.method = "add";

    // Not merely "decodes to empty": the KEY must be missing, because that is
    // what makes the frame identical to a pre-#129 one on a wire a foreign
    // implementation may be strict about.
    const auto bytes = codec.encode(AnyMessage{c});
    const std::string text(bytes.begin(), bytes.end());
    EXPECT_EQ(text.find("caller"), std::string::npos) << text;

    auto dec = std::get<CallMessage>(roundtrip(codec, AnyMessage{c}));
    EXPECT_TRUE(dec.caller.empty());
}

TEST(JsonCodecTest, ResultOk)
{
    JsonCodec codec;
    ResultMessage r;
    r.id = 99;
    r.ok = true;
    RpcMap m;
    m.emplace("status", RpcValue{std::string{"ok"}});
    m.emplace("count", RpcValue{int64_t{3}});
    r.value = RpcValue{std::move(m)};

    auto dec = std::get<ResultMessage>(roundtrip(codec, AnyMessage{r}));
    EXPECT_EQ(dec.id, 99u);
    EXPECT_TRUE(dec.ok);
    ASSERT_TRUE(dec.value.isMap());
    EXPECT_EQ(dec.value.asMap().at("status").asString(), "ok");
    EXPECT_EQ(dec.value.asMap().at("count").asInt(), 3);
}

TEST(JsonCodecTest, ResultError)
{
    JsonCodec codec;
    ResultMessage r;
    r.id = 1;
    r.ok = false;
    r.err = "module not loaded";
    r.errCode = "MODULE_NOT_LOADED";

    auto dec = std::get<ResultMessage>(roundtrip(codec, AnyMessage{r}));
    EXPECT_FALSE(dec.ok);
    EXPECT_EQ(dec.err, "module not loaded");
    EXPECT_EQ(dec.errCode, "MODULE_NOT_LOADED");
}

TEST(JsonCodecTest, EventWithNestedData)
{
    JsonCodec codec;
    EventMessage e;
    e.object = "core_service";
    e.eventName = "module_event";
    RpcList inner;
    inner.items.push_back(RpcValue{std::string{"nested"}});
    inner.items.push_back(RpcValue{double{3.14}});
    e.data.push_back(RpcValue{std::string{"testEvent"}});
    e.data.push_back(RpcValue{std::move(inner)});

    auto dec = std::get<EventMessage>(roundtrip(codec, AnyMessage{e}));
    ASSERT_EQ(dec.data.size(), 2u);
    EXPECT_EQ(dec.data[0].asString(), "testEvent");
    ASSERT_TRUE(dec.data[1].isList());
    const auto& il = dec.data[1].asList().items;
    ASSERT_EQ(il.size(), 2u);
    EXPECT_EQ(il[0].asString(), "nested");
    EXPECT_DOUBLE_EQ(il[1].asDouble(), 3.14);
}

TEST(JsonCodecTest, BytesRoundTrip)
{
    JsonCodec codec;
    CallMessage c;
    c.id = 3;
    c.object = "storage";
    c.method = "write";
    RpcBytes bytes;
    bytes.data = {0x00, 0x01, 0xfe, 0xff, 0xff, 0x42};
    c.args.push_back(RpcValue{std::move(bytes)});

    auto dec = std::get<CallMessage>(roundtrip(codec, AnyMessage{c}));
    ASSERT_EQ(dec.args.size(), 1u);
    ASSERT_TRUE(dec.args[0].isBytes());
    const auto& back = dec.args[0].asBytes().data;
    std::vector<uint8_t> expect = {0x00, 0x01, 0xfe, 0xff, 0xff, 0x42};
    EXPECT_EQ(back, expect);
}

TEST(JsonCodecTest, SubscribeUnsubscribe)
{
    JsonCodec codec;

    SubscribeMessage s;
    s.object = "x";
    s.eventName = "e";
    auto ds = std::get<SubscribeMessage>(roundtrip(codec, AnyMessage{s}));
    EXPECT_EQ(ds.object, "x");
    EXPECT_EQ(ds.eventName, "e");

    UnsubscribeMessage u;
    u.object = "x";
    u.eventName = "";  // wildcard
    auto du = std::get<UnsubscribeMessage>(roundtrip(codec, AnyMessage{u}));
    EXPECT_EQ(du.object, "x");
    EXPECT_EQ(du.eventName, "");
}

TEST(JsonCodecTest, TokenMessage)
{
    JsonCodec codec;
    TokenMessage t;
    t.authToken = "ca";
    t.moduleName = "chat";
    t.token = "secret";

    auto dec = std::get<TokenMessage>(roundtrip(codec, AnyMessage{t}));
    EXPECT_EQ(dec.authToken, "ca");
    EXPECT_EQ(dec.moduleName, "chat");
    EXPECT_EQ(dec.token, "secret");
}

TEST(JsonCodecTest, MethodsRequestAndResult)
{
    JsonCodec codec;

    MethodsMessage q;
    q.id = 5;
    q.authToken = "t";
    q.object = "obj";
    auto dq = std::get<MethodsMessage>(roundtrip(codec, AnyMessage{q}));
    EXPECT_EQ(dq.id, 5u);
    EXPECT_EQ(dq.object, "obj");

    MethodsResultMessage r;
    r.id = 5;
    r.ok = true;
    MethodMetadata m;
    m.name = "foo";
    m.signature = "foo(QString)";
    m.returnType = "int";
    r.methods.push_back(std::move(m));
    auto dr = std::get<MethodsResultMessage>(roundtrip(codec, AnyMessage{r}));
    EXPECT_EQ(dr.id, 5u);
    EXPECT_TRUE(dr.ok);
    ASSERT_EQ(dr.methods.size(), 1u);
    EXPECT_EQ(dr.methods[0].name, "foo");
    EXPECT_EQ(dr.methods[0].signature, "foo(QString)");
    EXPECT_EQ(dr.methods[0].returnType, "int");
}

TEST(JsonCodecTest, NullAndDoubleRoundTrip)
{
    JsonCodec codec;
    CallMessage c;
    c.id = 1;
    c.object = "o";
    c.method = "m";
    c.args.push_back(RpcValue{std::monostate{}});
    c.args.push_back(RpcValue{double{-2.5}});

    auto dec = std::get<CallMessage>(roundtrip(codec, AnyMessage{c}));
    ASSERT_EQ(dec.args.size(), 2u);
    EXPECT_TRUE(dec.args[0].isNull());
    ASSERT_TRUE(dec.args[1].isDouble());
    EXPECT_DOUBLE_EQ(dec.args[1].asDouble(), -2.5);
}
