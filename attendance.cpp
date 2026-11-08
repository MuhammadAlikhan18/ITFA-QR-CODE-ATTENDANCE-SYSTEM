// EF101-P10: QR-Based Student Attendance System (console prototype)
// Build:  g++ -std=c++17 -O2 attendance.cpp -o attendance
// Run:    ./attendance
//
// Data is stored in plain CSV files next to the program:
//   students.csv, courses.csv, sessions.csv, attendance.csv

#include <algorithm>
#include <climits>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#define NOMINMAX                 // stop windows.h from breaking std::min / std::max
#include <windows.h>
#endif

using namespace std;

// =====================================================================
//  PART 1: Minimal QR code generator (byte mode, ECC level L, versions 1-5)
//  Supports up to 106 bytes of text, which is plenty for our payload.
// =====================================================================
namespace qr {

// Data codewords and error-correction codewords per version (1..5), ECC level L, single block
const int DATA_CW[6] = {0, 19, 34, 55, 80, 108};
const int ECC_CW[6]  = {0, 7, 10, 15, 20, 26};

// Multiplication in GF(256) with polynomial 0x11D
static uint8_t gfMul(uint8_t x, uint8_t y) {
    int z = 0;
    for (int i = 7; i >= 0; i--) {
        z = (z << 1) ^ ((z >> 7) * 0x11D);
        z ^= ((y >> i) & 1) * x;
    }
    return (uint8_t)z;
}

// Reed-Solomon generator polynomial of the given degree
static vector<uint8_t> rsGenerator(int degree) {
    vector<uint8_t> res(degree, 0);
    res[degree - 1] = 1;
    uint8_t root = 1;
    for (int i = 0; i < degree; i++) {
        for (int j = 0; j < degree; j++) {
            res[j] = gfMul(res[j], root);
            if (j + 1 < degree) res[j] ^= res[j + 1];
        }
        root = gfMul(root, 0x02);
    }
    return res;
}

// Reed-Solomon remainder = the error-correction bytes
static vector<uint8_t> rsRemainder(const vector<uint8_t>& data, const vector<uint8_t>& gen) {
    vector<uint8_t> res(gen.size(), 0);
    for (uint8_t b : data) {
        uint8_t factor = b ^ res[0];
        res.erase(res.begin());
        res.push_back(0);
        for (size_t i = 0; i < res.size(); i++) res[i] ^= gfMul(gen[i], factor);
    }
    return res;
}

// Returns the QR matrix: matrix[y][x] == 1 means a dark module
vector<vector<char>> encode(const string& text) {
    // 1) Choose the smallest version that fits: 4 (mode) + 8 (length) + 8*len bits
    int ver = 0;
    for (int v = 1; v <= 5; v++) {
        if (4 + 8 + 8 * (int)text.size() <= DATA_CW[v] * 8) { ver = v; break; }
    }
    if (!ver) throw runtime_error("Text is too long for this QR generator");
    const int dataCw = DATA_CW[ver], eccCw = ECC_CW[ver];

    // 2) Build the bit stream
    vector<int> bits;
    auto put = [&](unsigned val, int n) {
        for (int i = n - 1; i >= 0; i--) bits.push_back((val >> i) & 1);
    };
    put(0x4, 4);                     // byte mode indicator
    put((unsigned)text.size(), 8);   // character count
    for (unsigned char c : text) put(c, 8);
    put(0, min(4, dataCw * 8 - (int)bits.size()));  // terminator
    while (bits.size() % 8) bits.push_back(0);

    vector<uint8_t> cw;
    for (size_t i = 0; i < bits.size(); i += 8) {
        int b = 0;
        for (int j = 0; j < 8; j++) b = (b << 1) | bits[i + j];
        cw.push_back((uint8_t)b);
    }
    for (uint8_t pad = 0xEC; (int)cw.size() < dataCw; pad ^= (0xEC ^ 0x11)) cw.push_back(pad);

    // 3) Append error correction bytes
    vector<uint8_t> ecc = rsRemainder(cw, rsGenerator(eccCw));
    cw.insert(cw.end(), ecc.begin(), ecc.end());

    // 4) Draw the fixed patterns
    const int size = 17 + 4 * ver;
    vector<vector<char>> m(size, vector<char>(size, 0));   // module colours
    vector<vector<char>> f(size, vector<char>(size, 0));   // 1 = function module (not data)
    auto setF = [&](int x, int y, bool dark) { m[y][x] = dark; f[y][x] = 1; };

    for (int i = 0; i < size; i++) {            // timing patterns
        setF(6, i, i % 2 == 0);
        setF(i, 6, i % 2 == 0);
    }
    auto finder = [&](int cx, int cy) {         // finder pattern + separator
        for (int dy = -4; dy <= 4; dy++)
            for (int dx = -4; dx <= 4; dx++) {
                int x = cx + dx, y = cy + dy;
                int dist = max(abs(dx), abs(dy));
                if (x >= 0 && x < size && y >= 0 && y < size) setF(x, y, dist != 2 && dist != 4);
            }
    };
    finder(3, 3);
    finder(size - 4, 3);
    finder(3, size - 4);

    if (ver >= 2) {                              // one alignment pattern (bottom-right)
        int c = size - 7;
        for (int dy = -2; dy <= 2; dy++)
            for (int dx = -2; dx <= 2; dx++) setF(c + dx, c + dy, max(abs(dx), abs(dy)) != 1);
    }

    // Format information (ECC level L + mask number)
    auto drawFormat = [&](int mask) {
        int d = (1 << 3) | mask;                 // L = 01, then 3 mask bits
        int rem = d;
        for (int i = 0; i < 10; i++) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
        int bitsF = ((d << 10) | rem) ^ 0x5412;
        auto gb = [&](int i) { return ((bitsF >> i) & 1) != 0; };
        for (int i = 0; i <= 5; i++) setF(8, i, gb(i));
        setF(8, 7, gb(6));
        setF(8, 8, gb(7));
        setF(7, 8, gb(8));
        for (int i = 9; i < 15; i++) setF(14 - i, 8, gb(i));
        for (int i = 0; i < 8; i++) setF(size - 1 - i, 8, gb(i));
        for (int i = 8; i < 15; i++) setF(8, size - 15 + i, gb(i));
        setF(8, size - 8, true);                 // the always-dark module
    };
    drawFormat(0);                               // reserve the area

    // 5) Place data bits in the zig-zag pattern
    size_t bitIdx = 0;
    for (int right = size - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;
        for (int vert = 0; vert < size; vert++) {
            for (int j = 0; j < 2; j++) {
                int x = right - j;
                bool upward = ((right + 1) & 2) == 0;
                int y = upward ? size - 1 - vert : vert;
                if (!f[y][x] && bitIdx < cw.size() * 8) {
                    m[y][x] = (cw[bitIdx >> 3] >> (7 - (bitIdx & 7))) & 1;
                    bitIdx++;
                }
            }
        }
    }

    // 6) Masking: try all 8 masks, keep the one with the lowest penalty
    auto applyMask = [&](int mask) {
        for (int y = 0; y < size; y++)
            for (int x = 0; x < size; x++) {
                bool inv = false;
                switch (mask) {
                    case 0: inv = (x + y) % 2 == 0; break;
                    case 1: inv = y % 2 == 0; break;
                    case 2: inv = x % 3 == 0; break;
                    case 3: inv = (x + y) % 3 == 0; break;
                    case 4: inv = (x / 3 + y / 2) % 2 == 0; break;
                    case 5: inv = x * y % 2 + x * y % 3 == 0; break;
                    case 6: inv = (x * y % 2 + x * y % 3) % 2 == 0; break;
                    case 7: inv = ((x + y) % 2 + x * y % 3) % 2 == 0; break;
                }
                if (!f[y][x] && inv) m[y][x] = !m[y][x];
            }
    };
    auto penalty = [&]() {
        int res = 0;
        for (int dir = 0; dir < 2; dir++)        // rule 1: long runs
            for (int a = 0; a < size; a++) {
                int run = 1;
                for (int b = 1; b < size; b++) {
                    char cur = dir ? m[b][a] : m[a][b];
                    char prv = dir ? m[b - 1][a] : m[a][b - 1];
                    if (cur == prv) run++;
                    else { if (run >= 5) res += run - 2; run = 1; }
                }
                if (run >= 5) res += run - 2;
            }
        for (int y = 0; y + 1 < size; y++)       // rule 2: 2x2 blocks
            for (int x = 0; x + 1 < size; x++)
                if (m[y][x] == m[y][x + 1] && m[y][x] == m[y + 1][x] && m[y][x] == m[y + 1][x + 1]) res += 3;
        static const int p1[11] = {1,0,1,1,1,0,1,0,0,0,0};
        static const int p2[11] = {0,0,0,0,1,0,1,1,1,0,1};
        for (int dir = 0; dir < 2; dir++)        // rule 3: finder-like patterns
            for (int a = 0; a < size; a++)
                for (int b = 0; b + 11 <= size; b++) {
                    bool ok1 = true, ok2 = true;
                    for (int k = 0; k < 11; k++) {
                        int v = dir ? m[b + k][a] : m[a][b + k];
                        if (v != p1[k]) ok1 = false;
                        if (v != p2[k]) ok2 = false;
                    }
                    if (ok1) res += 40;
                    if (ok2) res += 40;
                }
        int dark = 0, total = size * size;       // rule 4: dark/light balance
        for (auto& row : m) for (char c : row) dark += c;
        int k = (abs(dark * 20 - total * 10) + total - 1) / total - 1;
        res += max(k, 0) * 10;
        return res;
    };

    int best = 0, bestPen = INT_MAX;
    for (int mk = 0; mk < 8; mk++) {
        applyMask(mk);
        drawFormat(mk);
        int p = penalty();
        if (p < bestPen) { bestPen = p; best = mk; }
        applyMask(mk);                           // undo
    }
    applyMask(best);
    drawFormat(best);
    return m;
}

// Print QR in the terminal. Dark module = 2 spaces, light module = full block.
// (Works for dark terminals; on a light terminal use the SVG file instead.)
// Compact version: 1 character per module horizontally and 2 modules per character
// vertically (half-block characters), so it fits narrow phone screens (29 columns).
void printConsole(const vector<vector<char>>& m) {
    int n = (int)m.size(), quiet = 4;
    auto dark = [&](int x, int y) {
        return x >= 0 && y >= 0 && x < n && y < n && m[y][x];
    };
    for (int y = -quiet; y < n + quiet; y += 2) {
        for (int x = -quiet; x < n + quiet; x++) {
            bool top = dark(x, y), bottom = dark(x, y + 1);
            if (!top && !bottom)      cout << "\xE2\x96\x88";   // full block  (white)
            else if (!top && bottom)  cout << "\xE2\x96\x80";   // upper half  (white above, dark below)
            else if (top && !bottom)  cout << "\xE2\x96\x84";   // lower half  (dark above, white below)
            else                      cout << " ";              // dark
        }
        cout << "\n";
    }
}

// Save QR as an SVG image (open in any browser, scan from the screen or print it)
bool saveSvg(const vector<vector<char>>& m, const string& filename) {
    int n = (int)m.size(), border = 4, total = n + 2 * border;
    ofstream out(filename);
    if (!out) return false;
    out << "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 " << total << " " << total
        << "\" width=\"400\" height=\"400\" shape-rendering=\"crispEdges\">\n"
        << "<rect width=\"100%\" height=\"100%\" fill=\"#fff\"/>\n<path fill=\"#000\" d=\"";
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            if (m[y][x]) out << "M" << (x + border) << "," << (y + border) << "h1v1h-1z";
    out << "\"/>\n</svg>\n";
    return true;
}

}  // namespace qr

