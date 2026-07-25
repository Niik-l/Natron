/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 * (C) 2013-2018 INRIA and Alexandre Gauthier-Foichat
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "DeepExpression.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "../../AppInstance.h"
#include "DeepImage.h"
#include "DeepUtils.h"
#include "../../Image.h"
#include "../../ImagePlaneDesc.h"
#include "../../KnobTypes.h"
#include "../../Node.h"
#include "../../ViewIdx.h"

NATRON_NAMESPACE_ENTER

// ============================================================================
// Tiny per-sample expression engine (Nuke DeepExpression-style).
// Expressions are compiled once per render into an RPN program, then
// evaluated per deep sample against a flat variable-slot array.
// ============================================================================

namespace {

enum ExprFunc
{
    eFnAbs, eFnFloor, eFnCeil, eFnRound, eFnSqrt, eFnExp, eFnLog, eFnLog10,
    eFnSin, eFnCos, eFnTan, eFnAsin, eFnAcos, eFnAtan,               // 1-arg
    eFnMin, eFnMax, eFnPow, eFnFmod, eFnAtan2, eFnStep,              // 2-arg
    eFnClamp, eFnLerp, eFnMix, eFnSmoothstep                          // 3-arg
};

struct ExprFuncDef { const char* name; ExprFunc fn; int arity; };

static const ExprFuncDef g_exprFuncs[] = {
    {"abs", eFnAbs, 1}, {"floor", eFnFloor, 1}, {"ceil", eFnCeil, 1},
    {"round", eFnRound, 1}, {"sqrt", eFnSqrt, 1}, {"exp", eFnExp, 1},
    {"log", eFnLog, 1}, {"log10", eFnLog10, 1},
    {"sin", eFnSin, 1}, {"cos", eFnCos, 1}, {"tan", eFnTan, 1},
    {"asin", eFnAsin, 1}, {"acos", eFnAcos, 1}, {"atan", eFnAtan, 1},
    {"min", eFnMin, 2}, {"max", eFnMax, 2}, {"pow", eFnPow, 2},
    {"fmod", eFnFmod, 2}, {"atan2", eFnAtan2, 2}, {"step", eFnStep, 2},
    {"clamp", eFnClamp, 3}, {"lerp", eFnLerp, 3}, {"mix", eFnMix, 3},
    {"smoothstep", eFnSmoothstep, 3},
};

enum ExprOpCode
{
    eOpConst, eOpVar,
    eOpAdd, eOpSub, eOpMul, eOpDiv, eOpMod, eOpPow, eOpNeg, eOpNot,
    eOpLt, eOpGt, eOpLe, eOpGe, eOpEq, eOpNe, eOpAnd, eOpOr,
    eOpSelect,   // ternary — pops b, a, cond; pushes cond != 0 ? a : b
    eOpCall
};

struct ExprOp
{
    ExprOpCode code;
    float value;   // eOpConst
    int var;       // eOpVar slot
    ExprFunc fn;   // eOpCall
    int arity;     // eOpCall
};

struct ExprProgram
{
    std::vector<ExprOp> ops;
    bool valid = false;
};

// Recursive-descent parser producing RPN. Identifiers may contain '.' so
// "rgba.red" / "deep.front" lex as single names, exactly like Nuke's fields.
class ExprParser
{
public:
    ExprParser(const std::string& src, const std::map<std::string, int>& vars)
        : _src(src), _pos(0), _vars(vars) {}

    bool parse(ExprProgram* out, std::string* error)
    {
        _prog = out;
        _prog->ops.clear();
        _prog->valid = false;
        _err.clear();
        if (!parseTernary()) {
            *error = _err.empty() ? "syntax error" : _err;
            return false;
        }
        skipWs();
        if (_pos < _src.size()) {
            *error = "unexpected character '" + std::string(1, _src[_pos]) + "'";
            return false;
        }
        _prog->valid = true;
        return true;
    }

private:
    void skipWs() { while (_pos < _src.size() && std::isspace((unsigned char)_src[_pos])) ++_pos; }
    bool atEnd() { skipWs(); return _pos >= _src.size(); }
    char peek() { skipWs(); return _pos < _src.size() ? _src[_pos] : '\0'; }
    bool accept(char c) { if (peek() == c) { ++_pos; return true; } return false; }
    bool accept2(const char* s) // two-character operator
    {
        skipWs();
        if (_pos + 1 < _src.size() && _src[_pos] == s[0] && _src[_pos + 1] == s[1]) {
            _pos += 2;
            return true;
        }
        return false;
    }
    void emit(const ExprOp& op) { _prog->ops.push_back(op); }
    void emitCode(ExprOpCode c) { ExprOp op = {}; op.code = c; emit(op); }
    bool fail(const std::string& msg) { if (_err.empty()) _err = msg; return false; }

