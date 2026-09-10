#include "logos_transport_config_json.h"

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace logos {

namespace {

const char* protocolToString(LogosProtocol p)
{
    switch (p) {
    case LogosProtocol::LocalSocket: return "local";
    case LogosProtocol::Tcp:         return "tcp";
    case LogosProtocol::TcpSsl:      return "tcp_ssl";
    case LogosProtocol::Web:         return "web";
    }
    return "local";
}

LogosProtocol protocolFromString(const std::string& s)
{
    if (s == "tcp")     return LogosProtocol::Tcp;
    if (s == "tcp_ssl") return LogosProtocol::TcpSsl;
    if (s == "web")     return LogosProtocol::Web;
    return LogosProtocol::LocalSocket;
}

const char* codecToString(LogosWireCodec c)
{
    switch (c) {
    case LogosWireCodec::Cbor: return "cbor";
    case LogosWireCodec::Json: return "json";
    }
    return "json";
}

LogosWireCodec codecFromString(const std::string& s)
{
    if (s == "cbor") return LogosWireCodec::Cbor;
    return LogosWireCodec::Json;
}

} // namespace

std::string transportSetToJsonString(const LogosTransportSet& set)
{
    json arr = json::array();
    for (const auto& cfg : set) {
        json o;
        o["protocol"] = protocolToString(cfg.protocol);
        // LocalSocket has a derived socket path and Web has an injected
        // channel; neither has an address a peer could be told about, and Web
        // has exactly one encoding (see web_message_codec.h), so emitting
        // host/port/codec for either would be inventing configuration.
        if (cfg.protocol != LogosProtocol::LocalSocket
            && cfg.protocol != LogosProtocol::Web) {
            o["host"]  = cfg.host;
            o["port"]  = cfg.port;
            o["codec"] = codecToString(cfg.codec);
        }
        if (cfg.protocol == LogosProtocol::TcpSsl) {
            // Cert/key paths intentionally included — this serialization
            // is the parent → child handoff so the child knows what
            // files to load when binding its TLS listener.
            if (!cfg.caFile.empty())   o["ca_file"]   = cfg.caFile;
            if (!cfg.certFile.empty()) o["cert_file"] = cfg.certFile;
            if (!cfg.keyFile.empty())  o["key_file"]  = cfg.keyFile;
            o["verify_peer"] = cfg.verifyPeer;
        }
        arr.push_back(std::move(o));
    }
    return arr.dump();  // single-line, suitable for CLI / env-var passing
}

LogosTransportSet transportSetFromJsonString(const std::string& jsonStr)
{
    LogosTransportSet out;
    if (jsonStr.empty()) return out;

    json arr;
    try {
        arr = json::parse(jsonStr);
    } catch (const json::exception&) {
        return out;
    }
    if (!arr.is_array()) return out;

    for (const auto& o : arr) {
        if (!o.is_object()) continue;
        LogosTransportConfig cfg;
        cfg.protocol = protocolFromString(o.value("protocol", std::string{"local"}));
        cfg.host = o.value("host", std::string{"127.0.0.1"});
        const int rawPort = o.value("port", 0);
        if (rawPort < 0 || rawPort > 0xFFFF) continue;
        cfg.port = static_cast<uint16_t>(rawPort);
        cfg.codec = codecFromString(o.value("codec", std::string{"json"}));
        cfg.caFile   = o.value("ca_file",   std::string{});
        cfg.certFile = o.value("cert_file", std::string{});
        cfg.keyFile  = o.value("key_file",  std::string{});
        cfg.verifyPeer = o.value("verify_peer", true);
        out.push_back(std::move(cfg));
    }
    return out;
}

} // namespace logos