// =====================================================================
//  PART 2: Attendance system
// =====================================================================

struct Student { string id, name; };
struct Course  { string code, name; };
struct Session {
    int id;
    string course, date, time, token;
    long long created;   // creation time (epoch seconds)
    int validMin;        // QR validity in minutes, 0 = never expires
};
struct Record  { int sessionId; string studentId, timestamp; };

static vector<Student> students;
static vector<Course>  courses;
static vector<Session> sessions;
static vector<Record>  records;

static const string ADMIN_PASSWORD = "admin123";   // demo only, not secure

// ---------- small helpers ----------
static string lower(string s) {
    transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return tolower(c); });
    return s;
}
static string trim(const string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}
static vector<string> split(const string& line, char sep) {
    vector<string> parts;
    string cur;
    stringstream ss(line);
    while (getline(ss, cur, sep)) parts.push_back(trim(cur));
    return parts;
}
static string readLine(const string& prompt) {
    cout << prompt;
    string s;
    if (!getline(cin, s)) { cout << "\nInput closed. Bye.\n"; exit(0); }
    s = trim(s);
    replace(s.begin(), s.end(), ',', ' ');   // commas would break our CSV files
    return s;
}
static int readInt(const string& prompt) {
    string s = readLine(prompt);
    try { return stoi(s); } catch (...) { return -1; }
}
static string nowString() {
    time_t t = time(nullptr);
    char buf[32];
    strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", localtime(&t));
    return buf;
}
static bool validDate(const string& d) {   // YYYY-MM-DD
    if (d.size() != 10 || d[4] != '-' || d[7] != '-') return false;
    for (int i : {0,1,2,3,5,6,8,9}) if (!isdigit((unsigned char)d[i])) return false;
    int mo = stoi(d.substr(5, 2)), da = stoi(d.substr(8, 2));
    return mo >= 1 && mo <= 12 && da >= 1 && da <= 31;
}
static bool validTime(const string& t) {   // HH:MM
    if (t.size() != 5 || t[2] != ':') return false;
    for (int i : {0,1,3,4}) if (!isdigit((unsigned char)t[i])) return false;
    return stoi(t.substr(0, 2)) < 24 && stoi(t.substr(3, 2)) < 60;
}
static string randomToken() {              // 6 hex characters, hard to guess
    static mt19937 rng((unsigned)time(nullptr) ^ (unsigned)random_device{}());
    const char* hex = "0123456789abcdef";
    string t;
    for (int i = 0; i < 6; i++) t += hex[rng() % 16];
    return t;
}

