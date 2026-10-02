// ParamsPrinter.hpp
// Dependency-free CLI output helpers (C++17, standard library plus the OS
// tty-detection header). Follows the Heroku CLI style guide
// (https://devcenter.heroku.com/articles/cli-style-guide):
//
//   - human first: no borders or rules, "=== Heading" group headings,
//     "Label: value unit" rows, and tables with a header row and an
//     underline row (the `heroku regions` layout)
//   - grep-parseable: every row is self-contained, one line per record
//   - machine-readable on request: Format::Terse (tab separated, no header,
//     no colour, full precision) and Format::Json (one document)
//   - colour is sparse (bold headings, dim units and underline, one noun
//     colour), off for --no-color (ColorMode::Never), COLOR=false, NO_COLOR,
//     TERM=dumb, or when the stream is not a tty
//   - data goes to the stream you pass (stdout); warning() and error() go to
//     stderr
//
// Layers:
//   Style    : every setting (format, colour, indent, label width, precision,
//              symbols, SGR colour codes)
//   Table    : "=== Section" groups of label/value/unit rows
//   Grid     : columnar table (header, underline, rows), column selection
//   adapters : templates for TransmissionLine.hpp and Conductors.hpp types.
//              Duck typed on member names; this header includes neither.
//
// Quick start:
//   using namespace LineTool;
//   print::Style st;                       // st.format = print::Format::Json for --json
//   print::Table t(st);
//   t.section("Inputs").row("Voltage", 220.0, 1, "kV").key("voltage");
//   t.print(std::cout);
//
//   print::printLine(std::cout, geom, results, st);
//   print::printDualLine(std::cout, dualGeom, dualResults, st);
//   print::printConductor(std::cout, conductor, st);
//   print::printConductorList(std::cout, findResult, st);
//
// Machine output keys are stable identifiers ("section.key"): do not rename
// them once released, adding rows is safe. Set explicit keys with .key().
//
// ASCII only source (MSVC safe). On Windows the terminal must have virtual
// terminal processing enabled for colour to render.
#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
  #include <io.h>
#else
  #include <unistd.h>
#endif

namespace LineTool {
namespace print {

enum class Align     { Left, Right, Center };
enum class ColorMode { Auto, Always, Never };
enum class Format    { Human, Terse, Json };

// Symbols used in units, labels and marks. Machine formats always use ASCII.
struct Symbols {
    std::string ohm, micro, deg, angle, beta, lambda, sq, hrule, chevron;

    static Symbols unicode()
    {
        Symbols s;
        s.ohm     = "\xCE\xA9";          // U+03A9
        s.micro   = "\xC2\xB5";          // U+00B5
        s.deg     = "\xC2\xB0";          // U+00B0
        s.angle   = "\xE2\x88\xA0";      // U+2220
        s.beta    = "\xCE\xB2";          // U+03B2
        s.lambda  = "\xCE\xBB";          // U+03BB
        s.sq      = "\xC2\xB2";          // U+00B2
        s.hrule   = "\xE2\x94\x80";      // U+2500 (table underline)
        s.chevron = "\xE2\x80\xBA";      // U+203A (warning / error prefix)
        return s;
    }
    static Symbols ascii()
    {
        Symbols s;
        s.ohm = "ohm"; s.micro = "u"; s.deg = "deg"; s.angle = "<";
        s.beta = "beta"; s.lambda = "lambda"; s.sq = "2"; s.hrule = "-"; s.chevron = ">";
        return s;
    }
};

struct Style {
    Format    format = Format::Human;
    ColorMode color  = ColorMode::Auto;   // Never = --no-color

    // Layout (Human)
    int         indent      = 0;
    int         labelWidth  = 0;          // 0 = widest label in the table
    std::string labelSuffix = ":";
    int         unitGap     = 1;          // spaces between value and unit
    int         valueWidth  = 0;          // >0 pads values (see valueAlign)
    Align       valueAlign  = Align::Left;
    bool        header      = true;       // Grid header and underline (--no-header)
    std::string headingMark = "===";

    // Numbers
    int         extraDigits = 0;          // added to every display precision
    std::string missing     = "-";        // Grid cell for absent data
    std::string nonFinite   = "n/a";

    // Colour: SGR parameter strings, "" disables. Keep it to a couple of
    // colours; yellow and red are reserved for warnings and errors.
    std::string headingSgr = "1";         // bold
    std::string unitSgr    = "2";         // dim
    std::string ruleSgr    = "2";         // dim (Grid underline)
    std::string nounSgr    = "35";        // magenta (conductor codenames)
    std::string warnSgr    = "33";
    std::string errorSgr   = "31";

    Symbols sym = Symbols::unicode();     // used when format == Human

    static Style ascii()
    {
        Style s;
        s.sym = Symbols::ascii();
        return s;
    }

