#include "blockdb.h"
#include <khook.hpp>

using json = nlohmann::json;

static std::string SafeDump(const json& j)
{
    try { return j.dump(-1, ' ', false, json::error_handler_t::replace); }
    catch (...) { return "{}"; }
}

static void EnsureDirFor(const char* filepath)
{
    if (!g_pFullFileSystem) return;
    std::string p = filepath;
    size_t lastSlash = p.find_last_of('/');
    if (lastSlash == std::string::npos) return;
    std::string dir = p.substr(0, lastSlash);
    g_pFullFileSystem->CreateDirHierarchy(dir.c_str(), "GAME");
}

BlockDB g_BlockDB;
PLUGIN_EXPOSE(BlockDB, g_BlockDB);

IVEngineServer2*    engine              = nullptr;
CGameEntitySystem*  g_pGameEntitySystem = nullptr;
CEntitySystem*      g_pEntitySystem     = nullptr;
CGlobalVars*        gpGlobals           = nullptr;

IUtilsApi*   g_pUtils      = nullptr;
IPlayersApi* g_pPlayersApi = nullptr;
IAdminApi*   g_pAdminApi   = nullptr;

ISteamHTTP* g_http = nullptr;

KHook::Return<void> Hook_GameServerSteamAPIActivated(IServerGameDLL* pServer);
KHook::Virtual<IServerGameDLL, void> g_hkGameServerSteamAPIActivated(&IServerGameDLL::GameServerSteamAPIActivated, &Hook_GameServerSteamAPIActivated, nullptr);

static const char* kApiBase    = "https://api.blockdb.net";
static const char* kKeyFile    = "addons/data/blockdb_data.ini";
static const char* kBanIdFile  = "addons/data/blockdb_bans.ini";

static std::string LogFilePath()
{
    time_t now = time(nullptr);
    struct tm tm; localtime_r(&now, &tm);
    char d[32];
    snprintf(d, sizeof(d), "%02d_%02d_%04d",
        tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900);
    char buf[512];
    g_SMAPI->PathFormat(buf, sizeof(buf),
        "%s/addons/logs/blockdb_%s.txt", g_SMAPI->GetBaseDir(), d);
    return buf;
}

static std::string g_apiKey;
static uint64      g_serverSteamId = 0;

static std::map<std::string, std::string> g_phrases;

static const char* Tr(const char* key, const char* fallback = "")
{
    auto it = g_phrases.find(key);
    return (it != g_phrases.end() && !it->second.empty()) ? it->second.c_str() : fallback;
}

static void LoadPhrases()
{
    g_phrases.clear();
    KeyValues* kv = new KeyValues("Phrases");
    if (!kv->LoadFromFile(g_pFullFileSystem, "addons/translations/blockdb.phrases.txt")) {
        kv->deleteThis();
        return;
    }
    const char* lang = g_pUtils ? g_pUtils->GetLanguage() : "en";
    for (KeyValues* p = kv->GetFirstTrueSubKey(); p; p = p->GetNextTrueSubKey()) {
        const char* v = p->GetString(lang, nullptr);
        if (!v || !*v) v = p->GetString("en", nullptr);
        if (v) g_phrases[p->GetName()] = v;
    }
    kv->deleteThis();
}

static void FileLog(const char* prefix, const char* text)
{
    static bool s_dirOk = false;
    if (!s_dirOk) {
        if (g_pFullFileSystem)
            g_pFullFileSystem->CreateDirHierarchy("addons/logs", "GAME");
        s_dirOk = true;
    }
    std::string path = LogFilePath();
    FILE* fp = fopen(path.c_str(), "a");
    if (!fp) return;
    time_t now = time(nullptr);
    struct tm tm; localtime_r(&now, &tm);
    char ts[32]; strftime(ts, sizeof(ts), "%H:%M:%S", &tm);
    fprintf(fp, "[%s] %s %s\n", ts, prefix, text);
    fclose(fp);
}

