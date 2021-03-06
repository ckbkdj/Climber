﻿//
// Created by Climber on 2020/6/9.
//

#include "utils.h"
#include "Climber.h"
#include "Configuration.h"
#include "ClientManager.h"
#include "Paths.h"
#include "pac.h"

#ifdef CLIMBER_WINDOWS

#define CLIMBER_PRIVOXY_BIN ("climber_privoxy.exe")

#elif defined CLIMBER_DARWIN

#define CLIMBER_PRIVOXY_BIN ("climber_privoxy")

#endif

Climber *Climber::s_instance = nullptr;

void Climber::Init() {
    if (s_instance == nullptr) {
        s_instance = new Climber();
    }
}

void Climber::Destroy() {
    if (s_instance != nullptr) {
        s_instance->KillPacServer(false);
        s_instance->Stop();
        delete s_instance;
        s_instance = nullptr;
    }
}

Climber &Climber::GetInstance() {
    return *s_instance;
}

Climber::Climber() {

}

bool Climber::IsRunning() const {
    return m_running;
}

void Climber::Stop() {
    ClearSystemProxy();
    KillClient();
    KillPrivoxy();
    KillPacServer();
    m_running = false;
}

void Climber::Restart() {
    Stop();

    if (isPortInUse(CONFIGURATION.GetSocksPort())) {
        wxMessageDialog(nullptr,
                        wxString::Format(_("Socks port %d is in use by another program!"),
                                         CONFIGURATION.GetSocksPort()),
                        _("Warning")).ShowModal();
        return;
    }
    if (isPortInUse(CONFIGURATION.GetHttpPort())) {
        wxMessageDialog(nullptr,
                        wxString::Format(_("Http port %d is in use by another program!"),
                                         CONFIGURATION.GetHttpPort()),
                        _("Warning")).ShowModal();
        return;
    }
    if (isPortInUse(CONFIGURATION.GetPacPort())) {
        wxMessageDialog(nullptr,
                        wxString::Format(_("PAC port %d is in use by another program!"),
                                         CONFIGURATION.GetPacPort()),
                        _("Warning")).ShowModal();
        return;
    }

    if (!RestartClient()) return;

    RestartPrivoxy();
    RestartPacServer();
    ResetSystemProxy();

    m_running = true;
}

bool Climber::RestartClient() {
    KillClient();

    int index = CONFIGURATION.GetSelectedServerIndex();
    if (index == -1) {
        wxMessageDialog(
                nullptr,
                _("Can't start Climber, please select a server first."),
                _("Error")).ShowModal();
        return false;
    }
    const auto *client = CLIENT_MANAGER.Get(index);
    if (client == nullptr) {
        return false;
    }

    client->Start();
    return true;
}

void Climber::KillClient() {
    BaseClient::StopAll();
}

void Climber::RestartPrivoxy() {
    KillPrivoxy();

    auto privoxyTmpConfigFile = Paths::GetTmpDirFile("privoxy.conf");
    auto privoxyLogFile = Paths::GetLogDirFile("privoxy.log");
    auto privoxyConfigTplFile = Paths::GetAssetsDirFile("privoxy.conf");
    auto config = readTextFile(privoxyConfigTplFile, "", true);
    auto listenAddr = CONFIGURATION.GetShareOnLan() ? "0.0.0.0" : "127.0.0.1";
    auto listenPort = CONFIGURATION.GetHttpPort();
    config.Replace("__PRIVOXY_BIND_IP__", listenAddr);
    config.Replace("__PRIVOXY_BIND_PORT__", wxString::Format("%d", listenPort));
    config.Replace("__PRIVOXY_LOG_FILE__", privoxyLogFile);
    config.Replace("__SOCKS_HOST__", "127.0.0.1");
    config.Replace("__SOCKS_PORT__", wxString::Format("%d", CONFIGURATION.GetSocksPort()));
    writeTextFile(privoxyTmpConfigFile, config);

    auto privoxy = Paths::GetBinDirFile(CLIMBER_PRIVOXY_BIN);
    if (wxFileExists(privoxyLogFile)) {
        wxRemoveFile(privoxyLogFile);
    }
    wxExecute(wxString::Format("\"%s\" \"%s\"", privoxy, privoxyTmpConfigFile),
              wxEXEC_ASYNC | wxEXEC_HIDE_CONSOLE);
    wxLogMessage("Privoxy started at %s:%d", listenAddr, listenPort);
}

void Climber::KillPrivoxy() {
    killProcessByName(CLIMBER_PRIVOXY_BIN);
    wxLogMessage("Privoxy stopped");
}

void Climber::RestartPacServer() {
    if (m_pacServerThread != nullptr) {
        KillPacServer();
    }

    m_pacServerThread = new std::thread([&]() {
        auto pacTpl = readTextFile(Paths::GetAssetsDirFile("proxy.pac"), "", true);
        m_pacServer = new httplib::Server();
        m_pacServer->Get("/proxy.pac", [&](const httplib::Request &req, httplib::Response &res) {
            std::string ip = "127.0.0.1";
            if (req.headers.find("Host") != req.headers.end()) {
                auto host = req.headers.find("Host")->second;
                auto startPos = host.find(':');
                if (startPos != wxNOT_FOUND) {
                    ip = host.substr(0, startPos);
                } else {
                    ip = host;
                }
            }
            wxString pac = getPacScript(pacTpl, ip, CONFIGURATION.GetSocksPort(), CONFIGURATION.GetHttpPort());
            res.set_content(pac.ToStdString(), "application/x-ns-proxy-autoconfig");
        });
        wxLogMessage("PAC server start at %s:%d",
                     CONFIGURATION.GetShareOnLan() ? "0.0.0.0" : "127.0.0.1",
                     CONFIGURATION.GetPacPort());
        bool ok = m_pacServer->listen(CONFIGURATION.GetShareOnLan() ? "0.0.0.0" : "127.0.0.1",
                                      CONFIGURATION.GetPacPort());
        if (!ok) {
            wxLogMessage("PAC server error listening");
        }
    });
}

void Climber::KillPacServer(bool joinThread) {
    if (m_pacServer == nullptr) return;

    m_pacServer->stop();

    if (joinThread && m_pacServerThread->joinable()) {
        m_pacServerThread->join();
        delete m_pacServerThread;
    }

    delete m_pacServer;
    m_pacServer = nullptr;
    m_pacServerThread = nullptr;

    wxLogMessage("PAC server stopped");
}

void Climber::ResetSystemProxy() {
    const auto &mode = CONFIGURATION.GetProxyMode();
    if (mode == PROXY_MODE_DIRECT) {
        clearProxy();
    } else if (mode == PROXY_MODE_PAC) {
        setProxyPac(wxString::Format("http://127.0.0.1:%d/proxy.pac", CONFIGURATION.GetPacPort()),
                    CONFIGURATION.GetProxyBypass());
    } else if (mode == PROXY_MODE_GLOBAL) {
        setProxy("127.0.0.1", CONFIGURATION.GetSocksPort(),
                 "127.0.0.1", CONFIGURATION.GetHttpPort(),
                 CONFIGURATION.GetProxyBypass());
    }
}

void Climber::ClearSystemProxy() {
    clearProxy();
}