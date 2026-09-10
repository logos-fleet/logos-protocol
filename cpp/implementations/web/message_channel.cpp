#include "message_channel.h"

#include <mutex>

namespace logos::web {

namespace {

std::mutex& factoryMutex()
{
    static std::mutex mu;
    return mu;
}

MessageChannelFactory& factoryStorage()
{
    static MessageChannelFactory factory;
    return factory;
}

} // namespace

void setMessageChannelFactory(MessageChannelFactory factory)
{
    std::lock_guard<std::mutex> g(factoryMutex());
    factoryStorage() = std::move(factory);
}

MessageChannelPtr makeMessageChannel()
{
    // Copied out under the lock and invoked with it released: the factory is
    // host code (it may build a webview bridge, which may itself take locks),
    // and holding a process-global mutex across it is how a startup deadlock
    // gets built.
    MessageChannelFactory factory;
    {
        std::lock_guard<std::mutex> g(factoryMutex());
        factory = factoryStorage();
    }
    if (!factory) return nullptr;
    return factory();
}

} // namespace logos::web
