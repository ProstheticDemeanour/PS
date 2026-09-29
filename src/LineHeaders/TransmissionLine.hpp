// TransmissionLine.hpp
// Single-header transmission line parameter library (C++17).
//
// Usage:
//   #include "TransmissionLine.hpp"
//   auto r = LineTool::TransmissionLine::compute(geom);
//
// Everything is inline; no .cpp to compile or link. ASCII only (MSVC safe).
// Derived from core/TransmissionLine.{h,cpp} with the following corrections:
//   - ABCD uses cosh/sinh of the complex propagation constant
//   - dual-circuit beta taken from Im(gamma)
//   - dual-circuit SIL: SIL_total = V^2/Zc(parallel), SIL_MVA = per circuit
//   - capacitance uses conductor outer radius, not GMR (see radius_m)
//   - bundles of n conductors handled as regular n-gons (any n >= 1)
//   - dual-circuit series resistance halved (r_ac is per circuit)
//
// Units: lengths m (geometry) / km (line), L mH/km, C uF/km, R ohm/km,
//        V kV line-to-line, I A, results in MVA / MVAR / MW.
#pragma once

#include <cmath>
#include <complex>
#include <stdexcept>

namespace LineTool {

// ---- Constants --------------------------------------------------------------
inline constexpr double kPi           = 3.14159265358979323846;
inline constexpr double kSpeedOfLight = 299'792'458.0;
inline constexpr double kMu0          = 4e-7 * kPi;
inline constexpr double kEps0         = 8.854187817e-12;
inline constexpr double kTwoPi        = 2.0 * kPi;

using cdouble = std::complex<double>;

// ---- Data types -------------------------------------------------------------
struct LineRLGC {
    double r = 0;   // ohm/km
    double l = 0;   // H/km
    double g = 0;   // S/km
    double c = 0;   // F/km
};

struct LineKZc {
    cdouble k  = 0;   // propagation constant gamma = alpha + j*beta, per km
    cdouble Zc = 0;   // characteristic impedance, ohm
};

struct HybridParams {
    cdouble A, B, C, D;
};

struct LineResults {
    double inductance_mH_km   = 0;
    double capacitance_uF_km  = 0;
    double admittance_S       = 0;
    double reactance_ohm_km   = 0;
    double susceptance_S_km   = 0;
    double Zc_ohm             = 0;
    double k_rad_km           = 0;
    double vel_factor         = 0;
    double SIL_MVA            = 0;
    double quarter_wave_km    = 0;
    double charging_MVAR      = 0;
    double loadability_MW     = 0;
    double A_mag=0, B_mag=0, C_mag=0, D_mag=0;
    double A_ang=0, B_ang=0, C_ang=0, D_ang=0;   // degrees
};

struct GeometryInput {
    double x1 = -4.0, y1 = 10.0;
    double x2 =  0.0, y2 = 12.0;
    double x3 =  4.0, y3 = 10.0;
    bool   hasOhew1 = false;
    double xo1 = -3.0, yo1 = 16.0;
    bool   hasOhew2 = false;
    double xo2 =  3.0, yo2 = 16.0;
    double DS          = 0.0117;   // conductor GMR, m (inductance)
    double radius_m    = 0.0;      // conductor outer radius, m (capacitance).
                                   // <= 0 falls back to DS / exp(-1/4), i.e.
                                   // solid round conductor. Set it for stranded.
    int    bundleNo    = 1;        // regular n-gon bundle
    double bundleSpace = 0.40;     // adjacent sub-conductor spacing, m
    double r_ac      = 0.05;       // ohm/km per phase (per circuit for dual)
    double freq      = 50.0;       // Hz
    double lengthKm  = 100.0;
    double voltageKV = 220.0;
    double currentA  = 500.0;
};

// Circuit 2 phases: x4/y4 = A2, x5/y5 = B2, x6/y6 = C2.
// Both circuits assumed to have the same conductor and bundle.
//
// Equivalent positive-sequence parameters (circuits in parallel):
//   DS_b = gmrBundle(DS, bundleNo, bundleSpace)
//   GMRa = sqrt(DS_b * Da1a2), likewise GMRb, GMRc
//   GMRl = cbrt(GMRa * GMRb * GMRc)
//   Dab  = (Da1b1 * Da1b2 * Da2b1 * Da2b2)^(1/4), likewise Dbc, Dac
//   GMD  = cbrt(Dab * Dbc * Dac)
//   L    = 0.2 * ln(GMD / GMRl)   mH/km
//   C    = 0.0556 / ln(GMD / Req) uF/km, Req built like GMRl using the
//          conductor outer radius in place of DS.
// r_ac is the per-circuit resistance; the parallel equivalent uses r_ac / 2.
// currentA is per circuit.
struct DualCircuitInput : public GeometryInput {
    double x4 =  4.0, y4 = 10.0;
    double x5 =  0.0, y5 = 12.0;
    double x6 = -4.0, y6 = 10.0;
};

struct DualCircuitResults {
    // Geometry intermediates
    double GMD_m   = 0;
    double GMRl_m  = 0;
    double Req_m   = 0;   // equivalent radius for capacitance
    double DSb_m   = 0;
    double Da1a2_m = 0;
    double Db1b2_m = 0;
    double Dc1c2_m = 0;
    // Per unit length (both circuits in parallel)
    double inductance_mH_km  = 0;
    double capacitance_uF_km = 0;
    double reactance_ohm_km  = 0;
    double susceptance_S_km  = 0;
    // Characteristic
    double Zc_ohm     = 0;
    double k_rad_km   = 0;
    double vel_factor = 0;
    // Power
    double SIL_MVA         = 0;   // per circuit
    double SIL_total_MVA   = 0;   // both circuits
    double quarter_wave_km = 0;
    double charging_MVAR   = 0;   // total, both circuits
    double loadability_MW  = 0;   // per circuit
    // ABCD
    double A_mag=0, B_mag=0, C_mag=0, D_mag=0;
    double A_ang=0, B_ang=0, C_ang=0, D_ang=0;   // degrees
};

// ---- TransmissionLine -------------------------------------------------------
class TransmissionLine
{
public:
    // Equivalent GMR of a regular n-gon bundle, adjacent spacing bundleSpace (m).
    // GMR_b = (n * DS * R^(n-1))^(1/n),  R = s / (2 sin(pi/n)).
    // Reduces to sqrt(DS*s), cbrt(DS*s^2), 1.0905*(DS*s^3)^(1/4) for n = 2, 3, 4.
    static double gmrBundle(double DS, int bundleNo, double bundleSpace)
    {
        return bundleEquiv(DS, bundleNo, bundleSpace);
    }