static void LogFile(const char* fmt, ...)
{
    char body[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    FileLog("INFO", body);
}

static void LogErr(const char* fmt, ...)
{
    char body[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    ConColorMsg(Color(120, 180, 255, 255), "[BlockDB] ");
    ConColorMsg(Color(255, 80, 80, 255), "%s\n", body);
    FileLog("ERR ", body);
}

static void LogBox(int color, const std::vector<std::string>& lines)
{
    Color clr =
        color == 0 ? Color(60, 220, 60, 255) :
        color == 1 ? Color(255, 80, 80, 255) :
                     Color(255, 200, 60, 255);

    ConColorMsg(Color(255,255,255,255), "[BlockDB] +----------------------------+\n");
    FileLog("BOX ", "+----------------------------+");
    for (auto& s : lines) {
        std::string padded = "|   " + s;
        if (padded.size() > 29) padded = padded.substr(0, 26) + "...";
        while (padded.size() < 29) padded += " ";
        padded += "|";
        ConColorMsg(Color(255,255,255,255), "[BlockDB] ");
        ConColorMsg(clr, "%s\n", padded.c_str());
        FileLog("BOX ", padded.c_str());
    }
    ConColorMsg(Color(255,255,255,255), "[BlockDB] +----------------------------+\n");
    FileLog("BOX ", "+----------------------------+");
}

static bool ReadFileAll(const char* path, std::string& out)
{
    FILE* fp = fopen(path, "rb");
    if (!fp) return false;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0) { fclose(fp); return false; }
    out.resize((size_t)sz);
    size_t got = fread(&out[0], 1, (size_t)sz, fp);
    fclose(fp);
    out.resize(got);
    return true;
}

static bool WriteFileAll(const char* path, const std::string& data)
{
    EnsureDirFor(path);
    FILE* fp = fopen(path, "wb");
    if (!fp) return false;
    size_t w = fwrite(data.data(), 1, data.size(), fp);
    fclose(fp);
    return w == data.size();
}

static void LoadApiKey()
{
    g_apiKey.clear();
    KeyValues* kv = new KeyValues("BlockDB");
    if (!kv->LoadFromFile(g_pFullFileSystem, kKeyFile)) {
        kv->deleteThis();
        LogBox(2, {
            "API KEY NOT SET",
            "",
            "Run 'db_pair <code>'",
            "to pair this server."
        });
        return;
    }
    const char* k = kv->GetString("api_key", "");
    if (k && *k) g_apiKey = k;
    kv->deleteThis();

    if (g_apiKey.empty())
        LogBox(1, { "API KEY FILE EMPTY", "Re-run 'db_pair <code>'" });
    else
        LogBox(0, { "API KEY LOADED" });
}

static void SaveApiKey(const std::string& key)
{
    EnsureDirFor(kKeyFile);
    KeyValues* kv = new KeyValues("BlockDB");
    kv->SetString("api_key", key.c_str());
    bool ok = kv->SaveToFile(g_pFullFileSystem, kKeyFile);
    kv->deleteThis();
    if (!ok) LogErr("Cannot write %s", kKeyFile);
    else     LogFile("API key saved to %s", kKeyFile);
}

static std::string GetServerHostname()
{
    if (g_pCVar) {
        ConVarRefAbstract var("hostname");
        if (var.IsValidRef()) {
            CUtlString s = var.GetString();
            if (!s.IsEmpty()) return std::string(s.Get());
        }
    }
    return "Unknown Server";
}

static std::string GetServerIp()
{
    if (auto* sgs = SteamGameServer()) {
        auto ip = sgs->GetPublicIP();
        if (ip.IsSet()) {
            uint32 v4 = ip.m_unIPv4;
            char buf[32];
            snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                (v4 >> 24) & 0xFF, (v4 >> 16) & 0xFF,
                (v4 >>  8) & 0xFF,  v4        & 0xFF);
            return buf;
        }
    }
    return "0.0.0.0";
}

