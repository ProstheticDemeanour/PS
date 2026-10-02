// Structs.hpp
// Universal struct header: plain data types shared across the PowerSystems
// tools. Data only, no behaviour beyond trivial accessors. Add new structs
// below ElectricalParams as the toolset grows.
//
// Conventions: SI base units unless a field name says otherwise, angles in
// degrees, 3 phase quantities balanced.
#pragma once

namespace LineTool {

// ---- Generic building blocks --------------------------------------------------
enum class Source { Unset, Given, Derived, Assumed, Default };

// A value plus where it came from. known() is false until set.
struct Quantity {
    double value  = 0.0;
    Source source = Source::Unset;

    bool known() const { return source != Source::Unset; }
    void set(double v, Source s) { value = v; source = s; }
    void clear() { value = 0.0; source = Source::Unset; }
};

// Lag/lead of the load. Lagging = inductive (positive Q, positive angle).
enum class PfSense { Unknown, Lagging, Leading };

// ---- ElectricalParams ---------------------------------------------------------
// One balanced circuit point described by every quantity the quick calculator
// understands. Fill in what you know, leave the rest unset.
//
//   3 phase: voltage is line-to-line, current is line current, apparent/active/
//            reactive are total (three phase) power.
//   1 phase: voltage and current are as measured, power is the total.
//   impedance, resistance, reactance are per phase (star equivalent), so
//   impedance = voltage^2 / apparent in both cases.
struct ElectricalParams {
    int    phases       = 3;                 // 1 or 3
    Source phasesSource = Source::Default;

    Quantity voltage;     // V
    Quantity current;     // A
    Quantity apparent;    // VA
    Quantity active;      // W
    Quantity reactive;    // var, positive = lagging (inductive)
    Quantity pf;          // 0..1 (magnitude, see sense)
    Quantity angle;       // degrees, positive = lagging
    Quantity impedance;   // ohm per phase, |Z|
    Quantity resistance;  // ohm per phase
    Quantity reactance;   // ohm per phase, positive = inductive

    // Three phase symmetrical fault at the same point (shares `voltage`).
    //   faultLevel = sqrt(3) * voltage * faultCurrent      (voltage is L-L)
    //   faultImpedance = (voltage / sqrt(3)) / faultCurrent (L-N voltage / If)
    Quantity faultCurrent;    // A
    Quantity faultLevel;      // VA
    Quantity faultImpedance;  // ohm per phase, source impedance

    PfSense sense = PfSense::Unknown;
};

} // namespace LineTool