    bool parseTernary()
    {
        if (!parseOr()) return false;
        if (accept('?')) {
            if (!parseTernary()) return false;
            if (!accept(':')) return fail("expected ':' in ternary");
            if (!parseTernary()) return false;
            emitCode(eOpSelect);
        }
        return true;
    }
    bool parseOr()
    {
        if (!parseAnd()) return false;
        while (accept2("||")) { if (!parseAnd()) return false; emitCode(eOpOr); }
        return true;
    }
    bool parseAnd()
    {
        if (!parseEq()) return false;
        while (accept2("&&")) { if (!parseEq()) return false; emitCode(eOpAnd); }
        return true;
    }
    bool parseEq()
    {
        if (!parseRel()) return false;
        for (;;) {
            if (accept2("==")) { if (!parseRel()) return false; emitCode(eOpEq); }
            else if (accept2("!=")) { if (!parseRel()) return false; emitCode(eOpNe); }
            else break;
        }
        return true;
    }
    bool parseRel()
    {
        if (!parseAddSub()) return false;
        for (;;) {
            if (accept2("<=")) { if (!parseAddSub()) return false; emitCode(eOpLe); }
            else if (accept2(">=")) { if (!parseAddSub()) return false; emitCode(eOpGe); }
            else if (peek() == '<' && !lookahead2("<=")) { ++_pos; if (!parseAddSub()) return false; emitCode(eOpLt); }
            else if (peek() == '>' && !lookahead2(">=")) { ++_pos; if (!parseAddSub()) return false; emitCode(eOpGt); }
            else break;
        }
        return true;
    }
    bool lookahead2(const char* s)
    {
        skipWs();
        return _pos + 1 < _src.size() && _src[_pos] == s[0] && _src[_pos + 1] == s[1];
    }
    bool parseAddSub()
    {
        if (!parseMulDiv()) return false;
        for (;;) {
            if (accept('+')) { if (!parseMulDiv()) return false; emitCode(eOpAdd); }
            else if (accept('-')) { if (!parseMulDiv()) return false; emitCode(eOpSub); }
            else break;
        }
        return true;
    }
    bool parseMulDiv()
    {
        if (!parseUnary()) return false;
        for (;;) {
            if (accept('*')) { if (!parseUnary()) return false; emitCode(eOpMul); }
            else if (accept('/')) { if (!parseUnary()) return false; emitCode(eOpDiv); }
            else if (accept('%')) { if (!parseUnary()) return false; emitCode(eOpMod); }
            else break;
        }
        return true;
    }
    bool parseUnary()
    {
        if (accept('-')) { if (!parseUnary()) return false; emitCode(eOpNeg); return true; }
        if (accept('!')) { if (!parseUnary()) return false; emitCode(eOpNot); return true; }
        if (accept('+')) { return parseUnary(); }
        return parsePow();
    }
    bool parsePow()
    {
        if (!parsePrimary()) return false;
        if (accept('^')) {   // right-associative
            if (!parseUnary()) return false;
            emitCode(eOpPow);
        }
        return true;
    }
    bool parsePrimary()
    {
        skipWs();
        if (_pos >= _src.size()) return fail("unexpected end of expression");
        char c = _src[_pos];

        if (c == '(') {
            ++_pos;
            if (!parseTernary()) return false;
            if (!accept(')')) return fail("expected ')'");
            return true;
        }
        if (std::isdigit((unsigned char)c) || c == '.') {
            char* end = nullptr;
            float v = std::strtof(_src.c_str() + _pos, &end);
            if (end == _src.c_str() + _pos) return fail("bad number");
            _pos = (std::size_t)(end - _src.c_str());
            ExprOp op = {}; op.code = eOpConst; op.value = v; emit(op);
            return true;
        }
        if (std::isalpha((unsigned char)c) || c == '_') {
            std::size_t start = _pos;
            while (_pos < _src.size() &&
                   (std::isalnum((unsigned char)_src[_pos]) || _src[_pos] == '_' || _src[_pos] == '.')) {
                ++_pos;
            }
            std::string name = _src.substr(start, _pos - start);

            if (peek() == '(') {   // function call
                const ExprFuncDef* def = nullptr;
                for (const ExprFuncDef& f : g_exprFuncs) {
                    if (name == f.name) { def = &f; break; }
                }
                if (!def) return fail("unknown function '" + name + "'");
                ++_pos; // consume '('
                for (int i = 0; i < def->arity; ++i) {
                    if (i > 0 && !accept(',')) return fail("expected ',' in " + name + "()");
                    if (!parseTernary()) return false;
                }
                if (!accept(')')) return fail("expected ')' after " + name + "()");
                ExprOp op = {}; op.code = eOpCall; op.fn = def->fn; op.arity = def->arity; emit(op);
                return true;
            }

            if (name == "pi") { ExprOp op = {}; op.code = eOpConst; op.value = (float)M_PI; emit(op); return true; }
            if (name == "e")  { ExprOp op = {}; op.code = eOpConst; op.value = (float)M_E;  emit(op); return true; }

            std::map<std::string, int>::const_iterator it = _vars.find(name);
            if (it == _vars.end()) return fail("unknown variable '" + name + "'");
            ExprOp op = {}; op.code = eOpVar; op.var = it->second; emit(op);
            return true;
        }
        return fail(std::string("unexpected character '") + c + "'");
    }