    const Symbols& symbols() const
    {
        static const Symbols a = Symbols::ascii();
        return format == Format::Human ? sym : a;
    }
};

namespace detail {

inline int textWidth(const std::string& s)   // UTF-8 code points
{
    int n = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) ++n;
    return n;
}

inline std::string spaces(int n) { return std::string(n > 0 ? n : 0, ' '); }

inline std::string repeat(const std::string& s, int n)
{
    std::string o;
    for (int i = 0; i < n; ++i) o += s;
    return o;
}

inline std::string rtrim(std::string s)
{
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

inline std::string pad(const std::string& s, int w, Align a)
{
    const int gap = w - textWidth(s);
    if (gap <= 0) return s;
    switch (a) {
        case Align::Right:  return spaces(gap) + s;
        case Align::Center: return spaces(gap / 2) + s + spaces(gap - gap / 2);
        default:            return s + spaces(gap);
    }
}

// Truncate to w code points, ending in "..." when it fits.
inline std::string truncate(const std::string& s, int w)
{
    if (w <= 0 || textWidth(s) <= w) return s;
    const int keep = w > 3 ? w - 3 : w;
    std::string out;
    int n = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if ((c & 0xC0) != 0x80) {
            if (n == keep) break;
            ++n;
        }
        out += s[i];
    }
    return w > 3 ? out + "..." : out;
}

inline bool iequals(const std::string& a, const std::string& b)
{
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
               return std::tolower(x) == std::tolower(y);
           });
}

// "SIL (per circuit)" -> "sil_per_circuit"
inline std::string slug(const std::string& s)
{
    std::string o;
    bool sep = false;
    for (unsigned char c : s) {
        if (c < 128 && std::isalnum(c)) {
            if (sep && !o.empty()) o += '_';
            sep = false;
            o += static_cast<char>(std::tolower(c));
        } else {
            sep = true;
        }
    }
    return o;
}

inline std::string num(double v, int prec, char conv, bool sign, const std::string& nonFinite)
{
    if (!std::isfinite(v)) return nonFinite;
    std::string f = "%";
    if (sign) f += '+';
    f += ".*";
    f += conv;
    char buf[128];
    std::snprintf(buf, sizeof buf, f.c_str(), prec, v);
    return buf;
}

// Full precision for machine output.
inline std::string machineNum(double v) { return num(v, 10, 'g', false, ""); }

inline bool isNumberText(const std::string& s)
{
    if (s.empty()) return false;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    return end == s.c_str() + s.size() && std::isfinite(v);
}

inline std::string jsonEscape(const std::string& s)
{
    std::string o;
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += static_cast<char>(c);
        }
    }
    return o;
}

inline std::string jsonNumber(double v)
{
    return std::isfinite(v) ? machineNum(v) : std::string("null");
}

inline std::string tsvClean(std::string s)
{
    for (char& c : s) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return s;
}

// ---- colour ---------------------------------------------------------------
inline bool fdIsTty(bool err)
{
#if defined(_WIN32)
    return _isatty(_fileno(err ? stderr : stdout)) != 0;
#else
    return isatty(fileno(err ? stderr : stdout)) != 0;
#endif
}

inline const char* envVar(const char* name)
{
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    return std::getenv(name);
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}

// Off for machine formats, ColorMode::Never, COLOR=false|0, NO_COLOR,
// TERM=dumb, and any stream that is not a tty (only cout and cerr/clog are
// ever treated as ttys).
inline bool colorEnabled(const Style& st, const std::ostream& os)
{
    if (st.format != Format::Human) return false;
    if (st.color == ColorMode::Never)  return false;
    if (st.color == ColorMode::Always) return true;
    if (const char* c = envVar("COLOR"))
        if (!std::strcmp(c, "false") || !std::strcmp(c, "0")) return false;
    if (const char* n = envVar("NO_COLOR"))
        if (*n) return false;
    if (const char* t = envVar("TERM"))
        if (!std::strcmp(t, "dumb")) return false;
    if (&os == &std::cout) return fdIsTty(false);
    if (&os == &std::cerr || &os == &std::clog) return fdIsTty(true);
    return false;
}

inline std::string paint(const std::string& text, const std::string& sgr, bool on)
{
    if (!on || sgr.empty() || text.empty()) return text;
    return "\x1b[" + sgr + "m" + text + "\x1b[0m";
}

// Pad to width, colouring the text but not the padding.
inline std::string padPaint(const std::string& text, int w, Align a,
                            const std::string& sgr, bool on)
{
    const int gap = w - textWidth(text);
    int l = 0, r = 0;
    if (gap > 0) {
        if (a == Align::Right)       l = gap;
        else if (a == Align::Center) { l = gap / 2; r = gap - l; }
        else                         r = gap;
    }
    return spaces(l) + paint(text, sgr, on) + spaces(r);
}

} // namespace detail

// ---- Table ------------------------------------------------------------------
// "=== Heading" groups of "Label: value unit" rows.
class Table {
public:
    explicit Table(Style s = Style()) : st_(std::move(s)) {}

    const Style& style() const { return st_; }
    bool empty() const { return items_.empty(); }
    void clear() { items_.clear(); section_.clear(); }