static std::string GetServerPort()
{
    int port = 27015;
    FILE* fp = fopen("/proc/self/cmdline", "r");
    if (fp) {
        char cl[4096]; size_t l = fread(cl, 1, sizeof(cl) - 1, fp); fclose(fp);
        if (l > 0) {
            cl[l] = '\0';
            for (size_t i = 0; i < l; ++i) if (cl[i] == '\0') cl[i] = ' ';
            const char* keys[] = { "-port ", "+hostport ", "+port " };
            for (const char* k : keys) {
                char* p = strstr(cl, k);
                if (p) { int v = atoi(p + strlen(k)); if (v > 0) { port = v; break; } }
            }
        }
    }
    char buf[16]; snprintf(buf, sizeof(buf), "%d", port);
    return buf;
}

class AsyncHttpJob
{
public:
    using Callback = std::function<void(bool ok, int status, const std::string& body)>;

    static void Send(EHTTPMethod method, const std::string& url,
                     const std::string& body,
                     const std::vector<std::pair<std::string,std::string>>& headers,
                     Callback cb)
    {
        if (!g_http) { if (cb) cb(false, 0, std::string()); return; }

        auto* job = new AsyncHttpJob(std::move(cb));
        auto hReq = g_http->CreateHTTPRequest(method, url.c_str());
        g_http->SetHTTPRequestHeaderValue(hReq, "Content-Type", "application/json");
        for (auto& h : headers)
            g_http->SetHTTPRequestHeaderValue(hReq, h.first.c_str(), h.second.c_str());
        if (!body.empty())
            g_http->SetHTTPRequestRawPostBody(hReq, "application/json",
                (uint8*)body.data(), (uint32)body.size());

        SteamAPICall_t hCall = 0;
        if (!g_http->SendHTTPRequest(hReq, &hCall)) {
            g_http->ReleaseHTTPRequest(hReq);
            Callback c = std::move(job->m_cb);
            delete job;
            if (c) c(false, 0, std::string());
            return;
        }
        job->m_call.SetGameserverFlag();
        job->m_call.Set(hCall, job, &AsyncHttpJob::OnDone);
    }

private:
    explicit AsyncHttpJob(Callback cb) : m_cb(std::move(cb)) {}

    void OnDone(HTTPRequestCompleted_t* p, bool failed)
    {
        bool ok = false; int status = 0; std::string resp;
        if (!failed && p) {
            ok = true;
            status = (int)p->m_eStatusCode;
            uint32 size = 0;
            g_http->GetHTTPResponseBodySize(p->m_hRequest, &size);
            if (size > 0) {
                std::vector<uint8> buf(size + 1, 0);
                g_http->GetHTTPResponseBodyData(p->m_hRequest, buf.data(), size);
                resp.assign((char*)buf.data(), size);
            }
        }
        if (g_http && p) g_http->ReleaseHTTPRequest(p->m_hRequest);

        Callback cb = std::move(m_cb);
        delete this;
        if (cb) cb(ok, status, resp);
    }

    Callback m_cb;
    CCallResult<AsyncHttpJob, HTTPRequestCompleted_t> m_call;
};

static std::string Auth() { return std::string("Bearer ") + g_apiKey; }

static void DoPair(const std::string& code)
{
    std::string hostname = GetServerHostname();
    std::string ip       = GetServerIp();
    std::string port     = GetServerPort();
    json body = {
        {"hostname", hostname},
        {"code",     code},
        {"ip",       ip},
        {"port",     port}
    };
    LogFile("Pairing: hostname='%s' ip=%s port=%s code=%s ...",
        hostname.c_str(), ip.c_str(), port.c_str(), code.c_str());

    AsyncHttpJob::Send(k_EHTTPMethodPOST,
        std::string(kApiBase) + "/v1/projects/servers/pair",
        SafeDump(body), {},
        [](bool ok, int status, const std::string& resp) {
            if (!ok) { LogErr("Pair: HTTP timeout"); return; }
            if (status != 200) {
                const char* reason = "unknown";
                if (status == 400) reason = "invalid body";
                else if (status == 403) reason = "code invalid or expired (10 min)";
                else if (status == 500) reason = "internal server error";
                LogErr("Pair failed: HTTP %d (%s) response: %s",
                       status, reason, resp.c_str());
                return;
            }
            json j = json::parse(resp, nullptr, false);
            if (j.is_discarded() || !j.contains("api_key") || !j["api_key"].is_string()) {
                LogErr("Pair: response has no api_key: %s", resp.c_str());
                return;
            }
            std::string key = j["api_key"].get<std::string>();
            if (key.empty()) { LogErr("Pair: empty api_key"); return; }

            g_apiKey = key;
            SaveApiKey(key);
            LogBox(0, { "SERVER PAIRED OK", "BlockDB linked." });
        });
}

