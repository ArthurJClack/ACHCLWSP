#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

#include <nlohmann/json.hpp>

#include "wetwaresuite/core/cli.hpp"
#include "wetwaresuite/core/process.hpp"
#include "wetwaresuite/mcp/server.hpp"
#include "wetwaresuite/office/chart.hpp"
#include "wetwaresuite/office/deck.hpp"
#include "wetwaresuite/office/doc.hpp"
#include "wetwaresuite/office/office.hpp"
#include "wetwaresuite/office/sheet.hpp"
#include "wetwaresuite/pdf/reader.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "check.hpp"
#include "xml.hpp"

using namespace wetwaresuite;
using namespace wetwaresuite::office;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

fs::path tmp(const std::string& name) {
    // one folder per run, so parallel runs (other build trees on the machine) never collide
    static const std::string run = std::to_string(std::random_device{}());
    fs::path dir = fs::temp_directory_path() / ("wetwaresuite_office_test_" + run);
    fs::create_directories(dir);
    return dir / name;
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void writeFile(const fs::path& p, const std::string& s) {
    std::ofstream out(p, std::ios::binary);
    out << s;
}

bool near(double a, double b, double tol = 1e-9) { return std::abs(a - b) <= tol; }

Workbook book() {
    Workbook wb;
    wb.sheets.emplace_back();
    wb.sheets[0].name = "Sheet1";
    return wb;
}

void put(Workbook& wb, const std::string& ref, const std::string& input) {
    Range r;
    std::string err;
    if (!parseRange(wb, ref, 0, r, err)) {
        std::printf("bad ref %s: %s\n", ref.c_str(), err.c_str());
        return;
    }
    setInput(wb.sheets[static_cast<std::size_t>(r.sheet)].at(r.r1, r.c1), input);
}

CellValue val(const Workbook& wb, const std::string& ref) {
    Range r;
    std::string err;
    if (!parseRange(wb, ref, 0, r, err)) return CellValue::error("bad ref");
    const Cell* c = wb.sheets[static_cast<std::size_t>(r.sheet)].get(r.r1, r.c1);
    return c ? c->value : CellValue{};
}

double numAt(const Workbook& wb, const std::string& ref) {
    CellValue v = val(wb, ref);
    return v.type == CellValue::Number ? v.number : std::nan("");
}

CellValue eval(const Workbook& wb, const std::string& f) { return wb.evaluate(f, 0, 0, 0); }
double evalNum(const Workbook& wb, const std::string& f) {
    CellValue v = eval(wb, f);
    if (v.type != CellValue::Number) std::printf("  %s -> %s\n", f.c_str(), formatValue(v, "").c_str());
    return v.type == CellValue::Number ? v.number : std::nan("");
}
std::string evalText(const Workbook& wb, const std::string& f) { return formatValue(eval(wb, f), ""); }

void testA1() {
    CHECK(colName(0) == "A" && colName(25) == "Z" && colName(26) == "AA" && colName(16383) == "XFD");
    CHECK(colIndex("A") == 0 && colIndex("AB") == 27 && colIndex("XFD") == 16383 && colIndex("XFE") == -1);
    CHECK(cellName(9, 2) == "C10");
    Workbook wb = book();
    wb.sheets.emplace_back();
    wb.sheets[1].name = "My Data";
    Range r;
    std::string err;
    CHECK(parseRange(wb, "B2:D5", 0, r, err) && r.r1 == 1 && r.c1 == 1 && r.r2 == 4 && r.c2 == 3);
    CHECK(parseRange(wb, "'My Data'!A1", 0, r, err) && r.sheet == 1);
    CHECK(parseRange(wb, "my data!A:B", 0, r, err) && r.sheet == 1);  // sheet names match case-insensitively
    CHECK(parseRange(wb, "C:C", 0, r, err) && r.c1 == 2 && r.r2 == kMaxRows - 1);
    CHECK(parseRange(wb, "3:4", 0, r, err) && r.r1 == 2 && r.r2 == 3 && r.c2 == kMaxCols - 1);
    CHECK(!parseRange(wb, "Nope!A1", 0, r, err) && err.find("Nope") != std::string::npos);
    CHECK(quoteSheet("Sheet1") == "Sheet1" && quoteSheet("My Data") == "'My Data'" && quoteSheet("AB12") == "'AB12'");
}

void testInputAndFormats() {
    Cell c;
    setInput(c, "12.5");
    CHECK(c.value.type == CellValue::Number && c.value.number == 12.5 && c.format.empty());
    setInput(c, "=SUM(A1:A3)");
    CHECK(c.formula == "SUM(A1:A3)");
    Cell p;
    setInput(p, "15%");
    CHECK(near(p.value.number, 0.15) && p.format == "0%");
    Cell m;
    setInput(m, "$1,234.50");
    CHECK(near(m.value.number, 1234.5) && m.format == "$#,##0.00");
    Cell d;
    setInput(d, "2024-03-01");
    CHECK(d.value.number == 45352 && d.format == "yyyy-mm-dd");
    Cell t;
    setInput(t, "'007");
    CHECK(t.value.type == CellValue::Text && t.value.text == "007" && inputText(t) == "'007");
    Cell b;
    setInput(b, "true");
    CHECK(b.value.type == CellValue::Bool && b.value.number == 1);
    Cell s;
    setInput(s, "1,2,3");
    CHECK(s.value.type == CellValue::Text);

    CHECK(formatValue(CellValue::num(1234567.891), "#,##0.00") == "1,234,567.89");
    CHECK(formatValue(CellValue::num(0.256), "0.0%") == "25.6%");
    CHECK(formatValue(CellValue::num(-42), "$#,##0") == "-$42");
    CHECK(formatValue(CellValue::num(-42), "#,##0;(#,##0)") == "(42)");
    CHECK(formatValue(CellValue::num(3), "0.00") == "3.00");
    CHECK(formatValue(CellValue::num(0.5), "#.##") == ".5");
    CHECK(formatValue(CellValue::num(12345.678), "0.00E+00") == "1.23E+04");
    CHECK(formatValue(CellValue::num(45352), "yyyy-mm-dd") == "2024-03-01");
    CHECK(formatValue(CellValue::num(45352), "d mmm yyyy") == "1 Mar 2024");
    CHECK(formatValue(CellValue::num(45352), "dddd, mmmm d") == "Friday, March 1");
    CHECK(formatValue(CellValue::num(45352.75), "hh:mm") == "18:00");
    CHECK(formatValue(CellValue::num(45352.75), "h:mm AM/PM") == "6:00 PM");
    CHECK(formatValue(CellValue::num(1.0 / 3), "") == "0.3333333333");
    CHECK(formatValue(CellValue::num(1e12), "") == "1E+12");
    CHECK(formatValue(CellValue::num(100), "\"Total: \"0") == "Total: 100");
    CHECK(formatValue(CellValue::num(5), "[$€-407] #,##0.00") == "€ 5.00");
    CHECK(isDateFormat("yyyy-mm-dd") && isDateFormat("h:mm") && !isDateFormat("0.00") && !isDateFormat("#,##0"));
    int y, mo, dd;
    serialDate(1, y, mo, dd);
    CHECK(y == 1900 && mo == 1 && dd == 1);
    serialDate(61, y, mo, dd);
    CHECK(y == 1900 && mo == 3 && dd == 1);
    CHECK(dateSerial(1900, 2, 28) == 59 && dateSerial(2000, 1, 1) == 36526);
}

void testFormulas() {
    Workbook wb = book();
    put(wb, "A1", "10");
    put(wb, "A2", "20");
    put(wb, "A3", "30");
    put(wb, "A4", "apple");
    put(wb, "B1", "=A1*2");
    put(wb, "B2", "=B1+A2");
    put(wb, "C1", "=SUM(A1:A4)");
    put(wb, "C2", "=AVERAGE(A1:A3)");
    put(wb, "C3", "=A1/0");
    put(wb, "C4", "=C3+1");
    put(wb, "C5", "=FOO(1)");
    put(wb, "C6", "=A4*2");
    put(wb, "C7", "=Nope!A1");
    auto st = wb.recalc();
    CHECK(st.formulas == 9 && st.errors == 5);
    CHECK(numAt(wb, "B1") == 20 && numAt(wb, "B2") == 40);
    CHECK(numAt(wb, "C1") == 60 && numAt(wb, "C2") == 20);
    CHECK(val(wb, "C3").text == "#DIV/0!" && val(wb, "C4").text == "#DIV/0!");
    CHECK(val(wb, "C5").text == "#NAME?" && val(wb, "C6").text == "#VALUE!" && val(wb, "C7").text == "#REF!");

    // precedence and operators
    CHECK(evalNum(wb, "1+2*3") == 7);
    CHECK(evalNum(wb, "(1+2)*3") == 9);
    CHECK(evalNum(wb, "-2^2") == 4);  // Excel: negation binds tighter than ^
    CHECK(evalNum(wb, "2^3^2") == 64);
    CHECK(evalNum(wb, "50%") == 0.5);
    CHECK(evalNum(wb, "10-2-3") == 5);
    CHECK(evalText(wb, "\"a\"&1+1") == "a2");
    CHECK(evalText(wb, "1+1=2") == "TRUE");
    CHECK(evalText(wb, "\"abc\"<\"ABD\"") == "TRUE");
    CHECK(evalText(wb, "\"x\"\"y\"") == "x\"y");
    CHECK(evalText(wb, "0.1+0.2&\"\"") == "0.3");
    CHECK(formulaError("SUM(A1") != "" && formulaError("1+") != "" && formulaError("SUM(A1:B2)").empty());

    // absolute references and copying
    CHECK(offsetFormula("A1+$B$2+C$3+$D4", 2, 1) == "B3+$B$2+D$3+$D6");
    CHECK(offsetFormula("SUM(A1:A3)", -1, 0) == "SUM(#REF!)");
    CHECK(offsetFormula("Sheet2!A1*2", 1, 1) == "Sheet2!B2*2");

    // functions
    CHECK(evalNum(wb, "MIN(A1:A3)") == 10 && evalNum(wb, "MAX(A1:A3,99)") == 99);
    CHECK(evalNum(wb, "COUNT(A1:A4)") == 3 && evalNum(wb, "COUNTA(A1:A4)") == 4);
    CHECK(evalText(wb, "IF(A1>5,\"big\",\"small\")") == "big");
    CHECK(evalText(wb, "IFS(A1>50,\"a\",A1>5,\"b\")") == "b");
    CHECK(evalText(wb, "IFS(A1>50,\"a\")") == "#N/A");
    CHECK(evalText(wb, "AND(TRUE,A1>5)") == "TRUE" && evalText(wb, "OR(FALSE,A1<5)") == "FALSE" && evalText(wb, "NOT(FALSE)") == "TRUE");
    CHECK(evalNum(wb, "ROUND(2.675,2)") == 2.68 && evalNum(wb, "ROUND(-2.5,0)") == -3 && evalNum(wb, "ROUND(1234,-2)") == 1200);
    CHECK(evalNum(wb, "ROUNDUP(1.21,1)") == 1.3 && evalNum(wb, "ROUNDDOWN(-1.29,1)") == -1.2);
    CHECK(evalNum(wb, "ABS(-3)") == 3 && evalNum(wb, "SQRT(16)") == 4 && evalNum(wb, "POWER(2,10)") == 1024);
    CHECK(evalNum(wb, "MOD(-7,3)") == 2 && evalNum(wb, "INT(-1.5)") == -2);
    CHECK(evalText(wb, "SQRT(-1)") == "#NUM!" && evalText(wb, "MOD(1,0)") == "#DIV/0!");
    CHECK(evalNum(wb, "LEN(\"caf\xC3\xA9\")") == 4);
    CHECK(evalText(wb, "LEFT(\"Hello\",2)&RIGHT(\"Hello\",3)&MID(\"Hello\",2,3)") == "Helloell");
    CHECK(evalText(wb, "UPPER(\"ab\")&LOWER(\"CD\")") == "ABcd");
    CHECK(evalText(wb, "TRIM(\"  a   b  \")") == "a b");
    CHECK(evalText(wb, "CONCAT(A1:A2,\"-\",A4)") == "1020-apple");
    CHECK(evalText(wb, "TEXTJOIN(\", \",TRUE,A1:A4,\"\")") == "10, 20, 30, apple");
    CHECK(evalText(wb, "SUBSTITUTE(\"a-b-c\",\"-\",\"+\")") == "a+b+c");
    CHECK(evalText(wb, "TEXT(0.5,\"0%\")") == "50%");
    CHECK(evalNum(wb, "IFERROR(1/0,7)") == 7);
    CHECK(evalNum(wb, "SUMIF(A1:A3,\">15\")") == 50 && evalNum(wb, "COUNTIF(A1:A4,\"<>apple\")") == 3);
    CHECK(evalNum(wb, "AVERAGEIF(A1:A3,\">=20\")") == 25 && evalNum(wb, "COUNTIF(A1:A4,\"ap*\")") == 1);
    CHECK(evalNum(wb, "SUMIF(A1:A4,\"apple\",B1:B4)") == 0);
    CHECK(evalNum(wb, "SUMIFS(A1:A3,A1:A3,\">15\",A1:A3,\"<30\")") == 20 && evalNum(wb, "COUNTIFS(A1:A4,\"<>apple\",A1:A4,\">10\")") == 2);
    CHECK(evalNum(wb, "MAXIFS(A1:A3,A1:A3,\"<30\")") == 20 && evalNum(wb, "MINIFS(A1:A3,A1:A3,\">10\")") == 20);
    CHECK(evalNum(wb, "AVERAGEIFS(A1:A3,A1:A3,\">10\")") == 25 && evalText(wb, "SUMIFS(A1:A3,A1:A2,\">0\")") == "#VALUE!");
    CHECK(evalText(wb, "SWITCH(A1,10,\"ten\",20,\"twenty\")") == "ten" && evalText(wb, "SWITCH(A2,10,\"ten\",\"x\")") == "x");
    CHECK(evalText(wb, "SWITCH(A3,10,\"ten\")") == "#N/A");
    CHECK(evalNum(wb, "DATE(2024,3,1)") == 45352 && evalNum(wb, "DATE(2023,14,1)") == dateSerial(2024, 2, 1));
    CHECK(evalNum(wb, "YEAR(45352)") == 2024 && evalNum(wb, "MONTH(45352)") == 3 && evalNum(wb, "DAY(45352)") == 1);
    CHECK(evalNum(wb, "TODAY()") > 45000 && evalNum(wb, "NOW()") >= evalNum(wb, "TODAY()"));
    CHECK(evalNum(wb, "YEAR(\"2024-07-04\")") == 2024);
}

void testLookups() {
    Workbook wb = book();
    const char* rows[][3] = {{"id", "name", "price"}, {"1", "apple", "1.5"}, {"2", "pear", "2"}, {"3", "plum", "3.25"}, {"5", "fig", "4"}};
    for (int r = 0; r < 5; ++r) {
        for (int c = 0; c < 3; ++c) setInput(wb.sheets[0].at(r, c), rows[r][c]);
    }
    wb.recalc();
    CHECK(evalText(wb, "VLOOKUP(2,A2:C5,2,FALSE)") == "pear");
    CHECK(evalNum(wb, "VLOOKUP(4,A2:C5,3)") == 3.25);  // approximate: largest id <= 4
    CHECK(evalText(wb, "VLOOKUP(9,A2:C5,2,FALSE)") == "#N/A");
    CHECK(evalText(wb, "VLOOKUP(2,A2:C5,4,FALSE)") == "#REF!");
    CHECK(evalText(wb, "VLOOKUP(\"PLUM\",B2:C5,2,FALSE)") == "3.25");
    CHECK(evalNum(wb, "MATCH(\"plum\",B1:B5,0)") == 4);
    CHECK(evalNum(wb, "MATCH(4,A2:A5)") == 3);
    CHECK(evalNum(wb, "INDEX(A1:C5,3,3)") == 2);
    CHECK(evalText(wb, "INDEX(B2:B5,MATCH(5,A2:A5,0))") == "fig");
    CHECK(evalText(wb, "INDEX(A1:C1,2)") == "name");
    CHECK(evalNum(wb, "SUM(INDEX(A1:C5,0,3))") == 10.75);
    CHECK(evalText(wb, "XLOOKUP(\"fig\",B2:B5,C2:C5)") == "4");
    CHECK(evalText(wb, "XLOOKUP(\"kiwi\",B2:B5,C2:C5,\"none\")") == "none");
    CHECK(evalNum(wb, "XLOOKUP(4,A2:A5,C2:C5,,-1)") == 3.25);
    CHECK(evalNum(wb, "XLOOKUP(4,A2:A5,C2:C5,,1)") == 4);
    CHECK(evalText(wb, "XLOOKUP(\"p*\",B2:B5,B2:B5,,2,-1)") == "plum");
    CHECK(evalText(wb, "HLOOKUP(\"price\",A1:C5,3,FALSE)") == "2");
}

void testRecalcAndCycles() {
    Workbook wb = book();
    put(wb, "A1", "1");
    put(wb, "A2", "=A1+1");
    put(wb, "A3", "=A2*10");
    wb.recalc();
    CHECK(numAt(wb, "A3") == 20);
    put(wb, "A1", "5");
    wb.recalc();
    CHECK(numAt(wb, "A3") == 60);
    // a formula written before its inputs still sees them
    put(wb, "D1", "=D2+D3");
    put(wb, "D2", "=D3*2");
    put(wb, "D3", "4");
    wb.recalc();
    CHECK(numAt(wb, "D1") == 12);
    // cycles
    put(wb, "E1", "=E2+1");
    put(wb, "E2", "=E1+1");
    put(wb, "E3", "=E3");
    auto st = wb.recalc();
    CHECK(val(wb, "E1").text == "#CYCLE!" && val(wb, "E2").text == "#CYCLE!" && val(wb, "E3").text == "#CYCLE!");
    CHECK(st.cycles == 3);
    // a long reversed chain does not blow the stack
    Workbook big = book();
    const int n = 20000;
    for (int r = 0; r < n - 1; ++r) setInput(big.sheets[0].at(r, 0), "=A" + std::to_string(r + 2) + "+1");
    setInput(big.sheets[0].at(n - 1, 0), "1");
    big.recalc();
    CHECK(numAt(big, "A1") == n);
    // and a long cycle is still found
    setInput(big.sheets[0].at(n - 1, 0), "=A1");
    auto bs = big.recalc();
    CHECK(val(big, "A1").text == "#CYCLE!" && bs.cycles > 0);
    // cross-sheet references
    Workbook two = book();
    two.sheets.emplace_back();
    two.sheets[1].name = "Rates";
    setInput(two.sheets[1].at(0, 0), "0.2");
    put(two, "A1", "100");
    put(two, "B1", "=A1*Rates!A1");
    put(two, "B2", "=A1*Rates!$A$1+'Rates'!A1");
    two.recalc();
    CHECK(near(numAt(two, "B1"), 20) && near(numAt(two, "B2"), 20.2));
}

void testStructural() {
    Workbook wb = book();
    put(wb, "A1", "1");
    put(wb, "A2", "2");
    put(wb, "A3", "3");
    put(wb, "B1", "=SUM(A1:A3)");
    put(wb, "B2", "=A3*2");
    put(wb, "B3", "=$A$2");
    wb.sheets.emplace_back();
    wb.sheets[1].name = "Other";
    setInput(wb.sheets[1].at(0, 0), "=Sheet1!A3+1");
    wb.recalc();
    wb.insertRows(0, 1, 2);  // two rows before row 2
    wb.recalc();
    CHECK(wb.sheets[0].get(0, 1)->formula == "SUM(A1:A5)");
    CHECK(wb.sheets[0].get(3, 1)->formula == "A5*2");
    CHECK(wb.sheets[0].get(4, 1)->formula == "$A$4");
    CHECK(wb.sheets[1].get(0, 0)->formula == "Sheet1!A5+1");
    CHECK(numAt(wb, "B1") == 6 && numAt(wb, "Other!A1") == 4);
    wb.deleteRows(0, 4, 1);  // delete row 5 (the 3)
    wb.recalc();
    CHECK(wb.sheets[0].get(0, 1)->formula == "SUM(A1:A4)");
    CHECK(wb.sheets[0].get(3, 1)->formula == "#REF!*2");
    CHECK(val(wb, "B4").text == "#REF!" && numAt(wb, "B1") == 3);
    wb.insertCols(0, 0, 1);
    wb.recalc();
    CHECK(wb.sheets[0].get(0, 2)->formula == "SUM(B1:B4)" && numAt(wb, "C1") == 3);
    wb.deleteCols(0, 0, 1);
    wb.renameSheet(0, "Main Data");
    CHECK(wb.sheets[1].get(0, 0)->formula == "#REF!+1" || wb.sheets[1].get(0, 0)->formula.find("'Main Data'!") == 0);
    Workbook w2 = book();
    w2.sheets.emplace_back();
    w2.sheets[1].name = "B";
    setInput(w2.sheets[1].at(0, 0), "=Sheet1!A1*2");
    w2.renameSheet(0, "Main Data");
    CHECK(w2.sheets[1].get(0, 0)->formula == "'Main Data'!A1*2");
    w2.deleteSheet(0);
    CHECK(w2.sheets.size() == 1 && w2.sheets[0].get(0, 0)->formula == "#REF!*2");
}

void testCsv() {
    Workbook wb = readCsv("name,qty,price\n\"Smith, J\",3,1.50\nLee,,\"2\"\n\"multi\nline\",=B2*2,x\n");
    const Sheet& s = wb.sheets[0];
    CHECK(s.get(1, 0)->value.text == "Smith, J");
    CHECK(s.get(1, 1)->value.number == 3 && s.get(2, 2)->value.number == 2);
    CHECK(s.get(2, 1) == nullptr);
    CHECK(s.get(3, 0)->value.text == "multi\nline");
    CHECK(s.get(3, 1)->value.number == 6);
    std::string out = writeCsv(s);
    CHECK(out.find("\"Smith, J\",3,1.5\r\n") != std::string::npos);
    CHECK(out.find("\"multi\nline\",6,x") != std::string::npos);
    CHECK(sniffDelimiter("a;b;c\n1;2;3\n") == ';' && sniffDelimiter("a\tb\n1\t2\n") == '\t' && sniffDelimiter("a,b\n") == ',');
    Workbook t = readCsv("a\tb\n1\t2", '\t');
    CHECK(t.sheets[0].get(1, 1)->value.number == 2);
}

void testXlsxRoundTrip() {
    Workbook wb = book();
    wb.sheets.emplace_back();
    wb.sheets[1].name = "Second sheet";
    put(wb, "A1", "Item");
    wb.sheets[0].at(0, 0).style.bold = true;
    put(wb, "B1", "Cost");
    put(wb, "A2", "Rent & bills <home>");
    put(wb, "B2", "1200.5");
    wb.sheets[0].at(1, 1).format = "$#,##0.00";
    put(wb, "A3", "Food");
    put(wb, "B3", "310");
    put(wb, "B4", "=SUM(B2:B3)");
    put(wb, "C4", "=IFS(B4>1000,\"high\",TRUE,\"low\")");
    put(wb, "D4", "=1/0");
    put(wb, "E4", "2024-05-06");
    put(wb, "F4", "TRUE");
    wb.sheets[0].colWidths[0] = 24;
    setInput(wb.sheets[1].at(0, 0), "='Sheet1'!B4*2");
    wb.recalc();
    std::string bytes = writeXlsx(wb);
    Workbook back = readXlsx(bytes);
    CHECK(back.sheets.size() == 2 && back.sheets[1].name == "Second sheet");
    const Sheet& s = back.sheets[0];
    CHECK(s.get(0, 0)->value.text == "Item" && s.get(0, 0)->style.bold);
    CHECK(s.get(1, 0)->value.text == "Rent & bills <home>");
    CHECK(s.get(1, 1)->value.number == 1200.5 && s.get(1, 1)->format == "$#,##0.00");
    CHECK(s.get(3, 1)->formula == "SUM(B2:B3)" && s.get(3, 1)->value.number == 1510.5);
    CHECK(s.get(3, 2)->formula == "IFS(B4>1000,\"high\",TRUE,\"low\")" && s.get(3, 2)->value.text == "high");
    CHECK(s.get(3, 3)->value.text == "#DIV/0!");
    CHECK(s.get(3, 4)->value.number == dateSerial(2024, 5, 6) && s.get(3, 4)->format == "yyyy-mm-dd");
    CHECK(s.get(3, 5)->value.type == CellValue::Bool);
    CHECK(near(s.colWidths.at(0), 24));
    CHECK(back.sheets[1].get(0, 0)->value.number == 3021);
    CHECK(bytes.find("_xlfn.IFS") != std::string::npos);
    back.recalc();
    CHECK(back.sheets[1].get(0, 0)->value.number == 3021);
}

TextDocument sampleDoc() {
    TextDocument d = readMarkdown(
        "---\ntitle: Quarterly report\nauthor: Ada\n---\n"
        "# Results\n\nRevenue grew **12%** in *Q3*, see [the site](https://example.com) and `code`.\n"
        "A second line\n\n"
        "## Details\n\n"
        "- first point\n- second point\n    - nested point\n- third\n\n"
        "1. one\n2. two\n\n"
        "| Region | Sales |\n|---|---:|\n| North | 1,200 |\n| South | 900 |\n\n"
        "> A wise quote\n\n"
        "```\nint main() {}\n```\n\n"
        "<!-- pagebreak -->\n\n"
        "Snake_case_words and a <u>underlined</u> bit.\n");
    return d;
}

void testInlineMarkdown() {
    Runs r = parseInline("plain **bold** *it* ***both*** `x*y` [link **b**](http://a.b) <u>u</u> a*b 2 * 3 snake_case \\*lit\\*");
    CHECK(plainText(r) == "plain bold it both x*y link b u a*b 2 * 3 snake_case *lit*");
    bool sawBold = false, sawBoth = false, sawCode = false, sawLinkBold = false, sawUnder = false;
    for (const auto& x : r) {
        if (x.text == "bold") sawBold = x.bold && !x.italic;
        if (x.text == "both") sawBoth = x.bold && x.italic;
        if (x.text == "x*y") sawCode = x.code;
        if (x.text == "b" && x.link == "http://a.b") sawLinkBold = x.bold;
        if (x.text == "u") sawUnder = x.underline;
    }
    CHECK(sawBold && sawBoth && sawCode && sawLinkBold && sawUnder);
    CHECK(parseInline(inlineMarkdown(r)).size() == r.size());
    CHECK(plainText(parseInline(inlineMarkdown(r))) == plainText(r));
    CHECK(inlineMarkdown(parseInline("a **b** c")) == "a **b** c");
}

void testMarkdown() {
    TextDocument d = sampleDoc();
    CHECK(d.title == "Quarterly report" && d.author == "Ada");
    CHECK(d.blocks.size() == 10);
    CHECK(d.blocks[0].kind == Block::Heading && d.blocks[0].level == 1);
    CHECK(d.blocks[1].kind == Block::Paragraph && plainText(d.blocks[1].runs).find("A second line") != std::string::npos);
    CHECK(d.blocks[3].kind == Block::List && !d.blocks[3].ordered && d.blocks[3].items.size() == 4 && d.blocks[3].items[2].level == 1);
    CHECK(d.blocks[4].kind == Block::List && d.blocks[4].ordered && d.blocks[4].items.size() == 2);
    CHECK(d.blocks[5].kind == Block::Table && d.blocks[5].rows.size() == 3 && plainText(d.blocks[5].rows[1][1]) == "1,200");
    CHECK(d.blocks[6].kind == Block::Quote && d.blocks[7].kind == Block::Code && plainText(d.blocks[7].runs) == "int main() {}");
    CHECK(d.blocks[8].kind == Block::PageBreak);
    CHECK(plainText(d.blocks[9].runs) == "Snake_case_words and a underlined bit.");
    std::vector<MediaFile> media;
    std::string md = writeMarkdown(d, "m/", media);
    TextDocument again = readMarkdown(md);
    CHECK(again.blocks.size() == d.blocks.size());
    for (std::size_t i = 0; i < std::min(again.blocks.size(), d.blocks.size()); ++i) {
        CHECK(blockText(again.blocks[i]) == blockText(d.blocks[i]));
    }
    // notes-style Markdown survives a save: task boxes and [[links]] stay as written
    {
        std::vector<MediaFile> none;
        const std::string notes = writeMarkdown(readMarkdown("# Groceries\n\n- [ ] milk\n- [x] eggs\n\nsee [[Plans]] and [a link](x.md)\n"), "m/", none);
        CHECK(notes.find("- [ ] milk") != std::string::npos && notes.find("- [x] eggs") != std::string::npos);
        CHECK(notes.find("see [[Plans]]") != std::string::npos && notes.find("[a link](x.md)") != std::string::npos);
    }
    // images: a file next to the markdown
    raster::Image px(8, 4, {255, 0, 0, 255});
    writeFile(tmp("red.png"), raster::encodePng(px));
    TextDocument withImg = readMarkdown("Intro\n\n![red box](red.png)\n", tmp("").string());
    CHECK(withImg.blocks.size() == 2 && withImg.blocks[1].kind == Block::Image && withImg.blocks[1].alt == "red box");
    media.clear();
    std::string md2 = writeMarkdown(withImg, "pics/", media);
    CHECK(media.size() == 1 && media[0].path == "pics/image1.png" && md2.find("![red box](pics/image1.png)") != std::string::npos);
}

void testHtml() {
    TextDocument d = sampleDoc();
    std::string html = writeHtml(d);
    CHECK(html.find(">Results</h1>") != std::string::npos && html.find("<strong>12%</strong>") != std::string::npos);
    CHECK(html.find("<a href=\"https://example.com\">the site</a>") != std::string::npos);
    TextDocument back = readHtml(html);
    CHECK(back.title == "Quarterly report");
    CHECK(back.blocks.size() == d.blocks.size());
    for (std::size_t i = 0; i < std::min(back.blocks.size(), d.blocks.size()); ++i) {
        CHECK(back.blocks[i].kind == d.blocks[i].kind);
        CHECK(blockText(back.blocks[i]) == blockText(d.blocks[i]));
    }
    TextDocument h = readHtml("<html><head><title>T</title><style>p{}</style></head><body><h2>Hi &amp; bye</h2><p>One<br>two <b>bold</b></p>"
                          "<ul><li>a<ul><li>b</li></ul></li><li>c</li></ul><script>var x;</script><table><tr><th>A</th><th>B</th></tr>"
                          "<tr><td>1</td><td>2</td></tr></table></body></html>");
    CHECK(h.title == "T" && h.blocks.size() == 4);
    CHECK(h.blocks[0].kind == Block::Heading && h.blocks[0].level == 2 && plainText(h.blocks[0].runs) == "Hi & bye");
    CHECK(plainText(h.blocks[1].runs) == "One\ntwo bold");
    CHECK(h.blocks[2].items.size() == 3 && h.blocks[2].items[1].level == 1);
    CHECK(h.blocks[3].kind == Block::Table && h.blocks[3].header && plainText(h.blocks[3].rows[1][1]) == "2");
}

void testDocx() {
    TextDocument d = sampleDoc();
    raster::Image px(40, 20, {0, 128, 255, 255});
    Block img;
    setImage(img, raster::encodePng(px));
    img.alt = "blue";
    d.blocks.push_back(img);
    std::string bytes = writeDocx(d);
    TextDocument back = readDocx(bytes);
    CHECK(back.title == "Quarterly report" && back.author == "Ada");
    CHECK(back.blocks.size() == d.blocks.size());
    for (std::size_t i = 0; i < std::min(back.blocks.size(), d.blocks.size()); ++i) {
        CHECK(back.blocks[i].kind == d.blocks[i].kind);
        CHECK(blockText(back.blocks[i]) == blockText(d.blocks[i]));
    }
    CHECK(back.blocks[3].items[2].level == 1 && back.blocks[4].ordered);
    bool linkKept = false, boldKept = false;
    for (const auto& r : back.blocks[1].runs) {
        if (r.text == "the site") linkKept = r.link == "https://example.com";
        if (r.text == "12%") boldKept = r.bold;
    }
    CHECK(linkKept && boldKept);
    CHECK(back.blocks.back().kind == Block::Image && back.blocks.back().pixels && back.blocks.back().pixels->pixels.width() == 40);
    CHECK(near(back.blocks.back().width, 30, 0.01));

    if (auto soffice = findProgram("soffice")) {
        // LibreOffice opens what we write, and we read what it writes
        fs::path dir = tmp("lo");
        fs::create_directories(dir);
        writeFile(dir / "report.docx", bytes);
        auto r = runProgram({soffice->string(), "--headless", "--norestore", "--convert-to", "odt", "--outdir", dir.string(), (dir / "report.docx").string()});
        CHECK(r.exitCode == 0 && fs::exists(dir / "report.odt"));
        if (fs::exists(dir / "report.odt")) {
            TextDocument odt = readOdt(readFile(dir / "report.odt"));
            CHECK(odt.blocks.size() >= 9);
            bool sawHeading = false, sawTable = false, sawList = false, sawImage = false;
            for (const auto& b : odt.blocks) {
                sawHeading = sawHeading || (b.kind == Block::Heading && plainText(b.runs) == "Results");
                sawTable = sawTable || (b.kind == Block::Table && b.rows.size() == 3);
                sawList = sawList || (b.kind == Block::List && b.items.size() == 4 && b.items[2].level == 1);
                sawImage = sawImage || b.kind == Block::Image;
            }
            CHECK(sawHeading && sawTable && sawList && sawImage);
        }
        auto r2 = runProgram({soffice->string(), "--headless", "--norestore", "--convert-to", "pdf", "--outdir", dir.string(), (dir / "report.docx").string()});
        CHECK(r2.exitCode == 0 && fs::exists(dir / "report.pdf"));
        if (fs::exists(dir / "report.pdf")) {
            pdf::PdfFile f = pdf::PdfFile::load(dir / "report.pdf");
            std::string text;
            for (int i = 0; i < f.pageCount(); ++i) text += f.pageText(i);
            CHECK(text.find("Revenue grew") != std::string::npos && text.find("North") != std::string::npos);
        }
    }
}

void testLayout() {
    TextDocument d = sampleDoc();
    auto pages = layoutDocument(d);
    CHECK(pages.size() == 2);  // the page break
    std::string pdfBytes = pdf::writePdf(pages, {d.title, d.author, ""});
    pdf::PdfFile f(pdfBytes);
    CHECK(f.pageCount() == 2);
    std::string t = f.pageText(0);
    CHECK(t.find("Results") != std::string::npos && t.find("Revenue grew 12% in Q3") != std::string::npos);
    CHECK(t.find("North") != std::string::npos && t.find("1 / 2") != std::string::npos);
    CHECK(f.pageText(1).find("Snake_case_words") != std::string::npos);
    // long documents flow onto more pages, tables repeat their header
    TextDocument big;
    Block table;
    table.kind = Block::Table;
    for (int i = 0; i < 120; ++i) table.rows.push_back({parseInline("row " + std::to_string(i)), parseInline("value")});
    big.blocks.push_back(table);
    auto bp = layoutDocument(big);
    CHECK(bp.size() >= 3);
    pdf::PdfFile bf(pdf::writePdf(bp));
    CHECK(bf.pageText(1).find("row 0") != std::string::npos);  // header row repeated
    raster::Image img = pdf::renderPage(pages[0], 1.0);
    int dark = 0;
    for (int y = 0; y < img.height(); y += 2) {
        for (int x = 0; x < img.width(); x += 2) dark += img.at(x, y).r < 128 ? 1 : 0;
    }
    CHECK(dark > 200);
    writeFile(tmp("layout.png"), raster::encodePng(img));
}

void testDeck() {
    auto items = parseBullets("- Revenue up **12%**\n  - North strongest\n  - South flat\n* Costs down\n1. Hiring paused\n\n    deep");
    CHECK(items.size() == 6);
    if (items.size() == 6) {
        CHECK(items[0].level == 0 && items[1].level == 1 && items[2].level == 1 && items[3].level == 0 && items[4].level == 0);
        CHECK(items[5].level == 1);
        CHECK(plainText(items[4].runs) == "Hiring paused");
        CHECK(items[0].runs.size() == 2 && items[0].runs[1].bold);
    }
    CHECK(bulletText(parseBullets(bulletText(items))) == bulletText(items));
    bool numbered = false;
    auto steps = parseBullets("1. one\n   - detail\n2. two", &numbered);
    CHECK(numbered && steps.size() == 3 && bulletText(steps, true) == "1. one\n  - detail\n2. two\n");
    parseBullets("1. one\n- two", &numbered);
    CHECK(!numbered);

    Theme t;
    std::string err;
    CHECK(parseTheme("teal", t, err) && t.name == "teal");
    CHECK(parseTheme("#ff8800", t, err) && t.accent.r == 0xFF && t.accent.g == 0x88 && t.name == "#ff8800");
    CHECK(!parseTheme("no-such-theme", t, err) && !err.empty());

    Deck d;
    d.title = "Plan";
    d.author = "Ada";
    Slide s1;
    s1.layout = "title";
    s1.title = "Quarterly plan";
    s1.subtitle = "Q3 2026";
    s1.notes = "Welcome everyone";
    Slide s2;
    s2.title = "Highlights";
    s2.bullets = items;
    s2.notes = "Mention the north\nand the south";
    Slide s3;
    s3.layout = "two_column";
    s3.title = "Compare";
    s3.bullets = parseBullets("Left one\nLeft two");
    s3.right = parseBullets("Right one\nRight [link](https://example.com)");
    Slide s4;
    s4.title = "Picture";
    raster::Image px(64, 48, {200, 40, 40, 255});
    setSlideImage(s4, raster::encodePng(px));
    s4.bullets = parseBullets("With text");
    s3.numbered = true;
    Slide s5;
    s5.layout = "section";
    s5.title = "Next steps";
    Slide s6;
    s6.title = "Many bullets";
    std::string many;
    for (int i = 0; i < 20; ++i) many += "Point number " + std::to_string(i) + " with some words to wrap around the line\n";
    s6.bullets = parseBullets(many);
    d.slides = {s1, s2, s3, s4, s5, s6};

    auto pages = layoutDeck(d);
    CHECK(pages.size() == 6 && near(pages[0].width, 960, 0.01) && near(pages[0].height, 540, 0.01));
    pdf::PdfFile f(pdf::writePdf(pages));
    CHECK(f.pageCount() == 6);
    CHECK(f.pageText(0).find("Quarterly plan") != std::string::npos);
    CHECK(f.pageText(1).find("North strongest") != std::string::npos);
    CHECK(f.pageText(2).find("Right one") != std::string::npos && f.pageText(2).find("1. Left one") != std::string::npos);
    CHECK(f.pageText(5).find("Point number 19") != std::string::npos);  // shrunk to fit
    // the overflow slide keeps its text inside the slide
    double maxY = 0;
    for (const auto& op : pages[5].ops) {
        if (auto* t = std::get_if<pdf::TextOp>(&op)) maxY = std::max(maxY, t->y);
    }
    CHECK(maxY < 540);
    CHECK(slideOverflow(d, 5).empty());
    Deck huge = d;
    huge.slides[5].bullets = parseBullets(many + many + many);
    CHECK(slideOverflow(huge, 5) == std::vector<std::string>{"bullets"});
    raster::Image img = pdf::renderPage(pages[0], 0.5);
    CHECK(img.width() == 480 && img.height() == 270);
    CHECK(img.at(5, 5) == raster::Color(d.theme.accent));  // title slide background
    writeFile(tmp("deck1.png"), raster::encodePng(img));
    writeFile(tmp("deck4.png"), raster::encodePng(pdf::renderPage(pages[3], 0.5)));
    writeFile(tmp("deck6.png"), raster::encodePng(pdf::renderPage(pages[5], 0.5)));

    std::string bytes = writePptx(d);
    Deck back = readPptx(bytes);
    CHECK(back.title == "Plan" && back.author == "Ada");
    CHECK(back.slides.size() == 6);
    if (back.slides.size() == 6) {
        CHECK(back.slides[0].layout == "title" && back.slides[0].title == "Quarterly plan" && back.slides[0].subtitle == "Q3 2026");
        CHECK(back.slides[0].notes == "Welcome everyone");
        CHECK(back.slides[1].notes == "Mention the north\nand the south");
        CHECK(bulletText(back.slides[1].bullets) == bulletText(items));
        CHECK(back.slides[2].layout == "two_column" && back.slides[2].right.size() == 2);
        CHECK(back.slides[2].numbered && !back.slides[1].numbered);
        CHECK(back.slides[2].right.size() == 2 && back.slides[2].right[1].runs.back().link == "https://example.com");
        CHECK(back.slides[3].pixels && back.slides[3].pixels->pixels.width() == 64);
        CHECK(back.slides[4].layout == "section" && back.slides[4].title == "Next steps");
        CHECK(back.slides[5].bullets.size() == 20);
    }
    CHECK(near(back.width, 960, 0.01));
    CHECK(back.theme.name == "blue");

    if (auto soffice = findProgram("soffice")) {
        fs::path dir = tmp("lo_deck");
        fs::create_directories(dir);
        writeFile(dir / "plan.pptx", bytes);
        auto r = runProgram({soffice->string(), "--headless", "--norestore", "--convert-to", "pdf", "--outdir", dir.string(), (dir / "plan.pptx").string()});
        CHECK(r.exitCode == 0 && fs::exists(dir / "plan.pdf"));
        if (fs::exists(dir / "plan.pdf")) {
            pdf::PdfFile lo = pdf::PdfFile::load(dir / "plan.pdf");
            CHECK(lo.pageCount() == 6);
            std::string text;
            for (int i = 0; i < lo.pageCount(); ++i) text += lo.pageText(i);
            CHECK(text.find("Quarterly plan") != std::string::npos && text.find("North strongest") != std::string::npos);
        }
        // and we read what LibreOffice writes
        auto r2 = runProgram({soffice->string(), "--headless", "--norestore", "--convert-to", "odp", "--outdir", dir.string(), (dir / "plan.pptx").string()});
        if (r2.exitCode == 0 && fs::exists(dir / "plan.odp")) {
            fs::path again = dir / "again";
            fs::create_directories(again);
            auto r3 = runProgram({soffice->string(), "--headless", "--norestore", "--convert-to", "pptx", "--outdir", again.string(), (dir / "plan.odp").string()});
            CHECK(r3.exitCode == 0 && fs::exists(again / "plan.pptx"));
            if (fs::exists(again / "plan.pptx")) {
                Deck lo = readPptx(readFile(again / "plan.pptx"));
                CHECK(lo.slides.size() == 6);
                if (lo.slides.size() == 6) {
                    CHECK(lo.slides[1].title == "Highlights");
                    CHECK(lo.slides[1].bullets.size() == items.size());
                    CHECK(lo.slides[0].notes == "Welcome everyone");
                }
            }
        }
    }
}

void testChart() {
    Workbook wb = readCsv("Month,Sales,Costs\nJan,120,80\nFeb,150,95\nMar,90,100\nApr,170,110\n");
    Range r;
    std::string err;
    CHECK(parseRange(wb, "A1:C5", 0, r, err));
    Chart c = chartFromRange(wb, r);
    CHECK(c.categories.size() == 4 && c.categories[0] == "Jan");
    CHECK(c.series.size() == 2 && c.series[0].name == "Sales" && c.series[1].values[3] == 110);
    Chart rows = chartFromRange(wb, r, true);
    CHECK(rows.categories.size() == 2 && rows.categories[1] == "Costs" && rows.series.size() == 4 && rows.series[0].name == "Jan");
    Range nums;
    CHECK(parseRange(wb, "B2:B5", 0, nums, err));
    Chart plain = chartFromRange(wb, nums);
    CHECK(plain.series.size() == 1 && plain.categories[0] == "1");
    Range text;
    CHECK(parseRange(wb, "A2:A5", 0, text, err));
    bool threw = false;
    try {
        chartFromRange(wb, text);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
    for (const auto& type : chartTypes()) {
        c.type = type;
        c.title = "Sales by month";
        raster::Image img = renderChart(c);
        CHECK(img.width() == 800 && img.height() == 500);
        int colored = 0;
        for (int y = 0; y < img.height(); y += 3) {
            for (int x = 0; x < img.width(); x += 3) {
                auto p = img.at(x, y);
                colored += (p.r == 0x2B && p.g == 0x57 && p.b == 0x9A) ? 1 : 0;
            }
        }
        if (colored <= 50) std::printf("  %s chart: %d coloured samples\n", type.c_str(), colored);
        CHECK(colored > (type == "scatter" ? 5 : 50));
        writeFile(tmp("chart_" + type + ".png"), raster::encodePng(img));
    }
    // grid preview and sheet pages
    put(wb, "D1", "Margin");
    put(wb, "D2", "=B2-C2");
    wb.sheets[0].at(0, 0).style.bold = true;
    wb.recalc();
    Range all;
    CHECK(parseRange(wb, "A1:D5", 0, all, err));
    pdf::Page grid = sheetGridPage(wb, all, true);
    CHECK(grid.width > 200 && grid.height > 80);
    pdf::PdfFile gf(pdf::writePdf({grid}));
    CHECK(gf.pageText(0).find("Margin") != std::string::npos && gf.pageText(0).find("40") != std::string::npos);
    writeFile(tmp("grid.png"), raster::encodePng(pdf::renderPage(grid, 2)));
    Workbook big;
    big.sheets.push_back(Sheet{"Big", {}, {}});
    for (int i = 0; i < 100; ++i) big.sheets[0].at(i, 0).value = CellValue::num(i);
    auto pages = layoutSheet(big, 0);
    CHECK(pages.size() == 4);
}

std::string txt(const Result& r) { return toText(r.value); }
std::string txt(const Result& r, const char* key) {
    const Value* v = r.field(key);
    return v ? toText(*v) : std::string();
}

Params P(std::initializer_list<std::pair<const std::string, Value>> l) { return Params(l); }

void testSheetCommands() {
    Session s;
    registerCommands(s);
    Result r = s.run("office.sheet_new", P({{"csv", std::string("Region,Sales\nNorth,100\nSouth,50\nEast,75")}}));
    CHECK(r.ok && txt(r) == "sheet1");
    r = s.run("office.sheet_set", P({{"range", std::string("C1")}, {"value", std::string("Double")}}));  // doc is optional with one open
    CHECK(r.ok);
    r = s.run("office.sheet_set", P({{"range", std::string("C2:C4")}, {"value", std::string("=B2*2")}}));
    CHECK(r.ok && txt(r) == "200");
    r = s.run("office.sheet_get", P({{"range", std::string("C4")}}));
    CHECK(r.ok && txt(r) == "150" && r.message.find("=B4*2") != std::string::npos && r.message.find("reads B4") != std::string::npos);
    r = s.run("office.sheet_get", P({{"range", std::string("B2")}}));
    CHECK(r.message.find("used by C2") != std::string::npos);
    r = s.run("office.sheet_set", P({{"range", std::string("D1")}, {"value", std::string("=SUM(B2:B4")}}));
    CHECK(!r.ok);  // syntax errors are refused
    r = s.run("office.sheet_set", P({{"range", std::string("D1")}, {"csv", std::string("a\\tb\\n1\\t2")}}));  // escaped newlines and tabs
    CHECK(r.ok && r.message.find("D1:E2") != std::string::npos);
    CHECK(s.history().undo());
    r = s.run("office.sheet_get", P({{"range", std::string("D1")}}));
    CHECK(r.ok && r.message.find("empty") != std::string::npos);

    r = s.run("office.sheet_sort", P({{"by", std::string("Sales")}}));
    CHECK(r.ok && r.message.find("header kept") != std::string::npos);
    r = s.run("office.sheet_get", P({{"range", std::string("A2")}}));
    CHECK(txt(r) == "South");
    r = s.run("office.sheet_get", P({{"range", std::string("C2")}}));
    CHECK(txt(r) == "100" && r.message.find("=B2*2") != std::string::npos);  // formulas move with their rows
    r = s.run("office.sheet_sort", P({{"by", std::string("B desc")}}));
    CHECK(r.ok);
    CHECK(txt(s.run("office.sheet_get", P({{"range", std::string("A2")}}))) == "North");
    CHECK(s.history().undo());
    CHECK(txt(s.run("office.sheet_get", P({{"range", std::string("A2")}}))) == "South");
    CHECK(s.history().redo());
    CHECK(txt(s.run("office.sheet_get", P({{"range", std::string("A2")}}))) == "North");

    r = s.run("office.sheet_filter", P({{"column", std::string("Sales")}, {"criteria", std::string(">60")}}));
    CHECK(r.ok && txt(r) == "2");
    r = s.run("office.sheet_info", {});
    CHECK(r.ok && r.message.find("Sheet1 filtered (active)") != std::string::npos);
    r = s.run("office.sheet_filter", P({{"sheet", std::string("1")}, {"column", std::string("A")}, {"criteria", std::string("<>East")}, {"mode", std::string("delete")}}));
    CHECK(r.ok && txt(r) == "2");
    r = s.run("office.sheet_get", P({{"sheet", std::string("Sheet1")}, {"format", std::string("csv")}}));
    CHECK(r.ok && txt(r) == "Region,Sales,Double\nNorth,100,200\nSouth,50,100\n");

    r = s.run("office.sheet_insert_rows", P({{"sheet", std::string("Sheet1")}, {"at", std::string("2")}}));
    CHECK(r.ok);
    r = s.run("office.sheet_get", P({{"range", std::string("Sheet1!C3")}}));
    CHECK(txt(r) == "200" && r.message.find("=B3*2") != std::string::npos);
    r = s.run("office.sheet_delete_rows", P({{"sheet", std::string("Sheet1")}, {"at", std::string("2")}}));
    CHECK(r.ok);
    r = s.run("office.sheet_insert_cols", P({{"sheet", std::string("Sheet1")}, {"at", std::string("B")}}));
    CHECK(r.ok);
    r = s.run("office.sheet_get", P({{"range", std::string("Sheet1!D2")}}));
    CHECK(r.message.find("=C2*2") != std::string::npos);
    r = s.run("office.sheet_delete_cols", P({{"sheet", std::string("Sheet1")}, {"at", std::string("B")}}));
    CHECK(r.ok);
    r = s.run("office.sheet_select", P({{"sheet", std::string("Sheet1")}}));
    CHECK(r.ok);

    r = s.run("office.sheet_fill", P({{"range", std::string("E1:E5")}, {"step", 2.0}}));
    CHECK(!r.ok);  // no number in E1
    r = s.run("office.sheet_set", P({{"range", std::string("E1")}, {"value", std::string("10")}}));
    r = s.run("office.sheet_fill", P({{"range", std::string("E1:E5")}, {"step", 2.0}}));
    CHECK(r.ok);
    CHECK(txt(s.run("office.sheet_get", P({{"range", std::string("E5")}}))) == "18");
    r = s.run("office.sheet_format", P({{"range", std::string("B:C")}, {"format", std::string("currency")}, {"autofit", true}}));
    CHECK(r.ok);
    r = s.run("office.sheet_get", P({{"range", std::string("B2")}}));
    CHECK(r.message.find("$100.00") != std::string::npos);
    r = s.run("office.sheet_calc", P({{"formula", std::string("=SUMIF(A:A,\"North\",B:B)")}}));
    CHECK(r.ok && txt(r) == "100");
    r = s.run("office.sheet_find", P({{"query", std::string("south")}}));
    CHECK(r.ok && txt(r) == "1");
    r = s.run("office.sheet_replace", P({{"find", std::string("South")}, {"replace", std::string("Southwest")}}));
    CHECK(r.ok && txt(r) == "1");
    r = s.run("office.sheet_add", P({{"name", std::string("Notes")}, {"csv", std::string("x\n=Sheet1!B2+1")}}));
    CHECK(r.ok);
    CHECK(txt(s.run("office.sheet_get", P({{"range", std::string("Notes!A2")}}))) == "101");
    r = s.run("office.sheet_rename", P({{"sheet", std::string("Sheet1")}, {"name", std::string("Data")}}));
    CHECK(r.ok);
    r = s.run("office.sheet_get", P({{"range", std::string("Notes!A2")}}));
    CHECK(r.message.find("Data!B2") != std::string::npos && txt(r) == "101");
    r = s.run("office.sheet_chart", P({{"sheet", std::string("Data")}, {"range", std::string("A1:B3")}, {"type", std::string("pie")}, {"path", tmp("cmd_chart.png").string()}}));
    CHECK(r.ok && r.attachments.size() == 1 && fs::exists(tmp("cmd_chart.png")));
    r = s.run("office.sheet_preview", {});
    CHECK(r.ok && r.attachments.size() == 1 && r.attachments[0].mime == "image/png");

    const std::string xlsx = tmp("cmd.xlsx").string();
    r = s.run("office.sheet_save", P({{"path", xlsx}}));
    CHECK(r.ok && fs::exists(xlsx));
    r = s.run("office.sheet_save", P({{"path", tmp("cmd.csv").string()}, {"sheet", std::string("Data")}}));
    CHECK(r.ok && readFile(tmp("cmd.csv")).find("Southwest") != std::string::npos);
    r = s.run("office.sheet_save", P({{"path", tmp("cmd_sheet.pdf").string()}}));
    CHECK(r.ok);
    r = s.run("office.sheet_save", P({{"path", tmp("cmd_sheet.html").string()}}));
    CHECK(r.ok && readFile(tmp("cmd_sheet.html")).find("<td class=\"n\">") != std::string::npos);
    r = s.run("office.sheet_open", P({{"path", xlsx}}));
    CHECK(r.ok && txt(r) == "sheet2");
    r = s.run("office.sheet_get", P({{"doc", std::string("sheet2")}, {"range", std::string("Notes!A2")}}));
    CHECK(txt(r) == "101");
    r = s.run("office.sheet_get", P({{"range", std::string("A1")}}));
    CHECK(!r.ok && r.message.find("several") != std::string::npos);  // two open: doc= is needed
    r = s.run("office.sheet_close", P({{"doc", std::string("sheet2")}}));
    CHECK(r.ok);
    CHECK(s.history().undo());
    CHECK(s.workspace().get("sheet2") != nullptr);
}

void testDocCommands() {
    Session s;
    registerCommands(s);
    Result r = s.run("office.doc_new", P({{"markdown", std::string("---\ntitle: Report\n---\n# Results\n\nSales grew **12%**.\n")}}));
    CHECK(r.ok && txt(r) == "doc1");
    r = s.run("office.doc_add", P({{"kind", std::string("list")}, {"text", std::string("one\\ntwo\\n  nested")}}));
    CHECK(r.ok && txt(r) == "3");
    r = s.run("office.doc_add", P({{"kind", std::string("table")}, {"text", std::string("Region | Sales\nNorth | 1,200\nSouth | 900")}}));
    CHECK(r.ok);
    raster::Image px(60, 30, {10, 150, 60, 255});
    writeFile(tmp("cmd_img.png"), raster::encodePng(px));
    r = s.run("office.doc_add", P({{"kind", std::string("image")}, {"path", tmp("cmd_img.png").string()}, {"alt", std::string("green")}}));
    CHECK(r.ok);
    r = s.run("office.doc_add", P({{"kind", std::string("heading")}, {"text", std::string("Intro")}, {"at", std::int64_t{1}}, {"level", std::int64_t{2}}}));
    CHECK(r.ok && txt(r) == "1");
    r = s.run("office.doc_info", {});
    CHECK(r.ok && r.message.find("1. heading 2: Intro") != std::string::npos && r.message.find("4. list (3 items)") != std::string::npos);
    r = s.run("office.doc_edit", P({{"block", std::int64_t{3}}, {"kind", std::string("quote")}}));
    CHECK(r.ok);
    r = s.run("office.doc_get_text", P({{"from", std::int64_t{3}}, {"to", std::int64_t{3}}}));
    CHECK(txt(r) == "[3] > Sales grew **12%**.");
    r = s.run("office.doc_move", P({{"block", std::int64_t{1}}, {"to", std::int64_t{2}}}));
    CHECK(r.ok);
    r = s.run("office.doc_find_replace", P({{"find", std::string("north")}}));
    CHECK(r.ok && txt(r) == "1");
    r = s.run("office.doc_find_replace", P({{"find", std::string("North")}, {"replace", std::string("Nord")}, {"match_case", true}}));
    CHECK(r.ok && txt(r) == "1");
    r = s.run("office.doc_word_count", {});
    CHECK(r.ok && txt(r) == "14");
    r = s.run("office.doc_setup", P({{"page", std::string("letter")}, {"font", std::string("serif")}, {"margin", 20.0}}));
    CHECK(r.ok);
    r = s.run("office.doc_preview", {});
    CHECK(r.ok && r.attachments.size() == 1);
    const std::string pdfPath = tmp("cmd_doc.pdf").string();
    r = s.run("office.doc_export", P({{"path", pdfPath}}));
    CHECK(r.ok);
    r = s.run("office.pdf_text", P({{"path", pdfPath}}));
    CHECK(r.ok && txt(r).find("Nord") != std::string::npos && txt(r).find("Sales grew 12%.") != std::string::npos);
    r = s.run("office.pdf_info", P({{"path", pdfPath}}));
    CHECK(r.ok && r.message.find("Letter") != std::string::npos && r.message.find("title: Report") != std::string::npos);
    const std::string docx = tmp("cmd.docx").string();
    r = s.run("office.doc_save", P({{"path", docx}}));
    CHECK(r.ok);
    for (const char* ext : {"md", "html", "txt"}) {
        r = s.run("office.doc_export", P({{"path", tmp(std::string("cmd_doc.") + ext).string()}}));
        CHECK(r.ok);
    }
    CHECK(fs::exists(tmp("cmd_doc_media/image1.png")));
    r = s.run("office.doc_open", P({{"path", tmp("cmd_doc.md").string()}}));
    CHECK(r.ok);
    r = s.run("office.doc_info", P({{"doc", txt(r)}}));
    CHECK(r.ok && r.message.find("image (60x30 px)") != std::string::npos);
    r = s.run("office.doc_open", P({{"path", docx}}));
    CHECK(r.ok);
    r = s.run("office.doc_delete", P({{"doc", txt(r)}, {"block", std::int64_t{1}}, {"count", std::int64_t{2}}}));
    CHECK(r.ok);
    r = s.run("office.doc_export", P({{"doc", std::string("doc1")}, {"path", tmp("cmd_doc.png").string()}}));
    CHECK(r.ok && fs::exists(tmp("cmd_doc.png")));
}

void testDeckAndPdfCommands() {
    Session s;
    registerCommands(s);
    Result r = s.run("office.deck_new", P({{"theme", std::string("teal")},
                                          {"outline", std::string("# Launch plan\nQ3 2026\n\n# Goals\n- Ship it\n  - on time\n- Tell people\n> remember the demo\n\n"
                                                                  "<!-- layout: two_column -->\n# Compare\n- left\n<!-- column -->\n- right\n\n# Thanks\n")}}));
    CHECK(r.ok && txt(r) == "deck1");
    r = s.run("office.deck_info", {});
    CHECK(r.ok && r.message.find("4 slide(s), theme teal") != std::string::npos);
    CHECK(r.message.find("1. title: \"Launch plan\" / Q3 2026") != std::string::npos);
    CHECK(r.message.find("2. content: \"Goals\", 3 bullet(s), notes") != std::string::npos);
    CHECK(r.message.find("3. two_column") != std::string::npos && r.message.find("4. section") != std::string::npos);
    r = s.run("office.deck_add_slide", P({{"title", std::string("Numbers")}, {"bullets", std::string("a\\nb")}, {"at", std::int64_t{4}}, {"preview", true}}));
    CHECK(r.ok && txt(r) == "4" && r.attachments.size() == 1);
    r = s.run("office.deck_edit_slide", P({{"slide", std::int64_t{4}}, {"image", tmp("cmd_img.png").string()}}));
    CHECK(r.ok && r.message.find("image 60x30") != std::string::npos);
    r = s.run("office.deck_move_slide", P({{"slide", std::int64_t{5}}, {"to", std::int64_t{1}}}));
    CHECK(r.ok);
    CHECK(s.history().undo());
    // clicking a slide finds the part under the mouse
    auto hitAt = [&](std::int64_t slide, double x, double y) { return s.run("office.deck_hit", P({{"slide", slide}, {"x", x}, {"y", y}})); };
    r = hitAt(1, 480, 220);
    CHECK(r.ok && txt(r, "kind") == "title" && txt(r, "text") == "Launch plan");
    r = hitAt(2, 300, 60);
    CHECK(r.ok && txt(r, "kind") == "title" && txt(r, "text") == "Goals");
    r = hitAt(2, 200, 200);
    CHECK(r.ok && txt(r, "kind") == "bullets" && txt(r, "text").find("Ship it") != std::string::npos);
    r = hitAt(3, 700, 200);
    CHECK(r.ok && txt(r, "kind") == "right" && txt(r, "text").find("right") != std::string::npos);
    CHECK(s.run("office.deck_text_box", P({{"slide", std::int64_t{2}}, {"text", std::string("Note")}, {"x", 600.0}, {"y", 300.0}, {"w", 200.0}})).ok);
    r = hitAt(2, 650, 310);
    CHECK(r.ok && txt(r, "kind") == "object" && txt(r, "text") == "Note");
    CHECK(s.history().undo());
    CHECK(s.run("office.deck_add_slide", P({{"layout", std::string("content")}})).ok);
    r = hitAt(6, 300, 60);  // an empty title still takes a click, like "click to add title"
    CHECK(r.ok && txt(r, "kind") == "title" && txt(r, "text").empty());
    CHECK(s.history().undo());
    r = s.run("office.deck_theme", P({{"theme", std::string("#aa3300")}}));
    CHECK(r.ok);
    r = s.run("office.deck_preview", P({{"all", true}}));
    CHECK(r.ok && r.attachments.size() == 5);
    const std::string pptx = tmp("cmd.pptx").string(), deckPdf = tmp("cmd_deck.pdf").string();
    CHECK(s.run("office.deck_save", P({{"path", pptx}})).ok);
    CHECK(s.run("office.deck_export", P({{"path", deckPdf}})).ok);
    CHECK(s.run("office.deck_export", P({{"path", tmp("cmd_deck.md").string()}})).ok);
    r = s.run("office.deck_export", P({{"path", tmp("cmd_slides.png").string()}, {"width", std::int64_t{320}}}));
    CHECK(r.ok && fs::exists(tmp("cmd_slides-5.png")));
    r = s.run("office.deck_open", P({{"path", tmp("cmd_deck.md").string()}}));
    CHECK(r.ok);
    r = s.run("office.deck_info", P({{"doc", txt(r)}}));
    CHECK(r.ok && r.message.find("5 slide(s), theme #aa3300") != std::string::npos && r.message.find("4. content: \"Numbers\", 2 bullet(s), image 60x30") != std::string::npos);
    r = s.run("office.deck_open", P({{"path", pptx}}));
    CHECK(r.ok);
    r = s.run("office.deck_delete_slide", P({{"doc", txt(r)}, {"slide", std::int64_t{1}}}));
    CHECK(r.ok);

    // pdf tools on the deck (5 pages) and a 3-page doc
    Session d;
    registerCommands(d);
    d.run("office.doc_new", P({{"markdown", std::string("# One\n\ntext\n\n<!-- pagebreak -->\n\n# Two\n\n<!-- pagebreak -->\n\n# Three\n")}}));
    const std::string docPdf = tmp("cmd_three.pdf").string();
    CHECK(d.run("office.doc_export", P({{"path", docPdf}})).ok);
    r = s.run("office.pdf_info", P({{"path", docPdf}}));
    CHECK(txt(r) == "3");
    const std::string merged = tmp("cmd_merged.pdf").string();
    r = s.run("office.pdf_merge", P({{"inputs", docPdf + "; " + deckPdf + "[2-3]"}, {"output", merged}}));
    CHECK(r.ok && txt(s.run("office.pdf_info", P({{"path", merged}}))) == "5");
    r = s.run("office.pdf_text", P({{"path", merged}, {"pages", std::string("4")}}));
    CHECK(r.ok && txt(r).find("Goals") != std::string::npos);
    r = s.run("office.pdf_split", P({{"path", merged}, {"every", std::int64_t{2}}, {"output", tmp("split/part-{n}.pdf").string()}}));
    CHECK(r.ok && txt(s.run("office.pdf_info", P({{"path", tmp("split/part-3.pdf").string()}}))) == "1");
    r = s.run("office.pdf_split", P({{"path", merged}, {"ranges", std::string("1-2; 3-")}, {"output", tmp("split/range.pdf").string()}}));
    CHECK(r.ok && txt(s.run("office.pdf_info", P({{"path", tmp("split/range-2.pdf").string()}}))) == "3");
    const std::string ex = tmp("cmd_extract.pdf").string();
    r = s.run("office.pdf_extract", P({{"path", merged}, {"pages", std::string("5-4")}, {"output", ex}}));
    CHECK(r.ok && s.run("office.pdf_text", P({{"path", ex}, {"pages", std::string("1")}})).message.find("Compare") != std::string::npos);
    r = s.run("office.pdf_rotate", P({{"path", ex}, {"pages", std::string("1")}, {"angle", std::int64_t{90}}}));
    CHECK(r.ok && s.run("office.pdf_info", P({{"path", ex}})).message.find("rotated 90") != std::string::npos);
    r = s.run("office.pdf_delete_pages", P({{"path", ex}, {"pages", std::string("1")}}));
    CHECK(r.ok && txt(s.run("office.pdf_info", P({{"path", ex}}))) == "1");
    r = s.run("office.pdf_from_images", P({{"images", tmp("cmd_img.png").string() + "," + tmp("cmd_slides-1.png").string()}, {"output", tmp("cmd_images.pdf").string()}, {"page", std::string("a4")}}));
    CHECK(r.ok && txt(s.run("office.pdf_info", P({{"path", tmp("cmd_images.pdf").string()}}))) == "2");
    if (findProgram("pdftoppm")) {
        r = s.run("office.pdf_render", P({{"path", merged}, {"page", std::int64_t{4}}, {"dpi", 50.0}}));
        CHECK(r.ok && r.attachments.size() == 1);
    }

    // convert
    r = s.run("office.convert", P({{"input", tmp("cmd_doc.md").string()}, {"output", tmp("conv.docx").string()}}));
    CHECK(r.ok && r.message.find("built-in") != std::string::npos);
    r = s.run("office.convert", P({{"input", tmp("cmd.csv").string()}, {"output", tmp("conv.xlsx").string()}}));
    CHECK(r.ok);
    r = s.run("office.convert", P({{"input", tmp("cmd_deck.md").string()}, {"output", tmp("conv.pptx").string()}}));
    CHECK(r.ok && r.message.find("5 slides") != std::string::npos);
    r = s.run("office.convert", P({{"input", merged}, {"output", tmp("conv.txt").string()}}));
    CHECK(r.ok && readFile(tmp("conv.txt")).find("Three") != std::string::npos);
    r = s.run("office.convert", P({{"input", tmp("cmd_doc.md").string()}, {"output", tmp("conv.odt").string()}, {"backend", std::string("builtin")}}));
    CHECK(r.ok && readOdt(readFile(tmp("conv.odt"))).blocks.size() > 2);
    r = s.run("office.convert", P({{"input", tmp("cmd_doc.md").string()}, {"output", tmp("conv.rtf").string()}, {"backend", std::string("builtin")}}));
    CHECK(!r.ok && r.message.find("LibreOffice") != std::string::npos);
    r = s.run("office.formats", {});
    CHECK(r.ok && r.message.find("pptx") != std::string::npos);
    if (findProgram("soffice")) {
        r = s.run("office.convert", P({{"input", tmp("conv.xlsx").string()}, {"output", tmp("conv.ods").string()}}));
        CHECK(r.ok && r.message.find("built-in") != std::string::npos);
        r = s.run("office.sheet_open", P({{"path", tmp("conv.ods").string()}}));
        CHECK(r.ok);
        r = s.run("office.convert", P({{"input", tmp("conv.docx").string()}, {"output", tmp("conv.doc").string()}}));
        CHECK(r.ok);
        r = s.run("office.doc_open", P({{"path", tmp("conv.doc").string()}}));
        CHECK(r.ok);
        r = s.run("office.doc_get_text", P({{"doc", txt(r)}}));
        CHECK(r.ok && txt(r).find("Sales grew") != std::string::npos);
        r = s.run("office.convert", P({{"input", tmp("conv.pptx").string()}, {"output", tmp("conv.odp").string()}}));
        CHECK(r.ok);
        r = s.run("office.deck_open", P({{"path", tmp("conv.odp").string()}}));
        CHECK(r.ok);
        r = s.run("office.convert", P({{"input", tmp("conv.doc").string()}, {"output", tmp("conv_lo.pdf").string()}, {"backend", std::string("libreoffice")}}));
        CHECK(r.ok && r.message.find("LibreOffice") != std::string::npos);
    }
}

void testMcpAndBatch() {
    Session s;
    registerCommands(s);
    mcp::Server server(s, {"wetwaresuite", "test", "office"});
    json list = json::parse(*server.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})"))["result"]["tools"];
    int office = 0;
    bool sawSet = false, sawSave = false, sawInfo = false;
    for (const auto& t : list) {
        const std::string name = t["name"];
        if (name.rfind("office_", 0) == 0) ++office;
        if (name == "office_sheet_set") sawSet = t["inputSchema"]["properties"].contains("range") && t["inputSchema"]["required"].size() == 1;
        if (name == "office_doc_save") sawSave = t["annotations"]["readOnlyHint"] == false;
        if (name == "office_deck_info") sawInfo = t["annotations"]["readOnlyHint"] == true;
    }
    CHECK(office >= 60 && sawSet && sawSave && sawInfo);
    json req = {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/call"},
                {"params", {{"name", "office_deck_new"}, {"arguments", {{"outline", "# Hello\n\n# Agenda\n- one\n- two"}}}}}};
    json res = json::parse(*server.handle(req.dump()))["result"];
    CHECK(res["isError"] == false && res["structuredContent"]["doc"] == "deck1");
    req["params"] = {{"name", "office_deck_preview"}, {"arguments", {{"slide", 2}}}};
    res = json::parse(*server.handle(req.dump()))["result"];
    CHECK(res["isError"] == false && res["content"].size() == 2 && res["content"][1]["type"] == "image");
    req["params"] = {{"name", "office_sheet_new"}, {"arguments", {{"csv", "a,b\n1,2"}}}};
    res = json::parse(*server.handle(req.dump()))["result"];
    req["params"] = {{"name", "office_sheet_set"}, {"arguments", {{"range", "C2"}, {"value", "=A2+B2"}, {"preview", true}}}};
    res = json::parse(*server.handle(req.dump()))["result"];
    CHECK(res["isError"] == false && res["structuredContent"]["value"] == 3 && res["content"].size() == 2);

    Session b;
    registerCommands(b);
    cli::Batch batch(b);
    std::istringstream script("office.sheet_new csv=\"x,y\\\\n1,2\" -> s\n"
                              "office.sheet_set doc=$s range=C2 value=\"=A2*B2\"\n"
                              "office.sheet_get doc=$s range=C2\n");
    std::ostringstream out, err;
    CHECK(batch.run(script, out, err) == 0);
    CHECK(out.str().find("C2 = 2") != std::string::npos);
}

}  // namespace

