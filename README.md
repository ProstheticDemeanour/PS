# PS

A collection of command line utilities I have developed over my carrier to help with everyday electrical engineering. Written in C++.

Most of the core sits in single include header libraries that I have grown over time. 

# Key Headers

## TransmissionLine.hpp

Single-header C++17 library for positive-sequence parameters of overhead transmission lines, single or dual circuit. Given phase conductor coordinates and conductor data, it returns L, C, X, B, Zc, γ, SIL, line charging, ABCD constants and an approximate loadability. 

### Integration

```cpp
#include "TransmissionLine.hpp"
using namespace LineTool;
```

Requires C++17 (`inline constexpr`). The header is ASCII only and does not rely on `M_PI`, so it builds on GCC, Clang and MSVC without extra defines. Invalid geometry throws `std::invalid_argument`.

### Usage

Single circuit:

```cpp
GeometryInput g;
g.x1 = -4.0; g.y1 = 10.0;    // phase A (m)
g.x2 =  0.0; g.y2 = 12.0;    // phase B
g.x3 =  4.0; g.y3 = 10.0;    // phase C
g.DS          = 0.0117;      // conductor GMR (m)
g.radius_m    = 0.0141;      // conductor outer radius (m)
g.bundleNo    = 2;
g.bundleSpace = 0.40;        // adjacent sub-conductor spacing (m)
g.r_ac        = 0.05;        // ohm/km per phase
g.freq        = 50.0;
g.lengthKm    = 100.0;
g.voltageKV   = 220.0;       // line-to-line
g.currentA    = 500.0;

LineResults r = TransmissionLine::compute(g);
```

Dual circuit (circuit 2 phases are `x4/y4` = A2, `x5/y5` = B2, `x6/y6` = C2; both circuits use the same conductor and bundle):

```cpp
DualCircuitInput d;          // inherits GeometryInput
d.bundleNo = 2;
DualCircuitResults q = TransmissionLine::computeDual(d);
```

The lower-level functions (`calcInductance`, `calcCapacitance`, `rlgcToKZc`, `kZcToRlgc`, `hybrid`, `sil_MVA`, `chargingMVAR`, `gmrBundle`, `radiusBundle`) are public static members and can be used on their own.

### Units

| Quantity | Unit |
|---|---|
| Coordinates, DS, radius, bundle spacing | m |
| Line length | km |
| L / C / R / X / B (per unit length) | mH/km, uF/km, ohm/km, ohm/km, S/km |
| Voltage | kV line-to-line |
| Current | A |
| SIL, charging, loadability | MVA, MVAR, MW |
| ABCD angles | degrees |

`LineRLGC` uses SI base units per km (H/km, F/km). `LineKZc::k` is the complex propagation constant γ = α + jβ in 1/km.

### Method

Inductance and capacitance are per-phase, positive-sequence, and assume a fully transposed line:

- L = 0.2 ln(GMD / GMR_b) mH/km
- C = 0.0556 / ln(GMD / r_b) uF/km

GMD is the geometric mean of the three phase spacings. GMR_b and r_b are the equivalent GMR (from `DS`) and equivalent radius (from `radius_m`) of a regular n-gon bundle:

- GMR_b = (n · DS · R^(n-1))^(1/n), with R = s / (2 sin(π/n))

This is exact for any n and reduces to the usual expressions for n = 2, 3 and 4. Inductance uses the GMR, capacitance uses the outer radius. If `radius_m` is left at 0 it falls back to DS / 0.7788, which is only valid for a solid round conductor, so set it for stranded conductors.

Series impedance is z = r_ac + jωL and shunt admittance is y = jωC (g = 0). From these:

- γ = sqrt(zy)
- Zc = sqrt(z/y)
- ABCD = [cosh γl, Zc sinh γl; sinh γl / Zc, cosh γl]

Other outputs:

- SIL = V² / Re(Zc)
- Charging = V²ωC·l, with V line-to-line and C per phase
- Loadability is P ≈ (|Vs|/Vn)(|Vr|/Vn) · SIL · sin δ / sin βl, where δ = arg(Vs) is found from the ABCD constants with the receiving end at Vn = V/√3 carrying `currentA` at unity power factor.

Dual circuits are reduced to a single equivalent line with both circuits in parallel. Each phase group (for example A1 and A2) is treated as a two-position bundle at the inter-circuit spacing, and GMD is taken over the four distances between each pair of phase groups. Conventions for the dual-circuit results:

- `r_ac` and `currentA` are per circuit, and the equivalent line uses r_ac / 2.
- `Zc_ohm`, `charging_MVAR` and the ABCD constants refer to the parallel equivalent (total).
- `SIL_total_MVA` = V² / Zc, and `SIL_MVA` is half of that.
- `loadability_MW` is per circuit.

### Assumptions and limitations

- Positive sequence only. Zero-sequence and mutual coupling data are not produced.
- Fully transposed conductors. No treatment of untransposed asymmetry.
- L uses the free-space GMD/GMR form, with no earth-return correction. C ignores the ground plane, so it does not account for conductor height or sag.
- Overhead earth wire inputs (`hasOhew*`, `xo*`, `yo*`) are present in the struct but not used.
- `r_ac` is a user-supplied constant. There is no temperature, skin-effect or stranding model, and no corona.
- Phase-to-phase distances below 0.1 m are clamped to 0.1 m.
- `quarterWaveKm()` is the free-space value c/4f. The line-specific quarter-wave length is (π/2)/β.
- The loadability figure is an estimate. It is not a substitute for a thermal, voltage-drop or stability study.

### Checks

For the 100 km, 220 kV, 2-conductor bundle default geometry, the library returns L = 0.875 mH/km, C = 0.0131 uF/km, Zc ≈ 260 ohm, SIL ≈ 186 MVA, |A| = 0.9944, and AD - BC = 1.000000. The reciprocity check AD - BC = 1 should hold for any input.