    // Heading shown in Human output only (machine output has no title).
    Table& title(const std::string& t)
    {
        Item it; it.kind = Kind::Heading; it.text = t;
        items_.push_back(it);
        return *this;
    }
    // Heading that also starts a section: rows below belong to it in
    // Terse ("section.key") and Json (top-level object) output.
    // The machine key defaults to the slug of the name.
    Table& section(const std::string& name, const std::string& machineKey = "")
    {
        Item it; it.kind = Kind::Heading; it.text = name;
        items_.push_back(it);
        section_ = machineKey.empty() ? detail::slug(name) : machineKey;
        return *this;
    }
    Table& blank() { Item it; it.kind = Kind::Blank; items_.push_back(it); return *this; }
    Table& text(const std::string& t)
    {
        Item it; it.kind = Kind::Text; it.text = t;
        items_.push_back(it);
        return *this;
    }

    // String value.
    Table& row(const std::string& label, const std::string& value, const std::string& unit = "")
    {
        Item it = rowBase(label, unit);
        it.text = value; it.str = value; it.isStr = true;
        items_.push_back(it);
        return *this;
    }
    Table& row(const std::string& label, const char* value, const std::string& unit = "")
    {
        return row(label, std::string(value), unit);
    }
    // Fixed-point number, shown to `prec` decimals (+ Style::extraDigits).
    Table& row(const std::string& label, double v, int prec = 4, const std::string& unit = "")
    {
        Item it = rowBase(label, unit);
        it.text = detail::num(v, adj(prec), 'f', false, st_.nonFinite);
        it.num = v;
        items_.push_back(it);
        return *this;
    }
    // Skipped entirely when empty.
    Table& row(const std::string& label, const std::optional<double>& v, int prec = 4,
               const std::string& unit = "")
    {
        if (v) row(label, *v, prec, unit);
        return *this;
    }
    Table& rowSci(const std::string& label, double v, int prec = 6, const std::string& unit = "")
    {
        Item it = rowBase(label, unit);
        it.text = detail::num(v, adj(prec), 'e', false, st_.nonFinite);
        it.num = v;
        items_.push_back(it);
        return *this;
    }
    Table& rowInt(const std::string& label, long long v, const std::string& unit = "")
    {
        Item it = rowBase(label, unit);
        it.text = std::to_string(v);
        it.num = static_cast<double>(v);
        items_.push_back(it);
        return *this;
    }
    // Human text differs from the machine value (e.g. "auto" radius).
    Table& rowAlt(const std::string& label, const std::string& humanText,
                  double machineValue, const std::string& unit = "")
    {
        Item it = rowBase(label, unit);
        it.text = humanText;
        it.num = machineValue;
        items_.push_back(it);
        return *this;
    }
    // "0.9944 <angle> +1.23<deg>"; machine keys KEY_mag and KEY_ang (deg).
    Table& rowPolar(const std::string& label, double mag, double angDeg,
                    int magPrec = 4, int angPrec = 2)
    {
        const Symbols& y = st_.symbols();
        Item it = rowBase(label, "");
        it.text = detail::num(mag, adj(magPrec), 'f', false, st_.nonFinite) + " " + y.angle
                  + " " + detail::num(angDeg, adj(angPrec), 'f', true, st_.nonFinite) + y.deg;
        it.subs = { {"_mag", mag, ""}, {"_ang", angDeg, "deg"} };
        items_.push_back(it);
        return *this;
    }
    // "(x, y) unit"; machine keys KEY_x and KEY_y.
    Table& rowXY(const std::string& label, double x, double y, int prec = 2,
                 const std::string& unit = "m")
    {
        Item it = rowBase(label, unit);
        it.text = "(" + detail::num(x, adj(prec), 'f', false, st_.nonFinite) + ", "
                      + detail::num(y, adj(prec), 'f', false, st_.nonFinite) + ")";
        it.subs = { {"_x", x, unit}, {"_y", y, unit} };
        items_.push_back(it);
        return *this;
    }
    // Stable machine key for the row just added (default: slug of the label).
    Table& key(const std::string& k)
    {
        if (!items_.empty()) items_.back().key = k;
        return *this;
    }

    void print(std::ostream& os) const
    {
        switch (st_.format) {
            case Format::Human: printHuman(os); break;
            case Format::Terse: printTerse(os); break;
            case Format::Json:  printJson(os);  break;
        }
    }
    std::string str() const { std::ostringstream o; print(o); return o.str(); }

private:
    enum class Kind { Heading, Row, Text, Blank };
    struct Sub  { std::string suffix; double num; std::string unit; };
    struct Item {
        Kind kind = Kind::Row;
        std::string section, key, label, text, unit, str;
        std::optional<double> num;
        bool isStr = false;
        std::vector<Sub> subs;
    };
    struct Entry {
        std::string section, key, unit, str;
        bool        isNum = false;
        double      num   = 0;
    };

    Style             st_;
    std::vector<Item> items_;
    std::string       section_;

    int adj(int p) const { return std::max(0, p + st_.extraDigits); }

    Item rowBase(const std::string& label, const std::string& unit) const
    {
        Item it;
        it.kind = Kind::Row;
        it.section = section_;
        it.key = detail::slug(label);
        it.label = label;
        it.unit = unit;
        return it;
    }

