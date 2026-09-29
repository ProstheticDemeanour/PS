// ParamsPrinter.hpp
// Dependency-free console table printer (C++17, standard library only).
//
// Layers:
//   Style   : every knob (widths, indent, borders, symbols, precision, colour)
//   Table   : sections of "label  value  unit" rows (the Python paramsPrinter look)
//   Grid    : columnar table with a header row (lists, matrices)
//   adapters: templates that print TransmissionLine.hpp and Conductors.hpp
//             types by member name (duck typing). This header does NOT include
//             either of them, so it compiles alone and works with any struct
//             that has the same members.
//
// Quick start:
//   using namespace LineTool;
//   print::Table t;
//   t.title("MY TOOL").section("INPUTS")
//    .row("Voltage", 220.0, 1, "kV").row("Name", "Line 1");
//   std::cout << t;
//
//   print::printLine(std::cout, geom, results);              // GeometryInput + LineResults
//   print::printDualLine(std::cout, dualGeom, dualResults);
//   print::printConductor(std::cout, conductor);
//   print::printConductorList(std::cout, findResult);        // vector<Conductor> or vector<const Conductor*>
//
// Compose your own output from the add* functions:
//   print::Table t(style);
//   print::addLineInputs(t, g);
//   print::addLineResults(t, r);
//   t.section("NOTES").text("free text line");
//   t.print(std::cout);
//
// Source is ASCII only (MSVC safe). Unicode symbols are emitted as UTF-8 byte
// sequences and can be swapped for plain ASCII with Style::ascii().
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace LineTool {
namespace print {

enum class Align  { Left, Right, Center };
enum class Border { Plain, Ascii, Unicode };   // Plain = horizontal rules only

// Symbols used in units and labels.
struct Symbols {
    std::string ohm, micro, deg, angle, beta, lambda, sq;

    static Symbols unicode()
    {
        Symbols s;
        s.ohm    = "\xCE\xA9";        // U+03A9
        s.micro  = "\xC2\xB5";        // U+00B5
        s.deg    = "\xC2\xB0";        // U+00B0
        s.angle  = "\xE2\x88\xA0";    // U+2220
        s.beta   = "\xCE\xB2";        // U+03B2
        s.lambda = "\xCE\xBB";        // U+03BB
        s.sq     = "\xC2\xB2";        // U+00B2
        return s;
    }
    static Symbols ascii()
    {
        Symbols s;
        s.ohm = "ohm"; s.micro = "u"; s.deg = "deg"; s.angle = "<";
        s.beta = "beta"; s.lambda = "lambda"; s.sq = "2";
        return s;
    }
};

struct Style {
    int   width      = 52;          // minimum outer width in columns
    int   indent     = 2;           // left indent of content
    int   labelWidth = 32;          // label column width (Table)
    int   unitGap    = 2;           // spaces between value and unit
    int   valueWidth = 0;           // >0 pads values to this width (see valueAlign)
    Align valueAlign = Align::Left; // only effective when valueWidth > 0
    int   extraDigits = 0;          // added to every fixed/scientific precision
    Border border    = Border::Plain;
    char  rule       = '-';         // rule character for Border::Plain
    bool  color      = false;       // ANSI bold for titles and section names
    std::string missing   = "-";    // Grid cell for absent data (adapters)
    std::string nonFinite = "n/a";  // text for NaN / inf
    Symbols sym = Symbols::unicode();

    static Style ascii()
    {
        Style s;
        s.sym = Symbols::ascii();
        return s;
    }
};

namespace detail {

// Display width: UTF-8 code points (continuation bytes not counted).
inline int textWidth(const std::string& s)
{
    int n = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) ++n;
    return n;
}

inline std::string repeat(const std::string& s, int n)
{
    std::string o;
    for (int i = 0; i < n; ++i) o += s;
    return o;
}

inline std::string pad(const std::string& s, int w, Align a)
{
    const int gap = w - textWidth(s);
    if (gap <= 0) return s;
    switch (a) {
        case Align::Right:  return std::string(gap, ' ') + s;
        case Align::Center: return std::string(gap / 2, ' ') + s
                                   + std::string(gap - gap / 2, ' ');
        default:            return s + std::string(gap, ' ');
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

inline std::string rtrim(std::string s)
{
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

inline std::string num(double v, int prec, char conv, bool sign,
                       const std::string& nonFinite)
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

struct BoxChars {
    std::string h, v, tl, tm, tr, ml, mm, mr, bl, bm, br;
};

inline BoxChars boxChars(const Style& s)
{
    BoxChars b;
    switch (s.border) {
        case Border::Unicode:
            b.h  = "\xE2\x94\x80"; b.v  = "\xE2\x94\x82";
            b.tl = "\xE2\x94\x8C"; b.tm = "\xE2\x94\xAC"; b.tr = "\xE2\x94\x90";
            b.ml = "\xE2\x94\x9C"; b.mm = "\xE2\x94\xBC"; b.mr = "\xE2\x94\xA4";
            b.bl = "\xE2\x94\x94"; b.bm = "\xE2\x94\xB4"; b.br = "\xE2\x94\x98";
            break;
        case Border::Ascii:
            b.h = "-"; b.v = "|";
            b.tl = b.tm = b.tr = b.ml = b.mm = b.mr = b.bl = b.bm = b.br = "+";
            break;
        default:
            b.h = std::string(1, s.rule);
            b.tl = b.tm = b.tr = b.ml = b.mm = b.mr = b.bl = b.bm = b.br = b.h;
            break;
    }
    return b;
}

inline std::string bold(const Style& s, const std::string& t)
{
    return s.color ? "\x1b[1m" + t + "\x1b[0m" : t;
}

} // namespace detail

// ---- Table ------------------------------------------------------------------
// Sections of label / value / unit rows.
class Table {
public:
    explicit Table(Style s = Style()) : st_(std::move(s)) {}

    const Style& style() const { return st_; }
    bool empty() const { return lines_.empty(); }
    void clear() { lines_.clear(); }

    // rule, title, rule
    Table& title(const std::string& t)
    {
        ensureRule();
        push(Kind::Head, indent() + t);
        push(Kind::Rule, "");
        return *this;
    }
    // rule, name, rule (a rule directly above is not repeated)
    Table& section(const std::string& name)
    {
        ensureRule();
        push(Kind::Head, indent() + name);
        push(Kind::Rule, "");
        return *this;
    }
    Table& rule()  { ensureRule(); return *this; }
    Table& blank() { push(Kind::Text, ""); return *this; }
    Table& text(const std::string& t) { push(Kind::Text, indent() + t); return *this; }

    Table& row(const std::string& label, const std::string& value,
               const std::string& unit = "")
    {
        std::string v = value;
        if (st_.valueWidth > 0) v = detail::pad(v, st_.valueWidth, st_.valueAlign);
        std::string line = indent() + detail::pad(label, st_.labelWidth, Align::Left)
                           + " " + v;
        if (!unit.empty()) line += std::string(st_.unitGap, ' ') + unit;
        push(Kind::Text, line);
        return *this;
    }
    Table& row(const std::string& label, const char* value, const std::string& unit = "")
    {
        return row(label, std::string(value), unit);
    }
    Table& row(const std::string& label, double v, int prec = 4,
               const std::string& unit = "")
    {
        return row(label, detail::num(v, adj(prec), 'f', false, st_.nonFinite), unit);
    }
    // Skipped entirely when empty (like the Python "if v is None: return").
    Table& row(const std::string& label, const std::optional<double>& v, int prec = 4,
               const std::string& unit = "")
    {
        if (v) row(label, *v, prec, unit);
        return *this;
    }
    Table& rowSci(const std::string& label, double v, int prec = 6,
                  const std::string& unit = "")
    {
        return row(label, detail::num(v, adj(prec), 'e', false, st_.nonFinite), unit);
    }
    Table& rowInt(const std::string& label, long long v, const std::string& unit = "")
    {
        return row(label, std::to_string(v), unit);
    }
    // "0.9944 <angle> +1.23<deg>"
    Table& rowPolar(const std::string& label, double mag, double angDeg,
                    int magPrec = 4, int angPrec = 2)
    {
        return row(label, detail::num(mag, adj(magPrec), 'f', false, st_.nonFinite)
                          + " " + st_.sym.angle + " "
                          + detail::num(angDeg, adj(angPrec), 'f', true, st_.nonFinite)
                          + st_.sym.deg);
    }

    void print(std::ostream& os) const
    {
        if (lines_.empty()) return;
        std::vector<Line> ls = lines_;
        if (ls.front().kind != Kind::Rule) ls.insert(ls.begin(), Line{Kind::Rule, ""});
        if (ls.back().kind  != Kind::Rule) ls.push_back(Line{Kind::Rule, ""});

        const bool boxed = st_.border != Border::Plain;
        int maxw = 0;
        for (const Line& l : ls)
            if (l.kind != Kind::Rule) maxw = std::max(maxw, detail::textWidth(l.text));
        const int W  = std::max(st_.width, maxw + (boxed ? 4 : 0));
        const detail::BoxChars bc = detail::boxChars(st_);

        for (std::size_t i = 0; i < ls.size(); ++i) {
            const Line& l = ls[i];
            if (l.kind == Kind::Rule) {
                if (!boxed) {
                    os << detail::repeat(bc.h, W) << '\n';
                } else {
                    const std::string& L = i == 0 ? bc.tl : (i + 1 == ls.size() ? bc.bl : bc.ml);
                    const std::string& R = i == 0 ? bc.tr : (i + 1 == ls.size() ? bc.br : bc.mr);
                    os << L << detail::repeat(bc.h, W - 2) << R << '\n';
                }
                continue;
            }
            const std::string body = l.kind == Kind::Head
                ? detail::bold(st_, l.text) : l.text;
            if (!boxed) {
                os << detail::rtrim(body) << '\n';
            } else {
                const int gap = (W - 4) - detail::textWidth(l.text);
                os << bc.v << ' ' << body << std::string(gap > 0 ? gap : 0, ' ')
                   << ' ' << bc.v << '\n';
            }
        }
    }

    std::string str() const { std::ostringstream o; print(o); return o.str(); }

private:
    enum class Kind { Rule, Text, Head };
    struct Line { Kind kind; std::string text; };

    Style             st_;
    std::vector<Line> lines_;

    std::string indent() const { return std::string(std::max(0, st_.indent), ' '); }
    int  adj(int p) const { return std::max(0, p + st_.extraDigits); }
    void push(Kind k, const std::string& t) { lines_.push_back(Line{k, t}); }
    void ensureRule()
    {
        if (lines_.empty() || lines_.back().kind != Kind::Rule) push(Kind::Rule, "");
    }
};

inline std::ostream& operator<<(std::ostream& os, const Table& t)
{
    t.print(os);
    return os;
}

// ---- Grid -------------------------------------------------------------------
struct Column {
    std::string header;
    Align       align    = Align::Left;
    int         minWidth = 0;
    int         maxWidth = 0;   // 0 = unlimited; longer cells end in "..."
};

class Grid {
public:
    Grid(Style s, std::vector<Column> cols) : st_(std::move(s)), cols_(std::move(cols)) {}
    explicit Grid(std::vector<Column> cols) : cols_(std::move(cols)) {}

    Grid& title(const std::string& t) { title_ = t; return *this; }
    Grid& addRow(std::vector<std::string> cells)
    {
        rows_.push_back(std::move(cells));
        return *this;
    }
    std::size_t rows() const { return rows_.size(); }
    const Style& style() const { return st_; }

    void print(std::ostream& os) const
    {
        const std::size_t n = cols_.size();
        if (n == 0) return;

        std::vector<int> w(n);
        for (std::size_t i = 0; i < n; ++i) {
            w[i] = std::max(detail::textWidth(cols_[i].header), cols_[i].minWidth);
            for (const auto& r : rows_)
                if (i < r.size()) w[i] = std::max(w[i], detail::textWidth(r[i]));
            if (cols_[i].maxWidth > 0) w[i] = std::min(w[i], cols_[i].maxWidth);
        }

        auto cell = [&](std::size_t i, const std::string& s) {
            return detail::pad(detail::truncate(s, w[i]), w[i], cols_[i].align);
        };

        const std::string ind(std::max(0, st_.indent), ' ');
        if (!title_.empty()) os << ind << detail::bold(st_, title_) << '\n';

        const detail::BoxChars bc = detail::boxChars(st_);

        if (st_.border == Border::Plain) {
            int total = st_.indent;
            for (int x : w) total += x;
            total += 2 * static_cast<int>(n - 1);
            const int W = std::max(st_.width, total);
            const std::string ruleLine = detail::repeat(bc.h, W);

            auto line = [&](const std::vector<std::string>& cells, bool header) {
                std::string o = ind;
                for (std::size_t i = 0; i < n; ++i) {
                    const std::string s = header ? cols_[i].header
                                                 : (i < cells.size() ? cells[i] : std::string());
                    o += cell(i, s);
                    if (i + 1 < n) o += "  ";
                }
                os << detail::rtrim(header && st_.color ? detail::bold(st_, o) : o) << '\n';
            };
            os << ruleLine << '\n';
            line({}, true);
            os << ruleLine << '\n';
            for (const auto& r : rows_) line(r, false);
            os << ruleLine << '\n';
            return;
        }

        auto hline = [&](const std::string& L, const std::string& M, const std::string& R) {
            os << L;
            for (std::size_t i = 0; i < n; ++i) {
                os << detail::repeat(bc.h, w[i] + 2);
                os << (i + 1 < n ? M : R);
            }
            os << '\n';
        };
        auto boxRow = [&](const std::vector<std::string>& cells, bool header) {
            os << bc.v;
            for (std::size_t i = 0; i < n; ++i) {
                const std::string s = header ? cols_[i].header
                                             : (i < cells.size() ? cells[i] : std::string());
                const std::string c = cell(i, s);
                os << ' ' << (header ? detail::bold(st_, c) : c) << ' ' << bc.v;
            }
            os << '\n';
        };
        hline(bc.tl, bc.tm, bc.tr);
        boxRow({}, true);
        hline(bc.ml, bc.mm, bc.mr);
        for (const auto& r : rows_) boxRow(r, false);
        hline(bc.bl, bc.bm, bc.br);
    }

    std::string str() const { std::ostringstream o; print(o); return o.str(); }

private:
    Style                                 st_;
    std::vector<Column>                   cols_;
    std::vector<std::vector<std::string>> rows_;
    std::string                           title_;
};

inline std::ostream& operator<<(std::ostream& os, const Grid& g)
{
    g.print(os);
    return os;
}

// ---- Adapters: TransmissionLine.hpp ------------------------------------------
// Duck typed on member names of LineResults / DualCircuitResults /
// GeometryInput / DualCircuitInput.
namespace detail {

struct Units {
    std::string ohm, ohmKm, uFkm, mm2, deg;
    explicit Units(const Style& s)
        : ohm(s.sym.ohm), ohmKm(s.sym.ohm + "/km"), uFkm(s.sym.micro + "F/km"),
          mm2("mm" + s.sym.sq), deg(s.sym.deg) {}
};

inline std::string coord(const Style& s, double x, double y)
{
    return "(" + num(x, 2, 'f', false, s.nonFinite) + ", "
               + num(y, 2, 'f', false, s.nonFinite) + ")";
}

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
    t.section("CONDUCTOR");
    t.row("GMR, DS", g.DS, 5, "m");
    if (g.radius_m > 0) t.row("Outer radius", g.radius_m, 5, "m");
    else                t.row("Outer radius", "auto (DS / 0.7788, solid round)");
    t.rowInt("Sub-conductors per phase", g.bundleNo);
    if (g.bundleNo > 1) t.row("Bundle spacing", g.bundleSpace, 3, "m");
    t.row(dual ? "AC resistance (per circuit)" : "AC resistance", g.r_ac, 4, u.ohmKm);

    t.section("OPERATING");
    t.row("Frequency", g.freq, 1, "Hz");
    t.row("Length", g.lengthKm, 1, "km");
    t.row("Voltage (L-L)", g.voltageKV, 1, "kV");
    t.row(dual ? "Current (per circuit)" : "Current", g.currentA, 1, "A");
}

template <class R>
void abcdRows(Table& t, const R& r)
{
    t.section("ABCD MATRIX");
    t.rowPolar("A", r.A_mag, r.A_ang);
    t.rowPolar("B", r.B_mag, r.B_ang);
    t.rowPolar("C", r.C_mag, r.C_ang);
    t.rowPolar("D", r.D_mag, r.D_ang);
}

template <bool Dual, class R>
void commonLineRows(Table& t, const R& r)
{
    const Units u(t.style());
    const Symbols& s = t.style().sym;

    t.section("PER-UNIT-LENGTH");
    t.row("Inductance",  r.inductance_mH_km,  4, "mH/km");
    t.row("Capacitance", r.capacitance_uF_km, 5, u.uFkm);
    t.row("Reactance",   r.reactance_ohm_km,  4, u.ohmKm);
    t.row("Susceptance", r.susceptance_S_km,  6, "S/km");

    t.section("PROPAGATION");
    t.row("Zc", r.Zc_ohm, 3, u.ohm);
    t.row(s.beta + " (k)", r.k_rad_km, 6, "rad/km");
    t.row("Velocity factor", r.vel_factor, 4);

    t.section("POWER SYSTEM");
    if constexpr (Dual) {
        t.row("SIL (per circuit)", r.SIL_MVA, 2, "MVA");
        t.row("SIL (total)", r.SIL_total_MVA, 2, "MVA");
    } else {
        t.row("SIL", r.SIL_MVA, 2, "MVA");
    }
    t.row(s.lambda + "/4 length", r.quarter_wave_km, 1, "km");
    if constexpr (!Dual) t.row("Total admittance", r.admittance_S, 4, "S");
    t.row(Dual ? "Charging (both circuits)" : "Charging", r.charging_MVAR, 2, "MVAR");
    t.row(Dual ? "Loadability (per circuit)" : "Loadability", r.loadability_MW, 1, "MW");

    abcdRows(t, r);
}

} // namespace detail

// Inputs of a single circuit line (GeometryInput) plus derived phase spacings.
template <class G>
void addLineInputs(Table& t, const G& g)
{
    const Style& s = t.style();
    t.section("PHASE COORDINATES (x, y)");
    t.row("Phase A", detail::coord(s, g.x1, g.y1), "m");
    t.row("Phase B", detail::coord(s, g.x2, g.y2), "m");
    t.row("Phase C", detail::coord(s, g.x3, g.y3), "m");

    detail::conductorAndOperating(t, g, false);

    const double D12 = detail::phaseDist(g.x1, g.y1, g.x2, g.y2);
    const double D23 = detail::phaseDist(g.x2, g.y2, g.x3, g.y3);
    const double D13 = detail::phaseDist(g.x1, g.y1, g.x3, g.y3);
    t.section("GEOMETRY");
    t.row("D12", D12, 3, "m");
    t.row("D23", D23, 3, "m");
    t.row("D13", D13, 3, "m");
    t.row("GMD", std::cbrt(D12 * D23 * D13), 3, "m");
}

// Inputs of a dual circuit line (DualCircuitInput).
template <class G>
void addDualLineInputs(Table& t, const G& g)
{
    const Style& s = t.style();
    t.section("PHASE COORDINATES (x, y)");
    t.row("Phase A1", detail::coord(s, g.x1, g.y1), "m");
    t.row("Phase B1", detail::coord(s, g.x2, g.y2), "m");
    t.row("Phase C1", detail::coord(s, g.x3, g.y3), "m");
    t.row("Phase A2", detail::coord(s, g.x4, g.y4), "m");
    t.row("Phase B2", detail::coord(s, g.x5, g.y5), "m");
    t.row("Phase C2", detail::coord(s, g.x6, g.y6), "m");

    detail::conductorAndOperating(t, g, true);
}

// LineResults.
template <class R>
void addLineResults(Table& t, const R& r)
{
    detail::commonLineRows<false>(t, r);
}

// DualCircuitResults (adds the equivalent-geometry block first).
template <class R>
void addDualLineResults(Table& t, const R& r)
{
    t.section("GEOMETRY (EQUIVALENT)");
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
               const std::string& title = "TRANSMISSION LINE")
{
    Table t(st);
    if (!title.empty()) t.title(title);
    addLineInputs(t, g);
    addLineResults(t, r);
    t.print(os);
}

template <class R>
void printLineResults(std::ostream& os, const R& r, const Style& st = Style(),
                      const std::string& title = "TRANSMISSION LINE")
{
    Table t(st);
    if (!title.empty()) t.title(title);
    addLineResults(t, r);
    t.print(os);
}

template <class G, class R>
void printDualLine(std::ostream& os, const G& g, const R& r, const Style& st = Style(),
                   const std::string& title = "DUAL-CIRCUIT TRANSMISSION LINE")
{
    Table t(st);
    if (!title.empty()) t.title(title);
    addDualLineInputs(t, g);
    addDualLineResults(t, r);
    t.print(os);
}

template <class R>
void printDualLineResults(std::ostream& os, const R& r, const Style& st = Style(),
                          const std::string& title = "DUAL-CIRCUIT TRANSMISSION LINE")
{
    Table t(st);
    if (!title.empty()) t.title(title);
    addDualLineResults(t, r);
    t.print(os);
}

// ---- Adapters: Conductors.hpp ------------------------------------------------
namespace detail {

template <class T> const T& deref(const T& v) { return v; }
template <class T> const T& deref(const T* v)  { return *v; }

inline std::string ratingCell(const Style& s, const std::optional<double>& v)
{
    return v ? num(*v, 0, 'f', false, s.nonFinite) : s.missing;
}

} // namespace detail

// Identity, geometry, mechanical and electrical blocks of one Conductor.
template <class C>
void addConductor(Table& t, const C& c)
{
    const detail::Units u(t.style());

    t.section("IDENTITY");
    t.row("Type", c.type);
    t.row("Codename", c.codename);
    t.rowInt("Id", c.id);
    t.row("Source", c.source_file);

    t.section("GEOMETRY");
    t.row("Stranding", c.stranding);
    t.row("Overall diameter", c.overall_diameter_mm, 2, "mm");
    t.row("Area", c.area_mm2, 1, u.mm2);
    t.row("Outer radius", c.radius_m(), 5, "m");
    t.row("GMR (estimated)", c.gmr_m(), 5, "m");
    t.row("GMR / radius", c.gmrRatio(), 4);

    t.section("MECHANICAL");
    t.row("Mass", c.mass_kg_km, 1, "kg/km");
    t.row("Breaking load", c.breaking_load_kN, 2, "kN");
    t.row("Modulus", c.modulus_GPa, 1, "GPa");
    t.row("Expansion", c.expansion_1e6, 1, "x10^-6 /degC");

    t.section("ELECTRICAL");
    t.row("DC resistance (20 degC)", c.dc_resistance_20C, 4, u.ohmKm);
    t.row("DC resistance (75 degC)", c.dc_resistance_75C, 4, u.ohmKm);
    t.row("AC resistance (50 Hz)", c.ac_resistance_50Hz, 4, u.ohmKm);
    t.row("Reactance (50 Hz)", c.reactance_50Hz, 4, u.ohmKm);
}

// Current rating matrix (environment x season, by wind speed).
template <class C>
Grid ratingsGrid(const C& c, const Style& st = Style())
{
    Grid g(st, {Column{"Condition", Align::Left}, Column{"Still (A)", Align::Right},
                Column{"1 m/s (A)", Align::Right}, Column{"2 m/s (A)", Align::Right}});
    g.title("CURRENT RATING");
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
    Grid g(st, {Column{"Type"}, Column{"Codename"}, Column{"Stranding"},
                Column{"Dia (mm)", Align::Right}, Column{"Area (" + u.mm2 + ")", Align::Right},
                Column{"Rac (" + u.ohmKm + ")", Align::Right},
                Column{"X (" + u.ohmKm + ")", Align::Right},
                Column{"Mass (kg/km)", Align::Right}});
    for (const auto& item : list) {
        const auto& c = detail::deref(item);
        g.addRow({c.type, c.codename, c.stranding,
                  detail::num(c.overall_diameter_mm, 1, 'f', false, st.nonFinite),
                  detail::num(c.area_mm2, 1, 'f', false, st.nonFinite),
                  detail::num(c.ac_resistance_50Hz, 4, 'f', false, st.nonFinite),
                  detail::num(c.reactance_50Hz, 3, 'f', false, st.nonFinite),
                  detail::num(c.mass_kg_km, 0, 'f', false, st.nonFinite)});
    }
    return g;
}

template <class C>
void printConductor(std::ostream& os, const C& c, const Style& st = Style(),
                    bool withRatings = true)
{
    Table t(st);
    t.title(c.type + " " + c.codename);
    addConductor(t, c);
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

} // namespace print
} // namespace LineTool
