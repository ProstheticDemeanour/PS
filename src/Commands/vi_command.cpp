// vi command: compute electrical quantities from whatever you know.
#include "vi_command.hpp"

#include <iostream>
#include <string>
#include <vector>

using namespace LineTool;

std::string vi_command::getName() const {
    return "vi";
}

std::string vi_command::getDescription () const {
    return "compute electrical parameters from known quantities";
}

void vi_command::showHelp() const {
    std::cout <<
        "compute electrical quantities from whatever you know\n"
        "\n"
        "USAGE\n"
        "  $ ps vi [VALUE...] [FLAGS]\n"
        "\n"
        "VALUES\n"
        "  33kV      voltage, line-to-line for 3 phase      V kV MV\n"
        "  4.4A      current                                A kA mA\n"
        "  250kVA    apparent power                         VA kVA MVA\n"
        "  100kW     active power                           W kW MW\n"
        "  50kvar    reactive power, negative is leading    var kvar Mvar\n"
        "  50ohm     impedance per phase                    ohm kohm Mohm\n"
        "  0.9pf     power factor, add lag or lead          0.9lead\n"
        "  30deg     phase angle, positive is lagging\n"
        "  R=2ohm    named form, also X= Z= V= I= S= P= Q= pf= angle=\n"
        "  if=20kA   three phase fault current, with voltage gives fault level (MVA)\n"
        "            and fault impedance; also sf=1000MVA and zf=1ohm\n"
        "  vln=19kV  line-to-neutral voltage (multiplied by sqrt(3) to line-to-line)\n"
        "\n"
        "  prefixes are k, M and G. m means mega for W, VA and var, milli for V, A\n"
        "  and ohm\n"
        "\n"
        "FLAGS\n"
        "  --phases=<1|3>  number of phases [default: 3]\n"
        "  --digits=<n>    significant digits shown [default: 5]\n"
        "  --if=<current>  fault current, same as if=<current>\n"
        "  --sf=<power>    fault level, same as sf=<power>\n"
        "  --zf=<ohm>      fault impedance, same as zf=<ohm>\n"
        "  --json          output json\n"
        "  --terse         output tab separated values without a header\n"
        "  --ascii         use ascii symbols instead of unicode\n"
        "  --no-color      disable color\n"
        "  -h, --help      show this help\n"
        "\n"
        "EXAMPLES\n"
        "  $ ps vi 33kV 250kVA\n"
        "  $ ps vi 11kV 100A 0.9pf\n"
        "  $ ps vi 230V 10A --phases=1\n"
        "  $ ps vi 33kV --if 20kA\n"
        << std::endl;
}

// "--name=value" or "--name value". Returns true if arg is that flag.
static bool flagValue(const std::string& arg, const char* name, std::size_t& i,
                      const std::vector<std::string>& args, std::string& out, bool& missing)
{
    const std::string n = name;
    if (arg == n) {
        if (i + 1 >= args.size()) { missing = true; return true; }
        out = args[++i];
        return true;
    }
    if (arg.rfind(n + "=", 0) == 0) { out = arg.substr(n.size() + 1); return true; }
    return false;
}

int vi_command::execute(const std::vector<std::string>& args)
{
    print::Style st;
    ElectricalParams p;
    std::vector<std::string> tokens;
    int digits = 5;
    bool help = false;
    std::vector<std::string> errors;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string a = args[i];
        std::string v;
        bool missing = false;

        if (a == "-h" || a == "--help")      { help = true; continue; }
        if (a == "--json")                   { st.format = print::Format::Json; continue; }
        if (a == "--terse")                  { st.format = print::Format::Terse; continue; }
        if (a == "--ascii")                  { st.sym = print::Symbols::ascii(); continue; }
        if (a == "--no-color")               { st.color = print::ColorMode::Never; continue; }
        if (flagValue(a, "--phases", i, args, v, missing)) {
            if (missing) errors.push_back("--phases needs a value");
            else if (v == "1") { p.phases = 1; p.phasesSource = Source::Given; }
            else if (v == "3") { p.phases = 3; p.phasesSource = Source::Given; }
            else errors.push_back("--phases must be 1 or 3");
            continue;
        }
        bool faultFlag = false;
        for (const char* nm : {"if", "sf", "zf"}) {
            if (flagValue(a, (std::string("--") + nm).c_str(), i, args, v, missing)) {
                if (missing) errors.push_back(std::string("--") + nm + " needs a value");
                else tokens.push_back(std::string(nm) + "=" + v);
                faultFlag = true;
                break;
            }
        }
        if (faultFlag) continue;
        if (flagValue(a, "--digits", i, args, v, missing)) {
            const int n = missing ? 0 : std::atoi(v.c_str());
            if (n < 1 || n > 12) errors.push_back("--digits must be between 1 and 12");
            else digits = n;
            continue;
        }
        if (a.rfind("--", 0) == 0) { errors.push_back("unknown flag: " + a); continue; }
        tokens.push_back(a);
    }

    if (!errors.empty()) {
        for (const auto& e : errors) print::error(e, st);
        return 1;
    }
    if (help || tokens.empty()) { showHelp(); return 0; }

    for (const auto& t : tokens) {
        const std::string err = quickcalc::parseToken(p, t);
        if (!err.empty()) errors.push_back(err);
    }
    if (!errors.empty()) {
        for (const auto& e : errors) print::error(e, st);
        return 1;
    }

    const quickcalc::Report rep = quickcalc::solve(p);

    quickcalc::makeGrid(p, st, digits).print(std::cout);

    if (st.format == print::Format::Human && !rep.notComputed.empty()) {
        std::string list;
        for (const auto& n : rep.notComputed) list += (list.empty() ? "" : ", ") + n;
        std::cout << "\nnot determined: " << list;
        if (!rep.hint.empty()) std::cout << " (" << rep.hint << ")";
        std::cout << '\n';
    }

    for (const auto& c : rep.conflicts) print::warning(c, st);
    if (rep.assumedLagging)
        print::warning("lag or lead not given, assumed lagging (inductive); "
                       "add lag or lead to the pf, or a signed Q, X or angle", st);
    return 0;
}
