// The built-in symbol library. Coordinates are grid units (0.1 inch), y down;
// pin connection points sit on whole grid points so wires snap to them.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <mutex>

#include "wetwaresuite/circuit/circuit.hpp"

namespace wetwaresuite::circuit {

const char* toString(PinType type) {
    switch (type) {
        case PinType::Passive: return "passive";
        case PinType::Input: return "input";
        case PinType::Output: return "output";
        case PinType::Bidirectional: return "bidirectional";
        case PinType::PowerIn: return "power_in";
        case PinType::PowerOut: return "power_out";
        case PinType::OpenCollector: return "open_collector";
        case PinType::NotConnected: return "not_connected";
    }
    return "passive";
}

namespace {

constexpr double kPi = 3.14159265358979323846;

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

constexpr double kPiLib = 3.14159265358979323846;

struct B {
    Symbol s;
    B& line(std::vector<Pt> pts, bool closed = false, bool fill = false) {
        Shape sh;
        sh.kind = Shape::Poly;
        sh.pts = std::move(pts);
        sh.closed = closed;
        sh.fill = fill;
        s.shapes.push_back(std::move(sh));
        return *this;
    }
    B& circle(Pt c, double r, bool fill = false) {
        Shape sh;
        sh.kind = Shape::Circle;
        sh.pts = {c};
        sh.r = r;
        sh.fill = fill;
        s.shapes.push_back(std::move(sh));
        return *this;
    }
    B& rect(Pt a, Pt b) {
        Shape sh;
        sh.kind = Shape::Rect;
        sh.pts = {a, b};
        s.shapes.push_back(std::move(sh));
        return *this;
    }
    B& text(Pt at, std::string t, double size = 0.7) {
        Shape sh;
        sh.kind = Shape::Text;
        sh.pts = {at};
        sh.text = std::move(t);
        sh.size = size;
        s.shapes.push_back(std::move(sh));
        return *this;
    }
    B& pin(std::string number, std::string name, Pt pos, Pt end, PinType type = PinType::Passive) {
        s.pins.push_back({std::move(number), std::move(name), pos, end, type});
        return *this;
    }
    // arc from a0 to a1 (radians, y down so positive angles turn clockwise on screen)
    B& arc(Pt c, double r, double a0, double a1, int steps = 12) {
        std::vector<Pt> pts;
        for (int i = 0; i <= steps; ++i) {
            const double a = a0 + (a1 - a0) * i / steps;
            pts.push_back({c.x + r * std::cos(a), c.y + r * std::sin(a)});
        }
        return line(std::move(pts));
    }
    B& arrow(Pt from, Pt to, double head = 0.3) {
        line({from, to});
        const double a = std::atan2(to.y - from.y, to.x - from.x);
        return line({{to.x - head * std::cos(a - 0.45), to.y - head * std::sin(a - 0.45)},
                     to,
                     {to.x - head * std::cos(a + 0.45), to.y - head * std::sin(a + 0.45)}},
                    true, true);
    }
};

void computeBounds(Symbol& s) {
    double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
    auto add = [&](Pt p, double r = 0) {
        x0 = std::min(x0, p.x - r);
        y0 = std::min(y0, p.y - r);
        x1 = std::max(x1, p.x + r);
        y1 = std::max(y1, p.y + r);
    };
    for (const auto& sh : s.shapes) {
        if (sh.kind == Shape::Text) continue;
        for (const auto& p : sh.pts) add(p, sh.kind == Shape::Circle ? sh.r : 0);
    }
    if (x0 <= x1) s.bodyMinX = x0, s.bodyMinY = y0, s.bodyMaxX = x1, s.bodyMaxY = y1;
    for (const auto& p : s.pins) {
        add(p.pos);
        add(p.end);
    }
    if (x0 > x1) x0 = y0 = -1, x1 = y1 = 1;
    s.minX = x0, s.minY = y0, s.maxX = x1, s.maxY = y1;
}

Symbol twoPin(const std::string& name, const std::string& title, const std::string& prefix, const std::string& value,
              const std::string& footprint, double bodyHalf) {
    B b;
    b.s.name = name;
    b.s.title = title;
    b.s.category = "passive";
    b.s.prefix = prefix;
    b.s.value = value;
    b.s.footprint = footprint;
    b.pin("1", "1", {-2, 0}, {-bodyHalf, 0}).pin("2", "2", {2, 0}, {bodyHalf, 0});
    b.s.refPos = {0, -0.9};
    b.s.valuePos = {0, 1.5};
    return b.s;
}

Symbol resistor() {
    Symbol s = twoPin("resistor", "Resistor", "R", "10k", "R_0805", 1);
    B b{s};
    b.line({{-1, 0}, {-0.83, -0.4}, {-0.5, 0.4}, {-0.17, -0.4}, {0.17, 0.4}, {0.5, -0.4}, {0.83, 0.4}, {1, 0}});
    return b.s;
}

Symbol potentiometer() {
    Symbol s = twoPin("potentiometer", "Potentiometer (1, 3 = ends, 2 = wiper)", "RV", "10k", "Pot_THT", 1);
    B b{s};
    b.s.pins[1].number = "3";
    b.s.pins[1].name = "3";
    b.line({{-1, 0}, {-0.83, -0.4}, {-0.5, 0.4}, {-0.17, -0.4}, {0.17, 0.4}, {0.5, -0.4}, {0.83, 0.4}, {1, 0}});
    b.pin("2", "W", {0, -2}, {0, -1.2});
    b.arrow({0, -1.2}, {0, -0.45}, 0.3);
    b.s.refPos = {1.3, -1.2};
    b.s.valuePos = {0, 1.5};
    return b.s;
}

Symbol capacitor() {
    Symbol s = twoPin("capacitor", "Capacitor", "C", "100n", "C_0805", 0.25);
    B b{s};
    b.line({{-0.25, -0.8}, {-0.25, 0.8}}).line({{0.25, -0.8}, {0.25, 0.8}});
    b.s.refPos = {0, -1.1};
    return b.s;
}

Symbol capacitorPolarized() {
    Symbol s = twoPin("capacitor_polarized", "Polarized capacitor (pin 1 = +)", "C", "10u", "CP_THT", 0.25);
    B b{s};
    b.line({{-0.25, -0.8}, {-0.25, 0.8}});
    b.arc({1.35, 0}, 1.1, kPi - 0.8, kPi + 0.8);
    b.s.pins[1].end = {0.27, 0};
    b.text({-0.8, -0.45}, "+", 0.7);
    b.s.refPos = {0, -1.1};
    return b.s;
}

Symbol inductor() {
    Symbol s = twoPin("inductor", "Inductor", "L", "10u", "L_0805", 1.2);
    B b{s};
    for (int i = 0; i < 4; ++i) b.arc({-0.9 + 0.6 * i, 0}, 0.3, kPi, 2 * kPi, 8);
    b.s.refPos = {0, -0.8};
    b.s.valuePos = {0, 1.2};
    return b.s;
}

Symbol diode(bool led) {
    // KiCad numbering: pin 1 = cathode (right), pin 2 = anode (left)
    B b;
    b.s.name = led ? "led" : "diode";
    b.s.title = led ? "LED (1 = K, 2 = A)" : "Diode (1 = K, 2 = A)";
    b.s.category = "semiconductor";
    b.s.prefix = led ? "D" : "D";
    b.s.value = led ? "red" : "1N4148";
    b.s.footprint = led ? "LED_0805" : "D_SOD-123";
    b.pin("1", "K", {2, 0}, {0.5, 0}).pin("2", "A", {-2, 0}, {-0.5, 0});
    b.line({{-0.5, -0.5}, {-0.5, 0.5}, {0.5, 0}}, true, false);
    b.line({{0.5, -0.5}, {0.5, 0.5}});
    if (led) {
        b.arrow({-0.1, -0.7}, {0.4, -1.2}, 0.22);
        b.arrow({0.35, -0.6}, {0.85, -1.1}, 0.22);
        b.s.refPos = {-1.2, -0.9};
    } else {
        b.s.refPos = {0, -0.9};
    }
    b.s.valuePos = {0, 1.4};
    return b.s;
}

Symbol bjt(bool npn) {
    // pins match SOT-23 BJTs (BC847, MMBT3904): 1 = B, 2 = E, 3 = C
    B b;
    b.s.name = npn ? "npn" : "pnp";
    b.s.title = npn ? "NPN transistor (1 = B, 2 = E, 3 = C)" : "PNP transistor (1 = B, 2 = E, 3 = C)";
    b.s.category = "semiconductor";
    b.s.prefix = "Q";
    b.s.value = npn ? "BC847" : "BC857";
    b.s.footprint = "SOT-23";
    b.pin("1", "B", {-2, 0}, {-0.4, 0}).pin("2", "E", {1, 2}, {1, 1}).pin("3", "C", {1, -2}, {1, -1});
    b.circle({0.35, 0}, 1.2);
    b.line({{-0.4, -0.7}, {-0.4, 0.7}});
    b.line({{-0.4, -0.3}, {1, -1}});
    if (npn) {
        b.arrow({-0.4, 0.3}, {1, 1}, 0.35);
    } else {
        b.line({{-0.4, 0.3}, {1, 1}});
        b.arrow({0.6, 0.8}, {-0.3, 0.35}, 0.35);
    }
    b.s.refPos = {2.3, -0.3};
    b.s.valuePos = {2.3, 0.7};
    return b.s;
}

Symbol mosfet(bool nch) {
    // pins match SOT-23 MOSFETs (2N7002, AO3400): 1 = G, 2 = S, 3 = D
    B b;
    b.s.name = nch ? "nmos" : "pmos";
    b.s.title = nch ? "N-channel MOSFET (1 = G, 2 = S, 3 = D)" : "P-channel MOSFET (1 = G, 2 = S, 3 = D)";
    b.s.category = "semiconductor";
    b.s.prefix = "Q";
    b.s.value = nch ? "2N7002" : "AO3401";
    b.s.footprint = "SOT-23";
    b.pin("1", "G", {-2, 0}, {-0.6, 0}).pin("2", "S", {1, 2}, {1, 0.6}).pin("3", "D", {1, -2}, {1, -0.6});
    b.circle({0.3, 0}, 1.25);
    b.line({{-0.6, -0.6}, {-0.6, 0.6}});
    b.line({{-0.3, -0.8}, {-0.3, -0.4}}).line({{-0.3, -0.2}, {-0.3, 0.2}}).line({{-0.3, 0.4}, {-0.3, 0.8}});
    b.line({{-0.3, -0.6}, {1, -0.6}});
    b.line({{-0.3, 0.6}, {1, 0.6}});
    b.line({{1, 0}, {1, 0.6}});
    if (nch) b.arrow({1, 0}, {-0.25, 0}, 0.35);
    else b.arrow({-0.3, 0}, {0.95, 0}, 0.35);
    b.s.refPos = {2.4, -0.3};
    b.s.valuePos = {2.4, 0.7};
    return b.s;
}

Symbol opamp() {
    // pin numbers of a single op-amp in DIP-8/SOIC-8 (741, OP07, TL071)
    B b;
    b.s.name = "opamp";
    b.s.title = "Op-amp (2 = -, 3 = +, 6 = out, 7 = V+, 4 = V-)";
    b.s.category = "ic";
    b.s.prefix = "U";
    b.s.value = "TL071";
    b.s.footprint = "SOIC-8";
    b.line({{-1.5, -2}, {-1.5, 2}, {2, 0}}, true);
    b.pin("2", "-", {-3, -1}, {-1.5, -1}, PinType::Input)
        .pin("3", "+", {-3, 1}, {-1.5, 1}, PinType::Input)
        .pin("6", "OUT", {3, 0}, {2, 0}, PinType::Output)
        .pin("7", "V+", {0, -3}, {0, -1.143}, PinType::PowerIn)
        .pin("4", "V-", {0, 3}, {0, 1.143}, PinType::PowerIn);
    b.text({-1.05, -0.75}, "-", 0.8).text({-1.05, 1.25}, "+", 0.8);
    b.s.refPos = {1.4, -1.6};
    b.s.valuePos = {1.6, 1.9};
    return b.s;
}

Symbol regulator() {
    B b;
    b.s.name = "regulator";
    b.s.title = "Voltage regulator (1 = IN, 2 = GND, 3 = OUT; 78xx in TO-220)";
    b.s.category = "ic";
    b.s.prefix = "U";
    b.s.value = "LM7805";
    b.s.footprint = "TO-220";
    b.rect({-2, -1}, {2, 1});
    b.pin("1", "IN", {-4, 0}, {-2, 0}, PinType::PowerIn)
        .pin("2", "GND", {0, 3}, {0, 1}, PinType::PowerIn)
        .pin("3", "OUT", {4, 0}, {2, 0}, PinType::PowerOut);
    b.s.showPinNames = true;
    b.s.refPos = {0, -1.5};
    b.s.valuePos = {0, -2.4};
    return b.s;
}

Symbol crystal() {
    Symbol s = twoPin("crystal", "Crystal", "Y", "16MHz", "HC49", 0.6);
    s.category = "passive";
    B b{s};
    b.line({{-0.6, -0.7}, {-0.6, 0.7}}).line({{0.6, -0.7}, {0.6, 0.7}});
    b.rect({-0.35, -0.9}, {0.35, 0.9});
    b.s.refPos = {0, -1.3};
    return b.s;
}

Symbol pushButton() {
    Symbol s = twoPin("switch", "Push button (normally open)", "SW", "SW_Push", "SW_PUSH_6mm", 1);
    s.category = "switch";
    B b{s};
    b.circle({-1, 0}, 0.15).circle({1, 0}, 0.15);
    b.line({{-1.1, -0.45}, {1.1, -0.45}}).line({{0, -0.45}, {0, -1}}).line({{-0.4, -1}, {0.4, -1}});
    b.s.refPos = {0, -1.4};
    b.s.valuePos = {0, 1.1};
    return b.s;
}

Symbol spdt() {
    B b;
    b.s.name = "switch_spdt";
    b.s.title = "Toggle/slide switch SPDT (2 = common)";
    b.s.category = "switch";
    b.s.prefix = "SW";
    b.s.value = "SW_SPDT";
    b.s.footprint = "SW_SPDT";
    b.pin("2", "C", {-2, 0}, {-0.8, 0}).pin("1", "A", {2, -1}, {0.8, -1}).pin("3", "B", {2, 1}, {0.8, 1});
    b.circle({-0.8, 0}, 0.15).circle({0.8, -1}, 0.15).circle({0.8, 1}, 0.15);
    b.line({{-0.7, -0.05}, {0.75, -0.75}});
    b.s.refPos = {0, -1.7};
    b.s.valuePos = {0, 2};
    return b.s;
}

Symbol header(int n, bool dual) {
    B b;
    n = std::clamp(n, 1, 80);
    if (dual) n = std::max(2, n + (n % 2));
    b.s.name = dual ? "header2" : "header";
    b.s.category = "connector";
    b.s.prefix = "J";
    b.s.showPinNumbers = true;
    char fp[48];
    if (dual) {
        const int rows = n / 2;
        b.s.title = "Pin header 2x" + std::to_string(rows);
        b.s.value = "Conn_02x" + std::string(rows < 10 ? "0" : "") + std::to_string(rows);
        std::snprintf(fp, sizeof fp, "PinHeader_2x%02d", rows);
        const int top = -(rows - 1) / 2;
        b.rect({-1, top - 0.5}, {1, top + rows - 0.5});
        for (int i = 0; i < rows; ++i) {
            const double y = top + i;
            b.pin(std::to_string(2 * i + 1), "Pin_" + std::to_string(2 * i + 1), {-3, y}, {-1, y});
            b.pin(std::to_string(2 * i + 2), "Pin_" + std::to_string(2 * i + 2), {3, y}, {1, y});
        }
        b.s.refPos = {0, top - 1.0};
        b.s.valuePos = {0, top + rows + 0.6};
    } else {
        b.s.title = "Pin header 1x" + std::to_string(n);
        b.s.value = "Conn_01x" + std::string(n < 10 ? "0" : "") + std::to_string(n);
        std::snprintf(fp, sizeof fp, "PinHeader_1x%02d", n);
        const int top = -(n - 1) / 2;
        b.rect({-1, top - 0.5}, {0.6, top + n - 0.5});
        for (int i = 0; i < n; ++i) {
            const double y = top + i;
            b.pin(std::to_string(i + 1), "Pin_" + std::to_string(i + 1), {-3, y}, {-1, y});
        }
        b.s.refPos = {0, top - 1.0};
        b.s.valuePos = {0, top + n + 0.6};
    }
    b.s.footprint = fp;
    return b.s;
}

// A rectangle with named pins down both sides. Left pins are numbered from 1 top to
// bottom; right pins continue bottom to top (DIP style) or top to bottom (modules).
Symbol box(const std::string& name, const std::string& title, const std::string& prefix, const std::string& value,
           const std::string& footprint, const std::vector<std::string>& left, const std::vector<std::string>& right,
           bool dipNumbering, double halfWidth) {
    B b;
    b.s.name = name;
    b.s.title = title;
    b.s.category = "ic";
    b.s.prefix = prefix;
    b.s.value = value;
    b.s.footprint = footprint;
    b.s.showPinNumbers = true;
    b.s.showPinNames = true;
    const int rows = static_cast<int>(std::max(left.size(), right.size()));
    const int top = -(rows - 1) / 2;
    b.rect({-halfWidth, top - 1.0}, {halfWidth, static_cast<double>(top + rows)});
    auto guessType = [](const std::string& n) {
        std::string u = n;
        for (auto& c : u) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (u == "GND" || u == "VSS" || u == "VCC" || u == "VDD" || u == "V+" || u == "V-" || u == "VIN" || u == "5V" ||
            u == "3V3" || u == "VBAT" || u == "AVCC" || u == "VEE")
            return PinType::PowerIn;
        if (u == "NC") return PinType::NotConnected;
        return PinType::Passive;
    };
    int number = 1;
    for (std::size_t i = 0; i < left.size(); ++i) {
        const double y = top + static_cast<double>(i);
        b.pin(std::to_string(number++), left[i], {-halfWidth - 2, y}, {-halfWidth, y}, guessType(left[i]));
    }
    for (std::size_t k = 0; k < right.size(); ++k) {
        const std::size_t i = dipNumbering ? right.size() - 1 - k : k;
        const double y = top + static_cast<double>(i);
        b.pin(std::to_string(number++), right[i], {halfWidth + 2, y}, {halfWidth, y}, guessType(right[i]));
    }
    b.s.refPos = {0, top - 1.5};
    b.s.valuePos = {0, top + rows + 1.0};
    return b.s;
}

Symbol genericIc(int n, const std::vector<std::string>& names) {
    n = std::clamp(n <= 0 ? static_cast<int>(names.size() ? names.size() : 8) : n, 1, 200);
    if (n % 2) ++n;
    std::vector<std::string> left, right;
    for (int i = 0; i < n; ++i) {
        std::string nm = i < static_cast<int>(names.size()) && !names[static_cast<std::size_t>(i)].empty() ? names[static_cast<std::size_t>(i)]
                                                                                                          : std::to_string(i + 1);
        if (i < n / 2) left.push_back(nm);
        else right.push_back(nm);
    }
    // DIP numbering: right side is listed bottom to top, so reverse what we collected
    std::reverse(right.begin(), right.end());
    std::size_t longest = 1;
    for (const auto& s : names) longest = std::max(longest, s.size());
    const double hw = std::clamp(1.0 + static_cast<double>(longest) * 0.45, 2.0, 6.0);
    Symbol s = box("ic", "Generic IC with N pins (DIP numbering)", "U", "IC", "DIP-" + std::to_string(n), left, right, true,
                   std::round(hw));
    s.title = "Generic IC, " + std::to_string(n) + " pins";
    return s;
}

Symbol arduinoUno() {
    std::vector<std::string> left = {"NC", "IOREF", "RESET", "3V3", "5V", "GND", "GND", "VIN", "A0", "A1", "A2", "A3", "A4", "A5"};
    std::vector<std::string> right = {"D0/RX", "D1/TX", "D2", "D3", "D4", "D5", "D6", "D7", "D8", "D9",
                                      "D10", "D11", "D12", "D13/SCK", "GND", "AREF", "SDA", "SCL"};
    Symbol s = box("arduino_uno", "Arduino Uno R3 (shield headers)", "A", "Arduino_Uno", "Arduino_Uno_Shield", left, right, false, 4);
    s.category = "module";
    s.module = true;
    return s;
}

Symbol arduinoNano() {
    std::vector<std::string> left = {"D1/TX", "D0/RX", "RESET", "GND", "D2", "D3", "D4", "D5", "D6", "D7", "D8", "D9", "D10", "D11/MOSI", "D12/MISO"};
    std::vector<std::string> right = {"D13/SCK", "3V3", "AREF", "A0", "A1", "A2", "A3", "A4/SDA", "A5/SCL", "A6", "A7", "5V", "RESET", "GND", "VIN"};
    Symbol s = box("arduino_nano", "Arduino Nano (2x15, 0.6 in rows)", "A", "Arduino_Nano", "Arduino_Nano", left, right, false, 4);
    s.category = "module";
    s.module = true;
    return s;
}

Symbol esp32() {
    std::vector<std::string> left = {"EN", "GPIO36/VP", "GPIO39/VN", "GPIO34/D34", "GPIO35/D35", "GPIO32/D32", "GPIO33/D33", "GPIO25/D25",
                                     "GPIO26/D26", "GPIO27/D27", "GPIO14/D14", "GPIO12/D12", "GPIO13/D13", "GND", "VIN"};
    std::vector<std::string> right = {"GPIO23/D23", "GPIO22/D22/SCL", "GPIO1/TX0", "GPIO3/RX0", "GPIO21/D21/SDA", "GPIO19/D19", "GPIO18/D18", "GPIO5/D5",
                                      "GPIO17/TX2", "GPIO16/RX2", "GPIO4/D4", "GPIO2/D2", "GPIO15/D15", "GND", "3V3"};
    Symbol s = box("esp32_devkit", "ESP32 DevKit V1 (30 pins, DOIT layout)", "A", "ESP32_DevKit", "ESP32_DevKit", left, right, false, 5);
    s.category = "module";
    s.module = true;
    return s;
}

Symbol bluePill() {
    std::vector<std::string> left = {"PB12", "PB13", "PB14", "PB15", "PA8", "PA9/TX1", "PA10/RX1", "PA11/USB-", "PA12/USB+", "PA15",
                                     "PB3", "PB4", "PB5", "PB6/SCL1", "PB7/SDA1", "PB8", "PB9", "5V", "GND", "3V3"};
    std::vector<std::string> right = {"GND", "GND", "3V3", "NRST", "PB11/SDA2", "PB10/SCL2", "PB1", "PB0", "PA7", "PA6",
                                      "PA5", "PA4", "PA3", "PA2", "PA1", "PA0", "PC15", "PC14", "PC13/LED", "VBAT"};
    Symbol s = box("bluepill", "STM32F103 Blue Pill (2x20, 0.6 in rows)", "A", "STM32_BluePill", "BluePill", left, right, false, 4);
    s.category = "module";
    s.module = true;
    return s;
}

Symbol ground() {
    B b;
    b.s.name = "gnd";
    b.s.title = "Ground (net GND)";
    b.s.category = "power";
    b.s.prefix = "#PWR";
    b.s.value = "GND";
    b.s.power = true;
    b.pin("1", "GND", {0, 0}, {0, 0.5}, PinType::PowerOut);
    b.line({{0, 0}, {0, 0.5}}).line({{-0.7, 0.5}, {0.7, 0.5}}).line({{-0.45, 0.75}, {0.45, 0.75}}).line({{-0.2, 1.0}, {0.2, 1.0}});
    b.s.refPos = {0, 99};
    b.s.valuePos = {0, 1.9};
    return b.s;
}

Symbol powerFlag(const std::string& value) {
    B b;
    b.s.name = "power";
    b.s.title = "Power rail (the value names the net: VCC, +5V, +3V3, +12V ...)";
    b.s.category = "power";
    b.s.prefix = "#PWR";
    b.s.value = value;
    b.s.power = true;
    b.pin("1", value, {0, 0}, {0, -0.6}, PinType::PowerOut);
    b.line({{0, 0}, {0, -0.6}}).line({{-0.6, -0.6}, {0.6, -0.6}});
    b.s.refPos = {0, 99};
    b.s.valuePos = {0, -1.0};
    return b.s;
}

Symbol vsource(bool battery, bool current) {
    B b;
    b.s.name = battery ? "battery" : current ? "isource" : "vsource";
    b.s.title = battery    ? "Battery (1 = +, 2 = -)"
                : current ? "Current source (value = current out of pin 1 into the circuit)"
                          : "Voltage source for simulation (1 = +, 2 = -; value 5, SIN(0 1 1k), PULSE(0 5 0 1u 1u 1m 2m))";
    b.s.category = "source";
    b.s.prefix = battery ? "BT" : current ? "I" : "V";
    b.s.value = battery ? "9V" : current ? "1m" : "5";
    b.s.footprint = battery ? "PinHeader_1x02" : "";
    b.pin("1", "+", {0, -2}, {0, battery ? -0.3 : -1}).pin("2", "-", {0, 2}, {0, battery ? 0.3 : 1});
    if (battery) {
        b.line({{-0.8, -0.3}, {0.8, -0.3}}).line({{-0.4, 0.3}, {0.4, 0.3}});
        b.text({0.7, -0.6}, "+", 0.6);
        b.s.refPos = {1.4, -0.4};
        b.s.valuePos = {1.4, 0.7};
    } else {
        b.circle({0, 0}, 1);
        if (current) {
            b.arrow({0, 0.6}, {0, -0.6}, 0.35);
        } else {
            b.text({0, -0.15}, "+", 0.7).text({0, 0.75}, "-", 0.7);
        }
        b.s.refPos = {1.6, -0.3};
        b.s.valuePos = {1.6, 0.7};
    }
    return b.s;
}


// ---- more parts ------------------------------------------------------------------------

Symbol diodeVariant(bool zener) {
    Symbol s = diode(false);
    s.name = zener ? "zener" : "schottky";
    s.title = zener ? "Zener diode (1 = K, 2 = A; value = breakdown voltage)" : "Schottky diode (1 = K, 2 = A)";
    s.value = zener ? "5V1" : "BAT54";
    // replace the cathode bar with a bent (zener) or S-shaped (schottky) one
    s.shapes.pop_back();
    B b{s};
    if (zener) b.line({{0.3, -0.6}, {0.5, -0.5}, {0.5, 0.5}, {0.7, 0.6}});
    else b.line({{0.3, -0.35}, {0.3, -0.5}, {0.5, -0.5}, {0.5, 0.5}, {0.7, 0.5}, {0.7, 0.35}});
    return b.s;
}

Symbol fuse() {
    Symbol s = twoPin("fuse", "Fuse", "F", "500mA", "Fuse_1206", 1);
    B b{s};
    b.rect({-1, -0.35}, {1, 0.35});
    b.line({{-1.0, 0}, {1.0, 0}});
    return b.s;
}

Symbol ferrite() {
    Symbol s = twoPin("ferrite_bead", "Ferrite bead", "FB", "600R@100MHz", "L_0805", 1);
    B b{s};
    b.line({{-1, 0}, {-0.45, 0}}).line({{0.45, 0}, {1, 0}});
    b.line({{-0.75, 0.55}, {-0.1, -0.85}, {0.75, -0.45}, {0.1, 0.95}}, true);
    return b.s;
}

Symbol buzzer() {
    Symbol s = twoPin("buzzer", "Buzzer (1 = +)", "BZ", "Buzzer", "Buzzer_12mm", 0.6);
    B b{s};
    b.line({{-0.6, 0}, {-0.6, -0.6}, {0.6, -0.6}, {0.6, 0}}).arc({0, -0.6}, 0.6, kPi, 2 * kPi, 10);
    b.text({-1.2, -0.45}, "+", 0.6);
    b.s.refPos = {0, -1.8};
    return b.s;
}

Symbol testPoint() {
    B b;
    b.s.name = "test_point";
    b.s.title = "Test point (one pad)";
    b.s.category = "connector";
    b.s.prefix = "TP";
    b.s.value = "TP";
    b.s.footprint = "TestPoint_Pad_1.5mm";
    b.s.inBom = false;
    b.pin("1", "1", {0, 1}, {0, 0});
    b.circle({0, -0.3}, 0.3);
    b.s.refPos = {1.2, -0.6};
    b.s.valuePos = {1.2, 0.4};
    return b.s;
}

Symbol mountingHole() {
    B b;
    b.s.name = "mounting_hole";
    b.s.title = "Mounting hole (non-plated, no pins; the footprint sets the size)";
    b.s.category = "connector";
    b.s.prefix = "H";
    b.s.value = "M3";
    b.s.footprint = "MountingHole_3.2mm";
    b.s.inBom = false;
    b.circle({0, 0}, 0.8).circle({0, 0}, 0.4);
    b.s.refPos = {0, -1.3};
    b.s.valuePos = {0, 1.6};
    return b.s;
}

Symbol screwTerminal(int n) {
    Symbol s = header(n, false);
    s.name = "screw_terminal";
    s.title = "Screw terminal 1x" + std::to_string(s.pins.size()) + ", 5.08 mm";
    s.value = "Screw_Terminal_01x" + std::string(s.pins.size() < 10 ? "0" : "") + std::to_string(s.pins.size());
    char fp[48];
    std::snprintf(fp, sizeof fp, "TerminalBlock_1x%02d", static_cast<int>(s.pins.size()));
    s.footprint = fp;
    B b{s};
    for (const auto& p : s.pins) b.circle({-0.2, p.pos.y}, 0.3);
    return b.s;
}

// A labelled block: `left` and `right` list "number:name" top to bottom ("" leaves a gap),
// `bottom` left to right. Repeated power pin names are joined (KiCad's stacked pins).
Symbol block(const std::string& name, const std::string& title, const std::string& prefix, const std::string& value,
             const std::string& footprint, const std::vector<std::string>& left, const std::vector<std::string>& right, double halfWidth,
             const std::vector<std::string>& bottom = {}) {
    B b;
    b.s.name = name;
    b.s.title = title;
    b.s.category = "ic";
    b.s.prefix = prefix;
    b.s.value = value;
    b.s.footprint = footprint;
    b.s.showPinNumbers = true;
    b.s.showPinNames = true;
    b.s.joinSameName = true;
    const int rows = static_cast<int>(std::max(left.size(), right.size()));
    const int top = -(rows - 1) / 2;
    const double bodyBottom = static_cast<double>(top + rows);
    b.rect({-halfWidth, top - 1.0}, {halfWidth, bodyBottom});
    auto type = [](const std::string& n) {
        std::string u;
        for (char ch : n.substr(0, n.find('/'))) u += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        if (u == "NC") return PinType::NotConnected;
        if (u.find("GND") != std::string::npos || u == "VSS" || u.find("VDD") != std::string::npos || u.find("VCC") != std::string::npos ||
            u == "VBUS" || u == "VSYS" || u == "VIN" || u == "VREG_IN" || u == "3V3" || u == "5V" || u == "VBAT" || u == "V+" || u == "V-")
            return PinType::PowerIn;
        return PinType::Bidirectional;
    };
    auto add = [&](const std::string& entry, Pt pos, Pt end) {
        const auto colon = entry.find(':');
        const std::string num = entry.substr(0, colon), nm = colon == std::string::npos ? entry : entry.substr(colon + 1);
        b.pin(num, nm, pos, end, type(nm));
    };
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (left[i].empty()) continue;
        const double y = top + static_cast<double>(i);
        add(left[i], {-halfWidth - 2, y}, {-halfWidth, y});
    }
    for (std::size_t i = 0; i < right.size(); ++i) {
        if (right[i].empty()) continue;
        const double y = top + static_cast<double>(i);
        add(right[i], {halfWidth + 2, y}, {halfWidth, y});
    }
    const int nb = static_cast<int>(bottom.size());
    for (int i = 0; i < nb; ++i) {
        const double x = std::round(2 * i - (nb - 1));
        add(bottom[static_cast<std::size_t>(i)], {x, bodyBottom + 2}, {x, bodyBottom});
    }
    b.s.refPos = {0, top - 1.5};
    b.s.valuePos = {0, bodyBottom + (nb ? 2.8 : 1.0)};
    return b.s;
}

void setType(Symbol& s, const std::string& name, PinType t) {
    for (auto& p : s.pins) {
        if (p.name == name) p.type = t;
    }
}

Symbol ams1117() {
    Symbol s = block("ams1117", "LDO regulator AMS1117 (1 = GND/ADJ, 2 = OUT, 3 = IN; SOT-223, tab = OUT)", "U", "AMS1117-3.3", "SOT-223",
                     {"3:VIN"}, {"2:VOUT"}, 2, {"1:GND"});
    setType(s, "VOUT", PinType::PowerOut);
    return s;
}

Symbol lm317() {
    Symbol s = block("lm317", "Adjustable regulator LM317 (1 = ADJ, 2 = OUT, 3 = IN; TO-220)", "U", "LM317", "TO-220", {"3:VIN"}, {"2:VOUT"}, 2,
                     {"1:ADJ"});
    setType(s, "VOUT", PinType::PowerOut);
    setType(s, "ADJ", PinType::Input);
    return s;
}

Symbol ne555() {
    Symbol s = block("ne555", "555 timer (DIP-8: 1 GND, 2 TR, 3 Q, 4 R, 5 CV, 6 THR, 7 DIS, 8 VCC)", "U", "NE555", "DIP-8",
                     {"8:VCC", "4:R", "2:TR", "6:THR", "5:CV"}, {"3:Q", "7:DIS", "", "", ""}, 3, {"1:GND"});
    setType(s, "Q", PinType::Output);
    setType(s, "DIS", PinType::OpenCollector);
    for (const char* n : {"TR", "THR", "R", "CV"}) setType(s, n, PinType::Input);
    return s;
}

Symbol lm358() {
    Symbol s = block("lm358", "Dual op-amp LM358 (DIP/SOIC-8: 1 OUTA, 2 -A, 3 +A, 4 V-, 5 +B, 6 -B, 7 OUTB, 8 V+)", "U", "LM358", "SOIC-8",
                     {"3:+A", "2:-A", "", "5:+B", "6:-B"}, {"1:OUTA", "", "", "7:OUTB", ""}, 3, {"8:V+", "4:V-"});
    for (const char* n : {"OUTA", "OUTB"}) setType(s, n, PinType::Output);
    for (const char* n : {"+A", "-A", "+B", "-B"}) setType(s, n, PinType::Input);
    return s;
}

Symbol atmega328p(bool tqfp) {
    // pin numbers: DIP-28 (ATmega328P-PU) or TQFP-32 (ATmega328P-AU); names carry the Arduino pin numbers as aliases
    auto n = [&](int dip, int qfp) { return std::to_string(tqfp ? qfp : dip); };
    std::vector<std::string> left = {n(7, 4) + ":VCC", n(20, 18) + ":AVCC", n(21, 20) + ":AREF", "", n(1, 29) + ":PC6/RESET",
                                     n(9, 7) + ":PB6/XTAL1", n(10, 8) + ":PB7/XTAL2"};
    if (tqfp) {
        left.insert(left.begin() + 1, "6:VCC");
        left.push_back("");
        left.push_back("19:ADC6/A6");
        left.push_back("22:ADC7/A7");
    }
    std::vector<std::string> right = {n(14, 12) + ":PB0/D8",       n(15, 13) + ":PB1/D9",      n(16, 14) + ":PB2/SS/D10",  n(17, 15) + ":PB3/MOSI/D11",
                                      n(18, 16) + ":PB4/MISO/D12", n(19, 17) + ":PB5/SCK/D13", "",                          n(23, 23) + ":PC0/ADC0/A0",
                                      n(24, 24) + ":PC1/ADC1/A1",  n(25, 25) + ":PC2/ADC2/A2", n(26, 26) + ":PC3/ADC3/A3", n(27, 27) + ":PC4/SDA/A4",
                                      n(28, 28) + ":PC5/SCL/A5",   "",                         n(2, 30) + ":PD0/RXD/RX/D0",   n(3, 31) + ":PD1/TXD/TX/D1",
                                      n(4, 32) + ":PD2/INT0/D2",   n(5, 1) + ":PD3/INT1/D3",   n(6, 2) + ":PD4/D4",        n(11, 9) + ":PD5/D5",
                                      n(12, 10) + ":PD6/D6",       n(13, 11) + ":PD7/D7"};
    std::vector<std::string> bottom = {n(8, 3) + ":GND", n(22, 5) + ":GND"};
    if (tqfp) bottom.push_back("21:GND");
    Symbol s = block(tqfp ? "atmega328p_au" : "atmega328p",
                     tqfp ? "ATmega328P-AU microcontroller (TQFP-32), the Arduino Nano chip; pins also answer to D0..D13, A0..A7"
                          : "ATmega328P-PU microcontroller (DIP-28), the Arduino Uno chip; pins also answer to D0..D13, A0..A5",
                     "U", tqfp ? "ATmega328P-AU" : "ATmega328P-PU", tqfp ? "TQFP-32" : "DIP-28", left, right, 6, bottom);
    setType(s, "AREF", PinType::Input);
    return s;
}

Symbol attiny85() {
    return block("attiny85", "ATtiny85 microcontroller (DIP-8: 1 PB5/RESET, 2 PB3, 3 PB4, 4 GND, 5 PB0, 6 PB1, 7 PB2, 8 VCC)", "U", "ATtiny85-20PU",
                 "DIP-8", {"8:VCC", "", "1:PB5/RESET"}, {"5:PB0/MOSI/SDA", "6:PB1/MISO", "7:PB2/SCK/SCL", "2:PB3", "3:PB4"}, 4, {"4:GND"});
}

Symbol rp2040() {
    std::vector<std::string> left = {"1:IOVDD", "10:IOVDD", "22:IOVDD", "33:IOVDD", "42:IOVDD", "49:IOVDD", "23:DVDD", "50:DVDD", "44:VREG_IN",
                                     "45:VREG_VOUT", "43:ADC_AVDD", "48:USB_VDD", "", "46:USB_DM", "47:USB_DP", "", "26:RUN", "19:TESTEN",
                                     "24:SWCLK", "25:SWD", "", "20:XIN", "21:XOUT", "", "56:QSPI_SS", "52:QSPI_SCLK", "53:QSPI_SD0", "55:QSPI_SD1",
                                     "54:QSPI_SD2", "51:QSPI_SD3"};
    std::vector<std::string> right;
    const int gpioPin[30] = {2, 3, 4, 5, 6, 7, 8, 9, 11, 12, 13, 14, 15, 16, 17, 18, 27, 28, 29, 30, 31, 32, 34, 35, 36, 37, 38, 39, 40, 41};
    for (int g = 0; g < 30; ++g) {
        std::string nm = std::to_string(gpioPin[g]) + ":GPIO" + std::to_string(g) + "/GP" + std::to_string(g);
        if (g >= 26) nm += "/ADC" + std::to_string(g - 26);
        right.push_back(nm);
    }
    Symbol s = block("rp2040", "RP2040 microcontroller (QFN-56, pad 57 = GND); GPIOn also answers to GPn", "U", "RP2040", "QFN-56", left, right, 6,
                     {"57:GND"});
    setType(s, "VREG_VOUT", PinType::PowerOut);
    for (const char* n : {"XIN", "RUN", "TESTEN", "SWCLK"}) setType(s, n, PinType::Input);
    return s;
}

Symbol esp32Wroom() {
    std::vector<std::string> left = {"2:3V3", "3:EN", "", "25:IO0", "35:TXD0/IO1/TX", "24:IO2", "34:RXD0/IO3/RX", "26:IO4", "29:IO5", "",
                                     "20:CLK/IO6", "21:SD0/IO7", "22:SD1/IO8", "17:SD2/IO9", "18:SD3/IO10", "19:CMD/IO11"};
    std::vector<std::string> right = {"14:IO12",     "16:IO13",     "13:IO14", "23:IO15", "27:IO16", "28:IO17", "30:IO18", "31:IO19",
                                      "33:IO21/SDA", "36:IO22/SCL", "37:IO23", "10:IO25", "11:IO26", "12:IO27", "8:IO32",  "9:IO33",
                                      "6:IO34",      "7:IO35",      "4:SENSOR_VP/IO36",   "5:SENSOR_VN/IO39"};
    Symbol s = block("esp32_wroom", "ESP32-WROOM-32 module (38 pads + ground pad 39); IO6..IO11 drive the module's flash, leave them free", "U",
                     "ESP32-WROOM-32", "ESP32-WROOM-32", left, right, 5, {"1:GND", "15:GND", "38:GND", "39:GND", "32:NC"});
    s.category = "module";
    s.module = true;
    return s;
}

Symbol rpiPico() {
    std::vector<std::string> left, right;
    const char* names[40] = {"GP0",  "GP1",  "GND",  "GP2",  "GP3",  "GP4",  "GP5",  "GND",       "GP6",       "GP7",       "GP8",      "GP9",     "GND",
                             "GP10", "GP11", "GP12", "GP13", "GND",  "GP14", "GP15", "GP16",      "GP17",      "GND",       "GP18",     "GP19",    "GP20",
                             "GP21", "GND",  "GP22", "RUN",  "GP26/ADC0", "GP27/ADC1", "AGND", "GP28/ADC2", "ADC_VREF", "3V3_OUT/3V3", "3V3_EN", "GND", "VSYS", "VBUS"};
    for (int i = 0; i < 20; ++i) left.push_back(std::to_string(i + 1) + ":" + names[i]);
    for (int i = 39; i >= 20; --i) right.push_back(std::to_string(i + 1) + ":" + names[i]);
    Symbol s = block("rpi_pico", "Raspberry Pi Pico (2x20, 0.7 in rows, numbered like a DIP)", "A", "RPi_Pico", "RPi_Pico", left, right, 5);
    s.category = "module";
    s.module = true;
    return s;
}

Symbol usbMicro() {
    Symbol s = block("usb_micro", "USB Micro-B receptacle (1 VBUS, 2 D-, 3 D+, 4 ID, 5 GND, 6 shield)", "J", "USB_B_Micro", "USB_Micro-B", {},
                     {"1:VBUS", "2:D-", "3:D+", "4:ID", "5:GND", "6:SHIELD"}, 2);
    s.category = "connector";
    s.joinSameName = false;
    for (auto& p : s.pins) p.type = p.name == "VBUS" || p.name == "GND" ? PinType::PowerOut : PinType::Passive;
    return s;
}

struct CatalogRow {
    SymbolEntry entry;
    std::vector<std::string> aliases;
};

// ---- multi-unit parts: one package, several gates/amplifiers drawn as separate units -------------

struct UnitDef {
    std::string title, prefix, value, footprint;
    int units = 0;  // including the power unit (the last)
};

// pins per unit: {number, name} for in1, in2 (or -/+), out
Symbol opampUnit(const std::string& name, char letter, const std::string& minus, const std::string& plus, const std::string& out) {
    B b;
    b.s.name = name;
    b.s.prefix = "U";
    b.s.category = "ic";
    b.line({{-1.5, -2}, {-1.5, 2}, {2, 0}}, true);
    const std::string l(1, letter);
    b.pin(minus, "-" + l, {-3, -1}, {-1.5, -1}, PinType::Input)
        .pin(plus, "+" + l, {-3, 1}, {-1.5, 1}, PinType::Input)
        .pin(out, "OUT" + l, {3, 0}, {2, 0}, PinType::Output);
    b.text({-1.05, -0.75}, "-", 0.8).text({-1.05, 1.25}, "+", 0.8);
    b.s.refPos = {1.4, -1.6};
    b.s.valuePos = {1.6, 1.9};
    return b.s;
}

Symbol nandUnit(const std::string& name, char letter, const std::string& a, const std::string& bpin, const std::string& out) {
    B b;
    b.s.name = name;
    b.s.prefix = "U";
    b.s.category = "ic";
    // a D shape with a bubble
    b.line({{0, -1.5}, {-1.5, -1.5}, {-1.5, 1.5}, {0, 1.5}});
    b.arc({0, 0}, 1.5, -kPiLib / 2, kPiLib / 2, 16);
    b.circle({1.8, 0}, 0.3);
    const std::string l(1, letter);
    b.pin(a, "A" + l, {-3, -1}, {-1.5, -1}, PinType::Input)
        .pin(bpin, "B" + l, {-3, 1}, {-1.5, 1}, PinType::Input)
        .pin(out, "Y" + l, {3, 0}, {2.1, 0}, PinType::Output);
    b.s.refPos = {0, -2.2};
    b.s.valuePos = {0, 2.6};
    return b.s;
}

Symbol inverterUnit(const std::string& name, char letter, const std::string& in, const std::string& out) {
    B b;
    b.s.name = name;
    b.s.prefix = "U";
    b.s.category = "ic";
    b.line({{-1.2, -1.2}, {-1.2, 1.2}, {1, 0}}, true);
    b.circle({1.3, 0}, 0.3);
    const std::string l(1, letter);
    b.pin(in, "A" + l, {-3, 0}, {-1.2, 0}, PinType::Input).pin(out, "Y" + l, {3, 0}, {1.6, 0}, PinType::Output);
    b.s.refPos = {0, -1.9};
    b.s.valuePos = {0, 2.2};
    return b.s;
}

Symbol powerUnit(const std::string& name, const std::string& vplus, const std::string& vplusName, const std::string& vminus, const std::string& vminusName) {
    B b;
    b.s.name = name;
    b.s.prefix = "U";
    b.s.category = "ic";
    b.rect({-1, -1.5}, {1, 1.5});
    b.pin(vplus, vplusName, {0, -3}, {0, -1.5}, PinType::PowerIn).pin(vminus, vminusName, {0, 3}, {0, 1.5}, PinType::PowerIn);
    b.s.showPinNames = true;
    b.s.refPos = {2.4, -0.4};
    b.s.valuePos = {2.9, 0.7};
    return b.s;
}

const std::map<std::string, UnitDef>& unitDefs() {
    static const std::map<std::string, UnitDef> defs = {
        {"opamp_dual", {"Dual op-amp LM358 as units: A (1 out, 2 -, 3 +), B (7 out, 6 -, 5 +), C power (8 V+, 4 V-)", "U", "LM358", "SOIC-8", 3}},
        {"opamp_quad", {"Quad op-amp LM324 as units: A (1/2/3), B (7/6/5), C (8/9/10), D (14/13/12) out/-/+, E power (4 V+, 11 V-)", "U", "LM324", "SOIC-14", 5}},
        {"nand_quad", {"Quad NAND 74HC00 as units: A (1, 2 -> 3), B (4, 5 -> 6), C (9, 10 -> 8), D (12, 13 -> 11), E power (14 VCC, 7 GND)", "U", "74HC00", "SOIC-14", 5}},
        {"inverter_hex", {"Hex inverter 74HC04 as units: A (1 -> 2), B (3 -> 4), C (5 -> 6), D (9 -> 8), E (11 -> 10), F (13 -> 12), G power (14 VCC, 7 GND)", "U", "74HC04", "SOIC-14", 7}},
    };
    return defs;
}

Symbol makeUnit(const std::string& name, int unit) {
    const UnitDef& d = unitDefs().at(name);
    const char letter = static_cast<char>('A' + unit - 1);
    Symbol s;
    if (name == "opamp_dual") {
        static const char* p[2][3] = {{"2", "3", "1"}, {"6", "5", "7"}};
        s = unit <= 2 ? opampUnit(name, letter, p[unit - 1][0], p[unit - 1][1], p[unit - 1][2]) : powerUnit(name, "8", "V+", "4", "V-");
    } else if (name == "opamp_quad") {
        static const char* p[4][3] = {{"2", "3", "1"}, {"6", "5", "7"}, {"9", "10", "8"}, {"13", "12", "14"}};
        s = unit <= 4 ? opampUnit(name, letter, p[unit - 1][0], p[unit - 1][1], p[unit - 1][2]) : powerUnit(name, "4", "V+", "11", "V-");
    } else if (name == "nand_quad") {
        static const char* p[4][3] = {{"1", "2", "3"}, {"4", "5", "6"}, {"9", "10", "8"}, {"12", "13", "11"}};
        s = unit <= 4 ? nandUnit(name, letter, p[unit - 1][0], p[unit - 1][1], p[unit - 1][2]) : powerUnit(name, "14", "VCC", "7", "GND");
    } else {
        static const char* p[6][2] = {{"1", "2"}, {"3", "4"}, {"5", "6"}, {"9", "8"}, {"11", "10"}, {"13", "12"}};
        s = unit <= 6 ? inverterUnit(name, letter, p[unit - 1][0], p[unit - 1][1]) : powerUnit(name, "14", "VCC", "7", "GND");
    }
    s.title = d.title + " (unit " + std::string(1, letter) + (unit == d.units ? ", power" : "") + ")";
    s.value = d.value;
    s.footprint = d.footprint;
    s.prefix = d.prefix;
    s.showPinNumbers = true;
    finishSymbol(s);
    return s;
}

const std::vector<CatalogRow>& rows() {
    static const std::vector<CatalogRow> r = {
        {{"resistor", "passive", "resistor (R_0805)", "r, res"}, {"r", "res"}},
        {{"capacitor", "passive", "capacitor (C_0805)", "c, cap"}, {"c", "cap"}},
        {{"capacitor_polarized", "passive", "electrolytic capacitor, pin 1 = + (CP_THT)", "cp, electrolytic"}, {"cp", "electrolytic", "c_polarized"}},
        {{"inductor", "passive", "inductor (L_0805)", "l"}, {"l", "ind"}},
        {{"potentiometer", "passive", "potentiometer, 2 = wiper (Pot_THT)", "pot, rv"}, {"pot", "rv", "trimmer"}},
        {{"crystal", "passive", "crystal (HC49)", "xtal, y"}, {"xtal", "y", "quartz"}},
        {{"diode", "semiconductor", "diode, 1 = K, 2 = A (D_SOD-123)", "d"}, {"d"}},
        {{"led", "semiconductor", "LED, 1 = K, 2 = A (LED_0805)", ""}, {}},
        {{"zener", "semiconductor", "Zener diode, 1 = K, 2 = A, value = breakdown voltage (D_SOD-123)", "dz"}, {"dz", "d_zener"}},
        {{"schottky", "semiconductor", "Schottky diode, 1 = K, 2 = A (D_SOD-123)", "d_schottky"}, {"d_schottky", "bat54", "ss14"}},
        {{"fuse", "passive", "fuse (Fuse_1206)", "f, polyfuse"}, {"f", "polyfuse", "ptc"}},
        {{"ferrite_bead", "passive", "ferrite bead (L_0805)", "fb, ferrite"}, {"fb", "ferrite", "bead"}},
        {{"buzzer", "passive", "piezo/magnetic buzzer, 1 = + (Buzzer_12mm)", "bz, piezo"}, {"bz", "piezo", "beeper"}},
        {{"npn", "semiconductor", "NPN BJT, 1 = B, 2 = E, 3 = C (SOT-23)", "transistor, q_npn"}, {"transistor", "q_npn", "bjt"}},
        {{"pnp", "semiconductor", "PNP BJT, 1 = B, 2 = E, 3 = C (SOT-23)", "q_pnp"}, {"q_pnp"}},
        {{"nmos", "semiconductor", "N-channel MOSFET, 1 = G, 2 = S, 3 = D (SOT-23)", "mosfet, q_nmos"}, {"mosfet", "q_nmos", "nfet"}},
        {{"pmos", "semiconductor", "P-channel MOSFET, 1 = G, 2 = S, 3 = D (SOT-23)", "q_pmos"}, {"q_pmos", "pfet"}},
        {{"opamp", "ic", "single op-amp, 2 = -, 3 = +, 6 = out, 7 = V+, 4 = V- (SOIC-8)", "op-amp"}, {"op-amp", "op_amp", "amplifier"}},
        {{"regulator", "ic", "linear regulator, 1 = IN, 2 = GND, 3 = OUT (TO-220)", "vreg, 7805"}, {"vreg", "7805", "ldo", "voltage_regulator"}},
        {{"ams1117", "ic", "LDO AMS1117, 1 = GND/ADJ, 2 = OUT, 3 = IN (SOT-223)", "ldo_sot223, lm1117"}, {"ldo_sot223", "lm1117", "ams1117-3.3", "ams1117-5.0"}},
        {{"lm317", "ic", "adjustable regulator LM317, 1 = ADJ, 2 = OUT, 3 = IN (TO-220)", ""}, {"lm317t"}},
        {{"ne555", "ic", "555 timer, 1 GND, 2 TR, 3 Q, 4 R, 5 CV, 6 THR, 7 DIS, 8 VCC (DIP-8)", "555, timer"}, {"555", "timer", "lm555", "na555"}},
        {{"lm358", "ic", "dual op-amp LM358, 1 = OUTA ... 8 = V+ (SOIC-8)", "dual_opamp"}, {"dual_opamp", "lm358n"}},
        {{"opamp_dual", "ic", "LM358 as 3 units: op-amps A, B and a power unit C (SOIC-8); placing it places every unit", "lm358_units"}, {"lm358_units"}},
        {{"opamp_quad", "ic", "LM324 as 5 units: op-amps A..D and a power unit E (SOIC-14)", "lm324, quad_opamp"}, {"lm324", "quad_opamp", "lm324n"}},
        {{"nand_quad", "ic", "74HC00 as 5 units: NAND gates A..D and a power unit E (SOIC-14)", "74hc00, nand"}, {"74hc00", "nand", "74ls00", "7400"}},
        {{"inverter_hex", "ic", "74HC04 as 7 units: inverters A..F and a power unit G (SOIC-14)", "74hc04, not, inverter"}, {"74hc04", "inverter", "not_gate", "7404", "74ls04"}},
        {{"atmega328p", "ic", "ATmega328P-PU (DIP-28); pins also D0..D13, A0..A5", "atmega328, mega328"}, {"atmega328", "mega328", "atmega328p-pu"}},
        {{"atmega328p_au", "ic", "ATmega328P-AU (TQFP-32)", "atmega328p-au"}, {"atmega328p-au"}},
        {{"attiny85", "ic", "ATtiny85 (DIP-8)", "tiny85"}, {"tiny85", "attiny85-20pu"}},
        {{"rp2040", "ic", "RP2040 microcontroller (QFN-56)", ""}, {}},
        {{"ic", "ic", "generic IC: pins=N, pin_names=A,B,... (DIP-N)", "chip"}, {"chip", "generic_ic"}},
        {{"switch", "switch", "push button, normally open (SW_PUSH_6mm)", "button, sw, pushbutton"}, {"button", "sw", "pushbutton", "push_button"}},
        {{"switch_spdt", "switch", "SPDT toggle/slide switch, 2 = common (SW_SPDT)", "spdt, toggle"}, {"spdt", "toggle"}},
        {{"header", "connector", "1xN pin header: pins=N (PinHeader_1xNN)", "conn, connector, j"}, {"conn", "connector", "j", "pin_header"}},
        {{"header2", "connector", "2xN pin header: pins=total (PinHeader_2xNN)", "conn2"}, {"conn2", "header_2x"}},
        {{"screw_terminal", "connector", "1xN screw terminal, 5.08 mm: pins=N (TerminalBlock_1xNN)", "terminal"}, {"terminal", "terminal_block"}},
        {{"usb_micro", "connector", "USB Micro-B receptacle (USB_Micro-B)", "usb"}, {"usb", "micro_usb", "usb_b_micro"}},
        {{"test_point", "connector", "test point pad (TestPoint_Pad_1.5mm)", "tp"}, {"tp", "testpoint"}},
        {{"mounting_hole", "connector", "mounting hole, no pins (MountingHole_3.2mm)", "hole"}, {"hole", "mountinghole"}},
        {{"arduino_uno", "module", "Arduino Uno R3 shield headers", "uno"}, {"uno"}},
        {{"arduino_nano", "module", "Arduino Nano 2x15", "nano"}, {"nano"}},
        {{"esp32_devkit", "module", "ESP32 DevKit V1, 30 pins", "esp32"}, {"esp32", "esp32_devkitc"}},
        {{"bluepill", "module", "STM32F103 Blue Pill 2x20", "stm32, blue_pill"}, {"stm32", "blue_pill", "stm32_bluepill"}},
        {{"esp32_wroom", "module", "ESP32-WROOM-32 module, 38 pads (ESP32-WROOM-32)", "wroom, esp32-wroom-32"}, {"wroom", "esp32-wroom-32", "esp32_wroom_32"}},
        {{"rpi_pico", "module", "Raspberry Pi Pico 2x20", "pico"}, {"pico", "raspberry_pi_pico"}},
        {{"gnd", "power", "ground symbol, net GND", "ground"}, {"ground"}},
        {{"power", "power", "power rail symbol; value names the net (VCC, +5V, +3V3, +12V)", "vcc, +5v, +3v3, +12v, vdd"},
         {"vcc", "+5v", "+3v3", "+3.3v", "+12v", "vdd", "5v", "3v3", "12v"}},
        {{"vsource", "source", "voltage source for simulation: 5, SIN(off amp freq), PULSE(v1 v2 delay rise fall width period)", "v"}, {"v"}},
        {{"isource", "source", "current source for simulation", "i"}, {"i"}},
        {{"battery", "source", "battery, 1 = + (PinHeader_1x02)", "bt, bat"}, {"bt", "bat", "cell"}},
    };
    return r;
}

}  // namespace