    // Equivalent radius of the same bundle for capacitance (m).
    static double radiusBundle(double radius, int bundleNo, double bundleSpace)
    {
        return bundleEquiv(radius, bundleNo, bundleSpace);
    }

    // L = 0.2 ln(GMD / GMR_bundle)   [mH/km]
    static double calcInductance(double D12, double D23, double D13,
                                 double DS, int bundleNo, double bundleSpace)
    {
        const double GMD = gmd(D12, D23, D13);
        const double DSL = gmrBundle(DS, bundleNo, bundleSpace);
        if (DSL <= 0 || GMD <= 0)
            throw std::invalid_argument("Distances must be positive");
        return 0.2 * std::log(GMD / DSL);
    }

    // C = 0.0556 / ln(GMD / r_bundle)   [uF/km]
    // radius = conductor OUTER radius (m), not GMR.
    static double calcCapacitance(double D12, double D23, double D13,
                                  double radius, int bundleNo, double bundleSpace)
    {
        const double GMD = gmd(D12, D23, D13);
        const double rb  = radiusBundle(radius, bundleNo, bundleSpace);
        if (rb <= 0 || GMD <= 0 || GMD <= rb)
            throw std::invalid_argument("Distances must be positive");
        return 0.0556 / std::log(GMD / rb);
    }

