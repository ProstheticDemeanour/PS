// QuickCalc.hpp
// Quick calculator for balanced electrical quantities: give it whatever you
// know (voltage, current, power, impedance, pf) and it computes everything it
// can. Needs Structs.hpp and ParamsPrinter.hpp.
//
//   parseToken(p, "33kV");  parseToken(p, "250kVA");
//   quickcalc::Report rep = quickcalc::solve(p);
//   print::Style st;
//   quickcalc::makeGrid(p, st).print(std::cout);
//
// Relations used (k = sqrt(3) for 3 phase, 1 for 1 phase):
//   S = k V I        Z = V / (k I) = V^2 / S        S = k^2 I^2 Z
//   P = S pf         Q = +-S sin(phi)               R = Z pf     X = +-Z sin(phi)
// Any two of V, I, S, Z fix the other two. Fault level calculations use the
// same relations on a second set (If, Sf, Zf) that shares V:
//   Sf = k V If      Zf = V / (k If) = V^2 / Sf     (Zf = V_LN / If, Sf = 3 V_LN If)
// where V is the line-to-line voltage; give the line-to-neutral voltage with
// "vln=" and it is multiplied by sqrt(3). pf, angle, or one of P/Q/R/X with
// S or Z fixes the triangles. If lag or lead cannot be told from the inputs
// the load is assumed lagging and the results are marked "assumed".
//
// Input rules: given values that disagree with the rest by more than 1% are
// reported in Report::conflicts (the given value is kept).
#pragma once

#include "ParamsPrinter.hpp"
#include "Structs.hpp"
#include "base_command.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <stdexcept>

namespace LineTool {
namespace quickcalc {

    inline constexpr double kSqrt3 = 1.7320508075688772;
    inline constexpr double kPi    = 3.14159265358979323846;

    struct Report {
        std::vector<std::string> conflicts;     // given values that disagree
        bool                     assumedLagging = false;
        std::vector<std::string> notComputed;   // names still unknown
        std::string              hint;          // what to add, empty if complete
    };

    namespace detail {

    enum class Dim { Voltage, Current, Apparent, FaultLevel, Active, Reactive, Impedance, Angle };