// ---------- lookups ----------
static int findStudent(const string& id) {
    for (size_t i = 0; i < students.size(); i++) if (lower(students[i].id) == lower(id)) return (int)i;
    return -1;
}
static int findCourse(const string& code) {
    for (size_t i = 0; i < courses.size(); i++) if (lower(courses[i].code) == lower(code)) return (int)i;
    return -1;
}
static int findSession(int id) {
    for (size_t i = 0; i < sessions.size(); i++) if (sessions[i].id == id) return (int)i;
    return -1;
}
// Is this student present in this session? Optionally returns the timestamp.
static bool isPresent(int sessionId, const string& studentId, string* ts = nullptr) {
    for (auto& r : records)
        if (r.sessionId == sessionId && lower(r.studentId) == lower(studentId)) {
            if (ts) *ts = r.timestamp;
            return true;
        }
    return false;
}
static int presentCount(int sessionId) {
    int c = 0;
    for (auto& s : students) if (isPresent(sessionId, s.id)) c++;
    return c;
}

// ---------- file storage ----------
static void loadAll() {
    string line;
    { ifstream f("students.csv");
      while (getline(f, line)) { auto p = split(line, ','); if (p.size() >= 2) students.push_back({p[0], p[1]}); } }
    { ifstream f("courses.csv");
      while (getline(f, line)) { auto p = split(line, ','); if (p.size() >= 2) courses.push_back({p[0], p[1]}); } }
    { ifstream f("sessions.csv");
      while (getline(f, line)) {
          auto p = split(line, ',');
          if (p.size() >= 7) {
              try { sessions.push_back({stoi(p[0]), p[1], p[2], p[3], p[4], stoll(p[5]), stoi(p[6])}); }
              catch (...) { /* skip a damaged line */ }
          }
      } }
    { ifstream f("attendance.csv");
      while (getline(f, line)) {
          auto p = split(line, ',');
          if (p.size() >= 3) {
              try { records.push_back({stoi(p[0]), p[1], p[2]}); }
              catch (...) { /* skip a damaged line */ }
          }
      } }
}
static void saveStudents() { ofstream f("students.csv"); for (auto& s : students) f << s.id << "," << s.name << "\n"; }
static void saveCourses()  { ofstream f("courses.csv");  for (auto& c : courses)  f << c.code << "," << c.name << "\n"; }
static void saveSessions() {
    ofstream f("sessions.csv");
    for (auto& s : sessions)
        f << s.id << "," << s.course << "," << s.date << "," << s.time << "," << s.token << ","
          << s.created << "," << s.validMin << "\n";
}
static void saveRecords() {
    ofstream f("attendance.csv");
    for (auto& r : records) f << r.sessionId << "," << r.studentId << "," << r.timestamp << "\n";
}