    // RLGC (per km, SI base units) to gamma and Zc.
    static LineKZc rlgcToKZc(const LineRLGC& q, double freq)
    {
        const double omega = kTwoPi * freq;
        const cdouble x(q.r, omega * q.l);   // series impedance per km
        const cdouble y(q.g, omega * q.c);   // shunt admittance per km

        cdouble k  = std::sqrt(x * y);       // gamma = alpha + j*beta
        cdouble Zc = std::sqrt(x / y);
        if (Zc.real() < 0) Zc = -Zc;
        if (k.real()  < 0) k  = -k;
        return {k, Zc};
    }

    static LineRLGC kZcToRlgc(const LineKZc& kzc, double freq)
    {
        const double omega = kTwoPi * freq;
        const cdouble x = kzc.k * kzc.Zc;
        const cdouble y = kzc.k / kzc.Zc;

        LineRLGC q;
        q.r = x.real();
        q.l = x.imag() / omega;
        q.g = y.real();
        q.c = y.imag() / omega;
        return q;
    }

    // ABCD for a uniform line of length d_km. gamma is the complex propagation
    // constant per km (as returned in LineKZc::k).
    static HybridParams hybrid(cdouble gamma, cdouble Zc, double d_km)
    {
        const cdouble gl = gamma * d_km;
        HybridParams p;
        p.A = std::cosh(gl);
        p.B = Zc * std::sinh(gl);
        p.C = std::sinh(gl) / Zc;
        p.D = p.A;
        return p;
    }

    // Surge impedance loading, MVA.
    static double sil_MVA(double voltage_kV, double Zc_ohm)
    {
        const double V = voltage_kV * 1e3;
        return (V * V / Zc_ohm) * 1e-6;
    }

    // Quarter-wave length, km.
    static double quarterWaveKm(double freq)
    {
        return (kSpeedOfLight / freq) / 4.0 / 1000.0;
    }

    // Total line charging, MVAR: Q = V^2 * w * C_total (V line-to-line).
    static double chargingMVAR(double voltage_kV, double C_uF_km,
                               double length_km,  double freq)
    {
        const double V     = voltage_kV * 1e3;
        const double C_tot = C_uF_km * 1e-6 * length_km;
        return (V * V * kTwoPi * freq * C_tot) * 1e-6;
    }

    // Single circuit.
    static LineResults compute(const GeometryInput& g)
    {
        LineResults res;

        auto dist = [](double xa, double ya, double xb, double yb) {
            return std::hypot(xb - xa, yb - ya);
        };
        double D12 = dist(g.x1, g.y1, g.x2, g.y2);
        double D23 = dist(g.x2, g.y2, g.x3, g.y3);
        double D13 = dist(g.x1, g.y1, g.x3, g.y3);

        if (D12 < 0.1) D12 = 0.1;
        if (D23 < 0.1) D23 = 0.1;
        if (D13 < 0.1) D13 = 0.1;

        res.inductance_mH_km  = calcInductance (D12, D23, D13, g.DS, g.bundleNo, g.bundleSpace);
        res.capacitance_uF_km = calcCapacitance(D12, D23, D13,
                                                resolveRadius(g.radius_m, g.DS),
                                                g.bundleNo, g.bundleSpace);

        const double omega = kTwoPi * g.freq;
        res.reactance_ohm_km = omega * res.inductance_mH_km  * 1e-3;
        res.susceptance_S_km = omega * res.capacitance_uF_km * 1e-6;

        LineRLGC rlgc;
        rlgc.r = g.r_ac;
        rlgc.l = res.inductance_mH_km  * 1e-3;
        rlgc.g = 0.0;
        rlgc.c = res.capacitance_uF_km * 1e-6;

        const LineKZc kzc = rlgcToKZc(rlgc, g.freq);
        res.Zc_ohm   = kzc.Zc.real();
        res.k_rad_km = kzc.k.imag();   // beta

        if (res.k_rad_km > 0)
            res.vel_factor = (omega / res.k_rad_km) / kSpeedOfLight * 1e3;

        if (res.Zc_ohm > 0)
            res.SIL_MVA = sil_MVA(g.voltageKV, res.Zc_ohm);

        res.quarter_wave_km = quarterWaveKm(g.freq);
        res.admittance_S    = res.susceptance_S_km * g.lengthKm;
        res.charging_MVAR   = chargingMVAR(g.voltageKV, res.capacitance_uF_km,
                                           g.lengthKm, g.freq);

        const HybridParams abcd = hybrid(kzc.k, kzc.Zc, g.lengthKm);
        fillAbcd(res, abcd);

        res.loadability_MW = loadability(abcd, g.voltageKV, g.currentA,
                                         res.k_rad_km, g.lengthKm,
                                         res.Zc_ohm, res.SIL_MVA);
        return res;
    }

