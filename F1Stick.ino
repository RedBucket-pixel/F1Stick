// ============================================================================
//  F1 Stick - live 2026 Formula 1 info for M5StickC PLUS2
//  Single-file build. Libraries: M5Unified, ArduinoJson (7.x)
//  Data: Jolpica F1 API (Ergast-compatible, no key). All times UTC.
// ============================================================================
#include <Arduino.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <vector>
#include <map>
#include <algorithm>
#include <math.h>
#include <time.h>
#include <sys/time.h>

// ----------------------------------------------------------------- config
#define APP_NAME    "F1 Stick"
#define APP_VERSION "1.0.0"

#define API_BASE     "https://api.jolpi.ca/ergast/f1/"
#define SEASON_YEAR  2026

#define R_SCHEDULE_MS  (6UL * 60UL * 60UL * 1000UL)
#define R_STANDINGS_MS (5UL * 60UL * 1000UL)
#define R_ORDER_LIVE_MS   (20UL * 1000UL)
#define R_ORDER_IDLE_MS   (5UL * 60UL * 1000UL)
#define R_DETAIL_MS       (15UL * 60UL * 1000UL)
#define WIFI_TIMEOUT_MS   12000

static const int SCR_W  = 240;
static const int SCR_H  = 135;
static const int HDR_H  = 18;
static const int BODY_Y = 18;
static const int ROW_H  = 17;
static const int ROWS   = 6;
static const int HINT_Y = 120;

static const uint16_t COL_BG    = 0x0000;
static const uint16_t COL_FG    = 0xFFFF;
static const uint16_t COL_DIM   = 0x8410;
static const uint16_t COL_GREEN = 0x07E0;
static const uint16_t COL_HINT  = 0x9CD3;