const std::vector<SymbolEntry>& symbolCatalog() {
    static const std::vector<SymbolEntry> list = [] {
        std::vector<SymbolEntry> out;
        for (const auto& r : rows()) out.push_back(r.entry);
        return out;
    }();
    return list;
}

std::string canonicalSymbol(const std::string& name) {
    const std::string n = lower(name);
    for (const auto& r : rows()) {
        if (r.entry.name == n) return r.entry.name;
        for (const auto& a : r.aliases) {
            if (a == n) return r.entry.name;
        }
    }
    return {};
}

std::shared_ptr<const Symbol> findSymbol(const std::string& name, int pins, const std::vector<std::string>& pinNames, std::string* error) {
    const std::string canon = canonicalSymbol(name);
    if (canon.empty()) {
        if (error) *error = "no symbol called '" + name + "' (circuit.symbols lists the library)";
        return nullptr;
    }
    if (unitDefs().count(canon)) return findSymbolUnit(canon, 1);
    std::string key = canon + "#" + std::to_string(pins);
    if (canon == "power") key += "#" + lower(name);
    for (const auto& n : pinNames) key += "|" + n;
    static std::mutex mutex;
    static std::map<std::string, std::shared_ptr<const Symbol>> cache;
    std::lock_guard<std::mutex> lock(mutex);
    if (auto it = cache.find(key); it != cache.end()) return it->second;
    Symbol s;
    if (canon == "resistor") s = resistor();
    else if (canon == "capacitor") s = capacitor();
    else if (canon == "capacitor_polarized") s = capacitorPolarized();
    else if (canon == "inductor") s = inductor();
    else if (canon == "potentiometer") s = potentiometer();
    else if (canon == "crystal") s = crystal();
    else if (canon == "diode") s = diode(false);
    else if (canon == "led") s = diode(true);
    else if (canon == "npn") s = bjt(true);
    else if (canon == "pnp") s = bjt(false);
    else if (canon == "nmos") s = mosfet(true);
    else if (canon == "pmos") s = mosfet(false);
    else if (canon == "opamp") s = opamp();
    else if (canon == "regulator") s = regulator();
    else if (canon == "ic") s = genericIc(pins, pinNames);
    else if (canon == "switch") s = pushButton();
    else if (canon == "switch_spdt") s = spdt();
    else if (canon == "header") s = header(pins <= 0 ? 2 : pins, false);
    else if (canon == "header2") s = header(pins <= 0 ? 4 : pins, true);
    else if (canon == "arduino_uno") s = arduinoUno();
    else if (canon == "arduino_nano") s = arduinoNano();
    else if (canon == "esp32_devkit") s = esp32();
    else if (canon == "bluepill") s = bluePill();
    else if (canon == "zener") s = diodeVariant(true);
    else if (canon == "schottky") s = diodeVariant(false);
    else if (canon == "fuse") s = fuse();
    else if (canon == "ferrite_bead") s = ferrite();
    else if (canon == "buzzer") s = buzzer();
    else if (canon == "test_point") s = testPoint();
    else if (canon == "mounting_hole") s = mountingHole();
    else if (canon == "screw_terminal") s = screwTerminal(pins <= 0 ? 2 : pins);
    else if (canon == "ams1117") s = ams1117();
    else if (canon == "lm317") s = lm317();
    else if (canon == "ne555") s = ne555();
    else if (canon == "lm358") s = lm358();
    else if (canon == "atmega328p") s = atmega328p(false);
    else if (canon == "atmega328p_au") s = atmega328p(true);
    else if (canon == "attiny85") s = attiny85();
    else if (canon == "rp2040") s = rp2040();
    else if (canon == "esp32_wroom") s = esp32Wroom();
    else if (canon == "rpi_pico") s = rpiPico();
    else if (canon == "usb_micro") s = usbMicro();
    else if (canon == "gnd") s = ground();
    else if (canon == "power") {
        std::string v = lower(name);
        std::string value = "VCC";
        if (v == "+5v" || v == "5v") value = "+5V";
        else if (v == "+3v3" || v == "3v3" || v == "+3.3v") value = "+3V3";
        else if (v == "+12v" || v == "12v") value = "+12V";
        else if (v == "vdd") value = "VDD";
        s = powerFlag(value);
    } else if (canon == "vsource") s = vsource(false, false);
    else if (canon == "isource") s = vsource(false, true);
    else if (canon == "battery") s = vsource(true, false);
    computeBounds(s);
    auto ptr = std::make_shared<const Symbol>(std::move(s));
    cache[key] = ptr;
    return ptr;
}