// ---------- QR for a session ----------
// The QR contains only: ATT|<sessionId>|<token>
// It identifies the session; the student's identity comes from the ID they enter.
static string sessionPayload(const Session& s) { return "ATT|" + to_string(s.id) + "|" + s.token; }

static void showSessionQr(const Session& s) {
    string payload = sessionPayload(s);
    try {
        auto m = qr::encode(payload);
        cout << "\nQR for session " << s.id << " (" << s.course << ", " << s.date << " " << s.time << ")\n";
        qr::printConsole(m);
        string file = "session_" + to_string(s.id) + "_qr.svg";
        if (qr::saveSvg(m, file)) cout << "QR image saved as: " << file << " (open it in a browser)\n";
        cout << "QR text: " << payload << "\n";
    } catch (const exception& e) {
        cout << "QR error: " << e.what() << "\n";
    }
}

// ---------- student side: submit attendance ----------
static string markAttendance(const string& payload, const string& studentId) {
    auto p = split(payload, '|');
    if (p.size() != 3 || p[0] != "ATT") return "Invalid QR code.";
    int sid;
    try { sid = stoi(p[1]); } catch (...) { return "Invalid QR code."; }
    int si = findSession(sid);
    if (si < 0) return "Session not found.";
    const Session& s = sessions[si];
    if (s.token != p[2]) return "Invalid QR token.";
    if (s.validMin > 0 && time(nullptr) > s.created + (long long)s.validMin * 60)
        return "This QR code has expired.";
    int st = findStudent(studentId);
    if (st < 0) return "Student ID not found in the student list.";
    if (isPresent(sid, students[st].id)) return "Attendance already recorded for this session.";
    records.push_back({sid, students[st].id, nowString()});
    saveRecords();
    return "Attendance recorded for " + students[st].name + " (" + students[st].id + ").";
}

