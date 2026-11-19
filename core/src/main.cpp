#include <signal.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include "vmw.hpp"

using namespace vmw;

namespace {

void onSignal(int) {
  // Exiting directly is enough: all state on disk is written atomically.
  _exit(0);
}

const char* usage =
    "VM Watcher\n\n"
    "  vmwatcher serve  [--bind ADDR] [--port N] [--data DIR] [--web DIR] [--allow HOST[,HOST]] [--uri URI]\n"
    "                   [--tls-cert CHAIN.pem --tls-key KEY.pem] [--demo]\n"
    "      Central service: web UI, settings, sign-in. Reaches hypervisors locally,\n"
    "      through agents, or through remote libvirt URIs.\n"
    "  vmwatcher agent  [--bind ADDR] [--port N] [--data DIR] [--uri URI] [--demo]\n"
    "      Per-host agent: gives the central service full access to this hypervisor.\n"
    "  vmwatcher agent-token [--data DIR]     Print this agent's token.\n"
    "  vmwatcher passwd [--data DIR]          Set the viewer password (read from standard input).\n"
    "  vmwatcher version\n\n"
    "Environment: VMWATCHER_BIND, VMWATCHER_PORT, VMWATCHER_DATA, VMWATCHER_WEB, VMWATCHER_ALLOWED_HOSTS.\n";

std::string env(const char* name, const char* legacy, const std::string& fallback) {
  const char* value = getenv(name);
  if ((!value || !*value) && legacy) value = getenv(legacy);
  return value && *value ? value : fallback;
}

}  // namespace

int main(int argc, char** argv) {
  std::string command = argc > 1 ? argv[1] : "";
  if (command == "version" || command == "--version") {
    printf("vmwatcher %s\n", kVersion);
    return 0;
  }
  if (command != "serve" && command != "agent" && command != "agent-token" && command != "passwd") {
    fputs(usage, command == "help" || command == "--help" ? stdout : stderr);
    return command == "help" || command == "--help" ? 0 : 2;
  }
  bool agent = command == "agent" || command == "agent-token";
  Options options;
  // The older VIEWER_* names keep an existing service unit working.
  options.bind = env("VMWATCHER_BIND", "VIEWER_BIND", "127.0.0.1");
  options.port = atoi(env("VMWATCHER_PORT", "PORT", agent ? "6089" : "6088").c_str());
  options.data = env("VMWATCHER_DATA", "VIEWER_DATA", agent ? "./agent-data" : "./data");
  options.web = env("VMWATCHER_WEB", nullptr, "./web");
  std::string allowed = env("VMWATCHER_ALLOWED_HOSTS", "VIEWER_ALLOWED_HOSTS", "");
  for (int i = 2; i < argc; i++) {
    std::string flag = argv[i];
    auto value = [&]() -> std::string {
      if (i + 1 >= argc) {
        fprintf(stderr, "%s needs a value\n", flag.c_str());
        exit(2);
      }
      return argv[++i];
    };
    if (flag == "--bind") options.bind = value();
    else if (flag == "--port") options.port = atoi(value().c_str());
    else if (flag == "--data") options.data = value();
    else if (flag == "--web") options.web = value();
    else if (flag == "--uri") options.uri = value();
    else if (flag == "--allow") allowed = value();
    else if (flag == "--tls-cert") options.tlsCert = value();
    else if (flag == "--tls-key") options.tlsKey = value();
    else if (flag == "--demo") options.demo = true;
    else {
      fprintf(stderr, "Unknown option %s\n\n%s", flag.c_str(), usage);
      return 2;
    }
  }
  for (const auto& host : split(allowed, ','))
    if (!trim(host).empty()) options.allowedHosts.push_back(trim(host));
  while (options.data.size() > 1 && options.data.back() == '/') options.data.pop_back();
  try {
    if (command == "agent-token") {
      printf("%s\n", agentToken(options.data, false).c_str());
      return 0;
    }
    if (command == "passwd") {
      std::string password;
      std::getline(std::cin, password);
      setPassword(options.data, password);
      fprintf(stderr, "Password updated. Restart the service to apply it.\n");
      return 0;
    }
    if (options.port < 1 || options.port > 65535) throw std::runtime_error("Invalid port");
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);
    std::unique_ptr<Service> service = agent ? makeAgent(options) : makeCentral(options);
    Server server([&](Request& req, Conn& conn) { return service->handle(req, conn); });
    if (options.tlsCert.empty() != options.tlsKey.empty()) throw std::runtime_error("--tls-cert and --tls-key go together");
    if (agent && !options.tlsCert.empty()) throw std::runtime_error("The agent does not serve HTTPS yet");
    if (!options.tlsCert.empty()) server.enableTls(options.tlsCert, options.tlsKey);
    int port = server.listen(options.bind, options.port);
    logLine(std::string("VM Watcher ") + kVersion + (agent ? " agent" : " central service") + " listening on " +
            (options.tlsCert.empty() ? "http://" : "https://") + options.bind + ":" + std::to_string(port));
    if (agent) logLine("Add this host in Settings with the token from: vmwatcher agent-token --data " + options.data);
    server.run();
  } catch (const std::exception& e) {
    fprintf(stderr, "vmwatcher: %s\n", e.what());
    return 1;
  }
  return 0;
}
