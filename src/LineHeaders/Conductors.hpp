// Conductors.hpp
// Single-header conductor database loader (C++17), companion to
// TransmissionLine.hpp. Requires nlohmann/json (https://github.com/nlohmann/json).
//
// Loads the AAAC / AAC / ACSR-AC / ACSR-GZ JSON tables, normalises the three
// different current_rating layouts to one schema, and maps a conductor onto
// TransmissionLine input structs.
//
// Usage:
//   #include "TransmissionLine.hpp"
//   #include "Conductors.hpp"
//   using namespace LineTool;
//
//   auto all = conductors::loadAll("data");          // data/conductors/*.json
//   const auto* c = conductors::findByCodename(all, "Iodine");
//
//   GeometryInput g;                                 // or DualCircuitInput
//   c->applyTo(g);                                   // DS, radius_m, r_ac
//   c->applyRating(g, conductors::Environment::Rural,
//                     conductors::Season::Summer,
//                     conductors::Wind::Ms1);        // currentA (if tabulated)
//   auto res = TransmissionLine::compute(g);
//
// Normalised rating schema (missing values are std::nullopt):
//   rating.{rural,industrial}.{winter,summer}.{still,wind_1ms,wind_2ms}
//   Source layouts handled:
//     aac / aaac_1120 : rural.winter.still ...
//     acsr_ac         : rural_winter.still ...   (0 means "no data")
//     acsr_gz         : winter_night, summer_noon, industrial_winter,
//                       industrial_summer (still and wind_2ms only)
//   winter_night is mapped to rural.winter and summer_noon to rural.summer.
//
// ASCII only (MSVC safe).
#pragma once

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace LineTool {
namespace conductors {

// ---- Rating types -----------------------------------------------------------
struct RatingCondition {
    std::optional<double> still;      // A
    std::optional<double> wind_1ms;   // A
    std::optional<double> wind_2ms;   // A
};

struct RatingEnvironment {
    RatingCondition winter;
    RatingCondition summer;
};

struct CurrentRating {
    RatingEnvironment rural;
    RatingEnvironment industrial;
};

enum class Environment { Rural, Industrial };
enum class Season      { Winter, Summer };
enum class Wind        { Still, Ms1, Ms2 };

namespace detail {

// Total strand count from a stranding string, e.g. "7/3.00" -> 7,
// "30/2.00 + 7/3.25" -> 37. Returns 0 if unparseable.
inline int strandCount(const std::string& stranding)
{
    int total = 0;
    std::size_t pos = 0;
    while (pos < stranding.size()) {
        std::size_t plus  = stranding.find('+', pos);
        std::string token = stranding.substr(pos, plus == std::string::npos
                                                    ? std::string::npos : plus - pos);
        const std::size_t slash = token.find('/');
        if (slash != std::string::npos) {
            try { total += std::stoi(token.substr(0, slash)); }
            catch (...) { return 0; }
        }
        if (plus == std::string::npos) break;
        pos = plus + 1;
    }
    return total;
}

// GMR / outer radius for concentric-lay conductors by total strand count.
// 1 strand: exp(-1/4). 7, 19, 37, 61, 91: standard tabulated factors
// (0.726, 0.758, 0.768, 0.772, 0.774), linearly interpolated in between.
inline double gmrRatio(int n)
{
    if (n <= 0) return 0.726;
    if (n == 1) return std::exp(-0.25);
    constexpr int    ns[] = {7,     19,    37,    61,    91};
    constexpr double ks[] = {0.726, 0.758, 0.768, 0.772, 0.774};
    if (n <= ns[0]) return ks[0];
    for (int i = 1; i < 5; ++i) {
        if (n <= ns[i]) {
            const double t = double(n - ns[i-1]) / double(ns[i] - ns[i-1]);
            return ks[i-1] + t * (ks[i] - ks[i-1]);
        }
    }
    return ks[4];
}

inline std::string fmt(double v)
{
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

} // namespace detail

// ---- Conductor --------------------------------------------------------------
struct Conductor {
    // Identity
    std::string source_file;
    int         id = 0;
    std::string type;
    std::string codename;
    // Geometry
    std::string stranding;
    double overall_diameter_mm = 0.0;
    double area_mm2            = 0.0;
    // Mechanical
    double mass_kg_km       = 0.0;
    double breaking_load_kN = 0.0;
    double modulus_GPa      = 0.0;
    double expansion_1e6    = 0.0;   // x10^-6 /degC
    // Electrical
    double                dc_resistance_20C  = 0.0;   // ohm/km
    std::optional<double> dc_resistance_75C;          // ohm/km (acsr_ac only)
    double                ac_resistance_50Hz = 0.0;   // ohm/km
    double                reactance_50Hz     = 0.0;   // ohm/km
    // Current rating
    CurrentRating rating;

    // Outer radius, m. Feeds GeometryInput::radius_m (capacitance).
    double radius_m() const { return 0.5e-3 * overall_diameter_mm; }

    // GMR / outer radius from strand count (see detail::gmrRatio).
    double gmrRatio() const { return detail::gmrRatio(detail::strandCount(stranding)); }

    // Approximate GMR, m. Feeds GeometryInput::DS (inductance).
    // The JSON tables carry no datasheet GMR. For ACSR the whole conductor,
    // steel included, is treated as one concentric-lay bundle of strands, so
    // override g.DS with the datasheet value when you have it.
    double gmr_m() const { return gmrRatio() * radius_m(); }

    // Map this conductor onto a TransmissionLine input struct
    // (GeometryInput or DualCircuitInput; any type with DS, radius_m, r_ac).
    //   DS       <- gmr_m()
    //   radius_m <- radius_m()
    //   r_ac     <- ac_resistance_50Hz
    // r_ac is a 50 Hz table value. The data does not state its temperature
    // basis (ac/dc20 ratios of about 1.2 to 1.3 suggest roughly 75 degC).
    // Freq, geometry, bundle and voltage are left untouched.
    template <class Geometry>
    void applyTo(Geometry& g) const
    {
        g.DS       = gmr_m();
        g.radius_m = radius_m();
        g.r_ac     = ac_resistance_50Hz;
    }

    const RatingCondition& condition(Environment e, Season s) const
    {
        const RatingEnvironment& env =
            (e == Environment::Rural) ? rating.rural : rating.industrial;
        return (s == Season::Winter) ? env.winter : env.summer;
    }

    std::optional<double> currentRating(Environment e, Season s, Wind w) const
    {
        const RatingCondition& c = condition(e, s);
        switch (w) {
            case Wind::Still: return c.still;
            case Wind::Ms1:   return c.wind_1ms;
            case Wind::Ms2:   return c.wind_2ms;
        }
        return std::nullopt;
    }

    // Sets g.currentA if the requested rating is tabulated. Returns false
    // (and leaves g untouched) otherwise.
    template <class Geometry>
    bool applyRating(Geometry& g, Environment e, Season s, Wind w) const
    {
        const auto v = currentRating(e, s, w);
        if (!v) return false;
        g.currentA = *v;
        return true;
    }

    // Physical sanity checks on the rating table: current must not fall with
    // increasing wind speed, and summer must not exceed winter for the same
    // environment and wind. Returns human-readable descriptions of violations.
    std::vector<std::string> ratingIssues() const
    {
        std::vector<std::string> out;
        static const char* envName[]  = {"rural", "industrial"};
        static const char* seaName[]  = {"winter", "summer"};
        static const char* windName[] = {"still", "wind_1ms", "wind_2ms"};

        auto get = [this](int e, int s, int w) {
            return currentRating(e ? Environment::Industrial : Environment::Rural,
                                 s ? Season::Summer : Season::Winter,
                                 w == 0 ? Wind::Still : (w == 1 ? Wind::Ms1 : Wind::Ms2));
        };

        for (int e = 0; e < 2; ++e) {
            for (int s = 0; s < 2; ++s) {
                int prev = -1;
                for (int w = 0; w < 3; ++w) {
                    const auto v = get(e, s, w);
                    if (!v) continue;
                    if (prev >= 0 && *v < *get(e, s, prev))
                        out.push_back(std::string(envName[e]) + "/" + seaName[s] + ": "
                                      + windName[w] + " (" + detail::fmt(*v) + ") < "
                                      + windName[prev] + " (" + detail::fmt(*get(e, s, prev)) + ")");
                    prev = w;
                }
            }
            for (int w = 0; w < 3; ++w) {
                const auto win = get(e, 0, w);
                const auto sum = get(e, 1, w);
                if (win && sum && *sum > *win)
                    out.push_back(std::string(envName[e]) + ": summer " + windName[w]
                                  + " (" + detail::fmt(*sum) + ") > winter "
                                  + windName[w] + " (" + detail::fmt(*win) + ")");
            }
        }
        return out;
    }

    std::string describe() const
    {
        return "Conductor(" + type + " " + codename + ", " + detail::fmt(area_mm2)
               + " mm2, " + stranding + ")";
    }
};

// ---- JSON ingestion ---------------------------------------------------------
namespace detail {

using json = nlohmann::json;

inline const json& emptyObject()
{
    static const json e = json::object();
    return e;
}

// o[key] if o is an object containing key, else an empty object.
inline const json& sub(const json& o, const char* key)
{
    if (!o.is_object()) return emptyObject();
    const auto it = o.find(key);
    return it != o.end() ? *it : emptyObject();
}

inline double num(const json& o, const char* key, double def = 0.0)
{
    if (!o.is_object()) return def;
    const auto it = o.find(key);
    return (it != o.end() && it->is_number()) ? it->get<double>() : def;
}

inline std::string str(const json& o, const char* key)
{
    if (!o.is_object()) return {};
    const auto it = o.find(key);
    return (it != o.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

// Rating value, or nullopt if absent or 0. acsr_ac.json uses 0 as "no data".
inline std::optional<double> ratingValue(const json& o, const char* key)
{
    if (!o.is_object()) return std::nullopt;
    const auto it = o.find(key);
    if (it == o.end() || !it->is_number()) return std::nullopt;
    const double v = it->get<double>();
    return v == 0.0 ? std::nullopt : std::optional<double>(v);
}

inline RatingCondition condition(const json& o)
{
    return { ratingValue(o, "still"), ratingValue(o, "wind_1ms"), ratingValue(o, "wind_2ms") };
}

inline bool has(const json& o, const char* key)
{
    return o.is_object() && o.find(key) != o.end();
}

// Layout is detected per record.
inline CurrentRating parseRating(const json& raw)
{
    CurrentRating r;
    if (has(raw, "rural")) {                       // aac / aaac: nested
        const json& ru = sub(raw, "rural");
        const json& in = sub(raw, "industrial");
        r.rural      = { condition(sub(ru, "winter")), condition(sub(ru, "summer")) };
        r.industrial = { condition(sub(in, "winter")), condition(sub(in, "summer")) };
    } else if (has(raw, "winter_night") || has(raw, "summer_noon")) {   // acsr_gz
        r.rural      = { condition(sub(raw, "winter_night")),
                         condition(sub(raw, "summer_noon")) };
        r.industrial = { condition(sub(raw, "industrial_winter")),
                         condition(sub(raw, "industrial_summer")) };
    } else {                                       // acsr_ac: flat
        r.rural      = { condition(sub(raw, "rural_winter")),
                         condition(sub(raw, "rural_summer")) };
        r.industrial = { condition(sub(raw, "industrial_winter")),
                         condition(sub(raw, "industrial_summer")) };
    }
    return r;
}

using RowIndex = std::unordered_map<int, const json*>;

inline RowIndex indexByConductorId(const json& data, const char* table)
{
    RowIndex m;
    const json& t = sub(data, table);
    if (t.is_array())
        for (const json& row : t)
            m[row.at("conductor_id").get<int>()] = &row;
    return m;
}

inline const json& lookup(const RowIndex& m, int id)
{
    const auto it = m.find(id);
    return it != m.end() ? *it->second : emptyObject();
}

} // namespace detail

// Load and normalise one conductor JSON file.
// Throws std::runtime_error (file name included) on I/O or JSON errors.
inline std::vector<Conductor> loadFile(const std::filesystem::path& path)
{
    using namespace detail;

    std::ifstream f(path);
    if (!f) throw std::runtime_error("Cannot open " + path.string());

    try {
        const json data = json::parse(f);

        const RowIndex mech   = indexByConductorId(data, "mechanical");
        const RowIndex elec   = indexByConductorId(data, "electrical");
        const RowIndex rating = indexByConductorId(data, "current_rating");

        std::vector<Conductor> out;
        const json& list = sub(data, "conductor");
        if (!list.is_array()) return out;

        for (const json& c : list) {
            const int   cid = c.at("id").get<int>();
            const json& m   = lookup(mech, cid);
            const json& e   = lookup(elec, cid);

            Conductor k;
            k.source_file         = path.filename().string();
            k.id                  = cid;
            k.type                = str(c, "type");
            k.codename            = str(c, "codename");
            k.stranding           = str(c, "stranding");
            k.overall_diameter_mm = num(c, "overall_diameter_mm");
            k.area_mm2            = num(c, "area_mm2");
            k.mass_kg_km          = num(m, "mass");
            k.breaking_load_kN    = num(m, "breaking_load");
            k.modulus_GPa         = num(m, "modulus");
            k.expansion_1e6       = num(m, "expansion");
            k.dc_resistance_20C   = num(e, "dc_resistance_20C");
            if (has(e, "dc_resistance_75C"))
                k.dc_resistance_75C = num(e, "dc_resistance_75C");
            k.ac_resistance_50Hz  = num(e, "ac_resistance_50Hz");
            k.reactance_50Hz      = num(e, "reactance_50Hz");
            k.rating              = parseRating(lookup(rating, cid));
            out.push_back(std::move(k));
        }
        return out;
    } catch (const json::exception& ex) {
        throw std::runtime_error(path.filename().string() + ": " + ex.what());
    }
}

// Load aaac_1120.json, aac.json, acsr_ac.json and acsr_gz.json from
// data_dir/conductors/. Missing files are skipped and reported through
// `warnings` (or std::cerr if null). Result is sorted by type, then area.
inline std::vector<Conductor> loadAll(const std::filesystem::path& data_dir,
                                      std::vector<std::string>* warnings = nullptr)
{
    static const char* files[] = {"aaac_1120.json", "aac.json",
                                  "acsr_ac.json",   "acsr_gz.json"};
    const std::filesystem::path base = data_dir / "conductors";

    std::vector<Conductor> all;
    for (const char* name : files) {
        const std::filesystem::path p = base / name;
        if (std::filesystem::exists(p)) {
            auto v = loadFile(p);
            all.insert(all.end(), std::make_move_iterator(v.begin()),
                                  std::make_move_iterator(v.end()));
        } else {
            const std::string msg = "Warning: " + p.string() + " not found, skipping.";
            if (warnings) warnings->push_back(msg);
            else          std::cerr << msg << '\n';
        }
    }
    std::stable_sort(all.begin(), all.end(), [](const Conductor& a, const Conductor& b) {
        return a.type != b.type ? a.type < b.type : a.area_mm2 < b.area_mm2;
    });
    return all;
}

// ---- Lookup -----------------------------------------------------------------
// Unset fields match anything. Strings compare case-insensitively (exact
// otherwise), area_mm2 within 1e-6.
struct Filter {
    std::optional<std::string> source_file, type, codename, stranding;
    std::optional<int>         id;
    std::optional<double>      area_mm2;
};

namespace detail {
inline bool iequals(const std::string& a, const std::string& b)
{
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
               return std::tolower(x) == std::tolower(y);
           });
}
} // namespace detail

// Pointers refer into `all` and are valid while it is alive and unmodified.
inline std::vector<const Conductor*> find(const std::vector<Conductor>& all,
                                          const Filter& f)
{
    std::vector<const Conductor*> out;
    for (const Conductor& c : all) {
        if (f.source_file && !detail::iequals(c.source_file, *f.source_file)) continue;
        if (f.type        && !detail::iequals(c.type,        *f.type))        continue;
        if (f.codename    && !detail::iequals(c.codename,    *f.codename))    continue;
        if (f.stranding   && !detail::iequals(c.stranding,   *f.stranding))   continue;
        if (f.id          && c.id != *f.id)                                   continue;
        if (f.area_mm2    && std::abs(c.area_mm2 - *f.area_mm2) > 1e-6)       continue;
        out.push_back(&c);
    }
    return out;
}

// First conductor with this codename (case-insensitive), or nullptr.
inline const Conductor* findByCodename(const std::vector<Conductor>& all,
                                       const std::string& codename)
{
    Filter f;
    f.codename = codename;
    const auto r = find(all, f);
    return r.empty() ? nullptr : r.front();
}

} // namespace conductors
} // namespace LineTool