// The window's cell grid: cells read one at a time, and typing becomes an undoable command.
void testGrid() {
    Session s;
    registerCommands(s);
    CHECK(s.run("office.sheet_new", P({{"csv", std::string("Item,Price\nTea,2.5\nCake,4")}})).ok);
    CHECK(s.run("office.sheet_add", P({{"name", std::string("Notes")}})).ok);
    auto doc = s.workspace().get("sheet1");
    CHECK(doc != nullptr);
    if (!doc) return;
    CellGrid layout;
    CHECK(doc->grid(layout) && layout.sheets.size() == 2 && layout.sheets[1] == "Notes");
    CHECK(s.run("office.sheet_select", P({{"sheet", std::string("1")}})).ok);
    CHECK(doc->grid(layout) && layout.active == 0 && layout.usedRows == 3 && layout.usedCols == 2);
    GridCell c;
    CHECK(doc->gridCell(0, 1, 1, c) && c.text == "2.5" && c.number && !c.error);
    CHECK(!doc->gridCell(0, 5, 5, c));
    CHECK(!doc->gridCell(7, 0, 0, c));  // no such sheet
    auto type = [&](int sheet, int row, int col, const std::string& input) {
        std::vector<std::string> tokens = doc->gridSetCommand(sheet, row, col, input);
        if (tokens.empty()) return false;
        const CommandInfo* info = s.registry().find(tokens.front());
        if (!info) return false;
        tokens.erase(tokens.begin());
        tokens.push_back(layout.docParam + "=sheet1");
        return s.run(info->id, cli::parseArgs(*info, tokens)).ok;
    };
    CHECK(type(0, 3, 1, "=SUM(B2:B3)"));
    CHECK(doc->gridCell(0, 3, 1, c) && c.text == "6.5" && c.input == "=SUM(B2:B3)");
    CHECK(type(1, 0, 0, "hello world"));  // another sheet, with a space
    CHECK(doc->gridCell(1, 0, 0, c) && c.text == "hello world" && !c.number);
    CHECK(type(0, 3, 1, ""));  // empty clears
    CHECK(!doc->gridCell(0, 3, 1, c));
    CHECK(type(0, 5, 0, "x\ty\n1\t2"));  // a pasted block lands from that cell
    CHECK(doc->gridCell(0, 6, 1, c) && c.text == "2");
    CHECK(type(0, 0, 3, "=1/0"));
    CHECK(doc->gridCell(0, 0, 3, c) && c.error);
    CHECK(s.history().undo() && !doc->gridCell(0, 0, 3, c));
    CHECK(doc->gridSetCommand(0, -1, 0, "1").empty());
    CHECK(doc->gridColumnChars(0, 0) > 0);
}

int main() {
    testA1();
    testInputAndFormats();
    testFormulas();
    testLookups();
    testRecalcAndCycles();
    testStructural();
    testCsv();
    testXlsxRoundTrip();
    testInlineMarkdown();
    testMarkdown();
    testHtml();
    testDocx();
    testLayout();
    testDeck();
    testChart();
    testSheetCommands();
    testGrid();
    testDocCommands();
    testDeckAndPdfCommands();
    testMcpAndBatch();
    std::error_code ec;
    fs::remove_all(tmp("x").parent_path(), ec);
    return check::summary("office");
}
