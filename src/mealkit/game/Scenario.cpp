#include "mealkit/game/Scenario.h"

#include "mealkit/platform/Paths.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace mealkit::game {
namespace {

constexpr const char* kOps[] = {"==", "!=", "<=", ">=", "<", ">"};

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t\r") - a + 1);
}

bool numberOf(const std::string& text, double& out) {
    char* end = nullptr;
    out = std::strtod(text.c_str(), &end);
    return end != text.c_str() && *end == '\0';
}

} // namespace

bool loadScenario(const std::string& file, Scenario& out, std::string& error) {
    out = {};
    out.file = file;
    std::string content;
    if (!readTextFile(file, content)) {
        error = "cannot open file";
        return false;
    }
    std::istringstream in(content);
    std::string raw;
    u32 lineNo = 0;
    while (std::getline(in, raw)) {
        ++lineNo;
        const std::string line = trim(raw);
        if (line.empty() || line[0] == '#') continue;
        const size_t space = line.find(' ');
        const std::string key = line.substr(0, space);
        const std::string rest = space == std::string::npos ? std::string() : trim(line.substr(space + 1));
        auto bad = [&](const char* what) {
            error = "line " + std::to_string(lineNo) + ": " + what;
            return false;
        };
        if (key == "seed") out.seed = std::strtoull(rest.c_str(), nullptr, 10);
        else if (key == "scene") out.scene = rest;
        else if (key == "inputs") {
            const std::string dir = parentDirectory(file);
            out.inputs = dir.empty() ? rest : dir + "/" + rest;
        } else if (key == "ticks") out.ticks = static_cast<u32>(std::strtoul(rest.c_str(), nullptr, 10));
        else if (key == "tick") {
            // tick <n|seconds s> <path> <op> <value...>
            std::istringstream parts(rest);
            std::string when, path, op;
            if (!(parts >> when >> path >> op)) return bad("expected: tick <n> <path> <op> <value>");
            ScenarioCheck check;
            check.line = lineNo;
            check.path = path;
            check.op = op;
            if (std::find_if(std::begin(kOps), std::end(kOps), [&](const char* o) { return op == o; }) == std::end(kOps))
                return bad("unknown op (== != < <= > >=)");
            std::getline(parts, check.expected);
            check.expected = trim(check.expected);
            if (check.expected.empty()) return bad("missing value");
            char* end = nullptr;
            if (!when.empty() && when.back() == 's') {
                check.seconds = true;
                check.secondsValue = std::strtof(when.c_str(), &end);
                if (end != when.c_str() + when.size() - 1) return bad("bad tick");
            } else {
                check.tick = static_cast<u32>(std::strtoul(when.c_str(), &end, 10));
                if (end != when.c_str() + when.size()) return bad("bad tick");
            }
            out.checks.push_back(std::move(check));
        } else return bad("unknown key (seed, scene, inputs, ticks, tick)");
    }
    if (out.checks.empty()) {
        error = "no 'tick' checks";
        return false;
    }
    return true;
}

ScenarioRun::ScenarioRun(Scenario scenario, const reflect::TypeInfo& stateType, bool updateExpected)
    : m_scenario(std::move(scenario)), m_stateType(&stateType), m_updateExpected(updateExpected) {
    for (ScenarioCheck& c : m_scenario.checks)
        if (c.seconds) c.tick = Ticks::seconds(c.secondsValue).value;
    m_results.resize(m_scenario.checks.size());
}

u32 ScenarioRun::lastCheckTick() const {
    u32 last = 0;
    for (const ScenarioCheck& c : m_scenario.checks) last = std::max(last, c.tick);
    return last;
}