    const std::string& _src;
    std::size_t _pos;
    const std::map<std::string, int>& _vars;
    ExprProgram* _prog;
    std::string _err;
};

static inline float
evalFunc(ExprFunc fn, const float* a)
{
    switch (fn) {
        case eFnAbs:    return std::fabs(a[0]);
        case eFnFloor:  return std::floor(a[0]);
        case eFnCeil:   return std::ceil(a[0]);
        case eFnRound:  return std::floor(a[0] + 0.5f);
        case eFnSqrt:   return a[0] > 0.0f ? std::sqrt(a[0]) : 0.0f;
        case eFnExp:    return std::exp(a[0]);
        case eFnLog:    return a[0] > 0.0f ? std::log(a[0]) : -std::numeric_limits<float>::max();
        case eFnLog10:  return a[0] > 0.0f ? std::log10(a[0]) : -std::numeric_limits<float>::max();
        case eFnSin:    return std::sin(a[0]);
        case eFnCos:    return std::cos(a[0]);
        case eFnTan:    return std::tan(a[0]);
        case eFnAsin:   return std::asin(std::max(-1.0f, std::min(1.0f, a[0])));
        case eFnAcos:   return std::acos(std::max(-1.0f, std::min(1.0f, a[0])));
        case eFnAtan:   return std::atan(a[0]);
        case eFnMin:    return std::min(a[0], a[1]);
        case eFnMax:    return std::max(a[0], a[1]);
        case eFnPow:    return std::pow(a[0], a[1]);
        case eFnFmod:   return a[1] != 0.0f ? std::fmod(a[0], a[1]) : 0.0f;
        case eFnAtan2:  return std::atan2(a[0], a[1]);
        case eFnStep:   return a[1] >= a[0] ? 1.0f : 0.0f;
        case eFnClamp:  return std::max(a[1], std::min(a[2], a[0]));
        case eFnLerp:
        case eFnMix:    return a[0] + (a[1] - a[0]) * a[2];
        case eFnSmoothstep: {
            if (a[0] >= a[1]) return a[2] >= a[1] ? 1.0f : 0.0f;
            float t = std::max(0.0f, std::min(1.0f, (a[2] - a[0]) / (a[1] - a[0])));
            return t * t * (3.0f - 2.0f * t);
        }
    }
    return 0.0f;
}

static const int kExprStackSize = 64;

static inline float
evalProgram(const ExprProgram& prog, const float* vars)
{
    float stack[kExprStackSize];
    int sp = 0;
    for (const ExprOp& op : prog.ops) {
        switch (op.code) {
            case eOpConst: if (sp < kExprStackSize) stack[sp++] = op.value; break;
            case eOpVar:   if (sp < kExprStackSize) stack[sp++] = vars[op.var]; break;
            case eOpNeg:   if (sp >= 1) stack[sp - 1] = -stack[sp - 1]; break;
            case eOpNot:   if (sp >= 1) stack[sp - 1] = (stack[sp - 1] == 0.0f) ? 1.0f : 0.0f; break;
            case eOpAdd:   if (sp >= 2) { --sp; stack[sp - 1] += stack[sp]; } break;
            case eOpSub:   if (sp >= 2) { --sp; stack[sp - 1] -= stack[sp]; } break;
            case eOpMul:   if (sp >= 2) { --sp; stack[sp - 1] *= stack[sp]; } break;
            case eOpDiv:   if (sp >= 2) { --sp; stack[sp - 1] = (stack[sp] != 0.0f) ? stack[sp - 1] / stack[sp] : 0.0f; } break;
            case eOpMod:   if (sp >= 2) { --sp; stack[sp - 1] = (stack[sp] != 0.0f) ? std::fmod(stack[sp - 1], stack[sp]) : 0.0f; } break;
            case eOpPow:   if (sp >= 2) { --sp; stack[sp - 1] = std::pow(stack[sp - 1], stack[sp]); } break;
            case eOpLt:    if (sp >= 2) { --sp; stack[sp - 1] = stack[sp - 1] <  stack[sp] ? 1.0f : 0.0f; } break;
            case eOpGt:    if (sp >= 2) { --sp; stack[sp - 1] = stack[sp - 1] >  stack[sp] ? 1.0f : 0.0f; } break;
            case eOpLe:    if (sp >= 2) { --sp; stack[sp - 1] = stack[sp - 1] <= stack[sp] ? 1.0f : 0.0f; } break;
            case eOpGe:    if (sp >= 2) { --sp; stack[sp - 1] = stack[sp - 1] >= stack[sp] ? 1.0f : 0.0f; } break;
            case eOpEq:    if (sp >= 2) { --sp; stack[sp - 1] = stack[sp - 1] == stack[sp] ? 1.0f : 0.0f; } break;
            case eOpNe:    if (sp >= 2) { --sp; stack[sp - 1] = stack[sp - 1] != stack[sp] ? 1.0f : 0.0f; } break;
            case eOpAnd:   if (sp >= 2) { --sp; stack[sp - 1] = (stack[sp - 1] != 0.0f && stack[sp] != 0.0f) ? 1.0f : 0.0f; } break;
            case eOpOr:    if (sp >= 2) { --sp; stack[sp - 1] = (stack[sp - 1] != 0.0f || stack[sp] != 0.0f) ? 1.0f : 0.0f; } break;
            case eOpSelect:
                if (sp >= 3) { sp -= 2; stack[sp - 1] = (stack[sp - 1] != 0.0f) ? stack[sp] : stack[sp + 1]; }
                break;
            case eOpCall:
                if (sp >= op.arity) { sp -= op.arity; stack[sp] = evalFunc(op.fn, &stack[sp]); ++sp; }
                break;
        }
    }
    return sp > 0 ? stack[sp - 1] : 0.0f;
}

// Variable slot layout
enum
{
    eVarRed, eVarGreen, eVarBlue, eVarAlpha,
    eVarFront, eVarBack,
    eVarX, eVarY, eVarFrame,
    eVarTemp0,          // + up to 4 user temps
    kNumExprVars = eVarTemp0 + 4
};

static const int kNumTemps = 4;
static const int kNumChannelExprs = 6;   // red, green, blue, alpha, front, back

} // anonymous namespace