    inline std::string lower(std::string s)
    {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    inline std::string trim(const std::string& s)
    {
        std::size_t a = 0, b = s.size();
        while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
        while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
        return s.substr(a, b - a);
    }

    struct Scale { double f; std::string sym; double min; };   // used when |v| >= min

    inline std::vector<Scale> scales(Dim d, const print::Symbols& y)
    {
        auto S = [](double f, const std::string& sym, double min = -1.0) {
            return Scale{f, sym, min < 0 ? f : min};
        };
        switch (d) {
            case Dim::Voltage:    return {S(1, "V"), S(1e3, "kV"), S(1e6, "MV")};
            case Dim::Current:    return {S(1e-3, "mA", 0), S(1, "A", 0.1), S(1e3, "kA")};
            case Dim::Apparent:   return {S(1, "VA"), S(1e3, "kVA"), S(1e6, "MVA"), S(1e9, "GVA")};
            case Dim::FaultLevel: return {S(1e3, "kVA", 0), S(1e6, "MVA")};
            case Dim::Active:     return {S(1, "W"), S(1e3, "kW"), S(1e6, "MW"), S(1e9, "GW")};
            case Dim::Reactive:   return {S(1, "var"), S(1e3, "kvar"), S(1e6, "Mvar"), S(1e9, "Gvar")};
            case Dim::Impedance:  return {S(1e-3, "m" + y.ohm, 0), S(1, y.ohm, 0.1),
                                          S(1e3, "k" + y.ohm), S(1e6, "M" + y.ohm)};
            case Dim::Angle:      return {S(1, y.deg)};
        }
        return {S(1, "")};
    }

    // Value in the engineering unit that suits its size.
    inline std::pair<double, std::string> scaled(Dim d, double v, const print::Symbols& y)
    {
        const std::vector<Scale> t = scales(d, y);
        const Scale* pick = &t.front();
        if (v == 0) {
            for (const Scale& s : t) if (s.f == 1) pick = &s;
        } else {
            for (const Scale& s : t) if (std::fabs(v) >= s.min) pick = &s;
        }
        return {v / pick->f, pick->sym};
    }

    // SI unit symbol used in machine output.
    inline std::string baseUnit(Dim d)
    {
        switch (d) {
            case Dim::Voltage:   return "V";
            case Dim::Current:   return "A";
            case Dim::Apparent:  return "VA";
            case Dim::FaultLevel: return "VA";
            case Dim::Active:    return "W";
            case Dim::Reactive:  return "var";
            case Dim::Impedance: return "ohm";
            case Dim::Angle:     return "deg";
        }
        return "";
    }

    // Significant digits, no exponent, trailing zeros trimmed.
    inline std::string fmtSig(double v, int digits)
    {
        if (!std::isfinite(v)) return "n/a";
        if (v == 0) return "0";
        const int e = static_cast<int>(std::floor(std::log10(std::fabs(v))));
        const int dec = std::max(0, digits - 1 - e);
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.*f", dec, v);
        std::string s = buf;
        if (s.find('.') != std::string::npos) {
            while (s.back() == '0') s.pop_back();
            if (s.back() == '.') s.pop_back();
        }
        return s;
    }

    // ---- unit parsing ---------------------------------------------------------------
    inline bool parseUnit(const std::string& u, Dim& dim, double& scale)
    {
        const std::string l = lower(u);
        if (u == "\xC2\xB0" || l == "deg" || l == "degree" || l == "degrees") {
            dim = Dim::Angle; scale = 1; return true;
        }
        auto ends = [&](const std::string& s) {
            return l.size() >= s.size() && l.compare(l.size() - s.size(), s.size(), s) == 0;
        };
        auto endsRaw = [&](const char* s) {
            const std::size_t k = std::strlen(s);
            return u.size() >= k && u.compare(u.size() - k, k, s) == 0;
        };
        std::size_t baseLen = 0;
        if      (ends("ohms"))                 { dim = Dim::Impedance; baseLen = 4; }
        else if (ends("ohm"))                  { dim = Dim::Impedance; baseLen = 3; }
        else if (endsRaw("\xCE\xA9"))          { dim = Dim::Impedance; baseLen = 2; }   // U+03A9
        else if (endsRaw("\xE2\x84\xA6"))      { dim = Dim::Impedance; baseLen = 3; }   // U+2126
        else if (ends("var"))                  { dim = Dim::Reactive;  baseLen = 3; }
        else if (ends("va"))                   { dim = Dim::Apparent;  baseLen = 2; }
        else if (ends("w"))                    { dim = Dim::Active;    baseLen = 1; }
        else if (ends("v"))                    { dim = Dim::Voltage;   baseLen = 1; }
        else if (ends("a"))                    { dim = Dim::Current;   baseLen = 1; }
        else return false;

        const std::string prefix = u.substr(0, u.size() - baseLen);
        const bool power = dim == Dim::Apparent || dim == Dim::Active || dim == Dim::Reactive;
        if (prefix.empty())                           scale = 1;
        else if (prefix == "k" || prefix == "K")      scale = 1e3;
        else if (prefix == "M")                       scale = 1e6;
        else if (prefix == "m")                       scale = power ? 1e6 : 1e-3;
        else if (prefix == "G" || prefix == "g")      scale = 1e9;
        else return false;
        return true;
    }

    } // namespace detail