    std::vector<Entry> entries() const
    {
        std::vector<Entry> out;
        for (const Item& it : items_) {
            if (it.kind != Kind::Row) continue;
            if (!it.subs.empty()) {
                for (const Sub& s : it.subs) {
                    Entry e; e.section = it.section; e.key = it.key + s.suffix;
                    e.unit = s.unit; e.isNum = true; e.num = s.num;
                    out.push_back(e);
                }
            } else {
                Entry e; e.section = it.section; e.key = it.key; e.unit = it.unit;
                if (it.num) { e.isNum = true; e.num = *it.num; }
                else        { e.str = it.str; }
                out.push_back(e);
            }
        }
        return out;
    }

    void printHuman(std::ostream& os) const
    {
        const bool col = detail::colorEnabled(st_, os);
        const std::string ind = detail::spaces(st_.indent);
        const int suffixW = detail::textWidth(st_.labelSuffix);

        int lw = st_.labelWidth;
        if (lw <= 0)
            for (const Item& it : items_)
                if (it.kind == Kind::Row)
                    lw = std::max(lw, detail::textWidth(it.label) + suffixW);

        bool any = false, lastBlank = false;
        for (const Item& it : items_) {
            switch (it.kind) {
                case Kind::Heading:
                    if (any && !lastBlank) os << '\n';
                    os << ind << detail::paint(st_.headingMark + " " + it.text, st_.headingSgr, col) << '\n';
                    lastBlank = false;
                    break;
                case Kind::Blank:
                    os << '\n';
                    lastBlank = true;
                    continue;
                case Kind::Text:
                    os << ind << it.text << '\n';
                    lastBlank = false;
                    break;
                case Kind::Row: {
                    std::string v = it.text;
                    if (st_.valueWidth > 0) v = detail::pad(v, st_.valueWidth, st_.valueAlign);
                    std::string line = ind + detail::pad(it.label + st_.labelSuffix, lw, Align::Left)
                                       + " " + v;
                    line = detail::rtrim(line);
                    if (!it.unit.empty())
                        line += detail::spaces(st_.unitGap) + detail::paint(it.unit, st_.unitSgr, col);
                    os << line << '\n';
                    lastBlank = false;
                    break;
                }
            }
            any = true;
        }
    }

    void printTerse(std::ostream& os) const
    {
        for (const Entry& e : entries()) {
            const std::string k = e.section.empty() ? e.key : e.section + "." + e.key;
            os << detail::tsvClean(k) << '\t'
               << (e.isNum ? detail::machineNum(e.num) : detail::tsvClean(e.str)) << '\t'
               << detail::tsvClean(e.unit) << '\n';
        }
    }

    void printJson(std::ostream& os) const
    {
        const std::vector<Entry> es = entries();
        std::vector<std::string> names;
        for (const Entry& e : es) {
            const std::string n = e.section.empty() ? "general" : e.section;
            if (std::find(names.begin(), names.end(), n) == names.end()) names.push_back(n);
        }
        os << "{\n";
        for (std::size_t s = 0; s < names.size(); ++s) {
            os << "  \"" << detail::jsonEscape(names[s]) << "\": {\n";
            std::vector<const Entry*> mine;
            for (const Entry& e : es)
                if ((e.section.empty() ? std::string("general") : e.section) == names[s])
                    mine.push_back(&e);
            for (std::size_t i = 0; i < mine.size(); ++i) {
                const Entry& e = *mine[i];
                os << "    \"" << detail::jsonEscape(e.key) << "\": {\"value\": ";
                if (e.isNum) os << detail::jsonNumber(e.num);
                else         os << '"' << detail::jsonEscape(e.str) << '"';
                if (!e.unit.empty()) os << ", \"unit\": \"" << detail::jsonEscape(e.unit) << '"';
                os << '}' << (i + 1 < mine.size() ? "," : "") << '\n';
            }
            os << "  }" << (s + 1 < names.size() ? "," : "") << '\n';
        }
        os << "}\n";
    }
};

inline std::ostream& operator<<(std::ostream& os, const Table& t)
{
    t.print(os);
    return os;
}

// ---- Grid -------------------------------------------------------------------
// Header row, underline row, one line per record. No borders.
struct Column {
    std::string header;
    Align       align    = Align::Left;
    std::string key;                 // machine key (default: slug of header)
    bool        numeric  = false;    // Json: emit numeric cells as numbers
    std::string sgr;                 // cell colour (e.g. Style::nounSgr), "" = none
    int         minWidth = 0;
    int         maxWidth = 0;        // 0 = unlimited; longer cells end in "..."

    Column(std::string h, Align a = Align::Left, std::string k = "", bool num = false,
           std::string color = "", int minW = 0, int maxW = 0)
        : header(std::move(h)), align(a), key(std::move(k)), numeric(num),
          sgr(std::move(color)), minWidth(minW), maxWidth(maxW) {}
};

class Grid {
public:
    Grid(Style s, std::vector<Column> cols) : st_(std::move(s)), cols_(std::move(cols)) { resetOrder(); }
    explicit Grid(std::vector<Column> cols) : cols_(std::move(cols)) { resetOrder(); }