struct DeepExpressionPrivate
{
    // Temp variable rows (name = expression), evaluated in order per sample
    KnobStringWPtr tempName[kNumTemps];
    KnobStringWPtr tempExpr[kNumTemps];

    // Per-channel expressions
    KnobStringWPtr exprRed, exprGreen, exprBlue, exprAlpha;
    KnobStringWPtr exprFront, exprBack;
};


DeepExpression::DeepExpression(NodePtr node)
    : EffectInstance(node)
    , _imp(new DeepExpressionPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

DeepExpression::~DeepExpression()
{
}

std::string
DeepExpression::getPluginDescription() const
{
    return tr("Apply per-sample expressions to deep data, like Nuke's DeepExpression.\n\n"
              "Each field is evaluated per deep sample; empty fields pass the channel "
              "through unchanged. Available variables:\n"
              "  rgba.red, rgba.green, rgba.blue, rgba.alpha (aliases: red, green, blue, alpha, r, g, b, a)\n"
              "  deep.front, deep.back (aliases: front, back, z, zback)\n"
              "  x, y (pixel coords, y up), frame\n\n"
              "The four rows at the top define temporary variables (name = expression) "
              "usable in the channel expressions below, evaluated top to bottom.\n\n"
              "Operators: + - * / % ^ == != < > <= >= && || ! ?:\n"
              "Functions: abs floor ceil round sqrt exp log log10 sin cos tan asin acos "
              "atan min max pow fmod atan2 step clamp lerp mix smoothstep. Constants: pi, e.\n\n"
              "Examples:\n"
              "  deep.front:  deep.front - 0.5   (pull samples half a unit closer)\n"
              "  rgba.alpha:  alpha * smoothstep(20, 10, deep.front)   (fade with depth)\n"
              "  rgba.red:    deep.front > 5 ? 0 : red   (black out far samples' red)").toStdString();
}

void
DeepExpression::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
DeepExpression::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
DeepExpression::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
DeepExpression::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("DeepExpression"));

    // Temp variable rows: name field + expression field on one line
    for (int i = 0; i < kNumTemps; ++i) {
        KnobStringPtr name = AppManager::createKnob<KnobString>(this, tr(""));
        name->setName(std::string("temp") + std::to_string(i) + "Name");
        name->setHintToolTip(tr("Temporary variable name (usable in the expressions below)."));
        name->setAnimationEnabled(false);
        name->setAddNewLine(false);
        page->addKnob(name);
        _imp->tempName[i] = name;

        KnobStringPtr expr = AppManager::createKnob<KnobString>(this, tr("="));
        expr->setName(std::string("temp") + std::to_string(i) + "Expr");
        expr->setHintToolTip(tr("Expression assigned to the temporary variable."));
        expr->setAnimationEnabled(false);
        page->addKnob(expr);
        _imp->tempExpr[i] = expr;
    }

    struct ChanDef { const char* label; const char* name; KnobStringWPtr* slot; const char* hint; };
    ChanDef defs[kNumChannelExprs] = {
        {"rgba.red",   "exprRed",   &_imp->exprRed,   "Expression for the red channel. Empty = unchanged."},
        {"rgba.green", "exprGreen", &_imp->exprGreen, "Expression for the green channel. Empty = unchanged."},
        {"rgba.blue",  "exprBlue",  &_imp->exprBlue,  "Expression for the blue channel. Empty = unchanged."},
        {"rgba.alpha", "exprAlpha", &_imp->exprAlpha, "Expression for the alpha channel. Empty = unchanged."},
        {"deep.front", "exprFront", &_imp->exprFront, "Expression for the sample front depth (Z). Empty = unchanged."},
        {"deep.back",  "exprBack",  &_imp->exprBack,  "Expression for the sample back depth (ZBack). Empty = unchanged."},
    };
    for (int i = 0; i < kNumChannelExprs; ++i) {
        KnobStringPtr k = AppManager::createKnob<KnobString>(this, tr(defs[i].label));
        k->setName(defs[i].name);
        k->setHintToolTip(tr(defs[i].hint));
        k->setAnimationEnabled(false);
        page->addKnob(k);
        *defs[i].slot = k;
    }
}