    // ---- parsing ----------------------------------------------------------------------
    // Accepts "33kV", "250kVA", "4.4A", "100kW", "50kvar", "-50kvar", "50ohm",
    // "30deg", "0.9pf", "0.9lag", "0.9lead", "pf=0.9lead", and named forms such as
    // "V=33kV", "R=2ohm", "X=5ohm", "angle=30". Returns an error message, or an
    // empty string on success.
    inline std::string parseToken(ElectricalParams& p, const std::string& tokenIn)
    {
        using namespace detail;
        const std::string tok = trim(tokenIn);
        if (tok.empty()) return "empty value";

        std::string name, body = tok;
        const std::size_t eq = tok.find('=');
        if (eq != std::string::npos) {
            name = lower(trim(tok.substr(0, eq)));
            body = trim(tok.substr(eq + 1));
            if (body.empty()) return "missing value in '" + tok + "'";
        }
        const std::string lb = lower(body);

        // power factor
        if (name == "pf" || lb.find("pf") != std::string::npos ||
            lb.find("lag") != std::string::npos || lb.find("lead") != std::string::npos) {
            if (!name.empty() && name != "pf") return "'" + tok + "' is not a power factor";
            const std::size_t d = lb.find_first_of("0123456789.");
            if (d == std::string::npos) return "can't read a power factor in '" + tok + "'";
            char* end = nullptr;
            const double v = std::strtod(lb.c_str() + d, &end);
            std::string rest = trim(std::string(end));
            PfSense sense = PfSense::Unknown;
            if (rest.rfind("lead", 0) == 0)     { sense = PfSense::Leading; rest.erase(0, 4); }
            else if (rest.rfind("lag", 0) == 0) { sense = PfSense::Lagging; rest.erase(0, 3); }
            if (rest == "ging" || rest == "ing") rest.clear();
            if (rest == "pf") rest.clear();
            if (!rest.empty()) return "can't read a power factor in '" + tok + "'";
            if (!(v >= 0.0 && v <= 1.0)) return "power factor must be between 0 and 1 in '" + tok + "'";
            if (p.pf.known()) return "power factor given twice";
            p.pf.set(v, Source::Given);
            if (sense != PfSense::Unknown) p.sense = sense;
            return "";
        }

        char* end = nullptr;
        const double v = std::strtod(body.c_str(), &end);
        if (end == body.c_str() || !std::isfinite(v)) return "can't read a number in '" + tok + "'";
        const std::string unit = trim(std::string(end));

        // target quantity from the name, if any
        enum class Field { None, V, I, S, P, Q, Z, R, X, Angle, If, Sf, Zf } field = Field::None;
        bool lineToNeutral = false;
        Dim nameDim = Dim::Voltage;
        if (!name.empty()) {
            if      (name == "v") { field = Field::V; nameDim = Dim::Voltage; }
            else if (name == "i") { field = Field::I; nameDim = Dim::Current; }
            else if (name == "s") { field = Field::S; nameDim = Dim::Apparent; }
            else if (name == "p") { field = Field::P; nameDim = Dim::Active; }
            else if (name == "q") { field = Field::Q; nameDim = Dim::Reactive; }
            else if (name == "z") { field = Field::Z; nameDim = Dim::Impedance; }
            else if (name == "r") { field = Field::R; nameDim = Dim::Impedance; }
            else if (name == "x") { field = Field::X; nameDim = Dim::Impedance; }
            else if (name == "angle" || name == "phi") { field = Field::Angle; nameDim = Dim::Angle; }
            else if (name == "if")  { field = Field::If; nameDim = Dim::Current; }
            else if (name == "sf")  { field = Field::Sf; nameDim = Dim::Apparent; }
            else if (name == "zf")  { field = Field::Zf; nameDim = Dim::Impedance; }
            else if (name == "vln") { field = Field::V;  nameDim = Dim::Voltage; lineToNeutral = true; }
            else return "unknown name '" + name + "' (use v vln i if s sf p q z zf r x pf angle)";
        }

        Dim dim = nameDim;
        double scale = 1.0;
        if (!unit.empty()) {
            Dim ud;
            if (!parseUnit(unit, ud, scale)) return "unknown unit '" + unit + "' in '" + tok + "'";
            if (field != Field::None && ud != nameDim)
                return "unit '" + unit + "' doesn't match '" + name + "' in '" + tok + "'";
            dim = ud;
        } else if (field == Field::None) {
            return "missing unit in '" + tok + "' (for example 33kV, 250kVA, 4.4A, 50ohm)";
        }

        if (field == Field::None) {
            switch (dim) {
                case Dim::Voltage:   field = Field::V; break;
                case Dim::Current:   field = Field::I; break;
                case Dim::Apparent:  field = Field::S; break;
                case Dim::Active:    field = Field::P; break;
                case Dim::Reactive:  field = Field::Q; break;
                case Dim::Impedance: field = Field::Z; break;
                case Dim::Angle:     field = Field::Angle; break;
                case Dim::FaultLevel: field = Field::Sf; break;
            }
        }

        double val = v * scale;
        if (lineToNeutral && p.phases != 1) val *= kSqrt3;
        Quantity* q = nullptr;
        const char* nm = "";
        bool positive = true, nonNegative = false;
        switch (field) {
            case Field::V:     q = &p.voltage;    nm = "voltage";    break;
            case Field::I:     q = &p.current;    nm = "current";    break;
            case Field::S:     q = &p.apparent;   nm = "apparent power"; break;
            case Field::P:     q = &p.active;     nm = "active power"; positive = false; nonNegative = true; break;
            case Field::Q:     q = &p.reactive;   nm = "reactive power"; positive = false; break;
            case Field::Z:     q = &p.impedance;  nm = "impedance";  break;
            case Field::R:     q = &p.resistance; nm = "resistance"; positive = false; nonNegative = true; break;
            case Field::X:     q = &p.reactance;  nm = "reactance";  positive = false; break;
            case Field::Angle: q = &p.angle;      nm = "angle";      positive = false; break;
            case Field::If:    q = &p.faultCurrent;   nm = "fault current"; break;
            case Field::Sf:    q = &p.faultLevel;     nm = "fault level"; break;
            case Field::Zf:    q = &p.faultImpedance; nm = "fault impedance"; break;
            case Field::None:  return "can't place '" + tok + "'";
        }
        if (positive && !(val > 0)) return std::string(nm) + " must be positive in '" + tok + "'";
        if (nonNegative && val < 0) return std::string(nm) + " can't be negative in '" + tok + "'";
        if (field == Field::Angle && std::fabs(val) > 90.0)
            return "angle must be between -90 and 90 degrees in '" + tok + "'";
        if (q->known()) return std::string(nm) + " given twice";
        q->set(val, Source::Given);
        return "";
    }

