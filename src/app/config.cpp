#include "app/config.h"
#include "app/text_util.h"
#include <windows.h>

namespace nj {

namespace {
std::string ini_str(const std::string& ini, const char* sec, const char* key, const std::string& def)
{
    char buf[1024] = {};
    GetPrivateProfileStringA(sec, key, def.c_str(), buf, sizeof(buf), ini.c_str());
    return buf;
}
}

bool load_config(Config& cfg, std::string& err)
{
    Config c;
    const std::string dir = exe_dir();
    c.iniPath = dir + "tas.ini";
    if (!file_exists(c.iniPath)) c.iniPath = dir + "tas.ini.default";
    c.dllPath = dir + "tas_hook.dll";
    if (!file_exists(c.iniPath)) {
        err = "tas.ini not found next to TricksterAutomationStudio.exe";
        cfg = c;
        return false;
    }

    const std::string& ini = c.iniPath;
    c.tricksterPath = ini_str(ini, "Client", "TricksterPath", "");
    c.originalIp    = ini_str(ini, "Client", "OriginalIP", c.originalIp);
    c.patchLoginGlobals = GetPrivateProfileIntA("Client", "PatchLoginGlobals", 0, ini.c_str()) != 0;
    c.unpackWaitMs  = GetPrivateProfileIntA("Client", "UnpackWaitMs", c.unpackWaitMs, ini.c_str());
    c.preLaunch     = ini_str(ini, "Client", "PreLaunch", "");
    c.loginIp       = ini_str(ini, "Server", "LoginIP", c.loginIp);
    c.loginPort     = GetPrivateProfileIntA("Server", "LoginPort", c.loginPort, ini.c_str());
    c.groupId       = ini_str(ini, "Server", "GroupID", c.groupId);
    c.token         = ini_str(ini, "Server", "Token", c.token);
    c.gameDbDir     = ini_str(ini, "Data", "GameDbDir", "");
    c.mapDir        = ini_str(ini, "Data", "MapDir", "");
    c.libConfig     = ini_str(ini, "Data", "LibConfig", "");
    c.dataDir       = ini_str(ini, "Data", "DataDir", "");
    c.notifyStop    = GetPrivateProfileIntA("Bot", "Notify", 1, ini.c_str()) != 0;

    // [Accounts]  login=label, in file order
    std::vector<char> sec(32768);
    DWORD n = GetPrivateProfileSectionA("Accounts", sec.data(), (DWORD)sec.size(), ini.c_str());
    for (const char* p = sec.data(); p < sec.data() + n && *p; p += strlen(p) + 1) {
        std::string line = p;
        size_t eq = line.find('=');
        if (eq == std::string::npos || line[0] == ';') continue;
        c.accounts.push_back({ line.substr(0, eq), line.substr(eq + 1) });
    }
    if (c.accounts.empty()) c.accounts.push_back({ "1", "account 1" });

    if (c.tricksterPath.empty()) err = "[Client] TricksterPath is not set";
    cfg = c;
    return err.empty();
}

bool save_config(const Config& c, std::string& err)
{
    const std::string dir = exe_dir();
    const std::string ini = dir + "tas.ini";
    // the first save starts from the default file, so its comments stay
    if (!file_exists(ini) && file_exists(dir + "tas.ini.default") &&
        !CopyFileA((dir + "tas.ini.default").c_str(), ini.c_str(), TRUE)) {
        err = "cannot create " + ini;
        return false;
    }
    bool ok = true;
    auto put = [&](const char* sec, const char* key, const std::string& v) {
        ok &= WritePrivateProfileStringA(sec, key, v.c_str(), ini.c_str()) != 0;
    };
    put("Client", "TricksterPath", c.tricksterPath);
    put("Client", "OriginalIP", c.originalIp);
    put("Client", "PatchLoginGlobals", c.patchLoginGlobals ? "1" : "0");
    put("Client", "UnpackWaitMs", std::to_string(c.unpackWaitMs));
    put("Client", "PreLaunch", c.preLaunch);
    put("Server", "LoginIP", c.loginIp);
    put("Server", "LoginPort", std::to_string(c.loginPort));
    put("Server", "GroupID", c.groupId);
    put("Server", "Token", c.token);
    put("Data", "GameDbDir", c.gameDbDir);
    put("Data", "MapDir", c.mapDir);
    put("Data", "LibConfig", c.libConfig);
    put("Data", "DataDir", c.dataDir);
    put("Bot", "Notify", c.notifyStop ? "1" : "0");
    // [Accounts]: the whole section, "login=label\0...\0\0"
    std::string sec;
    for (const Account& a : c.accounts)
        if (!a.login.empty()) sec += a.login + "=" + (a.label.empty() ? a.login : a.label) + '\0';
    sec += '\0';
    ok &= WritePrivateProfileSectionA("Accounts", sec.c_str(), ini.c_str()) != 0;
    if (!ok) err = "cannot write " + ini;
    return ok;
}

} // namespace nj