struct SubsCheckResult
{
    bool        blocked = false;
    std::string reason;
    int         notificationsCount = 0;
};

static void CheckSubscriptionsAsync(uint64 sid, const std::string& ip,
                                    std::function<void(const SubsCheckResult&)> cb)
{
    if (g_apiKey.empty() || !g_http) return;

    char url[512];
    if (!ip.empty()) {
        snprintf(url, sizeof(url),
            "%s/v1/subscriptions/check?steamid64=%llu&ip=%s",
            kApiBase, (unsigned long long)sid, ip.c_str());
    } else {
        snprintf(url, sizeof(url),
            "%s/v1/subscriptions/check?steamid64=%llu",
            kApiBase, (unsigned long long)sid);
    }

    uint64 sidCopy = sid;
    AsyncHttpJob::Send(k_EHTTPMethodGET, url, "",
        {{"Authorization", Auth()}},
        [sidCopy, cb](bool ok, int status, const std::string& resp) {
            if (!ok) {
                LogErr("SubsCheck %llu: HTTP timeout", (unsigned long long)sidCopy);
                return;
            }
            if (status < 200 || status > 299) {
                LogErr("SubsCheck %llu: HTTP %d response: %s",
                    (unsigned long long)sidCopy, status, resp.c_str());
                return;
            }
            json j = json::parse(resp, nullptr, false);
            if (j.is_discarded() || !j.is_object()) return;

            SubsCheckResult r;
            try {
                if (j.contains("blocked") && j["blocked"].is_boolean())
                    r.blocked = j["blocked"].get<bool>();
                if (j.contains("reason") && j["reason"].is_string())
                    r.reason = j["reason"].get<std::string>();
                if (j.contains("notifications") && j["notifications"].is_array())
                    r.notificationsCount = (int)j["notifications"].size();
            } catch (...) {}
            if (cb) cb(r);
        });
}

static void LogApiResponse(const char* what, int status, const std::string& resp)
{
    if (status >= 200 && status < 300) {
        LogFile("✓ %s: HTTP %d", what, status);
        return;
    }
    std::string errText;
    json j = json::parse(resp, nullptr, false);
    if (!j.is_discarded() && j.is_object()) {
        if (j.contains("error") && j["error"].is_string())
            errText = j["error"].get<std::string>();
    }
    if (errText.empty()) errText = resp;
    LogErr("✗ %s: HTTP %d — %s", what, status, errText.c_str());
}

static std::map<uint64, std::string> g_BanIds;

static void LoadBanIds()
{
    g_BanIds.clear();
    KeyValues* kv = new KeyValues("BlockDB_Bans");
    if (!kv->LoadFromFile(g_pFullFileSystem, kBanIdFile)) { kv->deleteThis(); return; }
    for (KeyValues* p = kv->GetFirstValue(); p; p = p->GetNextValue()) {
        uint64 sid = 0;
        try { sid = std::stoull(p->GetName()); } catch (...) { continue; }
        const char* v = p->GetString(nullptr, "");
        if (sid && v && *v) g_BanIds[sid] = v;
    }
    kv->deleteThis();
    LogFile("Loaded %d cached ban_id(s)", (int)g_BanIds.size());
}