    // Dual circuit (equivalent parallel positive-sequence).
    static DualCircuitResults computeDual(const DualCircuitInput& g)
    {
        DualCircuitResults res;

        auto d = [](double xa, double ya, double xb, double yb) {
            const double v = std::hypot(xb - xa, yb - ya);
            return v < 0.1 ? 0.1 : v;
        };

        // Bundle GMR
        res.DSb_m = gmrBundle(g.DS, g.bundleNo, g.bundleSpace);

        // Same-phase inter-circuit distances
        res.Da1a2_m = d(g.x1, g.y1, g.x4, g.y4);
        res.Db1b2_m = d(g.x2, g.y2, g.x5, g.y5);
        res.Dc1c2_m = d(g.x3, g.y3, g.x6, g.y6);

        const double GMRa = std::sqrt(res.DSb_m * res.Da1a2_m);
        const double GMRb = std::sqrt(res.DSb_m * res.Db1b2_m);
        const double GMRc = std::sqrt(res.DSb_m * res.Dc1c2_m);
        res.GMRl_m = std::cbrt(GMRa * GMRb * GMRc);

        // Group GMDs
        const double Da1b1 = d(g.x1, g.y1, g.x2, g.y2);
        const double Da1b2 = d(g.x1, g.y1, g.x5, g.y5);
        const double Da2b1 = d(g.x4, g.y4, g.x2, g.y2);
        const double Da2b2 = d(g.x4, g.y4, g.x5, g.y5);
        const double Dab   = std::pow(Da1b1 * Da1b2 * Da2b1 * Da2b2, 0.25);

        const double Db1c1 = d(g.x2, g.y2, g.x3, g.y3);
        const double Db1c2 = d(g.x2, g.y2, g.x6, g.y6);
        const double Db2c1 = d(g.x5, g.y5, g.x3, g.y3);
        const double Db2c2 = d(g.x5, g.y5, g.x6, g.y6);
        const double Dbc   = std::pow(Db1c1 * Db1c2 * Db2c1 * Db2c2, 0.25);

        const double Da1c1 = d(g.x1, g.y1, g.x3, g.y3);
        const double Da1c2 = d(g.x1, g.y1, g.x6, g.y6);
        const double Da2c1 = d(g.x4, g.y4, g.x3, g.y3);
        const double Da2c2 = d(g.x4, g.y4, g.x6, g.y6);
        const double Dac   = std::pow(Da1c1 * Da1c2 * Da2c1 * Da2c2, 0.25);

        res.GMD_m = std::cbrt(Dab * Dbc * Dac);

        if (res.GMRl_m <= 0 || res.GMD_m <= 0)
            throw std::invalid_argument("Degenerate dual-circuit geometry");

        // Equivalent radius for capacitance (outer radius in place of GMR)
        const double rb   = radiusBundle(resolveRadius(g.radius_m, g.DS),
                                         g.bundleNo, g.bundleSpace);
        const double Reqa = std::sqrt(rb * res.Da1a2_m);
        const double Reqb = std::sqrt(rb * res.Db1b2_m);
        const double Reqc = std::sqrt(rb * res.Dc1c2_m);
        res.Req_m = std::cbrt(Reqa * Reqb * Reqc);

        if (res.GMD_m <= res.GMRl_m || res.GMD_m <= res.Req_m)
            throw std::invalid_argument("Degenerate dual-circuit geometry");

        res.inductance_mH_km  = 0.2   * std::log(res.GMD_m / res.GMRl_m);
        res.capacitance_uF_km = 0.0556 / std::log(res.GMD_m / res.Req_m);

        const double omega = kTwoPi * g.freq;
        res.reactance_ohm_km = omega * res.inductance_mH_km  * 1e-3;
        res.susceptance_S_km = omega * res.capacitance_uF_km * 1e-6;

        LineRLGC rlgc;
        rlgc.r = 0.5 * g.r_ac;   // two circuits in parallel
        rlgc.l = res.inductance_mH_km  * 1e-3;
        rlgc.g = 0.0;
        rlgc.c = res.capacitance_uF_km * 1e-6;

        const LineKZc kzc = rlgcToKZc(rlgc, g.freq);
        res.Zc_ohm   = kzc.Zc.real();
        res.k_rad_km = kzc.k.imag();   // beta

        if (res.k_rad_km > 0)
            res.vel_factor = (omega / res.k_rad_km) / kSpeedOfLight * 1e3;

        // Zc is for the two circuits in parallel, so V^2/Zc is the total SIL.
        if (res.Zc_ohm > 0) {
            res.SIL_total_MVA = sil_MVA(g.voltageKV, res.Zc_ohm);
            res.SIL_MVA       = 0.5 * res.SIL_total_MVA;
        }
        res.quarter_wave_km = quarterWaveKm(g.freq);
        res.charging_MVAR   = chargingMVAR(g.voltageKV, res.capacitance_uF_km,
                                           g.lengthKm, g.freq);

        const HybridParams abcd = hybrid(kzc.k, kzc.Zc, g.lengthKm);
        fillAbcd(res, abcd);

        // Parallel-equivalent line carries 2 x currentA; report per circuit.
        res.loadability_MW = 0.5 * loadability(abcd, g.voltageKV, 2.0 * g.currentA,
                                               res.k_rad_km, g.lengthKm,
                                               res.Zc_ohm, res.SIL_total_MVA);
        return res;
    }

private:
    static double resolveRadius(double radius, double DS)
    {
        return radius > 0 ? radius : DS / std::exp(-0.25);
    }