void finishSymbol(Symbol& s) { computeBounds(s); }

Symbol makeBoxSymbol(const std::string& name, const std::string& prefix, const std::string& value,
                     const std::vector<std::pair<std::string, std::string>>& pins) {
    std::vector<std::string> left, right;
    std::size_t longest = 1;
    for (std::size_t i = 0; i < pins.size(); ++i) {
        const std::string entry = pins[i].first + ":" + (pins[i].second.empty() ? pins[i].first : pins[i].second);
        longest = std::max(longest, pins[i].second.size());
        (i < (pins.size() + 1) / 2 ? left : right).push_back(entry);
    }
    const double hw = std::clamp(std::round(1.0 + static_cast<double>(longest) * 0.45), 2.0, 6.0);
    Symbol s = block(name, value, prefix, value, "", left, right, hw);
    s.joinSameName = false;
    s.category = "ic";
    for (auto& p : s.pins) p.type = PinType::Passive;
    computeBounds(s);
    return s;
}

int symbolUnits(const std::string& name) {
    const std::string canon = canonicalSymbol(name);
    auto it = unitDefs().find(canon.empty() ? lower(name) : canon);
    return it == unitDefs().end() ? 0 : it->second.units;
}

std::shared_ptr<const Symbol> findSymbolUnit(const std::string& name, int unit) {
    const std::string canon = canonicalSymbol(name);
    const std::string key = canon.empty() ? lower(name) : canon;
    auto it = unitDefs().find(key);
    if (it == unitDefs().end() || unit < 1 || unit > it->second.units) return nullptr;
    static std::mutex mutex;
    static std::map<std::pair<std::string, int>, std::shared_ptr<const Symbol>> cache;
    std::lock_guard<std::mutex> lock(mutex);
    auto& slot = cache[{key, unit}];
    if (!slot) slot = std::make_shared<const Symbol>(makeUnit(key, unit));
    return slot;
}

}  // namespace wetwaresuite::circuit