static void SaveBanIds()
{
    EnsureDirFor(kBanIdFile);
    KeyValues* kv = new KeyValues("BlockDB_Bans");
    for (auto& [sid, id] : g_BanIds) {
        char key[32]; snprintf(key, sizeof(key), "%llu", (unsigned long long)sid);
        kv->SetString(key, id.c_str());
    }
    kv->SaveToFile(g_pFullFileSystem, kBanIdFile);
    kv->deleteThis();
}

static void CreateBan(uint64 victimSid, const std::string& victimName,
                      const std::string& victimIp,
                      const std::string& reason, int durationMinutes,
                      uint64 adminSidArg = 0,
                      const std::string& adminName = "Server")
{
    if (g_apiKey.empty()) { LogErr("CreateBan: no api_key (run db_pair)"); return; }
    char sid[32]; snprintf(sid, sizeof(sid), "%llu", (unsigned long long)victimSid);

    uint64 adminSid = adminSidArg ? adminSidArg
                                  : (g_serverSteamId ? g_serverSteamId : 76561197960265728ULL);
    char adminSidStr[32];
    snprintf(adminSidStr, sizeof(adminSidStr), "%llu", (unsigned long long)adminSid);

    const int durationSec = (durationMinutes > 0) ? durationMinutes * 60 : 0;

    json b = {
        {"admin", {{"name", adminName}, {"steamid64", adminSidStr}}},
        {"offender", {
            {"ip", victimIp},
            {"steam", {{"steamid64", sid}, {"name", victimName}}}
        }},
        {"duration", durationSec},
        {"reason",   reason}
    };
    std::string body = SafeDump(b);
    LogFile("POST /v1/public/bans → victim=%llu admin=%llu dur=%dmin (%dsec) reason='%s'",
        (unsigned long long)victimSid, (unsigned long long)adminSid,
        durationMinutes, durationSec, reason.c_str());

    AsyncHttpJob::Send(k_EHTTPMethodPOST, std::string(kApiBase) + "/v1/public/bans",
        body, {{"Authorization", Auth()}},
        [victimSid](bool ok, int status, const std::string& resp) {
            if (!ok) { LogErr("Create ban: HTTP failed"); return; }
            LogApiResponse("Create ban", status, resp);
            if (status < 200 || status > 299) return;

            json j = json::parse(resp, nullptr, false);
            if (j.is_discarded() || !j.is_object()) return;

            std::string banId;
            for (const char* k : { "ban_id", "id", "uuid" }) {
                if (j.contains(k) && j[k].is_string()) { banId = j[k].get<std::string>(); break; }
            }
            if (banId.empty() && j.contains("data") && j["data"].is_object()) {
                const auto& d = j["data"];
                for (const char* k : { "ban_id", "id", "uuid" }) {
                    if (d.contains(k) && d[k].is_string()) { banId = d[k].get<std::string>(); break; }
                }
            }

            if (!banId.empty()) {
                g_BanIds[victimSid] = banId;
                SaveBanIds();
                LogFile("ban_id cached: %llu -> %s", (unsigned long long)victimSid, banId.c_str());
            } else {
                LogFile("ban_id not found in response (unban by SteamID will be unavailable)");
            }
        });
}

static void DoUnban(uint64 victimSid)
{
    if (g_apiKey.empty()) { LogErr("DoUnban: no api_key (run db_pair)"); return; }

    auto it = g_BanIds.find(victimSid);
    if (it == g_BanIds.end() || it->second.empty()) {
        LogErr("Unban %llu: ban_id unknown — снимите бан через панель BlockDB "
               "(API v1 разбанивает только по ban_id)",
               (unsigned long long)victimSid);
        return;
    }

    const std::string banId = it->second;
    json b = {{"ban_id", banId}};
    std::string body = SafeDump(b);
    LogFile("POST /v1/public/bans/unban → sid=%llu ban_id=%s",
        (unsigned long long)victimSid, banId.c_str());

    AsyncHttpJob::Send(k_EHTTPMethodPOST, std::string(kApiBase) + "/v1/public/bans/unban",
        body, {{"Authorization", Auth()}},
        [victimSid](bool ok, int status, const std::string& resp) {
            if (!ok) { LogErr("Unban: HTTP failed"); return; }
            LogApiResponse("Unban", status, resp);
            if (status >= 200 && status <= 299) {
                g_BanIds.erase(victimSid);
                SaveBanIds();
            }
        });
}