    // "=== name" heading above the table (Human output only).
    Grid& title(const std::string& t) { title_ = t; return *this; }
    Grid& addRow(std::vector<std::string> cells) { rows_.push_back(std::move(cells)); return *this; }
    std::size_t rows() const { return rows_.size(); }
    const Style& style() const { return st_; }

    // Show only these columns, in this order (match on key or header,
    // case-insensitive). Throws std::invalid_argument for an unknown name.
    Grid& select(const std::vector<std::string>& names)
    {
        std::vector<std::size_t> o;
        for (const std::string& n : names) {
            bool found = false;
            for (std::size_t i = 0; i < cols_.size(); ++i)
                if (detail::iequals(colKey(i), n) || detail::iequals(cols_[i].header, n)) {
                    o.push_back(i);
                    found = true;
                    break;
                }
            if (!found) throw std::invalid_argument("unknown column: " + n);
        }
        order_ = o;
        return *this;
    }

    void print(std::ostream& os) const
    {
        switch (st_.format) {
            case Format::Human: printHuman(os); break;
            case Format::Terse: printTerse(os); break;
            case Format::Json:  printJson(os);  break;
        }
    }
    std::string str() const { std::ostringstream o; print(o); return o.str(); }

private:
    Style                                 st_;
    std::vector<Column>                   cols_;
    std::vector<std::vector<std::string>> rows_;
    std::vector<std::size_t>              order_;
    std::string                           title_;

    void resetOrder()
    {
        order_.clear();
        for (std::size_t i = 0; i < cols_.size(); ++i) order_.push_back(i);
    }
    std::string colKey(std::size_t i) const
    {
        return cols_[i].key.empty() ? detail::slug(cols_[i].header) : cols_[i].key;
    }
    std::string cellAt(const std::vector<std::string>& r, std::size_t i) const
    {
        return i < r.size() ? r[i] : std::string();
    }

    void printHuman(std::ostream& os) const
    {
        if (order_.empty()) return;
        const bool col = detail::colorEnabled(st_, os);
        const std::string ind = detail::spaces(st_.indent);

        std::vector<int> w;
        for (std::size_t i : order_) {
            int x = std::max(st_.header ? detail::textWidth(cols_[i].header) : 0, cols_[i].minWidth);
            for (const auto& r : rows_) x = std::max(x, detail::textWidth(cellAt(r, i)));
            if (cols_[i].maxWidth > 0) x = std::min(x, cols_[i].maxWidth);
            w.push_back(x);
        }

        if (!title_.empty())
            os << ind << detail::paint(st_.headingMark + " " + title_, st_.headingSgr, col) << '\n';

        if (st_.header) {
            std::string h = ind, u = ind;
            for (std::size_t k = 0; k < order_.size(); ++k) {
                const Column& c = cols_[order_[k]];
                h += detail::padPaint(c.header, w[k], c.align, st_.headingSgr, col);
                u += detail::paint(detail::repeat(st_.symbols().hrule, w[k]), st_.ruleSgr, col);
                if (k + 1 < order_.size()) { h += "  "; u += "  "; }
            }
            os << detail::rtrim(h) << '\n' << u << '\n';
        }
        for (const auto& r : rows_) {
            std::string line = ind;
            for (std::size_t k = 0; k < order_.size(); ++k) {
                const Column& c = cols_[order_[k]];
                line += detail::padPaint(detail::truncate(cellAt(r, order_[k]), w[k]), w[k],
                                         c.align, c.sgr, col);
                if (k + 1 < order_.size()) line += "  ";
            }
            os << detail::rtrim(line) << '\n';
        }
    }

    void printTerse(std::ostream& os) const
    {
        for (const auto& r : rows_) {
            for (std::size_t k = 0; k < order_.size(); ++k) {
                os << detail::tsvClean(cellAt(r, order_[k]));
                if (k + 1 < order_.size()) os << '\t';
            }
            os << '\n';
        }
    }

    void printJson(std::ostream& os) const
    {
        os << "[\n";
        for (std::size_t j = 0; j < rows_.size(); ++j) {
            os << "  {";
            for (std::size_t k = 0; k < order_.size(); ++k) {
                const std::size_t i = order_[k];
                const std::string cell = cellAt(rows_[j], i);
                os << '"' << detail::jsonEscape(colKey(i)) << "\": ";
                if (cols_[i].numeric && cell.empty())          os << "null";
                else if (cols_[i].numeric && detail::isNumberText(cell)) os << cell;
                else                                           os << '"' << detail::jsonEscape(cell) << '"';
                if (k + 1 < order_.size()) os << ", ";
            }
            os << '}' << (j + 1 < rows_.size() ? "," : "") << '\n';
        }
        os << "]\n";
    }
};

inline std::ostream& operator<<(std::ostream& os, const Grid& g)
{
    g.print(os);
    return os;
}