    static double bundleEquiv(double x, int n, double s)
    {
        if (n <= 1) return x;
        if (s <= 0) throw std::invalid_argument("Bundle spacing must be positive");
        const double R = s / (2.0 * std::sin(kPi / n));
        return std::pow(n * x * std::pow(R, n - 1), 1.0 / n);
    }

    static double gmd(double D12, double D23, double D13)
    {
        return std::cbrt(D12 * D23 * D13);
    }

    template <class R>
    static void fillAbcd(R& res, const HybridParams& a)
    {
        constexpr double rad2deg = 180.0 / kPi;
        res.A_mag = std::abs(a.A);  res.A_ang = std::arg(a.A) * rad2deg;
        res.B_mag = std::abs(a.B);  res.B_ang = std::arg(a.B) * rad2deg;
        res.C_mag = std::abs(a.C);  res.C_ang = std::arg(a.C) * rad2deg;
        res.D_mag = std::abs(a.D);  res.D_ang = std::arg(a.D) * rad2deg;
    }

    // P ~ (|Vs|/Vn)(|Vr|/Vn) * SIL * sin(delta)/sin(beta*L), current as proxy.
    static double loadability(const HybridParams& abcd, double voltageKV,
                              double currentA, double beta, double lengthKm,
                              double Zc_ohm, double SIL_MVA)
    {
        const double  Vn     = voltageKV * 1e3 / std::sqrt(3.0);
        const cdouble Vr_ref(Vn, 0);
        const cdouble Ir(currentA, 0);
        const cdouble Vs     = abcd.A * Vr_ref + abcd.B * Ir;
        const double  delta  = std::arg(Vs);
        const double  beta_L = beta * lengthKm;
        if (std::abs(std::sin(beta_L)) > 1e-6 && Zc_ohm > 0)
            return (std::abs(Vs) / Vn) * (std::abs(Vr_ref) / Vn)
                   * SIL_MVA * (std::sin(delta) / std::sin(beta_L));
        return 0.0;
    }
};

} // namespace LineTool