// ---------- admin actions ----------
static void addStudent() {
    string id = readLine("Student ID / roll number: ");
    string name = readLine("Student name: ");
    if (id.empty() || name.empty()) { cout << "ID and name are required.\n"; return; }
    if (findStudent(id) >= 0) { cout << "This student ID already exists.\n"; return; }
    students.push_back({id, name});
    saveStudents();
    cout << "Student added.\n";
}
static void listStudents() {
    if (students.empty()) { cout << "No students yet.\n"; return; }
    cout << left << setw(15) << "ID" << "Name\n";
    for (auto& s : students) cout << left << setw(15) << s.id << s.name << "\n";
}
static void addCourse() {
    string code = readLine("Course code (e.g. EF101): ");
    string name = readLine("Course name: ");
    if (code.empty() || name.empty()) { cout << "Code and name are required.\n"; return; }
    if (findCourse(code) >= 0) { cout << "This course already exists.\n"; return; }
    courses.push_back({code, name});
    saveCourses();
    cout << "Course added.\n";
}
static void listCourses() {
    if (courses.empty()) { cout << "No courses yet.\n"; return; }
    cout << left << setw(12) << "Code" << "Name\n";
    for (auto& c : courses) cout << left << setw(12) << c.code << c.name << "\n";
}
static void createSession() {
    if (courses.empty()) { cout << "Add a course first.\n"; return; }
    listCourses();
    string cc = readLine("Course code: ");
    int ci = findCourse(cc);
    if (ci < 0) { cout << "Course not found.\n"; return; }
    string date = readLine("Date (YYYY-MM-DD): ");
    if (!validDate(date)) { cout << "Invalid date format.\n"; return; }
    string tm = readLine("Class time (HH:MM): ");
    if (!validTime(tm)) { cout << "Invalid time format.\n"; return; }
    int valid = readInt("QR valid for how many minutes? (0 = no limit): ");
    if (valid < 0) valid = 0;

    int nextId = 1;
    for (auto& s : sessions) nextId = max(nextId, s.id + 1);
    Session s{nextId, courses[ci].code, date, tm, randomToken(), (long long)time(nullptr), valid};
    sessions.push_back(s);
    saveSessions();
    cout << "Session " << s.id << " created.\n";
    showSessionQr(s);
}
static void showQrAgain() {
    int id = readInt("Session ID: ");
    int si = findSession(id);
    if (si < 0) { cout << "Session not found.\n"; return; }
    showSessionQr(sessions[si]);
}