    // ---- solver -------------------------------------------------------------------------
    namespace detail {

        class Solver {
        public:
            Solver(ElectricalParams& p, Report& r)
                : p_(p), r_(r), k_(p.phases == 1 ? 1.0 : kSqrt3), k2_(k_ * k_) {}

            void run(bool assume)
            {
                assume_ = assume;
                for (int i = 0; i < 50; ++i) {
                    changed_ = false;
                    step();
                    if (!changed_) break;
                }
            }

            ElectricalParams& p_;
            Report&           r_;
            double            k_, k2_;
            bool              assume_  = false;
            bool              changed_ = false;
            const char*       group_   = "core";   // one conflict message per group
            std::set<std::string> seen_;

            static Dim dimOf(const std::string& n, bool& plain)
            {
                plain = false;
                if (n == "V") return Dim::Voltage;
                if (n == "I") return Dim::Current;
                if (n == "S") return Dim::Apparent;
                if (n == "If") return Dim::Current;
                if (n == "Sf") return Dim::FaultLevel;
                if (n == "P") return Dim::Active;
                if (n == "Q") return Dim::Reactive;
                if (n == "angle") return Dim::Angle;
                if (n == "pf") { plain = true; return Dim::Angle; }
                return Dim::Impedance;   // Z, R, X
            }

            static std::string fmt(const std::string& n, double v)
            {
                bool plain;
                const Dim d = dimOf(n, plain);
                if (plain) return fmtSig(v, 4);
                const auto s = scaled(d, v, print::Symbols::ascii());
                return fmtSig(s.first, 5) + " " + s.second;
            }

            void conflictOnce(const std::string& key, const std::string& msg)
            {
                if (seen_.insert(key).second) r_.conflicts.push_back(msg);
            }

            static bool differs(const std::string& n, double a, double b)
            {
                const double floor = n == "angle" ? 0.25 : 1e-9;
                return std::fabs(a - b) > 0.01 * std::max(std::fabs(a), std::fabs(b)) + floor;
            }

            void propose(Quantity& t, const char* name, double cand, const std::string& via)
            {
                if (!std::isfinite(cand)) return;
                if (!t.known()) {
                    t.set(cand, assume_ ? Source::Assumed : Source::Derived);
                    changed_ = true;
                    return;
                }
                if (t.source != Source::Given || !differs(name, t.value, cand)) return;
                char pct[16];
                std::snprintf(pct, sizeof pct, "%.2g",
                              100.0 * std::fabs(t.value - cand) / std::max(std::fabs(t.value), std::fabs(cand)));
                conflictOnce(group_, std::string(name) + ": given " + fmt(name, t.value) + ", but "
                                   + via + " give " + fmt(name, cand) + " (" + pct + "% apart)");
            }

            int sign() const
            {
                if (p_.sense == PfSense::Lagging) return 1;
                if (p_.sense == PfSense::Leading) return -1;
                return assume_ ? 1 : 0;
            }

            void noteSign(PfSense s, const char* who)
            {
                if (p_.sense == PfSense::Unknown) { p_.sense = s; changed_ = true; return; }
                if (p_.sense != s)
                    conflictOnce(group_, std::string(who) + " sign disagrees with the lag/lead of the other inputs");
            }