// --------------------------------------------------------------- time utils
static inline int64_t daysFromCivil(int y, int m, int d) {
  y -= (m <= 2);
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int64_t)era * 146097 + (int64_t)doe - 719468;
}
static inline int64_t epochFromParts(int y, int mo, int d, int h, int mi, int s) {
  return daysFromCivil(y, mo, d) * 86400LL + h * 3600LL + mi * 60LL + s;
}
static inline bool parseIsoZ(const String& date, const String& tm, int64_t& out) {
  int y = 0, mo = 0, d = 0;
  if (sscanf(date.c_str(), "%d-%d-%d", &y, &mo, &d) != 3) return false;
  int h = 0, mi = 0, s = 0, hh, mm, ss;
  if (tm.length() && sscanf(tm.c_str(), "%d:%d:%d", &hh, &mm, &ss) >= 2) { h = hh; mi = mm; s = ss; }
  out = epochFromParts(y, mo, d, h, mi, s);
  return true;
}
static inline int64_t parseHttpDate(const String& v) {
  if (v.length() < 20) return 0;
  int d = 0, y = 0, h = 0, mi = 0, s = 0; char mon[8] = {0};
  if (sscanf(v.c_str(), "%*3s, %d %3s %d %d:%d:%d", &d, mon, &y, &h, &mi, &s) != 7) return 0;
  static const char* M[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
  int mo = 0;
  for (int i = 0; i < 12; ++i) if (!strncmp(mon, M[i], 3)) { mo = i + 1; break; }
  if (!mo) return 0;
  return epochFromParts(y, mo, d, h, mi, s);
}
static inline bool clockOk(int64_t t) { return t > 1700000000LL; }
static inline void maybeSetClock(int64_t epoch) {
  if (!clockOk(epoch)) return;
  time_t cur = time(nullptr);
  if (clockOk(cur) && labs((long)(cur - epoch)) < 300) return;
  struct timeval tv; tv.tv_sec = (time_t)epoch; tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
}
static inline String fmtUTC(int64_t t) {
  if (!clockOk(t)) return "--:--";
  time_t tt = (time_t)t; struct tm tmv; gmtime_r(&tt, &tmv);
  static const char* D[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
  char b[16];
  snprintf(b, sizeof(b), "%s %02d:%02d", D[tmv.tm_wday & 7], tmv.tm_hour, tmv.tm_min);
  return String(b);
}
static inline String fmtDelta(int64_t sec) {
  if (sec < 60) return "now";
  if (sec < 3600) return String((int)(sec / 60)) + "m";
  if (sec < 86400) {
    int h = (int)(sec / 3600), m = (int)((sec % 3600) / 60);
    return m ? String(h) + "h " + String(m) + "m" : String(h) + "h";
  }
  int d = (int)(sec / 86400), h = (int)((sec % 86400) / 3600);
  return h ? String(d) + "d " + String(h) + "h" : String(d) + "d";
}

// --------------------------------------------------------------- F1 model
enum SessionKind : uint8_t { SK_PRACTICE, SK_SQ, SK_SPRINT, SK_QUALI, SK_RACE };
enum SessState   : uint8_t { SS_UPCOMING, SS_LIVE, SS_DONE };

struct SessionSlot {
  SessionKind kind = SK_PRACTICE;
  String   name, longName;
  int64_t  start = 0;
  uint16_t durMin = 60;
  bool     hasResults = false;
};
struct WeekendInfo {
  bool    valid = false;
  int     round = 0;
  String  raceName, country, circuit, shortCode;
  bool    hasSprint = false;
  int64_t raceStart = 0;
  std::vector<SessionSlot> sessions;
};
struct RoundBrief {
  int round = 0;
  String raceName, shortCode, country;
  int64_t raceStart = 0, firstStart = 0;
  bool hasSprint = false;
};
struct DriverInfo {
  String id, code, given, family, team, teamShort;
  int    number = 0, champPos = 0, wins = 0;
  float  points = 0;
  uint8_t latestPos = 0;
};
struct OrderRow { int pos = 0; String id, code, team, right; };

typedef void (*ApiProgress)(const char* msg);

// --------------------------------------------------------------- API impl
namespace {

struct TeamAbbr { const char* id; const char* abbr; };
const TeamAbbr kTeams[] = {
  {"mercedes","MER"},{"ferrari","FER"},{"red_bull","RBR"},{"mclaren","MCL"},
  {"aston_martin","AMR"},{"alpine","ALP"},{"williams","WIL"},{"haas","HAA"},
  {"audi","AUD"},{"rb","RB"},{"cadillac","CAD"},{"sauber","SAU"},{"alphatauri","AT"},
};

String teamAbbr(const String& id, const String& name) {
  for (auto& t : kTeams) if (id == t.id) return String(t.abbr);
  String s = name; s.toUpperCase();
  if (s.length() > 3) s = s.substring(0, 3);
  return s;
}

const char* countryCode(const String& c) {
  static const struct { const char* n; const char* a; } T[] = {
    {"Australia","AUS"},{"China","CHN"},{"Japan","JPN"},{"USA","USA"},{"United States","USA"},
    {"Canada","CAN"},{"Monaco","MON"},{"Spain","ESP"},{"Austria","AUT"},{"UK","GBR"},
    {"United Kingdom","GBR"},{"Belgium","BEL"},{"Hungary","HUN"},{"Netherlands","NED"},
    {"Italy","ITA"},{"Azerbaijan","AZE"},{"Malaysia","MYS"},{"Singapore","SGP"},
    {"Mexico","MEX"},{"Brazil","BRA"},{"UAE","UAE"},{"United Arab Emirates","UAE"},
    {"Qatar","QAT"},{"Saudi Arabia","SAU"},{"Bahrain","BHR"},{"France","FRA"},
    {"Germany","GER"},{"Portugal","POR"},{"Turkey","TUR"},{"Russia","RUS"},
  };
  for (auto& t : T) if (c.equalsIgnoreCase(t.n)) return t.a;
  return "F1";
}

bool getJson(const String& path, JsonDocument& doc, JsonDocument& filter, String& err) {
  if (WiFi.status() != WL_CONNECTED) { err = "offline"; return false; }
  for (int attempt = 0; attempt < 2; ++attempt) {
    WiFiClientSecure tls;
    tls.setInsecure();                 // no CA bundle on purpose (see notes)
    tls.setTimeout(9);
    HTTPClient http;
    http.setConnectTimeout(9000);
    http.setTimeout(9000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    String url = String(API_BASE) + path;
    if (!http.begin(tls, url)) { err = "bad url"; return false; }
    http.addHeader("Accept", "application/json");
    http.addHeader("User-Agent", "F1StickC-Plus2/" APP_VERSION);

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
      err = "HTTP " + String(code);
      http.end();
      if (code < 0) { delay(150); continue; }
      return false;
    }
    maybeSetClock(parseHttpDate(http.header("Date")));

    DeserializationError e =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    http.end();
    if (e) { err = e.c_str(); delay(100); continue; }
    return true;
  }
  if (!err.length()) err = "network";
  return false;
}

void addSession(JsonObjectConst race, const char* key, SessionKind kind,
                const char* nm, const char* lng, uint16_t dur,
                std::vector<SessionSlot>& out, bool& firstSet, int64_t& first) {
  JsonObjectConst o = race[key];
  if (o.isNull()) return;
  int64_t t = 0;
  if (!parseIsoZ(String(o["date"] | ""), String(o["time"] | "00:00:00Z"), t)) return;
  SessionSlot s;
  s.kind = kind; s.name = nm; s.longName = lng; s.start = t; s.durMin = dur;
  out.push_back(s);
  if (!firstSet || t < first) { first = t; firstSet = true; }
}

void buildRound(JsonObjectConst r, WeekendInfo& w) {
  w.valid     = true;
  w.round     = String(r["round"] | "0").toInt();
  w.raceName  = String(r["raceName"] | "");
  w.country   = String(r["Circuit"]["Location"]["country"] | "");
  w.circuit   = String(r["Circuit"]["circuitName"] | "");
  w.shortCode = countryCode(w.country);
  if (w.shortCode == "F1" && w.raceName.length() >= 3) {
    w.shortCode = w.raceName.substring(0, 3); w.shortCode.toUpperCase();
  }
  parseIsoZ(String(r["date"] | ""), String(r["time"] | "00:00:00Z"), w.raceStart);

  w.sessions.clear();
  bool fs = false; int64_t first = 0;
  addSession(r, "FirstPractice",    SK_PRACTICE, "FP1",  "Practice 1",     60, w.sessions, fs, first);
  addSession(r, "SecondPractice",   SK_PRACTICE, "FP2",  "Practice 2",     60, w.sessions, fs, first);
  addSession(r, "ThirdPractice",    SK_PRACTICE, "FP3",  "Practice 3",     60, w.sessions, fs, first);
  addSession(r, "SprintQualifying", SK_SQ,       "SQ",   "Sprint Quali",   45, w.sessions, fs, first);
  addSession(r, "SprintShootout",   SK_SQ,       "SQ",   "Sprint Quali",   45, w.sessions, fs, first);
  addSession(r, "Sprint",           SK_SPRINT,   "SPR",  "Sprint",         60, w.sessions, fs, first);
  addSession(r, "Qualifying",       SK_QUALI,    "Q",    "Qualifying",     65, w.sessions, fs, first);
  addSession(r, "Race",             SK_RACE,     "RACE", "Race",          150, w.sessions, fs, first);

  if (w.raceStart) {
    SessionSlot race; race.kind = SK_RACE; race.name = "RACE"; race.longName = "Race";
    race.start = w.raceStart; race.durMin = 150;
    bool found = false;
    for (auto& s : w.sessions) if (s.kind == SK_RACE) { s = race; found = true; }
    if (!found) w.sessions.push_back(race);
  }
  std::sort(w.sessions.begin(), w.sessions.end(),
            [](const SessionSlot& a, const SessionSlot& b) { return a.start < b.start; });
  for (auto& s : w.sessions) if (s.kind == SK_SQ || s.kind == SK_SPRINT) w.hasSprint = true;
}

} // namespace

bool fetchSchedule(int season, std::vector<RoundBrief>& out, String& err, ApiProgress cb) {
  if (cb) cb("Schedule");
  JsonDocument filter;
  JsonObject rf = filter["MRData"]["RaceTable"]["Races"][0];
  rf["round"] = true; rf["raceName"] = true; rf["date"] = true; rf["time"] = true;
  rf["Circuit"]["Location"]["country"] = true;
  const char* sess[] = {"FirstPractice","SecondPractice","ThirdPractice",
                        "SprintQualifying","SprintShootout","Sprint","Qualifying"};
  for (auto k : sess) { rf[k]["date"] = true; rf[k]["time"] = true; }

  JsonDocument doc;
  if (!getJson(String(season) + ".json?limit=100", doc, filter, err)) return false;

  out.clear();
  for (JsonObjectConst r : doc["MRData"]["RaceTable"]["Races"].as<JsonArrayConst>()) {
    RoundBrief b;
    b.round     = String(r["round"] | "0").toInt();
    b.raceName  = String(r["raceName"] | "");
    b.country   = String(r["Circuit"]["Location"]["country"] | "");
    b.shortCode = countryCode(b.country);
    if (b.shortCode == "F1" && b.raceName.length() >= 3) {
      b.shortCode = b.raceName.substring(0, 3); b.shortCode.toUpperCase();
    }
    parseIsoZ(String(r["date"] | ""), String(r["time"] | "00:00:00Z"), b.raceStart);
    b.firstStart = b.raceStart;
    for (auto k : sess) {
      JsonObjectConst o = r[k];
      if (o.isNull()) continue;
      int64_t t = 0;
      if (parseIsoZ(String(o["date"] | ""), String(o["time"] | "00:00:00Z"), t) && t < b.firstStart)
        b.firstStart = t;
      if (!strcmp(k, "Sprint")) b.hasSprint = true;
    }
    if (b.round) out.push_back(b);
  }
  if (out.empty()) { err = "empty schedule"; return false; }
  return true;
}

bool fetchRound(int season, int round, WeekendInfo& out, String& err, ApiProgress cb) {
  if (cb) cb("Weekend");
  JsonDocument filter;
  JsonObject rf = filter["MRData"]["RaceTable"]["Races"][0];
  rf["round"] = true; rf["raceName"] = true; rf["date"] = true; rf["time"] = true;
  rf["Circuit"]["circuitName"] = true;
  rf["Circuit"]["Location"]["country"] = true;
  const char* sess[] = {"FirstPractice","SecondPractice","ThirdPractice",
                        "SprintQualifying","SprintShootout","Sprint","Qualifying"};
  for (auto k : sess) { rf[k]["date"] = true; rf[k]["time"] = true; }

  JsonDocument doc;
  if (!getJson(String(season) + "/" + String(round) + ".json", doc, filter, err)) return false;

  JsonArrayConst races = doc["MRData"]["RaceTable"]["Races"].as<JsonArrayConst>();
  if (races.isNull() || races.size() == 0) { err = "no round"; return false; }
  WeekendInfo w;
  buildRound(races[0].as<JsonObjectConst>(), w);
  out = w;
  return true;
}

bool fetchStandings(int season, std::vector<DriverInfo>& out, int& champRound,
                    String& err, ApiProgress cb) {
  if (cb) cb("Standings");
  JsonDocument filter;
  filter["MRData"]["StandingsTable"]["round"] = true;
  JsonObject df = filter["MRData"]["StandingsTable"]["StandingsLists"][0]["DriverStandings"][0];
  df["position"] = true; df["points"] = true; df["wins"] = true;
  df["Driver"]["driverId"] = true; df["Driver"]["permanentNumber"] = true;
  df["Driver"]["code"] = true;     df["Driver"]["givenName"] = true;
  df["Driver"]["familyName"] = true;
  df["Constructors"][0]["constructorId"] = true;
  df["Constructors"][0]["name"] = true;

  JsonDocument doc;
  if (!getJson(String(season) + "/driverstandings.json?limit=100", doc, filter, err)) return false;

  JsonObjectConst table = doc["MRData"]["StandingsTable"];
  champRound = String(table["round"] | "0").toInt();
  JsonArrayConst lists = table["StandingsLists"].as<JsonArrayConst>();
  if (lists.isNull() || lists.size() == 0) { err = "no standings"; return false; }

  out.clear();
  for (JsonObjectConst e : lists[0]["DriverStandings"].as<JsonArrayConst>()) {
    DriverInfo d;
    d.id       = String(e["Driver"]["driverId"] | "");
    d.code     = String(e["Driver"]["code"] | "");
    d.given    = String(e["Driver"]["givenName"] | "");
    d.family   = String(e["Driver"]["familyName"] | "");
    d.number   = String(e["Driver"]["permanentNumber"] | "0").toInt();
    d.champPos = String(e["position"] | "0").toInt();
    d.points   = String(e["points"] | "0").toFloat();
    d.wins     = String(e["wins"] | "0").toInt();
    String cid, cname;
    for (JsonObjectConst c : e["Constructors"].as<JsonArrayConst>()) {
      cid   = String(c["constructorId"] | "");
      cname = String(c["name"] | "");
    }
    d.team      = cname;
    d.teamShort = teamAbbr(cid, cname);
    if (!d.code.length() && d.family.length() >= 3) {
      d.code = d.family.substring(0, 3); d.code.toUpperCase();
    }
    out.push_back(d);
  }
  if (out.empty()) { err = "empty standings"; return false; }
  return true;
}

bool fetchOrder(int season, int round, SessionKind kind, std::vector<OrderRow>& out,
                String& title, String& err, ApiProgress cb) {
  out.clear();
  if (cb) cb("Session");
  if (kind == SK_PRACTICE) { err = "practice timing not in feed"; return false; }

  const char* suffix = "/results.json";
  const char* key    = "Results";
  if (kind == SK_SPRINT || kind == SK_SQ) { suffix = "/sprint.json"; key = "SprintResults"; title = "SPRINT"; }
  else if (kind == SK_QUALI)              { suffix = "/qualifying.json"; key = "QualifyingResults"; title = "QUALIFYING"; }
  else                                    { title = "RACE"; }
  if (kind == SK_SQ) title = "SPRINT Q*";

  JsonDocument filter;
  JsonObject rf = filter["MRData"]["RaceTable"]["Races"][0];
  rf[key][0]["position"] = true;
  rf[key][0]["positionText"] = true;
  rf[key][0]["grid"] = true;
  rf[key][0]["status"] = true;
  rf[key][0]["Time"]["time"] = true;
  rf[key][0]["Driver"]["driverId"] = true;
  rf[key][0]["Driver"]["code"] = true;
  rf[key][0]["Constructor"]["name"] = true;
  rf[key][0]["Constructor"]["constructorId"] = true;
  if (kind == SK_QUALI) { rf[key][0]["Q1"] = true; rf[key][0]["Q2"] = true; rf[key][0]["Q3"] = true; }

  JsonDocument doc;
  if (!getJson(String(season) + "/" + String(round) + suffix, doc, filter, err)) return false;

  JsonArrayConst races = doc["MRData"]["RaceTable"]["Races"].as<JsonArrayConst>();
  if (races.isNull() || races.size() == 0) { err = "not published"; return false; }
  JsonArrayConst arr = races[0][key].as<JsonArrayConst>();
  if (arr.isNull() || arr.size() == 0) { err = "not published"; return false; }

  for (JsonObjectConst e : arr) {
    OrderRow r;
    r.pos  = String(e["position"] | "0").toInt();
    r.id   = String(e["Driver"]["driverId"] | "");
    r.code = String(e["Driver"]["code"] | "");
    r.team = teamAbbr(String(e["Constructor"]["constructorId"] | ""),
                      String(e["Constructor"]["name"] | ""));
    if (kind == SK_QUALI) {
      const char* q3 = e["Q3"] | (const char*)nullptr;
      const char* q2 = e["Q2"] | (const char*)nullptr;
      const char* q1 = e["Q1"] | (const char*)nullptr;
      r.right = q3 ? String(q3) : (q2 ? String(q2) : (q1 ? String(q1) : ""));
    } else {
      const char* t = e["Time"]["time"] | (const char*)nullptr;
      String st = String(e["status"] | "");
      if (t) r.right = String(t);
      else if (st == "Did not start") r.right = "DNS";
      else if (st == "Disqualified")  r.right = "DSQ";
      else if (st == "Finished")      r.right = "";
      else if (st.length() > 8)       r.right = st.substring(0, 8);
      else                            r.right = st;
    }
    if (kind == SK_SQ) {
      int g = String(e["grid"] | "0").toInt();
      if (g > 0) r.pos = g;
    }
    out.push_back(r);
  }
  if (out.empty()) { err = "empty"; return false; }
  if (kind == SK_SQ)
    std::sort(out.begin(), out.end(), [](const OrderRow& a, const OrderRow& b) { return a.pos < b.pos; });
  return true;
}

bool fetchDriverSeason(int season, const String& driverId, int& bestFinish,
                       int& podiums, int& lastRacePos, String& err, ApiProgress cb) {
  if (cb) cb("Driver");
  JsonDocument filter;
  JsonObject rf = filter["MRData"]["RaceTable"]["Races"][0]["Results"][0];
  rf["position"] = true; rf["round"] = true;

  JsonDocument doc;
  if (!getJson(String(season) + "/drivers/" + driverId + "/results.json?limit=100",
               doc, filter, err)) return false;

  bestFinish = 0; podiums = 0; lastRacePos = 0;
  int lastRound = 0;
  for (JsonObjectConst r : doc["MRData"]["RaceTable"]["Races"].as<JsonArrayConst>()) {
    int round = String(r["round"] | "0").toInt();
    for (JsonObjectConst res : r["Results"].as<JsonArrayConst>()) {
      int p = String(res["position"] | "0").toInt();
      if (p > 0 && (bestFinish == 0 || p < bestFinish)) bestFinish = p;
      if (p >= 1 && p <= 3) podiums++;
      if (round >= lastRound) { lastRound = round; lastRacePos = p; }
    }
  }
  return true;
}

// --------------------------------------------------------------- WiFi manager
typedef bool (*WifiProgress)(int elapsedMs, int totalMs);

namespace WifiMgr {
  Preferences  prefs;
  WebServer    server(80);
  DNSServer    dns;
  bool         portal = false;
  String       apSsid;

  String htmlEscape(const String& s) {
    String o; o.reserve(s.length() + 8);
    for (char c : s) {
      if (c == '&') o += "&amp;";
      else if (c == '<') o += "&lt;";
      else if (c == '>') o += "&gt;";
      else if (c == '"') o += "&quot;";
      else o += c;
    }
    return o;
  }

  static const char* kCss =
    "<style>body{font-family:system-ui,sans-serif;background:#000;color:#fff;margin:0;padding:22px}"
    "h2{margin:0 0 4px}p{color:#aaa;margin:0 0 18px}"
    "input,button{width:100%;font-size:17px;padding:13px;margin:7px 0;box-sizing:border-box;"
    "background:#101010;color:#fff;border:1px solid #444;border-radius:10px}"
    "button{background:#00d95f;color:#000;font-weight:700;border:0}</style>";

  void handleRoot() {
    String opts;
    int n = WiFi.scanNetworks();
    for (int i = 0; i < n && i < 20; ++i) opts += "<option value=\"" + htmlEscape(WiFi.SSID(i)) + "\">";
    String page = "<!doctype html><html><head><meta charset=utf-8>";
    page += "<meta name=viewport content='width=device-width,initial-scale=1'>";
    page += "<title>F1 Stick WiFi</title>";
    page += kCss;
    page += "</head><body><h2>F1 Stick</h2><p>Enter your 2.4 GHz Wi-Fi network.</p>";
    page += "<form method='GET' action='/save'>";
    page += "<input name='s' list='nets' placeholder='Network name' required>";
    page += "<datalist id='nets'>" + opts + "</datalist>";
    page += "<input name='p' type='password' placeholder='Password'>";
    page += "<button>Save &amp; restart</button></form></body></html>";
    server.send(200, "text/html", page);
  }

  void handleSave() {
    String s = server.arg("s"); s.trim();
    String p = server.arg("p");
    if (s.length()) { prefs.putString("ssid", s); prefs.putString("pass", p); }
    server.send(200, "text/html",
      "<!doctype html><html><head><meta charset=utf-8>"
      "<meta http-equiv=refresh content='3;url=/'></head>"
      "<body style='background:#000;color:#0f0;font-family:sans-serif;padding:30px'>"
      "<h2>Saved</h2><p>Restarting the stick...</p></body></html>");
    delay(700);
    ESP.restart();
  }

  void begin() {
    prefs.begin("f1stick", false);
    uint8_t mac[6]; WiFi.macAddress(mac);
    char b[16];
    snprintf(b, sizeof(b), "F1Stick-%02X%02X", mac[4], mac[5]);
    apSsid = b;
  }
  bool   hasCreds() { return prefs.getString("ssid", "").length() > 0; }
  String ssid()     { return prefs.getString("ssid", ""); }
  bool   online()   { return WiFi.status() == WL_CONNECTED; }
  bool   portalActive() { return portal; }
  String apName()   { return apSsid; }
  String apUrl()    { return "http://" + WiFi.softAPIP().toString(); }

  bool connect(uint32_t timeoutMs, WifiProgress cb) {
    String s = prefs.getString("ssid", "");
    if (!s.length()) return false;
    String p = prefs.getString("pass", "");

    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(s.c_str(), p.c_str());

    uint32_t t0 = millis();
    while (millis() - t0 < timeoutMs) {
      if (WiFi.status() == WL_CONNECTED) { WiFi.setSleep(true); return true; }
      if (cb && !cb((int)(millis() - t0), (int)timeoutMs)) { WiFi.disconnect(true); return false; }
      delay(60);
    }
    WiFi.disconnect(true);
    return false;
  }

  void startPortal() {
    WiFi.persistent(false);
    WiFi.disconnect(true);
    delay(120);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(apSsid.c_str());
    delay(250);
    dns.start(53, "*", WiFi.softAPIP());
    server.on("/", handleRoot);
    server.on("/save", handleSave);
    server.onNotFound(handleRoot);
    server.begin();
    portal = true;
  }
  void portalLoop() {
    if (!portal) return;
    dns.processNextRequest();
    server.handleClient();
  }
  void stopPortal() {
    if (!portal) return;
    server.stop(); dns.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    portal = false;
  }
}

// --------------------------------------------------------------- app state
enum Screen : uint8_t { SC_MENU, SC_F1, SC_SETTINGS, SC_ABOUT,
                        SC_WIFI_CONNECT, SC_WIFI_PROBLEM, SC_WIFI_PORTAL };
enum TabId  : uint8_t { TAB_SESSION, TAB_STANDINGS, TAB_DRIVER };
enum Focus  : uint8_t { FOCUS_TABS, FOCUS_BODY };
enum ProblemMode : uint8_t { PM_NO_CREDS, PM_FAILED, PM_LOST };

struct DetailCache { uint32_t stamp = 0; int best = 0, podiums = 0, lastRace = 0; bool ok = false; };

struct AppState {
  Screen  screen = SC_MENU;
  uint8_t menuSel = 0;
  uint8_t tab = TAB_SESSION;
  uint8_t focus = FOCUS_TABS;

  int  driverSel = 0;
  int  standTop  = 0;
  int  sessSel   = 0;
  int  sessTop   = 0;
  bool sessListMode = false;
  int  orderTop  = 0;

  uint8_t settingsSel = 0;
  uint8_t problemSel  = 0;
  ProblemMode problemMode = PM_NO_CREDS;
  String  problemMsg;

  String wifiSsid;
  int    wifiElapsed = 0, wifiTotal = 0;

  bool     dirty = true;
  String   toast;
  uint32_t toastUntil = 0;
};

struct F1Data {
  std::vector<RoundBrief> rounds;
  WeekendInfo             weekend;
  std::vector<DriverInfo> drivers;
  std::vector<OrderRow>   order;
  String orderTitle;
  int    orderSessionIdx = -1;
  int    viewSession     = -1;
  int    champRound      = 0;
  int    curRound        = 0;

  uint32_t scheduleStamp = 0, standingsStamp = 0, orderStamp = 0;
  bool scheduleOk = false, standingsOk = false, orderOk = false;

  int64_t now = 0;
  String  latestLabel;
  String  nextRace;
  int64_t nextRaceStart = 0;
  std::map<String, DetailCache> details;
};

// --------------------------------------------------------------- UI
class Ui {
public:
  bool begin();
  void render(AppState& st, F1Data& d, bool force = false);
  void busy(const String& msg);
  void invalidate() { _lastHash = 0; }

private:
  M5Canvas    _c{&M5.Display};
  LovyanGFX*  _g = &M5.Display;
  bool        _sprite = false;
  uint32_t    _lastHash = 0;
  uint32_t    _lastMinute = 0xFFFFFFFF;
  Screen      _lastScreen = SC_MENU;
  String      _sig;

  void blit();
  void txt(int x, int y, const String& s, uint16_t col, uint8_t font = 2,
           textdatum_t dat = textdatum_t::top_left);
  void hdr(const String& left, const String& right, uint16_t lc, uint16_t rc);
  void hints(const char* s);
  void box(int y, const String& l1, const String& l2 = "", const String& l3 = "");
  void scrollbar(int top, int total, int rows);
  String clip(const String& s, int maxW, uint8_t font = 2);

  void drawMenu(AppState& st, F1Data& d);
  void drawF1(AppState& st, F1Data& d);
  void drawSession(AppState& st, F1Data& d);
  void drawStandings(AppState& st, F1Data& d);
  void drawDriver(AppState& st, F1Data& d);
  void drawSettings(AppState& st, F1Data& d);
  void drawAbout(F1Data& d);
  void drawConnect(AppState& st);
  void drawProblem(AppState& st);
  void drawPortal(AppState& st);
  void tabBar(uint8_t active, bool focused);
  String ctxRound(const F1Data& d);
};

static uint32_t fnv1a(const String& s, uint32_t h = 2166136261u) {
  for (size_t i = 0; i < s.length(); ++i) { h ^= (uint8_t)s[i]; h *= 16777619u; }
  return h;
}
static String ptsStr(float p) {
  if (fabsf(p - roundf(p)) < 0.05f) return String((long)roundf(p));
  return String(p, 1);
}
static SessState sessState(const SessionSlot& s, int64_t now) {
  if (!clockOk(now)) return SS_UPCOMING;
  if (now < s.start) return SS_UPCOMING;
  if (now < s.start + (int64_t)s.durMin * 60) return SS_LIVE;
  return SS_DONE;
}

bool Ui::begin() {
  _c.setColorDepth(16);
  if (_c.createSprite(SCR_W, SCR_H)) { _sprite = true; _g = &_c; _g->fillScreen(COL_BG); }
  else { _sprite = false; _g = &M5.Display; }
  _g->setTextWrap(false);
  _g->fillScreen(COL_BG);
  return _sprite;
}
void Ui::blit() { if (_sprite) _c.pushSprite(0, 0); }

void Ui::txt(int x, int y, const String& s, uint16_t col, uint8_t font, textdatum_t dat) {
  _g->setTextFont(font);
  _g->setTextSize(1);
  _g->setTextColor(col);
  _g->setTextDatum(dat);
  _g->drawString(s, x, y);
  _g->setTextDatum(textdatum_t::top_left);
  _sig += s; _sig += (char)(x + 31); _sig += (char)(y + 31);
  _sig += (char)(col >> 8); _sig += (char)font;
}

String Ui::clip(const String& s, int maxW, uint8_t font) {
  _g->setTextFont(font);
  if (_g->textWidth(s) <= maxW) return s;
  String t = s;
  while (t.length() && _g->textWidth(t + "..") > maxW) t.remove(t.length() - 1);
  return t + "..";
}

void Ui::hdr(const String& left, const String& right, uint16_t lc, uint16_t rc) {
  _g->fillRect(0, 0, SCR_W, HDR_H, COL_BG);
  txt(4, 1, clip(left, 150), lc);
  if (right.length()) txt(SCR_W - 4, 1, clip(right, 80), rc, 2, textdatum_t::top_right);
  _g->drawFastHLine(0, HDR_H - 1, SCR_W, COL_DIM);
}

void Ui::hints(const char* s) {
  _g->drawFastHLine(0, HINT_Y, SCR_W, COL_DIM);
  txt(4, HINT_Y + 4, s, COL_HINT, 1);
}

void Ui::box(int y, const String& l1, const String& l2, const String& l3) {
  txt(120, y, l1, COL_FG, 2, textdatum_t::top_center);
  if (l2.length()) txt(120, y + 22, l2, COL_DIM, 1, textdatum_t::top_center);
  if (l3.length()) txt(120, y + 34, l3, COL_DIM, 1, textdatum_t::top_center);
}

void Ui::scrollbar(int top, int total, int rows) {
  if (total <= rows) return;
  const int h = 102;
  int barH = h * rows / total; if (barH < 8) barH = 8;
  int barY = BODY_Y + (h - barH) * top / (total - rows);
  _g->fillRect(236, barY, 3, barH, COL_DIM);
}

String Ui::ctxRound(const F1Data& d) {
  if (!d.weekend.round) return "2026";
  return String("R") + d.weekend.round + " " + d.weekend.shortCode;
}

void Ui::render(AppState& st, F1Data& d, bool force) {
  d.now = (int64_t)time(nullptr);
  uint32_t minute = (uint32_t)(d.now / 60);
  if (st.toastUntil && millis() > st.toastUntil) { st.toastUntil = 0; st.toast = ""; st.dirty = true; }

  if (!force && !st.dirty && minute == _lastMinute && st.screen == _lastScreen) return;

  _sig = ""; _sig.reserve(2048);
  _g->fillScreen(COL_BG);

  switch (st.screen) {
    case SC_MENU:         drawMenu(st, d); break;
    case SC_F1:           drawF1(st, d); break;
    case SC_SETTINGS:     drawSettings(st, d); break;
    case SC_ABOUT:        drawAbout(d); break;
    case SC_WIFI_CONNECT: drawConnect(st); break;
    case SC_WIFI_PROBLEM: drawProblem(st); break;
    case SC_WIFI_PORTAL:  drawPortal(st); break;
  }

  uint32_t h = fnv1a(_sig);
  if (h != _lastHash || force) { blit(); _lastHash = h; }
  _lastMinute = minute;
  _lastScreen = st.screen;
  st.dirty = false;
}

void Ui::busy(const String& msg) {
  _g->fillRect(30, 50, 180, 34, COL_BG);
  _g->drawRect(30, 50, 180, 34, COL_DIM);
  _g->setTextFont(2); _g->setTextSize(1); _g->setTextColor(COL_FG);
  _g->setTextDatum(textdatum_t::top_center);
  _g->drawString(msg, 120, 58);
  _g->setTextDatum(textdatum_t::top_left);
  blit();
  _lastHash = 0;
}

void Ui::drawMenu(AppState& st, F1Data& d) {
  hdr("F1 2026", WifiMgr::online() ? ctxRound(d) : "OFFLINE",
      COL_FG, WifiMgr::online() ? COL_DIM : COL_GREEN);

  const char* titles[2] = { "F1 INFO", "SETTINGS" };
  const char* subs[2]   = { "sessions - table - driver", "wi-fi - sync - about" };
  for (int i = 0; i < 2; ++i) {
    int y = BODY_Y + (i + 1) * ROW_H;
    bool sel = (st.menuSel == i);
    if (sel) _g->fillRect(0, y, SCR_W, ROW_H, COL_GREEN);
    txt(6, y, titles[i], sel ? COL_BG : COL_FG);
    txt(SCR_W - 6, y + 5, subs[i], sel ? COL_BG : COL_DIM, 1, textdatum_t::top_right);
  }
  int y = BODY_Y + 4 * ROW_H;
  if (!d.rounds.empty()) {
    if (d.nextRace.length() && clockOk(d.now) && d.nextRaceStart > d.now) {
      txt(6, y, "Next: " + d.nextRace, COL_DIM, 1);
      txt(6, y + 11, "in " + fmtDelta(d.nextRaceStart - d.now), COL_FG, 1);
    } else {
      txt(6, y, "Season complete", COL_DIM, 1);
    }
  } else {
    txt(6, y, WifiMgr::online() ? "No data yet - hold B to sync" : "Offline - open Settings",
        COL_DIM, 1);
  }
  if (d.drivers.size()) {
    char b[48];
    snprintf(b, sizeof(b), "%d drivers - R%d standings", (int)d.drivers.size(), d.champRound);
    txt(6, y + 22, b, COL_DIM, 1);
  }
  hints("A/B: move   hold A: open   hold B: sync");
}

void Ui::tabBar(uint8_t active, bool focused) {
  static const char* N[3] = { "LIVE", "TABLE", "DRIVER" };
  for (int i = 0; i < 3; ++i) {
    int x = i * 80;
    bool on = (i == (int)active);
    if (on && focused) _g->fillRect(x, 0, 80, HDR_H, COL_GREEN);
    uint16_t c = (on && focused) ? COL_BG : (on ? COL_FG : COL_DIM);
    txt(x + 40, 1, N[i], c, 2, textdatum_t::top_center);
    if (on && !focused) _g->fillRect(x + 18, HDR_H - 3, 44, 2, COL_GREEN);
    if (i) _g->drawFastVLine(x, 3, 12, COL_DIM);
  }
  _g->drawFastHLine(0, HDR_H - 1, SCR_W, COL_DIM);
}

void Ui::drawF1(AppState& st, F1Data& d) {
  if (st.focus == FOCUS_TABS) tabBar(st.tab, true);
  switch (st.tab) {
    case TAB_SESSION:   drawSession(st, d); break;
    case TAB_STANDINGS: drawStandings(st, d); break;
    case TAB_DRIVER:    drawDriver(st, d); break;
  }
}

void Ui::drawSession(AppState& st, F1Data& d) {
  const WeekendInfo& w = d.weekend;
  if (d.viewSession < 0 && !w.sessions.empty()) d.viewSession = 0;
  if (d.viewSession >= (int)w.sessions.size()) d.viewSession = (int)w.sessions.size() - 1;

  if (st.focus == FOCUS_BODY) {
    String left = st.sessListMode ? "SESSIONS"
                 : (d.orderTitle.length() ? d.orderTitle : "SESSION");
    hdr(left, ctxRound(d), COL_FG, COL_DIM);
  }

  if (st.sessListMode) {
    if (w.sessions.empty()) {
      box(50, "No session data", "hold B: menu", WifiMgr::online() ? "" : "device offline");
      hints("hold B: tabs");
      return;
    }
    int n = (int)w.sessions.size();
    for (int i = 0; i < ROWS; ++i) {
      int idx = st.sessTop + i;
      if (idx >= n) break;
      int y = BODY_Y + i * ROW_H;
      const SessionSlot& s = w.sessions[idx];
      bool sel = (idx == st.sessSel);
      if (sel) _g->fillRect(0, y, SCR_W, ROW_H, COL_GREEN);
      SessState ss = sessState(s, d.now);
      txt(6, y, s.name, sel ? COL_BG : COL_FG);
      txt(66, y, clockOk(d.now) ? fmtUTC(s.start) : "--:--", sel ? COL_BG : COL_DIM);
      const char* lab = (ss == SS_LIVE) ? "LIVE" : (ss == SS_DONE ? "DONE" : "NEXT");
      txt(228, y, lab, sel ? COL_BG : (ss == SS_LIVE ? COL_GREEN : COL_DIM),
          2, textdatum_t::top_right);
    }
    scrollbar(st.sessTop, n, ROWS);
    hints("A/B: move   hold A: show   hold B: tabs");
    return;
  }

  if (d.order.empty()) {
    String l2;
    if (d.viewSession >= 0 && d.viewSession < (int)w.sessions.size()) {
      const SessionSlot& s = w.sessions[d.viewSession];
      l2 = (sessState(s, d.now) == SS_UPCOMING)
             ? s.longName + " starts in " + fmtDelta(s.start - d.now)
             : s.longName + " - " + fmtUTC(s.start) + " UTC";
    }
    box(44, "No timing data", l2,
        d.orderSessionIdx >= 0 ? "Feed updates after the session"
                               : "Practice timing not in feed");
    hints("hold A: sessions   hold B: tabs");
    return;
  }

  int n = (int)d.order.size();
  String trackId = (st.driverSel >= 0 && st.driverSel < (int)d.drivers.size())
                     ? d.drivers[st.driverSel].id : String();
  for (int i = 0; i < ROWS; ++i) {
    int idx = st.orderTop + i;
    if (idx >= n) break;
    int y = BODY_Y + i * ROW_H;
    const OrderRow& r = d.order[idx];
    bool hi = trackId.length() && r.id == trackId;
    if (hi) _g->fillRect(0, y, SCR_W, ROW_H, COL_GREEN);
    uint16_t c1 = hi ? COL_BG : COL_FG;
    uint16_t c2 = hi ? COL_BG : COL_DIM;
    txt(20, y, String(r.pos), c1, 2, textdatum_t::top_right);
    txt(26, y, r.code, c1);
    txt(58, y, r.team, c2);
    if (r.right.length()) txt(230, y, clip(r.right, 110), c2, 2, textdatum_t::top_right);
  }
  scrollbar(st.orderTop, n, ROWS);
  hints("A/B: scroll   hold A: sessions   hold B: tabs");
}

void Ui::drawStandings(AppState& st, F1Data& d) {
  if (st.focus == FOCUS_BODY)
    hdr("STANDINGS", d.champRound ? ("R" + String(d.champRound)) : String("2026"), COL_FG, COL_DIM);

  if (d.drivers.empty()) {
    box(50, "No standings", WifiMgr::online() ? "hold B: sync on menu" : "device offline");
    hints("hold B: tabs");
    return;
  }
  int n = (int)d.drivers.size();
  for (int i = 0; i < ROWS; ++i) {
    int idx = st.standTop + i;
    if (idx >= n) break;
    const DriverInfo& dr = d.drivers[idx];
    int y = BODY_Y + i * ROW_H;
    bool sel = (idx == st.driverSel);
    if (sel) _g->fillRect(0, y, SCR_W, ROW_H, COL_GREEN);
    uint16_t c1 = sel ? COL_BG : COL_FG;
    uint16_t c2 = sel ? COL_BG : COL_DIM;
    txt(20, y, String(dr.champPos), c1, 2, textdatum_t::top_right);
    txt(26, y, dr.code, c1);
    txt(58, y, clip(dr.family, 102), c1);
    txt(196, y, dr.teamShort, c2, 2, textdatum_t::top_right);
    txt(230, y, ptsStr(dr.points), c1, 2, textdatum_t::top_right);
  }
  scrollbar(st.standTop, n, ROWS);
  hints("A/B: scroll   hold A: driver   hold B: tabs");
}

void Ui::drawDriver(AppState& st, F1Data& d) {
  if (d.drivers.empty()) {
    hdr("DRIVER", ctxRound(d), COL_FG, COL_DIM);
    box(50, "No driver data", "sync from the menu");
    hints("hold B: tabs");
    return;
  }
  if (st.driverSel < 0) st.driverSel = 0;
  if (st.driverSel >= (int)d.drivers.size()) st.driverSel = (int)d.drivers.size() - 1;
  const DriverInfo& dr = d.drivers[st.driverSel];

  if (st.focus == FOCUS_BODY) {
    String left = String(dr.number) + "  " + dr.family;
    left.toUpperCase();
    hdr(left, dr.team, COL_FG, COL_DIM);
  }

  DetailCache dc;
  auto it = d.details.find(dr.id);
  if (it != d.details.end()) dc = it->second;

  String latest = "-";
  if (dr.latestPos) latest = d.latestLabel.length() ? (d.latestLabel + " P" + String(dr.latestPos))
                                                    : ("P" + String(dr.latestPos));
  else if (dc.ok && dc.lastRace) latest = "RACE P" + String(dc.lastRace);

  String gap;
  if (!d.drivers.empty()) {
    float lead = d.drivers[0].points;
    if (dr.champPos == 1 || fabsf(lead - dr.points) < 0.01f) gap = "LEADER";
    else gap = "-" + ptsStr(lead - dr.points);
  }

  struct Row { const char* k; String v; };
  Row rows[6] = {
    { "CHAMPIONSHIP", dr.champPos ? ("P" + String(dr.champPos)) : String("-") },
    { "POINTS",       ptsStr(dr.points) },
    { "WINS / PODIUMS",
      String(dr.wins) + (dc.ok ? (" / " + String(dc.podiums)) : String(" / -")) },
    { "LAST SESSION", latest },
    { "BEST FINISH",  (dc.ok && dc.best) ? ("P" + String(dc.best)) : String("-") },
    { "GAP TO LEADER", gap },
  };
  for (int i = 0; i < 6; ++i) {
    int y = BODY_Y + i * ROW_H;
    txt(8, y + 5, rows[i].k, COL_DIM, 1);
    txt(230, y, rows[i].v, COL_FG, 2, textdatum_t::top_right);
  }
  _g->fillRect(3, BODY_Y + 1, 3, ROW_H - 2, COL_GREEN);
  hints("A/B: driver  hold A: reload  hold B: tabs");
}

void Ui::drawSettings(AppState& st, F1Data& d) {
  bool on = WifiMgr::online();
  hdr("SETTINGS", on ? "ONLINE" : "OFFLINE", COL_FG, on ? COL_GREEN : COL_DIM);

  txt(8, BODY_Y + 2, "WI-FI", COL_DIM, 1);
  String state = on ? "Connected" : (WifiMgr::hasCreds() ? "Not connected" : "Not configured");
  txt(8, BODY_Y + 11, state, on ? COL_GREEN : COL_FG);
  if (on) {
    String info = WifiMgr::ssid() + "  " + String(WiFi.RSSI()) + "dBm";
    txt(232, BODY_Y + 11, clip(info, 120, 1), COL_DIM, 1, textdatum_t::top_right);
    txt(8, BODY_Y + 29, WiFi.localIP().toString(), COL_DIM, 1);
  } else {
    txt(8, BODY_Y + 29, WifiMgr::hasCreds() ? WifiMgr::ssid() : "Run Wi-Fi setup", COL_DIM, 1);
  }

  static const char* items[4] = { "Wi-Fi setup", "Reconnect", "Refresh now", "About" };
  static const char* notes[4] = { "AP portal", "saved net", "force sync", APP_VERSION };
  for (int i = 0; i < 4; ++i) {
    int y = BODY_Y + 46 + i * ROW_H;
    bool sel = (st.settingsSel == i);
    if (sel) _g->fillRect(0, y, SCR_W, ROW_H, COL_GREEN);
    txt(8, y, items[i], sel ? COL_BG : COL_FG);
    txt(230, y + 5, notes[i], sel ? COL_BG : COL_DIM, 1, textdatum_t::top_right);
  }
  hints("A/B: move   hold A: open   hold B: back");
}

void Ui::drawAbout(F1Data& d) {
  hdr("ABOUT", APP_VERSION, COL_FG, COL_DIM);
  txt(8, BODY_Y + 2,  "F1 Stick for M5StickC PLUS2", COL_FG, 1);
  txt(8, BODY_Y + 14, "Data: Jolpica F1 API", COL_DIM, 1);
  txt(8, BODY_Y + 26, "(Ergast compatible, no key)", COL_DIM, 1);
  txt(8, BODY_Y + 38, "All times UTC", COL_DIM, 1);
  char b[48];
  snprintf(b, sizeof(b), "Free heap: %u KB", (unsigned)(ESP.getFreeHeap() / 1024));
  txt(8, BODY_Y + 50, b, COL_DIM, 1);
  snprintf(b, sizeof(b), "Rounds: %d  Drivers: %d", (int)d.rounds.size(), (int)d.drivers.size());
  txt(8, BODY_Y + 62, b, COL_DIM, 1);
  hints("hold B: back");
}

void Ui::drawConnect(AppState& st) {
  hdr("WI-FI", "CONNECTING", COL_FG, COL_GREEN);
  txt(8, BODY_Y + 4, "Joining", COL_DIM, 1);
  txt(8, BODY_Y + 14, clip(st.wifiSsid, 220), COL_FG);
  int dots = (st.wifiElapsed / 300) % 4;
  String d; for (int i = 0; i < dots; ++i) d += ".";
  txt(8, BODY_Y + 38, "Please wait" + d, COL_DIM, 2);
  int left = (st.wifiTotal - st.wifiElapsed) / 1000;
  if (left < 0) left = 0;
  txt(8, BODY_Y + 62, "Timeout in " + String(left) + "s", COL_DIM, 1);
  hints("hold B: skip");
}

void Ui::drawProblem(AppState& st) {
  hdr("WI-FI", "SETUP", COL_FG, COL_DIM);
  txt(8, BODY_Y + 2, clip(st.problemMsg, 224), COL_FG, 2);
  const char* sub = (st.problemMode == PM_NO_CREDS)
                      ? "Connect the stick to your network."
                      : "Check password or pick another network.";
  txt(8, BODY_Y + 22, clip(sub, 224, 1), COL_DIM, 1);
  static const char* items[3] = { "Start Wi-Fi setup", "Retry connection", "Continue offline" };
  for (int i = 0; i < 3; ++i) {
    int y = BODY_Y + 40 + i * ROW_H;
    bool sel = (st.problemSel == i);
    if (sel) _g->fillRect(0, y, SCR_W, ROW_H, COL_GREEN);
    txt(8, y, items[i], sel ? COL_BG : COL_FG);
  }
  hints("A/B: move   hold A: open   hold B: menu");
}

void Ui::drawPortal(AppState& st) {
  hdr("WI-FI SETUP", WifiMgr::portalActive() ? "AP" : "STARTING", COL_FG, COL_GREEN);
  txt(8, BODY_Y + 2,  "1. Join Wi-Fi network", COL_DIM, 1);
  txt(8, BODY_Y + 12, WifiMgr::apName(), COL_FG);
  txt(8, BODY_Y + 32, "2. Open in browser", COL_DIM, 1);
  txt(8, BODY_Y + 42, WifiMgr::apUrl(), COL_FG);
  txt(8, BODY_Y + 66, "Screen updates when saved.", COL_DIM, 1);
  hints("hold B: cancel");
}

// --------------------------------------------------------------- globals
AppState    st;
F1Data      data;
Ui          ui;
Preferences appPrefs;

static int      pendingDetailFor = -1;
static uint32_t pendingDetailAt  = 0;
static uint32_t lastDriverSave   = 0;
static bool     driverSavePending = false;

static void ensureVisible(int sel, int& top, int rows, int count) {
  if (count <= rows) { top = 0; return; }
  if (sel < top) top = sel;
  else if (sel >= top + rows) top = sel - rows + 1;
  if (top > count - rows) top = count - rows;
  if (top < 0) top = 0;
}
static void toast(const String& s) { st.toast = s; st.toastUntil = millis() + 2200; st.dirty = true; }
static bool wifiReady() { return WiFi.status() == WL_CONNECTED; }

void savePrefsDriver() {
  if (st.driverSel >= 0 && st.driverSel < (int)data.drivers.size())
    appPrefs.putString("drv", data.drivers[st.driverSel].id);
}

static void apiProgress(const char* m) { ui.busy(String(m) + "..."); }

int pickRound(const std::vector<RoundBrief>& r, int64_t now) {
  if (r.empty()) return 0;
  for (auto& b : r)
    if (now >= b.firstStart - 12 * 3600LL && now <= b.raceStart + 6 * 3600LL) return b.round;
  int last = 0;
  for (auto& b : r) if (b.raceStart + 6 * 3600LL <= now) last = b.round;
  if (last) return last;
  for (auto& b : r) if (b.raceStart > now) return b.round;
  return r.back().round;
}

void updateNextRace() {
  data.nextRace = ""; data.nextRaceStart = 0;
  int64_t now = (int64_t)time(nullptr);
  for (auto& b : data.rounds)
    if (b.raceStart > now) { data.nextRace = b.raceName; data.nextRaceStart = b.raceStart; break; }
}

int latestResultsSession(const F1Data& d) {
  if (!clockOk(d.now)) return -1;
  int idx = -1;
  for (size_t i = 0; i < d.weekend.sessions.size(); ++i) {
    const SessionSlot& s = d.weekend.sessions[i];
    if (s.kind == SK_PRACTICE) continue;
    if (d.now >= s.start + (int64_t)s.durMin * 60) idx = (int)i;
  }
  return idx;
}

void applyDefaultSession(bool force) {
  const auto& ss = data.weekend.sessions;
  if (ss.empty()) return;
  if (!force && data.viewSession >= 0 && data.viewSession < (int)ss.size()) return;
  int idx = -1;
  for (size_t i = 0; i < ss.size(); ++i) if (data.now >= ss[i].start) idx = (int)i;
  if (idx < 0) idx = 0;
  data.viewSession = idx;
  st.sessSel = idx;
  st.sessListMode = (ss[idx].kind == SK_PRACTICE);
  st.orderTop = 0;
  data.orderSessionIdx = -1;
}

void loadScheduleAndRound(bool force) {
  String err;
  int64_t now = (int64_t)time(nullptr);
  data.now = now;

  if (force || !data.scheduleOk || millis() - data.scheduleStamp > R_SCHEDULE_MS) {
    if (fetchSchedule(SEASON_YEAR, data.rounds, err, apiProgress)) {
      data.scheduleOk = true; data.scheduleStamp = millis();
    }
  }
  if (data.rounds.empty()) return;

  int round = pickRound(data.rounds, now);
  if (round != data.curRound || !data.weekend.valid || force) {
    WeekendInfo w;
    if (fetchRound(SEASON_YEAR, round, w, err, apiProgress)) {
      bool newWeekend = (data.weekend.round != w.round);
      data.weekend = w;
      data.curRound = round;
      if (newWeekend || force) {
        data.order.clear(); data.orderTitle = "";
        data.orderSessionIdx = -1; data.orderOk = false;
        applyDefaultSession(true);
      }
    }
  }
  applyDefaultSession(false);
  updateNextRace();
}

void loadStandings(bool force) {
  if (!force && data.standingsOk && millis() - data.standingsStamp < R_STANDINGS_MS) return;
  std::vector<DriverInfo> fresh;
  int round = 0; String err;
  if (!fetchStandings(SEASON_YEAR, fresh, round, err, apiProgress)) return;

  String keep = (st.driverSel >= 0 && st.driverSel < (int)data.drivers.size())
                  ? data.drivers[st.driverSel].id : appPrefs.getString("drv", "");
  std::map<String, DetailCache> cache = data.details;
  data.drivers = fresh;
  data.champRound = round;
  data.standingsOk = true;
  data.standingsStamp = millis();
  data.details = cache;

  st.driverSel = 0;
  for (size_t i = 0; i < data.drivers.size(); ++i)
    if (data.drivers[i].id == keep) { st.driverSel = (int)i; break; }
  ensureVisible(st.driverSel, st.standTop, ROWS, (int)data.drivers.size());
}

void loadOrder(bool force) {
  if (!wifiReady()) return;
  int idx = data.viewSession;
  if (idx < 0 || idx >= (int)data.weekend.sessions.size()) return;

  const SessionSlot& s = data.weekend.sessions[idx];
  SessState ss = SS_UPCOMING; bool live = false;
  if (clockOk(data.now)) {
    if (data.now < s.start) ss = SS_UPCOMING;
    else if (data.now < s.start + (int64_t)s.durMin * 60) { ss = SS_LIVE; live = true; }
    else ss = SS_DONE;
  }
  uint32_t age = millis() - data.orderStamp;
  bool stale = (data.orderSessionIdx != idx) ||
               (live ? age > R_ORDER_LIVE_MS : age > R_ORDER_IDLE_MS);
  if (!force && !stale) return;

  std::vector<OrderRow> rows; String title, err;
  bool ok = fetchOrder(SEASON_YEAR, data.weekend.round, s.kind, rows, title, err, apiProgress);
  data.orderStamp = millis();
  data.orderSessionIdx = idx;
  data.orderOk = ok;
  if (ok) {
    data.order = rows;
    data.orderTitle = title;
    if (idx == latestResultsSession(data)) {
      data.latestLabel = title;
      for (auto& dr : data.drivers) {
        dr.latestPos = 0;
        for (auto& r : rows) if (r.id == dr.id) { dr.latestPos = (uint8_t)r.pos; break; }
      }
    }
    String tid = (st.driverSel >= 0 && st.driverSel < (int)data.drivers.size())
                   ? data.drivers[st.driverSel].id : "";
    st.orderTop = 0;
    for (size_t i = 0; i < rows.size(); ++i)
      if (tid.length() && rows[i].id == tid) {
        st.orderTop = (int)i - 2;
        if (st.orderTop < 0) st.orderTop = 0;
        int maxTop = (int)rows.size() - ROWS;
        if (maxTop < 0) maxTop = 0;
        if (st.orderTop > maxTop) st.orderTop = maxTop;
        break;
      }
  } else {
    data.order.clear(); data.orderTitle = "";
  }
  st.dirty = true;
}

void loadDetail(int sel, bool force) {
  if (!wifiReady()) return;
  if (sel < 0 || sel >= (int)data.drivers.size()) return;
  const String& id = data.drivers[sel].id;
  auto it = data.details.find(id);
  if (!force && it != data.details.end() && millis() - it->second.stamp < R_DETAIL_MS) return;
  int best = 0, pod = 0, last = 0; String err;
  if (fetchDriverSeason(SEASON_YEAR, id, best, pod, last, err, nullptr)) {
    DetailCache c; c.best = best; c.podiums = pod; c.lastRace = last;
    c.stamp = millis(); c.ok = true;
    data.details[id] = c;
    st.dirty = true;
  }
}

void loadAll(bool initial) {
  ui.busy("Updating...");
  loadScheduleAndRound(initial);
  loadStandings(initial);
  loadOrder(false);
  if (initial) loadDetail(st.driverSel, true);
  st.dirty = true;
}

// --------------------------------------------------------------- wifi flows
static bool connectWithUi() {
  st.screen = SC_WIFI_CONNECT;
  st.wifiSsid = WifiMgr::ssid();
  st.wifiElapsed = 0;
  st.wifiTotal = WIFI_TIMEOUT_MS;
  ui.render(st, data, true);

  bool ok = WifiMgr::connect(WIFI_TIMEOUT_MS, [](int el, int tot) -> bool {
    M5.update();
    if (M5.BtnB.wasHold()) return false;
    st.wifiElapsed = el; st.wifiTotal = tot;
    ui.render(st, data, true);
    return true;
  });

  if (ok) {
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    uint32_t t0 = millis();
    while (millis() - t0 < 5000 && !clockOk((int64_t)time(nullptr))) delay(80);
  }
  return ok;
}

void gotoProblem(ProblemMode m, const String& msg) {
  st.problemMode = m;
  st.problemMsg = msg;
  st.problemSel = (m == PM_NO_CREDS) ? 0 : 1;
  st.screen = SC_WIFI_PROBLEM;
  st.dirty = true;
}
void enterOffline() { st.screen = SC_MENU; st.dirty = true; toast("offline mode"); }

// --------------------------------------------------------------- input
static void moveDriver(int dir) {
  int n = (int)data.drivers.size();
  if (!n) return;
  st.driverSel = (st.driverSel + dir + n) % n;
  ensureVisible(st.driverSel, st.standTop, ROWS, n);
  driverSavePending = true;
  pendingDetailFor = st.driverSel;
  pendingDetailAt = millis() + 700;
  st.dirty = true;
}

static void handleMenu(bool aC, bool aH, bool bC, bool bH) {
  if (aC || bC) { st.menuSel = (st.menuSel + 1) % 2; st.dirty = true; }
  if (aH) {
    if (st.menuSel == 0) {
      st.screen = SC_F1;
      st.focus = FOCUS_TABS;
      st.tab = TAB_SESSION;
      if (data.drivers.empty() || !data.weekend.valid) loadAll(false);
    } else {
      st.screen = SC_SETTINGS;
      st.settingsSel = 0;
    }
    st.dirty = true;
  }
  if (bH) {
    if (wifiReady()) loadAll(false);
    else gotoProblem(PM_LOST, "No Wi-Fi connection");
  }
}

static void handleF1(bool aC, bool aH, bool bC, bool bH) {
  if (st.focus == FOCUS_TABS) {
    if (aC) { st.tab = (st.tab + 1) % 3; st.dirty = true; }
    if (bC) { st.tab = (st.tab + 2) % 3; st.dirty = true; }
    if (aH) { st.focus = FOCUS_BODY; st.dirty = true; }
    if (bH) { st.screen = SC_MENU; st.dirty = true; }
    return;
  }
  if (bH) { st.focus = FOCUS_TABS; st.dirty = true; return; }

  if (st.tab == TAB_SESSION) {
    if (st.sessListMode) {
      int n = (int)data.weekend.sessions.size();
      if (aC && n) { st.sessSel = (st.sessSel + 1) % n; ensureVisible(st.sessSel, st.sessTop, ROWS, n); st.dirty = true; }
      if (bC && n) { st.sessSel = (st.sessSel + n - 1) % n; ensureVisible(st.sessSel, st.sessTop, ROWS, n); st.dirty = true; }
      if (aH && n) {
        data.viewSession = st.sessSel;
        st.sessListMode = false;
        data.orderSessionIdx = -1;
        loadOrder(true);
        st.dirty = true;
      }
    } else {
      int n = (int)data.order.size();
      int maxTop = std::max(0, n - ROWS);
      if (aC) { if (st.orderTop < maxTop) st.orderTop++; st.dirty = true; }
      if (bC) { if (st.orderTop > 0) st.orderTop--; st.dirty = true; }
      if (aH) {
        st.sessListMode = true;
        ensureVisible(st.sessSel, st.sessTop, ROWS, (int)data.weekend.sessions.size());
        st.dirty = true;
      }
    }
  } else if (st.tab == TAB_STANDINGS) {
    if (aC) moveDriver(+1);
    if (bC) moveDriver(-1);
    if (aH) { st.tab = TAB_DRIVER; pendingDetailFor = st.driverSel; pendingDetailAt = millis() + 250; st.dirty = true; }
  } else {
    if (aC) moveDriver(+1);
    if (bC) moveDriver(-1);
    if (aH) loadDetail(st.driverSel, true);
  }
}

static void handleSettings(bool aC, bool aH, bool bC, bool bH) {
  if (aC) { st.settingsSel = (st.settingsSel + 1) % 4; st.dirty = true; }
  if (bC) { st.settingsSel = (st.settingsSel + 3) % 4; st.dirty = true; }
  if (bH) { st.screen = SC_MENU; st.dirty = true; }
  if (!aH) return;
  switch (st.settingsSel) {
    case 0:
      WifiMgr::startPortal();
      st.screen = SC_WIFI_PORTAL;
      break;
    case 1:
      if (!WifiMgr::hasCreds()) { gotoProblem(PM_NO_CREDS, "No Wi-Fi saved yet"); break; }
      if (connectWithUi()) { toast("connected"); loadAll(false); st.screen = SC_MENU; }
      else gotoProblem(PM_FAILED, "Could not connect");
      break;
    case 2:
      if (!wifiReady()) { gotoProblem(PM_LOST, "No Wi-Fi connection"); break; }
      loadAll(true); toast("synced");
      break;
    case 3:
      st.screen = SC_ABOUT;
      break;
  }
  st.dirty = true;
}

static void handleProblem(bool aC, bool aH, bool bC, bool bH) {
  if (aC) { st.problemSel = (st.problemSel + 1) % 3; st.dirty = true; }
  if (bC) { st.problemSel = (st.problemSel + 2) % 3; st.dirty = true; }
  if (bH) { enterOffline(); return; }
  if (!aH) return;
  switch (st.problemSel) {
    case 0:
      WifiMgr::startPortal();
      st.screen = SC_WIFI_PORTAL;
      break;
    case 1:
      if (!WifiMgr::hasCreds()) { gotoProblem(PM_NO_CREDS, "No Wi-Fi saved yet"); break; }
      if (connectWithUi()) { loadAll(true); st.screen = SC_MENU; }
      else gotoProblem(PM_FAILED, "Still no connection");
      break;
    case 2:
      enterOffline();
      break;
  }
  st.dirty = true;
}

void handleInput() {
  M5.update();
  bool aC = M5.BtnA.wasClicked(), aH = M5.BtnA.wasHold();
  bool bC = M5.BtnB.wasClicked(), bH = M5.BtnB.wasHold();
  if (M5.BtnPWR.wasClicked() && st.screen != SC_MENU && st.screen != SC_WIFI_PORTAL) {
    st.screen = SC_MENU; st.dirty = true;
  }
  if (!(aC || aH || bC || bH)) return;

  switch (st.screen) {
    case SC_MENU:         handleMenu(aC, aH, bC, bH); break;
    case SC_F1:           handleF1(aC, aH, bC, bH); break;
    case SC_SETTINGS:     handleSettings(aC, aH, bC, bH); break;
    case SC_ABOUT:
      if (bH || aH) { st.screen = SC_SETTINGS; st.dirty = true; }
      break;
    case SC_WIFI_PROBLEM: handleProblem(aC, aH, bC, bH); break;
    case SC_WIFI_PORTAL:
      if (bH) { WifiMgr::stopPortal(); gotoProblem(PM_FAILED, "Wi-Fi setup cancelled"); }
      break;
    case SC_WIFI_CONNECT: break;
  }
}

// --------------------------------------------------------------- services
static uint32_t offlineSince = 0;
static uint32_t lastReconnectTry = 0;

void serviceWifi() {
  if (st.screen == SC_WIFI_PORTAL) { WifiMgr::portalLoop(); return; }
  if (st.screen == SC_WIFI_CONNECT) return;

  if (wifiReady()) { offlineSince = 0; return; }
  if (!offlineSince) offlineSince = millis();

  if (millis() - offlineSince > 15000 && millis() - lastReconnectTry > 15000) {
    lastReconnectTry = millis();
    WiFi.reconnect();
  }
  if (millis() - offlineSince > 45000 && st.screen != SC_WIFI_PROBLEM)
    gotoProblem(PM_LOST, "Wi-Fi connection lost");
}

void serviceData() {
  if (!wifiReady()) return;

  if (pendingDetailFor >= 0 && millis() >= pendingDetailAt) {
    int sel = pendingDetailFor; pendingDetailFor = -1;
    loadDetail(sel, false);
  }
  if (driverSavePending && millis() - lastDriverSave > 2500) {
    savePrefsDriver(); driverSavePending = false; lastDriverSave = millis();
  }

  static uint32_t lastIdle = 0;
  if (millis() - lastIdle < 1000) return;
  lastIdle = millis();

  if (!data.scheduleOk || millis() - data.scheduleStamp > R_SCHEDULE_MS) loadScheduleAndRound(false);
  if (!data.standingsOk || millis() - data.standingsStamp > R_STANDINGS_MS) loadStandings(false);
  if (st.screen == SC_F1) loadOrder(false);
}

// --------------------------------------------------------------- setup / loop
void setup() {
  auto cfg = M5.config();
  cfg.internal_imu = false;
  cfg.internal_spk = false;
  cfg.internal_mic = false;
  cfg.clear_display = true;
  M5.begin(cfg);

  M5.Display.setRotation(1);          // landscape 240x135
  M5.Display.setBrightness(150);
  M5.BtnA.setHoldThresh(600);
  M5.BtnB.setHoldThresh(600);

  Serial.begin(115200);
  WifiMgr::begin();
  appPrefs.begin("f1app", false);
  ui.begin();
  ui.render(st, data, true);

  String keep = appPrefs.getString("drv", "");

  if (!WifiMgr::hasCreds()) { gotoProblem(PM_NO_CREDS, "No Wi-Fi saved"); return; }

  if (connectWithUi()) {
    loadAll(true);
    if (keep.length())
      for (size_t i = 0; i < data.drivers.size(); ++i)
        if (data.drivers[i].id == keep) { st.driverSel = (int)i; break; }
    ensureVisible(st.driverSel, st.standTop, ROWS, (int)data.drivers.size());
    st.screen = SC_MENU;
    st.dirty = true;
  } else {
    gotoProblem(PM_FAILED, "Could not connect");
  }
}

void loop() {
  handleInput();
  serviceWifi();
  serviceData();
  ui.render(st, data);
  delay(8);
}