// ---- Warnings and errors (stderr) ---------------------------------------------
inline void warning(const std::string& msg, const Style& st = Style(),
                    std::ostream& os = std::cerr)
{
    const bool col = detail::colorEnabled(st, os);
    os << detail::paint(" " + st.symbols().chevron + "   Warning: " + msg, st.warnSgr, col) << '\n';
}

inline void error(const std::string& msg, const Style& st = Style(),
                  std::ostream& os = std::cerr)
{
    const bool col = detail::colorEnabled(st, os);
    os << detail::paint(" " + st.symbols().chevron + "   Error: " + msg, st.errorSgr, col) << '\n';
}

// ---- Adapters: TransmissionLine.hpp ---------------------------------------------
// Duck typed on the members of LineResults / DualCircuitResults /
// GeometryInput / DualCircuitInput.
namespace detail {

struct Units {
    std::string ohm, ohmKm, uFkm, mm2;
    explicit Units(const Style& s)
    {
        const Symbols& y = s.symbols();
        ohm = y.ohm; ohmKm = y.ohm + "/km"; uFkm = y.micro + "F/km"; mm2 = "mm" + y.sq;
    }
};

// Same 0.1 m floor as TransmissionLine::compute().
inline double phaseDist(double xa, double ya, double xb, double yb)
{
    const double d = std::hypot(xb - xa, yb - ya);
    return d < 0.1 ? 0.1 : d;
}

template <class G>
void conductorAndOperating(Table& t, const G& g, bool dual)
{
    const Units u(t.style());
    t.section("Conductor");
    t.row("GMR, DS", g.DS, 5, "m").key("gmr");
    if (g.radius_m > 0) t.row("Outer radius", g.radius_m, 5, "m").key("outer_radius");
    else t.rowAlt("Outer radius", "auto (DS / 0.7788, solid round)",
                  g.DS / std::exp(-0.25), "m").key("outer_radius");
    t.rowInt("Sub-conductors per phase", g.bundleNo).key("bundle_conductors");
    if (g.bundleNo > 1) t.row("Bundle spacing", g.bundleSpace, 3, "m").key("bundle_spacing");
    t.row(dual ? "AC resistance (per circuit)" : "AC resistance", g.r_ac, 4, u.ohmKm).key("r_ac");

    t.section("Operating");
    t.row("Frequency", g.freq, 1, "Hz").key("frequency");
    t.row("Length", g.lengthKm, 1, "km").key("length");
    t.row("Voltage (L-L)", g.voltageKV, 1, "kV").key("voltage_ll");
    t.row(dual ? "Current (per circuit)" : "Current", g.currentA, 1, "A").key("current");
}

template <bool Dual, class R>
void commonLineRows(Table& t, const R& r)
{
    const Units u(t.style());
    const Symbols& s = t.style().symbols();

    t.section("Per-unit length");
    t.row("Inductance",  r.inductance_mH_km,  4, "mH/km").key("inductance");
    t.row("Capacitance", r.capacitance_uF_km, 5, u.uFkm).key("capacitance");
    t.row("Reactance",   r.reactance_ohm_km,  4, u.ohmKm).key("reactance");
    t.row("Susceptance", r.susceptance_S_km,  6, "S/km").key("susceptance");

    t.section("Propagation");
    t.row("Zc", r.Zc_ohm, 3, u.ohm).key("zc");
    t.row(s.beta + " (k)", r.k_rad_km, 6, "rad/km").key("beta");
    t.row("Velocity factor", r.vel_factor, 4).key("velocity_factor");

    t.section("Power system");
    if constexpr (Dual) {
        t.row("SIL (per circuit)", r.SIL_MVA, 2, "MVA").key("sil_per_circuit");
        t.row("SIL (total)", r.SIL_total_MVA, 2, "MVA").key("sil_total");
    } else {
        t.row("SIL", r.SIL_MVA, 2, "MVA").key("sil");
    }
    t.row(s.lambda + "/4 length", r.quarter_wave_km, 1, "km").key("quarter_wave_length");
    if constexpr (!Dual) t.row("Total admittance", r.admittance_S, 4, "S").key("total_admittance");
    if constexpr (Dual) {
        t.row("Charging (both circuits)", r.charging_MVAR, 2, "MVAR").key("charging_both_circuits");
        t.row("Loadability (per circuit)", r.loadability_MW, 1, "MW").key("loadability_per_circuit");
    } else {
        t.row("Charging", r.charging_MVAR, 2, "MVAR").key("charging");
        t.row("Loadability", r.loadability_MW, 1, "MW").key("loadability");
    }

    t.section("ABCD matrix");
    t.rowPolar("A", r.A_mag, r.A_ang).key("a");
    t.rowPolar("B", r.B_mag, r.B_ang).key("b");
    t.rowPolar("C", r.C_mag, r.C_ang).key("c");
    t.rowPolar("D", r.D_mag, r.D_ang).key("d");
}

} // namespace detail