// Sessions matching optional date and course filters (blank = no filter)
static vector<int> filteredSessions(const string& date, const string& course) {
    vector<int> out;
    for (size_t i = 0; i < sessions.size(); i++) {
        if (!date.empty() && sessions[i].date != date) continue;
        if (!course.empty() && lower(sessions[i].course) != lower(course)) continue;
        out.push_back((int)i);
    }
    return out;
}

// Session list with class-level statistics
static void listSessionsWithStats() {
    string date = readLine("Filter by date YYYY-MM-DD (blank = all): ");
    string course = readLine("Filter by course code (blank = all): ");
    auto idx = filteredSessions(date, course);
    if (idx.empty()) { cout << "No matching sessions.\n"; return; }
    cout << left << setw(5) << "ID" << setw(10) << "Course" << setw(12) << "Date" << setw(7) << "Time"
         << setw(9) << "Present" << setw(8) << "Absent" << "Attendance%\n";
    for (int i : idx) {
        const Session& s = sessions[i];
        int total = (int)students.size(), pres = presentCount(s.id);
        double pct = total ? 100.0 * pres / total : 0.0;
        cout << left << setw(5) << s.id << setw(10) << s.course << setw(12) << s.date << setw(7) << s.time
             << setw(9) << pres << setw(8) << (total - pres) << fixed << setprecision(1) << pct << "%\n";
    }
}

// Present/Absent status of every student for one session
static void showSessionStatus() {
    int id = readInt("Session ID: ");
    int si = findSession(id);
    if (si < 0) { cout << "Session not found.\n"; return; }
    const Session& s = sessions[si];
    cout << "\nSession " << s.id << " - " << s.course << " - " << s.date << " " << s.time << "\n";
    cout << left << setw(15) << "Student ID" << setw(25) << "Name" << setw(10) << "Status" << "Time\n";
    int pres = 0;
    for (auto& st : students) {
        string ts;
        bool p = isPresent(s.id, st.id, &ts);
        if (p) pres++;
        cout << left << setw(15) << st.id << setw(25) << st.name << setw(10) << (p ? "Present" : "Absent") << ts << "\n";
    }
    cout << "Present: " << pres << "  Absent: " << (int)students.size() - pres << "\n";
}

// Attendance percentage of each student across sessions (optionally one course)
static void showStudentPercentages() {
    string course = readLine("Course code (blank = all courses): ");
    auto idx = filteredSessions("", course);
    if (idx.empty()) { cout << "No sessions found.\n"; return; }
    cout << "Total sessions counted: " << idx.size() << "\n";
    cout << left << setw(15) << "Student ID" << setw(25) << "Name" << setw(10) << "Attended" << "Percentage\n";
    for (auto& st : students) {
        int att = 0;
        for (int i : idx) if (isPresent(sessions[i].id, st.id)) att++;
        double pct = 100.0 * att / idx.size();
        cout << left << setw(15) << st.id << setw(25) << st.name << setw(10) << att
             << fixed << setprecision(1) << pct << "%\n";
    }
}

// Export a full report (every student x every matching session) to CSV
static void exportCsv() {
    string date = readLine("Filter by date YYYY-MM-DD (blank = all): ");
    string course = readLine("Filter by course code (blank = all): ");
    auto idx = filteredSessions(date, course);
    if (idx.empty()) { cout << "Nothing to export.\n"; return; }
    ofstream out("attendance_export.csv");
    out << "SessionID,Course,Date,Time,StudentID,Name,Status,Timestamp\n";
    for (int i : idx) {
        const Session& s = sessions[i];
        for (auto& st : students) {
            string ts;
            bool p = isPresent(s.id, st.id, &ts);
            out << s.id << "," << s.course << "," << s.date << "," << s.time << ","
                << st.id << "," << st.name << "," << (p ? "Present" : "Absent") << "," << ts << "\n";
        }
    }
    cout << "Exported to attendance_export.csv (opens in Excel).\n";
}

