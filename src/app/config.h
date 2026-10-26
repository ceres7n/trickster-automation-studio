#pragma once
#include <string>
#include <vector>

namespace nj {

struct Account {
    std::string login;   // what the launcher passes; the AuthServer resolves it to an account
    std::string label;
};

// tas.ini next to the exe (falls back to tas.ini.default).
struct Config {
    std::string iniPath;
    std::string dllPath;          // tas_hook.dll next to the exe

    // [Client]
    std::string tricksterPath;
    std::string originalIp;                     // the server IP baked into the client: every copy becomes LoginIP
    bool        patchLoginGlobals = false;      // write the older build's login IP/port globals
    int         unpackWaitMs = 3000;            // before patching the IP + injecting
    std::string preLaunch;                      // command run hidden before each launch

    // [Server]
    std::string loginIp = "127.0.0.1";
    int         loginPort = 9980;
    std::string groupId = "0";
    std::string token = "blank0|0";

    // [Data] - the server under test
    std::string libConfig;                      // its libconfig XML: every game table (empty = the first conf\*.xml next to the exe)
    std::string dataDir;                        // its data folder: map\*.md3, gulim13.fnt2 (empty = conf\ next to the exe)
    std::string mapDir;                         // the .md3 maps when they are elsewhere (empty = <DataDir>\map)
    std::string gameDbDir;                      // (optional) a folder of ProjectPAULA-style db\*.yml when there is no XML

    // [Bot]
    bool        notifyStop = true;              // a Windows notification + the taskbar flashing when a bot ends / stops

    // [Accounts]
    std::vector<Account> accounts;
};

bool load_config(Config& cfg, std::string& err);
// Writes every setting to tas.ini next to the exe (made from tas.ini.default the first time).
bool save_config(const Config& cfg, std::string& err);

} // namespace nj