// Inputs of a single circuit line (GeometryInput) plus derived phase spacings.
template <class G>
void addLineInputs(Table& t, const G& g)
{
    t.section("Phase coordinates (x, y)", "phase_coordinates");
    t.rowXY("Phase A", g.x1, g.y1).key("phase_a");
    t.rowXY("Phase B", g.x2, g.y2).key("phase_b");
    t.rowXY("Phase C", g.x3, g.y3).key("phase_c");

    detail::conductorAndOperating(t, g, false);

    const double D12 = detail::phaseDist(g.x1, g.y1, g.x2, g.y2);
    const double D23 = detail::phaseDist(g.x2, g.y2, g.x3, g.y3);
    const double D13 = detail::phaseDist(g.x1, g.y1, g.x3, g.y3);
    t.section("Geometry");
    t.row("D12", D12, 3, "m");
    t.row("D23", D23, 3, "m");
    t.row("D13", D13, 3, "m");
    t.row("GMD", std::cbrt(D12 * D23 * D13), 3, "m");
}

// Inputs of a dual circuit line (DualCircuitInput).
template <class G>
void addDualLineInputs(Table& t, const G& g)
{
    t.section("Phase coordinates (x, y)", "phase_coordinates");
    t.rowXY("Phase A1", g.x1, g.y1).key("phase_a1");
    t.rowXY("Phase B1", g.x2, g.y2).key("phase_b1");
    t.rowXY("Phase C1", g.x3, g.y3).key("phase_c1");
    t.rowXY("Phase A2", g.x4, g.y4).key("phase_a2");
    t.rowXY("Phase B2", g.x5, g.y5).key("phase_b2");
    t.rowXY("Phase C2", g.x6, g.y6).key("phase_c2");

    detail::conductorAndOperating(t, g, true);
}

template <class R>
void addLineResults(Table& t, const R& r) { detail::commonLineRows<false>(t, r); }

template <class R>
void addDualLineResults(Table& t, const R& r)
{
    t.section("Equivalent geometry");
    t.row("DS_bundle", r.DSb_m,   4, "m");
    t.row("Da1a2",     r.Da1a2_m, 3, "m");
    t.row("Db1b2",     r.Db1b2_m, 3, "m");
    t.row("Dc1c2",     r.Dc1c2_m, 3, "m");
    t.row("GMRl",      r.GMRl_m,  4, "m");
    t.row("Req",       r.Req_m,   4, "m");
    t.row("GMD",       r.GMD_m,   3, "m");
    detail::commonLineRows<true>(t, r);
}

template <class G, class R>
void printLine(std::ostream& os, const G& g, const R& r, const Style& st = Style(),
               const std::string& title = "Transmission line")
{
    Table t(st);
    if (!title.empty()) t.title(title);
    addLineInputs(t, g);
    addLineResults(t, r);
    t.print(os);
}

template <class R>
void printLineResults(std::ostream& os, const R& r, const Style& st = Style(),
                      const std::string& title = "Transmission line")
{
    Table t(st);
    if (!title.empty()) t.title(title);
    addLineResults(t, r);
    t.print(os);
}

template <class G, class R>
void printDualLine(std::ostream& os, const G& g, const R& r, const Style& st = Style(),
                   const std::string& title = "Dual-circuit transmission line")
{
    Table t(st);
    if (!title.empty()) t.title(title);
    addDualLineInputs(t, g);
    addDualLineResults(t, r);
    t.print(os);
}

template <class R>
void printDualLineResults(std::ostream& os, const R& r, const Style& st = Style(),
                          const std::string& title = "Dual-circuit transmission line")
{
    Table t(st);
    if (!title.empty()) t.title(title);
    addDualLineResults(t, r);
    t.print(os);
}

// ---- Adapters: Conductors.hpp -----------------------------------------------------
namespace detail {

template <class T> const T& deref(const T& v) { return v; }
template <class T> const T& deref(const T* v)  { return *v; }

// Display precision for people, full precision for machines.
inline std::string cellNum(const Style& st, double v, int prec)
{
    if (st.format != Format::Human) return machineNum(v);
    return num(v, std::max(0, prec + st.extraDigits), 'f', false, st.nonFinite);
}

inline std::string ratingCell(const Style& st, const std::optional<double>& v)
{
    if (!v) return st.format == Format::Human ? st.missing : std::string();
    return st.format == Format::Human ? num(*v, 0, 'f', false, st.nonFinite) : machineNum(*v);
}

} // namespace detail

// Identity, geometry, mechanical and electrical blocks of one Conductor.
template <class C>
void addConductor(Table& t, const C& c)
{
    const detail::Units u(t.style());

    t.section("Identity");
    t.row("Type", c.type);
    t.row("Codename", c.codename);
    t.rowInt("Id", c.id);
    t.row("Source", c.source_file);

    t.section("Geometry");
    t.row("Stranding", c.stranding);
    t.row("Overall diameter", c.overall_diameter_mm, 2, "mm").key("overall_diameter");
    t.row("Area", c.area_mm2, 1, u.mm2);
    t.row("Outer radius", c.radius_m(), 5, "m");
    t.row("GMR (estimated)", c.gmr_m(), 5, "m").key("gmr");
    t.row("GMR / radius", c.gmrRatio(), 4).key("gmr_ratio");

    t.section("Mechanical");
    t.row("Mass", c.mass_kg_km, 1, "kg/km");
    t.row("Breaking load", c.breaking_load_kN, 2, "kN");
    t.row("Modulus", c.modulus_GPa, 1, "GPa");
    t.row("Expansion", c.expansion_1e6, 1, "x10^-6 /degC");

    t.section("Electrical");
    t.row("DC resistance (20 degC)", c.dc_resistance_20C, 4, u.ohmKm).key("dc_resistance_20c");
    t.row("DC resistance (75 degC)", c.dc_resistance_75C, 4, u.ohmKm).key("dc_resistance_75c");
    t.row("AC resistance (50 Hz)", c.ac_resistance_50Hz, 4, u.ohmKm).key("ac_resistance_50hz");
    t.row("Reactance (50 Hz)", c.reactance_50Hz, 4, u.ohmKm).key("reactance_50hz");
}