StatusEnum
DeepExpression::getRegionOfDefinition(U64 /*hash*/, double time, const RenderScale& scale,
                                      ViewIdx view, RectD* rod)
{
    EffectInstancePtr input = getInput(0);
    if (!input) return eStatusFailed;
    bool isProjectFormat = false;
    return input->getRegionOfDefinition_public(input->getHash(), time, scale, view, rod, &isProjectFormat);
}

DeepImagePtr
DeepExpression::getDeepImage() const
{
    return _lastDeepImage;
}

StatusEnum
DeepExpression::render(const RenderActionArgs& args)
{
    EffectInstancePtr deepInput = getInput(0);
    DeepImagePtr srcDeep = getDeepImageFromEffect(deepInput.get());
    if (!srcDeep) return eStatusFailed;

    // ---- Gather expression strings ----
    std::string tempNames[kNumTemps], tempExprs[kNumTemps];
    for (int i = 0; i < kNumTemps; ++i) {
        tempNames[i] = _imp->tempName[i].lock()->getValue();
        tempExprs[i] = _imp->tempExpr[i].lock()->getValue();
    }
    std::string chanExprs[kNumChannelExprs] = {
        _imp->exprRed.lock()->getValue(),
        _imp->exprGreen.lock()->getValue(),
        _imp->exprBlue.lock()->getValue(),
        _imp->exprAlpha.lock()->getValue(),
        _imp->exprFront.lock()->getValue(),
        _imp->exprBack.lock()->getValue(),
    };

    // ---- Build the variable table ----
    std::map<std::string, int> vars;
    vars["rgba.red"] = eVarRed;     vars["red"] = eVarRed;     vars["r"] = eVarRed;
    vars["rgba.green"] = eVarGreen; vars["green"] = eVarGreen; vars["g"] = eVarGreen;
    vars["rgba.blue"] = eVarBlue;   vars["blue"] = eVarBlue;   vars["b"] = eVarBlue;
    vars["rgba.alpha"] = eVarAlpha; vars["alpha"] = eVarAlpha; vars["a"] = eVarAlpha;
    vars["deep.front"] = eVarFront; vars["front"] = eVarFront; vars["z"] = eVarFront;
    vars["deep.back"] = eVarBack;   vars["back"] = eVarBack;   vars["zback"] = eVarBack;
    vars["x"] = eVarX;
    vars["y"] = eVarY;
    vars["frame"] = eVarFrame;

    // Register temp names (a temp may reference earlier temps)
    bool tempActive[kNumTemps] = {false, false, false, false};
    for (int i = 0; i < kNumTemps; ++i) {
        if (tempExprs[i].empty()) continue;
        const std::string& n = tempNames[i];
        if (n.empty()) {
            setPersistentMessage(eMessageTypeError,
                                 "DeepExpression: temp expression " + std::to_string(i)
                                 + " has no variable name.");
            return eStatusFailed;
        }
        if (vars.count(n)) {
            setPersistentMessage(eMessageTypeError,
                                 "DeepExpression: temp variable '" + n
                                 + "' shadows a built-in or duplicate name.");
            return eStatusFailed;
        }
        vars[n] = eVarTemp0 + i;
        tempActive[i] = true;
    }

    // ---- Compile ----
    ExprProgram tempProg[kNumTemps];
    ExprProgram chanProg[kNumChannelExprs];
    bool chanActive[kNumChannelExprs] = {false, false, false, false, false, false};

    for (int i = 0; i < kNumTemps; ++i) {
        if (!tempActive[i]) continue;
        std::string err;
        ExprParser p(tempExprs[i], vars);
        if (!p.parse(&tempProg[i], &err)) {
            setPersistentMessage(eMessageTypeError,
                                 "DeepExpression: error in temp '" + tempNames[i] + "': " + err);
            return eStatusFailed;
        }
    }
    static const char* chanLabels[kNumChannelExprs] =
        {"rgba.red", "rgba.green", "rgba.blue", "rgba.alpha", "deep.front", "deep.back"};
    for (int i = 0; i < kNumChannelExprs; ++i) {
        if (chanExprs[i].empty()) continue;
        std::string err;
        ExprParser p(chanExprs[i], vars);
        if (!p.parse(&chanProg[i], &err)) {
            setPersistentMessage(eMessageTypeError,
                                 std::string("DeepExpression: error in ") + chanLabels[i] + ": " + err);
            return eStatusFailed;
        }
        chanActive[i] = true;
    }
    clearPersistentMessage(false);

    // ---- Resolve channels; append targets missing from the source ----
    const RectI& dw = srcDeep->getDataWindow();
    int nChannels = srcDeep->getNumChannels();

    int srcIdx[kNumChannelExprs] = {
        srcDeep->findChannelIndex("R"), srcDeep->findChannelIndex("G"),
        srcDeep->findChannelIndex("B"), srcDeep->findChannelIndex("A"),
        srcDeep->findChannelIndex("Z"), srcDeep->findChannelIndex("ZBack"),
    };
    static const char* targetChannelNames[kNumChannelExprs] = {"R", "G", "B", "A", "Z", "ZBack"};

    std::vector<std::string> outNames = srcDeep->getChannelNames();
    int dstIdx[kNumChannelExprs];
    for (int i = 0; i < kNumChannelExprs; ++i) {
        dstIdx[i] = srcIdx[i];
        if (chanActive[i] && dstIdx[i] < 0) {
            dstIdx[i] = (int)outNames.size();
            outNames.push_back(targetChannelNames[i]);
        }
    }
    int nOutChannels = (int)outNames.size();

    DeepImagePtr result = std::make_shared<DeepImage>(dw, nOutChannels, outNames);
    for (int y = dw.y1; y < dw.y2; ++y) {
        for (int x = dw.x1; x < dw.x2; ++x) {
            result->setSampleCount(x, y, srcDeep->getSampleCount(x, y));
        }
    }
    result->allocateFromSampleCounts();

    // ---- Evaluate per sample ----
    float slots[kNumExprVars];
    slots[eVarFrame] = (float)args.time;

    for (int y = dw.y1; y < dw.y2; ++y) {
        // Deep rows are top-down; expose a bottom-up y like the viewer/Nuke
        slots[eVarY] = (float)(dw.y1 + (dw.y2 - 1 - y));

        for (int x = dw.x1; x < dw.x2; ++x) {
            int nSamples = srcDeep->getSampleCount(x, y);
            if (nSamples == 0) continue;

            slots[eVarX] = (float)x;

            const float* srcData = srcDeep->getSampleData(x, y);
            float* dstData = result->getSampleData(x, y);
            if (!dstData) continue;

            for (int s = 0; s < nSamples; ++s) {
                const float* src = srcData + s * nChannels;
                float* dst = dstData + s * nOutChannels;

                // Load input values into the variable slots
                slots[eVarRed]   = (srcIdx[0] >= 0) ? src[srcIdx[0]] : 0.0f;
                slots[eVarGreen] = (srcIdx[1] >= 0) ? src[srcIdx[1]] : 0.0f;
                slots[eVarBlue]  = (srcIdx[2] >= 0) ? src[srcIdx[2]] : 0.0f;
                slots[eVarAlpha] = (srcIdx[3] >= 0) ? src[srcIdx[3]] : 1.0f;
                slots[eVarFront] = (srcIdx[4] >= 0) ? src[srcIdx[4]] : 0.0f;
                slots[eVarBack]  = (srcIdx[5] >= 0) ? src[srcIdx[5]] : slots[eVarFront];

                // Temp variables, top to bottom
                for (int i = 0; i < kNumTemps; ++i) {
                    if (tempActive[i]) slots[eVarTemp0 + i] = evalProgram(tempProg[i], slots);
                }

                // All channel expressions read the INPUT values (simultaneous
                // assignment, like Nuke) — evaluate first, then write.
                float outV[kNumChannelExprs];
                for (int i = 0; i < kNumChannelExprs; ++i) {
                    if (chanActive[i]) outV[i] = evalProgram(chanProg[i], slots);
                }

                // Copy source channels, zero any appended ones
                for (int c = 0; c < nChannels; ++c) dst[c] = src[c];
                for (int c = nChannels; c < nOutChannels; ++c) dst[c] = 0.0f;

                // Apply expression results
                for (int i = 0; i < kNumChannelExprs; ++i) {
                    if (chanActive[i]) dst[dstIdx[i]] = outV[i];
                }
            }
        }
    }

    _lastDeepImage = result;

    // Flattened preview for the 2D viewer
    assert(!args.outputPlanes.empty());
    const std::pair<ImagePlaneDesc, ImagePtr>& output = args.outputPlanes.front();
    ImagePtr outImg = output.second;
    if (outImg) {
        result->flattenToImage(outImg.get());
    }

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_DeepExpression.cpp"