            void step()
            {
                Quantity &I = p_.current, &S = p_.apparent, &Z = p_.impedance;

                group_ = "core";
                core(I, S, Z, "I", "S", "Z");
                group_ = "fault";
                core(p_.faultCurrent, p_.faultLevel, p_.faultImpedance, "If", "Sf", "Zf");

                group_ = "triangle";
                angleRules();
                triangle(S, p_.active, p_.reactive, "S", "P", "Q");
                triangle(Z, p_.resistance, p_.reactance, "Z", "R", "X");
            }

            // Any two of V, I, S, Z fix the other two (V is shared between calls).
            void core(Quantity& I, Quantity& S, Quantity& Z, const char* in, const char* sn, const char* zn)
            {
                Quantity& V = p_.voltage;
                const std::string i = in, sx = sn, z = zn;
                if (V.known() && I.known()) {
                    propose(S, sn, k_ * V.value * I.value, "V and " + i);
                    propose(Z, zn, V.value / (k_ * I.value), "V and " + i);
                }
                if (S.known() && I.known()) {
                    propose(V, "V", S.value / (k_ * I.value), sx + " and " + i);
                    propose(Z, zn, S.value / (k2_ * I.value * I.value), sx + " and " + i);
                }
                if (S.known() && V.known()) {
                    propose(I, in, S.value / (k_ * V.value), sx + " and V");
                    propose(Z, zn, V.value * V.value / S.value, "V and " + sx);
                }
                if (Z.known() && I.known()) {
                    propose(V, "V", k_ * Z.value * I.value, z + " and " + i);
                    propose(S, sn, k2_ * I.value * I.value * Z.value, z + " and " + i);
                }
                if (Z.known() && V.known()) {
                    propose(I, in, V.value / (k_ * Z.value), "V and " + z);
                    propose(S, sn, V.value * V.value / Z.value, "V and " + z);
                }
                if (S.known() && Z.known()) {
                    propose(V, "V", std::sqrt(S.value * Z.value), sx + " and " + z);
                    propose(I, in, std::sqrt(S.value / (k2_ * Z.value)), sx + " and " + z);
                }
            }

            void angleRules()
            {
                Quantity& pf = p_.pf;
                Quantity& a  = p_.angle;
                if (a.known()) {
                    propose(pf, "pf", std::cos(a.value * kPi / 180.0), "the angle");
                    if (a.value != 0.0) noteSign(a.value > 0 ? PfSense::Lagging : PfSense::Leading, "angle");
                }
                if (pf.known()) {
                    const double s = std::sqrt(std::max(0.0, 1.0 - pf.value * pf.value));
                    if (s < 1e-12) propose(a, "angle", 0.0, "pf");
                    else if (const int sg = sign())
                        propose(a, "angle", sg * std::acos(pf.value) * 180.0 / kPi, "pf");
                }
            }

            // Right triangle H (hypotenuse), A (adjacent, pf * H), O (opposite, signed).
            void triangle(Quantity& H, Quantity& A, Quantity& O,
                          const char* hn, const char* an, const char* on)
            {
                Quantity& pf = p_.pf;
                const std::string H_ = hn, A_ = an, O_ = on;

                if (O.known() && O.value != 0.0)
                    noteSign(O.value > 0 ? PfSense::Lagging : PfSense::Leading, on);
                if (H.known() && A.known() && A.value > H.value * 1.01 + 1e-12)
                    conflictOnce(group_, A_ + " exceeds " + H_);
                if (H.known() && O.known() && std::fabs(O.value) > H.value * 1.01 + 1e-12)
                    conflictOnce(group_, O_ + " exceeds " + H_);

                const int sg = sign();
                auto sinOf = [&]() { return std::sqrt(std::max(0.0, 1.0 - pf.value * pf.value)); };

                if (H.known() && pf.known()) {
                    propose(A, an, H.value * pf.value, H_ + " and pf");
                    const double s = sinOf();
                    if (s < 1e-12) propose(O, on, 0.0, H_ + " and pf");
                    else if (sg)   propose(O, on, sg * H.value * s, H_ + " and pf");
                }
                if (A.known() && pf.known() && pf.value > 1e-12) {
                    propose(H, hn, A.value / pf.value, A_ + " and pf");
                    const double s = sinOf();
                    if (s < 1e-12) propose(O, on, 0.0, A_ + " and pf");
                    else if (sg)   propose(O, on, sg * A.value * s / pf.value, A_ + " and pf");
                }
                if (O.known() && pf.known()) {
                    const double s = sinOf();
                    if (s > 1e-9) {
                        propose(H, hn, std::fabs(O.value) / s, O_ + " and pf");
                        propose(A, an, std::fabs(O.value) * pf.value / s, O_ + " and pf");
                    }
                }
                if (H.known() && A.known() && H.value > 0)
                    propose(pf, "pf", std::min(1.0, A.value / H.value), H_ + " and " + A_);
                if (H.known() && A.known()) {
                    const double m = std::sqrt(std::max(0.0, H.value * H.value - A.value * A.value));
                    if (m < 1e-9 * H.value) propose(O, on, 0.0, H_ + " and " + A_);
                    else if (sg)            propose(O, on, sg * m, H_ + " and " + A_);
                }
                if (H.known() && O.known())
                    propose(A, an, std::sqrt(std::max(0.0, H.value * H.value - O.value * O.value)),
                            H_ + " and " + O_);
                if (A.known() && O.known())
                    propose(H, hn, std::hypot(A.value, O.value), A_ + " and " + O_);
            }
        };

        } // namespace detail