// Current ratings as key/value rows (used for Terse and Json output).
template <class C>
void addRatings(Table& t, const C& c)
{
    t.section("Current rating");
    auto add = [&](const char* prefix, const auto& cond) {
        const std::string p = prefix;
        t.row(p + " still",    cond.still,    0, "A").key(detail::slug(p) + "_still");
        t.row(p + " 1 m/s",    cond.wind_1ms, 0, "A").key(detail::slug(p) + "_wind_1ms");
        t.row(p + " 2 m/s",    cond.wind_2ms, 0, "A").key(detail::slug(p) + "_wind_2ms");
    };
    add("Rural winter",      c.rating.rural.winter);
    add("Rural summer",      c.rating.rural.summer);
    add("Industrial winter", c.rating.industrial.winter);
    add("Industrial summer", c.rating.industrial.summer);
}

// Current rating matrix (environment x season, by wind speed).
template <class C>
Grid ratingsGrid(const C& c, const Style& st = Style())
{
    Grid g(st, {Column{"Condition", Align::Left, "condition"},
                Column{"Still (A)", Align::Right, "still", true},
                Column{"1 m/s (A)", Align::Right, "wind_1ms", true},
                Column{"2 m/s (A)", Align::Right, "wind_2ms", true}});
    g.title("Current rating");
    auto add = [&](const char* name, const auto& cond) {
        g.addRow({name, detail::ratingCell(st, cond.still),
                  detail::ratingCell(st, cond.wind_1ms),
                  detail::ratingCell(st, cond.wind_2ms)});
    };
    add("Rural, winter",      c.rating.rural.winter);
    add("Rural, summer",      c.rating.rural.summer);
    add("Industrial, winter", c.rating.industrial.winter);
    add("Industrial, summer", c.rating.industrial.summer);
    return g;
}

// Accepts std::vector<Conductor> or std::vector<const Conductor*>.
template <class Vec>
Grid conductorGrid(const Vec& list, const Style& st = Style())
{
    const detail::Units u(st);
    Grid g(st, {Column{"Type", Align::Left, "type"},
                Column{"Codename", Align::Left, "codename", false, st.nounSgr},
                Column{"Stranding", Align::Left, "stranding"},
                Column{"Dia (mm)", Align::Right, "diameter_mm", true},
                Column{"Area (" + u.mm2 + ")", Align::Right, "area_mm2", true},
                Column{"Rac (" + u.ohmKm + ")", Align::Right, "r_ac_ohm_km", true},
                Column{"X (" + u.ohmKm + ")", Align::Right, "x_ohm_km", true},
                Column{"Mass (kg/km)", Align::Right, "mass_kg_km", true}});
    for (const auto& item : list) {
        const auto& c = detail::deref(item);
        g.addRow({c.type, c.codename, c.stranding,
                  detail::cellNum(st, c.overall_diameter_mm, 1),
                  detail::cellNum(st, c.area_mm2, 1),
                  detail::cellNum(st, c.ac_resistance_50Hz, 4),
                  detail::cellNum(st, c.reactance_50Hz, 3),
                  detail::cellNum(st, c.mass_kg_km, 0)});
    }
    return g;
}

// Human output: sections, then the rating matrix as a table.
// Terse and Json: a single document with the ratings as rows.
template <class C>
void printConductor(std::ostream& os, const C& c, const Style& st = Style(),
                    bool withRatings = true)
{
    Table t(st);
    t.title(c.type + " " + c.codename);
    addConductor(t, c);
    if (st.format != Format::Human) {
        if (withRatings) addRatings(t, c);
        t.print(os);
        return;
    }
    t.print(os);
    if (withRatings) {
        os << '\n';
        ratingsGrid(c, st).print(os);
    }
}

template <class Vec>
void printConductorList(std::ostream& os, const Vec& list, const Style& st = Style())
{
    conductorGrid(list, st).print(os);
}

// Rating table problems (Conductors.hpp ratingIssues()) as warnings on stderr.
template <class C>
void warnRatingIssues(const C& c, const Style& st = Style(), std::ostream& os = std::cerr)
{
    for (const auto& issue : c.ratingIssues())
        warning(c.type + " " + c.codename + ": " + issue, st, os);
}

} // namespace print
} // namespace LineTool