bool ScenarioRun::evaluate(const ScenarioCheck& check, const void* state, Result& result) {
    const reflect::ValueInfo* value = nullptr;
    void* data = nullptr;
    if (!reflect::resolvePath(*m_stateType, const_cast<void*>(state), check.path, value, data)) {
        result.actual = "(no such field or empty slot)";
        return false;
    }
    result.actual = reflect::formatScalar(*value, data);
    if (value->kind == reflect::Kind::Struct || value->kind == reflect::Kind::Array ||
        value->kind == reflect::Kind::Container) {
        result.actual = "(not a single value)";
        return false;
    }
    if (check.op == "==" || check.op == "!=") {
        alignas(16) u8 expected[64] = {};
        if (value->size > sizeof(expected) || !reflect::parseValue(*value, expected, check.expected)) {
            result.actual += " (cannot read the expected value)";
            return false;
        }
        const bool equal = std::memcmp(expected, data, value->size) == 0;
        return check.op == "==" ? equal : !equal;
    }
    double a = 0.0, b = 0.0;
    if (!numberOf(result.actual, a) || !numberOf(check.expected, b)) {
        result.actual += " (< <= > >= need numbers)";
        return false;
    }
    if (check.op == "<") return a < b;
    if (check.op == "<=") return a <= b;
    if (check.op == ">") return a > b;
    return a >= b;
}

void ScenarioRun::afterTick(u32 tick, const void* state) {
    for (size_t i = 0; i < m_scenario.checks.size(); ++i) {
        const ScenarioCheck& c = m_scenario.checks[i];
        if (c.tick != tick) continue;
        Result& r = m_results[i];
        r.done = true;
        r.passed = evaluate(c, state, r);
    }
}

bool ScenarioRun::writeExpected() const {
    std::string content;
    if (!readTextFile(m_scenario.file, content)) return false;
    std::vector<std::string> lines;
    std::istringstream in(content);
    for (std::string l; std::getline(in, l);) lines.push_back(l);
    for (size_t i = 0; i < m_scenario.checks.size(); ++i) {
        const ScenarioCheck& c = m_scenario.checks[i];
        const Result& r = m_results[i];
        if (c.op != "==" || !r.done || c.line == 0 || c.line > lines.size()) continue;
        std::string& l = lines[c.line - 1];
        const size_t op = l.find(" == ");
        if (op != std::string::npos) l = l.substr(0, op + 4) + r.actual;
    }
    std::string out;
    for (const std::string& l : lines) out += l + "\n";
    return writeTextFile(m_scenario.file, out);
}

int ScenarioRun::finish(u32 lastTick) {
    u32 failed = 0;
    for (size_t i = 0; i < m_scenario.checks.size(); ++i) {
        const ScenarioCheck& c = m_scenario.checks[i];
        const Result& r = m_results[i];
        if (r.passed || (m_updateExpected && c.op == "==" && r.done)) continue; // just rewritten
        ++failed;
        if (!r.done)
            std::printf("SCENARIO FAIL %s:%u tick %u not reached (ran to tick %u)\n", m_scenario.file.c_str(), c.line,
                        c.tick, lastTick);
        else
            std::printf("SCENARIO FAIL %s:%u tick %u %s %s %s (got %s)\n", m_scenario.file.c_str(), c.line, c.tick,
                        c.path.c_str(), c.op.c_str(), c.expected.c_str(), r.actual.c_str());
    }
    if (m_updateExpected) {
        if (!writeExpected()) {
            std::printf("SCENARIO FAIL %s cannot rewrite the file\n", m_scenario.file.c_str());
            return kScenarioFailedExit;
        }
        std::printf("SCENARIO updated %s\n", m_scenario.file.c_str());
    }
    const u32 total = static_cast<u32>(m_scenario.checks.size());
    if (failed == 0) std::printf("SCENARIO ok %s checks=%u\n", m_scenario.file.c_str(), total);
    else std::printf("SCENARIO FAIL %s failed=%u/%u\n", m_scenario.file.c_str(), failed, total);
    std::fflush(stdout);
    return failed == 0 ? 0 : kScenarioFailedExit;
}

} // namespace mealkit::game
