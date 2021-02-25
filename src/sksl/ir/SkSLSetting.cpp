/*
 * Copyright 2017 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "src/sksl/SkSLIRGenerator.h"
#include "src/sksl/ir/SkSLSetting.h"
#include "src/sksl/ir/SkSLVariableReference.h"

namespace SkSL {

// Helper classes for converting caps fields to Expressions and Types in the CapsLookupTable.
namespace {

class CapsLookupMethod {
public:
    virtual ~CapsLookupMethod() {}
    virtual const Type* type(const Context& context) const = 0;
    virtual std::unique_ptr<Expression> value(const Context& context) const = 0;
};

class BoolCapsLookup : public CapsLookupMethod {
public:
    using CapsFn = bool (ShaderCapsClass::*)() const;

    BoolCapsLookup(const CapsFn& fn) : fGetCap(fn) {}

    const Type* type(const Context& context) const override {
        return context.fTypes.fBool.get();
    }
    std::unique_ptr<Expression> value(const Context& context) const override {
        return std::make_unique<BoolLiteral>(context, /*offset=*/-1, (context.fCaps.*fGetCap)());
    }

private:
    CapsFn fGetCap;
};

class IntCapsLookup : public CapsLookupMethod {
public:
    using CapsFn = int (ShaderCapsClass::*)() const;

    IntCapsLookup(const CapsFn& fn) : fGetCap(fn) {}

    const Type* type(const Context& context) const override {
        return context.fTypes.fInt.get();
    }
    std::unique_ptr<Expression> value(const Context& context) const override {
        return std::make_unique<IntLiteral>(context, /*offset=*/-1, (context.fCaps.*fGetCap)());
    }

private:
    CapsFn fGetCap;
};

class CapsLookupTable {
public:
    using Pair = std::pair<const char*, CapsLookupMethod*>;

    CapsLookupTable(std::initializer_list<Pair> capsLookups) {
        for (auto& entry : capsLookups) {
            fMap[entry.first] = std::unique_ptr<CapsLookupMethod>(entry.second);
        }
    }

    const CapsLookupMethod* lookup(const String& name) const {
        auto iter = fMap.find(name);
        return (iter != fMap.end()) ? iter->second.get() : nullptr;
    }

private:
    std::unordered_map<String, std::unique_ptr<CapsLookupMethod>> fMap;
};

// Create a lookup table at startup that converts strings into the equivalent ShaderCapsClass
// methods.
static CapsLookupTable sCapsLookupTable{{
#define CAP(T, name) CapsLookupTable::Pair{#name, new T##CapsLookup{&ShaderCapsClass::name}}
    CAP(Bool, fbFetchSupport),
    CAP(Bool, fbFetchNeedsCustomOutput),
    CAP(Bool, flatInterpolationSupport),
    CAP(Bool, noperspectiveInterpolationSupport),
    CAP(Bool, externalTextureSupport),
    CAP(Bool, mustEnableAdvBlendEqs),
    CAP(Bool, mustDeclareFragmentShaderOutput),
    CAP(Bool, mustDoOpBetweenFloorAndAbs),
    CAP(Bool, mustGuardDivisionEvenAfterExplicitZeroCheck),
    CAP(Bool, inBlendModesFailRandomlyForAllZeroVec),
    CAP(Bool, atan2ImplementedAsAtanYOverX),
    CAP(Bool, canUseAnyFunctionInShader),
    CAP(Bool, floatIs32Bits),
    CAP(Bool, integerSupport),
    CAP(Bool, builtinFMASupport),
    CAP(Bool, builtinDeterminantSupport),
#undef CAP
}};

}  // namespace

const Type* Setting::GetType(const Context& context, int offset, const String& name) {
    if (const CapsLookupMethod* caps = sCapsLookupTable.lookup(name)) {
        return caps->type(context);
    }

    context.fErrors.error(offset, "unknown capability flag '" + name + "'");
    return nullptr;
}

std::unique_ptr<Expression> Setting::GetValue(const Context& context, int offset,
                                              const String& name) {
    if (const CapsLookupMethod* caps = sCapsLookupTable.lookup(name)) {
        return caps->value(context);
    }

    context.fErrors.error(offset, "unknown capability flag '" + name + "'");
    return nullptr;
}

std::unique_ptr<Expression> Setting::constantPropagate(const IRGenerator& irGenerator,
                                                       const DefinitionMap& definitions) {
    if (irGenerator.fContext.fConfig->fSettings.fReplaceSettings) {
        return GetValue(irGenerator.fContext, fOffset, this->name());
    }
    return nullptr;
}

}  // namespace SkSL