    // Computes everything that can be computed from the Given quantities.
    inline Report solve(ElectricalParams& p)
    {
        Report rep;
        { detail::Solver s(p, rep); s.run(false); }

        if (p.sense == PfSense::Unknown) {
            ElectricalParams trial = p;
            Report scratch;
            detail::Solver s(trial, scratch);
            s.run(true);
            const Quantity* a[] = {&p.voltage, &p.current, &p.apparent, &p.active, &p.reactive,
                                   &p.pf, &p.angle, &p.impedance, &p.resistance, &p.reactance,
                                   &p.faultCurrent, &p.faultLevel, &p.faultImpedance};
            const Quantity* b[] = {&trial.voltage, &trial.current, &trial.apparent, &trial.active,
                                   &trial.reactive, &trial.pf, &trial.angle, &trial.impedance,
                                   &trial.resistance, &trial.reactance,
                                   &trial.faultCurrent, &trial.faultLevel, &trial.faultImpedance};
            bool grew = false;
            for (int i = 0; i < 13; ++i) if (!a[i]->known() && b[i]->known()) grew = true;
            if (grew) {
                p = trial;
                p.sense = PfSense::Lagging;
                rep.assumedLagging = true;
            }
        }

        auto anyGiven = [](std::initializer_list<const Quantity*> l) {
            for (const Quantity* q : l) if (q->source == Source::Given) return true;
            return false;
        };
        const bool faultGiven = anyGiven({&p.faultCurrent, &p.faultLevel, &p.faultImpedance});
        const bool loadGiven  = anyGiven({&p.current, &p.apparent, &p.active, &p.reactive, &p.pf,
                                          &p.angle, &p.impedance, &p.resistance, &p.reactance});
        const bool showLoad = loadGiven || !faultGiven;

        struct N { const char* n; const Quantity* q; };
        std::vector<N> names;
        if (showLoad)
            names = {{"V", &p.voltage}, {"I", &p.current}, {"S", &p.apparent}, {"P", &p.active},
                     {"Q", &p.reactive}, {"pf", &p.pf}, {"angle", &p.angle}, {"Z", &p.impedance},
                     {"R", &p.resistance}, {"X", &p.reactance}};
        else
            names = {{"V", &p.voltage}};
        if (faultGiven) {
            names.push_back({"If", &p.faultCurrent});
            names.push_back({"Sf", &p.faultLevel});
            names.push_back({"Zf", &p.faultImpedance});
        }
        for (const N& x : names) if (!x.q->known()) rep.notComputed.push_back(x.n);

        // what to add: any two of {V, I, S, Z} (and of {V, If, Sf, Zf}) fix the rest
        auto need = [](std::initializer_list<N> core, const char* tail) -> std::string {
            int have = 0;
            std::string missing;
            for (const N& c : core) {
                if (c.q->known()) ++have;
                else missing += (missing.empty() ? "" : ", ") + std::string(c.n);
            }
            if (have >= 2) return "";
            return std::string("give ") + (have == 1 ? "one more" : "two") + " of " + missing + tail;
        };
        if (showLoad) {
            rep.hint = need({{"V", &p.voltage}, {"I", &p.current}, {"S", &p.apparent},
                             {"Z", &p.impedance}}, " to get the rest");
            if (rep.hint.empty() && !p.pf.known()) rep.hint = "give pf or angle to get P, Q, R and X";
        }
        if (faultGiven) {
            const std::string f = need({{"V", &p.voltage}, {"If", &p.faultCurrent},
                                        {"Sf", &p.faultLevel}, {"Zf", &p.faultImpedance}},
                                       " to get the fault level");
            if (!f.empty()) rep.hint += (rep.hint.empty() ? "" : "; ") + f;
        }
        return rep;
    }