static void KickPlayer(int slot)
{
    if (!engine) return;
    g_pUtils->NextFrame([slot]() {
        engine->DisconnectClient(CPlayerSlot(slot), NETWORK_DISCONNECT_KICKBANADDED);
    });
}

static void NotifyAdmins(const std::string& playerName, int banCount)
{
    if (!g_pUtils || !g_pPlayersApi) return;
    if (!g_pAdminApi) return;

    auto sendAll = [](const char* line) {
        for (int i = 0; i < 64; ++i) {
            if (!g_pPlayersApi->IsConnected(i) || g_pPlayersApi->IsFakeClient(i)) continue;
            if (!g_pAdminApi->IsAdmin(i)) continue;
            g_pUtils->PrintToChat(i, "%s", line);
        }
    };

    char buf[512];
    snprintf(buf, sizeof(buf), Tr("Notify_Player", " [BlockDB] Player: %s"),
        playerName.c_str());
    sendAll(buf);

    snprintf(buf, sizeof(buf), Tr("Notify_BanCount", " [BlockDB] Active bans: %d"),
        banCount);
    sendAll(buf);
}

static void OnClientAuthorized(int iSlot, uint64 sid)
{
    if (!sid || g_apiKey.empty()) return;

    const char* ipP = g_pPlayersApi ? g_pPlayersApi->GetIpAddress(iSlot) : nullptr;
    std::string ip = (ipP && *ipP) ? ipP : "";

    size_t colon = ip.find(':');
    if (colon != std::string::npos) ip.erase(colon);

    CheckSubscriptionsAsync(sid, ip, [iSlot, sid](const SubsCheckResult& r) {
        if (!g_pPlayersApi) return;
        if (!g_pPlayersApi->IsConnected(iSlot)) return;
        if (g_pPlayersApi->GetSteamID64(iSlot) != sid) return;

        if (r.blocked) {
            LogFile("Player %llu blocked (reason='%s'), kicking",
                (unsigned long long)sid, r.reason.c_str());
            KickPlayer(iSlot);
            return;
        }
        if (r.notificationsCount <= 0) return;

        g_pUtils->CreateTimer(2.0f, [iSlot, sid, r]() -> float {
            if (!g_pPlayersApi || !g_pPlayersApi->IsConnected(iSlot)) return -1.0f;
            if (g_pPlayersApi->GetSteamID64(iSlot) != sid) return -1.0f;

            std::string name;
            const char* nm = g_pPlayersApi->GetPlayerName(iSlot);
            if (nm && *nm) {
                name = nm;
            } else {
                char buf[32];
                snprintf(buf, sizeof(buf), "%llu", (unsigned long long)sid);
                name = buf;
            }
            NotifyAdmins(name, r.notificationsCount);
            return -1.0f;
        });
    });
}

static void OnPlayerPunish(int iSlot, int iType, int iTime, const char* szReason, int)
{
    if (iType != RT_BAN) return;
    uint64 sid = g_pPlayersApi->GetSteamID64(iSlot);
    if (!sid) return;
    const char* nm = g_pPlayersApi->GetPlayerName(iSlot);
    const char* ip = g_pPlayersApi->GetIpAddress(iSlot);
    CreateBan(sid,
              (nm && *nm) ? nm : "Unknown",
              (ip && *ip) ? ip : "",
              szReason ? szReason : "",
              iTime);
}

static void OnOfflinePlayerUnpunish(const char* szSteamID64, int iType, int)
{
    if (iType != RT_BAN || !szSteamID64 || !*szSteamID64) return;
    uint64 sid = 0; try { sid = std::stoull(szSteamID64); } catch (...) { return; }
    if (!sid) return;
    DoUnban(sid);
}