// ---------- menus ----------
static void adminMenu() {
    while (true) {
        cout << "\n===== ADMIN MENU =====\n"
             << " 1. Add student\n"
             << " 2. List students\n"
             << " 3. Add course\n"
             << " 4. List courses\n"
             << " 5. Create session + generate QR\n"
             << " 6. Show QR of a session again\n"
             << " 7. Sessions list + class statistics (date/course filter)\n"
             << " 8. Attendance status of a session\n"
             << " 9. Student attendance percentage\n"
             << "10. Export to CSV\n"
             << " 0. Logout\n";
        switch (readInt("Choice: ")) {
            case 1: addStudent(); break;
            case 2: listStudents(); break;
            case 3: addCourse(); break;
            case 4: listCourses(); break;
            case 5: createSession(); break;
            case 6: showQrAgain(); break;
            case 7: listSessionsWithStats(); break;
            case 8: showSessionStatus(); break;
            case 9: showStudentPercentages(); break;
            case 10: exportCsv(); break;
            case 0: return;
            default: cout << "Invalid choice.\n";
        }
    }
}

static void studentScan() {
    cout << "\nScan the QR with your phone camera. It contains text like: ATT|1|a3f9c2\n";
    string payload = readLine("Scanned QR text: ");
    string id = readLine("Your student ID / roll number: ");
    cout << markAttendance(payload, id) << "\n";
}

// ---------- default class data (used only when students.csv / courses.csv are empty) ----------
static void seedDefaults() {
    static const char* LIST[][2] = {
        {"EE-154","Javeria Rukhsar"},        {"EE-155","Rehab Kaleem"},
        {"EE-156","Ayesha Faisal Farooqui"}, {"EE-157","Hafsa Hasan Siddiqui"},
        {"EE-158","Rameen Zaheer"},          {"EE-159","Amna Naqvi"},
        {"EE-160","Hifza Nadeem"},           {"EE-161","Syeda Ashal Tanveer"},
        {"EE-162","Afia Anshra"},            {"EE-163","Manahil Tanzeem"},
        {"EE-164","Yumna Hamid"},            {"EE-165","Tania Anwer"},
        {"EE-166","Maryum Humayun"},         {"EE-167","Ramiya Aslam"},
        {"EE-168","Sarim Ameen"},            {"EE-169","Abdul Hadi Khan"},
        {"EE-170","Muhammad Ali Alam"},      {"EE-171","Muhammad Anas"},
        {"EE-172","Muhammad Azhan Mirza"},   {"EE-174","Mohammad Rohaan"},
        {"EE-176","Syed Bilal Jaffery"},     {"EE-177","Muhammad Bilal"},
        {"EE-178","Abdul Moiz"},             {"EE-179","Aqib Jabri"},
        {"EE-180","M. Waeez Kamali"},        {"EE-181","Muhammad Ahmed Alam"},
        {"EE-182","Muhammad Zeeshan Adnan"}, {"EE-183","Muhammad Shaheer"},
        {"EE-184","Ramish Jamil"},           {"EE-185","Muhammad Ziauddin"},
        {"EE-186","Shahmeer Ansari"},        {"EE-187","Muhammad Shees"},
        {"EE-188","Muhammad Arsalan Aslam"}, {"EE-189","Maaz Ahmed"},
        {"EE-190","Muhammad Shayan Zamurad"},{"EE-191","Ali Khan"},
        {"EE-192","Tanveer Ahmed"},          {"EE-193","Hassan Jameel"},
        {"EE-194","Minhal Moosa"},           {"EE-195","Syed Hussain Ali"},
        {"EE-196","Adeen Iqram"},            {"EE-197","Syed Muhammad Rayyan"},
        {"EE-198","Tanzeel Ansar"},          {"EE-199","Jasim Ahmed Khan"}};
    if (students.empty()) {
        for (auto& e : LIST) students.push_back({e[0], e[1]});
        saveStudents();
    }
    if (courses.empty()) {
        courses.push_back({"ITFA", "ITFA"});
        saveCourses();
    }
}

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(65001);   // UTF-8 output so the QR blocks display correctly
#endif
    loadAll();
    seedDefaults();
    while (true) {
        cout << "\n===== QR ATTENDANCE SYSTEM =====\n"
             << "1. Admin / Instructor login\n"
             << "2. Student: submit attendance\n"
             << "0. Exit\n";
        switch (readInt("Choice: ")) {
            case 1:
                if (readLine("Password: ") == ADMIN_PASSWORD) adminMenu();
                else cout << "Wrong password.\n";
                break;
            case 2: studentScan(); break;
            case 0: cout << "Goodbye.\n"; return 0;
            default: cout << "Invalid choice.\n";
        }
    }
}