    // ---- output ---------------------------------------------------------------------------
    inline const char* sourceName(Source s)
    {
        switch (s) {
            case Source::Given:   return "given";
            case Source::Derived: return "derived";
            case Source::Assumed: return "assumed";
            case Source::Default: return "default";
            default:              return "";
        }
    }

    // One row per known quantity: Quantity, Value, Unit, Source.
    // Human: engineering units, `digits` significant digits.
    // Terse/Json: stable keys, SI base units, full precision.
    inline print::Grid makeGrid(const ElectricalParams& p, const print::Style& st, int digits = 5)
    {
        using namespace detail;
        const bool human = st.format == print::Format::Human;
        const print::Symbols& y = st.symbols();

        print::Grid g(st, {print::Column{"Quantity", print::Align::Left, "quantity"},
                           print::Column{"Value", print::Align::Right, "value", true},
                           print::Column{"Unit", print::Align::Left, "unit"},
                           print::Column{"Source", print::Align::Left, "source"}});
        g.title(p.phases == 1 ? "Electrical parameters (1 phase)"
                              : "Electrical parameters (3 phase, voltage is line-to-line)");

        auto plain = [&](const char* label, const char* key, double v, const std::string& unit, Source s) {
            g.addRow({human ? label : key,
                      human ? fmtSig(v, digits) : print::detail::machineNum(v), unit, sourceName(s)});
        };
        auto add = [&](const char* label, const char* key, Dim d, const Quantity& q) {
            if (!q.known()) return;
            if (human) {
                const auto s = scaled(d, q.value, y);
                g.addRow({label, fmtSig(s.first, digits), s.second, sourceName(q.source)});
            } else {
                g.addRow({key, print::detail::machineNum(q.value), baseUnit(d), sourceName(q.source)});
            }
        };

        plain("Phases", "phases", p.phases, "", p.phasesSource);
        add(p.phases == 1 ? "Voltage" : "Voltage (L-L)", p.phases == 1 ? "voltage" : "voltage_ll",
            Dim::Voltage, p.voltage);
        if (p.phases != 1 && p.voltage.known()) {
            Quantity ln; ln.set(p.voltage.value / kSqrt3, Source::Derived);
            add("Voltage (L-N)", "voltage_ln", Dim::Voltage, ln);
        }
        add("Current",        "current",        Dim::Current,   p.current);
        add("Apparent power", "apparent_power", Dim::Apparent,  p.apparent);
        add("Active power",   "active_power",   Dim::Active,    p.active);
        add("Reactive power", "reactive_power", Dim::Reactive,  p.reactive);
        if (p.pf.known()) {
            const char* sense = p.sense == PfSense::Lagging ? "lagging"
                              : p.sense == PfSense::Leading ? "leading" : "";
            plain("Power factor", "power_factor", p.pf.value, sense, p.pf.source);
        }
        add("Phase angle",           "angle",      Dim::Angle,     p.angle);
        add("Impedance (per phase)", "impedance",  Dim::Impedance, p.impedance);
        add("Resistance (per phase)", "resistance", Dim::Impedance, p.resistance);
        add("Reactance (per phase)", "reactance",  Dim::Impedance, p.reactance);
        add("Fault current",         "fault_current",   Dim::Current,    p.faultCurrent);
        add("Fault level",           "fault_level",     Dim::FaultLevel, p.faultLevel);
        add("Fault impedance (per phase)", "fault_impedance", Dim::Impedance, p.faultImpedance);
        return g;
    }

} // namespace quickcalc
} // namespace LineTool

class vi_command : public BaseCommand {
public:
    explicit vi_command(const SoftwareConfig* cfg) : BaseCommand(cfg) {}

    std::string getName() const override;
    std::string getDescription() const override;
    int execute(const std::vector<std::string>& args) override;
    void showHelp() const override;
};