CON_COMMAND_F(db_pair, "Pair this server with BlockDB",
              FCVAR_GAMEDLL | FCVAR_RELEASE | FCVAR_SERVER_CAN_EXECUTE)
{
    if (args.ArgC() < 2) {
        LogErr("Usage: db_pair <code>");
        return;
    }
    std::string code = args.Arg(1);
    while (!code.empty() && (code.front() == ' ' || code.front() == '\t'))
        code.erase(code.begin());
    while (!code.empty() && (code.back()  == ' ' || code.back()  == '\t'))
        code.pop_back();
    if (code.empty()) { LogErr("Usage: db_pair <code>"); return; }
    DoPair(code);
}

#ifdef _WIN32
  #define BLOCKDB_EXPORT extern "C" __declspec(dllexport)
#else
  #define BLOCKDB_EXPORT extern "C" __attribute__((visibility("default")))
#endif

BLOCKDB_EXPORT void BlockDB_RelayBan(const char* victimSidStr,
                                     const char* victimName,
                                     const char* victimIp,
                                     const char* adminSidStr,
                                     const char* adminName,
                                     int         durationMinutes,
                                     const char* reason)
{
    uint64 vsid = 0, asid = 0;
    if (victimSidStr) { try { vsid = std::stoull(victimSidStr); } catch (...) {} }
    if (adminSidStr)  { try { asid = std::stoull(adminSidStr);  } catch (...) {} }
    if (!vsid) { LogErr("BlockDB_RelayBan: invalid victim sid"); return; }

    std::string vName = victimName ? victimName : "Unknown";
    std::string vIp   = victimIp   ? victimIp   : "";
    std::string aName = (adminName && *adminName) ? adminName : "Console";
    std::string rsn   = reason ? reason : "";

    LogFile("Admin System ban relayed: victim=%llu admin=%llu dur=%d reason='%s'",
        (unsigned long long)vsid, (unsigned long long)asid,
        durationMinutes, rsn.c_str());
    CreateBan(vsid, vName, vIp, rsn, durationMinutes, asid, aName);
}

BLOCKDB_EXPORT void BlockDB_RelayUnban(const char* victimSidStr)
{
    if (!victimSidStr) return;
    uint64 sid = 0;
    try { sid = std::stoull(victimSidStr); } catch (...) { return; }
    if (!sid) return;
    LogFile("Admin System unban relayed: sid=%llu", (unsigned long long)sid);
    DoUnban(sid);
}

void StartupServer()
{
    g_pGameEntitySystem = GameEntitySystem();
    g_pEntitySystem     = g_pUtils->GetCEntitySystem();
    gpGlobals           = g_pUtils->GetCGlobalVars();
}

bool BlockDB::Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late)
{
    PLUGIN_SAVEVARS();
    GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
    GET_V_IFACE_ANY(GetEngineFactory, g_pGameResourceServiceServer, IGameResourceService, GAMERESOURCESERVICESERVER_INTERFACE_VERSION);
    GET_V_IFACE_ANY    (GetEngineFactory, g_pSchemaSystem, ISchemaSystem, SCHEMASYSTEM_INTERFACE_VERSION);
    GET_V_IFACE_CURRENT(GetServerFactory, g_pSource2Server, ISource2Server, SOURCE2SERVER_INTERFACE_VERSION);
    GET_V_IFACE_CURRENT(GetEngineFactory, engine, IVEngineServer2, SOURCE2ENGINETOSERVER_INTERFACE_VERSION);
    GET_V_IFACE_CURRENT(GetFileSystemFactory, g_pFullFileSystem, IFileSystem, FILESYSTEM_INTERFACE_VERSION);

    g_SMAPI->AddListener(this, this);
    g_hkGameServerSteamAPIActivated.Add(g_pSource2Server);

    ConVar_Register(FCVAR_GAMEDLL | FCVAR_SERVER_CAN_EXECUTE |
                    FCVAR_CLIENT_CAN_EXECUTE | FCVAR_NOTIFY);
    return true;
}

static int s_authAttempts = 0;
static float BlockDB_AuthorizeTick()
{
    if (!engine) return -1.0f;
    if (engine->GetGameServerSteamID().GetStaticAccountKey()) {
        g_http          = SteamGameServerHTTP();
        g_serverSteamId = engine->GetGameServerSteamID().ConvertToUint64();
        LogFile("SteamHTTP ready, server SteamID=%llu",
            (unsigned long long)g_serverSteamId);
        return -1.0f;
    }
    if (++s_authAttempts >= 300) {
        LogErr("Authorization: gameserver SteamID not ready after 30s");
        return -1.0f;
    }
    return 0.1f;
}

KHook::Return<void> Hook_GameServerSteamAPIActivated(IServerGameDLL* pServer)
{
    g_http = SteamGameServerHTTP();
    if (engine && engine->GetGameServerSteamID().GetStaticAccountKey()) {
        g_serverSteamId = engine->GetGameServerSteamID().ConvertToUint64();
        LogFile("SteamHTTP ready, server SteamID=%llu",
            (unsigned long long)g_serverSteamId);
    } else if (g_pUtils) {
        s_authAttempts = 0;
        g_pUtils->CreateTimer(0.1f, BlockDB_AuthorizeTick);
    }
    return { KHook::Action::Ignore };
}

bool BlockDB::Unload(char* error, size_t maxlen)
{
    int ret;
    if (!g_SMAPI->MetaFactory(UTILS_INTERFACE, &ret, nullptr)) g_pUtils = nullptr;
    g_hkGameServerSteamAPIActivated.Remove(g_pSource2Server);
    if (g_pUtils) g_pUtils->ClearAllHooks(g_PLID);
    ConVar_Unregister();
    return true;
}

void BlockDB::AllPluginsLoaded()
{
    int ret;
    g_pUtils = (IUtilsApi*)g_SMAPI->MetaFactory(UTILS_INTERFACE, &ret, NULL);
    if (ret == META_IFACE_FAILED) {
        ConColorMsg(Color(255,0,0,255), "[BlockDB] Missing Utils plugin\n");
        std::string c = "meta unload " + std::to_string(g_PLID);
        engine->ServerCommand(c.c_str());
        return;
    }
    g_pPlayersApi = (IPlayersApi*)g_SMAPI->MetaFactory(PLAYERS_INTERFACE, &ret, NULL);
    if (ret == META_IFACE_FAILED) {
        g_pUtils->ErrorLog("[BlockDB] Missing Players plugin"); return;
    }

    g_pAdminApi = (IAdminApi*)g_SMAPI->MetaFactory(Admin_INTERFACE, &ret, NULL);
    bool hasAdmin = (ret != META_IFACE_FAILED);
    if (!hasAdmin) g_pAdminApi = nullptr;

    g_pUtils->StartupServer(g_PLID, StartupServer);

    g_pPlayersApi->HookOnClientAuthorized(g_PLID, OnClientAuthorized);
    if (hasAdmin) {
        g_pAdminApi->OnPlayerPunish(g_PLID, OnPlayerPunish);
        g_pAdminApi->OnOfflinePlayerUnpunish(g_PLID, OnOfflinePlayerUnpunish);
    }

    LoadApiKey();
    LoadBanIds();
    LoadPhrases();

    if (!hasAdmin) {
        LogBox(2, {
            "ADMIN SYSTEM NOT FOUND"
        });
    }
}

const char *BlockDB::GetLicense()
{
    return "Public";
}

const char *BlockDB::GetVersion()
{
    return "1.1.1";
}

const char *BlockDB::GetDate()
{
    return __DATE__;
}

const char *BlockDB::GetLogTag()
{
    return "[BlockDB]";
}

const char *BlockDB::GetAuthor()
{
    return "_ded_cookies";
}

const char *BlockDB::GetDescription()
{
    return "BlockDB";
}

const char *BlockDB::GetName()
{
    return "BlockDB";
}

const char *BlockDB::GetURL()
{
    return "https://api.onlypublic.net/";
}
